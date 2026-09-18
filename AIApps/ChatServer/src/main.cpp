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

    static const std::string kInsertChatMessage =
        "INSERT INTO chat_message "
        "(id, username, session_id, is_user, content, ts, model, prompt_tokens, completion_tokens) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)";

    // 统一以字符串绑定：DbConnection 的参数绑定内部本来就是 setString，
    // 数值列由 MySQL 侧做隐式转换，行为和改造前一致。
    // 必须声明成 const，否则会匹配到按 to_string 处理的那个泛型重载导致编译失败。
    const std::string id                = std::to_string(payload.value("id", 0));
    const std::string username          = payload.value("username", std::string());
    // 空 sessionId 在旧实现里会拼出 "VALUES (1, 'x', , 0, ...)" 这种语法错误的 SQL，
    // 这里统一兜成 "0"。注意必须落成具名 const 变量：
    // 直接把三元表达式的临时对象传进去会绑到泛型重载的 T&&，编译不过。
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

    try {
        mysqlUtil_.executeUpdate(kInsertChatMessage, id, username, sessionId,
            isUser, content, ts, model, promptTokens, completionTokens);
    }
    catch (const std::exception& e) {
        // 消费线程里抛出去会打死 worker，这里兜住并落日志。
        // 注意：当前实现是先 handler 后 BasicAck，抛异常会导致消息不被 ack 而重投，
        // 吞掉异常等于放弃这条消息——用可观测性换消费链路不被单条脏数据卡死。
        LOG_ERROR << "insert chat_message failed: " << e.what();
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
