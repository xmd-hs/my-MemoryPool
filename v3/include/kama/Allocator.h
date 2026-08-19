#pragma once

#include "MemoryPool.h"

#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>

namespace Kama_memoryPool
{

template<typename T>
class Allocator
{
public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using propagate_on_container_move_assignment = std::true_type;
    using is_always_equal = std::true_type;

    Allocator() noexcept = default;

    template<typename U>
    Allocator(const Allocator<U>&) noexcept {}

    T* allocate(std::size_t n)
    {
        if (n == 0)
            return nullptr;
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(T))
            throw std::bad_array_new_length();
        void* p = MemoryPool::allocateAligned(n * sizeof(T), alignof(T));
        if (!p)
            throw std::bad_alloc();
        return static_cast<T*>(p);
    }

    void deallocate(T* p, std::size_t n) noexcept
    {
        if constexpr (alignof(T) <= kAlignment)
            MemoryPool::deallocate(p, n * sizeof(T));
        else
            MemoryPool::deallocate(p);
    }
};

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
