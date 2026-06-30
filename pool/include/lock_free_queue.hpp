#pragma once
// ============================================================
// lock_free_queue.hpp — Michael-Scott 无锁队列
// ============================================================
// 基于链表的无锁并发队列，支持多生产者多消费者（MPMC）。
// 使用双字 CAS（DoubleWord）防止 ABA 问题，延迟释放保证安全。
//
// 模板参数：
//   T         — 元素类型（必须默认可构造）
//   Allocator — 分配器类型
// ============================================================

#include "lock_free_utility.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace lock_free_container
{
namespace inner_queue
{

// 128 位原子结构体，用于双字 CAS（防 ABA）
struct alignas(16) DoubleWord
{
    void* ptr;
    uint64_t tag;

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

// 链表节点
template <typename T>
struct LockFreeNode
{
    T* data;
    std::atomic<LockFreeNode<T>*> next;

    explicit LockFreeNode(T* d) : data(d), next(nullptr) {}

    LockFreeNode(const LockFreeNode&) = delete;
    LockFreeNode& operator=(const LockFreeNode&) = delete;
};

// 默认分配器（::operator new/delete）
template <typename T>
struct DefaultAllocator
{
    using value_type = T;
    using size_type = size_t;
    using difference_type = ptrdiff_t;

    template <typename U>
    struct rebind
    {
        using other = DefaultAllocator<U>;
    };

    DefaultAllocator() noexcept = default;
    template <typename U>
    DefaultAllocator(const DefaultAllocator<U>&) noexcept
    {
    }

    T* allocate(size_t n)
    {
        return static_cast<T*>(::operator new(n * sizeof(T)));
    }

    void deallocate(T* p, size_t) noexcept
    {
        ::operator delete(p);
    }
};

}  // namespace inner_queue

template <typename T,
          typename Allocator = inner_queue::DefaultAllocator<T>>
class LockFreeQueue
    {
    public:
        /// @brief 分配器类型（队列元素 T 的分配器）
        using allocator_type = Allocator;

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

        template <typename... Args>
            requires std::constructible_from<T, Args...>
        bool enqueue(Args&&... args)
        {
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
                    // tail 落后，帮助推进
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
                        continue;
                    }

                    T* ptr = next->data;
                    POOL_PREFETCH_R(next->next.load(std::memory_order_relaxed));

                    inner_queue::DoubleWord newHead(next, head.tag + 1);
                    if (POOL_LIKELY(head_.compare_exchange_weak(
                        head, newHead,
                        std::memory_order_release,
                        std::memory_order_relaxed)))
                    {
                        if (POOL_LIKELY(ptr))
                        {
                            data = std::move(*ptr);
                            ValueAllocTraits::destroy(valueAllocator_, ptr);
                            ValueAllocTraits::deallocate(valueAllocator_, ptr, 1);
                        }

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

        /// @brief 获取队列的数据分配器副本（同 std::priority_queue 约定）
        allocator_type get_allocator() const noexcept
        {
            return valueAllocator_;
        }

        NodeAllocator getNodeAllocator() const noexcept
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