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
    messages.push_back({ roleFromIsUser(is_user), userInput, ms });
    //消息队列异步入库
    pushMessageToMysql(userId, userName, is_user, userInput, ms, sessionId);
}

void AIHelper::restoreMessage(const std::string& userInput,long long ms, bool is_user) {
    messages.push_back({ roleFromIsUser(is_user), userInput, ms });
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
            // ---- 流式路径：协议细节（stream:true / SSE 头）收口在策略里 ----
            resetStreamState();
            try {
                json response = executeCurl(payload, true);
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
    // 单例取配置：原来每次对话都要开一次文件、解析一次 JSON、编译两个正则，
    // 这些开销全压在用户等待的链路上，而配置内容从头到尾没变过。
    const AIConfig& config = AIConfig::instance();
    std::string tempUserQuestion =config.buildPrompt(userQuestion);
    std::cout << "tempUserQuestion is " << tempUserQuestion << std::endl;
    messages.push_back({ "user", tempUserQuestion, 0 });

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
        std::string plainAnswer = aiResult;

        // 区分两种"不调工具"：
        //  a) rejectReason 为空 —— 模型本来就是正常文本作答，直接用。
        //  b) rejectReason 非空 —— 模型想调工具但没过格式校验（编了个不存在的工具名、
        //     参数缺一半）。此时 aiResult 是一坨内部协议 JSON，原样返回等于把
        //     提示词协议泄露给用户。重发一次不带工具提示词的纯净问题，让它好好说人话。
        if (!call.rejectReason.empty()) {
            std::cout << "[MCP] fallback to plain answer, reason=" << call.rejectReason << std::endl;
            messages.push_back({ "user", userQuestion, 0 });
            try {
                std::string retry = strategy->parseResponse(executeCurl(strategy->buildRequest(messages)));
                if (!retry.empty()) plainAnswer = retry;
            }
            catch (const std::exception& e) {
                // 重试失败不影响主流程，沿用第一段的原始输出
                std::cout << "[MCP] plain retry failed: " << e.what() << std::endl;
            }
            messages.pop_back();
        }

        addMessage(userId, userName, true, userQuestion, sessionId);
        addMessage(userId, userName, false, plainAnswer, sessionId);

        std::cout << "No tools required" << std::endl;
        return plainAnswer;
    }

    // 情况 2：AI 要调用工具
    json toolResult;
    // 注册表只读，构造一次复用：每次请求 new 一张 hash 表纯属浪费
    static const AIToolRegistry registry;

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
    messages.push_back({ "user", secondPrompt, 0 });

    std::string finalAnswer;
    json secondReq = strategy->buildRequest(messages);
    if (useStream) {
        // 第 2 段：拿工具结果组织最终答案——开流式，增量往外推
        resetStreamState();
        try {
            json secondResp = executeCurl(secondReq, true);
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

std::vector<ChatMessage> AIHelper::GetMessages() {
    return this->messages;
}


// 内部方法：执行 curl 请求
// stream=true 时：按策略改请求体/头，边收边推增量；
// 返回由增量拼成的响应（同时带 choices 和 output.text，parseResponse 无感）
json AIHelper::executeCurl(const json& payload, bool stream) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize curl");
    }

    // 日志严禁打印 key 明文，仅打印前 6 位 + "..."
    std::string maskedKey = strategy->getApiKey();
    if (maskedKey.size() > 6) maskedKey = maskedKey.substr(0, 6) + "...";
    std::cout << "test " << strategy->getApiUrl() << ' ' << maskedKey << std::endl;

    json body = payload;
    if (stream) {
        strategy->prepareStreamRequest(body);
    }

    // 回调上下文：全量累积 + 可选流式解析
    CurlCtx ctx;
    ctx.self = this;
    ctx.wantStream = stream && m_streamCallback != nullptr;

    struct curl_slist* headers = nullptr;
    std::string authHeader = "Authorization: Bearer " + strategy->getApiKey();

    headers = curl_slist_append(headers, authHeader.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    // extra 必须活过 curl_easy_perform：curl_slist 只存指针
    const std::vector<std::string> extra = strategy->extraHttpHeaders(stream);
    for (const auto& h : extra) {
        headers = curl_slist_append(headers, h.c_str());
    }

    std::string payloadStr = body.dump();


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
        fake["output"]["text"] = m_streamedAnswer;
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
// 首块以 '{' 起头视为全量 JSON（服务端没走 SSE），停止流式
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

void AIHelper::pushMessageToMysql(int userId, const std::string& userName, bool is_user, const std::string& userInput,long long ms, std::string sessionId) {
    // 改造前这里是手工拼 SQL 字符串 + 自己写 escapeString 转义，有两个问题：
    //  ① sessionId 直接拼进来，连转义都没做（因为它被当成数字列）。
    //     它来自请求体，客户端传 "1,0,'x',0)-- " 之类就能改写整条语句。
    //  ② 自研转义永远追不上边界情况（字符集、\0、注释符），这是公认的错误做法。
    // 现在队列里传的是结构化 JSON，SQL 语句在消费端写死、参数走预处理绑定，
    // 用户输入从此没有任何机会被当成 SQL 解析。
    //
    // 顺带的好处：消息自描述了，排查时能直接看懂队列里堆的是什么，
    // 而不是一坨拼好的 SQL。

    // 用量列：仅 AI 回复行（is_user=0）携带本轮 token 消耗，用户消息行填 0
    int promptTokens     = is_user ? 0 : m_lastPromptTokens;
    int completionTokens = is_user ? 0 : m_lastCompletionTokens;

    json payload;
    payload["type"]              = "chat_message";   // 预留：以后可复用同一队列投递别的写操作
    payload["id"]                = userId;
    payload["username"]          = userName;
    payload["session_id"]        = sessionId;
    payload["is_user"]           = is_user ? 1 : 0;
    payload["content"]           = userInput;
    payload["ts"]                = ms;
    payload["model"]             = m_curModel;
    payload["prompt_tokens"]     = promptTokens;
    payload["completion_tokens"] = completionTokens;

    //改成消息队列异步执行mysql操作，用于流量削峰与解耦逻辑
    MQManager::instance().publish("sql_queue", payload.dump());
}

