#include "rocksdb-cache.hpp"
#include <cassert>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <iomanip>

#include "rocksdb/db.h"
#include "rocksdb/slice.h"
#include "rocksdb/write_batch.h"

static void HandleStatus(const rocksdb::Status& status) {
    if (! status.ok()) {
        fprintf(stderr, "RocksDB Error : %s\n", status.ToString().c_str());
    }
}

static std::string buildKey(NnCacheId id, NnUint bufferIndex) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    oss << std::setw(sizeof(NnUint) * 2) << bufferIndex << "_";
    oss << std::setw(sizeof(NnCacheId) * 2) << id;
    return oss.str();
}

static void parseKey(const std::string& key, NnCacheId &id, NnUint &bufferIndex) {
    assert(key.size() == (sizeof(NnUint) * 2 + 1 + sizeof(NnCacheId) * 2));
    assert(key[sizeof(NnUint) * 2] == '_');
    std::string indexHex = std::string(key.data(), sizeof(NnUint) * 2);
    std::string idHex = std::string(key.data() + sizeof(NnUint) * 2 + 1, sizeof(NnCacheId) * 2);
    id = std::stoul(idHex, 0, 16);
    bufferIndex = std::stoul(indexHex, 0, 16);
}

void RocksDbCacheDatabase::open(const std::string& path) {
    std::unique_lock lg{mut};
    rocksdb::Options op;
    op.create_if_missing = true;
    op.enable_blob_files = true;
    auto status = rocksdb::DB::Open(op, path, &db);
    HandleStatus(status);
}

RocksDbCacheDatabase::~RocksDbCacheDatabase() {
    delete db;
}

void RocksDbCacheDatabase::read(NnCacheId id, NnUint bufferIndex, NnByte* dst, NnSize* nBytes) {
    std::unique_lock lg{mut};
    std::string key = buildKey(id, bufferIndex);
    rocksdb::PinnableSlice slice;
    auto status = db->Get(rocksdb::ReadOptions(), db->DefaultColumnFamily(), key, &slice);
    if (status.IsNotFound()) {
        *nBytes = 0;
        return;
    }
    HandleStatus(status);
    if (*nBytes > slice.size()) {
        *nBytes = slice.size();
    }
    std::memcpy(dst, slice.data(), *nBytes);
}

NnByte* RocksDbCacheDatabase::getWriteBuffer(NnCacheId id, NnUint bufferIndex, NnSize nBytes) {
    std::unique_lock lg{mut};
    std::string key = buildKey(id, bufferIndex);
    stage[id].buffers[bufferIndex].resize(nBytes);
    return (NnByte*) stage[id].buffers[bufferIndex].data();
}

void RocksDbCacheDatabase::commit(NnCacheId id) {
    std::unique_lock lg{mut};
    rocksdb::WriteBatch batch;
    if (stage.count(id) == 0)
        return;
    for (const auto& it : stage[id].buffers) {
        std::string key = buildKey(id, it.first);
        auto status = batch.Put(key, it.second);
        HandleStatus(status);
        printf("RocksDB: Writing %s\n", key.c_str());
    }
    auto status = db->Write(rocksdb::WriteOptions(), &batch);
    HandleStatus(status);
    stage.erase(id);
}

std::vector<NnCacheDatabaseMetadata> RocksDbCacheDatabase::loadMetadata() {
    rocksdb::Iterator* it = db->NewIterator(rocksdb::ReadOptions());
    std::string beginKey = buildKey(0, NnLruScoreIndex);
    std::string endKey = buildKey(~0, NnLruScoreIndex);
    std::vector<NnCacheDatabaseMetadata> metas;
    for (it->SeekForPrev(endKey); it->Valid() && it->key().compare(beginKey) >= 0; it->Prev()) {

        NnCacheDatabaseMetadata meta;
        assert(it->value().size() == sizeof(meta.lruScore));
        std::memcpy(&meta.lruScore, it->value().data(), sizeof(meta.lruScore));

        NnCacheId id;
        NnUint bufferIndex;
        std::string prefixBytes;
        parseKey(it->key().ToString(), id, bufferIndex);
        auto status = db->Get(rocksdb::ReadOptions(), buildKey(id, NnPrefixIndex), &prefixBytes);
        HandleStatus(status);

        meta.id = id;
        meta.prefixLen = prefixBytes.size() / sizeof(NnUint);
        assert(meta.prefixLen < NnPrefixCacheMaxLen);
        std::memcpy(meta.prefix, prefixBytes.data(), prefixBytes.size());

        metas.push_back(meta);
    }
    HandleStatus(it->status());
    return metas;
}

