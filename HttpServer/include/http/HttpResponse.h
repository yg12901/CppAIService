#pragma once

#include <muduo/net/TcpServer.h>
#include <functional>
#include <string>

namespace http
{

class HttpResponse
{
public:
    enum HttpStatusCode
    {
        kUnknown,
        k200Ok = 200,
        k204NoContent = 204,
        k301MovedPermanently = 301,
        k400BadRequest = 400,
        k401Unauthorized = 401,
        k403Forbidden = 403,
        k404NotFound = 404,
        k409Conflict = 409,
        k500InternalServerError = 500,
    };

    HttpResponse(bool close = true)
        : statusCode_(kUnknown)
        , closeConnection_(close)
    {}

    // ---------------- 流式响应扩展（SSE 等） ----------------
    // Handler 声明流式模式：框架跳过统一序列化发送，由 Handler 通过
    // streamSender 自行多次写连接（如 LLM 增量透传）
    void setStreaming(bool on) { streaming_ = on; }
    bool isStreaming() const { return streaming_; }

    // 注入底层连接写能力（由 HttpServer::onRequest 设置，Handler 只读）
    void setStreamSender(std::function<void(const std::string&)> sender)
    { streamSender_ = std::move(sender); }

    // 取写接口；未注入（单元测试等场景）时返回 false
    bool sendChunk(const std::string& data)
    {
        if (!streamSender_) return false;
        streamSender_(data);
        return true;
    }

    void setVersion(std::string version)
    { httpVersion_ = version; }
    void setStatusCode(HttpStatusCode code)
    { statusCode_ = code; }

    HttpStatusCode getStatusCode() const
    { return statusCode_; }

    void setStatusMessage(const std::string message)
    { statusMessage_ = message; }

    void setCloseConnection(bool on)
    { closeConnection_ = on; }

    bool closeConnection() const
    { return closeConnection_; }
    
    void setContentType(const std::string& contentType)
    { addHeader("Content-Type", contentType); }

    void setContentLength(uint64_t length)
    { addHeader("Content-Length", std::to_string(length)); }

    void addHeader(const std::string& key, const std::string& value)
    { headers_[key] = value; }
    
    void setBody(const std::string& body)
    { 
        body_ = body;
        // body_ += "\0";
    }

    void setStatusLine(const std::string& version,
                         HttpStatusCode statusCode,
                         const std::string& statusMessage);

    void setErrorHeader(){}

    void appendToBuffer(muduo::net::Buffer* outputBuf) const;
private:
    std::string                        httpVersion_;
    HttpStatusCode                     statusCode_;
    std::string                        statusMessage_;
    bool                               closeConnection_;
    std::map<std::string, std::string> headers_;
    std::string                        body_;
    bool                               isFile_;
    // 流式响应状态
    bool                               streaming_ = false;
    std::function<void(const std::string&)> streamSender_;
};

} // namespace http