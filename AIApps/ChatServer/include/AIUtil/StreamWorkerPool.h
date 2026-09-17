#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

/**
 * StreamWorkerPool - SSE 流式任务工作线程池（单例）
 *
 * 现状问题：Handler 在 Muduo IO 线程里同步执行 curl 到 [DONE]，
 * 一条流钉死一个 IO 线程（共 4 个），且拖累同 loop 上其他连接。
 *
 * 改造后：IO 线程只做"写 SSE 头 + 打包任务提交 + 立即返回"，
 * 整条 chat（curl 阻塞 + 增量回调 + 发送）在 worker 线程执行；
 * conn->send 由 muduo 自动转移到连接所属 IO 线程，线程安全。
 */
class StreamWorkerPool {
public:
    using Task = std::function<void()>;

    static StreamWorkerPool& instance();

    // 启动线程池（幂等；ChatServer 初始化时显式调用）
    void start(size_t threadNum);

    // 提交流式任务（未启动时自动以默认线程数启动，防御式兜底）
    void submit(Task task);

    // 停止并回收线程（进程退出时）
    void shutdown();

private:
    StreamWorkerPool() = default;
    ~StreamWorkerPool() { shutdown(); }
    StreamWorkerPool(const StreamWorkerPool&) = delete;
    StreamWorkerPool& operator=(const StreamWorkerPool&) = delete;

    void workerLoop();

    std::vector<std::thread> workers_;
    std::queue<Task>         tasks_;
    std::mutex               mtx_;
    std::condition_variable  cv_;
    bool                     started_ = false;
    bool                     stopping_ = false;
};
