#pragma once

// ============================================================
// lock_free_utility.hpp — 跨编译器内建宏与工具类型
// ============================================================
// 提供编译器优化提示（likely/unlikely、预取、内联控制）及
// 编译期类型检查工具。非 GCC/Clang 编译器自动降级为空宏，
// 不影响正确性。
// ============================================================

#include <cstddef>
#include <type_traits>
#include <variant>

// ============================================================
// 缓存行大小常量
// ============================================================
namespace lock_free_util
{
/// @brief 典型 x86-64 缓存行大小，用于对齐隔离 false sharing
inline constexpr std::size_t kCacheLineSize = 64;
}  // namespace lock_free_util

// ============================================================
// 编译器内建分支预测提示
// ============================================================
#if defined(__GNUC__) || defined(__clang__)
#define POOL_LIKELY(x) __builtin_expect(!!(x), 1)
#define POOL_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define POOL_LIKELY(x) (x)
#define POOL_UNLIKELY(x) (x)
#endif

// ============================================================
// 编译器内建预取指令
// ============================================================
// 参数说明：
//   addr  — 预取地址
//   rw    — 0 = 读预取, 1 = 写预取
//   locality — 0=无局部性, 3=极高局部性（保留在各级缓存）
#if defined(__GNUC__) || defined(__clang__)
#define POOL_PREFETCH_R(addr) __builtin_prefetch((addr), 0, 3)
#define POOL_PREFETCH_W(addr) __builtin_prefetch((addr), 1, 3)
#else
#define POOL_PREFETCH_R(addr)
#define POOL_PREFETCH_W(addr)
#endif

// ============================================================
// 编译器内建内联控制
// ============================================================
#if defined(__GNUC__) || defined(__clang__)
#define POOL_ALWAYS_INLINE __attribute__((always_inline)) inline
#define POOL_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define POOL_ALWAYS_INLINE __forceinline
#define POOL_NOINLINE __declspec(noinline)
#else
#define POOL_ALWAYS_INLINE inline
#define POOL_NOINLINE
#endif

// ============================================================
// 编译期工具：类型是否在 variant 中
// ============================================================
// 用法：
//   using V = std::variant<int, double>;
//   static_assert(is_in_variant_v<int, V>);
//   static_assert(!is_in_variant_v<float, V>);

template <typename T, typename Variant>
struct is_in_variant;

template <typename T, typename... Ts>
struct is_in_variant<T, std::variant<Ts...>>
    : std::disjunction<std::is_same<T, Ts>...>
{
};

template <typename T, typename Variant>
inline constexpr bool is_in_variant_v = is_in_variant<T, Variant>::value;

// ============================================================
// 编译期工具：获取类型在 variant 中的索引
// ============================================================
template <typename T, typename Variant>
struct variant_index;

template <typename T, typename... Ts>
struct variant_index<T, std::variant<Ts...>>
{
private:
    template <std::size_t I, typename U, typename... Us>
    static constexpr std::size_t find()
    {
        if constexpr (std::is_same_v<T, U>)
        {
            return I;
        }
        else if constexpr (sizeof...(Us) > 0)
        {
            return find<I + 1, Us...>();
        }
        else
        {
            // 编译期报错：类型不在 variant 中
            static_assert(sizeof(T) == 0, "Type not found in variant");
            return 0;
        }
    }

public:
    static constexpr std::size_t value = find<0, Ts...>();
};

template <typename T, typename Variant>
inline constexpr std::size_t variant_index_v =
    variant_index<T, Variant>::value;
