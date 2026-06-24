#pragma once

#include "lock_free_queue.hpp"

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace lock_free
{
    namespace inner_lock_free
    {
        /// @brief Common lock-free worker pool core.
        /// @tparam FuncType Exact task representation stored in the queue.
        /// @tparam Derived CRTP derived pool type.
        /// @tparam TaskAllocator Allocator for FuncType nodes.
        template<
            typename FuncType,
            typename Derived,
            typename TaskAllocator = std::allocator<FuncType>>
        class Base
        {
        protected:
            using Task = FuncType;
            using TaskAllocTraits = std::allocator_traits<TaskAllocator>;
            using NodeType = lock_free_container::inner_queue::LockFreeNode<Task>;
            using NodeAllocator = typename TaskAllocTraits::template rebind_alloc<NodeType>;
            using TaskQueue = lock_free_container::LockFreeQueue<Task, NodeAllocator>;

            TaskQueue taskQueue_;
            std::vector<std::thread> workers_;
            std::atomic<bool> stop_{false};
            std::atomic<std::size_t> activeTasks_{0};
            std::atomic<std::size_t> totalTasks_{0};
            std::atomic<std::size_t> completedTasks_{0};
            TaskAllocator taskAllocator_;

            explicit Base(
                std::size_t numThreads = std::thread::hardware_concurrency(),
                const TaskAllocator& alloc = TaskAllocator())
                : taskQueue_(NodeAllocator(alloc)), taskAllocator_(alloc)
            {
                if (numThreads == 0)
                {
                    numThreads = 1;
                }

                workers_.reserve(numThreads);
                for (std::size_t i = 0; i < numThreads; ++i)
                {
                    workers_.emplace_back([this] { this->workerThread(); });
                }
            }

            ~Base()
            {
                shutdown();
            }

            Base(const Base&) = delete;
            Base& operator=(const Base&) = delete;
            Base(Base&&) = delete;
            Base& operator=(Base&&) = delete;

            void enqueue_task(Task task)
            {
                totalTasks_.fetch_add(1, std::memory_order_relaxed);
                activeTasks_.fetch_add(1, std::memory_order_relaxed);

                if (!taskQueue_.enqueue(std::move(task)))
                {
                    activeTasks_.fetch_sub(1, std::memory_order_relaxed);
                    throw std::runtime_error("Failed to enqueue task");
                }
            }

        public:
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
                        {
                            worker.join();
                        }
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
                    {
                        worker.join();
                    }
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
                    Task task{};
                    if (taskQueue_.dequeue(task))
                    {
                        try
                        {
                            task();
                        }
                        catch (...)
                        {
                            // Exceptions are transported by packaged_task for
                            // dynamic pools. Raw static tasks are intentionally
                            // fire-and-forget.
                        }

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
    } // namespace inner_lock_free

    /// @brief Dynamic task pool backed by std::function<void()>.
    /// This keeps the old fully generic submit surface, but uses type erasure.
    template<typename TaskAllocator = std::allocator<std::function<void()>>>
    class DynamicPool
        : public inner_lock_free::Base<
              std::function<void()>,
              DynamicPool<TaskAllocator>,
              TaskAllocator>
    {
        using Task = std::function<void()>;
        using Base = inner_lock_free::Base<Task, DynamicPool<TaskAllocator>, TaskAllocator>;

    public:
        explicit DynamicPool(
            std::size_t numThreads = std::thread::hardware_concurrency(),
            const TaskAllocator& alloc = TaskAllocator())
            : Base(numThreads, alloc)
        {
        }

        template<typename Func, typename... Args>
        auto submit(Func&& func, Args&&... args)
            -> std::future<std::invoke_result_t<Func, Args...>>
        {
            using ReturnType = std::invoke_result_t<Func, Args...>;
            using PackagedTask = std::packaged_task<ReturnType()>;

            auto task = std::make_shared<PackagedTask>(
                std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
            auto result = task->get_future();

            this->enqueue_task(Task([task] { (*task)(); }));
            return result;
        }
    };

#if defined(__cpp_lib_move_only_function) && __cpp_lib_move_only_function >= 202110L
    /// @brief Dynamic task pool backed by std::move_only_function<void()>.
    /// Avoids the shared_ptr hop required by std::function + packaged_task.
    template<typename TaskAllocator = std::allocator<std::move_only_function<void()>>>
    class DynamicMoveOnlyPool
        : public inner_lock_free::Base<
              std::move_only_function<void()>,
              DynamicMoveOnlyPool<TaskAllocator>,
              TaskAllocator>
    {
        using Task = std::move_only_function<void()>;
        using Base = inner_lock_free::Base<Task, DynamicMoveOnlyPool<TaskAllocator>, TaskAllocator>;

    public:
        explicit DynamicMoveOnlyPool(
            std::size_t numThreads = std::thread::hardware_concurrency(),
            const TaskAllocator& alloc = TaskAllocator())
            : Base(numThreads, alloc)
        {
        }

        template<typename Func, typename... Args>
        auto submit(Func&& func, Args&&... args)
            -> std::future<std::invoke_result_t<Func, Args...>>
        {
            using ReturnType = std::invoke_result_t<Func, Args...>;

            std::packaged_task<ReturnType()> task(
                std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
            auto result = task.get_future();

            this->enqueue_task(Task([task = std::move(task)]() mutable { task(); }));
            return result;
        }
    };
#endif

    /// @brief Static task pool with no type-erased task wrapper.
    /// @tparam FuncType Exact task type, for example void(*)().
    /// StaticPool stores FuncType directly; different callable types require
    /// different StaticPool<FuncType> instantiations.
    template<
        typename FuncType,
        typename TaskAllocator = std::allocator<FuncType>>
    class StaticPool
        : public inner_lock_free::Base<
              FuncType,
              StaticPool<FuncType, TaskAllocator>,
              TaskAllocator>
    {
        using Base = inner_lock_free::Base<
            FuncType,
            StaticPool<FuncType, TaskAllocator>,
            TaskAllocator>;

    public:
        explicit StaticPool(
            std::size_t numThreads = std::thread::hardware_concurrency(),
            const TaskAllocator& alloc = TaskAllocator())
            : Base(numThreads, alloc)
        {
        }

        void submit_static(FuncType task)
        {
            this->enqueue_task(std::move(task));
        }

        void submit(FuncType task)
        {
            submit_static(std::move(task));
        }
    };
} // namespace lock_free

namespace thread_pool
{
    template<typename TaskAllocator = std::allocator<std::function<void()>>>
    using LockFreeThreadPool = lock_free::DynamicPool<TaskAllocator>;
} // namespace thread_pool
