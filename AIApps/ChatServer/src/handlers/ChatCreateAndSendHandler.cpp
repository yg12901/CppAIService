#include "../include/handlers/ChatCreateAndSendHandler.h"
#include "../include/AIUtil/SseChannel.h"
#include "../include/AIUtil/SseKeepalive.h"
#include "../include/AIUtil/StreamWorkerPool.h"


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


        // 原实现在这里犯了两个错：
        //  ① sessionsIdsMap 是在 mutexForChatInformation 的保护下写的，
        //     而读它的 /chat/sessions 用的却是 mutexForSessionsId —— 两把不同的锁
        //     保护同一份数据，等于没保护，是实打实的数据竞争。
        //  ② 想顺手合并成一次加锁，反而埋下了跨表嵌套持锁的隐患。
        // 现在拆成两段串行加锁：各自锁各自的表，全程不嵌套，既修了竞争也免了死锁。
        bool created = false;
        std::shared_ptr<AIHelper> AIHelperPtr =
            server_->getOrCreateChatHelper(userId, sessionId, &created);
        if (created) {
            server_->appendSessionId(userId, sessionId);
        }

        // 流式响应模式：请求要求流式且模型支持（RAG 等自动降级非流式）
        // 新会话先以首条 SSE 事件回传 sessionId，前端据此保存会话
        if (stream && AIHelperPtr->isStreamSupported()) {
            auto conn = resp->connection();
            if (!conn) {
                // 无底层连接（异常场景）：退回非流式路径
                std::string aiInformation = AIHelperPtr->chat(userId, username, sessionId, userQuestion, modelType);
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

            // 1) 长生命周期发送通道 + 心跳注册
            auto channel = std::make_shared<SseChannel>(conn);
            SseKeepalive::instance().registerFlow(channel);

            // 2) IO 线程写 SSE 响应头 + 首条事件回传 sessionId
            channel->sendRaw(
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: close\r\n"
                "\r\n");
            json sidEv;
            sidEv["sessionId"] = sessionId;
            channel->sendEvent(sidEv.dump());

            // 3) 打包流式任务丢给 worker 池，IO 线程立即返回
            auto helper = AIHelperPtr;
            StreamWorkerPool::instance().submit(
                [helper, channel, userId, username, sessionId, userQuestion, modelType]() {
                    helper->setStreamCallback([helper, channel](const std::string& delta) {
                        json ev;
                        ev["delta"] = delta;
                        if (!channel->sendEvent(ev.dump())) {
                            helper->requestAbort();   // 客户端断开 → 中止 LLM 流
                        }
                    });
                    try {
                        helper->chat(userId, username, sessionId, userQuestion, modelType, true);
                    } catch (const std::exception& e) {
                        if (channel->alive()) {
                            json ev;
                            ev["delta"] = std::string("[流式异常] ") + e.what();
                            channel->sendEvent(ev.dump());
                        }
                    }
                    channel->sendRaw("data: [DONE]\n\n");
                    SseKeepalive::instance().unregisterFlow(channel->id());
                    channel->close();
                });

            // 4) 流式声明：框架跳过统一序列化；收尾由 worker 负责
            resp->setStreaming(true);
            resp->setCloseConnection(false);
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









