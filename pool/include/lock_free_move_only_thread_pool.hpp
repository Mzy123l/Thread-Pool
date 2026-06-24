#pragma once
#include "lock_free_queue.hpp"
#include <atomic>
#include <thread>
#include <functional>     
#include <utility>
#include <vector>
#include <future>
#include <type_traits>
#include <memory>
#include <algorithm>
#include <stdexcept>

namespace thread_pool
{
    /// @brief 无锁线程池（支持 move-only 任务，使用 std::move_only_function）
    /// @tparam TaskAllocator 任务分配器，默认分配 move_only_function<void()>
    template<typename TaskAllocator = std::allocator<std::move_only_function<void()>>>
    class LockFreeMoveOnlyThreadPool
    {
    private:
        using Task = std::move_only_function<void()>;

        using TaskAllocTraits = std::allocator_traits<TaskAllocator>;
        using NodeType = lock_free_container::inner_queue::LockFreeNode<Task>;
        using NodeAllocator = typename TaskAllocTraits::template rebind_alloc<NodeType>;
        using NodeAllocTraits = std::allocator_traits<NodeAllocator>;
        using TaskQueue = lock_free_container::LockFreeQueue<Task, NodeAllocator>;

        TaskQueue taskQueue_;
        std::vector<std::thread> workers_;
        std::atomic<bool> stop_{ false };
        std::atomic<std::size_t> activeTasks_{ 0 };
        std::atomic<std::size_t> totalTasks_{ 0 };
        std::atomic<std::size_t> completedTasks_{ 0 };

        TaskAllocator taskAllocator_;

    public:
        explicit LockFreeMoveOnlyThreadPool(
            std::size_t numThreads = std::thread::hardware_concurrency(),
            const TaskAllocator& alloc = TaskAllocator())
            : taskQueue_(NodeAllocator(alloc)), taskAllocator_(alloc)
        {
            workers_.reserve(numThreads);
            for (size_t i = 0; i < numThreads; ++i)
            {
                workers_.emplace_back([this] { this->workerThread(); });
            }
        }

        LockFreeMoveOnlyThreadPool(const LockFreeMoveOnlyThreadPool&) = delete;
        LockFreeMoveOnlyThreadPool& operator=(const LockFreeMoveOnlyThreadPool&) = delete;
        LockFreeMoveOnlyThreadPool(LockFreeMoveOnlyThreadPool&&) = delete;
        LockFreeMoveOnlyThreadPool& operator=(LockFreeMoveOnlyThreadPool&&) = delete;

        ~LockFreeMoveOnlyThreadPool()
        {
            shutdown();
        }

        template<typename Func, typename... Args>
        auto submit(Func&& func, Args&&... args)
            -> std::future<typename std::invoke_result<Func, Args...>::type>
        {
            using ReturnType = typename std::invoke_result<Func, Args...>::type;
            using PackagedTask = std::packaged_task<ReturnType()>;
            using PackagedTaskAllocator =
                typename TaskAllocTraits::template rebind_alloc<PackagedTask>;

            auto task = std::allocate_shared<PackagedTask>(
                PackagedTaskAllocator(taskAllocator_),
                std::bind(std::forward<Func>(func), std::forward<Args>(args)...));

            std::future<ReturnType> result = task->get_future();

            Task wrapper = [task]() { (*task)(); };

            totalTasks_.fetch_add(1, std::memory_order_relaxed);
            activeTasks_.fetch_add(1, std::memory_order_relaxed);

            if (!taskQueue_.enqueue(std::move(wrapper)))
            {
                activeTasks_.fetch_sub(1, std::memory_order_relaxed);
                throw std::runtime_error("Failed to enqueue task");
            }

            return result;
        }

        void wait_all()
        {
            while (activeTasks_.load(std::memory_order_acquire) > 0)
            {
                std::this_thread::yield();
            }
        }

        void shutdown()
        {
            if (!stop_.exchange(true, std::memory_order_release))
            {
                for (auto& worker : workers_)
                {
                    if (worker.joinable())
                        worker.join();
                }
            }
        }

        void shutdown_now()
        {
            stop_.store(true, std::memory_order_release);
            taskQueue_.clear();
            for (auto& worker : workers_)
            {
                if (worker.joinable())
                    worker.join();
            }
        }

        std::size_t active_count() const
        {
            return activeTasks_.load(std::memory_order_relaxed);
        }

        std::size_t total_count() const
        {
            return totalTasks_.load(std::memory_order_relaxed);
        }

        std::size_t completed_count() const
        {
            return completedTasks_.load(std::memory_order_relaxed);
        }

        std::size_t thread_count() const
        {
            return workers_.size();
        }

        TaskAllocator get_allocator() const noexcept
        {
            return taskAllocator_;
        }

    private:
        void workerThread()
        {
            while (!stop_.load(std::memory_order_acquire) ||
                activeTasks_.load(std::memory_order_acquire) > 0)
            {
                Task task;
                if (taskQueue_.dequeue(task))
                {
                    try
                    {
                        task();
                    }
                    catch (...) {}
                    completedTasks_.fetch_add(1, std::memory_order_relaxed);
                    activeTasks_.fetch_sub(1, std::memory_order_relaxed);
                }
                else
                {
                    std::this_thread::yield();
                }
            }
        }
    };
}