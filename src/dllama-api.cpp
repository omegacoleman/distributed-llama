#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cassert>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <vector>
#include <string>
#include <csignal>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

#include "tokenizer.hpp"
#include "app.hpp"
#include "json.hpp"
#include "api-types.hpp"
#include "nn/nn-network.hpp"
#include "nn/nn-cache.hpp"

#include "httplib.h"

typedef unsigned int pos_t;

using json = nlohmann::json;

void writeChatCompletionChunk(httplib::DataSink &sink, const std::string &delta, const bool stop){
    ChunkChoice choice;
    if (stop) {
        choice.finish_reason = "stop";
    } else {
        choice.delta = ChatMessageDelta("assistant", delta);
    }
    ChatCompletionChunk chunk = ChatCompletionChunk(choice);

    std::ostringstream buffer;
    buffer << "data: " << ((json)chunk).dump() << "\r\n\r\n";
    sink.os << buffer.str();

    if (stop) {
        sink.os << "data: [DONE]";
    }
}

class ApiServer {
private:
    RootLlmInference *inference;
    Tokenizer *tokenizer;
    Sampler *sampler;
    AppCliArgs *args;
    LlmHeader *header;
    EosDetector *eosDetector;
    ChatTemplateGenerator *templateGenerator;
    NnPrefixCacheManager* cacheManager; // TODO ptr or obj ?
    NnCacheDatabase* db;
    NnCachedContextTokenizer *contextTokenizer;
    std::mutex mut;

public:
    ApiServer(RootLlmInference *inference, Tokenizer *tokenizer, Sampler *sampler, AppCliArgs *args, LlmHeader *header, EosDetector *eosDetector, ChatTemplateGenerator *templateGenerator, NnCacheDatabase *db) {
        this->inference = inference;
        this->tokenizer = tokenizer;
        this->sampler = sampler;
        this->args = args;
        this->header = header;
        this->eosDetector = eosDetector;
        this->templateGenerator = templateGenerator;
        this->cacheManager = new NnPrefixCacheManager(header->slots, db);
        this->db = db;
        this->contextTokenizer = new NnCachedContextTokenizer(templateGenerator, tokenizer, db);
    }

    bool provideContent(const InferenceParams &params, httplib::DataSink &sink) {
        std::unique_lock lg{mut}; // TODO multi-slot scheduling support should start w/ removal of this lock

        size_t nInputItems = params.messages.size();
        std::unique_ptr<ChatItem[]> inputItemsPtr(new ChatItem[nInputItems]);
        ChatItem *inputItems = inputItemsPtr.get();
        for (size_t i = 0; i < nInputItems; i++) {
            inputItems[i].role = params.messages[i].role;
            inputItems[i].message = params.messages[i].content;
        }

        int nPromptTokens;
        std::unique_ptr<int[]> promptTokensPtr(new int[header->seqLen + 2]);
        int *promptTokens = promptTokensPtr.get();
        if ((nPromptTokens = this->contextTokenizer->encodeContext(
              inputItems, nInputItems, promptTokens, header->seqLen)) < 0) {
            if (params.stream) {
                writeChatCompletionChunk(sink, "", true);
            } else {
                ChatUsage usage(header->seqLen, 0, header->seqLen);
                Choice choice("");
                ChatCompletion completion(choice, usage);
                std::string chatJson = ((json)completion).dump();
                sink.os << chatJson;
            }
            sink.done();
            return true;
        }

#ifndef NDEBUG
        printf("🔹");
        for (size_t i = 0; i < nPromptTokens; i++) {
            printf("%s", tokenizer->decode(promptTokens[i], false));
        }
        printf("🔸\n");
#endif

        pos_t promptEndPos = nPromptTokens - 1;
        if (promptEndPos > header->seqLen)
            promptEndPos = header->seqLen;

        pos_t maxPredPos = params.max_tokens > 0 ? (nPromptTokens + params.max_tokens) : header->seqLen;
        if (maxPredPos > header->seqLen)
            maxPredPos = header->seqLen;

        unsigned slot;
        pos_t inferenceStartPos = 0;
        NnCacheDst dst = cacheManager->lookup((unsigned*)promptTokens, promptEndPos);
        NnCacheId loadFrom = CACHE_SKIP;
        NnCacheId saveTo = CACHE_SKIP;
        if (dst.type == CACHE_DEVICE_SLOT) {
            slot = dst.slot;
            inferenceStartPos = dst.matchLen;
            if (dst.matchLen == dst.prefixLen)
                saveTo = dst.id;
            printf("🔄 Prefix cache hit device slot %u (len = %u, match = %u)\n", dst.slot, dst.prefixLen, dst.matchLen);
        } else if (dst.type == CACHE_DB) {
            slot = cacheManager->pickSlot();
            loadFrom = dst.id;
            inferenceStartPos = dst.matchLen;
            if (dst.matchLen == dst.prefixLen)
                saveTo = loadFrom;
            printf("🔄 Prefix cache hit db, loading from cache id %ld to slot %u (len = %u)\n", loadFrom, slot, dst.prefixLen);
            
        } else { // CACHE_MISS
            slot = cacheManager->pickSlot();
            printf("❌ Prefix cache missed, inference from scratch on slot %u\n", slot);
        }

        if (saveTo == CACHE_SKIP) {
            saveTo = cacheManager->getCacheId();
        }

        std::ostringstream oss;

        if (templateGenerator->getPublicPrompt().size()) {
            if (params.stream) {
              if (!sink.is_writable()) return false;
                writeChatCompletionChunk(sink, templateGenerator->getPublicPrompt(), false);
            }
            oss << templateGenerator->getPublicPrompt();
        }

        printf("🔶 Scanning from %u to %u ..\n", inferenceStartPos, promptEndPos);

        NnUint pos = inferenceStartPos;
        for (; ;) {
            long remainingTokens = promptEndPos - pos;
            if (remainingTokens <= 0)
                break;

            NnUint batchSize = remainingTokens < args->nBatches
                ? remainingTokens
                : args->nBatches;

            inference->setBatchSize(batchSize);
            inference->setPosition(pos);
            inference->setSlot(slot);
            inference->setCacheId(CACHE_SKIP, pos == inferenceStartPos ? loadFrom : CACHE_SKIP);
            for (NnUint j = 0; j < batchSize; j++)
                inference->setToken(j, promptTokens[pos + j]);

            inference->forward();

            pos += batchSize;
        }

        inference->setBatchSize(1);
        tokenizer->resetDecoder();
        eosDetector->reset();

        printf("🔶 Completion stated ..\n");

        int token = promptTokens[pos];
        for (; pos < maxPredPos;) {
            inference->setPosition(pos);
            inference->setToken(0, token);
            inference->setCacheId(pos == promptEndPos ? saveTo : CACHE_SKIP, pos == inferenceStartPos ? loadFrom : CACHE_SKIP);
            inference->forward();

            if (pos == promptEndPos) {
              if (db && saveTo != CACHE_SKIP) {
                cacheManager->updateNnCacheDatabaseMetadata(saveTo, (unsigned*)promptTokens, pos);
                db->commit(saveTo);
              }
            }

            pos++;
            token = sampler->sample(inference->logitsPipe);
            promptTokens[pos] = token;

            char *piece = tokenizer->decode(token);
            EosDetectorType eosType = eosDetector->append(token, piece);

#ifndef NDEBUG
            if (piece != nullptr) {
                printf("%s", piece);
                fflush(stdout);
            }
#endif

            if (eosType == NOT_EOS || eosType == EOS) {
                char *delta = eosDetector->getDelta();
                if (delta != nullptr) {
                    std::string deltaStr(delta);
                    if (params.stream) {
                        if (!sink.is_writable()) return false;
                        writeChatCompletionChunk(sink, deltaStr, false);
                    }
                    oss << deltaStr;
                }
                eosDetector->reset();
            }
            if (eosType == EOS)
              break;
        }
        if (pos < NnPrefixCacheMinLen) {
            saveTo = CACHE_SKIP;
        }
        if (db && saveTo != CACHE_SKIP) {
            // TODO here's some reduntant calculation
            inference->setPosition(pos);
            inference->setToken(0, token);
            inference->setCacheId(saveTo, CACHE_SKIP);
            inference->forward();
        }

        ChatMessage chatMessage("assistant", oss.str());

        if (!sink.is_writable()) return false;
        if (params.stream) {
            writeChatCompletionChunk(sink, "", true);
        } else {
            int nCompletionTokens = pos - promptEndPos;
            ChatUsage usage(nPromptTokens, nCompletionTokens, nPromptTokens + nCompletionTokens);
            Choice choice(chatMessage);
            ChatCompletion completion(choice, usage);
            std::string chatJson = ((json)completion).dump();
            sink.os << chatJson;
        }
#ifndef NDEBUG
        printf("🔶\n");
#endif
        printf("🔶 Completion finished with %u new tokens\n", pos - promptEndPos);
        fflush(stdout);

        sink.done();

        cacheManager->updateDeviceSlot(slot, (unsigned*)promptTokens, pos, saveTo);
        if (db && saveTo != CACHE_SKIP) {
            cacheManager->updateNnCacheDatabaseMetadata(saveTo, (unsigned*)promptTokens, pos);
            db->commit(saveTo);
        }

        return true;
    }

    void complete(const httplib::Request& req, httplib::Response &res) {
        std::unique_lock lg{mut};

        InferenceParams params = parseRequest(req);

        if (params.stream) {
            res.set_chunked_content_provider(
                "text/event-stream; charset=UTF-8",
                [this, params](size_t, httplib::DataSink &sink) {
                    return this->provideContent(params, sink);
                }
            );
        } else {
            res.set_content_provider(
                "application/json; charset=UTF-8",
                [this, params](size_t, httplib::DataSink &sink) {
                    return this->provideContent(params, sink);
                }
            );
        }
    }

private:
    InferenceParams parseRequest(const httplib::Request& req) {
        json parsedJson = json::parse(req.body);

        InferenceParams params;
        params.temperature = args->temperature;
        params.top_p = args->topp;
        params.seed = args->seed;
        params.stream = false;
        params.messages = parseChatMessages(parsedJson["messages"]);
        params.max_tokens = -1;

        if (parsedJson.contains("stream")) {
            params.stream = parsedJson["stream"].get<bool>();
        }
        if (parsedJson.contains("temperature")) {
            params.temperature = parsedJson["temperature"].template get<float>();
        }
        if (parsedJson.contains("seed")) {
            params.seed = parsedJson["seed"].template get<unsigned long long>();
            sampler->setSeed(params.seed);
        }
        if (parsedJson.contains("max_tokens")) {
            params.max_tokens = parsedJson["max_tokens"].template get<int>();
        }
        if (parsedJson.contains("stop")) {
            params.stop = parsedJson["stop"].template get<std::vector<std::string>>();
        } else {
            const std::string defaultStop = "<|eot_id|>";
            params.stop = std::vector<std::string>{defaultStop};
        }
        return params;
    }
};

void handleCompletionsRequest(const httplib::Request& req, httplib::Response &res, ApiServer *api) {
    try {
        api->complete(req, res);
    } catch (const std::exception& e) {
        printf("⚠️ Error processing completion: %s\n", e.what());
        res.status = httplib::StatusCode::InternalServerError_500;
    }
}

void handleModelsRequest(const httplib::Request &, httplib::Response &res, const char* modelPath) {
    std::string path(modelPath);
    size_t pos = path.find_last_of("/\\");
    std::string modelName = (pos == std::string::npos) ? path : path.substr(pos + 1);

    Model model(modelName);
    ModelList list(model);
    std::string response = ((json)list).dump();
    res.set_content(response, "application/json");
}

static void server(AppInferenceContext *context) {
    httplib::Server svr;

    TokenizerChatStops stops(context->tokenizer);
    ChatTemplateGenerator templateGenerator(context->args->chatTemplateType, context->tokenizer->chatTemplate, stops.stops[0]);
    EosDetector eosDetector(stops.nStops, context->tokenizer->eosTokenIds.data(), stops.stops, stops.maxStopLength, stops.maxStopLength);
    ApiServer api(context->inference, context->tokenizer, context->sampler, context->args, context->header, &eosDetector, &templateGenerator, context->cacheDb);

    svr.set_pre_request_handler([](const httplib::Request& req, httplib::Response& res) -> httplib::Server::HandlerResponse {
        printf("🔷 %s %s\n", req.method.c_str(), req.path.c_str());
        return httplib::Server::HandlerResponse::Unhandled;
    });

    svr.Post("/v1/chat/completions", [&api](const httplib::Request &req, httplib::Response &res) {
        handleCompletionsRequest(req, res, &api);
    });

    svr.Get("/v1/models", [context](const httplib::Request &req, httplib::Response &res) {
        handleModelsRequest(req, res, context->args->modelPath);
    });

    svr.new_task_queue = [context] { return new httplib::ThreadPool(context->args->nThreads); }; // TODO change to nIOThreads ..
    printf("Server URL: http://127.0.0.1:%d/v1/\n", context->args->port);
    svr.listen("0.0.0.0", context->args->port);
}

#ifdef _WIN32
    #define EXECUTABLE_NAME "dllama-api.exe"
#else
    #define EXECUTABLE_NAME "dllama-api"
#endif

void usage() {
    fprintf(stderr, "Usage: %s {--model <path>} {--tokenizer <path>} [--port <p>]\n", EXECUTABLE_NAME);
    fprintf(stderr, "        [--buffer-float-type {f32|f16|q40|q80}]\n");
    fprintf(stderr, "        [--weights-float-type {f32|f16|q40|q80}]\n");
    fprintf(stderr, "        [--max-seq-len <max>]\n");
    fprintf(stderr, "        [--nthreads <n>]\n");
    fprintf(stderr, "        [--workers <ip:port> ...]\n");
    fprintf(stderr, "        [--temperature <temp>]\n");
    fprintf(stderr, "        [--topp <t>]\n");
    fprintf(stderr, "        [--seed <s>]\n");
    fprintf(stderr, "        [--cache-db <path>]\n");
    fprintf(stderr, "Example:\n");
    fprintf(stderr, "  sudo nice -n -20 ./dllama-api --port 9990 --nthreads 4 \\\n");
    fprintf(stderr, "    --model dllama_model_llama3_2_3b_instruct_q40.m \\\n");
    fprintf(stderr, "    --tokenizer dllama_tokenizer_llama3_2_3b_instruct_q40.t \\\n");
    fprintf(stderr, "    --buffer-float-type q80 --max-seq-len 8192 \\\n");
    fprintf(stderr, "    --workers 10.0.0.2:9998 10.0.0.3:9998 10.0.0.4:9998 \\\n");
    fprintf(stderr, "    --cache-db ~/.cache/dllama/dllama_model_llama3_2_3b_instruct_q4\n");
    fflush(stderr);
}

int main(int argc, char *argv[]) {
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);
#endif

    initQuants();
    initSockets();

    int returnCode = EXIT_SUCCESS;
    try {
        AppCliArgs args = AppCliArgs::parse(argc, argv, false);
        if (args.help) {
            usage();
        } else {
            runInferenceApp(&args, server);
        }
    } catch (std::exception &e) {
        printf("🚨 Critical error: %s\n", e.what());
        returnCode = EXIT_FAILURE;
    }

    cleanupSockets();
    return returnCode;
}
