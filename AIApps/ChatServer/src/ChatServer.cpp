#include "../include/handlers/ChatLoginHandler.h"
#include "../include/handlers/ChatRegisterHandler.h"
#include "../include/handlers/ChatLogoutHandler.h"
#include"../include/handlers/ChatHandler.h"
#include"../include/handlers/ChatEntryHandler.h"
#include"../include/handlers/ChatSendHandler.h"
#include"../include/handlers/AIMenuHandler.h"
#include"../include/handlers/AIUploadSendHandler.h"
#include"../include/handlers/AIUploadHandler.h"
#include"../include/handlers/KbUploadHandler.h"
#include"../include/handlers/KbUploadSendHandler.h"
#include"../include/handlers/KbJobStatusHandler.h"
#include"../include/handlers/ChatHistoryHandler.h"


#include"../include/handlers/ChatCreateAndSendHandler.h"
#include"../include/handlers/ChatSessionsHandler.h"
#include"../include/handlers/ChatSpeechHandler.h"

#include"../include/AIUtil/StreamWorkerPool.h"
#include"../include/AIUtil/SseKeepalive.h"

#include "../include/ChatServer.h"
#include "../../../HttpServer/include/http/HttpRequest.h"
#include "../../../HttpServer/include/http/HttpResponse.h"
#include "../../../HttpServer/include/http/HttpServer.h"
#include <chrono>



using namespace http;


ChatServer::ChatServer(int port,
    const std::string& name,
    muduo::net::TcpServer::Option option)
    : httpServer_(port, name, option)
{
    initialize();
}

void ChatServer::initialize() {
    std::cout << "ChatServer initialize start  ! " << std::endl;
	http::MysqlUtil::init("tcp://127.0.0.1:3306", "root", "123456", "ChatHttpServer", 5);
	ensureImageResultTable();

    initializeSession();

    initializeMiddleware();

    initializeRouter();

    // SSE 流式基建：
    // 1) 工作线程池——流式任务从 Muduo IO 线程剥离，IO 线程只做"写头+提交任务"
    //    立即返回（原先一条流占死一个 IO 线程，4 条流就拖垮全部连接）
    StreamWorkerPool::instance().start(8);

    // 2) 心跳保活——主 loop 每 5s 扫描活跃流，空闲 >10s 补发 ": ping" 注释行，
    //    防止 TTFT 期间（LLM 思考/MCP 工具执行）被 nginx/SLB 等中间代理掐断
    httpServer_.getLoop()->runEvery(5.0, []() {
        SseKeepalive::instance().onTimer();
    });
}

void ChatServer::initChatMessage() {

    std::cout << "initChatMessage start ! " << std::endl;
    readDataFromMySQL();
    std::cout << "initChatMessage success ! " << std::endl;
}

namespace {

// ONNX 模型路径：原先硬编码在 AIUploadSendHandler 里（带 todo 注释），
// 换机器就得改代码重编。这里收口成一处，并允许用环境变量覆盖。
const std::string& imageModelPath() {
    static const std::string path = [] {
        const char* p = std::getenv("IMAGE_MODEL_PATH");
        return (p && *p) ? std::string(p)
                         : std::string("/root/models/mobilenetv2/mobilenetv2-7.onnx");
    }();
    return path;
}

} // namespace

// ================= 共享状态访问器 =================
// 读路径：只查不建。注意用 find 而不是 operator[]——
// operator[] 在 key 不存在时会插入默认值，那是写操作，在 shared_lock 下是未定义行为。
std::shared_ptr<AIHelper> ChatServer::findChatHelper(int userId,
    const std::string& sessionId) const
{
    std::shared_lock<std::shared_mutex> lock(mutexForChatInformation);
    auto itUser = chatInformation.find(userId);
    if (itUser == chatInformation.end()) return nullptr;
    auto itSession = itUser->second.find(sessionId);
    if (itSession == itUser->second.end()) return nullptr;
    return itSession->second;
}

// 写路径：双重检查。/chat/send 的绝大多数请求都是"往已有会话里继续发"，
// 会在第一段 shared_lock 就命中返回，真正需要独占写锁的只有会话的第一条消息。
std::shared_ptr<AIHelper> ChatServer::getOrCreateChatHelper(int userId,
    const std::string& sessionId, bool* created)
{
    if (created) *created = false;

    // 第一次检查：共享锁，多线程可同时进来
    {
        std::shared_lock<std::shared_mutex> lock(mutexForChatInformation);
        auto itUser = chatInformation.find(userId);
        if (itUser != chatInformation.end()) {
            auto itSession = itUser->second.find(sessionId);
            if (itSession != itUser->second.end()) return itSession->second;
        }
    }

    // 第二次检查：升级为独占锁后必须重查一遍。
    // 因为释放共享锁到拿到独占锁之间存在空窗，别的线程可能已经把它建好了，
    // 少了这次重查就会出现"两个线程各建一个 AIHelper，后者覆盖前者"，
    // 表现为用户上下文凭空丢失。
    std::unique_lock<std::shared_mutex> lock(mutexForChatInformation);
    auto& userSessions = chatInformation[userId];
    auto itSession = userSessions.find(sessionId);
    if (itSession != userSessions.end()) return itSession->second;

    auto helper = std::make_shared<AIHelper>();
    userSessions.emplace(sessionId, helper);
    if (created) *created = true;
    return helper;
}

// 图像识别器：和上面同构，但多一个关键处理——模型加载放在锁外。
std::shared_ptr<ImageRecognizer> ChatServer::getOrCreateRecognizer(int userId)
{
    {
        std::shared_lock<std::shared_mutex> lock(mutexForImageRecognizerMap);
        auto it = ImageRecognizerMap.find(userId);
        if (it != ImageRecognizerMap.end()) return it->second;
    }

    // 构造函数要读 ONNX 权重 + 建推理 session，是百毫秒级的重操作。
    // 如果放在写锁里做，这段时间所有用户的图像请求全被挡住，
    // 等于把"首次加载"的代价摊给了全服。所以先在锁外造好。
    auto fresh = std::make_shared<ImageRecognizer>(imageModelPath());

    std::unique_lock<std::shared_mutex> lock(mutexForImageRecognizerMap);
    // 代价是可能白造一个：两个线程同时首次上传时都会各造一份。
    // emplace 只有先到者成功，后到者拿到已存在的那个，自己造的 fresh 随即析构。
    // 用"偶尔多造一份"换"不阻塞全服"，这笔买卖划算。
    auto result = ImageRecognizerMap.emplace(userId, fresh);
    return result.first->second;
}

void ChatServer::ensureImageResultTable() {
    // 启动时建表：手动跑 init_v3.sql 也可以，这里兜底避免忘了执行导致消费端一直 INSERT 失败。
    static const std::string kDdl =
        "CREATE TABLE IF NOT EXISTS image_result ("
        "  pk BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        "  user_id INT NOT NULL,"
        "  username VARCHAR(50) NOT NULL DEFAULT '',"
        "  filename VARCHAR(255) NOT NULL DEFAULT '',"
        "  class_name VARCHAR(256) NOT NULL DEFAULT '',"
        "  class_id INT NOT NULL DEFAULT -1,"
        "  confidence FLOAT NOT NULL DEFAULT 0,"
        "  model VARCHAR(64) NOT NULL DEFAULT '',"
        "  ts BIGINT NOT NULL,"
        "  KEY idx_image_result_user_ts (user_id, ts)"
        ")";
    try {
        mysqlUtil_.executeUpdate(kDdl);
    }
    catch (const std::exception& e) {
        std::cerr << "ensureImageResultTable failed: " << e.what() << std::endl;
    }
}

void ChatServer::pushImageResult(int userId, const std::string& username,
    const std::string& filename, const ImagePrediction& prediction)
{
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    json payload;
    payload["type"]       = "image_result";
    payload["id"]         = userId;
    payload["username"]   = username;
    payload["filename"]   = filename;
    payload["class_name"] = prediction.label;
    payload["class_id"]   = prediction.classId;
    payload["confidence"] = prediction.confidence;
    payload["model"]      = "mobilenetv2";
    payload["ts"]         = ms;

    MQManager::instance().publish("sql_queue", payload.dump());
}

void ChatServer::appendSessionId(int userId, const std::string& sessionId)
{
    std::unique_lock<std::shared_mutex> lock(mutexForSessionsId);
    sessionsIdsMap[userId].push_back(sessionId);
}

std::vector<std::string> ChatServer::listSessionIds(int userId) const
{
    std::shared_lock<std::shared_mutex> lock(mutexForSessionsId);
    auto it = sessionsIdsMap.find(userId);
    if (it == sessionsIdsMap.end()) return {};
    return it->second;   // 拷贝一份出去，调用方拿去拼 JSON 时不再持锁
}

// 登录抢占：判断"是否已在线"和"标记为在线"必须是一个原子步骤。
// 原实现先在锁外 find 判断、再进锁写入，两步之间有空窗：
// 同一账号并发登录时两个线程都会判定"不在线"，双双登录成功，防重复登录形同虚设。
bool ChatServer::tryMarkOnline(int userId)
{
    std::unique_lock<std::shared_mutex> lock(mutexForOnlineUsers_);
    auto it = onlineUsers_.find(userId);
    if (it != onlineUsers_.end() && it->second) return false;   // 已在线
    onlineUsers_[userId] = true;
    return true;
}

void ChatServer::markOffline(int userId)
{
    std::unique_lock<std::shared_mutex> lock(mutexForOnlineUsers_);
    onlineUsers_.erase(userId);
}

// 从 MySQL 恢复历史聊天记录
// 按 ts 时间戳升序读取 chat_message 表，重建 chatInformation 二级 map 与 AIHelper
void ChatServer::readDataFromMySQL() {


    std::string sql = "SELECT id, username,session_id, is_user, content, ts FROM chat_message ORDER BY ts ASC, id ASC";

    sql::ResultSet* res;
    try {
        res = mysqlUtil_.executeQuery(sql);
    }
    catch (const std::exception& e) {
        std::cerr << "MySQL query failed: " << e.what() << std::endl;
        return;
    }

    while (res->next()) {
        long long user_id = 0;
        std::string session_id ;  
        std::string username, content;
        long long ts = 0;
        int is_user = 1;

        try {
            user_id    = res->getInt64("id");       
            session_id = res->getString("session_id");  
            username   = res->getString("username");
            content    = res->getString("content");
            ts         = res->getInt64("ts");
            is_user    = res->getInt("is_user");
        }
        catch (const std::exception& e) {
            std::cerr << "Failed to read row: " << e.what() << std::endl;
            continue; 
        }

        // 走统一访问器：启动期虽是单线程，但让恢复路径和运行期共用同一套加锁语义，
        // 避免以后有人在这里直接动裸 map 而绕过锁。
        bool created = false;
        auto helper = getOrCreateChatHelper(static_cast<int>(user_id), session_id, &created);
        if (created) {
            appendSessionId(static_cast<int>(user_id), session_id);
        }

        helper->restoreMessage(content, ts, is_user != 0);
    }

    std::cout << "readDataFromMySQL finished" << std::endl;
}



void ChatServer::setThreadNum(int numThreads) {
    httpServer_.setThreadNum(numThreads);
}


void ChatServer::start() {
    httpServer_.start();
}


void ChatServer::initializeRouter() {

    httpServer_.Get("/", std::make_shared<ChatEntryHandler>(this));
    httpServer_.Get("/entry", std::make_shared<ChatEntryHandler>(this));
    
    httpServer_.Post("/login", std::make_shared<ChatLoginHandler>(this));
    
    httpServer_.Post("/register", std::make_shared<ChatRegisterHandler>(this));
    
    httpServer_.Post("/user/logout", std::make_shared<ChatLogoutHandler>(this));

    httpServer_.Get("/chat", std::make_shared<ChatHandler>(this));

    httpServer_.Post("/chat/send", std::make_shared<ChatSendHandler>(this));
 
    httpServer_.Get("/menu", std::make_shared<AIMenuHandler>(this));
    
    httpServer_.Get("/upload", std::make_shared<AIUploadHandler>(this));
   
    httpServer_.Post("/upload/send", std::make_shared<AIUploadSendHandler>(this));

    httpServer_.Get("/kb", std::make_shared<KbUploadHandler>(this));
    httpServer_.Post("/kb/upload", std::make_shared<KbUploadSendHandler>(this));
    httpServer_.Post("/kb/job-status", std::make_shared<KbJobStatusHandler>(this));
    
    httpServer_.Post("/chat/history", std::make_shared<ChatHistoryHandler>(this));

    
    httpServer_.Post("/chat/send-new-session", std::make_shared<ChatCreateAndSendHandler>(this));
    httpServer_.Get("/chat/sessions", std::make_shared<ChatSessionsHandler>(this));

    httpServer_.Post("/chat/tts", std::make_shared<ChatSpeechHandler>(this));
}

void ChatServer::initializeSession() {

    auto sessionStorage = std::make_unique<http::session::MemorySessionStorage>();

    auto sessionManager = std::make_unique<http::session::SessionManager>(std::move(sessionStorage));

    setSessionManager(std::move(sessionManager));
}

void ChatServer::initializeMiddleware() {

    auto corsMiddleware = std::make_shared<http::middleware::CorsMiddleware>();

    httpServer_.addMiddleware(corsMiddleware);
}


void ChatServer::packageResp(const std::string& version,
    http::HttpResponse::HttpStatusCode statusCode,
    const std::string& statusMsg,
    bool close,
    const std::string& contentType,
    int contentLen,
    const std::string& body,
    http::HttpResponse* resp)
{
    if (resp == nullptr)
    {
        LOG_ERROR << "Response pointer is null";
        return;
    }

    try
    {
        resp->setVersion(version);
        resp->setStatusCode(statusCode);
        resp->setStatusMessage(statusMsg);
        resp->setCloseConnection(close);
        resp->setContentType(contentType);
        resp->setContentLength(contentLen);
        resp->setBody(body);

        LOG_INFO << "Response packaged successfully";
    }
    catch (const std::exception& e)
    {
        LOG_ERROR << "Error in packageResp: " << e.what();

        resp->setStatusCode(http::HttpResponse::k500InternalServerError);
        resp->setStatusMessage("Internal Server Error");
        resp->setCloseConnection(true);
    }
}
