#pragma once
// Job 系统：一个固定大小的线程池 + 一个「主线程任务队列」。
//
// 为什么现在需要它：资源烘焙（BC7 压缩、网格简化）和加载（读文件、解析）都很慢，
// 放在主线程会让编辑器卡住几秒。把它们丢给后台线程，主线程继续渲染。
//
// 为什么还要主线程队列：Vulkan 的上传（immediateSubmit）、修改场景这类操作不能在后台线程做
// （本引擎只有一个图形队列，命令池也不是线程安全的）。后台任务做完重活后，
// 把「收尾」作为一个主线程任务排队，由主循环每帧执行一部分。
//
// EXTEND: 任务数量变多（动画、物理、剔除并行化）后，可以升级为带依赖关系的任务图和工作窃取队列。

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace eng {

// 一组任务的完成计数：submit 时 +1，任务执行完 -1，归零即全部完成
struct JobCounter {
    std::atomic<uint32_t> pending{0};
    bool done() const { return pending.load() == 0; }
};

class JobSystem {
public:
    using Job = std::function<void()>;

    // workers = 0：使用「CPU 核心数 - 1」个工作线程（给主线程留一个核心），至少 1 个
    void init(uint32_t workers = 0);
    void shutdown();
    ~JobSystem() { shutdown(); }

    // 提交后台任务。counter 可为空；不为空时完成后计数减一
    void submit(Job job, std::shared_ptr<JobCounter> counter = nullptr);
    // 阻塞直到 counter 归零；等待期间调用线程也帮忙执行队列里的任务（不浪费一个核心）
    void wait(const JobCounter& counter);
    // 把 [0, count) 切成大小为 batch 的块并行执行，返回时全部完成
    void parallelFor(uint32_t count, uint32_t batch, const std::function<void(uint32_t begin, uint32_t end)>& fn);

    // 主线程任务：从任意线程排队，由主线程在 pumpMainThread 中执行
    void runOnMainThread(Job job);
    // 主线程每帧调用；最多执行 maxJobs 个，避免一帧里做太多上传导致卡顿
    uint32_t pumpMainThread(uint32_t maxJobs = UINT32_MAX);

    uint32_t workerCount() const { return static_cast<uint32_t>(m_workers.size()); }

private:
    struct Task {
        Job job;
        std::shared_ptr<JobCounter> counter;
    };
    bool tryRunOne();
    void workerLoop();

    std::vector<std::thread> m_workers;
    std::deque<Task> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop = false;

    std::deque<Job> m_mainQueue;
    std::mutex m_mainMutex;
};

} // namespace eng
