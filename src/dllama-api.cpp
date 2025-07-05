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

typedef unsigned int pos_t;

using json = nlohmann::json;

enum class HttpMethod {
    METHOD_GET = 0,
    METHOD_POST = 1,
    METHOD_PUT = 2,
    METHOD_DELETE = 3,
    METHOD_OPTIONS = 4,
    METHOD_UNKNOWN = 5
};

class HttpRequest {
public:
    static HttpRequest read(int serverSocket) {
        HttpRequest req(serverSocket);

        std::vector<char> httpRequest = req.readHttpRequest();
        // Parse the HTTP request
        std::string data = std::string(httpRequest.begin(), httpRequest.end());

        // Split request into lines
        std::istringstream iss(data);
        std::string line;
        std::getline(iss, line);

        // Parse request line
        std::istringstream lineStream(line);
        std::string methodStr, path;
        lineStream >> methodStr >> path;
        req.method = parseMethod(methodStr);
        req.path = path;

        // Parse headers
        while (std::getline(iss, line) && line != "\r") {
            size_t pos = line.find(':');
            if (pos != std::string::npos) {
                std::string key = line.substr(0, pos);
                std::string value = line.substr(pos + 2); // Skip ': ' after key
                // Trim whitespace and non-printable characters from header value
                value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) {
                    return std::isspace(c) || !std::isprint(c);
                }), value.end());
                req.headers[key] = value;
            }
        }

        // Parse body
        std::getline(iss, req.body, '\0');

        if (req.body.size() > 0) {
            // printf("body: %s\n", req.body.c_str());
            req.parsedJson = json::parse(req.body);
        }
        return req;
    }

    static HttpMethod parseMethod(const std::string& method) {
        if (method == "GET") return HttpMethod::METHOD_GET;
        if (method == "POST") return HttpMethod::METHOD_POST;
        if (method == "PUT") return HttpMethod::METHOD_PUT;
        if (method == "DELETE") return HttpMethod::METHOD_DELETE;
        if (method == "OPTIONS") return HttpMethod::METHOD_OPTIONS;
        return HttpMethod::METHOD_UNKNOWN;
    }

private:
    int serverSocket;
public:
    std::string path;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    json parsedJson;
    HttpMethod method;

    HttpRequest(int serverSocket) {
        this->serverSocket = serverSocket;
    }

    std::vector<char> readHttpRequest() {
        std::string httpRequest;
        char buffer[1024 * 64];
        ssize_t bytesRead;

        // First, read all headers
        std::string headerData;
        size_t headerEnd;
        bool headerDone = false;
        std::string extraReadPastHeader;
        while (!headerDone) {
            bytesRead = recv(serverSocket, buffer, sizeof(buffer) - 1, 0);
            if (bytesRead <= 0) {
                throw std::runtime_error("Error while reading headers from socket");
            }
            buffer[bytesRead] = '\0';
            headerData.append(buffer);

            // Check for end of headers (http header says "\r\n\r\n")
            headerEnd = headerData.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                headerDone = true;
                if (headerEnd < headerData.size()-4) {
                    // We read something past the header
                    extraReadPastHeader = headerData.substr(headerEnd+4);
                }
            }
        }

        httpRequest.append(headerData);

        // Next, find Content-Length header for body length
        std::istringstream headerStream(headerData);
        std::string line;
        ssize_t contentLength = 0;
        while (std::getline(headerStream, line) && line != "\r") {
            size_t pos = line.find(':');
            if (pos != std::string::npos) {
                std::string key = line.substr(0, pos);
                std::string value = line.substr(pos + 2); // Skip ': ' after key
                if (key == "Content-Length") {
                    try {
                      contentLength = std::stoi(value);  // stoi ignores any whitespace
                    } catch (const std::invalid_argument& e) {
                      throw std::runtime_error("Bad Content-Length header - not a number");
                    }
                    break;
                }
            }
        }

        // Now read the full content body
        if (contentLength > 0) {
            // If we read any extra past the header before, read that much less now
            // But first, sanity check to make sure Content-Length isn't lying and there is actually more
            if (extraReadPastHeader.size() > static_cast<size_t>(contentLength)) {
                throw std::runtime_error("Received more body data than Content-Length header said");
            }
            contentLength -= extraReadPastHeader.size();

            std::vector<char> body(contentLength);
            ssize_t totalRead = 0;
            while (totalRead < contentLength) {
                bytesRead = recv(serverSocket, body.data() + totalRead, contentLength - totalRead, 0);
                if (bytesRead <= 0) {
                    throw std::runtime_error("Error while reading body from socket");
                }
                totalRead += bytesRead;
            }
            if (body.size() > 0) {
              httpRequest.append(body.data(), contentLength);
            }
        }

        return std::vector<char>(httpRequest.begin(), httpRequest.end());
    }

    std::string getMethod() {
        if (method == HttpMethod::METHOD_GET) return "GET";
        if (method == HttpMethod::METHOD_POST) return "POST";
        if (method == HttpMethod::METHOD_PUT) return "PUT";
        if (method == HttpMethod::METHOD_DELETE) return "DELETE";
        if (method == HttpMethod::METHOD_OPTIONS) return "OPTIONS";
        return "UNKNOWN";
    }
 
    void writeCors() {
        std::ostringstream buffer;
        buffer << "HTTP/1.1 204 No Content\r\n"
            << "Access-Control-Allow-Origin: *\r\n"
            << "Access-Control-Allow-Methods: GET, POST, PUT, DELETE\r\n"
            << "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
            << "Connection: close\r\n"
            << "\r\n";
        std::string data = buffer.str();
        writeSocket(serverSocket, data.c_str(), data.size());
    }

    void writeTooLarge() {
        std::ostringstream buffer;
        buffer << "HTTP/1.1 413 Content Too Large\r\n"
            << "Connection: close\r\n"
            << "Content-Length: 17\r\n"
            << "\r\n"
            << "Content Too Large";
        std::string data = buffer.str();
        writeSocket(serverSocket, data.c_str(), data.size());
    }

    void writeNotFound() {
        std::ostringstream buffer;
        buffer << "HTTP/1.1 404 Not Found\r\n"
            << "Connection: close\r\n"
            << "Content-Length: 9\r\n"
            << "\r\n"
            << "Not Found";
        std::string data = buffer.str();
        writeSocket(serverSocket, data.c_str(), data.size());
    }

    void writeJson(std::string json) {
        std::ostringstream buffer;
        buffer << "HTTP/1.1 200 OK\r\n"
            << "Access-Control-Allow-Origin: *\r\n"
            << "Content-Type: application/json; charset=utf-8\r\n"
            << "Connection: close\r\n"
            << "Content-Length: " << json.length() << "\r\n\r\n" << json;
        std::string data = buffer.str();
        writeSocket(serverSocket, data.c_str(), data.size());
    }

    void writeStreamStartChunk() {
        std::ostringstream buffer;
        buffer << "HTTP/1.1 200 OK\r\n"
            << "Access-Control-Allow-Origin: *\r\n"
            << "Content-Type: text/event-stream; charset=utf-8\r\n"
            << "Connection: close\r\n"
            << "Transfer-Encoding: chunked\r\n\r\n";
        std::string data = buffer.str();
        writeSocket(serverSocket, data.c_str(), data.size());
    }

    void writeStreamChunk(const std::string data) {
        std::ostringstream buffer;
        buffer << std::hex << data.size() << "\r\n" << data << "\r\n";
        std::string d = buffer.str();
        writeSocket(serverSocket, d.c_str(), d.size());
    }

    void writeStreamEndChunk() {
        const char *endChunk = "0000\r\n\r\n";
        writeSocket(serverSocket, endChunk, strlen(endChunk));
    }
};

struct Route {
    std::string path;
    HttpMethod method;
    std::function<void(HttpRequest&)> handler;
};

class Router {
public:
    static void resolve(HttpRequest& request, std::vector<Route>& routes) {
        if (request.method == HttpMethod::METHOD_OPTIONS) {
            request.writeCors();
            return;
        }
        for (const auto& route : routes) {
            if (request.method == route.method && request.path == route.path) {
                route.handler(request);
                return;
            }
        }
        request.writeNotFound();
    }
};

void writeChatCompletionChunk(HttpRequest &request, const std::string &delta, const bool stop){
    ChunkChoice choice;
    if (stop) {
        choice.finish_reason = "stop";
    } else {
        choice.delta = ChatMessageDelta("assistant", delta);
    }
    ChatCompletionChunk chunk = ChatCompletionChunk(choice);

    std::ostringstream buffer;
    buffer << "data: " << ((json)chunk).dump() << "\r\n\r\n";
    request.writeStreamChunk(buffer.str());

    if (stop) {
        request.writeStreamChunk("data: [DONE]");
        request.writeStreamEndChunk();
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

    void complete(HttpRequest& request) {
        InferenceParams params = parseRequest(request);

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
            request.writeTooLarge();
            return;
        }

        if (true) {
            for (size_t i = 0; i < nPromptTokens; i++) {
                printf("%s", tokenizer->decode(promptTokens[i], false));
            }
            printf("🔶\n");
        }

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

        if (params.stream)
            request.writeStreamStartChunk();
        if (templateGenerator->getPublicPrompt().size()) {
            if (params.stream)
                writeChatCompletionChunk(request, templateGenerator->getPublicPrompt(), false);
            oss << templateGenerator->getPublicPrompt();
        }

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

        int token = promptTokens[pos];
        for (; pos < maxPredPos;) {
            inference->setPosition(pos);
            inference->setToken(0, token);
            inference->setCacheId(CACHE_SKIP, pos == inferenceStartPos ? loadFrom : CACHE_SKIP);
            inference->forward();

            pos++;
            token = sampler->sample(inference->logitsPipe);
            promptTokens[pos] = token;

            char *piece = tokenizer->decode(token);
            EosDetectorType eosType = eosDetector->append(token, piece);

            if (piece != nullptr) {
                printf("%s", piece);
                fflush(stdout);
            }

            if (eosType == NOT_EOS || eosType == EOS) {
                char *delta = eosDetector->getDelta();
                if (delta != nullptr) {
                    std::string deltaStr(delta);
                    if (params.stream)
                        writeChatCompletionChunk(request, deltaStr, false);
                    oss << deltaStr;
                }
                eosDetector->reset();
            }
            if (eosType == EOS)
              break;
        }
        inference->setPosition(pos);
        inference->setToken(0, token);
        inference->setCacheId(saveTo, CACHE_SKIP);
        inference->forward();

        ChatMessage chatMessage("assistant", oss.str());

        if (params.stream) {
            writeChatCompletionChunk(request, "", true);
        } else {
            int nCompletionTokens = pos - promptEndPos;
            ChatUsage usage(nPromptTokens, nCompletionTokens, nPromptTokens + nCompletionTokens);
            Choice choice(chatMessage);
            ChatCompletion completion(choice, usage);
            std::string chatJson = ((json)completion).dump();
            request.writeJson(chatJson);
        }
        printf("🔶\n");
        fflush(stdout);

        cacheManager->updateDeviceSlot(slot, (unsigned*)promptTokens, pos, saveTo);
        if (db && saveTo != CACHE_SKIP) {
            cacheManager->updateNnCacheDatabaseMetadata(saveTo, (unsigned*)promptTokens, pos);
            db->commit(saveTo);
        }
    }

private:
    InferenceParams parseRequest(HttpRequest& request) {
        InferenceParams params;
        params.temperature = args->temperature;
        params.top_p = args->topp;
        params.seed = args->seed;
        params.stream = false;
        params.messages = parseChatMessages(request.parsedJson["messages"]);
        params.max_tokens = -1;

        if (request.parsedJson.contains("stream")) {
            params.stream = request.parsedJson["stream"].get<bool>();
        }
        if (request.parsedJson.contains("temperature")) {
            params.temperature = request.parsedJson["temperature"].template get<float>();
        }
        if (request.parsedJson.contains("seed")) {
            params.seed = request.parsedJson["seed"].template get<unsigned long long>();
            sampler->setSeed(params.seed);
        }
        if (request.parsedJson.contains("max_tokens")) {
            params.max_tokens = request.parsedJson["max_tokens"].template get<int>();
        }
        if (request.parsedJson.contains("stop")) {
            params.stop = request.parsedJson["stop"].template get<std::vector<std::string>>();
        } else {
            const std::string defaultStop = "<|eot_id|>";
            params.stop = std::vector<std::string>{defaultStop};
        }
        return params;
    }
};

void handleCompletionsRequest(HttpRequest& request, ApiServer *api) {
    api->complete(request);
}

void handleModelsRequest(HttpRequest& request, const char* modelPath) {
    std::string path(modelPath);
    size_t pos = path.find_last_of("/\\");
    std::string modelName = (pos == std::string::npos) ? path : path.substr(pos + 1);

    Model model(modelName);
    ModelList list(model);
    std::string response = ((json)list).dump();
    request.writeJson(response);
}

static void server(AppInferenceContext *context) {
    int serverSocket = createServerSocket(context->args->port);

    TokenizerChatStops stops(context->tokenizer);
    ChatTemplateGenerator templateGenerator(context->args->chatTemplateType, context->tokenizer->chatTemplate, stops.stops[0]);
    EosDetector eosDetector(stops.nStops, context->tokenizer->eosTokenIds.data(), stops.stops, stops.maxStopLength, stops.maxStopLength);
    ApiServer api(context->inference, context->tokenizer, context->sampler, context->args, context->header, &eosDetector, &templateGenerator, context->cacheDb);

    printf("Server URL: http://127.0.0.1:%d/v1/\n", context->args->port);

    std::vector<Route> routes = {
        {
            "/v1/chat/completions",
            HttpMethod::METHOD_POST,
            std::bind(&handleCompletionsRequest, std::placeholders::_1, &api)
        },
        {
            "/v1/models",
            HttpMethod::METHOD_GET,
            std::bind(&handleModelsRequest, std::placeholders::_1, context->args->modelPath)
        }
    };

    while (true) {
        try {
            int clientSocket = acceptSocket(serverSocket);
            HttpRequest request = HttpRequest::read(clientSocket);
            printf("🔷 %s %s\n", request.getMethod().c_str(), request.path.c_str());
            Router::resolve(request, routes);
            close(clientSocket);
        } catch (NnReadNetworkException& ex) {
            printf("Read socket error: %d %s\n", ex.code, ex.message);
        } catch (NnWriteNetworkException& ex) {
            printf("Write socket error: %d %s\n", ex.code, ex.message);
        }
    }

    closeServerSocket(serverSocket);
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
