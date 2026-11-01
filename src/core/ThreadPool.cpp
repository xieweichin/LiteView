#include "core/ThreadPool.h"

#include <algorithm>
#include <new>
#include <utility>

namespace {
thread_local const ThreadPool* currentWorkerPool = nullptr;
}

bool ThreadPool::EarlierJob::operator()(const std::shared_ptr<Job>& left,
                                        const std::shared_ptr<Job>& right) const noexcept {
    if (left->priority != right->priority) {
        return static_cast<int>(left->priority) > static_cast<int>(right->priority);
    }
    return left->id < right->id;
}

std::size_t ThreadPool::DefaultThreadCount() noexcept {
    const unsigned hardware = std::thread::hardware_concurrency();
    return std::max(1u, std::min(4u, hardware > 1 ? hardware - 1 : 1u));
}

ThreadPool::ThreadPool(std::size_t threadCount) noexcept {
    if (threadCount == 0) threadCount = 1;
    try {
        workers_.reserve(threadCount);
        for (std::size_t i = 0; i < threadCount; ++i) {
            workers_.emplace_back(&ThreadPool::Worker, this);
        }
    } catch (const std::bad_alloc&) {
        startupResult_ = E_OUTOFMEMORY;
    } catch (...) {
        startupResult_ = E_FAIL;
    }
    if (FAILED(startupResult_)) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
    }
}

ThreadPool::~ThreadPool() { (void)Shutdown(); }

HRESULT ThreadPool::Submit(Task task, Priority priority, TaskId* taskId) noexcept {
    if (!task) return E_INVALIDARG;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (FAILED(startupResult_)) return startupResult_;
        if (stopping_) return E_ABORT;
        const TaskId id = nextId_++;
        if (id == 0) return E_FAIL;
        auto job = std::make_shared<Job>(Job{id, priority, std::move(task)});
        pending_.emplace(id, job);
        try {
            queue_.insert(job);
        } catch (...) {
            pending_.erase(id);
            throw;
        }
        if (taskId) *taskId = id;
        ready_.notify_one();
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}

bool ThreadPool::Cancel(TaskId taskId) noexcept {
    std::shared_ptr<Job> removed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = pending_.find(taskId);
        if (found == pending_.end()) return false;
        removed = found->second;
        queue_.erase(removed);
        pending_.erase(found);
    }
    // Destroy user captures outside the lock: their destructors may call us.
    return true;
}

std::size_t ThreadPool::CancelPending() noexcept {
    decltype(queue_) removedQueue;
    decltype(pending_) removedPending;
    std::size_t count;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        count = pending_.size();
        removedQueue.swap(queue_);
        removedPending.swap(pending_);
    }
    return count;
}

HRESULT ThreadPool::Shutdown() noexcept {
    // Check before taking shutdownMutex_: an external shutdown may already be
    // waiting for this task to finish.
    if (currentWorkerPool == this) return E_UNEXPECTED;
    decltype(queue_) removedQueue;
    decltype(pending_) removedPending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        removedQueue.swap(queue_);
        removedPending.swap(pending_);
    }
    ready_.notify_all();
    // Release queued captures promptly, without holding either pool lock.
    removedQueue.clear();
    removedPending.clear();
    {
        std::lock_guard<std::mutex> shutdownLock(shutdownMutex_);
        try {
            for (auto& worker : workers_) {
                if (worker.joinable()) worker.join();
            }
        } catch (...) {
            return E_FAIL;
        }
    }
    return S_OK;
}

std::size_t ThreadPool::PendingCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

void ThreadPool::Worker() noexcept {
    currentWorkerPool = this;
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) {
                currentWorkerPool = nullptr;
                return;
            }
            job = *queue_.begin();
            queue_.erase(queue_.begin());
            pending_.erase(job->id);
        }
        // A task must not be able to terminate the worker process.
        try {
            job->task();
        } catch (...) {
            // Task-specific errors should be reported by the task itself.
        }
    }
}
