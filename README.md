# AI Agent 平台

基于 C++ 开发的智能 AI Agent 后端服务，集成多模型对话、共享知识库灌库、图像识别、工具调用与语音合成能力，支持多轮会话记忆与异步任务调度。

## 核心能力

- **多模型适配（Strategy + Factory）**：通过策略模式与工厂模式统一抽象多厂商 API 差异，一键切换模型，新增模型仅需新增一个策略类并注册一行代码
- **轻量级 MCP 工具调用**：参考 Model Context Protocol 思想，自研两段式工具调用机制，第一段由模型决策是否调工具，第二段基于工具结果二次推理生成最终答案
- **图像识别**：基于 ONNX Runtime + OpenCV 部署 MobileNetV2 分类模型，实现端到端推理流程
- **共享 RAG 灌库**：平台上传文档，经百炼数据面追加进同一份知识库；聊天选「百炼RAG」检索仍在云上
- **语音合成（TTS）**：集成百度语音合成 API，支持创建任务→轮询→回传 URL 的异步流程
- **异步消息队列**：RabbitMQ 承载持久化写库，前台同步写内存、异步入库，避免主线程阻塞
- **多会话管理**：通过 `unordered_map<userId, map<sessionId, AIHelper>>` 实现多用户多会话隔离

## 技术栈

- **语言**：C++17
- **网络框架**：Muduo
- **数据库**：MySQL + MySQL Connector/C++
- **消息队列**：RabbitMQ + SimpleAmqpClient
- **AI 推理**：ONNX Runtime + OpenCV
- **HTTP 客户端**：libcurl
- **模型接入**：阿里百炼、豆包、百炼 RAG、百炼 MCP

## 项目结构

```
├── HttpServer/          # 自研 HTTP 框架
│   ├── include/http/    # HTTP 解析、请求、响应
│   ├── include/router/  # 路由分发
│   ├── include/session/ # 会话管理
│   └── src/             # 框架实现
├── AIApps/ChatServer/   # AI 应用层
│   ├── include/         # 业务头文件
│   │   ├── AIUtil/      # AI 策略、工具、语音、图像
│   │   └── handlers/    # HTTP 请求处理器
│   ├── src/             # 业务实现
│   └── resource/        # 配置、前端页面（含 kb.html 知识库上传）
└── CMakeLists.txt       # 构建配置
```

## 构建与运行

### 环境要求

- GCC 8+ (C++17)
- CMake 3.10+
- Muduo 网络库
- MySQL 8.0+ + MySQL Connector/C++
- RabbitMQ + SimpleAmqpClient
- ONNX Runtime
- OpenCV 4.x
- libcurl、OpenSSL

### 环境变量

```bash
export DASHSCOPE_API_KEY="sk-xxx"      # 阿里百炼
export DOUBAO_API_KEY="xxx"            # 火山豆包
export BAIDU_CLIENT_ID="xxx"           # 百度语音
export BAIDU_CLIENT_SECRET="xxx"       # 百度语音
export Knowledge_Base_ID="xxx"         # 百炼 RAG 应用 ID（聊天选「百炼RAG」时用）
export BAILIAN_WORKSPACE_ID="llm-xxx"  # 业务空间 ID（控制台左上角）
export BAILIAN_INDEX_ID="xxx"          # 知识库 IndexId（灌库用，不是应用 ID）
# export BAILIAN_CATEGORY_ID="default" # 可选，默认 default
```

### 编译运行

```bash
mkdir build && cd build
cmake ..
make -j4
./http_server -p 80
```

### 数据库初始化

```sql
CREATE DATABASE ChatHttpServer;

CREATE TABLE users (
    id INT AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(50) NOT NULL UNIQUE,
    password VARCHAR(50) NOT NULL
);

CREATE TABLE chat_message (
    id INT,
    username VARCHAR(50),
    session_id VARCHAR(100),
    is_user TINYINT(1),
    content TEXT,
    ts BIGINT,
    PRIMARY KEY (id, ts)
);
```
