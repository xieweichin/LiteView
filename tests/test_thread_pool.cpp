#include "doctest.h"

#include "core/ThreadPool.h"

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

struct Gate {
    std::promise<void> opened;
    std::shared_future<void> ready = opened.get_future().share();
    bool released = false;
    void Release() {
        if (!released) {
            released = true;
            opened.set_value();
        }
    }
    ~Gate() { Release(); }
};

}  // namespace

TEST_CASE("queued tasks honor priority, FIFO, and cancellation") {
    ThreadPool pool(1);
    Gate gate;
    std::promise<void> started;
    auto startedFuture = started.get_future();
    CHECK(pool.Submit([&] { started.set_value(); gate.ready.wait(); }) == S_OK);
    const auto startStatus = startedFuture.wait_for(3s);
    CHECK(startStatus == std::future_status::ready);
    if (startStatus != std::future_status::ready) return;

    std::mutex orderMutex;
    std::vector<int> order;
    std::promise<void> finished;
    auto finishedFuture = finished.get_future();
    auto record = [&](int value) {
        std::lock_guard<std::mutex> lock(orderMutex);
        order.push_back(value);
        if (order.size() == 3) finished.set_value();
    };
    ThreadPool::TaskId cancelled = 0;
    CHECK(pool.Submit([&] { record(99); }, ThreadPool::Priority::Low, &cancelled) == S_OK);
    CHECK(pool.Submit([&] { record(2); }) == S_OK);
    CHECK(pool.Submit([&] { record(1); }, ThreadPool::Priority::High) == S_OK);
    CHECK(pool.Submit([&] { record(3); }) == S_OK);
    CHECK(pool.Cancel(cancelled));
    CHECK_FALSE(pool.Cancel(cancelled));
    gate.Release();
    const auto finishStatus = finishedFuture.wait_for(3s);
    CHECK(finishStatus == std::future_status::ready);
    CHECK(pool.Shutdown() == S_OK);
    CHECK(order == std::vector<int>{1, 2, 3});
    CHECK(pool.Submit([] {}) == E_ABORT);
}

TEST_CASE("shutdown discards queued tasks and waits for active work") {
    ThreadPool pool(1);
    Gate gate;
    std::promise<void> started;
    auto startedFuture = started.get_future();
    CHECK(pool.Submit([&] { started.set_value(); gate.ready.wait(); }) == S_OK);
    const auto startStatus = startedFuture.wait_for(3s);
    CHECK(startStatus == std::future_status::ready);
    if (startStatus != std::future_status::ready) return;

    bool queuedRan = false;
    CHECK(pool.Submit([&] { queuedRan = true; }) == S_OK);
    auto closing = std::async(std::launch::async, [&] { return pool.Shutdown(); });
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (pool.PendingCount() != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    CHECK(pool.PendingCount() == 0);
    gate.Release();
    CHECK(closing.wait_for(3s) == std::future_status::ready);
    CHECK(closing.get() == S_OK);
    CHECK_FALSE(queuedRan);
}

TEST_CASE("a worker cannot shut down its own pool") {
    ThreadPool pool(1);
    std::promise<HRESULT> result;
    auto resultFuture = result.get_future();
    CHECK(pool.Submit([&] { result.set_value(pool.Shutdown()); }) == S_OK);
    const auto status = resultFuture.wait_for(3s);
    CHECK(status == std::future_status::ready);
    if (status == std::future_status::ready) CHECK(resultFuture.get() == E_UNEXPECTED);
    CHECK(pool.Shutdown() == S_OK);
}
