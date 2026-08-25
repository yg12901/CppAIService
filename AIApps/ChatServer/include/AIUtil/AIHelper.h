#pragma once
#include <string>
#include <vector>
#include <utility>
#include <functional>
#include <chrono>
#include <curl/curl.h>
#include <iostream>
#include <sstream>

#include "../../../../HttpServer/include/utils/JsonUtil.h"
#include"../../../../HttpServer/include/utils/MysqlUtil.h"

#include"AIFactory.h"
#include"AIConfig.h"
#include"AIToolRegistry.h"
#include"SseParser.h"

//这边封装curl去访问对阿里的模型
class AIHelper {
public:
    // 构造函数，初始化API Key
    AIHelper();

    // 设置默认模型
    //void setModel(const std::string& modelName);

    void setStrategy(std::shared_ptr<AIStrategy> strat);

    // 添加一条消息
    void addMessage(int userId, const std::string& userName, bool is_user, const std::string& userInput, std::string sessionId);
    // 恢复一条消息
    void restoreMessage(const std::string& userInput, long long ms);

    // 发送聊天消息，返回AI的响应内容（完整答案，流式时为增量拼接结果）
    // stream=true 时：模型支持且已设置回调则走 SSE 流式，边收边推增量
    std::string chat(int userId, std::string userName, std::string sessionId, std::string userQuestion, std::string modelType, bool stream = false);

    // 设置流式增量回调（Handler 在调 chat 前设置；chat 结束时自动清空防止悬挂）
    void setStreamCallback(std::function<void(const std::string& delta)> cb)
    { m_streamCallback = std::move(cb); }

    // 当前策略是否支持 SSE 流式（Handler 据此决定响应模式）
    bool isStreamSupported() const { return strategy && strategy->isStreamSupported(); }

    // 可选：发送自定义请求体
    json request(const json& payload);

    std::vector<std::pair<std::string, long long>> GetMessages();

private:
    std::string escapeString(const std::string& input);
    //加入到mysql的接口（提供加入到线程池的接口，线程池做异步mysql更新操作）
    //todo:
    void pushMessageToMysql(int userId, const std::string& userName, bool is_user, const std::string& userInput, long long ms, std::string sessionId);

    // 内部方法：执行curl请求，返回原始JSON（流式时返回由增量拼成的标准形状响应）
    json executeCurl(const json& payload);

    // curl 回调上下文：累积原始响应 + 触发流式解析
    struct CurlCtx {
        std::string buffer;        // 原始响应全量累积（非流式路径/兜底用）
        AIHelper* self = nullptr;  // 流式解析宿主
        bool wantStream = false;   // 本次请求是否期望流式
    };
    // curl 回调函数，把返回的数据写到 ctx 并按需触发流式解析
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);

    // WriteCallback 内部：喂 SSE 解析器，逐个 delta 触发回调并记录 TTFT
    void processStreamChunk(const std::string& chunk);

    // 重置一次请求的流式状态
    void resetStreamState();

private:

    /*
    * 重构代码，将其使用策略模式&&工厂模式抽离出来
    std::string apiKey_;
    //默认用通义千问
    std::string model_ = "qwen-plus";
    //对应地址
    std::string apiUrl_ = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";
    */
    std::shared_ptr<AIStrategy> strategy;

    //一个用户针对一个AIHelper，messages存放用户的历史对话
    //偶数下标代表用户的信息，奇数下标是ai返回的内容
    //后者代表时间戳
    std::vector<std::pair<std::string, long long>> messages;

    // 用量统计：最近一次 chat 的 token 消耗（MCP 两段式会累加两段）
    int m_lastPromptTokens = 0;
    int m_lastCompletionTokens = 0;
    // 当前 chat 使用的模型标识（入库用，用户消息行也记录）
    std::string m_curModel;

    // ---------------- 流式状态（单次 chat 内生命周期） ----------------
    std::function<void(const std::string& delta)> m_streamCallback; // 增量转发回调
    SseParser   m_sse;              // SSE 解析状态机（跨 WriteCallback 保留）
    std::string m_streamedAnswer;   // 已收到的增量拼接（完整答案）
    int         m_streamDeltaCount = 0; // 已收到的增量条数（判降级用）
    std::chrono::steady_clock::time_point m_chatStart; // chat 起点（算 TTFT）
};
