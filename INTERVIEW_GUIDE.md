# CppAIService 面试技术准备文档

> 本文档汇总了 CppAIService 项目的 **8 个简历要点 + 15 个深度技术话题**，每块包含代码位置、工作原理、面试话术和可能被追问的问题。

---

## 目录

- [一、AI 对话系统集成](#一ai-对话系统集成)
- [二、AI 图像识别功能](#二ai-图像识别功能)
- [三、高并发 AI 推理服务](#三高并发-ai-推理服务)
- [四、服务化与部署](#四服务化与部署)
- [五、用户功能实现](#五用户功能实现)
- [六、策略模式 + 工厂模式设计](#六策略模式--工厂模式设计)
- [七、轻量级 MCP 服务设计](#七轻量级-mcp-服务设计)
- [八、语音识别与合成](#八语音识别与合成)
- [九、深度话题 1：细粒度锁优化](#九深度话题-1细粒度锁优化)
- [十、深度话题 2：分布式追踪](#十深度话题-2分布式追踪)
- [十一、深度话题 3：多租户安全](#十一深度话题-3多租户安全)
- [十二、深度话题 4：成本可观测 + 弹性伸缩 + 降级](#十二深度话题-4成本可观测--弹性伸缩--降级)
- [十三、深度话题 5：长文档解析入库（异步流水线）](#十三深度话题-5长文档解析入库异步流水线)
- [十四、深度话题 6：MCP 两段式的 JSON 稳定性](#十四深度话题-6mcp-两段式的-json-稳定性)
- [十五、深度话题 7：AIHelper 与工厂联动全链路](#十五深度话题-7aihelper-与工厂联动全链路)
- [十六、深度话题 8：messages 结构的 role 优化](#十六深度话题-8messages-结构的-role-优化)
- [十七、深度话题 9：MCP parseAIResponse 的 4 层兜底方案](#十七深度话题-9mcp-parseairesponse-的-4-层兜底方案)
- [十八、FAQ：面试最可能被问的 8 个高频问题](#十八faq面试最可能被问的-8-个高频问题)

---

## 一、AI 对话系统集成

### 简历原话

> "AI对话系统集成：在自研HTTP框架中嵌入多模型对话接口，支持多轮对话与会话记忆，实现用户上下文信息的持久化与同步，为用户提供个性化的智能交互体验。"

### 核心代码位置

| 模块 | 文件 | 作用 |
|------|------|------|
| 自研 HTTP 框架 | `HttpServer/` 整个目录 | 基于 Muduo 二次封装的 HTTP 服务 |
| 路由注册 | `ChatServer.cpp:117-145` | 13 个路由 Handler |
| 多模型接口 | `AIStrategy.h:13-34` | 5 个纯虚函数（策略基类） |
| 4 个具体策略 | `AIStrategy.h:36-117` | Aliyun / DouBao / RAG / MCP |
| 自动注册 | `AIStrategy.cpp:175-178` | 4 个 StrategyRegister |
| 多轮对话 | `AIHelper.cpp:22-30` | messages vector 不断 push_back |
| 会话记忆 | `ChatServer.h:101` | chatInformation[userId][sessionId] = AIHelper |
| 持久化 | `AIHelper.cpp:197-219` | pushMessageToMysql → RabbitMQ → MySQL |
| 启动恢复 | `ChatServer.cpp:53-103` | readDataFromMySQL |
| 异步写库 | `MQManager.cpp:4-22` | 5 连接池 + 轮询 publish |

### 工作流程

```
用户登录 → ChatLoginHandler 验证身份 → 创建 Session
  ↓
用户发消息 → ChatSendHandler → 加锁 → chatInformation[userId][sessionId] 找 AIHelper
  ↓
AIHelper::chat → 同步写内存（messages.push_back） → 异步写库（MQManager::publish）
  ↓
strategy->buildRequest() → 拼 JSON → executeCurl → 调外部 LLM
  ↓
strategy->parseResponse() → 抽 AI 回答 → 返回前端
```

### 面试话术

> "这个功能的核心是 5 块组合：自研 HTTP 框架（Muduo 二次封装，13 个路由）+ 策略模式（4 个厂商，5 个虚函数）+ 二级嵌套 map（多用户多会话隔离）+ messages 向量（多轮对话历史）+ 异步持久化（RabbitMQ 消息队列写库）。启动时从 MySQL 恢复历史，对话不丢失。"

### 可能追问

**Q1：自研 HTTP 框架怎么实现的？**
- 基于 Muduo TcpServer + EventLoop，自己实现了 Router、HttpContext 解析器、Middleware 链、Session 管理器。Muduo 提供的 Reactor 模式保证高并发 I/O 效率

**Q2：多轮对话怎么实现的？**
- `AIHelper` 内部 `messages` vector 不断 push 用户和 AI 的对话。每轮 buildRequest 把整个 vector 打包发给 LLM，LLM 就知道之前的上下文

**Q3：会话隔离怎么做的？**
- 二级嵌套 map：`unordered_map<userId, unordered_map<sessionId, shared_ptr<AIHelper>>>`。每个 userId 有独立的 session 集合，每个 session 有自己独立的消息历史

**Q4：服务器重启对话会丢吗？**
- 不会。聊天记录存 MySQL（`chat_message` 表），启动时 `readDataFromMySQL()` 读取并按 userId+sessionId 重建 chatInformation

---

## 二、AI 图像识别功能

### 简历原话

> "AI 图像识别功能：基于 ONNXRuntime + OpenCV 部署轻量级分类模型，完成端到端图像识别流程（数据预处理 → 推理调用 → 结果解析），在 1000 类有限数据集下实现高精度识别效果。"

### 核心代码位置

| 模块 | 文件 | 作用 |
|------|------|------|
| 类定义 | `ImageRecognizer.h:11-39` | ImageRecognizer 类 |
| 加载模型 | `ImageRecognizer.cpp:3-25` | 构造时加载 ONNX 模型 |
| 加载标签 | `ImageRecognizer.cpp:27-44` | 读 1000 个类别标签 |
| 数据预处理 | `ImageRecognizer.cpp:67-72` | resize + 归一化 + NCHW 转换 |
| 推理调用 | `ImageRecognizer.cpp:87-91` | session->Run() |
| 结果解析 | `ImageRecognizer.cpp:93-104` | argmax + labels 查表 |
| HTTP 入口 | `AIUploadSendHandler.cpp:39-65` | base64 解码 → PredictFromBuffer |

### 关键：数据预处理 3 步

```
cv::resize(img_raw, img, 224×224)    // 原图 → 224×224（L68）
img.convertTo(img, CV_32F, 1/255)    // 像素值 0-255 → 0-1（L69）
cv::dnn::blobFromImage(img, img)     // HWC → NCHW（L72）
```

> 任何一步做错，预测全错。因为必须和**模型训练时的输入格式匹配**。

### 推理调用：`session->Run` 一行搞定

```cpp
auto output_tensors = session->Run(Ort::RunOptions{nullptr},
    &input_name, &input_tensor, 1,      // 1 个输入
    &output_name, 1);                    // 1 个输出
// 返回 1000 个 float → argmax 找最大 → labels[下标] 查类别
```

### 面试话术

> "这个功能的本质是 **3 步流水线**：预处理（resize/归一化/NCHW）→ 推理（session->Run）→ 解析（argmax）。全程在 C++ 进程内完成，不上传图片到任何外部 API，这就是'端到端'的含义。用的是 MobileNetV2（~14MB），在 ImageNet 1000 类上 Top-5 准确率约 90%。"

### 可能追问

**Q1：为什么用 ONNX Runtime 而不是 PyTorch？**
- ONNX 是模型中间格式，一次转换任何框架都能跑。C++ 推理比 Python 快 10-100 倍。ONNX Runtime 还做算子融合等优化

**Q2：为什么用 MobileNetV2 而不是 ResNet？**
- 轻量级。ResNet-50 约 100MB，MobileNetV2 约 14MB。准确率差距不大（Top-1 76% vs 72%），但内存占用少得多

**Q3：confidence 为什么写死了 0.95？**
- 这是遗留问题（TODO 没做）。正确做法：`confidence = output_data[pred_class]` 取真实概率值。面试时应主动承认这个 bug，并给改进方案

**Q4：图像格式不匹配怎么办（如灰度图）？**
- `cv::imdecode` 用了 `IMREAD_COLOR` 参数，灰度图会自动变成 3 通道伪彩色图，兼容处理

### 深度学习 QA：模型、标签、OpenCV 的角色

#### Q5：模型是自己训练的还是别人训练好的？

**别人训练好的。** 项目用的是 Google 在 ImageNet 数据集上预训练的 MobileNetV2，导出成 ONNX 格式后直接加载使用。

模型路径写死在代码里（`AIUploadSendHandler.cpp:33`）：

```cpp
std::make_shared<ImageRecognizer>("/root/models/mobilenetv2/mobilenetv2-7.onnx")
```

加载到内存（`ImageRecognizer.cpp:11`）：

```cpp
session = std::make_unique<Ort::Session>(env, model_path.c_str(), session_options);
```

#### Q6：只能识别 1000 类吗？

**是的。** ImageNet 数据集定义了固定的 1000 个类别，模型输出 1000 个浮点数（每个代表"像这一类"的概率），argmax 取最大的下标。

`ImageRecognizer.cpp:96-97`：

```cpp
int num_classes = labels.empty() ? 1000 : (int)labels.size();
int pred_class = std::max_element(output_data, output_data + num_classes) - output_data;
```

如果用户上传了不在 1000 类里的图片（如动漫人物），模型也会**强行归到概率最高的那一类**——结果是错的。这是 ImageNet 分类模型的天然限制。

#### Q7：labels 在哪里存的？

两处：

**① 磁盘文件**：`/root/imagenet_classes.txt`，每行一个类别名，共 1000 行

**② 内存 vector**：`ImageRecognizer.cpp:27-37` 启动时逐行读入

```cpp
void ImageRecognizer::LoadLabels(const std::string& label_path) {
    std::ifstream infile(label_path);
    std::string line;
    while (std::getline(infile, line)) {
        if (!line.empty()) { labels.push_back(line); }
    }
}
```

加载完后 `labels[207] = "golden retriever"`——模型返回下标 207，就取这个。

#### Q8：OpenCV 在项目里做了什么？

**只做预处理，不参与推理。** 4 步全在 `ImageRecognizer.cpp:55-72`：

| 步骤 | 代码 | 作用 |
|------|------|------|
| 解码 | `cv::imdecode(data, IMREAD_COLOR)` | base64 字节流 → OpenCV 图片 |
| 缩放 | `cv::resize(img, 224, 224)` | 任意尺寸 → 224×224（模型只认这个） |
| 归一化 | `img.convertTo(CV_32F, 1/255)` | 像素 0-255 → 浮点数 0-1 |
| NCHW 转换 | `cv::dnn::blobFromImage(img)` | HWC → NCHW（OpenCV 和 ONNX 的内存布局不同） |

真正的推理是 **ONNX Runtime** 做的：`session->Run()` 一行（`ImageRecognizer.cpp:87`）。OpenCV 预处理完后把数据交给 ONNX Runtime，自己不跑模型。

---

## 三、高并发 AI 推理服务

### 简历原话

> "高并发 AI 推理服务：依托 Muduo 网络库与线程池实现高并发请求处理，将 AI 聊天与图像识别任务分发至 RabbitMQ 消息队列，异步执行数据库写入，避免主线程阻塞，显著提升系统吞吐量与响应性能。"

### ⚠️ 诚实提醒

简历话术有**美化**：AI 聊天和图像识别**没有**走 RabbitMQ——它们是**同步阻塞**的（`curl_easy_perform` 卡住工作线程）。**只有 MySQL 写入走了 RabbitMQ**。

### 真实架构

```
Muduo（4 个 EventLoop 线程） → 接收 HTTP 请求
  ↓
Handler 同步调 AI 推理 / 图像识别（阻塞 2-5 秒）
  ↓
MQManager::publish("sql_queue", INSERT_SQL) → 0.1ms 返回
  ↓
RabbitMQThreadPool（2 个后台线程） → 消费 SQL → 写 MySQL
```

### 核心代码位置

| 模块 | 文件 | 作用 |
|------|------|------|
| 线程池 | `main.cpp:43` | `server.setThreadNum(4)` |
| MQ 发布 | `MQManager.cpp:15-22` | 5 连接池 + 原子计数器轮询 |
| MQ 消费 | `MQManager.cpp:26-67` | 2 线程死循环 + Qos(1) |
| 业务函数 | `main.cpp:15-18` | `executeMysql(sql)` |

### 面试话术

> "这个项目的并发分两层：Muduo 多 EventLoop（`setThreadNum(4)`）处理 HTTP 请求，RabbitMQ（`MQManager`）处理异步写库。HTTP 响应不阻塞 DB I/O——一次 PRAGMA INSERT 只需要 0.1ms 就发到队列。真实情况是 AI 推理同步阻塞（因为 libcurl 是同步的），但通过 4 的工作线程实现了粗粒度的并发。如果把 AI 推理也异步化，需要从'同步返回'改成'先返回 taskId + 前端轮询 / WebSocket 推送'。"

### 可能追问

**Q1：怎么把 AI 推理也改成异步？**
- 方案：Handler 生成 taskId → publish 到 `ai_task_queue` → 立即返回 202 → 3 个 AI worker 消费队列 → 结果存 Redis → 前端轮询 `/chat/result?taskId=xxx`

**Q2：MQ 连接池为什么用 5 个连接？**
- 避免单连接热点。`counter_.fetch_add(1) % 5` 轮询选连接，每个连接独立一把锁。多线程同时 publish 不会全卡在一个连接上

**Q3：BasicQos(1) 有什么用？**
- 每次只取 1 条。防止某个 worker 一次性抢走 100 条导致其他 worker 空闲（不公平分配问题）

---

## 四、服务化与部署

### 简历原话

> "服务化与部署：利用 Docker 构建统一运行环境，整合 MySQL、RabbitMQ 等组件，实现一键部署与快速迁移，提升系统可维护性与扩展性。"

### ⚠️ 诚实提醒

**项目里一个 Dockerfile 都没有**。`CMakeLists.txt` 里全是硬编码路径（`/usr/include/mysql-cppconn-8` 等），换台 Linux 发行版就装不上。

### 给面试话术建议

> "我们项目平时在开发环境中是手动安装依赖的。如果要 Docker 化，方案是用**多阶段构建**——第一阶段编译（装 g++、cmake、Muduo、ONNX Runtime），第二阶段只装运行时库。最后镜像约 1.5GB。用 docker-compose 整合 chat_app、mysql:8.0、rabbitmq:3-management 三个服务，MySQL 第一次启动时自动执行 init.sql 建表。部署时间从 1-2 小时降到 5 分钟。"

### 三个关键文件（如果做 Docker 化）

```
Dockerfile              ← 多阶段构建（ONNX 走预编译包避免编译 30 分钟）
docker-compose.yml      ← 4 个服务（chat_app + mysql + rabbitmq + redis） + depends_on + Volume 持久化
init.sql               ← 自动建表 + 插测试用户
.env                   ← API Key（不入 git）
```

### 可能追问

**Q1：多阶段构建的优势？**
- 第一阶段（builder 2-3GB）→ 第二阶段（runtime 600MB-1.5GB）。省空间 + 小攻击面 + 没有编译器

**Q2：Volume vs 容器内存储？**
- 容器内存储 = 删掉容器数据就没了。Volume 持久化到宿主机

**Q3：网络模式？**
- 开发用 bridge 够用。生产用 K8s CNI（Calico/Flannel）或 Docker overlay

---

## 五、用户功能实现

### 简历原话

> "用户功能实现：实现用户登录、注册、退出及历史记录同步功能，基于自研 Session 机制完成用户状态校验与持久化管理。"

### ⚠️ 诚实提醒

"持久化管理"有夸大：Session 存在内存（MemorySessionStorage），进程重启就丢。但 `SessionStorage` 是抽象基类，加个 `RedisSessionStorage` 就能平滑切换。

### 核心代码位置

| 模块 | 文件 | 作用 |
|------|------|------|
| Session 类 | `Session.h:16-44` | data_ KV map + expiry + refresh |
| SessionStorage | `SessionStorage.h:10-28` | 抽象基类 + 内存实现 |
| SessionManager | `SessionManager.h:14-41` | 管理器：getSession / destroy / 生成 ID |
| getSession | `SessionManager.cpp:18-43` | 核心：Cookie → 加载/创建 → 续命 |
| Cookie 读写 | `SessionManager.cpp:71-102` | getSessionIdFromCookie + setSessionCookie |
| 生成 ID | `SessionManager.cpp:46-57` | 32 位随机十六进制（128bit 熵） |
| 登录 | `ChatLoginHandler.cpp` | queryUserId + setValue("isLoggedIn", "true") |
| 注册 | `ChatRegisterHandler.cpp` | insertUser（⚠️ SQL 注入风险） |
| 退出 | `ChatLogoutHandler.cpp` | clear() + destroySession + erase onlineUsers_ |
| 历史记录 | `ChatHistoryHandler.cpp` | chatInformation[userId][sessionId] → messages |
| 防重复登录 | `ChatServer.h:94-95` | onlineUsers_ map + mutexForOnlineUsers_ |

### 面试话术

> "Session 是**完全自研**的，3 层架构。`Session` 是 KV 小票（data_ map + expiryTime_），`SessionStorage` 是抽象存储后端（内存实现 + 未来可换 Redis），`SessionManager` 管理整个生命周期（从 Cookie 取 ID → 加载/创建 → refresh 续命 → 返回）。登录时 `setValue("isLoggedIn", "true")`，其他 Handler 入口先查这个字段。额外有 `onlineUsers_` 防重复登录。"

### 可能追问

**Q1：怎么防止重复登录？**
- `onlineUsers_` 是 `unordered_map<userId, bool>`。登录时检查：如果 `onlineUsers_[userId]==true` 返回 403

**Q2：Session 怎么和 JWT 比较？**
- Session：服务端状态（可以随时撤销 `destroySession`）。JWT：无状态（撤销麻烦）。本项目用 Session 因为会话管理简单

**Q3：`cleanExpiredSessions` 为什么是空的？**
- 这是个遗留的 TODO。应该加一个定时器后台清理过期 session

**Q4：SQL 注入风险在哪？**
- `ChatRegisterHandler.cpp:50` 拼接 SQL（`"INSERT INTO ... VALUES ('" + username + "', ..."`）。应该用预编译参数（像 `ChatLoginHandler` 那种）

---

## 六、策略模式 + 工厂模式设计

### 简历原话

> "策略模式 + 工厂模式设计：支持多模型动态切换（RAG、百炼、豆包等），通过策略模式抽象不同模型的对话行为，提升模块解耦性与扩展性；结合工厂模式实现模型动态实例化，使新增模型无需修改主逻辑，增强系统的可维护性与可插拔性。"

### 核心代码位置

| 模块 | 文件 | 作用 |
|------|------|------|
| 抽象基类 | `AIStrategy.h:13-34` | 5 个纯虚函数 |
| 4 个子类 | `AIStrategy.h:36-117` | Aliyun / DouBao / RAG / MCP |
| 工厂单例 | `AIFactory.h:14-35` | creators hash 表 |
| 工厂实现 | `AIFactory.cpp:7-22` | instance + register + create |
| 自动注册 | `AIFactory.h:42-50` | StrategyRegister<T> 模板 |
| 注册行为 | `AIStrategy.cpp:175-178` | 4 个 static 变量 |

### 4 个厂商的 6 大差异

| 维度 | 阿里百炼 | 豆包 | 阿里 RAG | 阿里 MCP |
|------|:---:|:---:|:---:|:---:|
| **URL** | dashscope/chat/completions | volces/chat/completions | dashscope/apps/{ID}/completion | dashscope/chat/completions |
| **Key** | DASHSCOPE_API_KEY | DOUBAO_API_KEY | DASHSCOPE_API_KEY | DASHSCOPE_API_KEY |
| **Model** | qwen-plus | doubao-seed | "" | qwen-plus |
| **JSON 结构** | `payload.messages` | `payload.messages` | `payload.input.messages` + `parameters` | `payload.messages` |
| **响应路径** | `choices[0].message.content` | `choices[0].message.content` | `output.text` | `choices[0].message.content` |
| **isMCPModel** | ❌ | ❌ | ❌ | ✅ |
| **上下文窗口** | 32K | 128K | 32K | 32K |

### messages 的 role 优化

**当前问题**：`AIHelper.h:68` 用 `vector<pair<string, long long>>` —— role 靠 `i % 2 == 0` 猜（4 个 buildRequest + ChatHistoryHandler 共 5 处 `% 2`）。MCP 异常时 push/pop 不配对会永久破坏 messages。

**优化方案**：改成 `struct ChatMessage { string role; string content; long long ts; }`。

- `addMessage` 里显式设 `role = "user/assistant/system"`
- 4 个 buildRequest 里 12 行 `i % 2` 全删，改 `msg["role"] = m.role`
- MCP 临时 prompt 改用独立 vector，不污染 messages

### 面试话术

> "我们用**策略 + 工厂 + 自动注册**三件套管理多模型。`AIStrategy` 抽象的 5 个纯虚函数（getApiUrl / getApiKey / getModel / buildRequest / parseResponse）把 URL、密钥、JSON 格式、响应解析全部封装。`StrategyFactory` 是单例，核心是 `creators` hash 表（key 是模型名，value 是造模型的 lambda）。`StrategyRegister<T>` 模板加一行 static 就能注册新厂商——程序启动时自动把造 X 的方法挂到工厂里。**加新模型只要 2 步**——写新类继承 AIStrategy + 加一行 `static StrategyRegister<NewModel>("name")`。AIHelper 主体只调 5 个虚函数，完全不知道是哪家厂商。"

### 可能追问

**Q1：为什么用 lambda 存注册信息而不是直接存实例？**
- 用时才造（节省内存）。没用的模型不创建

**Q2：static 初期化顺序问题？**
- 用 Meyers 单例（函数内部 static）规避了静态初始化顺序问题。C++11 后线程安全

**Q3：4 个 buildRequest 高度重复怎么优化？**
- 阿里百炼和豆包几乎一字不差。可以抽一个基类方法做共享，用模板方法模式

---

## 七、轻量级 MCP 服务设计

### 简历原话

> "轻量级 MCP 服务设计：参考 Model Context Protocol 思想，封装自定义工具调用机制，实现服务端工具注册、提示词构建与调用流程，为语音识别等新功能扩展预留接口能力。"

### 核心代码位置

| 模块 | 文件 | 作用 |
|------|------|------|
| 出题模板 | `config.json` | prompt_template + tools 列表 |
| AIConfig 类 | `AIConfig.h:12-38` | 出题 + 阅卷 |
| 加载配置 | `AIConfig.cpp:3-35` | loadFromFile |
| 拼工具清单 | `AIConfig.cpp:37-50` | buildToolList |
| 拼 Prompt | `AIConfig.cpp:52-57` | buildPrompt |
| 阅卷 | `AIConfig.cpp:59-78` | parseAIResponse |
| 拼第 2 段 Prompt | `AIConfig.cpp:80-93` | buildToolResultPrompt |
| 工具箱 | `AIToolRegistry.h:11-28` | registerTool / invoke / hasTool |
| 2 个内置工具 | `AIToolRegistry.cpp:37-89` | getWeather（调 wttr.in）/ getTime（读系统时钟） |
| MCP 两段式 | `AIHelper.cpp:55-118` | 第 1 段：决定 → 第 2 段：组织答案 |

### MCP 两段式流程

```
第 1 段：AI 决定要不要调工具
  buildPrompt → 拼 "用户问题 + 工具清单 + 强约束措辞 + JSON 示例"
  executeCurl → 调阿里百炼
  parseAIResponse → 阅卷（是工具调用？）
    ↓ YES
调用工具：registry.invoke(toolName, args)
    ↓
第 2 段：拿工具结果再问 AI
  buildToolResultPrompt → "用户问了 X，工具返回了 Y，请组织答案"
  executeCurl → 第 2 次调阿里百炼
  parseResponse → 最终自然语言答案
```

### 加新工具只需 3 步

```
第 1 步：写工具函数（在 AIToolRegistry.cpp 或新文件）
第 2 步：在 AIToolRegistry 构造里 registerTool("asr_recognize", asrRecognize)
第 3 步：在 config.json 加工具描述
```

> AIHelper 一行不用改——这就是"预留接口"的含义。

### 面试话术

> "我们参考了 MCP 的思想，做了一套轻量级工具调用机制。核心两段式：第 1 段模型判断要不要工具（通过 Prompt 里的强约束措辞 + JSON 示例），第 2 段根据工具结果生成自然语言回答。`AIConfig` 负责出题和阅卷，`AIToolRegistry` 是个 hash 表存工具函数。加新工具只要注册函数 + 改 config.json，AIHelper 主体零改动。"

### 可能追问

**Q1：怎么保证 AI 稳定输出 JSON？**
- 3 个手段：强约束措辞（"只输出 JSON，其他任何内容不需要输出"）、少样本示例（给 JSON 格式）、反例。业界经验 85-95% 一次听话

**Q2：超出 95% 的部分怎么兜底？**
- 项目没做但应该做：正则抠 JSON（防 ```json``` 包裹）+ Schema 校验工具名/参数 + 1-2 次重试 + 失败降级为普通回答

**Q3：标准 MCP 和本项目的区别？**
- 标准 MCP 走 JSON-RPC over stdio/HTTP。本项目用它的思想（AI 决定 + 工具调用 + 二次推理），但消息传发用 HTTP / OpenAI 兼容格式

---

## 八、语音识别与合成

### 简历原话

> "语音识别与合成 (ASR + TTS)：集成百度语音识别与合成 API，实现语音输入转文本、文本转语音输出，支持异步任务轮询与结果回传，为系统提供完整语音交互能力。"

### ⚠️ 诚实提醒

- ASR 函数存在（`recognize`）但**没有 HTTP 接口**（无 Handler 调用它）
- TTS 有接口 `/chat/tts`，但代码有 2 个编译错误：
  1. `::string clientSecret` 应为 `std::string`
  2. `speechUrl` 变量未定义，应为 `stdspeechUrl`

### 核心代码位置

| 模块 | 文件 | 作用 |
|------|------|------|
| AISpeechProcessor | `AISpeechProcessor.h:17-43` | 百度 TTS/ASR 封装 |
| 获取 token | `AISpeechProcessor.cpp:12-48` | getAccessToken |
| ASR 实现 | `AISpeechProcessor.cpp:52-109` | recognize |
| TTS create | `AISpeechProcessor.cpp:114-184` | synthesize 第 1 步 |
| TTS query 轮询 | `AISpeechProcessor.cpp:186-241` | synthesize 第 2 步（60 次循环） |
| HTTP 接口 | `ChatSpeechHandler.cpp` | `/chat/tts`（⚠️ 有 bug） |

### TTS 两段式

```
第 1 步：create — POST /tts/v1/create → 返回 task_id
第 2 步：query 轮询（最多 60 次，每 1s 一次）→ 拿到 speech_url（MP3 URL）
```

### 面试话术

> "我们用百度语音 API 做 ASR + TTS，自研 `AISpeechProcessor` 类封装。TTS 的 HTTP 接口存在（`/chat/tts`），走 create+query 两步——因为百度 TTS 是异步任务模型（提交任务 → 等处理 → 拿结果）。ASR 函数写了但还没接 Handler。TTS Handler 有 2 个编译错误是项目遗留问题，应该主动承认。"

### 可能追问

**Q1：token 30 天过期怎么处理？**
- 目前没做。应该加"检测到 401 自动重新获取 token"的机制

**Q2：60 秒轮询太慢怎么办？**
- 应该改成异步架构——先返回 `202 + taskId`，前端轮询 `/chat/tts/result`，或者用 WebSocket 推送

---

## 九、深度话题 1：细粒度锁优化

### 问题
`mutexForChatInformation` 是一把全局互斥锁，保护整个 `chatInformation` 嵌套 map。

### 当前代码

`ChatServer.h:101-102`：
```cpp
unordered_map<int, unordered_map<string, shared_ptr<AIHelper>>> chatInformation;
mutex mutexForChatInformation;
```

### 3 种优化方案

| 方案 | 复杂度 | 效果 |
|------|--------|------|
| `shared_mutex`（读写锁） | 低 | 读并发（GetMessages 并行） |
| 分段锁（遮阳 bucket lock） | 中 | 不同 userId 完全并发 |
| per-session mutex（最细） | 高 | 跨 session 零冲突 |

### 推荐方案：shared_mutex

#### shared_mutex 是什么？

`std::shared_mutex` 就是**读写锁**。一把锁，两种模式：

```
┌──────────────────────────────────────┐
│         std::shared_mutex            │
│                                      │
│  共享模式（读）：多个人可以同时拿     │
│  独占模式（写）：只能一个人拿         │
│                                      │
│  共享和独占不能同时存在               │
└──────────────────────────────────────┘
```

**生活比喻**：图书馆的阅览室。

| 操作 | 比喻 | 几个人能同时干 |
|------|------|:---:|
| 共享模式（读） | 进去看书 | 多个人，不冲突 |
| 独占模式（写） | 进去换书架 | 只能 1 个人 |

#### shared_lock 和 unique_lock 分别是什么意思？

```cpp
// 读路径：申请"看书证"
std::shared_lock<std::shared_mutex> lock(mutex);
```

**大白话**：跟管理员说"我要进去看书"——只要现在没人换书架，就放你进去。**同时可以有 10 个人拿看书证**。

```cpp
// 写路径：申请"换书架证"
std::unique_lock<std::shared_mutex> lock(mutex);
```

**大白话**：跟管理员说"我要进去换书架"——**等所有人都出来**才放你一个人进去。**期间任何人不许进**。

#### 为什么这么定义？

C++ 标准库把"锁"和"锁的模式"分开了：

| 组件 | 作用 |
|------|------|
| `std::shared_mutex` | 锁本身（图书馆大门） |
| `std::shared_lock` | 共享模式 RAII 包装（看书证） |
| `std::unique_lock` | 独占模式 RAII 包装（换书架证） |

RAII：构造时自动加锁，析构时自动解锁。花括号结束，`lock` 对象销毁，锁自动释放。

#### 具体在项目里改哪些地方？

**4 个文件需要改：**

**第 1 步：改头文件声明**

`ChatServer.h:102`：
```cpp
// 原来
std::mutex mutexForChatInformation;

// 改成
#include <shared_mutex>
std::shared_mutex mutexForChatInformation;
```

**第 2 步：读路径——`ChatHistoryHandler.cpp:39`（纯读）**
```cpp
// 原来
std::lock_guard<std::mutex> lock(server_->mutexForChatInformation);

// 改成
std::shared_lock<std::shared_mutex> lock(server_->mutexForChatInformation);
```
**原因**：拉历史只读不写，多人同时拉不冲突。

**第 3 步：写路径——`ChatSendHandler.cpp:45`（可能写）**
```cpp
// 原来
std::lock_guard<std::mutex> lock(server_->mutexForChatInformation);

// 改成
std::unique_lock<std::shared_mutex> lock(server_->mutexForChatInformation);
```
**原因**：Handler 里 `if (find == end()) { emplace(...) }` 可能写入，需要独占锁。

**第 4 步：写路径——`ChatCreateAndSendHandler.cpp:48`（肯定写）**

同上，改成 `std::unique_lock<std::shared_mutex>`。

**不需要改的**：`ChatSessionsHandler.cpp:32` 用的是 `mutexForSessionsId`，不是这把锁。

#### 改前改后对比

```
改前（mutex）：
  线程A 拉历史 ──→ 锁 ──→ 读 ──→ 解锁
  线程B 拉历史 ──→ 等A解锁…… ──→ 读 ──→ 解锁   ← B 白白等
  线程C 发消息 ──→ 等B解锁…… ──→ 写 ──→ 解锁   ← C 也等

改后（shared_mutex）：
  线程A 拉历史 ──→ shared_lock ──→ 读 ──→ 解锁
  线程B 拉历史 ──→ shared_lock ──→ 读 ──→ 解锁   ← A和B同时！
  线程C 发消息 ──→ 等A、B都解锁 → unique_lock → 写 → 解锁
```

#### 面试话术

> "把全局 `std::mutex` 改成 `std::shared_mutex` 是最低成本的优化。读路径（拉历史）用 `shared_lock`，多人可并行；写路径（创建新会话）用 `unique_lock`，独占。项目中拉历史的频率远高于创建新会话，大部分场景没有锁竞争。改动只涉及 4 个文件，风险可控。"

### 并发写同一会话如何避免乱序

| 方案 | 做法 |
|------|------|
| 单调 seqId | `atomic<uint64_t>` 递增，读端按 seqId 排序 |
| per-session mutex | 每个 AIHelper 内部加 `std::mutex` 串行化写入 |
| 时间戳 + seqId 兜底 | 先按 ts 排，ts 相同按 seqId 排 |
| MPSC 单写者队列 | 写入请求入队，单线程消费保证有序 |

---

## 十、深度话题 2：分布式追踪

### 现状

项目中**没有任何追踪能力**。搜索 `trace|TraceId|span|SpanId|x-request|requestId|correlation` 返回 0 个匹配。

### 改造方案：TraceContext + thread_local

**新建 TraceContext.h**：
```cpp
namespace tracing {
struct TraceContext {
    std::string traceId;      // 全链路唯一（32hex）
    std::string spanId;       // 当前 span（16hex）
    std::string parentSpanId; // 上游 span
    int userId;
    std::string sessionId;
};
extern thread_local TraceContext currentCtx;
}
```

**在每个 Handler 入口生成根 span**，在关键节点（buildPrompt / executeCurl / invokeTool / synthesize）开子 span，记录 `duration_ms` + `status_code`。

**效果**：同一条 `traceId` 串起从用户请求 → RAG → LLM → 工具 → TTS 的完整链路

### TTS 跨请求关联

TTS 是独立 HTTP 请求。需要前端透传 `X-Trace-Id` + `X-Span-Id` HTTP Header

---

## 十一、深度话题 3：多租户安全

### 现状

项目**完全没有多租户**。所有用户共享同一个 `DASHSCOPE_API_KEY`，同一个 `Knowledge_Base_ID`

### 4 大洞

| 问题 | 代码位置 | 风险 |
|------|---------|------|
| API Key 共享 | `AIStrategy.h:39-104`，硬编码读环境变量 | 无法计费、无法限流 |
| RAG 数据域共享 | `AIStrategy.cpp:91`，硬编码读 `Knowledge_Base_ID` | 全公司查同一库 |
| 会话越权 | `ChatSendHandler.cpp:47`，只按 userId 索引，不校验 sessionId 归属 | 横向越权偷看历史 |
| PDF/HTML 上传清洗 | `AIUploadSendHandler.cpp:39-55`，根本不支持 PDF | 无白名单、无魔数嗅探、无脚本过滤 |

### 改造方案（按性价比排序）

| 顺序 | 改造项 | 耗时 |
|------|--------|------|
| 1 | 会话越权防护（加 10 行检查 sessionId 归属） | 10 分钟 |
| 2 | API Key 存数据库（users 表加 api_key 列） | 半天 |
| 3 | RAG 知识库加 owner 字段 | 半天 |
| 4 | 文件上传清洗（后缀白名单 + 魔数嗅探 + HTML 脚本过滤） | 1-2 天 |

---

## 十二、深度话题 4：成本可观测 + 弹性伸缩 + 降级

### 现状

项目**三件事都没有做**。搜索 `prometheus/grafana/metric/cost/token/降级/degradation/circuit/breaker/限流` 返回 0 个匹配。

### 成本可观测（最小可行版本，1 天）

1. 新建 `CostMetrics.h`：`record(model, input_tokens, output_tokens, duration_ms)`
2. 改 `executeCurl`：解析 `response["usage"]` 拿 token 数 → 调用 `CostMetrics::record`
3. 新增 `/admin/metrics` Handler：输出 Prometheus 格式给 Grafana 画图

### 弹性伸缩

| 档次 | 方案 |
|------|------|
| 1 档（简单） | 无状态化（chatInformation 从 MySQL 读） + Supervisor 多进程 + Nginx 负载均衡 |
| 2 档（专业） | Docker + K8s Deployment + HPA（CPU > 60% 自动扩容） |

### 降级（3 段式）

```
模型降级：豆包挂了 → 切阿里百炼 → 报错"系统繁忙"
RAG 降级：检索超时（5s 超时）→ 关掉检索 → 当普通 LLM 回答
TTS 降级：百度 TTS 挂了 → 返回纯文本 → 前端用浏览器 SpeechSynthesis API
```

**开关化管理**：用 `runtime_config.json` 控制 `allow_models / allow_rag / allow_tts / daily_budget`

### 改造性价比排序

| 顺序 | 改造项 | 耗时 | 
|------|--------|------|
| 1 | CostMetrics 埋点 + Prometheus | 1 天 |
| 2 | 三段式模型降级 | 半天 |
| 3 | BudgetManager 配置化开关 | 半天 |
| 4 | 无状态化 + 多进程 | 2-3 天 |
| 5 | K8s HPA | 2-3 天 |

---

## 十三、深度话题 5：长文档解析入库（异步流水线）

### 现状

项目**完全没有** PDF/HTML 解析、Embedding、向量数据库。但有 **MQ 基建**（`MQManager` + `RabbitMQThreadPool`）可以直接复用。

### 3 段流水线

```
文档上传 → queue1(doc_task_queue)     → 消费者A：解析（流式 + 分块）
         → queue2(chunk_queue)       → 消费者B：调 Embedding API
         → queue3(vector_queue)      → 消费者C：写向量库
```

### 需要新增的能力

| 能力 | 工具 | 备注 |
|------|------|------|
| 流式解析 | poppler-cpp（PDF）+ gumbo（HTML） | 按页处理，不囤积内存 |
| 文本分块 | 2-8KB + 50-200 token overlap | 在句子边界切 |
| 批量 Embedding | 阿里 text-embedding-v2 | 批量 16-32 条，比单条快 15 倍 |
| 限流 | TokenBucket 类 | 防阿里云限流掐断 |
| 失败重试 | BasicReject + requeue | 当前 worker 失败也 BasicAck（已漏） |
| 断点续传 | doc_progress MySQL 表 | parsedChunks / embeddedChunks |
| 幂等 | docId + chunkIndex + checksum | 同一 chunk 重处理不重复 |

### 失败重试改造

当前 `MQManager::worker` 第 58 行：
```cpp
channel->BasicAck(env);   // ← 不管成功失败都确认，消息丢了！
```

应该改成：
```cpp
try {
    handler_(msg);
    channel->BasicAck(env);               // 成功才确认
} catch (...) {
    json j = json::parse(msg);
    if (j["retryCount"] >= 3) {
        MQManager::publish("dlq", msg);    // 3 次失败 → 死信队列
    } else {
        j["retryCount"] = j["retryCount"]+1;
        channel->BasicReject(env, true);    // 重新入队
    }
}
```

---

## 十四、深度话题 6：MCP 两段式的 JSON 稳定性

### 现状

`AIConfig::parseAIResponse` 只 `json::parse` 一下（L63），AI 输出 ` ```json...``` ` 包裹或带废话前缀都会失败。**5-15% 的请求会翻车**。

### 4 层兜底

| 层 | 做法 | 成功率 |
|----|------|--------|
| 1 直接 parse | `json::parse(response)` | 85-95% |
| 2 正则抠 JSON | 抠 ` ```json...``` ` 代码块 | → 99%+ |
| 3 JSON Schema 校验 | `validateCall` 检查工具名/参数 | 防误判 |
| 4 重试 + 失败日志 | 失败 2-3 次重试 + 原文写 dead_letter | 防丢失 |

### 改造后 parseAIResponse

```cpp
json j;
try {
    j = json::parse(response);                      // 情况 1
} catch (...) {
    std::regex jsonBlock(R"(```(?:json)?\s*(\{.*?\})\s*```)", std::regex::icase);
    std::smatch m;
    if (std::regex_search(response, m, jsonBlock)) {
        try { j = json::parse(m[1].str()); } catch (...) {}  // 情况 2
    }
    if (j.is_null()) {
        auto first = response.find('{'); auto last = response.rfind('}');
        if (first != npos && last != npos && last > first) {
            try { j = json::parse(response.substr(first, last-first+1)); } catch (...) {}  // 情况 3
        }
    }
}
```

> 成功率从 85% 提升到 **99%+**

---

## 十五、深度话题 7：AIHelper 与工厂联动全链路

### 问题

> 前端换模型的时候，后端就是根据 modelType 创建对应的 class？每次都创建实例吗？注册到工厂这个概念怎么理解？

### 答案拆开讲

#### Q1：每次都要 new 一个策略实例吗？

**是的，每次调用 `AIHelper::chat` 都 new 一个。**

`AIHelper.cpp:38-41`：

```cpp
std::string AIHelper::chat(..., std::string modelType) {
    // 设置策略
    setStrategy(StrategyFactory::instance().create(modelType));  // ← 每次都 create
```

`AIFactory.cpp:17-22`：

```cpp
std::shared_ptr<AIStrategy> StrategyFactory::create(const std::string& name) {
    auto it = creators.find(name);
    if (it == creators.end()) throw ...;
    return it->second();               // ← 调 lambda → make_shared<T>() → 真的 new
}
```

**为什么每次都 new？不是浪费吗？**

| 原因 | 说明 |
|------|------|
| 对象很小 | 一个 `AliyunStrategy` 只有 1 个 `string` 成员（`apiKey_`），约 32 字节 |
| 自动回收 | `shared_ptr` 在 `chat` 函数退出时引用计数归零，自动析构，不泄漏 |
| 开销可忽略 | 32 字节 × 1000 次/秒 = 32KB/秒 |
| 避免加锁 | 复用需要缓存 + `shared_ptr` 引用计数 + 多线程安全加锁，**比直接 new 复杂得多** |

**能不能改成复用？**可以但没必要：

```cpp
// 假设加缓存
static unordered_map<string, shared_ptr<AIStrategy>> cache;
// 问题 1：apiKey 是读环境变量，换了还得重新读，没省多少
// 问题 2：多线程安全要加读写锁 → 比 new 32 字节复杂 10 倍
// 结论：不要优化！
```

#### Q2：每次都把整个 messages 全部发给 AI 吗？

**是的。`buildRequest` 遍历整个 messages vector 打包成 JSON。**

`AIHelper.cpp:46-47`：

```cpp
addMessage(userId, userName, true, userQuestion, sessionId); // ① 新消息先 push
json payload = strategy->buildRequest(this->messages);        // ② 整个 messages 全发给 LLM
```

`AIStrategy.cpp:18-36`（以阿里百炼为例）：

```cpp
for (size_t i = 0; i < messages.size(); ++i) {  // ← 从 0 到末尾，全部遍历
    json msg;
    if (i % 2 == 0) msg["role"] = "user";
    else            msg["role"] = "assistant";
    msg["content"] = messages[i].first;
    msgArray.push_back(msg);
}
payload["messages"] = msgArray;  // ← 全部塞进 payload
```

**问题**：messages 太长（聊了 100 轮有 200 条）会超上下文窗口——阿里百炼 32K、豆包 128K。**目前没处理超长截断，要给 `AIStrategy` 加 `maxContextTokens()` 虚函数。**

#### Q3：注册到工厂就是 key-value 对应吗？

**对，就是 `unordered_map<string, 函数>`。本质是一个 hash 表：**

```cpp
creators = {
    "1" → 造 AliyunStrategy 的 lambda
    "2" → 造 DouBaoStrategy 的 lambda
    "3" → 造 AliyunRAGStrategy 的 lambda
    "4" → 造 AliyunMcpStrategy 的 lambda
}
```

**全流程**：

```
程序启动时（main 之前）
  ↓ 4 个 static StrategyRegister<T> 变量被构造
  → 每个构造里调 factory.registerStrategy("1", lambda)
  → creators["1"] = lambda

运行时
  ↓ 前端传 modelType="2"
  ↓ AIHelper::chat → factory.create("2")
  ↓ 查 creators["2"] → 找到 lambda → 调它 → make_shared<DouBaoStrategy>()
  ↓ 返回豆包策略

加新厂商
  ↓ 写新类 XunFeiStrategy
  ↓ 加一行 static StrategyRegister<XunFeiStrategy> reg("5");
  ↓ creators["5"] 自动多一个 lambda
  ↓ 前端传 "5" 自动走讯飞，AIHelper 一行不改
```

### 面试话术

> "工厂就是一个 hash 表，key 是模型编号（"1"/"2"/"3"/"4"），value 是造模型的函数。注册就是往里塞，创建就是拿出来调。每次 chat 都 new 一个策略对象，因为一个策略只有 ~32 字节，shared_ptr 在函数退出时自动回收，不需要缓存优化。加新模型只需写新类 + 一行 `static StrategyRegister<New>("5")`。"

---

## 十六、深度话题 8：messages 结构的 role 优化

### 问题

> 前面提到的如果要优化根据奇偶判断是用户还是 AI，这个就是 message 的结构变了，每条消息加上 key 是用户还是 AI 对吧？

### 对！核心就是改数据结构

**现在**（`AIHelper.h:68`）：

```cpp
std::vector<std::pair<std::string, long long>> messages;
//              ↑ 只有内容        ↑ 时间戳
//              没有 role 信息！
```

一条消息存的是 `("北京天气", 1717000000)` —— **没有"这是谁说的"这个信息**。

所以 4 个 `buildRequest` 里全要靠 `i % 2 == 0` 猜：

```cpp
// AIStrategy.cpp L25, L68, L113, L153 —— 共 4 处
if (i % 2 == 0) msg["role"] = "user";
else            msg["role"] = "assistant";
```

`ChatHistoryHandler.cpp:61` 也是：

```cpp
msgJson["is_user"] = (i % 2 == 0);  // 靠下标猜
```

**改成**：

```cpp
// AIHelper.h:68 改成显式结构体
struct ChatMessage {
    std::string role;       // "user" 或 "assistant"
    std::string content;    // 消息内容
    long long   timestamp;  // 时间戳
};
std::vector<ChatMessage> messages;
```

改完之后一条消息存的是 `{"user", "北京天气", 1717000000}` —— **role 写在数据里了**。

**收益**：

| 改前 | 改后 |
|------|------|
| `addMessage`：`push_back({content, ms})`（role 丢了） | `push_back({"user", content, ms})`（role 保留） |
| `buildRequest`：`if (i%2==0) role="user"`（猜，4 处） | `msg["role"] = m.role`（直接读，0 处猜） |
| ChatHistoryHandler：`i%2==0` 猜（1 处） | `messages[i].role == "user"` |
| MCP 异常安全：push/pop 不配对时下标全乱 | 不依赖下标，只读数据本身 |

### 额外收益：支持 system prompt

改完后可以直接 `push_back({"system", "你是AI助手...", 0})`——**之前没法区分 user/assistant/system**。

### 面试话术

> "messages 的优化就是改数据结构——从 `pair(内容, 时间)` 改成 `struct{角色, 内容, 时间}`。之前 role 信息靠 `i % 2 == 0` 猜（buildRequest 4 处 + ChatHistoryHandler 1 处），改完后直接读 `m.role`。好处三个：代码可读性（不依赖隐式约定）、异常安全（MCP 异常 push/pop 不配对也不影响）、扩展性（支持 system 角色）。"

---

## 十七、深度话题 9：MCP parseAIResponse 的 4 层兜底方案

### 问题

> 如果第一段 prompt 没有返回预期的 json 格式，后面是怎么走的，AI 会怎么返回？

### 代码在哪

两个文件：

`AIApps/ChatServer/src/AIUtil/AIConfig.cpp:59-78`（阅卷逻辑）：

```cpp
AIToolCall AIConfig::parseAIResponse(const std::string& response) const {
    AIToolCall result;
    try {
        json j = json::parse(response);                     // L63 ★ 直接当 JSON parse
        if (j.contains("tool") && j["tool"].is_string()) { // L65
            result.toolName = j["tool"].get<std::string>();
            if (j.contains("args") && j["args"].is_object()) {
                result.args = j["args"];
            }
            result.isToolCall = true;                        // L70 工具调用
        }
    }
    catch (...) {
        result.isToolCall = false;                           // L75 ★ parse 失败 → 当普通回答
    }
    return result;
}
```

`AIApps/ChatServer/src/AIUtil/AIHelper.cpp:70-78`（阅卷后的分流）：

```cpp
    AIToolCall call = config.parseAIResponse(aiResult);     // L70 阅卷

    if (!call.isToolCall) {                                 // L73 parse 失败走这里
        addMessage(userId, userName, true, userQuestion, sessionId);
        addMessage(userId, userName, false, aiResult, sessionId);  // ★ 存 AI 回答
        return aiResult;                                     // ★ 直接返回给前端
    }
```

### 走一遍具体例子

用户问"北京天气"，AI 输出不是 JSON 而是废话：

```
AI 输出：
  "好的，我来帮您查看北京天气！让我调用天气工具……"
```

代码路线：
1. `json::parse("好的，我来帮您查看...")` → 💥 异常
2. catch → `result.isToolCall = false`
3. 回到 AIHelper::chat → `!call.isToolCall == true`
4. **把 AI 的废话当最终回答存到 messages + 返回给前端**

**用户看到的**：

> AI 回答："好的，我来帮您查看北京天气！让我调用天气工具……"
>
> 用户：？？？（AI 说去查，但没给结果）

### 模型实际会输出什么？

通义千问 qwen-plus 大约 85-95% 一次输出纯 JSON。5-15% 会输出这些：

| 模型输出 | parseAIResponse | 后果 |
|---------|:---:|------|
| `{"tool":"get_weather","args":{"city":"北京"}}` | ✅ `isToolCall=true` | 正常调工具 |
| `` ```json {"tool":"get_weather",...} ``` `` | ❌ parse 失败 | 丢工具 |
| `好的，我来调工具：{"tool":"get_weather",...}` | ❌ parse 失败 | 丢工具 |
| `好的，北京晴 25 度` | ✅ `isToolCall=false` | 正常，无需工具 |
| `` {"tool":"get_weather","args":{}} `` | ✅ parse 成功但缺 city | **能 parse 但参数不全** |

### 改造方案：4 层兜底

```cpp
AIToolCall AIConfig::parseAIResponse(const std::string& response) const {
    AIToolCall result;

    // 层 1：直接 parse（85-95% 场景）
    try {
        json j = json::parse(response);
        if (j.contains("tool") && j["tool"].is_string()) {
            result.toolName = j["tool"].get<std::string>();
            if (j.contains("args")) result.args = j["args"];
            result.isToolCall = true;
            return result;
        }
    } catch (...) {}

    // 层 2：抠 ```json ... ``` 代码块
    std::regex jsonBlock(R"(```(?:json)?\s*(\{.*?\})\s*```)", std::regex::icase);
    std::smatch m;
    if (std::regex_search(response, m, jsonBlock)) {
        try {
            json j = json::parse(m[1].str());
            if (j.contains("tool") && j["tool"].is_string()) {
                result.toolName = j["tool"].get<std::string>();
                if (j.contains("args")) result.args = j["args"];
                result.isToolCall = true;
                return result;
            }
        } catch (...) {}
    }

    // 层 3：抠"第一个 { 到最后一个 }"
    auto first = response.find('{');
    auto last  = response.rfind('}');
    if (first != std::string::npos && last != std::string::npos && last > first) {
        try {
            json j = json::parse(response.substr(first, last - first + 1));
            if (j.contains("tool") && j["tool"].is_string()) {
                result.toolName = j["tool"].get<std::string>();
                if (j.contains("args")) result.args = j["args"];
                result.isToolCall = true;
                return result;
            }
        } catch (...) {}
    }

    // 层 4：全失败 → 当普通回答 + 记录日志
    LOG_WARN << "[MCP] 解析失败，原始输出: " << response;
    result.isToolCall = false;
    return result;
}
```

**成功率**：

| 只有层 1（当前） | 85-95% |
| 加层 2（抠代码块） | → 97%+ |
| 加层 3（抠花括号） | → 99%+ |
| 加层 4（Schema 校验 + 重试） | → 99.9% |

### 面试话术

> "`parseAIResponse` 当前只有一层 `json::parse`，5-15% 的场景（模型输出被 ```json``` 包裹或带废话前缀）会失败。改造方是 4 层兜底：直接 parse → 正则抠代码块 → 抠花括号 → 失败日志。三层的代码大约 50 行，能把成功率从 85% 提到 99%+。"

---

## 十八、FAQ：面试最可能被问的 8 个高频问题

### Q1：4 个厂商有什么不同？具体代码在哪里？

**代码位置**：`AIStrategy.h:36-117`（4 个类）+ `AIStrategy.cpp`（176 行实现）

**6 大差异表**：

| 维度 | 阿里百炼 | 豆包 | 阿里 RAG | 阿里 MCP |
|------|:---:|:---:|:---:|:---:|
| URL | dashscope/chat/completions | volces/chat/completions | dashscope/apps/{ID}/completion | dashscope/chat/completions |
| Key（环境变量） | DASHSCOPE_API_KEY | DOUBAO_API_KEY | DASHSCOPE_API_KEY | DASHSCOPE_API_KEY |
| Model | qwen-plus | doubao-seed | ""（空） | qwen-plus |
| JSON 结构 | `payload.messages` | `payload.messages` | `payload.input.messages` + `parameters` | `payload.messages` |
| 响应路径 | `choices[0].message.content` | `choices[0].message.content` | `output.text` | `choices[0].message.content` |
| isMCPModel | ❌ | ❌ | ❌ | ✅ |
| 窗口 | 32K | 128K | 32K | 32K |

> 最关键的差异：RAG 的 JSON 结构完全不一样（`input.messages`），所以 if-else 没法处理，必须用策略模式。

### Q2：messages 靠奇偶猜 role 怎么优化？

- 现状：`vector<pair<string, long long>>` — 4 个 buildRequest + 1 个 ChatHistoryHandler 靠 `i % 2 == 0` 猜
- 优化：改成 `struct ChatMessage { string role; string content; long long ts; }`
- `addMessage`：`push_back({"user", content, ms})` — role 保留
- `buildRequest`：`msg["role"] = m.role` — 直接读

### Q3：注册到工厂是什么意思？怎么实现的？

- 工厂 = `unordered_map<string, function<shared_ptr<AIStrategy>()>>`
- "注册"：`creators["1"] = [] { return make_shared<AliyunStrategy>(); }`
- "创建"：`creators["1"]()` → 返一个新的 AliyunStrategy
- 4 个注册：`static StrategyRegister<AliyunStrategy>("1")` — 程序启动时自动跑

### Q4：每次调 chat 都会 new 策略吗？

- 是的。一个策略 ~32 字节，`shared_ptr` 函数退出时自动回收
- 开销 = 32KB/秒 × 1000 QPS，完全不用担心

### Q5：AI 推理为什么没走 RabbitMQ？怎么改成异步？

- 现状：`curl_easy_perform` 同步阻塞，占着工作线程 2-5 秒
- 异步改造：Handler → 生成 taskId → publish 到 `ai_task_queue` → 返回 202 → 3 个 worker 消费 → 结果存 Redis → 前端轮询

### Q6：mutexForChatInformation 怎么优化成细粒度？

- 方案 A（最简）：`shared_mutex` → 读并行
- 方案 B（进阶）：分段锁（按 userId hash 到 16 个分片）
- 方案 C（最细）：per-session mutex → map 端用 `shared_mutex`、AIHelper 内部独立锁

### Q7：如果允许多端并发写同一会话，怎么避免消息乱序？

- 方案 A：`atomic<uint64_t> seqId`，读端按 seqId 排序
- 方案 B：AIHelper 内部 `std::mutex` 串行化
- 方案 C：MPSC 单写者队列

### Q8：项目里最大的 3 个坑是什么？

| # | 坑 | 位置 | 面试怎么说 |
|---|-----|------|----------|
| 1 | `parseAIResponse` 只 `json::parse` 一下 | `AIConfig.cpp:63` | "这是当前项目的局限，5-15% 的场景 AI 输出非纯 JSON 会失败。我给 4 层兜底方案（正则抠代码块 + 抠花括号 + Schema 校验 + 重试）能到 99%+" |
| 2 | messages 靠 `i % 2 == 0` 猜 role | `AIHelper.h:68` | "这是个隐式约定。我建议改成显式 struct，role 写进数据里，不依赖下标" |
| 3 | `cleanExpiredSessions` 是空的 | `SessionManager.cpp:64-69` | "这是遗留的 TODO。应该加定时器后台清理过期 session" |
