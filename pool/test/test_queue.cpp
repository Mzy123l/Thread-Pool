// ============================================================
// test_queue.cpp — 链表无锁队列 GoogleTest 单元测试
// ============================================================

#include "../include/lock_free_queue.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using lock_free_container::LockFreeQueue;

// ============================================================
// 基本功能测试
// ============================================================

TEST(LockFreeQueueTest, FifoOrder)
{
    LockFreeQueue<int> queue;

    EXPECT_TRUE(queue.empty());
    EXPECT_TRUE(queue.enqueue(1));
    EXPECT_TRUE(queue.enqueue(2));
    EXPECT_TRUE(queue.enqueue(3));
    EXPECT_FALSE(queue.empty());

    int value = 0;
    EXPECT_TRUE(queue.dequeue(value));
    EXPECT_EQ(value, 1);
    EXPECT_TRUE(queue.dequeue(value));
    EXPECT_EQ(value, 2);
    EXPECT_TRUE(queue.dequeue(value));
    EXPECT_EQ(value, 3);
    EXPECT_TRUE(queue.empty());
}

TEST(LockFreeQueueTest, EmptyDequeueReturnsFalse)
{
    LockFreeQueue<int> queue;
    int value = -1;
    EXPECT_FALSE(queue.dequeue(value));
    EXPECT_EQ(value, -1);  // 值不应被修改
}

TEST(LockFreeQueueTest, EnqueueAfterDequeueAll)
{
    LockFreeQueue<int> queue;
    queue.enqueue(10);
    int value = 0;
    queue.dequeue(value);
    EXPECT_EQ(value, 10);
    EXPECT_TRUE(queue.empty());

    // 重用队列
    queue.enqueue(20);
    EXPECT_FALSE(queue.empty());
    queue.dequeue(value);
    EXPECT_EQ(value, 20);
}

TEST(LockFreeQueueTest, ClearQueue)
{
    LockFreeQueue<int> queue;
    for (int i = 0; i < 100; ++i) {
        queue.enqueue(i);
    }
    EXPECT_FALSE(queue.empty());

    queue.clear();
    EXPECT_TRUE(queue.empty());

    int value = 0;
    EXPECT_FALSE(queue.dequeue(value));
}

TEST(LockFreeQueueTest, MoveConstructor)
{
    LockFreeQueue<int> q1;
    q1.enqueue(42);
    q1.enqueue(99);

    LockFreeQueue<int> q2(std::move(q1));

    int value = 0;
    EXPECT_TRUE(q2.dequeue(value));
    EXPECT_EQ(value, 42);
    EXPECT_TRUE(q2.dequeue(value));
    EXPECT_EQ(value, 99);
    EXPECT_TRUE(q2.empty());
}

// ============================================================
// 非平凡类型
// ============================================================

TEST(LockFreeQueueTest, StringType)
{
    LockFreeQueue<std::string> queue;

    queue.enqueue("hello");
    queue.enqueue(std::string("world"));

    std::string value;
    EXPECT_TRUE(queue.dequeue(value));
    EXPECT_EQ(value, "hello");
    EXPECT_TRUE(queue.dequeue(value));
    EXPECT_EQ(value, "world");
}

// ============================================================
// 并发测试
// ============================================================

TEST(LockFreeQueueTest, ConcurrentMPMC)
{
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr int kValuesPerProducer = 1000;
    constexpr int kTotalValues = kProducers * kValuesPerProducer;

    LockFreeQueue<int> queue;
    std::atomic<int> consumed_count{0};
    std::atomic<long long> consumed_sum{0};

    const long long expected_sum =
        static_cast<long long>(kTotalValues - 1) * kTotalValues / 2;

    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;

    for (int p = 0; p < kProducers; ++p)
    {
        producers.emplace_back([&, p]()
        {
            const int begin = p * kValuesPerProducer;
            const int end = begin + kValuesPerProducer;
            for (int v = begin; v < end; ++v)
            {
                queue.enqueue(v);
            }
        });
    }

    for (int i = 0; i < kConsumers; ++i)
    {
        consumers.emplace_back([&]()
        {
            while (consumed_count.load(std::memory_order_acquire)
                   < kTotalValues)
            {
                int value = 0;
                if (queue.dequeue(value))
                {
                    consumed_sum.fetch_add(value,
                        std::memory_order_relaxed);
                    consumed_count.fetch_add(1,
                        std::memory_order_release);
                }
                else
                {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (auto& p : producers) p.join();
    for (auto& c : consumers) c.join();

    EXPECT_EQ(consumed_count.load(), kTotalValues);
    EXPECT_EQ(consumed_sum.load(), expected_sum);
    EXPECT_TRUE(queue.empty());
}

TEST(LockFreeQueueTest, ConcurrentStress)
{
    constexpr int kThreads = 8;
    constexpr int kOpsPerThread = 5000;

    LockFreeQueue<int> queue;
    std::atomic<long long> sum{0};
    std::atomic<int> count{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&, t]()
        {
            // 半数入队、半数出队
            if (t % 2 == 0)
            {
                for (int i = 0; i < kOpsPerThread; ++i)
                {
                    queue.enqueue(t * kOpsPerThread + i);
                }
            }
            else
            {
                int consumed = 0;
                while (consumed < kOpsPerThread)
                {
                    int v = 0;
                    if (queue.dequeue(v))
                    {
                        sum.fetch_add(v,
                            std::memory_order_relaxed);
                        ++consumed;
                    }
                    else
                    {
                        std::this_thread::yield();
                    }
                }
                count.fetch_add(consumed,
                    std::memory_order_relaxed);
            }
        });
    }

    for (auto& t : threads) t.join();

    // 验证 4 个消费者各消费了 kOpsPerThread 个
    EXPECT_EQ(count.load(), kOpsPerThread * kThreads / 2);
}

// ============================================================
// 移动语义
// ============================================================

TEST(LockFreeQueueTest, MoveOnlyElement)
{
    // unique_ptr 不可复制但可移动，LockFreeQueue 通过 enqueue
    // (Args&&...) 支持移动语义
    LockFreeQueue<std::unique_ptr<int>> queue;

    queue.enqueue(std::make_unique<int>(42));
    queue.enqueue(std::make_unique<int>(99));

    std::unique_ptr<int> value;
    EXPECT_TRUE(queue.dequeue(value));
    EXPECT_EQ(*value, 42);
    EXPECT_TRUE(queue.dequeue(value));
    EXPECT_EQ(*value, 99);
}
