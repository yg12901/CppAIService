#include <string>
#include <iostream>
#include <thread>
#include <chrono>
#include <muduo/net/TcpServer.h>
#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>

#include"../include/ChatServer.h"

const std::string RABBITMQ_HOST = "localhost";
const std::string QUEUE_NAME = "sql_queue";
const int THREAD_NUM = 2;

// RabbitMQ 消费回调：把队列里的一条消息落进 MySQL。
// 消息体是 AIHelper::pushMessageToMysql 投递的结构化 JSON，
// SQL 语句写死在这里，用户数据只通过预处理语句的占位符进入，杜绝注入。
void executeMysql(const std::string msg) {
    http::MysqlUtil mysqlUtil_;

    json payload = json::parse(msg, nullptr, false);   // 不抛异常，失败返回 discarded
    if (payload.is_discarded() || !payload.is_object()) {
        // 兼容旧版本：升级期间队列里可能还堆着上一版投递的裸 SQL 字符串。
        // 等积压消费干净后这个分支就可以删掉。
        LOG_WARN << "legacy raw-sql message in queue, executing as-is";
        mysqlUtil_.executeUpdate(msg);
        return;
    }

    const std::string type = payload.value("type", std::string("chat_message"));

    try {
        if (type == "image_result") {
            static const std::string kInsertImageResult =
                "INSERT INTO image_result "
                "(user_id, username, filename, class_name, class_id, confidence, model, ts) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?)";

            const std::string id          = std::to_string(payload.value("id", 0));
            const std::string username    = payload.value("username", std::string());
            const std::string filename    = payload.value("filename", std::string());
            const std::string className   = payload.value("class_name", std::string());
            const std::string classId     = std::to_string(payload.value("class_id", -1));
            const std::string confidence  = std::to_string(payload.value("confidence", 0.0f));
            const std::string model       = payload.value("model", std::string());
            const std::string ts          = std::to_string(payload.value("ts", 0LL));

            mysqlUtil_.executeUpdate(kInsertImageResult, id, username, filename,
                className, classId, confidence, model, ts);
            return;
        }

        static const std::string kInsertChatMessage =
            "INSERT INTO chat_message "
            "(id, username, session_id, is_user, content, ts, model, prompt_tokens, completion_tokens) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)";

        const std::string id                = std::to_string(payload.value("id", 0));
        const std::string username          = payload.value("username", std::string());
        const std::string sessionId         = [&payload] {
            std::string s = payload.value("session_id", std::string("0"));
            return s.empty() ? std::string("0") : s;
        }();
        const std::string isUser            = std::to_string(payload.value("is_user", 1));
        const std::string content           = payload.value("content", std::string());
        const std::string ts                = std::to_string(payload.value("ts", 0LL));
        const std::string model             = payload.value("model", std::string());
        const std::string promptTokens      = std::to_string(payload.value("prompt_tokens", 0));
        const std::string completionTokens  = std::to_string(payload.value("completion_tokens", 0));

        mysqlUtil_.executeUpdate(kInsertChatMessage, id, username, sessionId,
            isUser, content, ts, model, promptTokens, completionTokens);
    }
    catch (const std::exception& e) {
        LOG_ERROR << "insert " << type << " failed: " << e.what();
    }
}


// 程序入口
// 创建 ChatServer → 4 工作线程 → 从 MySQL 恢复历史 → 启动 RabbitMQ 线程池 → 进入事件循环
int main(int argc, char* argv[]) {
	LOG_INFO << "pid = " << getpid();
	std::string serverName = "ChatServer";
	int port = 80;
    // 
    int opt;
    const char* str = "p:";
    while ((opt = getopt(argc, argv, str)) != -1)
    {
        switch (opt)
        {
        case 'p':
        {
            port = atoi(optarg);
            break;
        }
        default:
            break;
        }
    }
    muduo::Logger::setLogLevel(muduo::Logger::WARN);
    ChatServer server(port, serverName);
    server.setThreadNum(4);
    
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    server.initChatMessage();    


    RabbitMQThreadPool pool(RABBITMQ_HOST, QUEUE_NAME, THREAD_NUM, executeMysql);
    pool.start();

    server.start();
}
