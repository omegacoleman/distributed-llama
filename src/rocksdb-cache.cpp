#include "rocksdb-cache.hpp"
#include <cassert>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <iomanip>

#include "rocksdb/db.h"
#include "rocksdb/slice.h"
#include "rocksdb/write_batch.h"

#include "city.h"

static void HandleStatus(const rocksdb::Status& status) {
    if (! status.ok()) {
        fprintf(stderr, "RocksDB Error : %s\n", status.ToString().c_str());
        std::_Exit(1);
    }
}

static std::string buildKey(NnCacheId id, NnUint bufferIndex) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    oss << std::setw(sizeof(NnUint) * 2) << bufferIndex << "_";
    oss << std::setw(sizeof(NnCacheId) * 2) << id;
    return oss.str();
}

#define FIELD_CHAT                3
#define FIELD_TOKENS              4
#define FIELD_LRU_TIMESTAMP       5

static std::string buildMessageItemKey(uint128 hash, NnUint fieldIndex) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    oss << "__msg_";
    oss << std::setw(sizeof(uint64) * 2) << hash.first << hash.second << "_";
    oss << std::setw(sizeof(NnUint) * 2) << fieldIndex;
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

bool RocksDbCacheDatabase::dirty(NnCacheId id) {
    return this->stage.count(id);
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

void RocksDbCacheDatabase::putMessageTokens(const char* chat, size_t len, NnUint* tokens, size_t nTokens) {
    auto hash = CityHash128(chat, len);
    std::string keyChat = buildMessageItemKey(hash, FIELD_CHAT);
    std::string keyTokens = buildMessageItemKey(hash, FIELD_TOKENS);

    rocksdb::WriteBatch batch;
    batch.Put(keyChat, rocksdb::Slice(chat, len));
    batch.Put(keyTokens, rocksdb::Slice((const char *) tokens, nTokens * sizeof(NnUint)));
    printf("RocksDB: Writing %s\n", keyChat.c_str());
    printf("RocksDB: Writing %s, len = %lu\n", keyTokens.c_str(), nTokens);

    auto status = db->Write(rocksdb::WriteOptions(), &batch);
    HandleStatus(status);
}

int RocksDbCacheDatabase::tryGetMessageTokens(const char* chat, size_t len, NnUint* tokens, size_t maxTokens) {
    auto hash = CityHash128(chat, len);
    std::string keyChat = buildMessageItemKey(hash, FIELD_CHAT);
    std::string keyTokens = buildMessageItemKey(hash, FIELD_TOKENS);

    // verify the chat, handle colllisions
    std::string chatBuf;
    auto status = db->Get(rocksdb::ReadOptions(), keyChat, &chatBuf);
    if (status.IsNotFound()) return 0;
    HandleStatus(status);
    if (chatBuf.compare(0, std::string::npos, chat, len) != 0) {
        return 0;
    }

    std::string tokenBuf;
    status = db->Get(rocksdb::ReadOptions(), keyTokens, &tokenBuf);
    HandleStatus(status);

    if (tokenBuf.size() / sizeof(NnUint) > maxTokens)
        return -1;

    std::memcpy(tokens, tokenBuf.data(), tokenBuf.size());
    return tokenBuf.size() / sizeof(NnUint);
}

