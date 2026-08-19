#pragma once

#include "export.h"

#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

namespace Kama_memoryPool
{

constexpr std::size_t kAlignment = 8;
constexpr std::size_t kMaxBytes = 256 * 1024;

struct MemoryPoolStats
{
    std::uint64_t allocCount{0};
    std::uint64_t freeCount{0};
    std::uint64_t liveAllocs{0};
    std::uint64_t smallAllocCount{0};
    std::uint64_t largeAllocCount{0};
    std::uint64_t sizedFreeCount{0};
    std::uint64_t unsizedFreeCount{0};
    std::uint64_t centralRefillCount{0};
    std::uint64_t centralFlushCount{0};
    std::uint64_t reservedBytes{0};
    std::uint64_t cachedPageBytes{0};
};

class KAMA_API MemoryPool
{
public:
    static void* allocate(std::size_t size);
    static void* allocateAligned(std::size_t size, std::size_t alignment);
    static void deallocate(void* ptr);
    static void deallocate(void* ptr, std::size_t size);
    static MemoryPoolStats stats();

    template<typename T, typename... Args>
    static T* newElement(Args&&... args)
    {
        const std::size_t align = alignof(T);
        void* mem = (align <= kAlignment)
                        ? allocate(sizeof(T))
                        : allocateAligned(sizeof(T), align);
        if (!mem)
            return nullptr;
        try
        {
            return ::new (mem) T(std::forward<Args>(args)...);
        }
        catch (...)
        {
            deallocate(mem);
            throw;
        }
    }

    template<typename T>
    static void deleteElement(T* p)
    {
        if (!p)
            return;
        p->~T();
        deallocate(p);
    }
};

template<typename T, typename... Args>
T* newElement(Args&&... args)
{
    return MemoryPool::newElement<T>(std::forward<Args>(args)...);
}

template<typename T>
void deleteElement(T* p)
{
    MemoryPool::deleteElement(p);
}

} // namespace Kama_memoryPool
