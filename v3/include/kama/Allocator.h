// STL 分配器适配器（v3）
//
// 让 std::vector / std::list 等容器可以用本内存池分配内存。
// 用法：std::vector<int, Kama_memoryPool::Allocator<int>> v;

#pragma once

#include "MemoryPool.h"  // MemoryPool 门面

#include <cstddef>      // size_t
#include <limits>       // std::numeric_limits
#include <new>          // std::bad_alloc
#include <type_traits>  // std::true_type

namespace Kama_memoryPool
{

template<typename T>
class Allocator
{
public:
    using value_type = T;                          // 分配的元素类型
    using size_type = std::size_t;                 // 大小类型
    using difference_type = std::ptrdiff_t;        // 指针差类型
    using propagate_on_container_move_assignment = std::true_type;  // 容器移动赋值时传播分配器
    using is_always_equal = std::true_type;        // 所有 Allocator 实例都等价（无状态）

    Allocator() noexcept = default;  // 默认构造

    // 从其他类型的 Allocator 构造（无状态，什么都不做）
    template<typename U>
    Allocator(const Allocator<U>&) noexcept {}

    // 分配 n 个 T 的内存
    T* allocate(std::size_t n)
    {
        if (n == 0)
            return nullptr;
        // 检查 n * sizeof(T) 是否溢出
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(T))
            throw std::bad_array_new_length();
        const std::size_t bytes = n * sizeof(T);  // 总字节数
        // 对齐 <= 8 走普通分配，否则走对齐分配
        void* p = (alignof(T) <= kAlignment)
                      ? MemoryPool::allocate(bytes)
                      : MemoryPool::allocateAligned(bytes, alignof(T));
        if (!p)
            throw std::bad_alloc();
        return static_cast<T*>(p);
    }

    // 释放 n 个 T 的内存
    void deallocate(T* p, std::size_t n) noexcept
    {
        // 编译期分支：对齐 <= 8 带大小释放（更快），否则不带大小释放
        if (alignof(T) <= kAlignment)
            MemoryPool::deallocate(p, n * sizeof(T));
        else
            MemoryPool::deallocate(p);
    }
};

// 比较两个 Allocator 是否相等（无状态，永远相等）
template<typename T, typename U>
bool operator==(const Allocator<T>&, const Allocator<U>&) noexcept
{
    return true;
}

template<typename T, typename U>
bool operator!=(const Allocator<T>&, const Allocator<U>&) noexcept
{
    return false;
}

} // namespace Kama_memoryPool
