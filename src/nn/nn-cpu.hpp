#ifndef NN_CPU_H
#define NN_CPU_H

#include <vector>
#include "nn-executor.hpp"
#include "nn-cpu-ops.hpp"

#define DEBUG_USE_MMAP_FOR_WEIGHTS false

class NnCpuDevice : public NnDevice {
public:
    NnByte ***slotBuffers;
private:
    NnNetConfig *netConfig;
    NnNodeConfig *nodeConfig;
    NnNetExecution *netExecution;
    NnUint nBuffers;
    NnUint nSlots;
    NnByte *bufferFlags;
public:
    NnCpuDevice(NnNetConfig *netConfig, NnNodeConfig *nodeConfig, NnNetExecution *netExecution);
    ~NnCpuDevice() override;
    NnUint maxNThreads() override;
    NnDeviceSegment *createSegment(NnUint segmentIndex) override;
    void resolvePointer(NnByte **pntr, NnSize2D *pntrSize, NnPointerConfig *pointerConfig, NnUint slot);
};

class NnCpuDeviceCacheSyncOp {
public:
    NnByte** slotTargets;
    NnSize2D size;
    NnCacheSyncType type;
    NnUint bufferIndex;
    NnCacheId* cacheId;

    // only available in KV-related cache saving
    float* kvPos;
};

class NnCpuDeviceSegment : public NnDeviceSegment {
public:
    NnUint nOps;
    NnCpuOpForward *opForward;
    NnCpuOpContext *opContexts;

    NnUint* slot;
    NnUint nCacheSyncOps;
    NnCpuDeviceCacheSyncOp* cacheSyncOps;

    NnCpuDeviceSegment(NnCpuOpForward *opForward, NnCpuOpContext *opContexts, NnUint nOps, NnCpuDeviceCacheSyncOp* cacheSyncOps, NnUint nCacheSyncOps, NnUint* slot)
        : opForward(opForward), opContexts(opContexts), nOps(nOps), cacheSyncOps(cacheSyncOps), nCacheSyncOps(nCacheSyncOps), slot(slot) {}
    ~NnCpuDeviceSegment() override;
    void loadWeight(NnUint opIndex, NnSize nBytes, NnByte *weight) override;
    void forward(NnUint opIndex, NnUint nThreads, NnUint threadIndex, NnUint batchSize) override;
    void syncCache(NnCacheDatabase* db, NnUint nThreads, NnUint threadIndex, NnCacheSyncType type) override;
};

#endif
