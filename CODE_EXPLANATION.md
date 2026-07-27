# CppAIService 项目完整代码解析

> 本文档对 `/data/workspace/CppAIService` 项目的所有源文件进行逐行解析，包含原理说明、关键代码注释以及用法示例。

---

## 目录

- [一、项目概览](#一项目概览)
- [二、项目文件结构](#二项目文件结构)
- [三、构建配置文件](#三构建配置文件)
  - [CMakeLists.txt](#cmakeliststxt)
- [四、入口点：main.cpp](#四入口点maincpp)
- [五、ChatServer 核心类](#五chatserver-核心类)
  - [ChatServer.h](#chatserverh)
  - [ChatServer.cpp](#chatservercpp)
- [六、AI 策略模块（Strategy + Factory）](#六ai-策略模块strategy--factory)
  - [AIStrategy.h](#aistrategyh)
  - [AIStrategy.cpp](#aistrategycpp)
  - [AIFactory.h](#aifactoryh)
  - [AIFactory.cpp](#aifactorycpp)
- [七、AI 对话助手](#七ai-对话助手)
  - [AIHelper.h](#aihelperh)
  - [AIHelper.cpp](#aihelpercpp)
- [八、MCP 工具协议](#八mcp-工具协议)
  - [AIConfig.h / AIConfig.cpp](#aiconfigh--aiconfigcpp)
  - [AIToolRegistry.h / AIToolRegistry.cpp](#aitoolregistryh--aitoolregistrycpp)
- [九、语音处理模块](#九语音处理模块)
  - [AISpeechProcessor.h / AISpeechProcessor.cpp](#aispeechprocessorh--aispeechprocessorcpp)
- [十、图像识别模块](#十图像识别模块)
  - [ImageRecognizer.h / ImageRecognizer.cpp](#imagerecognizerh--imagerecognizercpp)
- [十一、消息队列模块](#十一消息队列模块)
  - [MQManager.h / MQManager.cpp](#mqmanagerh--mqmanagercpp)
- [十二、HTTP 请求处理器（Handlers）](#十二http-请求处理器handlers)
  - [处理器模式概述](#处理器模式概述)
  - [ChatEntryHandler](#chatentryhandler)
  - [ChatLoginHandler](#chatloginhandler)
  - [ChatRegisterHandler](#chatregisterhandler)
  - [ChatLogoutHandler](#chatlogouthandler)
  - [ChatHandler](#chathandler)
  - [ChatSendHandler](#chatsendhandler)
  - [ChatHistoryHandler](#chathistoryhandler)
  - [ChatCreateAndSendHandler](#chatcreateandsendhandler)
  - [ChatSessionsHandler](#chatsessionshandler)
  - [ChatSpeechHandler](#chatspeechhandler)
  - [AIMenuHandler](#aimenuhandler)
  - [AIUploadHandler](#aiuploadhandler)
  - [AIUploadSendHandler](#aiuploadsendhandler)
- [十三、工具类](#十三工具类)
  - [AISessionIdGenerator](#aisessionidgenerator)
  - [base64](#base64)
- [十四、HttpServer 框架部分](#十四httpserver-框架部分)
- [十五、完整请求流程](#十五完整请求流程)
- [十六、部署与运行](#十六部署与运行)

---

## 一、项目概览

**CppAIService** 是一个**纯 C++ 构建的 AI 应用服务平台（第二版）**。它基于自研的 C++ HTTP 网络框架，整合了以下能力：

| 能力 | 说明 |
|------|------|
| **多模型对话** | 支持阿里百炼（通义千问）、豆包（火山引擎）、百炼RAG、百炼MCP |
| **RAG 检索增强** | 配置化知识库接入，自动检索后回答 |
| **轻量级 MCP** | 配置化工具注册 + 两段式推理（模型判断 → 工具调用 → 二次回答） |
| **图像识别** | ONNX Runtime + OpenCV DNN 推理 |
| **语音合成（TTS）** | 百度 TTS API（创建任务 → 轮询 → 回传 URL） |
| **异步消息队列** | RabbitMQ 承载持久化写库，同步写内存、异步入库 |
| **多会话管理** | 单用户多会话隔离，`unordered_map<userId, map<sessionId, AIHelper>>` |

---

## 二、项目文件结构

```
CppAIService/
├── CMakeLists.txt                          # 主构建配置
├── README.md                               # 项目说明
├── HttpServer/                             # 自研 HTTP 框架（见第十四章）
│   ├── include/http/
│   │   ├── HttpContext.h                   # HTTP 报文解析器
│   │   ├── HttpRequest.h                   # HTTP 请求对象
│   │   ├── HttpResponse.h                  # HTTP 响应对象
│   │   └── HttpServer.h                    # HTTP 服务器
│   ├── include/router/
│   │   ├── Router.h                        # 路由模块
│   │   └── RouterHandler.h                 # 处理器接口
│   ├── include/middleware/
│   │   ├── Middleware.h / MiddlewareChain.h # 中间件
│   │   └── cors/CorsMiddleware.h           # CORS 跨域
│   ├── include/session/
│   │   └── Session.h / SessionManager.h    # 会话管理
│   ├── include/utils/
│   │   ├── FileUtil.h                      # 文件工具
│   │   ├── JsonUtil.h                      # JSON 工具（nlohmann/json）
│   │   ├── MysqlUtil.h                     # MySQL 工具
│   │   └── db/                             # 数据库连接池
│   └── src/                                # 对应实现
├── AIApps/ChatServer/                      # AI 聊天应用
│   ├── include/
│   │   ├── ChatServer.h                    # ChatServer 主类
│   │   ├── AIUtil/                         # AI 核心组件
│   │   │   ├── AIStrategy.h                # 策略模式（多模型适配）
│   │   │   ├── AIFactory.h                 # 工厂模式（注册式）
│   │   │   ├── AIHelper.h                  # 对话助手
│   │   │   ├── AIConfig.h                  # MCP 配置解析
│   │   │   ├── AIToolRegistry.h            # 工具注册表
│   │   │   ├── AISpeechProcessor.h         # 语音处理（ASR/TTS）
│   │   │   ├── ImageRecognizer.h           # 图像识别（ONNX）
│   │   │   ├── MQManager.h                 # 消息队列管理
│   │   │   ├── AISessionIdGenerator.h      # 会话ID生成器
│   │   │   └── base64.h                    # Base64 编解码
│   │   └── handlers/                       # HTTP 请求处理器（13个）
│   │       ├── ChatEntryHandler.h
│   │       ├── ChatLoginHandler.h
│   │       ├── ChatRegisterHandler.h
│   │       ├── ChatLogoutHandler.h
│   │       ├── ChatHandler.h
│   │       ├── ChatSendHandler.h
│   │       ├── ChatHistoryHandler.h
│   │       ├── ChatCreateAndSendHandler.h
│   │       ├── ChatSessionsHandler.h
│   │       ├── ChatSpeechHandler.h
│   │       ├── AIMenuHandler.h
│   │       ├── AIUploadHandler.h
│   │       └── AIUploadSendHandler.h
│   ├── src/
│   │   ├── main.cpp                        # 程序入口
│   │   ├── ChatServer.cpp                  # ChatServer 实现
│   │   ├── AIUtil/                         # AI 核心实现
│   │   └── handlers/                       # 处理器实现
│   └── resource/
│       ├── config.json                     # MCP 工具配置
│       ├── entry.html                      # 入口页面
│       ├── AI.html                         # AI 聊天页面
│       ├── menu.html                       # 功能菜单页面
│       └── upload.html                     # 图片上传页面
└── images/                                 # 示例图片（3张）
```

---

## 三、构建配置文件

### CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.10)
project(http_server)

# 设置 C++17 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找依赖库
# OpenSSL：用于 HTTPS 加密通信
# CURL：用于调用各 AI 模型的 HTTP API
# ONNX Runtime：用于本地模型推理（图像识别）
# MySQL Connector/C++：用于数据持久化
# SimpleAmqpClient + rabbitmq：用于消息队列
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

# 头文件路径：覆盖了 HttpServer 框架、AI 应用、MySQL、OpenSSL、CURL、OpenCV、ONNX
include_directories(
    ${PROJECT_SOURCE_DIR}/HttpServer/include    # HTTP 框架头文件
    ${PROJECT_SOURCE_DIR}/AIApps/ChatServer/include  # AI 应用头文件
    /usr/include/mysql-cppconn-8                # MySQL 连接器
    /usr/include/opencv4                        # OpenCV（图像处理）
    /usr/local/include                          # SimpleAmqpClient & ONNX Runtime
    ...
)

# 收集所有源文件
file(GLOB_RECURSE HTTP_SERVER_SRC   "${PROJECT_SOURCE_DIR}/HttpServer/src/*.cpp")
file(GLOB_RECURSE GOMOKU_SERVER_SRC "${PROJECT_SOURCE_DIR}/AIApps/ChatServer/src/*.cpp")
set(MAIN_SRC "${PROJECT_SOURCE_DIR}/AIApps/ChatServer/src/main.cpp")

# 构建可执行文件 http_server
add_executable(http_server ${MAIN_SRC} ${HTTP_SERVER_SRC} ${GOMOKU_SERVER_SRC})

# 链接所有依赖库
target_link_libraries(http_server
    pthread            # 线程库
    muduo_net muduo_base  # Muduo 网络库（底层 TCP 事件循环）
    mysqlcppconn mysqlclient  # MySQL 客户端
    ssl crypto         # OpenSSL
    ${CURL_LIBRARIES}  # HTTP 请求库
    ${OPENCV4_LIBRARIES}  # OpenCV（图像处理）
    ${ONNXRUNTIME_LIBRARY}  # ONNX 推理引擎
    SimpleAmqpClient rabbitmq  # 消息队列
)
```

**依赖关系图**：

```
http_server（可执行文件）
├── main.cpp              ← 入口
├── ChatServer.cpp        ← 核心业务
├── AIUtil/*.cpp          ← AI 模型调用
├── handlers/*.cpp        ← HTTP 请求处理
└── HttpServer/src/*.cpp  ← HTTP 框架底层
    ├── muduo              ← TCP 网络事件循环
    ├── OpenSSL            ← HTTPS 加密
    ├── libcurl            ← 调用外部 AI API
    ├── MySQL              ← 数据持久化
    ├── ONNX Runtime       ← 图像识别推理
    ├── OpenCV             ← 图像预处理
    └── RabbitMQ           ← 异步消息队列
```

---

## 四、入口点：main.cpp

```cpp
#include"../include/ChatServer.h"

// RabbitMQ 连接配置
const std::string RABBITMQ_HOST = "localhost";  // RabbitMQ 服务地址
const std::string QUEUE_NAME = "sql_queue";     // 队列名称
const int THREAD_NUM = 2;                       // 消费线程数

// 消息处理函数：从队列取出 SQL 并执行
void executeMysql(const std::string sql) {
    http::MysqlUtil mysqlUtil_;
    mysqlUtil_.executeUpdate(sql);
}

int main(int argc, char* argv[]) {
    LOG_INFO << "pid = " << getpid();
    std::string serverName = "ChatServer";
    int port = 80;

    // 支持命令行参数 -p 指定端口号
    // 例：./http_server -p 8080
    int opt;
    const char* str = "p:";
    while ((opt = getopt(argc, argv, str)) != -1) {
        switch (opt) {
        case 'p':
            port = atoi(optarg);  // 解析端口参数
            break;
        }
    }

    // 设置日志级别为 WARN（减少不必要的日志输出）
    muduo::Logger::setLogLevel(muduo::Logger::WARN);

    // 创建 ChatServer 实例（内部初始化了 HttpServer、路由、会话、中间件）
    // 服务器运行在指定端口，例如 80 或 8080
    ChatServer server(port, serverName);

    // 设置 4 个 I/O 线程（Muduo 的工作线程数）
    // 更多线程 = 更高并发处理能力
    server.setThreadNum(4);

    // 等待 2 秒让系统稳定（数据库连接池初始化需要时间）
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // 从 MySQL 恢复历史聊天记录到内存
    // 这样服务器重启后不会丢失对话历史
    server.initChatMessage();

    // 启动 RabbitMQ 线程池
    // 作用：异步执行 MySQL 写操作，避免主线程阻塞
    // 每当有新的聊天消息需要入库时，先存入 RabbitMQ 队列
    // 2 个后台线程从队列取出 SQL 语句并执行
    RabbitMQThreadPool pool(RABBITMQ_HOST, QUEUE_NAME, THREAD_NUM, executeMysql);
    pool.start();

    // 启动 HTTP 服务器，进入事件循环
    // 此后主线程阻塞，处理所有 HTTP 请求
    server.start();
}
```

**main.cpp 执行流程**：

```
main()
  ├── 解析命令行参数（端口号）
  ├── 创建 ChatServer 实例
  │   ├── 初始化 MySQL 连接池
  │   ├── 初始化会话管理
  │   ├── 初始化 CORS 中间件
  │   └── 注册所有路由（13 个处理器）
  ├── 设置 4 个工作线程
  ├── 从 MySQL 恢复历史聊天记录
  ├── 启动 RabbitMQ 消费线程池
  └── 启动 HTTP 服务器（阻塞，进入事件循环）
```

---

## 五、ChatServer 核心类

### ChatServer.h

```cpp
class ChatServer {
public:
    // 构造函数：
    // port  - 监听端口
    // name  - 服务器名称
    // option - TCP 端口复用选项（kNoReusePort = 不重用端口）
    ChatServer(int port, const std::string& name, ...);

    void setThreadNum(int numThreads);  // 设置工作线程数
    void start();                        // 启动 HTTP 服务器
    void initChatMessage();              // 从 MySQL 恢复历史消息

private:
    // 声明 11 个 Handler 类为友元
    // 这样 Handler 可以访问 ChatServer 的 private 成员（如 chatInformation、onlineUsers_）
    friend class ChatLoginHandler;
    friend class ChatRegisterHandler;
    // ... 其他 9 个 Handler

private:
    void initialize();          // 初始化(MySQL + Session + 中间件 + 路由)
    void initializeSession();   // 初始化会话管理
    void initializeRouter();    // 注册所有路由
    void initializeMiddleware(); // 初始化 CORS 中间件
    void readDataFromMySQL();   // 从 MySQL 读取历史聊天记录

    // 封装 HTTP 响应
    void packageResp(...);

    // ===== 核心成员变量 =====
    http::HttpServer httpServer_;          // HTTP 服务器框架实例
    http::MysqlUtil   mysqlUtil_;          // MySQL 操作工具

    // 在线用户状态表：userId → 是否在线
    // 防止同一用户重复登录
    std::unordered_map<int, bool> onlineUsers_;
    std::mutex mutexForOnlineUsers_;

    // 聊天信息表：userId → (sessionId → AIHelper)
    // 支持单用户多会话隔离
    std::unordered_map<int, std::unordered_map<std::string, std::shared_ptr<AIHelper>>> chatInformation;
    std::mutex mutexForChatInformation;

    // 图像识别器表：userId → ImageRecognizer
    // 每个用户一个独立的识别器实例
    std::unordered_map<int, std::shared_ptr<ImageRecognizer>> ImageRecognizerMap;
    std::mutex mutexForImageRecognizerMap;

    // 会话 ID 列表：userId → [sessionId1, sessionId2, ...]
    std::unordered_map<int, std::vector<std::string>> sessionsIdsMap;
    std::mutex mutexForSessionsId;
};
```

**关键设计：多会话隔离**

```
chatInformation[userId][sessionId] = AIHelper
     ↑              ↑              ↑
   用户ID        会话ID         该会话的对话助手（包含历史消息和模型策略）

例：
chatInformation[1001]["abc123"] → AIHelper_A（用户1001在会话abc123的对话）
chatInformation[1001]["xyz789"] → AIHelper_B（用户1001在会话xyz789的对话）
chatInformation[2002]["abc123"] → AIHelper_C（用户2002的对话，完全独立）
```

### ChatServer.cpp

```cpp
// ===== 构造函数 =====
ChatServer::ChatServer(int port, const std::string& name, ...)
    : httpServer_(port, name, option)  // 初始化底层 HttpServer
{
    initialize();  // 调用初始化方法
}

// ===== 初始化 =====
void ChatServer::initialize() {
    // 1. 初始化 MySQL 连接池
    //    连接地址: tcp://127.0.0.1:3306
    //    用户名: root
    //    密码: 123456
    //    数据库: ChatHttpServer
    //    连接池大小: 5
    http::MysqlUtil::init("tcp://127.0.0.1:3306", "root", "123456", "ChatHttpServer", 5);

    // 2. 初始化会话管理（内存存储）
    initializeSession();

    // 3. 初始化中间件（CORS 跨域）
    initializeMiddleware();

    // 4. 注册路由
    initializeRouter();
}

// ===== 初始化会话管理 =====
void ChatServer::initializeSession() {
    // 使用内存存储（重启会丢失会话，但聊天记录保存在 MySQL 中）
    auto sessionStorage = std::make_unique<http::session::MemorySessionStorage>();
    auto sessionManager = std::make_unique<http::session::SessionManager>(std::move(sessionStorage));
    setSessionManager(std::move(sessionManager));
}

// ===== 初始化中间件 =====
void ChatServer::initializeMiddleware() {
    // 添加 CORS 中间件，允许跨域请求
    // 这样前端页面（可能部署在不同的域名/端口）可以正常调用 API
    auto corsMiddleware = std::make_shared<http::middleware::CorsMiddleware>();
    httpServer_.addMiddleware(corsMiddleware);
}

// ===== 注册路由 =====
void ChatServer::initializeRouter() {
    // 每个路由绑定到一个 Handler 实例
    // Handler 通过 RouterHandler 接口的 handle() 方法处理请求

    httpServer_.Get("/",          std::make_shared<ChatEntryHandler>(this));      // 入口页面
    httpServer_.Get("/entry",     std::make_shared<ChatEntryHandler>(this));      // 入口页面(别名)
    httpServer_.Post("/login",    std::make_shared<ChatLoginHandler>(this));      // 登录
    httpServer_.Post("/register", std::make_shared<ChatRegisterHandler>(this));   // 注册
    httpServer_.Post("/user/logout", std::make_shared<ChatLogoutHandler>(this));  // 登出
    httpServer_.Get("/chat",      std::make_shared<ChatHandler>(this));           // 聊天页面
    httpServer_.Post("/chat/send", std::make_shared<ChatSendHandler>(this));      // 发送消息
    httpServer_.Get("/menu",      std::make_shared<AIMenuHandler>(this));         // 功能菜单
    httpServer_.Get("/upload",    std::make_shared<AIUploadHandler>(this));       // 图片上传页
    httpServer_.Post("/upload/send", std::make_shared<AIUploadSendHandler>(this)); // 图片识别
    httpServer_.Post("/chat/history", std::make_shared<ChatHistoryHandler>(this)); // 历史记录
    httpServer_.Post("/chat/send-new-session", std::make_shared<ChatCreateAndSendHandler>(this)); // 新会话
    httpServer_.Get("/chat/sessions", std::make_shared<ChatSessionsHandler>(this)); // 会话列表
    httpServer_.Post("/chat/tts", std::make_shared<ChatSpeechHandler>(this));     // 语音合成
}

// ===== 从 MySQL 恢复历史聊天记录 =====
void ChatServer::readDataFromMySQL() {
    // SQL：按时间戳升序读取所有聊天消息
    std::string sql = "SELECT id, username, session_id, is_user, content, ts "
                      "FROM chat_message ORDER BY ts ASC, id ASC";

    sql::ResultSet* res = mysqlUtil_.executeQuery(sql);

    while (res->next()) {
        long long user_id = res->getInt64("id");
        std::string session_id = res->getString("session_id");
        std::string username   = res->getString("username");
        std::string content    = res->getString("content");
        long long ts           = res->getInt64("ts");
        int is_user            = res->getInt("is_user");

        // 找到或创建该用户在该会话下的 AIHelper
        auto& userSessions = chatInformation[user_id];
        std::shared_ptr<AIHelper> helper;
        auto itSession = userSessions.find(session_id);
        if (itSession == userSessions.end()) {
            // 新会话，创建新的 AIHelper
            helper = std::make_shared<AIHelper>();
            userSessions[session_id] = helper;
            sessionsIdsMap[user_id].push_back(session_id);
        } else {
            // 已有会话，使用已有的 AIHelper
            helper = itSession->second;
        }

        // 恢复消息到 AIHelper 的 messages 向量中
        // restoreMessage 只添加消息，不会触发入库（避免重复写入）
        helper->restoreMessage(content, ts);
    }
}
```

---

## 六、AI 策略模块（Strategy + Factory）

### AIStrategy.h

```cpp
// ============================================================
// 策略模式基类：定义统一的 AI 模型调用接口
// 所有模型（阿里百炼、豆包、RAG、MCP）都实现这个接口
// ============================================================
class AIStrategy {
public:
    virtual ~AIStrategy() = default;

    // 返回模型 API 的 URL 地址
    virtual std::string getApiUrl() const = 0;

    // 返回 API 密钥（从环境变量读取）
    virtual std::string getApiKey() const = 0;

    // 返回模型名称
    virtual std::string getModel() const = 0;

    // 构建 API 请求体（JSON 格式）
    // messages: 对话历史 [(content, timestamp), ...]
    virtual json buildRequest(const std::vector<std::pair<std::string, long long>>& messages) const = 0;

    // 解析 API 响应，提取 AI 的回复文本
    virtual std::string parseResponse(const json& response) const = 0;

    // 是否支持 MCP 工具调用
    bool isMCPModel = false;
};

// ============================================================
// 阿里云百炼策略：调用通义千问（qwen-plus）
// API 地址：DashScope 兼容 OpenAI 格式
// ============================================================
class AliyunStrategy : public AIStrategy {
public:
    AliyunStrategy() {
        // 从环境变量 DASHSCOPE_API_KEY 读取 API 密钥
        // 需要在运行前设置：export DASHSCOPE_API_KEY="sk-xxx"
        const char* key = std::getenv("DASHSCOPE_API_KEY");
        if (!key) throw std::runtime_error("Aliyun API Key not found!");
        apiKey_ = key;
        isMCPModel = false;  // 不支持工具调用
    }
    std::string getApiUrl() const override;
    std::string getApiKey() const override;
    std::string getModel() const override;
    json buildRequest(...) const override;
    std::string parseResponse(...) const override;
private:
    std::string apiKey_;
};

// ============================================================
// 豆包策略：调用火山引擎豆包大模型
// API 地址：ark.cn-beijing.volces.com
// ============================================================
class DouBaoStrategy : public AIStrategy {
    // 从环境变量 DOUBAO_API_KEY 读取密钥
};

// ============================================================
// 阿里云 RAG 策略：调用百炼 RAG 应用
// API 地址：DashScope 百炼应用 API
// 需要环境变量 Knowledge_Base_ID 指定知识库 ID
// ============================================================
class AliyunRAGStrategy : public AIStrategy {
    // RAG 模式不需要显式指定 model，由百炼应用配置决定
};

// ============================================================
// 阿里云 MCP 策略：支持工具调用
// 模型会判断是否需要调用工具，然后进行两段式推理
// ============================================================
class AliyunMcpStrategy : public AIStrategy {
    // isMCPModel = true，触发工具调用流程
};
```

### AIStrategy.cpp

```cpp
// ===== AliyunStrategy 实现 =====

std::string AliyunStrategy::getApiUrl() const {
    // DashScope 的 OpenAI 兼容端点
    return "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";
}

std::string AliyunStrategy::getModel() const {
    return "qwen-plus";  // 通义千问 Plus 版本
}

// 构建请求体
json AliyunStrategy::buildRequest(const std::vector<std::pair<std::string, long long>>& messages) const {
    json payload;
    payload["model"] = getModel();           // "qwen-plus"
    json msgArray = json::array();

    for (size_t i = 0; i < messages.size(); ++i) {
        json msg;
        // 偶数索引 = 用户消息（user）
        // 奇数索引 = AI 回复（assistant）
        if (i % 2 == 0) {
            msg["role"] = "user";
        } else {
            msg["role"] = "assistant";
        }
        msg["content"] = messages[i].first;  // 消息文本内容
        msgArray.push_back(msg);
    }
    payload["messages"] = msgArray;
    return payload;
}

// 解析响应
std::string AliyunStrategy::parseResponse(const json& response) const {
    // OpenAI 格式：choices[0].message.content
    if (response.contains("choices") && !response["choices"].empty()) {
        return response["choices"][0]["message"]["content"];
    }
    return {};  // 解析失败返回空字符串
}

// ===== DouBaoStrategy 实现 =====

std::string DouBaoStrategy::getApiUrl() const {
    return "https://ark.cn-beijing.volces.com/api/v3/chat/completions";
}

std::string DouBaoStrategy::getModel() const {
    return "doubao-seed-1-6-thinking-250715";  // 豆包思考模型
}
// buildRequest 和 parseResponse 逻辑与 AliyunStrategy 相同（都是 OpenAI 兼容格式）

// ===== AliyunRAGStrategy 实现 =====

std::string AliyunRAGStrategy::getApiUrl() const {
    // RAG 应用 API：需要环境变量 Knowledge_Base_ID
    const char* key = std::getenv("Knowledge_Base_ID");
    std::string id(key);
    // URL 格式：https://dashscope.aliyuncs.com/api/v1/apps/{知识库ID}/completion
    return "https://dashscope.aliyuncs.com/api/v1/apps/" + id + "/completion";
}

json AliyunRAGStrategy::buildRequest(...) const {
    // RAG 格式不同于标准 Chat Completions
    // 请求体结构：input.messages、parameters
    json payload;
    payload["input"]["messages"] = msgArray;
    payload["parameters"] = json::object();
    return payload;
}

std::string AliyunRAGStrategy::parseResponse(const json& response) const {
    // RAG 返回格式：output.text
    if (response.contains("output") && response["output"].contains("text")) {
        return response["output"]["text"];
    }
    return {};
}

// ===== AliyunMcpStrategy 实现 =====
// 与 AliyunStrategy 基本相同，但 isMCPModel = true
// buildRequest 和 parseResponse 使用标准 OpenAI 格式

// ===== 注册策略到工厂 =====
// 静态全局变量，在程序启动时自动注册
// "1" = 阿里百炼, "2" = 豆包, "3" = 百炼RAG, "4" = 百炼MCP
static StrategyRegister<AliyunStrategy>    regAliyun("1");
static StrategyRegister<DouBaoStrategy>    regDoubao("2");
static StrategyRegister<AliyunRAGStrategy> regAliyunRag("3");
static StrategyRegister<AliyunMcpStrategy> regAliyunMcp("4");
```

### AIFactory.h

```cpp
// ============================================================
// 工厂模式（单例 + 注册式）
// 支持通过字符串名称创建对应的策略实例
// ============================================================

class StrategyFactory {
public:
    // 创建函数类型：输入为空，输出为智能指针
    using Creator = std::function<std::shared_ptr<AIStrategy>()>;

    // 单例模式：获取全局唯一实例
    static StrategyFactory& instance();

    // 注册一个策略创建函数
    void registerStrategy(const std::string& name, Creator creator);

    // 根据名称创建策略实例
    // name 可以是 "1"（阿里）、"2"（豆包）、"3"（RAG）、"4"（MCP）
    std::shared_ptr<AIStrategy> create(const std::string& name);

private:
    StrategyFactory() = default;  // 私有构造，确保单例
    std::unordered_map<std::string, Creator> creators;  // 名称 → 创建函数
};

// ============================================================
// 自动注册模板类
// 在程序初始化阶段（静态构造）自动将策略注册到工厂
// 用法：static StrategyRegister<AliyunStrategy> reg("1");
// ============================================================
template<typename T>
struct StrategyRegister {
    StrategyRegister(const std::string& name) {
        StrategyFactory::instance().registerStrategy(name, [] {
            return std::make_shared<T>();  // 创建具体策略实例
        });
    }
};
```

### AIFactory.cpp

```cpp
// 单例实现（线程安全的局部静态变量）
StrategyFactory& StrategyFactory::instance() {
    static StrategyFactory factory;
    return factory;
}

void StrategyFactory::registerStrategy(const std::string& name, Creator creator) {
    creators[name] = std::move(creator);
}

std::shared_ptr<AIStrategy> StrategyFactory::create(const std::string& name) {
    auto it = creators.find(name);
    if (it == creators.end()) {
        throw std::runtime_error("Unknown strategy: " + name);
    }
    return it->second();  // 调用创建函数，返回新实例
}
```

**策略 + 工厂模式数据流**：

```
客户端请求 modelType="2"
    ↓
AIHelper::chat() → StrategyFactory::instance().create("2")
    ↓
查找 creators["2"] → 返回 DouBaoStrategy 实例
    ↓
strategy->buildRequest(messages) → 生成豆包格式的请求体
    ↓
executeCurl(payload) → 发送 HTTP 请求到豆包 API
    ↓
strategy->parseResponse(response) → 提取 AI 回复文本
```

---

## 七、AI 对话助手

### AIHelper.h

```cpp
class AIHelper {
public:
    AIHelper();
    void setStrategy(std::shared_ptr<AIStrategy> strat);

    // 添加一条消息（用户消息或 AI 回复）
    // 同时将消息通过 RabbitMQ 异步入库
    void addMessage(int userId, const std::string& userName, bool is_user,
                    const std::string& userInput, std::string sessionId);

    // 恢复一条历史消息（仅加载到内存，不入库）
    void restoreMessage(const std::string& userInput, long long ms);

    // 核心方法：发送聊天消息，返回 AI 回复
    std::string chat(int userId, std::string userName, std::string sessionId,
                     std::string userQuestion, std::string modelType);

    // 获取所有历史消息
    std::vector<std::pair<std::string, long long>> GetMessages();

private:
    // SQL 语句转义（防止注入）
    std::string escapeString(const std::string& input);

    // 将消息通过 RabbitMQ 异步入库
    void pushMessageToMysql(...);

    // 执行 CURL HTTP 请求
    json executeCurl(const json& payload);
    static size_t WriteCallback(...);  // CURL 数据回调

private:
    std::shared_ptr<AIStrategy> strategy;  // 当前使用的模型策略

    // 历史消息列表：偶数索引=用户消息，奇数索引=AI回复
    // pair: (消息文本, 时间戳毫秒)
    std::vector<std::pair<std::string, long long>> messages;
};
```

### AIHelper.cpp

```cpp
// ===== 构造函数 =====
AIHelper::AIHelper() {
    // 默认使用阿里百炼模型（策略编号 "1"）
    strategy = StrategyFactory::instance().create("1");
}

// ===== 添加消息 =====
void AIHelper::addMessage(int userId, const std::string& userName,
                          bool is_user, const std::string& userInput,
                          std::string sessionId) {
    // 获取当前时间戳（毫秒）
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    // 1. 同步写入内存（立即可用）
    messages.push_back({userInput, ms});

    // 2. 异步写入 MySQL（通过 RabbitMQ 队列）
    pushMessageToMysql(userId, userName, is_user, userInput, ms, sessionId);
}

// ===== chat() 核心方法 =====
std::string AIHelper::chat(int userId, std::string userName,
                           std::string sessionId, std::string userQuestion,
                           std::string modelType) {

    // 1. 根据 modelType 切换策略
    setStrategy(StrategyFactory::instance().create(modelType));

    // 2. 判断是否支持 MCP 工具调用
    if (false == strategy->isMCPModel) {
        // ============================================================
        // 非 MCP 模式：直接调用模型，一次完成
        // ============================================================

        // 将用户消息存入历史（同步内存+异步入库）
        addMessage(userId, userName, true, userQuestion, sessionId);

        // 构建请求体（包含完整对话历史）
        json payload = strategy->buildRequest(this->messages);

        // 发送 HTTP 请求到模型 API
        json response = executeCurl(payload);

        // 解析 AI 回复
        std::string answer = strategy->parseResponse(response);

        // 将 AI 回复存入历史
        addMessage(userId, userName, false, answer, sessionId);

        return answer.empty() ? "[Error] 无法解析响应" : answer;
    }

    // ============================================================
    // MCP 模式：两段式推理
    // 第1段：模型判断是否需要工具调用
    // 第2段：如果调用了工具，根据工具结果再次请求模型
    // ============================================================

    AIConfig config;
    config.loadFromFile("../AIApps/ChatServer/resource/config.json");

    // 构建带工具列表的 Prompt
    std::string tempUserQuestion = config.buildPrompt(userQuestion);
    messages.push_back({tempUserQuestion, 0});  // 临时添加 Prompt

    // 第1段：发送 Prompt 给模型
    json firstReq = strategy->buildRequest(this->messages);
    json firstResp = executeCurl(firstReq);
    std::string aiResult = strategy->parseResponse(firstResp);
    messages.pop_back();  // 移除临时 Prompt

    // 解析 AI 响应，判断是否要调用工具
    AIToolCall call = config.parseAIResponse(aiResult);

    if (!call.isToolCall) {
        // 模型回答，不需要工具调用
        addMessage(userId, userName, true, userQuestion, sessionId);
        addMessage(userId, userName, false, aiResult, sessionId);
        return aiResult;
    }

    // 模型要求调用工具，执行工具
    AIToolRegistry registry;
    json toolResult = registry.invoke(call.toolName, call.args);

    // 第2段：根据工具结果再次请求模型
    std::string secondPrompt = config.buildToolResultPrompt(
        userQuestion, call.toolName, call.args, toolResult);

    messages.push_back({secondPrompt, 0});
    json secondReq = strategy->buildRequest(messages);
    json secondResp = executeCurl(secondReq);
    std::string finalAnswer = strategy->parseResponse(secondResp);
    messages.pop_back();  // 移除第二次 Prompt

    // 保存最终的问答对
    addMessage(userId, userName, true, userQuestion, sessionId);
    addMessage(userId, userName, false, finalAnswer, sessionId);
    return finalAnswer;
}

// ===== 消息异步入库 =====
void AIHelper::pushMessageToMysql(...) {
    // 构建 SQL INSERT 语句
    std::string sql = "INSERT INTO chat_message (id, username, session_id, is_user, content, ts) VALUES ("
        + std::to_string(userId) + ", "
        + "'" + safeUserName + "', "
        + sessionId + ", "
        + std::to_string(is_user ? 1 : 0) + ", "
        + "'" + safeUserInput + "', "
        + std::to_string(ms) + ")";

    // 发布消息到 RabbitMQ 队列"sql_queue"
    // 后台线程会从队列消费并执行 SQL
    MQManager::instance().publish("sql_queue", sql);
}
```

**MCP 两段式推理流程**：

```
用户: "今天北京天气怎么样？"
  ↓
构建带工具列表的 Prompt（告诉模型有哪些工具可用）
  ↓
第1次请求模型
  ↓
模型判断: 需要调用 get_weather 工具
  ↓
执行工具: AIToolRegistry.invoke("get_weather", {"city": "北京"})
  ↓
工具返回: {"city": "北京", "weather": "北京: 晴, 25°C"}
  ↓
构建第二次 Prompt（用户原问题 + 工具结果）
  ↓
第2次请求模型
  ↓
模型回答: "今天北京天气晴朗，气温25°C"
  ↓
保存问答对到历史记录
```

---

## 八、MCP 工具协议

### AIConfig.h / AIConfig.cpp

```cpp
// ===== 工具定义结构体 =====
struct AITool {
    std::string name;                                    // 工具名称，如 "get_weather"
    std::unordered_map<std::string, std::string> params; // 参数名→参数描述
    std::string desc;                                    // 工具描述
};

// ===== 工具调用结果结构体 =====
struct AIToolCall {
    std::string toolName;  // 要调用的工具名
    json args;             // 工具参数
    bool isToolCall;       // 是否为工具调用（vs 普通回答）
};

class AIConfig {
public:
    // 从 config.json 加载 Prompt 模板和工具列表
    bool loadFromFile(const std::string& path);

    // 构建完整 Prompt：模板 + 用户输入 + 工具列表
    // 将 {user_input} 替换为实际用户输入
    // 将 {tool_list} 替换为工具清单
    std::string buildPrompt(const std::string& userInput) const;

    // 解析 AI 响应，判断是否要求调用工具
    AIToolCall parseAIResponse(const std::string& response) const;

    // 构建工具结果 Prompt（用于第二段推理）
    std::string buildToolResultPrompt(...) const;

private:
    std::string promptTemplate_;    // Prompt 模板（从 config.json 加载）
    std::vector<AITool> tools_;     // 工具列表（从 config.json 加载）
};

// loadFromFile 解析的 config.json 格式：
/*
{
    "prompt_template": "你是AI助手。用户说：{user_input}\\n可用工具：\\n{tool_list}\\n如果需要调用工具，返回JSON:{\"tool\":\"工具名\",\"args\":{...}}，否则直接回答。",
    "tools": [
        {
            "name": "get_weather",
            "desc": "查询指定城市的天气",
            "params": {"city": "城市名称"}
        },
        {
            "name": "get_time",
            "desc": "获取当前时间",
            "params": {}
        }
    ]
}
*/
```

### AIToolRegistry.h / AIToolRegistry.cpp

```cpp
class AIToolRegistry {
public:
    // 工具函数类型：接收 JSON 参数，返回 JSON 结果
    using ToolFunc = std::function<json(const json&)>;

    AIToolRegistry() {
        // 注册两个内置工具
        registerTool("get_weather", getWeather);
        registerTool("get_time", getTime);
    }

    void registerTool(const std::string& name, ToolFunc func);
    json invoke(const std::string& name, const json& args) const;
    bool hasTool(const std::string& name) const;

private:
    std::unordered_map<std::string, ToolFunc> tools_;

    // ===== get_weather 工具实现 =====
    static json getWeather(const json& args) {
        // 从参数中提取城市名
        std::string city = args["city"].get<std::string>();

        // 调用 wttr.in 免费天气 API
        // URL 格式：https://wttr.in/{城市}?format=3&lang=zh
        std::string url = "https://wttr.in/" + encodedCity + "?format=3&lang=zh";

        // 使用 CURL 发起 HTTP 请求
        // 返回: {"city": "北京", "weather": "Beijing: Clear 25°C"}
        return json{{"city", city}, {"weather", response}};
    }

    // ===== get_time 工具实现 =====
    static json getTime(const json& args) {
        // 获取当前系统时间
        std::time_t t = std::time(nullptr);
        std::tm* now = std::localtime(&t);
        // 格式化为 "2025-06-05 15:30:00"
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", now);
        return json{{"time", buffer}};
    }
};
```

---

## 九、语音处理模块

### AISpeechProcessor.h / AISpeechProcessor.cpp

```cpp
class AISpeechProcessor {
public:
    // 构造函数
    // clientId      - 百度云 API Key（Client ID）
    // clientSecret  - 百度云 Secret Key
    // cuid          - 用户唯一标识
    AISpeechProcessor(const std::string& clientId,
                      const std::string& clientSecret,
                      const std::string& cuid = "RZjSQGzNaA8EFWf6rvuHEKDh9i4XJIV9");

    // 语音识别（ASR）：将音频数据转为文本
    // speechData - Base64 编码的音频数据
    // format     - 音频格式（默认 pcm）
    // rate       - 采样率（默认 16000）
    // channel    - 声道数（默认 1）
    std::string recognize(const std::string& speechData, ...);

    // 语音合成（TTS）：将文本转为语音 URL
    // text   - 要合成的文本
    // format - 音频格式（默认 mp3-16k）
    // lang   - 语言（默认 zh 中文）
    // speed  - 语速（1-15, 默认5）
    // pitch  - 音调（1-15, 默认5）
    // volume - 音量（1-15, 默认5）
    std::string synthesize(const std::string& text, ...);

private:
    std::string getAccessToken();  // 获取百度 API Access Token
    std::string token_;            // 缓存的 Token
};

// ===== 获取 Access Token =====
// 调用百度 OAuth API 获取调用凭证
// POST https://aip.baidubce.com/oauth/2.0/token
// Body: grant_type=client_credentials&client_id=xxx&client_secret=xxx
std::string AISpeechProcessor::getAccessToken() {
    CURL* curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_URL, "https://aip.baidubce.com/oauth/2.0/token");
    // ... 发送 client_credentials 请求
    // 返回: {"access_token": "24.xxx", "expires_in": 2592000}
    return j["access_token"];
}

// ===== 语音合成流程 =====
std::string AISpeechProcessor::synthesize(const std::string& text, ...) {
    // 第1步：创建合成任务
    // POST https://aip.baidubce.com/rpc/2.0/tts/v1/create?access_token=xxx
    // Body: {"text": "你好世界", "format": "mp3-16k", "lang": "zh", "speed": 5, ...}
    // 返回: {"task_id": "abc123"}

    // 第2步：轮询查询任务状态（最多等60秒）
    // POST https://aip.baidubce.com/rpc/2.0/tts/v1/query?access_token=xxx
    // Body: {"task_ids": ["abc123"]}
    // 返回: {"tasks_info": [{"task_status": "Success", "task_result": {"speech_url": "http://..."}}]}

    while (loops++ < 60) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        // 查询任务状态
        if (status == "Success") {
            return task["task_result"]["speech_url"];  // 返回音频 URL
        }
    }

    return "";  // 超时或失败
}
```

**TTS 调用流程**：
```
ChatSpeechHandler.handle()
  ↓ 接收 POST /chat/tts, body: {"text": "你好"}
  ↓ 从环境变量读取 BAIDU_CLIENT_ID 和 BAIDU_CLIENT_SECRET
  ↓ 创建 AISpeechProcessor 实例
  ↓ speechProcessor.synthesize("你好")
    ↓ 第1步：POST create API → 获取 task_id
    ↓ 第2步：轮询 query API（每1秒一次，最多60次）
    ↓ 任务完成 → 获取 speech_url
  ↓ 返回 JSON: {"success": true, "url": "http://xxx.mp3"}
```

---

## 十、图像识别模块

### ImageRecognizer.h / ImageRecognizer.cpp

```cpp
class ImageRecognizer {
public:
    // 构造函数
    // model_path - ONNX 模型文件路径（如 "/root/models/mobilenetv2/mobilenetv2-7.onnx"）
    // label_path - 分类标签文件路径（每行一个标签名）
    explicit ImageRecognizer(const std::string& model_path,
                             const std::string& label_path = "/root/imagenet_classes.txt");

    // 从文件路径预测
    std::string PredictFromFile(const std::string& image_path);

    // 从内存缓冲区预测（用于接收前端上传的图片）
    std::string PredictFromBuffer(const std::vector<unsigned char>& image_data);

    // 从 OpenCV Mat 预测
    std::string PredictFromMat(const cv::Mat& img);

private:
    Ort::Env env;                                   // ONNX 环境
    std::unique_ptr<Ort::Session> session;           // ONNX 会话（模型）
    std::string input_name, output_name;             // 输入/输出节点名
    std::vector<int64_t> input_shape;                // 输入张量形状
    std::vector<std::string> labels;                 // 分类标签
};

// ===== 构造函数：加载 ONNX 模型 =====
ImageRecognizer::ImageRecognizer(const std::string& model_path, ...)
    : env(ORT_LOGGING_LEVEL_WARNING, "ImageRecognizer")
{
    // 配置 ONNX Session 选项
    Ort::SessionOptions session_options;
    session_options.SetIntraOpNumThreads(1);  // 单线程推理
    session_options.SetGraphOptimizationLevel(
        GraphOptimizationLevel::ORT_ENABLE_EXTENDED);  // 启用图优化

    // 加载 ONNX 模型
    session = std::make_unique<Ort::Session>(env, model_path.c_str(), session_options);

    // 获取输入/输出节点信息
    input_name = session->GetInputNameAllocated(0, *allocator).get();
    output_name = session->GetOutputNameAllocated(0, *allocator).get();

    // 获取输入张量形状（如 [1, 3, 224, 224]）
    input_shape = session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    input_height = static_cast<int>(input_shape[2]);  // 224
    input_width  = static_cast<int>(input_shape[3]);  // 224

    // 加载标签文件
    LoadLabels(label_path);
}

// ===== 图像推理 =====
std::string ImageRecognizer::PredictFromMat(const cv::Mat& img_raw) {
    // 1. 预处理：缩放 + 归一化
    cv::Mat img;
    cv::resize(img_raw, img, cv::Size(input_width, input_height));
    img.convertTo(img, CV_32F, 1.0 / 255.0);  // 归一化到 [0, 1]

    // 2. NHWC → NCHW 转换（OpenCV 读取是 HWC，ONNX 需要 CHW）
    cv::dnn::blobFromImage(img, img);

    // 3. 创建输入张量
    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        memory_info, img.ptr<float>(), input_tensor_size, dims.data(), dims.size());

    // 4. 执行推理
    auto output_tensors = session->Run(
        Ort::RunOptions{nullptr},
        &input_name, &input_tensor, 1,   // 1个输入
        &output_name, 1);                 // 1个输出

    // 5. argmax：找到概率最大的类别
    float* output_data = output_tensors.front().GetTensorMutableData<float>();
    int pred_class = std::max_element(output_data, output_data + num_classes) - output_data;

    // 6. 返回标签名
    return labels[pred_class];  // 如 "golden retriever"
}
```

---

## 十一、消息队列模块

### MQManager.h / MQManager.cpp

```cpp
// ============================================================
// MQManager：RabbitMQ 连接池（单例）
// 用于生产者：发送消息到队列
// ============================================================
class MQManager {
public:
    static MQManager& instance();  // 单例

    // 发布消息到指定队列
    void publish(const std::string& queue, const std::string& msg);

private:
    MQManager(size_t poolSize = 5);  // 默认 5 个连接
    // 连接结构：一个 Channel + 一个 Mutex
    struct MQConn {
        AmqpClient::Channel::ptr_t channel;
        std::mutex mtx;
    };
    std::vector<std::shared_ptr<MQConn>> pool_;  // 连接池
    std::atomic<size_t> counter_;                 // 轮询计数器
};

// 发布消息（轮询选择连接，线程安全）
void MQManager::publish(const std::string& queue, const std::string& msg) {
    // 轮询选择连接（取模操作，原子递增计数器）
    size_t index = counter_.fetch_add(1) % poolSize_;
    auto& conn = pool_[index];

    // 加锁，确保同一连接上消息不交错
    std::lock_guard<std::mutex> lock(conn->mtx);

    // 创建并发布消息到队列
    auto message = AmqpClient::BasicMessage::Create(msg);
    conn->channel->BasicPublish("", queue, message);  // 空 exchange → 走默认交换机
}

// ============================================================
// RabbitMQThreadPool：消费者线程池
// 从队列取出消息，调用处理函数
// ============================================================
class RabbitMQThreadPool {
public:
    using HandlerFunc = std::function<void(const std::string&)>;

    void start();    // 启动所有消费线程
    void shutdown(); // 停止所有线程

    ~RabbitMQThreadPool() {
        shutdown();  // 析构时自动停止
    }

private:
    void worker(int id);  // 单个工作线程
    std::vector<std::thread> workers_;  // 工作线程列表
    std::atomic<bool> stop_;            // 停止标志
    HandlerFunc handler_;               // 消息处理函数
};

// 工作线程：循环从队列取出消息并处理
void RabbitMQThreadPool::worker(int id) {
    // 每个线程创建独立的 Channel
    auto channel = AmqpClient::Channel::Create(rabbitmq_host_, 5672, "guest", "guest", "/");

    // 声明队列（非持久化、排他、非自动删除）
    channel->DeclareQueue(queue_name_, false, true, false, false);

    // 开始消费
    std::string consumer_tag = channel->BasicConsume(queue_name_, "", true, false, false);
    channel->BasicQos(consumer_tag, 1);  // 每次只取一条消息

    while (!stop_) {
        AmqpClient::Envelope::ptr_t env;
        // 阻塞等待消息，超时 500ms（配合 stop_ 检查）
        bool ok = channel->BasicConsumeMessage(consumer_tag, env, 500);
        if (ok && env) {
            std::string msg = env->Message()->Body();
            handler_(msg);        // 调用处理函数（执行 SQL）
            channel->BasicAck(env); // 确认消息
        }
    }
    channel->BasicCancel(consumer_tag);
}
```

**消息队列数据流**：

```
AIHelper::pushMessageToMysql()
  ↓
MQManager::publish("sql_queue", "INSERT INTO chat_message ...")
  ↓ (Producer → RabbitMQ Broker → Consumer)
RabbitMQThreadPool::worker() 取出消息
  ↓
handler_(msg) = executeMysql(msg)
  ↓
mysqlUtil_.executeUpdate("INSERT INTO chat_message ...")
```

---

## 十二、HTTP 请求处理器（Handlers）

### 处理器模式概述

所有 Handler 都继承自 `http::router::RouterHandler` 接口：

```cpp
// RouterHandler.h
class RouterHandler {
public:
    virtual ~RouterHandler() = default;
    virtual void handle(const HttpRequest& req, HttpResponse* resp) = 0;
};
```

**通用处理流程**（大部分 Handler 遵循）：

1. **获取会话**：`server_->getSessionManager()->getSession(req, resp)`
2. **鉴权检查**：`session->getValue("isLoggedIn") != "true"` → 返回 401
3. **获取用户信息**：从 session 读取 `userId` 和 `username`
4. **处理业务逻辑**
5. **封装 JSON 响应**

---

### ChatEntryHandler

**路由**：`GET /` 和 `GET /entry`

**功能**：返回入口 HTML 页面（`entry.html`）

```cpp
void ChatEntryHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp) {
    // 读取 HTML 文件
    FileUtil fileOperater("../AIApps/ChatServer/resource/entry.html");

    std::vector<char> buffer(fileOperater.size());
    fileOperater.readFile(buffer);
    std::string bufStr = std::string(buffer.data(), buffer.size());

    // 返回 HTML 响应
    resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
    resp->setCloseConnection(false);  // Keep-Alive
    resp->setContentType("text/html");
    resp->setContentLength(bufStr.size());
    resp->setBody(bufStr);
}
```

---

### ChatLoginHandler

**路由**：`POST /login`

**请求体**：
```json
{"username": "admin", "password": "123456"}
```

**功能**：
1. 验证 Content-Type 为 `application/json`
2. 解析用户名和密码
3. 查询 MySQL 验证身份
4. 创建 Session，设置 `userId`、`username`、`isLoggedIn`
5. 标记用户为在线状态
6. 如果已在线，返回 403 拒绝重复登录

**响应**：
```json
// 成功
{"success": true, "userId": 1}
// 失败
{"status": "error", "message": "Invalid username or password"}
```

---

### ChatRegisterHandler

**路由**：`POST /register`

**请求体**：
```json
{"username": "newuser", "password": "123456"}
```

**功能**：
1. 检查用户是否已存在
2. 插入新用户到 MySQL
3. 返回新用户的 userId

---

### ChatLogoutHandler

**路由**：`POST /user/logout`

**功能**：
1. 清除当前 Session
2. 从 `onlineUsers_` 移除用户

---

### ChatHandler

**路由**：`GET /chat`

**功能**：返回 AI 聊天页面（`AI.html`），并在 `</head>` 前注入 userId 的 JavaScript 变量

```cpp
// 在 HTML 中注入 userId，前端 JS 可以直接使用
size_t headEnd = htmlContent.find("</head>");
if (headEnd != std::string::npos) {
    std::string script = "<script>const userId = '" + std::to_string(userId) + "';</script>";
    htmlContent.insert(headEnd, script);
}
```

---

### ChatSendHandler

**路由**：`POST /chat/send`

**请求体**：
```json
{"question": "你好", "modelType": "1", "sessionId": "abc123"}
```

**功能**：核心聊天接口
1. 鉴权检查
2. 解析用户问题、模型类型、会话 ID
3. 查找或创建该会话的 AIHelper
4. 调用 `AIHelper::chat()` 获取 AI 回复
5. 返回 JSON 响应

```cpp
// 核心逻辑
{
    std::lock_guard<std::mutex> lock(server_->mutexForChatInformation);
    auto& userSessions = server_->chatInformation[userId];
    if (userSessions.find(sessionId) == userSessions.end()) {
        // 新会话，创建 AIHelper
        userSessions.emplace(sessionId, std::make_shared<AIHelper>());
    }
    AIHelperPtr = userSessions[sessionId];
}

// 调用 AI 聊天
std::string aiInformation = AIHelperPtr->chat(userId, username, sessionId, userQuestion, modelType);
```

**响应**：
```json
{"success": true, "Information": "你好！有什么可以帮助你的吗？"}
```

---

### ChatHistoryHandler

**路由**：`POST /chat/history`

**请求体**：
```json
{"sessionId": "abc123"}
```

**功能**：获取指定会话的历史消息

```cpp
// 获取消息列表
messages = AIHelperPtr->GetMessages();

// 构建 JSON 数组
for (size_t i = 0; i < messages.size(); ++i) {
    json msgJson;
    msgJson["is_user"] = (i % 2 == 0);  // 偶数索引=用户消息
    msgJson["content"] = messages[i].first;
    successResp["history"].push_back(msgJson);
}
```

**响应**：
```json
{
    "success": true,
    "history": [
        {"is_user": true, "content": "你好"},
        {"is_user": false, "content": "你好！有什么可以帮助你的吗？"}
    ]
}
```

---

### ChatCreateAndSendHandler

**路由**：`POST /chat/send-new-session`

**请求体**：
```json
{"question": "你好", "modelType": "1"}
```

**功能**：创建新会话并发消息
1. 使用 `AISessionIdGenerator` 生成唯一会话 ID
2. 创建新的 AIHelper
3. 发送消息并返回 AI 回复
4. 在响应中返回 sessionId（前端后续可用此 ID 继续对话）

**与 ChatSendHandler 的区别**：
- `ChatSendHandler`：需要在已有会话中发消息（需提供 sessionId）
- `ChatCreateAndSendHandler`：自动创建新会话并发送消息

---

### ChatSessionsHandler

**路由**：`GET /chat/sessions`

**功能**：返回用户的所有会话列表

**响应**：
```json
{
    "success": true,
    "sessions": [
        {"sessionId": "abc123", "name": "会话 abc123"},
        {"sessionId": "xyz789", "name": "会话 xyz789"}
    ]
}
```

---

### ChatSpeechHandler

**路由**：`POST /chat/tts`

**请求体**：
```json
{"text": "你好世界"}
```

**功能**：文字转语音
1. 从环境变量读取百度 API 凭证
2. 创建 `AISpeechProcessor`
3. 调用 `synthesize()` 获取语音 URL
4. 返回语音 URL

**响应**：
```json
{"success": true, "url": "http://tsn.baidu.com/xxx.mp3"}
```

---

### AIMenuHandler

**路由**：`GET /menu`

**功能**：返回功能菜单页面（`menu.html`），注入 userId

---

### AIUploadHandler

**路由**：`GET /upload`

**功能**：返回图片上传页面（`upload.html`），注入 userId

---

### AIUploadSendHandler

**路由**：`POST /upload/send`

**请求体**：
```json
{"filename": "dog.jpg", "image": "base64编码的图片数据"}
```

**功能**：图片识别
1. 为每个用户创建独立的 `ImageRecognizer` 实例
2. Base64 解码图片数据
3. 调用 `ImageRecognizer::PredictFromBuffer()` 进行 ONNX 推理
4. 返回识别结果

```cpp
// 每个用户一个 ImageRecognizer 实例
if (server_->ImageRecognizerMap.find(userId) == server_->ImageRecognizerMap.end()) {
    server_->ImageRecognizerMap.emplace(
        userId,
        std::make_shared<ImageRecognizer>("/root/models/mobilenetv2/mobilenetv2-7.onnx")
    );
}

// Base64 解码 → 推理
std::string decodedData = base64_decode(imageBase64);
std::vector<uchar> imgData(decodedData.begin(), decodedData.end());
std::string className = ImageRecognizerPtr->PredictFromBuffer(imgData);
```

**响应**：
```json
{
    "success": "ok",
    "filename": "dog.jpg",
    "class_name": "golden retriever",
    "confidence": 0.95
}
```

---

## 十三、工具类

### AISessionIdGenerator

```cpp
class AISessionIdGenerator {
public:
    std::string generate() {
        // 获取系统时间的纳秒计数
        auto now = std::chrono::system_clock::now().time_since_epoch().count();
        // 生成 0~99999 的随机数
        long long randVal = std::rand() % 100000;
        // XOR 组合时间戳和随机数，生成唯一 ID
        long long rawId = now ^ randVal;
        return std::to_string(rawId);
    }
};
```

### base64

基于 René Nyffenegger 的经典 C++ Base64 库，提供：
- `base64_encode(data)` — 编码为 Base64
- `base64_decode(data)` — 从 Base64 解码
- `base64_encode_pem(data)` — PEM 格式编码（每行 64 字符）

在项目中的用途：
- **图片上传识别**：前端将图片 Base64 编码后发送，后端解码后送入 ONNX
- **语音处理**：音频数据 Base64 编码传输

---

## 十四、HttpServer 框架部分

HttpServer 框架的代码在 `/data/workspace/Kama-HTTPServer` 中也有相同实现，前后已详细解释过。这里简要回顾核心组件：

| 组件 | 文件 | 作用 |
|------|------|------|
| **HttpContext** | `HttpContext.h/cpp` | 状态机解析 HTTP 报文（请求行→请求头→请求体） |
| **HttpRequest** | `HttpRequest.h/cpp` | 请求对象，存储 method/path/headers/body |
| **HttpResponse** | `HttpResponse.h/cpp` | 响应对象，序列化为 HTTP 报文 |
| **HttpServer** | `HttpServer.h/cpp` | TCP 服务器，处理连接/消息/请求 |
| **Router** | `Router.h/cpp` | 路由分发（精准匹配 + 正则匹配） |
| **RouterHandler** | `RouterHandler.h` | Handler 接口（handle 纯虚函数） |
| **Middleware** | `Middleware.h` | 中间件接口（processBefore/processAfter） |
| **CorsMiddleware** | `CorsMiddleware.h/cpp` | CORS 跨域处理 |
| **Session** | `Session.h/cpp` | 会话存储（userId/username/isLoggedIn） |
| **SessionManager** | `SessionManager.h/cpp` | 会话管理（创建/获取/销毁） |
| **SslConnection** | `SslConnection.h/cpp` | SSL/TLS 加密连接 |

---

## 十五、完整请求流程

以用户登录后发送一条 AI 聊天消息为例：

```
1. 用户请求 POST /login
   ↓
2. Muduo TcpServer 接收 TCP 数据包
   ↓
3. HttpServer::onMessage() 被调用
   ↓
4. HttpContext::parseRequest() 解析 HTTP 报文
   ↓ 状态机：请求行 → 请求头 → 请求体 → gotAll
   ↓
5. HttpServer::onRequest() 分发请求
   ↓
6. httpCallback_ → handleRequest()
   ↓ 中间件前处理
   ↓ 路由匹配：POST /login → ChatLoginHandler
   ↓
7. ChatLoginHandler::handle()
   ↓ 查询 MySQL 验证身份
   ↓ 创建 Session，设置 isLoggedIn=true
   ↓ onlineUsers_[userId] = true
   ↓ 返回 JSON: {"success": true, "userId": 1}
   ↓
8. 响应序列化 → 发送给客户端

===============================

9. 用户请求 POST /chat/send
   Body: {"question": "你好", "modelType": "1", "sessionId": "abc123"}
   ↓
10. 同步骤 2-6，路由到 ChatSendHandler
   ↓
11. ChatSendHandler::handle()
    ↓ 检查 isLoggedIn
    ↓ 获取 userId、username
    ↓ 查找/创建 chatInformation[userId][sessionId] → AIHelper
    ↓
12. AIHelper::chat(userId, username, "abc123", "你好", "1")
    ↓ StrategyFactory::create("1") → AliyunStrategy
    ↓ addMessage(User, "你好") → 同步写内存 + RabbitMQ 异步入库
    ↓ buildRequest(messages) → OpenAI 格式 JSON
    ↓ executeCurl(payload) → POST DashScope API
    ↓ parseResponse(response) → 提取 AI 回复
    ↓ addMessage(Assistant, AI回复) → 同步写内存 + 异步入库
    ↓ 返回 "你好！有什么可以帮助你的吗？"
    ↓
13. ChatSendHandler 封装 JSON 响应 → 发送给客户端

===============================

14. 后台 RabbitMQ 消费线程取出 SQL 并执行
    ↓
15. INSERT INTO chat_message (id, username, session_id, is_user, content, ts)
     VALUES (1, "admin", "abc123", 1, "你好", 1717000000000);
     VALUES (1, "admin", "abc123", 0, "你好！...", 1717000001000);
```

---

## 十六、部署与运行

### 环境要求

| 依赖 | 版本/说明 |
|------|----------|
| C++ 编译器 | GCC 8+ (支持 C++17) |
| Muduo | 网络库 |
| MySQL | 8.0+（数据存储） |
| MySQL Connector/C++ | 8.0+（C++ 客户端） |
| RabbitMQ | 3.x（消息队列） |
| SimpleAmqpClient | RabbitMQ C++ 客户端 |
| ONNX Runtime | 1.x（模型推理） |
| OpenCV | 4.x（图像处理） |
| libcurl | 7.x（HTTP 请求） |
| OpenSSL | 1.x+（加密通信） |

### 环境变量

```bash
# 阿里百炼 API 密钥
export DASHSCOPE_API_KEY="sk-xxx"

# 豆包 API 密钥
export DOUBAO_API_KEY="xxx"

# 百度语音 API 凭证
export BAIDU_CLIENT_ID="xxx"
export BAIDU_CLIENT_SECRET="xxx"

# 百炼 RAG 知识库 ID
export Knowledge_Base_ID="xxx"
```

### 数据库初始化

```sql
-- 创建数据库
CREATE DATABASE ChatHttpServer;

-- 用户表
CREATE TABLE users (
    id INT AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(50) NOT NULL UNIQUE,
    password VARCHAR(50) NOT NULL
);

-- 聊天消息表
CREATE TABLE chat_message (
    id INT,
    username VARCHAR(50),
    session_id VARCHAR(100),
    is_user TINYINT(1),   -- 1=用户消息, 0=AI回复
    content TEXT,
    ts BIGINT,             -- 时间戳（毫秒）
    PRIMARY KEY (id, ts)
);
```

### 编译运行

```bash
cd /data/workspace/CppAIService
mkdir build && cd build
cmake ..
make -j4

# 启动 RabbitMQ
systemctl start rabbitmq-server

# 启动服务（默认端口 80）
./http_server

# 或指定端口
./http_server -p 8080
```

---

## 附录：设计模式总结

| 设计模式 | 应用位置 | 作用 |
|----------|---------|------|
| **策略模式** | `AIStrategy` 及其子类 | 统一封装不同模型的调用方式，运行时切换 |
| **工厂模式** | `StrategyFactory` | 通过名称动态创建策略实例 |
| **注册式模式** | `StrategyRegister<T>` | 静态构造函数自动注册策略到工厂 |
| **单例模式** | `StrategyFactory::instance()`, `MQManager::instance()` | 全局唯一实例 |
| **观察者模式** | Muduo 的 ConnectionCallback / MessageCallback | TCP 事件回调 |
| **责任链模式** | `MiddlewareChain` | 请求/响应在中间件链上传递 |
| **模板方法模式** | `RouterHandler::handle()` | 定义 Handler 接口模板 |
| **生产者-消费者** | `MQManager` + `RabbitMQThreadPool` | 异步消息处理 |

---

> 文档生成时间：2026-06-24
> 对应 Git 版本：CppAIService 第二版
