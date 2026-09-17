#include "AIUtil/StreamWorkerPool.h"

StreamWorkerPool& StreamWorkerPool::instance() {
    static StreamWorkerPool pool;   // Meyers 单例，C++11 起线程安全
    return pool;
}

// 启动线程池（幂等：重复调用只扩容不重复起线程）
void StreamWorkerPool::start(size_t threadNum) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (started_) return;
    started_ = true;
    for (size_t i = 0; i < threadNum; ++i) {
        workers_.emplace_back(&StreamWorkerPool::workerLoop, this);
    }
}

// 提交任务；未启动时自动以 4 线程兜底启动
void StreamWorkerPool::submit(Task task) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!started_) {
            started_ = true;
            for (size_t i = 0; i < 4; ++i) {
                workers_.emplace_back(&StreamWorkerPool::workerLoop, this);
            }
        }
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
}

// 停止并回收全部线程
void StreamWorkerPool::shutdown() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!started_ || stopping_) return;
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
}

// 工作线程主循环：阻塞取任务执行（任务内部包含完整的"curl + 增量转发"）
void StreamWorkerPool::workerLoop() {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this] { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty()) return;   // 排空后退出
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        if (task) task();   // 异常由任务自带 try-catch 兜底，不能打死 worker
    }
}
