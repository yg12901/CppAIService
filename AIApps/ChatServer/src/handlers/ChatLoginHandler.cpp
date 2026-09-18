#include "../include/handlers/ChatLoginHandler.h"
#include "../include/AIUtil/UserAuthDao.h"

// 登录接口 POST /login
// 解析用户名密码 → MySQL 查询验证 → 创建 Session → setValue 写入 userId/isLoggedIn
void ChatLoginHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp)
{
    
    auto contentType = req.getHeader("Content-Type");
    if (contentType.empty() || contentType != "application/json" || req.getBody().empty())
    {
        LOG_INFO << "content" << req.getBody();
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
        resp->setCloseConnection(true);
        resp->setContentType("application/json");
        resp->setContentLength(0);
        resp->setBody("");
        return;
    }


    try
    {
        json parsed = json::parse(req.getBody());
        std::string username = parsed["username"];
        std::string password = parsed["password"];

        int userId = queryUserId(username, password);
        if (userId != -1)
        {

            auto session = server_->getSessionManager()->getSession(req, resp);


            session->setValue("userId", std::to_string(userId));
            session->setValue("username", username);
            session->setValue("isLoggedIn", "true");
            // 功能权限：登录时查一次写入会话，后续请求按功能鉴权
            //（查不到 DB 异常时按最保守处理：全部置 false）
            {
                bool canChat = false, canImage = false, canTts = false;
                UserAuthDao::GetUserPermissions(userId, canChat, canImage, canTts);
                session->setValue("canChat",  canChat  ? "true" : "false");
                session->setValue("canImage", canImage ? "true" : "false");
                session->setValue("canTts",   canTts   ? "true" : "false");
            }
            // 原实现是"锁外判断是否在线 → 锁内标记在线"，两步之间有空窗：
            // 同一账号并发登录时，两个线程都读到"不在线"，于是双双登录成功，
            // 防重复登录形同虚设。现在把判断和标记收进访问器的同一个写锁里，
            // 保证只有一个线程能抢到上线名额。
            if (server_->tryMarkOnline(userId))
            {
                json successResp;
                successResp["success"] = true;
                successResp["userId"] = userId;
                std::string successBody = successResp.dump(4);

                resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
                resp->setCloseConnection(false);
                resp->setContentType("application/json");
                resp->setContentLength(successBody.size());
                resp->setBody(successBody);
                return;
            }
            else
            {

                json failureResp;
                failureResp["success"] = false;
                failureResp["error"] = "该账号已在别处登录";
                std::string failureBody = failureResp.dump(4);

                resp->setStatusLine(req.getVersion(), http::HttpResponse::k403Forbidden, "Forbidden");
                resp->setCloseConnection(true);
                resp->setContentType("application/json");
                resp->setContentLength(failureBody.size());
                resp->setBody(failureBody);
                return;
            }
        }
        else 
        {
            json failureResp;
            failureResp["status"] = "error";
            failureResp["message"] = "Invalid username or password";
            std::string failureBody = failureResp.dump(4);

            resp->setStatusLine(req.getVersion(), http::HttpResponse::k401Unauthorized, "Unauthorized");
            resp->setCloseConnection(false);
            resp->setContentType("application/json");
            resp->setContentLength(failureBody.size());
            resp->setBody(failureBody);
            return;
        }
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
        return;
    }

}

int ChatLoginHandler::queryUserId(const std::string& username, const std::string& password)
{

    std::string sql = "SELECT id FROM users WHERE username = ? AND password = ?";
    // std::vector<std::string> params = {username, password};
    auto res = mysqlUtil_.executeQuery(sql, username, password);
    if (res->next())
    {
        int id = res->getInt("id");
        return id;
    }

    return -1;
}

