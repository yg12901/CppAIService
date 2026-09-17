#pragma once
#include "AIUtil/SseChannel.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

/**
 * SseKeepalive - SSE 流心跳管理器（单例）
 *
 * 问题：TTFT 之前（LLM 思考 / MCP 工具执行期间）SSE 流上一个字节都没有，
 * 中间代理（nginx proxy_read_timeout 默认 60s、SLB/CDN 30~60s）可能掐断空闲连接。
 *
 * 方案：ChatServer 在主 loop 上 runEvery(5s) 调 onTimer()，对空闲超过 10s
 * 的活跃流补发 ": ping\n\n" 注释行保活（SSE 规范：冒号开头是注释，
 * 浏览器与 SseParser 均自动忽略，前端零改动）。
 *
 * 为什么不用 curl 进度回调发心跳：进度回调只在 curl 传输期间触发，
 * 盖不住 MCP"第 1 段结束 → 工具执行 5s → 第 2 段开始"的间隙。
 */
class SseKeepalive {
public:
    static SseKeepalive& instance();

    // Handler 创建流时注册（持 weak_ptr，worker 意外崩溃没注销也能被清理）
    void registerFlow(const std::shared_ptr<SseChannel>& ch);

    // 流结束时注销（worker 发完 [DONE] 后调用）
    void unregisterFlow(uint64_t id);

    // 定时扫描（runEvery 每 5s 调一次）：
    //   清理已断/已释放的流 + 对空闲 >10s 的活跃流补发心跳
    void onTimer();

private:
    SseKeepalive() = default;
    SseKeepalive(const SseKeepalive&) = delete;
    SseKeepalive& operator=(const SseKeepalive&) = delete;

    std::mutex mtx_;   // 注册/注销来自 Handler 与 worker 线程，定时器在主 loop，需加锁
    std::unordered_map<uint64_t, std::weak_ptr<SseChannel>> flows_;
};
