// MemoryPool 门面实现（v3）
//
// 门面层，把 allocate/deallocate 委托给 ThreadCache。
// 同时维护全局统计计数器。

#include <kama/MemoryPool.h>
#include "CentralCache.h"
#include "Common.h"
#include "PageCache.h"
#include "ThreadCache.h"

#include <atomic>

namespace Kama_memoryPool
{

namespace
{
// 全局统计计数器（kStatsEnabled=0 时这些原子操作会被 if constexpr 优化掉）
std::atomic<std::uint64_t> g_allocCount{0};           // 总分配次数
std::atomic<std::uint64_t> g_freeCount{0};            // 总释放次数
std::atomic<std::uint64_t> g_smallAllocCount{0};      // 小对象分配
std::atomic<std::uint64_t> g_largeAllocCount{0};      // 大对象分配
std::atomic<std::uint64_t> g_sizedFreeCount{0};       // 带大小释放
std::atomic<std::uint64_t> g_unsizedFreeCount{0};     // 不带大小释放
std::atomic<std::uint64_t> g_centralRefillCount{0};   // 中央取块
std::atomic<std::uint64_t> g_centralFlushCount{0};    // 归还中央
std::atomic<std::uint64_t> g_centralLockWaits{0};     // 中央锁等待
std::array<std::atomic<std::uint64_t>, kMaxSizeClasses> g_refillByClass{};   // 各档位取块
std::array<std::atomic<std::uint64_t>, kMaxSizeClasses> g_flushByClass{};    // 各档位归还
std::array<std::atomic<std::uint64_t>, kMaxSizeClasses> g_tlsHitByClass{};   // 各档位命中
std::array<std::atomic<std::uint64_t>, kMaxSizeClasses> g_tlsMissByClass{};  // 各档位未命中
}

// 记录分配：size >= MAX_BYTES 算大对象
void recordAllocation(size_t size)
{
    if (!kStatsEnabled)  // 统计关闭时直接返回，零开销
        return;
    g_allocCount.fetch_add(1, std::memory_order_relaxed);
    if (size >= MAX_BYTES)
        g_largeAllocCount.fetch_add(1, std::memory_order_relaxed);
    else
        g_smallAllocCount.fetch_add(1, std::memory_order_relaxed);
}

// 记录释放
void recordFree(bool sized)
{
    if (!kStatsEnabled)
        return;
    g_freeCount.fetch_add(1, std::memory_order_relaxed);
    if (sized)
        g_sizedFreeCount.fetch_add(1, std::memory_order_relaxed);
    else
        g_unsizedFreeCount.fetch_add(1, std::memory_order_relaxed);
}

void recordCentralRefill(size_t index, size_t count)
{
    if (!kStatsEnabled)
        return;
    g_centralRefillCount.fetch_add(count, std::memory_order_relaxed);
    if (index < kMaxSizeClasses)
        g_refillByClass[index].fetch_add(count, std::memory_order_relaxed);
}

void recordCentralFlush(size_t index, size_t count)
{
    if (!kStatsEnabled)
        return;
    g_centralFlushCount.fetch_add(count, std::memory_order_relaxed);
    if (index < kMaxSizeClasses)
        g_flushByClass[index].fetch_add(count, std::memory_order_relaxed);
}

void recordThreadCacheHit(size_t index)
{
    if (kStatsEnabled)
        if (index < kMaxSizeClasses)
            g_tlsHitByClass[index].fetch_add(1, std::memory_order_relaxed);
}

void recordThreadCacheMiss(size_t index)
{
    if (kStatsEnabled)
        if (index < kMaxSizeClasses)
            g_tlsMissByClass[index].fetch_add(1, std::memory_order_relaxed);
}

void recordCentralLockWait(std::uint64_t count)
{
    if (!kStatsEnabled)
        return;
    g_centralLockWaits.fetch_add(count, std::memory_order_relaxed);
}

// ===== 门面接口实现 =====

void* MemoryPool::allocate(std::size_t size)
{
    ThreadCache* cache = ThreadCache::getInstance();  // 拿本线程的缓存
    void* ptr = cache->allocate(size);                // 委托给 ThreadCache
    if (ptr)
        recordAllocation(size);                        // 统计
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
    if (ThreadCache::getInstance()->deallocate(ptr))  // 不带大小释放
        recordFree(false);
}

void MemoryPool::deallocate(void* ptr, std::size_t size)
{
    if (!ptr)
        return;
    if (ThreadCache::getInstance()->deallocate(ptr, size))  // 带大小释放（更快）
        recordFree(true);
}

void MemoryPool::trim()
{
    ThreadCache::getInstance()->trim();  // 归还当前线程多余缓存
}

void MemoryPool::setPageCacheLimit(std::size_t bytes)
{
    PageCache::getInstance().setCachedPageLimit(bytes);
}

std::size_t MemoryPool::pageCacheLimit()
{
    return PageCache::getInstance().cachedPageLimit();
}

void MemoryPool::setLargeCacheLimit(std::size_t bytes)
{
    ThreadCache::setLargeCacheLimit(bytes);
}

std::size_t MemoryPool::largeCacheLimit()
{
    return ThreadCache::largeCacheLimit();
}

void MemoryPool::releaseUnused()
{
    ThreadCache::getInstance()->releaseUnused();
    CentralCache::getInstance().drainTransfers();
    PageCache::getInstance().releaseCachedPages();
}

// 收集统计信息
MemoryPoolStats MemoryPool::stats()
{
    MemoryPoolStats out;
    out.reservedBytes = PageCache::getInstance().reservedBytes();      // 系统预留字节
    out.cachedPageBytes = PageCache::getInstance().cachedPageBytes();  // 缓存空闲页字节
    if (!kStatsEnabled)  // 统计关闭只返回内存信息
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
