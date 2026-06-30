// ============================================================
// test_ring_queue.cpp — 环形无锁队列 GoogleTest 单元测试
// ============================================================

#include "../include/lock_free_ring_queue.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using lock_free_container::LockFreeRingQueue;

// 测试用容量
constexpr std::size_t kSmallCap = 4;
constexpr std::size_t kLargeCap = 1024;

// ============================================================
// 基本功能测试
// ============================================================

TEST(RingQueueTest, FifoOrder)
{
    LockFreeRingQueue<int, kLargeCap> queue;

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

TEST(RingQueueTest, Capacity)
{
    LockFreeRingQueue<int, 256> queue;
    EXPECT_EQ(queue.capacity(), 256);
}

TEST(RingQueueTest, FullQueueReturnsFalse)
{
    LockFreeRingQueue<int, kSmallCap> queue;

    // 填满队列
    for (std::size_t i = 0; i < kSmallCap; ++i)
    {
        EXPECT_TRUE(queue.enqueue(static_cast<int>(i)));
    }
    EXPECT_FALSE(queue.empty());

    // 再入队应该失败
    EXPECT_FALSE(queue.enqueue(999));
}

TEST(RingQueueTest, EmptyDequeueReturnsFalse)
{
    LockFreeRingQueue<int, kSmallCap> queue;

    int value = -1;
    EXPECT_FALSE(queue.dequeue(value));
    EXPECT_EQ(value, -1);
}

TEST(RingQueueTest, WrapAround)
{
    LockFreeRingQueue<int, kSmallCap> queue;

    // 多次环绕填满-清空循环
    for (int round = 0; round < 10; ++round)
    {
        // 入队 kSmallCap 个
        for (std::size_t i = 0; i < kSmallCap; ++i)
        {
            int v = round * static_cast<int>(kSmallCap) + static_cast<int>(i);
            EXPECT_TRUE(queue.enqueue(v));
        }

        // 出队 kSmallCap 个
        for (std::size_t i = 0; i < kSmallCap; ++i)
        {
            int value = 0;
            EXPECT_TRUE(queue.dequeue(value));
            EXPECT_EQ(value,
                round * static_cast<int>(kSmallCap) + static_cast<int>(i));
        }

        EXPECT_TRUE(queue.empty());
    }
}

TEST(RingQueueTest, ClearAndReuse)
{
    LockFreeRingQueue<int, kLargeCap> queue;

    for (int i = 0; i < 500; ++i) { queue.enqueue(i); }
    EXPECT_FALSE(queue.empty());
    queue.clear();
    EXPECT_TRUE(queue.empty());

    // 清空后可重新使用
    EXPECT_TRUE(queue.enqueue(42));
    int value = 0;
    EXPECT_TRUE(queue.dequeue(value));
    EXPECT_EQ(value, 42);
}

// ============================================================
// 边界测试
// ============================================================

TEST(RingQueueTest, SmallCapacity)
{
    LockFreeRingQueue<int, 2> queue;

    EXPECT_TRUE(queue.enqueue(1));
    EXPECT_TRUE(queue.enqueue(2));
    EXPECT_FALSE(queue.enqueue(3));  // 满

    int v = 0;
    EXPECT_TRUE(queue.dequeue(v)); EXPECT_EQ(v, 1);
    EXPECT_TRUE(queue.enqueue(3));  // 可继续入队
    EXPECT_TRUE(queue.dequeue(v)); EXPECT_EQ(v, 2);
    EXPECT_TRUE(queue.dequeue(v)); EXPECT_EQ(v, 3);
}

TEST(RingQueueTest, LargeCapacity)
{
    constexpr std::size_t N = 16384;
    LockFreeRingQueue<int, N> queue;

    for (std::size_t i = 0; i < N; ++i)
    {
        EXPECT_TRUE(queue.enqueue(static_cast<int>(i)));
    }
    EXPECT_FALSE(queue.enqueue(0));  // 满

    for (std::size_t i = 0; i < N; ++i)
    {
        int v = 0;
        EXPECT_TRUE(queue.dequeue(v));
        EXPECT_EQ(v, static_cast<int>(i));
    }
    EXPECT_TRUE(queue.empty());
}

// ============================================================
// 非平凡类型
// ============================================================

TEST(RingQueueTest, StringType)
{
    LockFreeRingQueue<std::string, 256> queue;

    queue.enqueue("hello");
    queue.enqueue(std::string("world"));

    std::string value;
    EXPECT_TRUE(queue.dequeue(value));
    EXPECT_EQ(value, "hello");
    EXPECT_TRUE(queue.dequeue(value));
    EXPECT_EQ(value, "world");
}

TEST(RingQueueTest, MoveOnlyType)
{
    LockFreeRingQueue<std::unique_ptr<int>, 256> queue;

    queue.enqueue(std::make_unique<int>(42));
    queue.enqueue(std::make_unique<int>(99));

    std::unique_ptr<int> value;
    EXPECT_TRUE(queue.dequeue(value));
    EXPECT_EQ(*value, 42);
    EXPECT_TRUE(queue.dequeue(value));
    EXPECT_EQ(*value, 99);
}

// ============================================================
// 并发测试
// ============================================================

TEST(RingQueueTest, ConcurrentMPMC)
{
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr int kValuesPerProducer = 1000;
    constexpr int kTotalValues = kProducers * kValuesPerProducer;

    LockFreeRingQueue<int, 8192> queue;
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
                // 自旋入队（环形队列满时重试）
                while (!queue.enqueue(v))
                {
                    std::this_thread::yield();
                }
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
}

TEST(RingQueueTest, ConcurrentStressSmall)
{
    // 小容量高并发——极端压力场景（降低规格以适配 8GB RAM）
    constexpr int kThreads = 4;
    constexpr int kOpsPerThread = 500;
    constexpr int kTotalOps = kThreads / 2 * kOpsPerThread;

    LockFreeRingQueue<int, 2> queue;
    std::atomic<long long> sum{0};
    std::atomic<int> done{0};
    std::atomic<bool> stop{false};

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&, t]()
        {
            if (t % 2 == 0)
            {
                // 生产者
                for (int i = 0; i < kOpsPerThread && !stop.load();)
                {
                    if (queue.enqueue(t * kOpsPerThread + i))
                    {
                        ++i;
                    }
                    else
                    {
                        std::this_thread::yield();
                    }
                }
            }
            else
            {
                // 消费者
                int consumed = 0;
                while (consumed < kOpsPerThread && !stop.load())
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
                done.fetch_add(consumed,
                    std::memory_order_relaxed);
            }
        });
    }

    for (auto& t : threads) t.join();

    // 所有消费者完成
    EXPECT_EQ(done.load(), kTotalOps);
}
