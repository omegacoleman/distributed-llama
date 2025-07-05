#ifndef ROCKSDB_CACHE_HPP
#define ROCKSDB_CACHE_HPP

#include "nn/nn-cache.hpp"
#include <mutex>

namespace rocksdb {
    class DB;
}

class RocksDbCacheDatabaseStageEntry {
public:
    std::string metadata;
    std::map<NnUint, std::string> buffers;
};

class RocksDbCacheDatabase : public NnCacheDatabase {
public:
    ~RocksDbCacheDatabase();

    void open(const std::string& path);
    std::vector<NnCacheDatabaseMetadata> loadMetadata();
    void read(NnCacheId id, NnUint bufferIndex, NnByte* dst, NnSize* nBytes) override;
    NnByte *getWriteBuffer(NnCacheId id, NnUint bufferIndex, NnSize nBytes) override;
    bool dirty(NnCacheId id);
    void commit(NnCacheId id) override;
    void putMessageTokens(const char* chat, size_t len, NnUint* tokens, size_t nTokens);
    int tryGetMessageTokens(const char* chat, size_t len, NnUint* tokens, size_t maxTokens);

private:
    rocksdb::DB* db = nullptr;
    std::map<NnUint, RocksDbCacheDatabaseStageEntry> stage;
    std::mutex mut;
};

#endif
