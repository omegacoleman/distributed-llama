#ifndef NN_CACHE_H
#define NN_CACHE_H

#include "nn-core.hpp"

#include <vector>
#include <map>
#include <mutex>
#include "tsl/htrie_map.h"

enum NnCacheDstType {
    CACHE_MISS,
    CACHE_DB,
    CACHE_DEVICE_SLOT,
};

class NnCacheDst {
public:
    NnCacheDstType type;
    NnUint slot; // available with CACHE_DEVICE_SLOT
    NnCacheId id; // for CACHE_DEVICE_SLOT, this is the associated db cache id
    NnUint prefixLen;
    NnUint matchLen;
};

class NnDeviceSlotStatus {
public:
    NnUint slot;
    NnUint prefix[NnPrefixCacheMaxLen];
    NnUint prefixLen;
    NnUint lruScore;
};

class NnPrefixCacheManager {
private:
    NnUint nSlots;
    using TrieImpl = tsl::htrie_map<char, NnCacheDst>;
    TrieImpl trie;
    std::vector<NnDeviceSlotStatus> deviceSlots;
    std::map<NnCacheId, NnCacheDatabaseMetadata> dbMetas;
    NnUint nextLruScore;
    NnUint nextCacheId;
    NnCacheDatabase* db;
public:
    NnPrefixCacheManager(NnUint slots, NnCacheDatabase* db);
    NnCacheDst lookup(NnUint* token, NnUint tokenLen);
    NnUint pickSlot();
    NnCacheId getCacheId();
    void updateDeviceSlot(NnUint slot, NnUint* token, NnUint tokenLen, NnCacheId id);
    void updateNnCacheDatabaseMetadata(NnCacheId id, NnUint* token, NnUint tokenLen);
};

#endif
