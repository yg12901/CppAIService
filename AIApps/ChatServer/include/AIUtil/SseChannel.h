#pragma once
#include <muduo/net/TcpConnection.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

/**
 * SseChannel - 一条 SSE 流的发送通道（worker 线程与心跳定时器共用）
 *
 * 职责：
 *   1. 持有 TcpConnectionPtr（shared_ptr），解决 Handler 返回后 resp 栈对象析构的生命周期问题
 *   2. 断开检测：alive() 立刻反映客户端是否还在
 *   3. 背压：客户端收得慢时挡住发送线程（反压上游 curl 读取，内存有界）
 *   4. 心跳触点：touch()/idleMs() 供 SseKeepalive 判断何时补发 ": ping" 保活
 *
 * 线程安全性：conn->send / connected 由 muduo 保证（转移到连接所属 IO 线程执行）
 */
class SseChannel : public std::enable_shared_from_this<SseChannel> {
public:
    explicit SseChannel(muduo::net::TcpConnectionPtr conn)
        : conn_(std::move(conn)), id_(nextId()), lastSendMs_(nowMs()) {}

    // 流的唯一标识（SseKeepalive 注册表的 key）
    uint64_t id() const { return id_; }

    // 客户端是否还在（断开后返回 false，调用方应 requestAbort 上游 LLM 流）
    bool alive() const {
        if (clientGone_.load()) return false;
        return conn_ && conn_->connected();
    }

    // 发一条 SSE data 事件；false = 客户端已断
    bool sendEvent(const std::string& payload) {
        if (!prepareSend()) return false;
        conn_->send("data: " + payload + "\n\n");
        touch();
        return true;
    }

    // 写原始字节（SSE 响应头 / [DONE]）；false = 客户端已断
    bool sendRaw(const std::string& raw) {
        if (!prepareSend()) return false;
        conn_->send(raw);
        touch();
        return true;
    }

    // 心跳注释行：SSE 规范中冒号开头是注释，客户端与 SseParser 均自动忽略
    // （独立路径不走背压：仅 8 字节且在心跳定时器线程调用，不能被背压 sleep 卡住）
    bool sendPing() {
        if (!alive()) return false;
        conn_->send(": ping\n\n");
        touch();
        return true;
    }

    // 优雅收尾：发完 [DONE] 后关闭写端（muduo 排空发送缓冲后断开连接）
    void close() {
        if (conn_ && conn_->connected()) conn_->shutdown();
    }

    // 心跳触点：每次成功写入后调用，重置空闲计时
    void touch() { lastSendMs_.store(nowMs(), std::memory_order_relaxed); }
    int64_t idleMs() const { return nowMs() - lastSendMs_.load(std::memory_order_relaxed); }

private:
    static uint64_t nextId() {
        static std::atomic<uint64_t> counter{0};
        return counter.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    static int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // 发送前置：存活检查 + 背压等待
    // 背压原理：输出缓冲超过阈值时 sleep 等待网络排空 → 挡住 worker 线程
    // → curl 的 WriteCallback 跟着被挡 → 不再继续读 LLM 流（TCP 窗口自然反压）
    bool prepareSend() {
        if (!alive()) return false;
        while (conn_->connected() &&
               static_cast<size_t>(conn_->outputBuffer()->readableBytes()) > kBackpressureBytes) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!conn_->connected()) {
            clientGone_.store(true);
            return false;
        }
        return true;
    }

    muduo::net::TcpConnectionPtr conn_;   // 持有连接（连接活着通道就活着）
    uint64_t id_;
    std::atomic<int64_t> lastSendMs_;     // 最后一次写字节的时间（心跳基准）
    std::atomic<bool> clientGone_{false};

    static constexpr size_t kBackpressureBytes = 1 << 20;  // 1MB 输出缓冲上限
};
