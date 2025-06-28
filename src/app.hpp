#ifndef APP_HPP
#define APP_HPP

#include <chrono>
#include "nn/nn-core.hpp"
#include "nn/nn-cpu.hpp"
#include "nn/nn-cache.hpp"
#include "tokenizer.hpp"
#include "llm.hpp"

class AppCliArgs {
public:
    char *mode;
    NnUint nThreads;
    NnUint nBatches;
    NnUint nSlots;
    bool help;

    // inference
    char *modelPath;
    char *tokenizerPath;
    char *cacheDbPath;
    char *prompt;
    NnFloatType syncType;
    NnUint nWorkers;
    char **workerHosts;
    NnUint *workerPorts;
    float temperature;
    float topp;
    NnUint steps;
    bool benchmark;
    unsigned long long seed;
    ChatTemplateType chatTemplateType;
    NnUint maxSeqLen;
    bool netTurbo;
    int gpuIndex;
    int gpuSegmentFrom;
    int gpuSegmentTo;

    // worker
    NnUint port;

    static AppCliArgs parse(int argc, char **argv, bool hasMode);
    ~AppCliArgs();
};

typedef struct {
    NnUint position;
    NnUint batchSize; // 0 = stop signal
    NnUint slot;
    NnCacheId cacheSaveId;
    NnCacheId cacheLoadId;
} LlmControlPacket;

class RootLlmInference {
public:
    float *logitsPipe;
private:
    float *tokenPipe;
    float *positionPipe;
    LlmHeader *header;
    NnNetExecution *execution;
    NnExecutor *executor;
    NnNetwork *network;
    LlmControlPacket controlPacket;
public:
    RootLlmInference(LlmNet *net, NnNetExecution *execution, NnExecutor *executor, NnNetwork *network);
    void setBatchSize(NnUint batchSize);
    void setPosition(NnUint position);
    void setSlot(NnUint slot);
    void setCacheId(NnCacheId cacheSaveId, NnCacheId cacheLoadId);
    void setToken(NnUint batchIndex, NnUint token);
    void forward();
    void finish();
};

class WorkerLlmInference {
public:
    bool isFinished;
private:
    float *positionPipe;
    NnNetExecution *execution;
    NnNetwork *network;
    LlmControlPacket controlPacket;
public:
    WorkerLlmInference(NnNetExecution *execution, NnNetwork *network);
    bool tryReadControlPacket();
};

typedef struct {
    AppCliArgs *args;
    LlmHeader *header;
    RootLlmInference *inference;
    Tokenizer *tokenizer;
    Sampler *sampler;
    NnNetwork *network;
    NnExecutor *executor;
    NnCacheDatabase *cacheDb;
} AppInferenceContext;

void runInferenceApp(AppCliArgs *args, void (*handler)(AppInferenceContext *context));
void runWorkerApp(AppCliArgs *args);

#endif
