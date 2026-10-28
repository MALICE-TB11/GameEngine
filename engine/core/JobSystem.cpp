#include "core/JobSystem.h"

#include "core/Log.h"

#include <algorithm>

namespace eng {

void JobSystem::init(uint32_t workers) {
    if (!m_workers.empty()) return;
    if (workers == 0) workers = std::max(1u, std::thread::hardware_concurrency() - 1);
    m_stop = false;
    for (uint32_t i = 0; i < workers; ++i) m_workers.emplace_back([this] { workerLoop(); });
    LOG_INFO("Job 系统：{} 个工作线程", workers);
}

void JobSystem::shutdown() {
    {
        std::lock_guard lock(m_mutex);
        if (m_workers.empty()) return;
        m_stop = true;
    }
    m_cv.notify_all();
    for (std::thread& t : m_workers) t.join();
    m_workers.clear();
    m_queue.clear();
}

void JobSystem::submit(Job job, std::shared_ptr<JobCounter> counter) {
    if (counter) ++counter->pending;
    if (m_workers.empty()) {
        // 没有初始化线程池（例如单元测试）：退化为同步执行，行为仍然正确
        job();
        if (counter) --counter->pending;
        return;
    }
    {
        std::lock_guard lock(m_mutex);
        m_queue.push_back({std::move(job), std::move(counter)});
    }
    m_cv.notify_one();
}

bool JobSystem::tryRunOne() {
    Task task;
    {
        std::lock_guard lock(m_mutex);
        if (m_queue.empty()) return false;
        task = std::move(m_queue.front());
        m_queue.pop_front();
    }
    task.job();
    if (task.counter) --task.counter->pending;
    return true;
}

void JobSystem::workerLoop() {
    while (true) {
        Task task;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
            if (m_stop && m_queue.empty()) return;
            task = std::move(m_queue.front());
            m_queue.pop_front();
        }
        task.job();
        if (task.counter) --task.counter->pending;
    }
}

void JobSystem::wait(const JobCounter& counter) {
    while (!counter.done()) {
        // 自己也干活；队列空了就让出时间片，等其他线程把剩下的做完
        if (!tryRunOne()) std::this_thread::yield();
    }
}

void JobSystem::parallelFor(uint32_t count, uint32_t batch, const std::function<void(uint32_t, uint32_t)>& fn) {
    if (count == 0) return;
    batch = std::max(1u, batch);
    auto counter = std::make_shared<JobCounter>();
    for (uint32_t begin = 0; begin < count; begin += batch) {
        const uint32_t end = std::min(count, begin + batch);
        submit([&fn, begin, end] { fn(begin, end); }, counter);
    }
    wait(*counter);
}

void JobSystem::runOnMainThread(Job job) {
    std::lock_guard lock(m_mainMutex);
    m_mainQueue.push_back(std::move(job));
}

uint32_t JobSystem::pumpMainThread(uint32_t maxJobs) {
    uint32_t ran = 0;
    while (ran < maxJobs) {
        Job job;
        {
            std::lock_guard lock(m_mainMutex);
            if (m_mainQueue.empty()) break;
            job = std::move(m_mainQueue.front());
            m_mainQueue.pop_front();
        }
        job();
        ++ran;
    }
    return ran;
}

} // namespace eng
