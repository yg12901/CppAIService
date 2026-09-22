#include "../include/handlers/ChatLogoutHandler.h"

// 登出接口 POST /user/logout
// clear 会话内容 → destroySession 从 storage 删除 → erase onlineUsers_ 释放登录态
void ChatLogoutHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp)
{
    // 身份在 Cookie 里，body 不用。以前还卡 Content-Type 且要求非空 JSON，纯属复制登录逻辑。
    try
    {

        auto session = server_->getSessionManager()->getSession(req, resp);

        int userId = std::stoi(session->getValue("userId"));

        session->clear();

        server_->getSessionManager()->destroySession(session->getId());

        server_->markOffline(userId);


        json response;
        response["message"] = "logout successful";
        std::string responseBody = response.dump(4);
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
        resp->setCloseConnection(true);
        resp->setContentType("application/json");
        resp->setContentLength(responseBody.size());
        resp->setBody(responseBody);
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