// ============================================================
// test_concurrency.cpp — 线程池并发压力测试（8GB 适配）
// ============================================================

#include "../include/lock_free_thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

// ---- 多线程同时提交 ----

TEST(ConcurrencyTest, MultiThreadSubmit)
{
    thread_pool::DynamicThreadPool<> pool(2);
    constexpr int kThreads = 3;
    constexpr int kTasksPerThread = 100;

    std::atomic<int> total{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&]
        {
            std::vector<std::future<void>> futures;
            for (int i = 0; i < kTasksPerThread; ++i)
            {
                futures.push_back(pool.submit([&total]
                {
                    total.fetch_add(1, std::memory_order_relaxed);
                }));
            }
            for (auto& f : futures) f.wait();
        });
    }

    for (auto& t : threads) t.join();
    pool.wait_all();

    EXPECT_EQ(total.load(), kThreads * kTasksPerThread);
    EXPECT_EQ(pool.completed_count(), kThreads * kTasksPerThread);
    EXPECT_EQ(pool.active_count(), 0);
}

// ---- 提交与关闭竞态 ----

TEST(ConcurrencyTest, SubmitShutdownRace)
{
    for (int r = 0; r < 3; ++r)
    {
        thread_pool::DynamicThreadPool<> pool(2);
        std::atomic<bool> start{false};

        std::thread submitter([&]
        {
            while (!start.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            for (int i = 0; i < 50; ++i)
            {
                try
                {
                    pool.submit([]
                    { std::this_thread::yield(); });
                }
                catch (...) { break; }
            }
        });

        start.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        pool.shutdown();
        submitter.join();
    }
    SUCCEED();
}

// ---- 长链 future 依赖 ----

TEST(ConcurrencyTest, ChainedFutures)
{
    thread_pool::DynamicThreadPool<> pool(2);

    auto f1 = pool.submit([] { return 1; });
    auto f2 = pool.submit([&f1] { return f1.get() + 2; });
    auto f3 = pool.submit([&f2] { return f2.get() + 3; });

    EXPECT_EQ(f3.get(), 6);
}

// ---- shutdown_now 丢弃未执行任务 ----

TEST(ConcurrencyTest, ShutdownNowDiscardsTasks)
{
    thread_pool::DynamicThreadPool<> pool(2);
    std::atomic<bool> stop_submit{false};

    std::thread submitter([&]
    {
        while (!stop_submit.load(std::memory_order_acquire))
        {
            try
            {
                pool.submit([]
                { std::this_thread::yield(); });
            }
            catch (...) { break; }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    pool.shutdown_now();
    stop_submit.store(true, std::memory_order_release);
    submitter.join();
    SUCCEED();
}

// ---- 析构自动关闭 ----

TEST(ConcurrencyTest, DestructorAutoShutdown)
{
    constexpr int kTasks = 50;
    {
        thread_pool::DynamicThreadPool<> pool(2);
        std::vector<std::future<int>> futures;
        for (int i = 0; i < kTasks; ++i)
        {
            futures.push_back(pool.submit([i] { return i * i; }));
        }
        for (auto& f : futures) f.get();
    }
    SUCCEED();
}
