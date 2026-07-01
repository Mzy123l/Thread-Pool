#pragma once
// ============================================================
// lock_free_thread_pool.hpp — 线程池汇总头文件
// ============================================================
// 包含所有四种线程池变体：
//   - DynamicThreadPool            (std::function)
//   - DynamicMoveOnlyThreadPool    (std::move_only_function, C++23)
//   - VariantThreadPool            (std::variant + packaged_task)
//   - StrictVariantThreadPool      (std::variant + 自实现 task/future)
//
// 同时提供向后兼容别名：
//   - LockFreeThreadPool         -> DynamicThreadPool
//   - LockFreeMoveOnlyThreadPool -> DynamicMoveOnlyThreadPool
// ============================================================

#include "lock_free_dynamic_thread_pool.hpp"
#include "lock_free_move_only_thread_pool.hpp"
#include "lock_free_variant_thread_pool.hpp"
#include "lock_free_strict_variant_thread_pool.hpp"
