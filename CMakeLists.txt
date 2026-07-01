# ============================================================
# Thread-Pool — C++20 无锁线程池 CMake 构建配置
# ============================================================
cmake_minimum_required(VERSION 3.20)
project(thread-pool
    VERSION 2.0.0
    DESCRIPTION "C++20 lock-free thread pool with swappable queue backends"
    LANGUAGES CXX)

# ── C++ 标准 ──────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 导出 compile_commands.json（供 clangd / IDE 使用）
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── 编译选项 ──────────────────────────────────────────
# 公用警告（GCC / Clang 通用）
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
)

# GCC 专属警告抑制（std::function 布局警告在 GCC 12+ 上无害）
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-Wno-psabi)
endif()

# ── C++23 特性检测 ──────────────────────────────────────
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-std=c++23" COMPILER_SUPPORTS_CXX23)

# ── 线程库 ──────────────────────────────────────────
find_package(Threads REQUIRED)

# ── GoogleTest ────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)

# ── 库目标（header-only）─────────────────────────────
add_library(thread_pool INTERFACE)
target_include_directories(thread_pool INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/pool/include)
target_compile_features(thread_pool INTERFACE cxx_std_20)
# 始终链接 atomic（某些 x86-64 平台需要 128 位原子操作，
# 不需要时链接器自动忽略）
target_link_libraries(thread_pool INTERFACE atomic)

# ── 启用测试 ─────────────────────────────────────────
enable_testing()

# ── 测试辅助函数 ───────────────────────────────────────
function(add_pool_test name source_path standard)
    get_filename_component(test_name ${source_path} NAME_WE)
    add_executable(${name} ${source_path})
    target_link_libraries(${name} PRIVATE
        thread_pool
        GTest::gtest_main
        Threads::Threads)
    target_compile_features(${name} PRIVATE ${standard})
    # GCC 12+ 的 std::function 布局警告（无害）
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${name} PRIVATE -Wno-psabi)
    endif()
    gtest_discover_tests(${name}
        DISCOVERY_TIMEOUT 60
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endfunction()

# ── 测试目标 ───────────────────────────────────────────
# C++20 测试
add_pool_test(test_queue
    pool/test/test_queue.cpp cxx_std_20)
add_pool_test(test_ring_queue
    pool/test/test_ring_queue.cpp cxx_std_20)
add_pool_test(test_thread_pool
    pool/test/test_thread_pool.cpp cxx_std_20)
add_pool_test(test_concurrency
    pool/test/test_concurrency.cpp cxx_std_20)

# C++23 条件编译目标
if(COMPILER_SUPPORTS_CXX23)
    add_pool_test(test_move_only
        pool/test/test_move_only.cpp cxx_std_23)

    # compare_with_mutex_pool 是独立 benchmark（自带 main），
    # 不用 gtest_discover_tests，注册为普通 CTest
    add_executable(test_compare
        pool/test/compare_with_mutex_pool.cpp)
    target_link_libraries(test_compare PRIVATE
        thread_pool
        Threads::Threads)
    target_compile_features(test_compare PRIVATE cxx_std_23)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(test_compare PRIVATE -Wno-psabi)
    endif()
    add_test(NAME test_compare COMMAND test_compare
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

    message(STATUS "C++23 supported — move_only & compare tests enabled")
else()
    message(STATUS "C++23 NOT supported — move_only & compare tests skipped")
endif()

# ── 安装 ────────────────────────────────────────────────
include(GNUInstallDirs)
install(DIRECTORY pool/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/thread_pool
    FILES_MATCHING PATTERN "*.hpp")
