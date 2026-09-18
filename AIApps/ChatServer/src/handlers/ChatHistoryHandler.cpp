#include "../include/handlers/ChatHistoryHandler.h"

// 历史记录接口 POST /chat/history
// 鉴权后从 chatInformation 查找会话 AIHelper，拷贝 messages 快照返回给前端
void ChatHistoryHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp)
{
    try
    {

        auto session = server_->getSessionManager()->getSession(req, resp);
        LOG_INFO << "session->getValue(\"isLoggedIn\") = " << session->getValue("isLoggedIn");
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


        int userId = std::stoi(session->getValue("userId"));
        std::string username = session->getValue("username");

        std::string sessionId;
        auto body = req.getBody();
        if (!body.empty()) {
            auto j = json::parse(body);
            if (j.contains("sessionId")) sessionId = j["sessionId"];
        }

        std::vector<ChatMessage> messages;

        // 拉历史是纯读，走 shared_lock 快路径，多个用户可以同时拉。
        // 原实现用 operator[]，查一个不存在的 sessionId 会顺手建一个空 AIHelper
        // 塞进 map —— 既是"读接口偷偷写数据"，也让伪造 sessionId 能无限撑大内存。
        // 现在查不到就直接返回空历史，对前端的表现完全一致。
        auto AIHelperPtr = server_->findChatHelper(userId, sessionId);
        if (AIHelperPtr) {
            messages = AIHelperPtr->GetMessages();
        }


        json successResp;
        successResp["success"] = true;
        successResp["history"] = json::array();

        for (const auto& m : messages) {
            json msgJson;
            msgJson["role"] = m.role;
            msgJson["is_user"] = (m.role == "user");
            msgJson["content"] = m.content;
            successResp["history"].push_back(msgJson);
        }

        std::string successBody = successResp.dump(4);

        resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
        resp->setCloseConnection(false);
        resp->setContentType("application/json");
        resp->setContentLength(successBody.size());
        resp->setBody(successBody);
        return;
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









