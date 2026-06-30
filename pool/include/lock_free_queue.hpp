#pragma once
#include "lock_free_utility.hpp"

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
            T* data;
            std::atomic<LockFreeNode<T>*> next;

            explicit LockFreeNode(T* d)
                : data(d), next(nullptr)
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
        using Node = inner_queue::LockFreeNode<T>;
        using NodeAllocator = typename std::allocator_traits<Allocator>::template rebind_alloc<Node>;
        using AllocTraits = std::allocator_traits<NodeAllocator>;

        // 针对数据值 T 的分配器
        using ValueAllocator = typename std::allocator_traits<Allocator>::template rebind_alloc<T>;
        using ValueAllocTraits = std::allocator_traits<ValueAllocator>;

        alignas(16) std::atomic<inner_queue::DoubleWord> head_;
        alignas(16) std::atomic<inner_queue::DoubleWord> tail_;
        NodeAllocator allocator_;
        ValueAllocator valueAllocator_;

        static thread_local Node* toDeallocate_;

    public:
        LockFreeQueue(const Allocator& alloc = Allocator())
            : allocator_(alloc), valueAllocator_(alloc)
        {
            Node* dummy = allocateNode(nullptr); // 初始化使用空指针
            inner_queue::DoubleWord init(dummy, 0);
            head_.store(init, std::memory_order_relaxed);
            tail_.store(init, std::memory_order_relaxed);
        }

        LockFreeQueue(const LockFreeQueue&) = delete;
        LockFreeQueue(LockFreeQueue&& other) noexcept
            : head_(other.head_.load(std::memory_order_relaxed)),
            tail_(other.tail_.load(std::memory_order_relaxed)),
            allocator_(std::move(other.allocator_)),
            valueAllocator_(std::move(other.valueAllocator_))
        {
            Node* dummy = allocateNode(nullptr);
            inner_queue::DoubleWord init(dummy, 0);
            other.head_.store(init, std::memory_order_relaxed);
            other.tail_.store(init, std::memory_order_relaxed);
        }

        LockFreeQueue& operator=(const LockFreeQueue&) = delete;
        LockFreeQueue& operator=(LockFreeQueue&&) = delete;

        ~LockFreeQueue()
        {
            clear();
            Node* dummy = static_cast<Node*>(head_.load(std::memory_order_relaxed).ptr);
            deallocateNode(dummy);

            if (toDeallocate_)
            {
                deallocateNode(toDeallocate_);
                toDeallocate_ = nullptr;
            }
        }

        // 入队
        template <typename... Args>
            requires std::constructible_from<T, Args...>
        bool enqueue(Args&&... args)
        {
            // 1. 先用 Allocator 独立构造出数据 (T*)
            T* newData = ValueAllocTraits::allocate(valueAllocator_, 1);
            try
            {
                ValueAllocTraits::construct(valueAllocator_, newData, std::forward<Args>(args)...);
            }
            catch (...)
            {
                ValueAllocTraits::deallocate(valueAllocator_, newData, 1);
                throw;
            }

            // 2. 把指针装载到 Node 中
            Node* newNode;
            try
            {
                newNode = allocateNode(newData);
            }
            catch (...)
            {
                ValueAllocTraits::destroy(valueAllocator_, newData);
                ValueAllocTraits::deallocate(valueAllocator_, newData, 1);
                throw;
            }

            while (true)
            {
                inner_queue::DoubleWord tail = tail_.load(std::memory_order_acquire);
                Node* tailPtr = static_cast<Node*>(tail.ptr);

                Node* next = tailPtr->next.load(std::memory_order_acquire);

                if (POOL_UNLIKELY(tail != tail_.load(std::memory_order_acquire)))
                {
                    continue;
                }

                if (POOL_LIKELY(next == nullptr))
                {
                    if (POOL_LIKELY(tailPtr->next.compare_exchange_weak(
                        next, newNode,
                        std::memory_order_release,
                        std::memory_order_relaxed)))
                    {
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
                inner_queue::DoubleWord tail = tail_.load(std::memory_order_acquire);
                inner_queue::DoubleWord head = head_.load(std::memory_order_acquire);
                Node* headPtr = static_cast<Node*>(head.ptr);
                Node* tailPtr = static_cast<Node*>(tail.ptr);

                Node* next = headPtr->next.load(std::memory_order_acquire);

                if (POOL_UNLIKELY(head != head_.load(std::memory_order_acquire)))
                {
                    continue;
                }

                if (POOL_UNLIKELY(headPtr == tailPtr))
                {
                    if (POOL_UNLIKELY(next == nullptr))
                    {
                        if (POOL_UNLIKELY(tail != tail_.load(std::memory_order_acquire)))
                        {
                            continue;
                        }
                        return false;
                    }
                    // 帮助推进尾部（队列不为空但 tail 落后）
                    inner_queue::DoubleWord newTail(next, tail.tag + 1);
                    tail_.compare_exchange_strong(
                        tail, newTail,
                        std::memory_order_release,
                        std::memory_order_relaxed);
                }
                else
                {
                    if (POOL_UNLIKELY(next == nullptr))
                    {
                        // 瞬态：head 已推进但 next 尚未发布
                        continue;
                    }

                    // 在 CAS 之前，仅仅将指针读取出来。
                    // 若线程此时被挂起，即使 next 被回收重用，ptr 悬垂指针也会因为后续 CAS 失败被丢弃，不会造成解引用
                    // prefetch 下一个待出队节点
                    T* ptr = next->data;
                    POOL_PREFETCH_R(next->next.load(std::memory_order_relaxed));

                    inner_queue::DoubleWord newHead(next, head.tag + 1);
                    if (POOL_LIKELY(head_.compare_exchange_weak(
                        head, newHead,
                        std::memory_order_release,
                        std::memory_order_relaxed)))
                    {
                        // CAS 成功，当前线程绝对占有了 ptr 数据，安全解引用
                        if (POOL_LIKELY(ptr))
                        {
                            data = std::move(*ptr);
                            ValueAllocTraits::destroy(valueAllocator_, ptr);
                            ValueAllocTraits::deallocate(valueAllocator_, ptr, 1);
                        }

                        // 延迟释放旧头节点
                        if (toDeallocate_)
                        {
                            deallocateNode(toDeallocate_);
                        }
                        toDeallocate_ = headPtr;

                        return true;
                    }
                }
            }
        }

        void clear()
        {
            T dummy;
            while (dequeue(dummy)) {}
        }

        bool empty() const
        {
            inner_queue::DoubleWord head = head_.load(std::memory_order_acquire);
            inner_queue::DoubleWord tail = tail_.load(std::memory_order_acquire);
            Node* headPtr = static_cast<Node*>(head.ptr);
            Node* tailPtr = static_cast<Node*>(tail.ptr);
            return headPtr == tailPtr &&
                headPtr->next.load(std::memory_order_acquire) == nullptr;
        }

        NodeAllocator getAllocator() const noexcept
        {
            return allocator_;
        }

    private:
        Node* allocateNode(T* d)
        {
            Node* node = AllocTraits::allocate(allocator_, 1);
            try
            {
                AllocTraits::construct(allocator_, node, d);
            }
            catch (...)
            {
                AllocTraits::deallocate(allocator_, node, 1);
                throw;
            }
            return node;
        }

        void deallocateNode(Node* node) noexcept
        {
            if (node)
            {
                AllocTraits::destroy(allocator_, node);
                AllocTraits::deallocate(allocator_, node, 1);
            }
        }
    };

    template<typename T, typename Allocator>
    thread_local typename LockFreeQueue<T, Allocator>::Node* LockFreeQueue<T, Allocator>::toDeallocate_ = nullptr;
}