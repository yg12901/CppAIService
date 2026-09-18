#include "../include/handlers/ChatSendHandler.h"
#include "../include/AIUtil/SseChannel.h"
#include "../include/AIUtil/SseKeepalive.h"
#include "../include/AIUtil/StreamWorkerPool.h"


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


        // 继续已有会话是常态，会在访问器的 shared_lock 快路径直接命中，多请求并行。
        // 只有"会话的第一条消息"才会走到 unique_lock。
        std::shared_ptr<AIHelper> AIHelperPtr = server_->getOrCreateChatHelper(userId, sessionId);
        

        // 流式响应模式：请求要求流式且模型支持（RAG 等自动降级非流式）
        // 登录/权限校验已在上方完成（401/403 走标准 JSON 错误响应）
        if (stream && AIHelperPtr->isStreamSupported()) {
            auto conn = resp->connection();
            if (!conn) {
                // 无底层连接（异常场景）：退回非流式路径
                std::string aiInformation = AIHelperPtr->chat(userId, username, sessionId, userQuestion, modelType);
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

            // 1) 构造长生命周期发送通道（Handler 返回后 resp 栈对象析构，
            //    worker 只能持有 SseChannel，不能持有 resp）
            auto channel = std::make_shared<SseChannel>(conn);
            SseKeepalive::instance().registerFlow(channel);

            // 2) IO 线程一次性写 SSE 响应头
            channel->sendRaw(
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: close\r\n"
                "\r\n");

            // 3) 打包流式任务丢给 worker 池，IO 线程立即返回（不占 Muduo 线程）
            auto helper = AIHelperPtr;   // shared_ptr 拷贝进 lambda，生命周期安全
            StreamWorkerPool::instance().submit(
                [helper, channel, userId, username, sessionId, userQuestion, modelType]() {
                    // 增量回调：delta 转 SSE 事件；客户端断开时 requestAbort
                    // 中止上游 LLM 流（WriteCallback 返回 0，不再浪费 token）
                    helper->setStreamCallback([helper, channel](const std::string& delta) {
                        json ev;
                        ev["delta"] = delta;
                        if (!channel->sendEvent(ev.dump())) {
                            helper->requestAbort();
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
                    // 4) 收尾：[DONE] + 注销心跳 + 优雅关连接（排空缓冲后断开）
                    channel->sendRaw("data: [DONE]\n\n");
                    SseKeepalive::instance().unregisterFlow(channel->id());
                    channel->close();
                });

            // 5) 声明流式模式：框架跳过统一序列化；连接收尾由 worker 在
            //    DONE 后 close()，故这里必须保持 closeConnection=false
            resp->setStreaming(true);
            resp->setCloseConnection(false);
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









