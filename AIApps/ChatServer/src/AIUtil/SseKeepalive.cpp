#include "AIUtil/SseKeepalive.h"

SseKeepalive& SseKeepalive::instance() {
    static SseKeepalive ka;   // Meyers 单例
    return ka;
}

// 注册一条活跃流（Handler 写 SSE 头之后调用）
void SseKeepalive::registerFlow(const std::shared_ptr<SseChannel>& ch) {
    if (!ch) return;
    std::lock_guard<std::mutex> lk(mtx_);
    flows_[ch->id()] = ch;
}

// 注销（worker 发完 [DONE] 后调用）
void SseKeepalive::unregisterFlow(uint64_t id) {
    std::lock_guard<std::mutex> lk(mtx_);
    flows_.erase(id);
}

// 定时扫描：清理死流 + 空闲超阈值的心跳补发
void SseKeepalive::onTimer() {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto it = flows_.begin(); it != flows_.end();) {
        std::shared_ptr<SseChannel> ch = it->second.lock();
        // weak_ptr 已释放（worker 异常退出没注销）或客户端已断开 → 清理
        if (!ch || !ch->alive()) {
            it = flows_.erase(it);
            continue;
        }
        // 空闲超过 10s（远低于常见代理 30~60s 的 idle 超时）→ 补发心跳
        if (ch->idleMs() > 10000) {
            ch->sendPing();   // 发送失败也无妨：下轮扫描会因 !alive() 清理
        }
        ++it;
    }
}
