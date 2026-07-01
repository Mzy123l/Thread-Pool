#include "../include/lock_free_thread_pool.hpp"

#include <atomic>
#include <cassert>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using lock_free_container::LockFreeQueue;
using thread_pool::LockFreeThreadPool;

void test_queue_fifo()
{
    LockFreeQueue<int> queue;

    assert(queue.empty());
    assert(queue.enqueue(1));
    assert(queue.enqueue(2));
    assert(queue.enqueue(3));
    assert(!queue.empty());

    int value = 0;
    assert(queue.dequeue(value) && value == 1);
    assert(queue.dequeue(value) && value == 2);
    assert(queue.dequeue(value) && value == 3);
    assert(!queue.dequeue(value));
    assert(queue.empty());
}

void test_queue_concurrent()
{
    constexpr int producerCount = 4;
    constexpr int consumerCount = 4;
    constexpr int valuesPerProducer = 1000;
    constexpr int totalValues = producerCount * valuesPerProducer;

    LockFreeQueue<int> queue;
    std::atomic<int> consumedCount{0};
    std::atomic<long long> consumedSum{0};
    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;

    const long long expectedSum =
        static_cast<long long>(totalValues - 1) * totalValues / 2;

    for (int p = 0; p < producerCount; ++p)
    {
        producers.emplace_back([&, p]() {
            const int begin = p * valuesPerProducer;
            const int end = begin + valuesPerProducer;
            for (int value = begin; value < end; ++value)
            {
                queue.enqueue(value);
            }
        });
    }

    for (int i = 0; i < consumerCount; ++i)
    {
        consumers.emplace_back([&]() {
            while (consumedCount.load(std::memory_order_acquire) < totalValues)
            {
                int value = 0;
                if (queue.dequeue(value))
                {
                    consumedSum.fetch_add(value, std::memory_order_relaxed);
                    consumedCount.fetch_add(1, std::memory_order_release);
                }
                else
                {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (auto& producer : producers)
    {
        producer.join();
    }

    for (auto& consumer : consumers)
    {
        consumer.join();
    }

    assert(consumedCount.load() == totalValues);
    assert(consumedSum.load() == expectedSum);
    assert(queue.empty());
}

void test_thread_pool_return_values()
{
    LockFreeThreadPool pool(4);

    auto sum = pool.submit([](int a, int b) {
        return a + b;
    }, 10, 32);

    auto text = pool.submit([] {
        return std::string("thread pool");
    });

    assert(sum.get() == 42);
    assert(text.get() == "thread pool");

    pool.wait_all();
    assert(pool.thread_count() == 4);
    assert(pool.total_count() == 2);
    assert(pool.completed_count() == 2);
    assert(pool.active_count() == 0);
    pool.shutdown();
}

void test_thread_pool_batch_tasks()
{
    LockFreeThreadPool pool(4);
    std::atomic<int> counter{0};
    std::vector<std::future<int>> futures;

    for (int i = 1; i <= 100; ++i)
    {
        futures.emplace_back(pool.submit([&, i]() {
            counter.fetch_add(1, std::memory_order_relaxed);
            return i * i;
        }));
    }

    int sum = 0;
    for (auto& future : futures)
    {
        sum += future.get();
    }

    pool.wait_all();

    const int expectedSum = 100 * 101 * 201 / 6;
    assert(sum == expectedSum);
    assert(counter.load() == 100);
    assert(pool.total_count() == 100);
    assert(pool.completed_count() == 100);
    assert(pool.active_count() == 0);
    pool.shutdown();
}

void test_thread_pool_exception_task()
{
    LockFreeThreadPool pool(2);

    auto failed = pool.submit([]() -> int {
        throw std::runtime_error("expected test exception");
    });

    auto ok = pool.submit([] {
        return 7;
    });

    bool caught = false;
    try
    {
        (void)failed.get();
    }
    catch (const std::runtime_error&)
    {
        caught = true;
    }

    assert(caught);
    assert(ok.get() == 7);

    pool.wait_all();
    assert(pool.total_count() == 2);
    assert(pool.completed_count() == 2);
    assert(pool.active_count() == 0);
    pool.shutdown();
}

int main()
{
    test_queue_fifo();
    test_queue_concurrent();
    test_thread_pool_return_values();
    test_thread_pool_batch_tasks();
    test_thread_pool_exception_task();

    std::cout << "All tests passed.\n";
    return 0;
}
