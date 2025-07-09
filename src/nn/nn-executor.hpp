#ifndef NN_EXECUTOR_H
#define NN_EXECUTOR_H

#include "nn-core.hpp"
#include <atomic>
#include <vector>

#include "oneapi/tbb/task_group.h"
#include "oneapi/tbb/task_arena.h"

class NnDeviceSegment {
public:
    virtual ~NnDeviceSegment() {};
    virtual void loadWeight(NnUint opIndex, NnSize nBytes, NnByte *weight) = 0;
    virtual void forward(NnUint opIndex, NnUint nThreads, NnUint threadIndex, NnUint batchSize) = 0;
    virtual void syncCache(NnCacheDatabase* db, NnUint nThreads, NnUint threadIndex, NnCacheSyncType type) = 0;
};

class NnDevice {
public:
    virtual NnUint maxNThreads() = 0;
    virtual ~NnDevice() {}
    virtual NnDeviceSegment *createSegment(NnUint segmentIndex) = 0;
};

class NnNodeSynchronizer {
public:
    virtual ~NnNodeSynchronizer() {};
    virtual void sync(NnUint segmentIndex, NnUint nThreads, NnUint threadIndex) = 0;
};

class NnFakeNodeSynchronizer : public NnNodeSynchronizer {
public:
    ~NnFakeNodeSynchronizer() override {};
    void sync(NnUint segmentIndex, NnUint nThreads, NnUint threadIndex) override;
};

class NnNetExecution {
public:
    NnUint nThreads;
    NnUint nPipes;
    NnByte **pipes;
    NnUint batchSize;
    NnUint nBatches;
    NnUint nSlots;
    NnUint slot;
    NnCacheId cacheSaveId;
    NnCacheId cacheLoadId;
    NnNetExecution(NnUint nThreads, NnNetConfig *netConfig);
    ~NnNetExecution();
    void setBatchSize(NnUint batchSize);
    void setSlot(NnUint slot);
    void setCacheId(NnCacheId cacheSaveId, NnCacheId cacheLoadId);
};

enum NnExecutorStepType {
    STEP_EXECUTE_OP,
    STEP_SYNC_NODES,
    STEP_SYNC_CACHE_LOAD,
    STEP_SYNC_CACHE_SAVE,
    N_STEP_TYPES,
};

class NnExecutorDevice {
public:
    std::unique_ptr<NnDevice> device;
    int segmentFrom;
    int segmentTo;
    NnExecutorDevice(NnDevice *device, int segmentFrom, int segmentTo);
};

typedef struct {
    NnExecutorStepType type;
    NnDeviceSegment *segment;
    NnUint arg0;
    NnOpConfig *opConfig;
} NnExecutorStep;

typedef struct {
    NnUint nThreads;
    NnUint nSteps;
    NnExecutorStep *steps;
    NnNodeSynchronizer *synchronizer;
    NnUint batchSize;
    Timer *timer;
    NnUint totalTime[N_STEP_TYPES];
    NnCacheDatabase *cacheDb;
} NnExecutorContext;

typedef struct {
    NnUint threadIndex;
    NnExecutorContext *context;
} NnExecutorThread;

class NnExecutor {
private:
    NnNetExecution *netExecution;
    NnNodeConfig *nodeConfig;
    std::vector<std::unique_ptr<NnDeviceSegment>> segments;
    std::vector<NnExecutorStep> steps;
    NnExecutorThread *threads;
    NnExecutorContext context;
    tbb::task_arena arena;
    tbb::task_group group;
public:
    NnExecutor(NnNetConfig *netConfig, NnNodeConfig *nodeConfig, std::vector<NnExecutorDevice> *device, NnNetExecution *netExecution, NnNodeSynchronizer *synchronizer, NnCacheDatabase *cacheDb, bool benchmark);
    ~NnExecutor();
    void loadWeight(const char *name, NnUint index, NnSize nBytes, NnByte *weight);
    void forward();
    NnUint getTotalTime(NnExecutorStepType type);
};

#endif
