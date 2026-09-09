#include <kama/MemoryPool.h>
#include "Common.h"
#include "PageCache.h"
#include "ThreadCache.h"

#include <atomic>

namespace Kama_memoryPool
{

namespace
{
std::atomic<std::uint64_t> g_allocCount{0};
std::atomic<std::uint64_t> g_freeCount{0};
std::atomic<std::uint64_t> g_smallAllocCount{0};
std::atomic<std::uint64_t> g_largeAllocCount{0};
std::atomic<std::uint64_t> g_sizedFreeCount{0};
std::atomic<std::uint64_t> g_unsizedFreeCount{0};
std::atomic<std::uint64_t> g_centralRefillCount{0};
std::atomic<std::uint64_t> g_centralFlushCount{0};
std::atomic<std::uint64_t> g_centralLockWaits{0};
std::atomic<std::uint64_t> g_remoteFreeCount{0};
std::array<std::atomic<std::uint64_t>, kMaxSizeClasses> g_refillByClass{};
std::array<std::atomic<std::uint64_t>, kMaxSizeClasses> g_flushByClass{};
std::array<std::atomic<std::uint64_t>, kMaxSizeClasses> g_tlsHitByClass{};
std::array<std::atomic<std::uint64_t>, kMaxSizeClasses> g_tlsMissByClass{};
}

void recordAllocation(size_t size)
{
    if constexpr (!kStatsEnabled)
        return;
    g_allocCount.fetch_add(1, std::memory_order_relaxed);
    // Keep statistics consistent with ThreadCache: MAX_BYTES itself takes
    // the large-object path.
    if (size >= MAX_BYTES)
        g_largeAllocCount.fetch_add(1, std::memory_order_relaxed);
    else
        g_smallAllocCount.fetch_add(1, std::memory_order_relaxed);
}

void recordFree(bool sized)
{
    if constexpr (!kStatsEnabled)
        return;
    g_freeCount.fetch_add(1, std::memory_order_relaxed);
    if (sized)
        g_sizedFreeCount.fetch_add(1, std::memory_order_relaxed);
    else
        g_unsizedFreeCount.fetch_add(1, std::memory_order_relaxed);
}

void recordCentralRefill(size_t index, size_t count)
{
    if constexpr (!kStatsEnabled)
        return;
    g_centralRefillCount.fetch_add(count, std::memory_order_relaxed);
    if (index < kMaxSizeClasses)
        g_refillByClass[index].fetch_add(count, std::memory_order_relaxed);
}

void recordCentralFlush(size_t index, size_t count)
{
    if constexpr (!kStatsEnabled)
        return;
    g_centralFlushCount.fetch_add(count, std::memory_order_relaxed);
    if (index < kMaxSizeClasses)
        g_flushByClass[index].fetch_add(count, std::memory_order_relaxed);
}

void recordThreadCacheHit(size_t index)
{
    if constexpr (kStatsEnabled)
        if (index < kMaxSizeClasses)
            g_tlsHitByClass[index].fetch_add(1, std::memory_order_relaxed);
}

void recordThreadCacheMiss(size_t index)
{
    if constexpr (kStatsEnabled)
        if (index < kMaxSizeClasses)
            g_tlsMissByClass[index].fetch_add(1, std::memory_order_relaxed);
}

void recordCentralLockWait(std::uint64_t count)
{
    if constexpr (!kStatsEnabled)
        return;
    g_centralLockWaits.fetch_add(count, std::memory_order_relaxed);
}

void recordRemoteFree(std::uint64_t count)
{
    if constexpr (!kStatsEnabled)
        return;
    g_remoteFreeCount.fetch_add(count, std::memory_order_relaxed);
}

void* MemoryPool::allocate(std::size_t size)
{
    ThreadCache* cache = ThreadCache::getInstance();
    void* ptr = cache->allocate(size);
    if (ptr)
        recordAllocation(size);
    return ptr;
}

void* MemoryPool::allocateAligned(std::size_t size, std::size_t alignment)
{
    ThreadCache* cache = ThreadCache::getInstance();
    void* ptr = cache->allocateAligned(size, alignment);
    if (ptr)
        recordAllocation(size);
    return ptr;
}

void MemoryPool::deallocate(void* ptr)
{
    if (!ptr)
        return;
    if (ThreadCache::getInstance()->deallocate(ptr))
        recordFree(false);
}

void MemoryPool::deallocate(void* ptr, std::size_t size)
{
    if (!ptr)
        return;
    if (ThreadCache::getInstance()->deallocate(ptr, size))
        recordFree(true);
}

void MemoryPool::trim()
{
    ThreadCache::getInstance()->trim();
}

MemoryPoolStats MemoryPool::stats()
{
    MemoryPoolStats out;
    out.reservedBytes = PageCache::getInstance().reservedBytes();
    out.cachedPageBytes = PageCache::getInstance().cachedPageBytes();
    if constexpr (!kStatsEnabled)
        return out;
    out.allocCount = g_allocCount.load(std::memory_order_relaxed);
    out.freeCount = g_freeCount.load(std::memory_order_relaxed);
    out.liveAllocs = out.allocCount > out.freeCount ? out.allocCount - out.freeCount : 0;
    out.smallAllocCount = g_smallAllocCount.load(std::memory_order_relaxed);
    out.largeAllocCount = g_largeAllocCount.load(std::memory_order_relaxed);
    out.sizedFreeCount = g_sizedFreeCount.load(std::memory_order_relaxed);
    out.unsizedFreeCount = g_unsizedFreeCount.load(std::memory_order_relaxed);
    out.centralRefillCount = g_centralRefillCount.load(std::memory_order_relaxed);
    out.centralFlushCount = g_centralFlushCount.load(std::memory_order_relaxed);
    out.centralLockWaits = g_centralLockWaits.load(std::memory_order_relaxed);
    out.remoteFreeCount = g_remoteFreeCount.load(std::memory_order_relaxed);
    for (size_t i = 0; i < kMaxSizeClasses; ++i)
    {
        out.centralRefillByClass[i] = g_refillByClass[i].load(std::memory_order_relaxed);
        out.centralFlushByClass[i] = g_flushByClass[i].load(std::memory_order_relaxed);
        out.threadCacheHitByClass[i] = g_tlsHitByClass[i].load(std::memory_order_relaxed);
        out.threadCacheMissByClass[i] = g_tlsMissByClass[i].load(std::memory_order_relaxed);
    }
    return out;
}

} // namespace Kama_memoryPool
