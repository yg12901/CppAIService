#include "../include/handlers/KbUploadSendHandler.h"
#include "../include/AIUtil/BailianKnowledgeClient.h"
#include "../include/AIUtil/base64.h"
#include <stdexcept>

// POST /kb/upload  共享知识库灌库：平台收文件 → 百炼租约/入库/追加索引
void KbUploadSendHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp)
{
    try
    {
        auto session = server_->getSessionManager()->getSession(req, resp);
        if (session->getValue("isLoggedIn") != "true")
        {
            json errorResp;
            errorResp["status"] = "error";
            errorResp["message"] = "Unauthorized";
            std::string errorBody = errorResp.dump(4);
            server_->packageResp(req.getVersion(), http::HttpResponse::k401Unauthorized,
                "Unauthorized", true, "application/json", errorBody.size(),
                errorBody, resp);
            return;
        }

        if (session->getValue("canChat") != "true")
        {
            json errorResp;
            errorResp["status"] = "error";
            errorResp["message"] = "knowledge upload not permitted for your account";
            std::string errorBody = errorResp.dump(4);
            server_->packageResp(req.getVersion(), http::HttpResponse::k403Forbidden,
                "Forbidden", true, "application/json", errorBody.size(),
                errorBody, resp);
            return;
        }

        if (req.getBody().size() > 18ull * 1024ull * 1024ull)
        {
            throw std::runtime_error("请求体过大（解码后请控制在 12MB 以内）");
        }

        std::string filename;
        std::string fileBase64;
        auto body = req.getBody();
        if (!body.empty())
        {
            auto j = json::parse(body);
            if (j.contains("filename")) filename = j["filename"].get<std::string>();
            if (j.contains("file")) fileBase64 = j["file"].get<std::string>();
        }
        if (fileBase64.empty()) throw std::runtime_error("No file data provided");

        std::string bytes = base64_decode(fileBase64);
        auto result = BailianKnowledgeClient::instance().ingest(filename, bytes);

        json successResp;
        successResp["success"] = true;
        successResp["filename"] = result.filename;
        successResp["fileId"] = result.fileId;
        successResp["jobId"] = result.jobId;
        successResp["message"] = "已提交到共享知识库，索引完成后所有用户选「百炼RAG」都能问到";
        std::string successBody = successResp.dump(4);

        resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
        resp->setCloseConnection(false);
        resp->setContentType("application/json");
        resp->setContentLength(successBody.size());
        resp->setBody(successBody);
    }
    catch (const std::exception& e)
    {
        json failureResp;
        failureResp["status"] = "error";
        failureResp["message"] = e.what();
        std::string failureBody = failureResp.dump(4);
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
        resp->setCloseConnection(true);
        resp->setContentType("application/json");
        resp->setContentLength(failureBody.size());
        resp->setBody(failureBody);
    }
}
