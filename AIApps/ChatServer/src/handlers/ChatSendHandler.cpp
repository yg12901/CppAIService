#include "../include/handlers/ChatSendHandler.h"


// 核心聊天接口 POST /chat/send
// 鉴权后从 chatInformation 二级 map 查找或创建会话 AIHelper，调用 chat 方法获取 AI 回复
void ChatSendHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp)
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

        std::string userQuestion;
        std::string modelType;
        std::string sessionId;
        bool stream = true;   // 默认请求流式（打字机效果），前端可显式传 false

        auto body = req.getBody();
        if (!body.empty()) {
            auto j = json::parse(body);
            if (j.contains("question")) userQuestion = j["question"];
            if (j.contains("sessionId")) sessionId = j["sessionId"];

            modelType = j.contains("modelType") ? j["modelType"].get<std::string>() : "1";
            if (j.contains("stream")) stream = j["stream"].get<bool>();
        }

        // 功能权限校验：canChat=false 的用户禁止使用 AI 对话（fail-closed，未登录刷新前一律拒绝）
        if (session->getValue("canChat") != "true") {
            json errorResp;
            errorResp["status"] = "error";
            errorResp["message"] = "chat not permitted for your account";
            std::string errorBody = errorResp.dump(4);

            server_->packageResp(req.getVersion(), http::HttpResponse::k403Forbidden,
                "Forbidden", true, "application/json", errorBody.size(),
                errorBody, resp);
            return;
        }


        std::shared_ptr<AIHelper> AIHelperPtr;
        {
            std::lock_guard<std::mutex> lock(server_->mutexForChatInformation);

            auto& userSessions = server_->chatInformation[userId];

            if (userSessions.find(sessionId) == userSessions.end()) {

                userSessions.emplace( 
                    sessionId,
                    std::make_shared<AIHelper>()
                );
            }
            AIHelperPtr= userSessions[sessionId];
        }
        

        // 流式响应模式：请求要求流式且模型支持（RAG 等自动降级非流式）
        // 登录/权限校验已在上方完成（401/403 走标准 JSON 错误响应）
        if (stream && AIHelperPtr->isStreamSupported()) {
            // 声明流式：框架跳过统一序列化，由本 Handler 直写连接，结束后关闭连接
            resp->setStreaming(true);
            resp->setCloseConnection(true);

            // 1) 一次性写 SSE 响应头（不带 Content-Length，连接关闭即流结束）
            resp->sendChunk(
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: close\r\n"
                "\r\n");

            // 2) 增量回调：每个 delta 转一条 SSE 事件（dump() 自动转义 JSON 特殊字符）
            AIHelperPtr->setStreamCallback([resp](const std::string& delta) {
                json ev;
                ev["delta"] = delta;
                resp->sendChunk("data: " + ev.dump() + "\n\n");
            });

            // 3) 执行对话（内部边收 LLM 增量边转发）；异常兜底发 error 事件保证前端能收尾
            try {
                AIHelperPtr->chat(userId, username, sessionId, userQuestion, modelType, true);
            } catch (const std::exception& e) {
                json ev;
                ev["delta"] = std::string("[流式异常] ") + e.what();
                resp->sendChunk("data: " + ev.dump() + "\n\n");
            }

            // 4) 结束标记（chat 结束时已自动清空回调，防止悬挂）
            resp->sendChunk("data: [DONE]\n\n");
            return;
        }

        std::string aiInformation=AIHelperPtr->chat(userId, username,sessionId, userQuestion, modelType);
        json successResp;
        successResp["success"] = true;
        successResp["Information"] = aiInformation;
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









