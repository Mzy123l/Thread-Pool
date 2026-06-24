#include "pool/include/lock_free_queue.hpp"
#include "pool/include/lock_free_thread_pool.hpp"

#include <atomic>
#include <cassert>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using lock_free_container::LockFreeQueue;
using lock_free::DynamicMoveOnlyPool;
using lock_free::DynamicPool;
using lock_free::StaticPool;

class CustomTask
{
public:
    CustomTask() = default;

    template<typename Func>
    CustomTask(Func&& func)
        : func_(std::forward<Func>(func))
    {
    }

    void operator()()
    {
        func_();
    }

private:
    std::function<void()> func_;
};

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
    using DefaultTaskAllocator = std::allocator<std::function<void()>>;
    DynamicPool<DefaultTaskAllocator> pool(4);

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

void test_thread_pool_custom_func_type()
{
    StaticPool<CustomTask> pool(2);

    std::atomic<int> value{0};
    pool.submit(CustomTask([&value] {
        value.store(42, std::memory_order_release);
    }));
    pool.wait_all();
    assert(pool.completed_count() == 1);
    assert(value.load(std::memory_order_acquire) == 42);
    pool.shutdown();
}

void test_thread_pool_batch_tasks()
{
    DynamicPool<> pool(4);
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
    DynamicPool<> pool(2);

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

std::atomic<int>* staticCounter = nullptr;

void increment_static_counter()
{
    staticCounter->fetch_add(1, std::memory_order_relaxed);
}

void test_thread_pool_static_function_pointer()
{
    StaticPool<void(*)()> pool(2);
    std::atomic<int> counter{0};
    staticCounter = &counter;

    pool.submit_static(&increment_static_counter);
    pool.submit(&increment_static_counter);
    pool.wait_all();

    assert(counter.load(std::memory_order_relaxed) == 2);
    assert(pool.completed_count() == 2);
    pool.shutdown();
    staticCounter = nullptr;
}

#if defined(__cpp_lib_move_only_function) && __cpp_lib_move_only_function >= 202110L
void test_thread_pool_move_only_return_values()
{
    DynamicMoveOnlyPool<> pool(2);

    auto value = pool.submit([] {
        return 123;
    });

    assert(value.get() == 123);
    pool.wait_all();
    assert(pool.completed_count() == 1);
    pool.shutdown();
}
#endif

int main()
{
    test_queue_fifo();
    test_queue_concurrent();
    test_thread_pool_return_values();
    test_thread_pool_custom_func_type();
    test_thread_pool_static_function_pointer();
    test_thread_pool_batch_tasks();
    test_thread_pool_exception_task();
#if defined(__cpp_lib_move_only_function) && __cpp_lib_move_only_function >= 202110L
    test_thread_pool_move_only_return_values();
#endif

    std::cout << "All tests passed.\n";
    return 0;
}
