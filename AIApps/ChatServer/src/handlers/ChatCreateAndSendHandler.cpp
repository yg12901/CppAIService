#include "../include/handlers/ChatCreateAndSendHandler.h"


// 新建会话并发送 POST /chat/send-new-session
// 使用 AISessionIdGenerator 生成唯一 sessionId，创建新 AIHelper 并返回 sessionId 给前端
void ChatCreateAndSendHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp)
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
        bool stream = true;   // 默认请求流式（打字机效果），前端可显式传 false

        auto body = req.getBody();
        if (!body.empty()) {
            auto j = json::parse(body);
            if (j.contains("question")) userQuestion = j["question"];


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

        AISessionIdGenerator generator;
        std::string sessionId = generator.generate();
        std::cout<<"ɵsessionIdΪ "<<sessionId<<std::endl;


        std::shared_ptr<AIHelper> AIHelperPtr;
        {
            std::lock_guard<std::mutex> lock(server_->mutexForChatInformation);

            auto& userSessions = server_->chatInformation[userId];

            if (userSessions.find(sessionId) == userSessions.end()) {

                userSessions.emplace( 
                    sessionId,
                    std::make_shared<AIHelper>()
                );
                server_->sessionsIdsMap[userId].push_back(sessionId);
            }
            AIHelperPtr= userSessions[sessionId];

        }

        // 流式响应模式：请求要求流式且模型支持（RAG 等自动降级非流式）
        // 新会话先以首条 SSE 事件回传 sessionId，前端据此保存会话
        if (stream && AIHelperPtr->isStreamSupported()) {
            resp->setStreaming(true);
            resp->setCloseConnection(true);

            // 1) SSE 响应头
            resp->sendChunk(
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: close\r\n"
                "\r\n");

            // 2) 首条事件回传 sessionId（前端创建会话后才开始拼接增量）
            json sidEv;
            sidEv["sessionId"] = sessionId;
            resp->sendChunk("data: " + sidEv.dump() + "\n\n");

            // 3) 增量回调：delta 转一条 SSE 事件
            AIHelperPtr->setStreamCallback([resp](const std::string& delta) {
                json ev;
                ev["delta"] = delta;
                resp->sendChunk("data: " + ev.dump() + "\n\n");
            });

            // 4) 执行对话；异常兜底发 error 事件保证前端能收尾
            try {
                AIHelperPtr->chat(userId, username, sessionId, userQuestion, modelType, true);
            } catch (const std::exception& e) {
                json ev;
                ev["delta"] = std::string("[流式异常] ") + e.what();
                resp->sendChunk("data: " + ev.dump() + "\n\n");
            }

            // 5) 结束标记
            resp->sendChunk("data: [DONE]\n\n");
            return;
        }

        std::string aiInformation=AIHelperPtr->chat(userId, username,sessionId, userQuestion, modelType);
        json successResp;
        successResp["success"] = true;
        successResp["Information"] = aiInformation;
        successResp["sessionId"] = sessionId;
        
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









