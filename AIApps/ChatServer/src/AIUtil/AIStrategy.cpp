#include"../include/AIUtil/AIStrategy.h"
#include"../include/AIUtil/AIFactory.h"

// 4 个策略实现：阿里百炼(qwen-plus) / 豆包(doubao-seed) / 阿里RAG(嵌套 JSON) / 阿里MCP(工具调用)
// 底部 4 个 static StrategyRegister 在程序启动时自动注册到工厂

namespace {

json messagesToArray(const std::vector<ChatMessage>& messages) {
    json msgArray = json::array();
    for (const auto& m : messages) {
        json msg;
        msg["role"] = m.role;
        msg["content"] = m.content;
        msgArray.push_back(std::move(msg));
    }
    return msgArray;
}

} // namespace

std::string AliyunStrategy::getApiUrl() const {
    return "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";
}

std::string AliyunStrategy::getApiKey()const {
    return apiKey_;
}


std::string AliyunStrategy::getModel() const {
    return "qwen-plus";
}


json AliyunStrategy::buildRequest(const std::vector<ChatMessage>& messages) const {
    json payload;
    payload["model"] = getModel();
    payload["messages"] = messagesToArray(messages);
    return payload;
}


std::string AliyunStrategy::parseResponse(const json& response) const {
    if (response.contains("choices") && !response["choices"].empty()) {
        return response["choices"][0]["message"]["content"];
    }
    return {};
}


std::string DouBaoStrategy::getApiUrl()const {
    return "https://ark.cn-beijing.volces.com/api/v3/chat/completions";
}

std::string DouBaoStrategy::getApiKey()const {
    return apiKey_;
}


std::string DouBaoStrategy::getModel() const {
    return "doubao-seed-1-6-thinking-250715";
}


json DouBaoStrategy::buildRequest(const std::vector<ChatMessage>& messages) const {
    json payload;
    payload["model"] = getModel();
    payload["messages"] = messagesToArray(messages);
    return payload;
}


std::string DouBaoStrategy::parseResponse(const json& response) const {
    if (response.contains("choices") && !response["choices"].empty()) {
        return response["choices"][0]["message"]["content"];
    }
    return {};
}


std::string AliyunRAGStrategy::getApiUrl() const {
    const char* key = std::getenv("Knowledge_Base_ID");
    if (!key) throw std::runtime_error("Knowledge_Base_ID not found!");
    std::string id(key);
    //϶Ӧ֪ʶID
    return "https://dashscope.aliyuncs.com/api/v1/apps/"+id+"/completion";
}

std::string AliyunRAGStrategy::getApiKey()const {
    return apiKey_;
}


std::string AliyunRAGStrategy::getModel() const {
    return ""; //Ҫģ
}


json AliyunRAGStrategy::buildRequest(const std::vector<ChatMessage>& messages) const {
    json payload;
    payload["input"]["messages"] = messagesToArray(messages);
    payload["parameters"] = json::object(); 
    return payload;
}


std::string AliyunRAGStrategy::parseResponse(const json& response) const {
    if (response.contains("output") && response["output"].contains("text")) {
        return response["output"]["text"];
    }
    // 流式收尾时 executeCurl 会拼一份 OpenAI 形状的 fake，两边都能拆
    if (response.contains("choices") && !response["choices"].empty()) {
        return response["choices"][0]["message"].value("content", std::string());
    }
    return {};
}

void AliyunRAGStrategy::prepareStreamRequest(json& payload) const {
    // 应用 completion 不认顶层 stream:true。增量靠 parameters + SSE 头。
    if (!payload.contains("parameters") || !payload["parameters"].is_object()) {
        payload["parameters"] = json::object();
    }
    payload["parameters"]["incremental_output"] = true;
}

std::vector<std::string> AliyunRAGStrategy::extraHttpHeaders(bool stream) const {
    if (!stream) return {};
    return {"X-DashScope-SSE: enable"};
}



std::string AliyunMcpStrategy::getApiUrl() const {
    return "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";
}

std::string AliyunMcpStrategy::getApiKey()const {
    return apiKey_;
}


std::string AliyunMcpStrategy::getModel() const {
    return "qwen-plus";
}


json AliyunMcpStrategy::buildRequest(const std::vector<ChatMessage>& messages) const {
    json payload;
    payload["model"] = getModel();
    payload["messages"] = messagesToArray(messages);
    return payload;
}


std::string AliyunMcpStrategy::parseResponse(const json& response) const {
    if (response.contains("choices") && !response["choices"].empty()) {
        return response["choices"][0]["message"]["content"];
    }
    return {};
}


static StrategyRegister<AliyunStrategy> regAliyun("1");
static StrategyRegister<DouBaoStrategy> regDoubao("2");
static StrategyRegister<AliyunRAGStrategy> regAliyunRag("3");
static StrategyRegister<AliyunMcpStrategy> regAliyunMcp("4");