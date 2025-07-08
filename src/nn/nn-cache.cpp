#include "nn-cache.hpp"
#include "nn-core.hpp"

#include <algorithm>

NnPrefixCacheManager::NnPrefixCacheManager(NnUint slots, NnCacheDatabase *db) {
    nSlots = slots;
    this->db = db;
    deviceSlots.resize(nSlots);
    nextLruScore = 1;
    nextCacheId = 1;

    if (db) {
        std::vector<NnCacheDatabaseMetadata> metas = db->loadMetadata();
        printf("Loading cache metadata from DB:\n");
        for (const NnCacheDatabaseMetadata& it : metas) {
            if (nextLruScore <= it.lruScore) {
                nextLruScore = it.lruScore + 1;
            }
            if (nextCacheId <= it.id) {
                nextCacheId = it.id + 1;
            }
            dbMetas[it.id] = it;
            NnCacheDst dst;
            dst.type = CACHE_DB;
            dst.id = it.id;
            dst.prefixLen = it.prefixLen;
            dst.matchLen = 0;
            trie.insert_ks((const char *)it.prefix, it.prefixLen * sizeof(NnUint), dst);

            printf("ID=%lu, LruScore=%u, Prefix=", it.id, it.lruScore);
            dumpTokens(it.prefix, it.prefixLen);
        }
        printf("-- Finished loading cache metadata --\n");
    }
    for (NnUint slotIndex = 0; slotIndex < nSlots; slotIndex++) {
        deviceSlots[slotIndex].slot = slotIndex;
        deviceSlots[slotIndex].prefixLen = 0;
        deviceSlots[slotIndex].lruScore = 0;
    }
}

NnCacheDst NnPrefixCacheManager::lookup(NnUint* token, NnUint tokenLen) {
    printf("Lookup: ");
    dumpTokens(token, tokenLen);

    size_t matchedPrefixLen = 0;
    auto rng = trie.equal_longest_prefix_range_ks((const char *)token, tokenLen * sizeof(NnUint), matchedPrefixLen);
    matchedPrefixLen /= sizeof(NnUint);
    if (rng.first == rng.second || matchedPrefixLen < NnPrefixCacheMinLen) {
        NnCacheDst miss;
        miss.type = CACHE_MISS;
        miss.prefixLen = 0;
        return miss;
    }
    NnCacheDst dst = rng.first.value(); // TODO will it be better if we select the shortest cache here?
    dst.matchLen = matchedPrefixLen;
    return dst;
}

static bool preferPickThan(const NnDeviceSlotStatus& lhs, const NnDeviceSlotStatus& rhs) {
    if (lhs.lruScore < rhs.lruScore) return true;
    if (lhs.lruScore > rhs.lruScore) return false;
    if (lhs.prefixLen < rhs.prefixLen) return true;
    if (lhs.prefixLen > rhs.prefixLen) return false;

    return lhs.slot < rhs.slot;
}

NnUint NnPrefixCacheManager::pickSlot() {
    std::vector<NnDeviceSlotStatus> sortSlots = deviceSlots;
    std::sort(sortSlots.begin(), sortSlots.end(), preferPickThan);
    return sortSlots[0].slot;
}

NnCacheId NnPrefixCacheManager::getCacheId() {
    if (!this->db)
        return CACHE_SKIP;
    return nextCacheId++;
}

void NnPrefixCacheManager::updateDeviceSlot(NnUint slot, NnUint* token, NnUint tokenLen, NnCacheId cacheId) {
    if (tokenLen >= NnPrefixCacheMaxLen)
        tokenLen = NnPrefixCacheMaxLen;

    NnSize tokenBytes = tokenLen * sizeof(NnUint);

    printf("Slot %u upd: ", slot);
    dumpTokens(token, tokenLen);

    if (deviceSlots[slot].prefixLen > 0) {
        TrieImpl::iterator it = trie.find_ks((const char *)deviceSlots[slot].prefix, deviceSlots[slot].prefixLen * sizeof(NnUint));
        if (it != trie.end() && it.value().type == CACHE_DEVICE_SLOT && it.value().slot == slot) {
            trie.erase(it);
        }
    }
    std::memcpy(deviceSlots[slot].prefix, token, tokenBytes);
    deviceSlots[slot].prefixLen = tokenLen;
    deviceSlots[slot].lruScore = nextLruScore++;

    if (trie.find_ks((const char *)token, tokenBytes) != trie.end()) {
        // unlikely to happend, free up resource of dup slots
        deviceSlots[slot].prefixLen = 0;
        deviceSlots[slot].lruScore = 0;
        return;
    }

    NnCacheDst dst;
    dst.type = CACHE_DEVICE_SLOT;
    dst.slot = slot;
    dst.prefixLen = tokenLen;
    dst.matchLen = 0;
    dst.id = cacheId;
    trie.insert_ks((const char *)token, tokenBytes, dst);
}

void NnPrefixCacheManager::updateNnCacheDatabaseMetadata(NnCacheId id, NnUint* token, NnUint tokenLen) {
    if (!this->db)
        return;

    if (tokenLen >= NnPrefixCacheMaxLen)
        tokenLen = NnPrefixCacheMaxLen;

    NnSize tokenBytes = tokenLen * sizeof(NnUint);

    printf("DB cache %lu upd: ", id);
    dumpTokens(token, tokenLen);

    if (dbMetas.count(id)) {
        TrieImpl::iterator it = trie.find_ks((const char *)dbMetas[id].prefix, dbMetas[id].prefixLen * sizeof(NnUint));
        if (it != trie.end() && it.value().type == CACHE_DB && it.value().id == id) {
            trie.erase(it);
        }
    }
    std::memcpy(dbMetas[id].prefix, token, tokenBytes);
    dbMetas[id].prefixLen = tokenLen;
    dbMetas[id].lruScore = nextLruScore++;

    if (trie.find_ks((const char *)token, tokenBytes) == trie.end()) {
        NnCacheDst dst;
        dst.type = CACHE_DB;
        dst.id = id;
        dst.prefixLen = tokenLen;
        dst.matchLen = 0;
        trie.insert_ks((const char *)token, tokenLen * sizeof(NnUint), dst);
    }

    NnByte* dbPrefixBuffer = db->getWriteBuffer(id, NnPrefixIndex, tokenBytes);
    std::memcpy(dbPrefixBuffer, token, tokenBytes);

    NnByte* dbLruBuffer = db->getWriteBuffer(id, NnLruScoreIndex, sizeof(dbMetas[id].lruScore));
    std::memcpy(dbLruBuffer, &dbMetas[id].lruScore, sizeof(dbMetas[id].lruScore));
}
