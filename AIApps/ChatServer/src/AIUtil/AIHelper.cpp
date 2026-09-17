#include"../include/AIUtil/AIHelper.h"
#include"../include/AIUtil/MQManager.h"
#include <stdexcept>
#include<chrono>

// 构造函数
// 默认使用阿里云通义千问大模型（modelType="1"）
AIHelper::AIHelper() {
    //默认使用阿里云大模型
    strategy = StrategyFactory::instance().create("1");
}

void AIHelper::setStrategy(std::shared_ptr<AIStrategy> strat) {
    strategy = strat;
}


// 设置默认模型
//void AIHelper::setModel(const std::string& modelName) {
  //  model_ = modelName;
//}

// 添加消息到对话历史
// 两步操作：同步写入内存 messages 向量 + 异步推送到 RabbitMQ 等待落库
void AIHelper::addMessage(int userId,const std::string& userName, bool is_user,const std::string& userInput, std::string sessionId) {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    messages.push_back({ userInput,ms });
    //消息队列异步入库
    pushMessageToMysql(userId, userName, is_user, userInput, ms, sessionId);
}

void AIHelper::restoreMessage(const std::string& userInput,long long ms) {
    messages.push_back({ userInput,ms });
}


// 核心聊天方法
// 按 modelType 动态切换策略，非 MCP 模式走单次调用，MCP 模式走两段式推理
// stream=true 时：模型支持 SSE 且已设置回调 → 边收边推增量（打字机效果）
std::string AIHelper::chat(int userId,std::string userName, std::string sessionId, std::string userQuestion, std::string modelType, bool stream) {

    // 记录本轮模型标识，清零用量统计（MCP 两段式会在 executeCurl 里累加）
    m_curModel = modelType;
    m_lastPromptTokens = 0;
    m_lastCompletionTokens = 0;
    m_abortRequested.store(false);                     // 清除上一轮的取消标志
    m_chatStart = std::chrono::steady_clock::now();   // TTFT 计时起点

    //设置策略
    setStrategy(StrategyFactory::instance().create(modelType));

    // 是否真正走流式：调用方要求 + 模型支持 + 已设置回调
    const bool useStream = stream && strategy->isStreamSupported() && m_streamCallback;

    // chat 结束时清空回调，防止跨请求悬挂（Handler 每次重新设置）
    struct CallbackGuard {
        std::function<void(const std::string&)>& slot;
        ~CallbackGuard() { slot = nullptr; }
    } cbGuard{m_streamCallback};

    if (false == strategy->isMCPModel) {

        addMessage(userId, userName, true, userQuestion, sessionId);
        json payload = strategy->buildRequest(this->messages);

        std::string answer;
        if (useStream) {
            // ---- 流式路径 ----
            resetStreamState();
            json streamPayload = payload;
            streamPayload["stream"] = true;
            try {
                json response = executeCurl(streamPayload);
                answer = strategy->parseResponse(response);
            } catch (const std::exception& e) {
                if (m_abortRequested) {
                    // 客户端已断开：直接收尾，不降级重发（重发也没人收）
                    answer = m_streamedAnswer;
                } else if (m_streamDeltaCount == 0) {
                    // 零增量：降级重发一次非流式请求（保证功能不倒退）
                    std::cout << "[SSE] stream failed with no delta, fallback: "
                              << e.what() << std::endl;
                    resetStreamState();
                    answer = strategy->parseResponse(executeCurl(payload));
                } else {
                    // 已有增量：把收到的部分当完整答案收尾
                    std::cout << "[SSE] WARN stream interrupted after "
                              << m_streamDeltaCount << " deltas, finalize partial" << std::endl;
                    answer = m_streamedAnswer;
                }
            }
        } else {
            // ---- 非流式路径（原有逻辑） ----
            json response = executeCurl(payload);
            answer = strategy->parseResponse(response);
        }

        addMessage(userId, userName, false, answer, sessionId);
        return answer.empty() ? "[Error] 无法解析响应" : answer;
    }
    //说明支持MCP
    AIConfig config;
    config.loadFromFile("../AIApps/ChatServer/resource/config.json");
    std::string tempUserQuestion =config.buildPrompt(userQuestion);
    std::cout << "tempUserQuestion is " << tempUserQuestion << std::endl;
    messages.push_back({ tempUserQuestion, 0 });

    // 第 1 段：模型决策是否调工具——必须全量（要等完整 JSON 才能阅卷），不开流式
    resetStreamState();
    json firstReq = strategy->buildRequest(this->messages);
    json firstResp = executeCurl(firstReq);
    std::string aiResult = strategy->parseResponse(firstResp);
    // 用完立即移除提示词
    messages.pop_back();

    std::cout << "aiResult is " << aiResult << std::endl;
    // 解析AI响应（是否工具调用）
    AIToolCall call = config.parseAIResponse(aiResult);

    // 情况1：AI 不调用工具
    if (!call.isToolCall) {
        addMessage(userId, userName, true, userQuestion, sessionId);
        addMessage(userId, userName, false, aiResult, sessionId);

        std::cout << "No tools required" << std::endl;
        return aiResult;
    }

    // 情况 2：AI 要调用工具
    json toolResult;
    AIToolRegistry registry;

    try {
        toolResult = registry.invoke(call.toolName, call.args);
        std::cout << "Tool call success" << std::endl;
    }
    catch (const std::exception& e) {
        //大多数情况都不会走这里
        std::string err = "[工具调用失败] " + std::string(e.what());
        addMessage(userId, userName, true, userQuestion, sessionId);
        addMessage(userId, userName, false, err, sessionId);

        std::cout << "Tool call failed" << std::endl << std::string(e.what());
        return err;
    }

    // 第二次调用AI
    // 用同样的 prompt_template，但说明工具执行过
    std::string secondPrompt = config.buildToolResultPrompt(userQuestion, call.toolName, call.args, toolResult);

    std::cout << "secondPrompt is " << secondPrompt << std::endl;
    messages.push_back({ secondPrompt, 0 });

    std::string finalAnswer;
    json secondReq = strategy->buildRequest(messages);
    if (useStream) {
        // 第 2 段：拿工具结果组织最终答案——开流式，增量往外推
        resetStreamState();
        json streamPayload = secondReq;
        streamPayload["stream"] = true;
        try {
            json secondResp = executeCurl(streamPayload);
            finalAnswer = strategy->parseResponse(secondResp);
        } catch (const std::exception& e) {
            if (m_abortRequested) {
                // 客户端已断开：直接收尾，不降级重发
                finalAnswer = m_streamedAnswer;
            } else if (m_streamDeltaCount == 0) {
                std::cout << "[SSE] mcp round2 stream failed with no delta, fallback: "
                          << e.what() << std::endl;
                resetStreamState();
                finalAnswer = strategy->parseResponse(executeCurl(secondReq));
            } else {
                std::cout << "[SSE] WARN mcp round2 interrupted after "
                          << m_streamDeltaCount << " deltas, finalize partial" << std::endl;
                finalAnswer = m_streamedAnswer;
            }
        }
    } else {
        json secondResp = executeCurl(secondReq);
        finalAnswer = strategy->parseResponse(secondResp);
    }
    //删除包含提示词的信息
    messages.pop_back();

    std::cout << "finalAnswer is " << finalAnswer << std::endl;

    addMessage(userId, userName, true, userQuestion, sessionId);
    addMessage(userId, userName, false, finalAnswer, sessionId);
    return finalAnswer;

}

// 发送自定义请求体
json AIHelper::request(const json& payload) {
    return executeCurl(payload);
}

std::vector<std::pair<std::string, long long>> AIHelper::GetMessages() {
    return this->messages;
}


// 内部方法：执行 curl 请求
// 流式请求（payload 带 stream:true 且已设回调）时：边收边推增量，
// 返回由增量拼成的标准形状响应（choices[0].message.content），parseResponse 无感
json AIHelper::executeCurl(const json& payload) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize curl");
    }

    // 日志严禁打印 key 明文，仅打印前 6 位 + "..."
    std::string maskedKey = strategy->getApiKey();
    if (maskedKey.size() > 6) maskedKey = maskedKey.substr(0, 6) + "...";
    std::cout << "test " << strategy->getApiUrl() << ' ' << maskedKey << std::endl;

    // 回调上下文：全量累积 + 可选流式解析
    CurlCtx ctx;
    ctx.self = this;
    ctx.wantStream = payload.value("stream", false) && m_streamCallback != nullptr;

    struct curl_slist* headers = nullptr;
    std::string authHeader = "Authorization: Bearer " + strategy->getApiKey();

    headers = curl_slist_append(headers, authHeader.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    std::string payloadStr = payload.dump();


    curl_easy_setopt(curl, CURLOPT_URL, strategy->getApiUrl().c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payloadStr.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error("curl_easy_perform() failed: " + std::string(curl_easy_strerror(res)));
    }

    // 流式路径：已收集到增量（或收到 [DONE]）→ 用拼接答案构造标准响应
    if (ctx.wantStream && !m_sse.plainJson && (m_streamDeltaCount > 0 || m_sse.done)) {
        // 处理流结束仍残留在 pending 里的最后一段（无结尾分隔符的尾巴）
        std::vector<std::string> tail;
        m_sse.flush(tail);
        for (auto& d : tail) {
            m_streamedAnswer += d;
            ++m_streamDeltaCount;
            if (m_streamCallback) m_streamCallback(d);
        }
        // SSE 最后一帧可能携带 usage（容错解析到的值）
        m_lastPromptTokens += m_sse.usagePrompt;
        m_lastCompletionTokens += m_sse.usageCompletion;

        json fake;
        fake["choices"] = json::array({ json{{"message", json{{"content", m_streamedAnswer}}}} });
        return fake;
    }

    // 非流式路径（原有逻辑）：整体解析全量 JSON
    try {
        json response = json::parse(ctx.buffer);

        // 解析 LLM 用量（usage 字段），MCP 两段式会自动累加两段消耗
        if (response.contains("usage")) {
            m_lastPromptTokens += response["usage"].value("prompt_tokens", 0);
            m_lastCompletionTokens += response["usage"].value("completion_tokens", 0);
        }
        return response;
    }
    catch (...) {
        throw std::runtime_error("Failed to parse JSON response: " + ctx.buffer);
    }
}

// curl 回调函数：写全量缓冲；期望流式且格式判定为 SSE 时喂解析器
// 返回 0（非 totalSize）会让 curl 立刻中止下载（CURLE_WRITE_ERROR）——
// 用于客户端断开连接时取消上游 LLM 流，不再浪费 token
size_t AIHelper::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    CurlCtx* ctx = static_cast<CurlCtx*>(userp);
    ctx->buffer.append(static_cast<char*>(contents), totalSize);
    if (ctx->wantStream && ctx->self) {
        if (ctx->self->abortRequested()) {
            return 0;   // 取消：中止下载，executeCurl 将抛 CURLE_WRITE_ERROR
        }
        ctx->self->processStreamChunk(
            std::string(static_cast<char*>(contents), totalSize));
    }
    return totalSize;
}

// WriteCallback 内部：喂 SSE 解析器，逐 delta 触发回调并记录 TTFT
// 首块数据若不是 "data:" 开头则判定为全量 JSON（服务端忽略 stream），停止流式
void AIHelper::processStreamChunk(const std::string& chunk) {
    std::vector<std::string> deltas;
    if (!m_sse.feed(chunk, deltas)) {
        return; // 非 SSE 格式，交给全量路径
    }
    for (auto& d : deltas) {
        if (m_streamDeltaCount == 0) {
            // 首 token 延迟（TTFT，毫秒），格式统一便于 grep
            auto ttft = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - m_chatStart).count();
            std::cout << "[SSE] TTFT=" << ttft << "ms" << std::endl;
        }
        m_streamedAnswer += d;
        ++m_streamDeltaCount;
        if (m_streamCallback) m_streamCallback(d);
    }
}

// 重置一次流式请求的解析状态（executeCurl / chat 各段调用前）
void AIHelper::resetStreamState() {
    m_sse = SseParser{};
    m_streamedAnswer.clear();
    m_streamDeltaCount = 0;
}

std::string AIHelper::escapeString(const std::string& input) {
    std::string output;
    output.reserve(input.size() * 2);
    for (char c : input) {
        switch (c) {
            case '\\': output += "\\\\"; break;
            case '\'': output += "\\\'"; break;
            case '\"': output += "\\\""; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:   output += c; break;
        }
    }
    return output;
}


void AIHelper::pushMessageToMysql(int userId, const std::string& userName, bool is_user, const std::string& userInput,long long ms, std::string sessionId) {
    // std::string sql = "INSERT INTO chat_message (id, username, is_user, content, ts) VALUES ("
    //     + std::to_string(userId) + ", "  // 这里用 userId 作为 id，或者你自己生成
    //     + "'" + userName + "', "
    //     + std::to_string(is_user ? 1 : 0) + ", "
    //     + "'" + userInput + "', "
    //     + std::to_string(ms) + ")";
    std::string safeUserName = escapeString(userName);
    std::string safeUserInput = escapeString(userInput);

    // 用量列：仅 AI 回复行（is_user=0）携带本轮 token 消耗，用户消息行填 0
    int promptTokens     = is_user ? 0 : m_lastPromptTokens;
    int completionTokens = is_user ? 0 : m_lastCompletionTokens;

    std::string sql = "INSERT INTO chat_message "
        "(id, username, session_id, is_user, content, ts, model, prompt_tokens, completion_tokens) VALUES ("
        + std::to_string(userId) + ", "
        + "'" + safeUserName + "', "
        + sessionId + ", "
        + std::to_string(is_user ? 1 : 0) + ", "
        + "'" + safeUserInput + "', "
        + std::to_string(ms) + ", "
        + "'" + m_curModel + "', "
        + std::to_string(promptTokens) + ", "
        + std::to_string(completionTokens) + ")";

    //改成消息队列异步执行mysql操作，用于流量削峰与解耦逻辑
    //mysqlUtil_.executeUpdate(sql);

    MQManager::instance().publish("sql_queue", sql);
}

