#pragma once

#include <windows.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

// A bounded set of workers. Queued tasks run by priority and then FIFO.
// Shutdown cancels queued work and waits for already-running tasks to finish.
// The pool must be shut down and destroyed outside its own worker threads.
class ThreadPool {
public:
    using Task = std::function<void()>;
    using TaskId = std::uint64_t;

    enum class Priority { Low = 0, Normal = 1, High = 2 };

    explicit ThreadPool(std::size_t threadCount) noexcept;
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    HRESULT Submit(Task task, Priority priority = Priority::Normal,
                   TaskId* taskId = nullptr) noexcept;
    bool Cancel(TaskId taskId) noexcept;
    std::size_t CancelPending() noexcept;
    HRESULT Shutdown() noexcept;
    std::size_t PendingCount() const noexcept;

private:
    struct Job {
        TaskId id;
        Priority priority;
        Task task;
        bool cancelled = false;
    };
    struct EarlierJob {
        bool operator()(const std::shared_ptr<Job>& left,
                        const std::shared_ptr<Job>& right) const noexcept;
    };

    void Worker() noexcept;

    mutable std::mutex mutex_;
    std::mutex shutdownMutex_;
    std::condition_variable ready_;
    std::priority_queue<std::shared_ptr<Job>, std::vector<std::shared_ptr<Job>>, EarlierJob> queue_;
    std::unordered_map<TaskId, std::shared_ptr<Job>> pending_;
    std::vector<std::thread> workers_;
    TaskId nextId_ = 1;
    bool stopping_ = false;
    HRESULT startupResult_ = S_OK;
};
