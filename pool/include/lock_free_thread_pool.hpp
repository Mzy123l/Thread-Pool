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
    /// @brief 无锁线程池
    /// @tparam TaskAllocator 任务分配器
    template<typename TaskAllocator = std::allocator<std::function<void()>>>
    class LockFreeThreadPool
    {
    private:
        using Task = std::function<void()>;
        using TaskAllocTraits = std::allocator_traits<TaskAllocator>;
        using NodeType = lock_free_container::inner_queue::LockFreeNode<Task>;
        using NodeAllocator = typename TaskAllocTraits::template rebind_alloc<NodeType>;
        using NodeAllocTraits = std::allocator_traits<NodeAllocator>;
        using TaskQueue = lock_free_container::LockFreeQueue<std::function<void()>, NodeAllocator>;

        TaskQueue taskQueue_; // 无锁任务队列
        std::vector<std::thread> workers_; // 工作线程
        std::atomic<bool> stop_{false};  // 停止标志
        std::atomic<std::size_t> activeTasks_{ 0 }; // 活跃任务计数
        std::atomic<std::size_t> totalTasks_{ 0 }; // 总任务计数
        std::atomic<std::size_t> completedTasks_{ 0 }; // 完成任务计数

        // 存储分配器实例
        TaskAllocator taskAllocator_;

    public:
        // 构造函数
        // 禁止隐式转换
        // @param numThreads 线程数 默认为硬件并发线程数
        // @param alloc为任务分配器实例
        explicit LockFreeThreadPool(std::size_t numThreads = std::thread::hardware_concurrency(), const TaskAllocator& alloc = TaskAllocator())
            : taskQueue_(NodeAllocator(alloc)), taskAllocator_(alloc)
        {
            workers_.reserve(numThreads);
            for (size_t i = 0; i < numThreads; ++i)
            {
                workers_.emplace_back([this] { this->workerThread(); });
            }

        }

        // 禁止拷贝构造 
        LockFreeThreadPool(const LockFreeThreadPool&) = delete;
        // 禁止拷贝赋值
        LockFreeThreadPool& operator=(const LockFreeThreadPool&) = delete;
        // 禁止移动构造
        LockFreeThreadPool(LockFreeThreadPool&&) = delete;
        // 禁止移动赋值
        LockFreeThreadPool& operator=(LockFreeThreadPool&&) = delete;
        
        // 析构函数
        ~LockFreeThreadPool()
        {
            shutdown();
        }

        // 提交任务
        /// @tparam Func 可调用对象类型
        /// @tparam Args 参数类型包
        /// @param func 可调用对象
        /// @param args 参数包
        /// @return std::future<typename std::invoke_result<Func, Args...>::type>
        template<typename Func, typename... Args>
        auto submit(Func&& func, Args&&... args) -> std::future<typename std::invoke_result<Func, Args...>::type>
        {
            using ReturnType = typename std::invoke_result<Func, Args...>::type;
            using PackagedTask = std::packaged_task<ReturnType()>;
            using PackagedTaskAllocator = typename TaskAllocTraits::template rebind_alloc<PackagedTask>;

            // 创建任务包装器
            auto task = 
                std::allocate_shared<PackagedTask>(PackagedTaskAllocator(taskAllocator_), std::bind(std::forward<Func>(func), std::forward<Args>(args)...));

            // 获取future
            std::future<ReturnType> result = task->get_future();

            // 包装任务到void()函数
            Task wrapper = [task]() { (*task)(); };
            
            // 提交任务
            totalTasks_.fetch_add(1, std::memory_order_relaxed);
            activeTasks_.fetch_add(1, std::memory_order_relaxed);

            if (!taskQueue_.enqueue(std::move(wrapper)))
            {
                activeTasks_.fetch_sub(1, std::memory_order_relaxed);
                throw std::runtime_error("Failed to enqueue task");
            }

            return result;
        }

        // 等待所有任务完成
        void wait_all()
        {
            while (activeTasks_.load(std::memory_order_acquire) > 0)
            {
                std::this_thread::yield();
            }
        }


        // 优雅关闭
        void shutdown()
        {
            if (!stop_.exchange(true, std::memory_order_release))
            {
                // 唤醒所有等待的线程
                for (auto& worker : workers_)
                {
                    if (worker.joinable())
                    {
                        worker.join();
                    }
                }
            }
        }
        // 立即关闭
        void shutdown_now()
        {
            stop_.store(true, std::memory_order_release);

            // 清空任务队列
            taskQueue_.clear();

            // 等待线程结束
            for (auto& worker : workers_)
            {
                if (worker.joinable())
                {
                    worker.join();
                }
            }
        }

        // 获取活跃任务数
        std::size_t active_count() const
        {
            return activeTasks_.load(std::memory_order_relaxed);
        }

        // 获取总任务数
        std::size_t total_count() const
        {
            return totalTasks_.load(std::memory_order_relaxed);
        }

        // 获取完成任务数
        std::size_t completed_count() const
        {
            return completedTasks_.load(std::memory_order_relaxed);
        }

        // 获取线程数
        std::size_t thread_count() const
        {
            return workers_.size();
        }

        // 获取分配器
        TaskAllocator get_allocator() const noexcept
        {
            return taskAllocator_;
        }
    private:
        // 工作线程函数
        void workerThread()
        {
            while (!stop_.load(std::memory_order_acquire) || activeTasks_.load(std::memory_order_acquire) > 0)
            {
                Task task;
                if (taskQueue_.dequeue(task))
                {
                    try
                    {
                        task();
                    }
                    catch (...)
                    {
                        // 处理任务执行中的异常
                    }
                    completedTasks_.fetch_add(1, std::memory_order_relaxed);
                    activeTasks_.fetch_sub(1, std::memory_order_relaxed);
                }
                else
                {
                    // 没有任务，休息一下
                    std::this_thread::yield();
                }
            }
        }

    };
} 