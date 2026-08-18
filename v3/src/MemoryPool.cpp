#include <kama/MemoryPool.h>
#include "ThreadCache.h"

#include <atomic>

namespace Kama_memoryPool
{

namespace
{
std::atomic<std::uint64_t> g_allocCount{0};
std::atomic<std::uint64_t> g_freeCount{0};
}

void* MemoryPool::allocate(std::size_t size)
{
    void* ptr = ThreadCache::getInstance()->allocate(size);
    if (ptr)
        g_allocCount.fetch_add(1, std::memory_order_relaxed);
    return ptr;
}

void* MemoryPool::allocateAligned(std::size_t size, std::size_t alignment)
{
    void* ptr = ThreadCache::getInstance()->allocateAligned(size, alignment);
    if (ptr)
        g_allocCount.fetch_add(1, std::memory_order_relaxed);
    return ptr;
}

void MemoryPool::deallocate(void* ptr)
{
    if (!ptr)
        return;
    g_freeCount.fetch_add(1, std::memory_order_relaxed);
    ThreadCache::getInstance()->deallocate(ptr);
}

void MemoryPool::deallocate(void* ptr, std::size_t size)
{
    if (!ptr)
        return;
    g_freeCount.fetch_add(1, std::memory_order_relaxed);
    ThreadCache::getInstance()->deallocate(ptr, size);
}

MemoryPoolStats MemoryPool::stats()
{
    MemoryPoolStats out;
    out.allocCount = g_allocCount.load(std::memory_order_relaxed);
    out.freeCount = g_freeCount.load(std::memory_order_relaxed);
    out.liveAllocs = out.allocCount > out.freeCount ? out.allocCount - out.freeCount : 0;
    return out;
}

} // namespace Kama_memoryPool
