#pragma once

#include <atomic>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>


#include "../../../HttpServer/include/http/HttpServer.h"
#include "../../../HttpServer/include/utils/MysqlUtil.h"
#include "../../../HttpServer/include/utils/FileUtil.h"
#include "../../../HttpServer/include/utils/JsonUtil.h"
#include"AIUtil/AISpeechProcessor.h"
#include"AIUtil/AIHelper.h"
#include"AIUtil/ImageRecognizer.h"
#include"AIUtil/base64.h"
#include"AIUtil/MQManager.h"


class ChatLoginHandler;
class ChatRegisterHandler;
class ChatLogoutHandler;
class ChatHandler;
class ChatEntryHandler;
class ChatSendHandler;
class ChatHistoryHandler;

class AIMenuHandler;
class AIUploadHandler;
class AIUploadSendHandler;


class ChatCreateAndSendHandler;
class ChatSessionsHandler;
class ChatSpeechHandler;

/**
 * ChatServer - AI Agent 平台核心服务类
 * 基于 muduo 网络库构建，集成多模型对话、图像识别、语音合成与异步消息队列
 */
class ChatServer {
public:
	ChatServer(int port,
		const std::string& name,
		muduo::net::TcpServer::Option option = muduo::net::TcpServer::kNoReusePort);

	void setThreadNum(int numThreads);
	void start();
	void initChatMessage();
private:
	friend class ChatLoginHandler;
	friend class ChatRegisterHandler;
	friend  ChatLogoutHandler;
	friend class ChatHandler;
	friend class ChatEntryHandler;
	friend class ChatSendHandler;
	friend class AIMenuHandler;
	friend class AIUploadHandler;
	friend class AIUploadSendHandler;
	friend class ChatHistoryHandler;

	friend class ChatCreateAndSendHandler;
	friend class ChatSessionsHandler;
	friend class ChatSpeechHandler;

private:
	void initialize();
	void initializeSession();
	void initializeRouter();
	void initializeMiddleware();
	

	void readDataFromMySQL();

	// ---------------- 共享状态访问器（统一收口加锁逻辑） ----------------
	// 改造前：每个 Handler 自己 lock_guard 全局 mutex，再直接 operator[] 操作裸 map。
	// 问题有三：① 读也串行；② operator[] 会隐式插入，"只读"接口其实是写；
	//          ③ 加锁逻辑散落在 7 个 Handler 里，漏锁/用错锁无人察觉（确实各出过一处 bug）。
	// 改造后：裸 map 只能通过下面这组方法访问，读路径 shared_lock 并行，写路径 unique_lock 独占。

	// 读路径：只查不建，未命中返回 nullptr。多个请求可同时进入。
	std::shared_ptr<AIHelper> findChatHelper(int userId, const std::string& sessionId) const;

	// 写路径：先用 shared_lock 试读（已存在的会话走这条，占绝大多数），
	// 未命中才升级 unique_lock，并在持写锁后再查一次（双重检查），防止两个线程重复创建。
	// created 非空时回传"本次是否真的新建了"，调用方据此决定要不要登记 sessionId。
	std::shared_ptr<AIHelper> getOrCreateChatHelper(int userId,
		const std::string& sessionId,
		bool* created = nullptr);

	// 图像识别器：构造要把 ONNX 模型从磁盘读进来（百毫秒级），
	// 绝不能占着写锁做，因此先在锁外构造好再抢锁插入。
	std::shared_ptr<ImageRecognizer> getOrCreateRecognizer(int userId);

	// 会话 ID 列表：登记走 unique_lock，列举走 shared_lock
	void appendSessionId(int userId, const std::string& sessionId);
	std::vector<std::string> listSessionIds(int userId) const;

	// 在线态：check-and-set 必须在同一个 unique_lock 内完成。
	// 返回 true 表示本次成功抢到"上线"名额；false 表示该账号已在线（重复登录）。
	bool tryMarkOnline(int userId);
	void markOffline(int userId);

	void packageResp(const std::string& version, http::HttpResponse::HttpStatusCode statusCode,
		const std::string& statusMsg, bool close, const std::string& contentType,
		int contentLen, const std::string& body, http::HttpResponse* resp);

	void setSessionManager(std::unique_ptr<http::session::SessionManager> manager)
	{
		httpServer_.setSessionManager(std::move(manager));
	}
	http::session::SessionManager* getSessionManager() const
	{
		return httpServer_.getSessionManager();
	}

	http::HttpServer	httpServer_;

	http::MysqlUtil		mysqlUtil_;

	// ---------------- 共享状态 ----------------
	// 四张表一律是"读多写少"：查会话/拉历史/列会话的 QPS 远高于新建会话。
	// 因此全部换成 shared_mutex —— 读路径 shared_lock 可并行，写路径 unique_lock 独占。
	// mutable 是因为纯读的访问器声明成了 const 成员函数，但仍需要上锁。
	//
	// 锁顺序约定：任何时候都不允许同时持有下面两把及以上的锁。
	// 需要连续操作两张表时（如新建会话同时登记 sessionId），拆成两段串行加锁，
	// 不做嵌套，从根上消除死锁可能。

	// 在线用户状态：userId -> isOnline，用于防止重复登录
	std::unordered_map<int, bool>	onlineUsers_;
	mutable std::shared_mutex	mutexForOnlineUsers_;

	// 多租户会话管理：userId -> (sessionId -> AIHelper)
	// 二级嵌套 map 实现单用户多会话隔离
	std::unordered_map<int, std::unordered_map<std::string,std::shared_ptr<AIHelper> > > chatInformation;
	mutable std::shared_mutex	mutexForChatInformation;

	// 图像识别器映射：userId -> ImageRecognizer，每用户独立实例
	std::unordered_map<int, std::shared_ptr<ImageRecognizer> > ImageRecognizerMap;
	mutable std::shared_mutex	mutexForImageRecognizerMap;

	// 会话 ID 列表：userId -> [sessionId1, sessionId2, ...]
	std::unordered_map<int,std::vector<std::string> > sessionsIdsMap;
	mutable std::shared_mutex mutexForSessionsId;

};

