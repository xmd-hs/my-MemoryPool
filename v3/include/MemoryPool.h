#pragma once
#include "ThreadCache.h"

#include <new>
#include <utility>
#include <cstdint>
#include <atomic>

namespace Kama_memoryPool
{

struct MemoryPoolStats
{
    uint64_t allocCount{0};
    uint64_t freeCount{0};
    uint64_t liveAllocs{0};
};

class MemoryPool
{
public:
    static void* allocate(size_t size)
    {
        void* ptr = ThreadCache::getInstance()->allocate(size);
        if (ptr)
            recordAlloc();
        return ptr;
    }

    static void* allocateAligned(size_t size, size_t alignment)
    {
        void* ptr = ThreadCache::getInstance()->allocateAligned(size, alignment);
        if (ptr)
            recordAlloc();
        return ptr;
    }

    static void deallocate(void* ptr)
    {
        if (!ptr)
            return;
        recordFree();
        ThreadCache::getInstance()->deallocate(ptr);
    }

    static void deallocate(void* ptr, size_t size)
    {
        if (!ptr)
            return;
        recordFree();
        ThreadCache::getInstance()->deallocate(ptr, size);
    }

    static MemoryPoolStats stats()
    {
        MemoryPoolStats out;
        out.allocCount = allocCount_.load(std::memory_order_relaxed);
        out.freeCount = freeCount_.load(std::memory_order_relaxed);
        out.liveAllocs = out.allocCount > out.freeCount ? out.allocCount - out.freeCount : 0;
        return out;
    }

    template<typename T, typename... Args>
    static T* newElement(Args&&... args)
    {
        const size_t align = alignof(T);
        void* mem = (align <= ALIGNMENT)
                        ? allocate(sizeof(T))
                        : allocateAligned(sizeof(T), align);
        if (!mem)
            return nullptr;
        return ::new (mem) T(std::forward<Args>(args)...);
    }

    template<typename T>
    static void deleteElement(T* p)
    {
        if (!p)
            return;
        p->~T();
        deallocate(p);
    }

private:
    static void recordAlloc()
    {
        allocCount_.fetch_add(1, std::memory_order_relaxed);
    }

    static void recordFree()
    {
        freeCount_.fetch_add(1, std::memory_order_relaxed);
    }

    inline static std::atomic<uint64_t> allocCount_{0};
    inline static std::atomic<uint64_t> freeCount_{0};
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
