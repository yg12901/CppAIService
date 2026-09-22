#pragma once
#include <string>
#include <vector>
#include <utility>
#include <iostream>
#include <sstream>
#include <memory>

#include "../../../../HttpServer/include/utils/JsonUtil.h"


// 一条对话消息。role 写在数据里，不再靠 vector 下标奇偶猜测。
// 取值约定与 OpenAI 兼容接口一致：user / assistant / system。
struct ChatMessage {
    std::string role;
    std::string content;
    long long   timestamp{0};
};

inline const char* roleFromIsUser(bool is_user) {
    return is_user ? "user" : "assistant";
}

/**
 * AIStrategy - 策略模式基类
 * 定义多模型厂商的统一调用接口，子类各自实现请求构建与响应解析
 */
class AIStrategy {
public:
    virtual ~AIStrategy() = default;

    virtual std::string getApiUrl() const = 0;

    // API Key
    virtual std::string getApiKey() const = 0;


    virtual std::string getModel() const = 0;


    virtual json buildRequest(const std::vector<ChatMessage>& messages) const = 0;


    virtual std::string parseResponse(const json& response) const = 0;

    bool isMCPModel = false;

    // 是否支持 SSE 流式输出。默认 false；四家对话策略都 override 为 true。
    virtual bool isStreamSupported() const { return false; }

    // 流式时改请求体。默认 OpenAI 兼容：顶层 stream=true。
    // RAG 应用 API 不认这个字段，子类改 parameters.incremental_output。
    virtual void prepareStreamRequest(json& payload) const {
        payload["stream"] = true;
    }

    // 流式时追加的 HTTP 头。RAG 需要 X-DashScope-SSE: enable。
    virtual std::vector<std::string> extraHttpHeaders(bool /*stream*/) const {
        return {};
    }

};

// 阿里百炼策略：对接通义千问 qwen-plus
class AliyunStrategy : public AIStrategy {

public:
    AliyunStrategy() {
        const char* key = std::getenv("DASHSCOPE_API_KEY");
        if (!key) throw std::runtime_error("Aliyun API Key not found!");
        apiKey_ = key;
        isMCPModel = false;
    }

    std::string getApiUrl() const override;
    std::string getApiKey() const override;
    std::string getModel() const override;

    json buildRequest(const std::vector<ChatMessage>& messages) const override;
    std::string parseResponse(const json& response) const override;

    bool isStreamSupported() const override { return true; }   // 百炼支持流式

private:
    std::string apiKey_;
};

// 豆包策略：对接火山引擎 doubao-seed 思考模型（上下文 128K）
class DouBaoStrategy : public AIStrategy {

public:
    DouBaoStrategy() {
        const char* key = std::getenv("DOUBAO_API_KEY");
        if (!key) throw std::runtime_error("DOUBAO API Key not found!");
        apiKey_ = key;
        isMCPModel = false;
    }
    std::string getApiUrl() const override;
    std::string getApiKey() const override;
    std::string getModel() const override;

    json buildRequest(const std::vector<ChatMessage>& messages) const override;
    std::string parseResponse(const json& response) const override;

    bool isStreamSupported() const override { return true; }   // 豆包支持流式

private:
    std::string apiKey_;
};

// 阿里百炼 RAG 策略：对接百炼应用 API，支持知识库检索增强
class AliyunRAGStrategy : public AIStrategy {

public:
    AliyunRAGStrategy() {
        const char* key = std::getenv("DASHSCOPE_API_KEY");
        if (!key) throw std::runtime_error("Aliyun API Key not found!");
        apiKey_ = key;
        isMCPModel = false;
    }

    std::string getApiUrl() const override;
    std::string getApiKey() const override;
    std::string getModel() const override;

    json buildRequest(const std::vector<ChatMessage>& messages) const override;
    std::string parseResponse(const json& response) const override;

    bool isStreamSupported() const override { return true; }   // 应用 API 走 SSE
    void prepareStreamRequest(json& payload) const override;
    std::vector<std::string> extraHttpHeaders(bool stream) const override;

private:
    std::string apiKey_;
};

// 阿里百炼 MCP 策略：支持工具调用，走两段式推理流程
class AliyunMcpStrategy : public AIStrategy {

public:
    AliyunMcpStrategy() {
        const char* key = std::getenv("DASHSCOPE_API_KEY");
        if (!key) throw std::runtime_error("Aliyun API Key not found!");
        apiKey_ = key;
        isMCPModel = true;
    }

    std::string getApiUrl() const override;
    std::string getApiKey() const override;
    std::string getModel() const override;

    json buildRequest(const std::vector<ChatMessage>& messages) const override;
    std::string parseResponse(const json& response) const override;

    bool isStreamSupported() const override { return true; }   // MCP 第二段可流式

private:
    std::string apiKey_;
};







