#pragma once
#include <atomic>
#include <memory>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <new>
#include <concepts>

namespace lock_free_container
{
    namespace inner_queue
    {
        // 定义128位原子结构体，用于双字CAS
        struct alignas(16) DoubleWord
        {
            void* ptr;        // 64位指针
            uint64_t tag;     // 64位标记（用于避免ABA问题）

            DoubleWord() : ptr(nullptr), tag(0) {}
            DoubleWord(void* p, uint64_t t) : ptr(p), tag(t) {}

            bool operator==(const DoubleWord& other) const
            {
                return ptr == other.ptr && tag == other.tag;
            }

            bool operator!=(const DoubleWord& other) const
            {
                return !(*this == other);
            }
        };

        // 无锁队列节点
        template<typename T>
        struct LockFreeNode
        {
            T data;
            std::atomic<LockFreeNode<T>*> next;

            template<typename... Args>
            explicit LockFreeNode(Args&&... args)
                : data(std::forward<Args>(args)...), next(nullptr)
            {}

            LockFreeNode(const LockFreeNode&) = delete;
            LockFreeNode& operator=(const LockFreeNode&) = delete;
        };

        // 默认分配器
        template<typename T>
        struct DefaultAllocator
        {
            using value_type = T;
            using size_type = size_t;
            using difference_type = ptrdiff_t;

            template<typename U>
            struct rebind
            {
                using other = DefaultAllocator<U>;
            };

            DefaultAllocator() noexcept = default;
            template<typename U> DefaultAllocator(const DefaultAllocator<U>&) noexcept {}

            T* allocate(size_t n)
            {
                return static_cast<T*>(::operator new(n * sizeof(T)));
            }

            void deallocate(T* p, size_t) noexcept
            {
                ::operator delete(p);
            }
        };
    }

    // 无锁队列
    /// @brief 链表实现
    template<typename T, typename Allocator = inner_queue::DefaultAllocator<T>>
    class LockFreeQueue
    {
    private:
        // 分配器相关类型
        using Node = inner_queue::LockFreeNode<T>;
        using NodeAllocator = typename std::allocator_traits<Allocator>::template rebind_alloc<Node>;
        using AllocTraits = std::allocator_traits<NodeAllocator>;

        // 头指针（带标记）
        alignas(16) std::atomic<inner_queue::DoubleWord> head_;

        // 尾指针（带标记）
        alignas(16) std::atomic<inner_queue::DoubleWord> tail_;

        // 分配器
        NodeAllocator allocator_;
    public:
        // 构造函数
        LockFreeQueue(const Allocator& alloc = Allocator()) :allocator_(alloc)
        {
            // 创建哨兵节点
            Node* dummy = allocateNode(T());

            // 初始化头尾指针指向哨兵节点
            inner_queue::DoubleWord init(dummy, 0);
            head_.store(init, std::memory_order_relaxed);
            tail_.store(init, std::memory_order_relaxed);
        }

        // 拷贝构造函数
        LockFreeQueue(const LockFreeQueue&) = delete;

        // 移动构造函数
        LockFreeQueue(LockFreeQueue&& other) noexcept
            : head_(other.head_.load(std::memory_order_relaxed)),
            tail_(other.tail_.load(std::memory_order_relaxed)),
            allocator_(std::move(other.allocator_))
        {

            // 重置原队列
            Node* dummy = allocateNode(T());
            inner_queue::DoubleWord init(dummy, 0);
            other.head_.store(init, std::memory_order_relaxed);
            other.tail_.store(init, std::memory_order_relaxed);
        }
        // 赋值运算符
        LockFreeQueue& operator=(const LockFreeQueue&) = delete;
        LockFreeQueue& operator=(LockFreeQueue&&) = delete;

        ~LockFreeQueue()
        {
            clear();

            // 删除哨兵节点
            Node* dummy = static_cast<Node*>(head_.load(std::memory_order_relaxed).ptr);
            deallocateNode(dummy);
        }
        // 入队
        template <typename... Args>
            requires std::constructible_from<T, Args...>
        bool enqueue(Args&&... args)
        {
            // 创建新节点
            Node* newNode = allocateNode(std::forward<Args>(args)...);

            while (true)
            {
                // 获取当前尾指针
                inner_queue::DoubleWord tail = tail_.load(std::memory_order_acquire);
                Node* tailPtr = static_cast<Node*>(tail.ptr);

                // 获取尾节点的next指针
                Node* next = tailPtr->next.load(std::memory_order_acquire);

                // 验证tail是否仍然有效
                inner_queue::DoubleWord currentTail = tail_.load(std::memory_order_acquire);
                if (tail != currentTail)
                {
                    continue; // 已被其他线程修改，重试
                }

                if (next == nullptr)
                {
                    // 尝试将新节点链接到尾部
                    if (tailPtr->next.compare_exchange_weak(
                        next, newNode,
                        std::memory_order_release,
                        std::memory_order_relaxed))
                    {

                        // 尝试更新尾指针
                        inner_queue::DoubleWord newTail(newNode, tail.tag + 1);
                        tail_.compare_exchange_strong(
                            tail, newTail,
                            std::memory_order_release,
                            std::memory_order_relaxed);

                        return true;
                    }
                }
                else
                {
                    // 帮助其他线程完成尾指针更新
                    inner_queue::DoubleWord newTail(next, tail.tag + 1);
                    tail_.compare_exchange_strong(
                        tail, newTail,
                        std::memory_order_release,
                        std::memory_order_relaxed);
                }
            }
        }


        // 出队
        bool dequeue(T& data)
        {
            while (true)
            {
                // 获取头指和尾指针
                inner_queue::DoubleWord head = head_.load(std::memory_order_acquire);
                inner_queue::DoubleWord tail = tail_.load(std::memory_order_acquire);

                Node* headPtr = static_cast<Node*>(head.ptr);
                Node* tailPtr = static_cast<Node*>(tail.ptr);
                 
                // 获取头节点的下一个节点
                Node* next = headPtr->next.load(std::memory_order_acquire);

                // 验证head是否仍然有效
                inner_queue::DoubleWord currentHead = head_.load(std::memory_order_acquire);
                if (head != currentHead)
                {
                    continue; // 已被其他线程修改，重试
                }

                // 检查队列是否为空
                if (headPtr == tailPtr)
                {
                    if (next == nullptr)
                    {
                        return false; // 队列为空
                    }

                    // 队列非空，但尾指针落后，帮助更新
                    inner_queue::DoubleWord newTail(next, tail.tag + 1);
                    tail_.compare_exchange_strong(
                        tail, newTail,
                        std::memory_order_release,
                        std::memory_order_relaxed);
                }
                else
                {
                    if (next == nullptr)
                    {
                        continue; // 下一个节点为空，重试
                    }

                    // 读取数据
                    data = std::move(next->data);

                    // 尝试更新头指针
                    inner_queue::DoubleWord newHead(next, head.tag + 1);
                    if (head_.compare_exchange_weak(
                        head, newHead,
                        std::memory_order_release,
                        std::memory_order_relaxed))
                    {

                        // 释放旧头节点
                        deallocateNode(headPtr);
                        return true;
                    }
                }
            }
        }
        // 清空队列
        void clear()
        {
            T dummy;
            while (dequeue(dummy))
            {
                // 不断出队直到队列为空
            }
        }

        // 检查队列是否为空
        bool empty() const
        {
            inner_queue::DoubleWord head = head_.load(std::memory_order_acquire);
            inner_queue::DoubleWord tail = tail_.load(std::memory_order_acquire);

            Node* headPtr = static_cast<Node*>(head.ptr);
            Node* tailPtr = static_cast<Node*>(tail.ptr);

            return headPtr == tailPtr &&
                headPtr->next.load(std::memory_order_acquire) == nullptr;
        }

        // 获取分配器
        NodeAllocator getAllocator() const noexcept
        {
            return allocator_;
        }

    private:
        // 分配节点
        template<typename... Args>
        Node* allocateNode(Args&&... args)
        {
            Node* node = AllocTraits::allocate(allocator_, 1);
            try
            {
                AllocTraits::construct(allocator_, node, std::forward<Args>(args)...);
            }
            catch (...)
            {
                AllocTraits::deallocate(allocator_, node, 1);
                throw;
            }
            return node;
        }
    
        // 释放节点
        void deallocateNode(Node* node) noexcept
        {
            if (node)
            {
                AllocTraits::destroy(allocator_, node);
                AllocTraits::deallocate(allocator_, node, 1);
            }
        }
    
        static Node* getPtr(const inner_queue::DoubleWord& dw)
        {
            return static_cast<Node*>(dw.ptr);
        }

    };

    
}