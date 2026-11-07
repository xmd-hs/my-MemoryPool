#include "ThreadCache.h"
#include "CentralCache.h"
#include "PageCache.h"
#include "Platform.h"

#include <algorithm>
#include <cstdint>

namespace Kama_memoryPool
{
static std::atomic<size_t> g_largeCacheLimitBytes{8 * 1024 * 1024};

namespace
{

size_t lookupSizeClass(void* ptr)
{
    // Prefer the in-page tag (always current). Do not cache page->class in TLS:
    // PageCache can recycle the same virtual page into another size class.
    const size_t ps = PageCache::pageSize();
    if (const PageTag* tag = pageTagIfValid(ptr, ps))
        return tag->sizeClass;
    return PageCache::getInstance().findSizeClass(ptr);
}

#if 0
constexpr size_t kRemoteBatchLimit = 64;
#else
constexpr size_t kRemoteBatchLimit = 128;
#endif

struct LocalLargeCache
{
    // Bound retained memory rather than span count. A count-only cap can
    // retain an unexpectedly large amount of address space when large spans
    // are used repeatedly.
    static constexpr size_t kMaxCachedBytes = 8 * 1024 * 1024;

    Span* head = nullptr;
    // Fast exact-size bins for the common 512KiB/1MiB/2MiB network buffers.
    Span* hot[3] = {nullptr, nullptr, nullptr};
    size_t count = 0;
    size_t bytes = 0;

    static size_t hotIndex(size_t pages)
    {
        return pages == 128 ? 0 : (pages == 256 ? 1 : (pages == 512 ? 2 : 3));
    }

    Span* take(size_t numPages)
    {
        const size_t i = hotIndex(numPages);
        if (i < 3 && hot[i])
        {
            Span* span = hot[i];
            hot[i] = span->next;
            span->next = nullptr;
            --count;
            bytes -= span->numPages * PageCache::pageSize();
            return span;
        }
        Span** link = &head;
        while (*link)
        {
            Span* span = *link;
            if (span->numPages == numPages)
            {
            *link = span->next;
            span->next = nullptr;
            --count;
            bytes -= span->numPages * PageCache::pageSize();
            return span;
            }
            link = &span->next;
        }
        return nullptr;
    }

    bool store(Span* span)
    {
        if (!span)
            return false;
        const size_t spanBytes = span->numPages * PageCache::pageSize();
        if (spanBytes > kMaxCachedBytes || bytes > kMaxCachedBytes - spanBytes)
            return false;
        const size_t i = hotIndex(span->numPages);
        span->next = i < 3 ? hot[i] : head;
        if (i < 3) hot[i] = span;
        else head = span;
        ++count;
        bytes += spanBytes;
        return true;
    }

    void flush()
    {
        for (size_t i = 0; i < 3; ++i)
            while (hot[i])
            {
                Span* span = hot[i]; hot[i] = span->next; span->next = nullptr;
                PageCache::getInstance().deallocateSpan(span);
            }
        while (head)
        {
            Span* span = head;
            head = span->next;
            span->next = nullptr;
            PageCache::getInstance().deallocateSpan(span);
        }
        count = 0;
        bytes = 0;
    }
};

thread_local LocalLargeCache g_localLargeCache{};
thread_local Span* g_lastLargeSpan = nullptr;

size_t debugBlockIndex(const Span* span, const void* ptr)
{
    const auto* start = static_cast<const char*>(span->pageAddr);
    const auto* target = static_cast<const char*>(ptr);
    if (target < start)
        return static_cast<size_t>(-1);
    const size_t offset = static_cast<size_t>(target - start);
    if (span->blockSize == 0)
        return static_cast<size_t>(-1);
    if (span->blocksPerPage > 0)
    {
        const size_t ps = PageCache::pageSize();
        const size_t pageIndex = offset / ps;
        const size_t offsetInPage = offset % ps;
        const size_t usable = ps - kPageTagBytes;
        if (offsetInPage >= usable || offsetInPage % span->blockSize != 0)
            return static_cast<size_t>(-1);
        return pageIndex * span->blocksPerPage + offsetInPage / span->blockSize;
    }
    if (offset % span->blockSize != 0)
        return static_cast<size_t>(-1);
    return offset / span->blockSize;
}

bool debugMarkAllocated(Span* span, void* ptr)
{
    if (!kDebugGuardsEnabled)
        return true;
    if (!span || span->isLarge)
        return true;

    std::lock_guard<std::mutex> lock(span->debugMutex);
    const size_t blockIndex = debugBlockIndex(span, ptr);
    if (blockIndex >= span->debugAllocMap.size())
        return false;
    if (span->debugAllocMap[blockIndex] != 0)
        return false;
    span->debugAllocMap[blockIndex] = 1;
    return true;
}

bool debugValidateAndMarkFreed(Span* span, void* ptr, size_t expectedIndex)
{
    if (!kDebugGuardsEnabled)
        return true;
    if (!span)
        return false;

    std::lock_guard<std::mutex> lock(span->debugMutex);
    if (span->isLarge)
    {
        if (ptr != span->pageAddr || !span->debugLargeAllocated)
            return false;
        span->debugLargeAllocated = false;
        return true;
    }
    if (expectedIndex != static_cast<size_t>(-1) && span->sizeClass != expectedIndex)
        return false;
    const size_t blockIndex = debugBlockIndex(span, ptr);
    if (blockIndex >= span->debugAllocMap.size())
        return false;
    if (span->debugAllocMap[blockIndex] == 0)
        return false;
    span->debugAllocMap[blockIndex] = 0;
    return true;
}

void* allocateLargeObject(size_t size)
{
    const size_t ps = PageCache::pageSize();
    if (size > SIZE_MAX - (ps - 1))
        return nullptr;
    size_t numPages = (size + ps - 1) / ps;

    if (Span* span = g_localLargeCache.take(numPages))
    {
        span->inFreeList = false;
        span->blockSize = size;
        span->blockCount = 1;
        span->freeCount = 0;
        span->sizeClass = static_cast<size_t>(-1);
        span->isLarge = true;
        span->next = nullptr;
        span->debugAllocMap.clear();
        span->debugLargeAllocated = true;
        g_lastLargeSpan = span;
        return span->pageAddr;
    }

    Span* span = PageCache::getInstance().allocateLargeSpan(numPages);
    if (!span)
        return nullptr;
    span->blockSize = size;
    span->blockCount = 1;
    span->freeCount = 0;
    span->sizeClass = static_cast<size_t>(-1);
    span->isLarge = true;
    span->next = nullptr;
    span->debugAllocMap.clear();
    span->debugLargeAllocated = true;
    g_lastLargeSpan = span;
    return span->pageAddr;
}

bool freeLargeObject(Span* span)
{
    if (!span || span->inFreeList)
        return false;
    span->blockSize = 0;
    span->blockCount = 0;
    span->freeCount = 0;
    span->sizeClass = 0;
    span->debugAllocMap.clear();
    span->debugLargeAllocated = false;
    if (g_localLargeCache.store(span))
    {
        span->inFreeList = true;
        return true;
    }
    PageCache::getInstance().deallocateSpan(span);
    return true;
}

} // namespace

ThreadCache::ThreadCache()
{
    freeList_.fill(nullptr);
    freeListSize_.fill(0);
}

ThreadCache::~ThreadCache()
{
    flushAll();
    g_localLargeCache.flush();
}

void ThreadCache::flushAll()
{
    for (size_t i = 0; i < FREE_LIST_SIZE; ++i)
    {
        if (freeList_[i] && freeListSize_[i] > 0)
        {
            CentralCache::getInstance().pushTransfer(freeList_[i], freeListSize_[i], i);
            freeList_[i] = nullptr;
            freeListSize_[i] = 0;
        }
    }
}

size_t ThreadCache::maxCachedBlocks(size_t size) const
{
    constexpr size_t kBaseCacheBytes = 256 * 1024;
    size_t n = std::max(size_t(1), kBaseCacheBytes / std::max(size, ALIGNMENT));
#if 0
    if (size <= 32) return 2048;
    if (size <= 64) return 1024;
    if (size <= 256) return 512;
    if (size <= 1024) return 128;
    if (size <= 4096) return 256;
    return std::min(n, size_t(64));
#else
    if (size <= 32) return 4096;
    if (size <= 64) return 2048;
    if (size <= 256) return 1024;
    if (size <= 1024) return 256;
    if (size <= 4096) return 512;
    // Cover the 256-live scalability batch so 64KB-class allocs stay in TLS.
    if (size <= 64 * 1024) return 256;
    return std::min(n, size_t(128));
#endif
}

bool ThreadCache::shouldReturnToCentralCache(size_t index) const
{
    return freeListSize_[index] > maxCachedBlocks(SizeClass::getSize(index));
}

size_t ThreadCache::getBatchNum(size_t size) const
{
    constexpr size_t kMaxBatchBytes = 256 * 1024;
    size_t n = std::max(size_t(1), kMaxBatchBytes / std::max(size, ALIGNMENT));
#if 0
    if (size <= 32) n = std::min(n, size_t(256));
    else if (size <= 64) n = std::min(n, size_t(128));
    else if (size <= 256) n = std::min(n, size_t(64));
    else if (size <= 1024) n = std::min(n, size_t(24));
    else n = std::min(n, size_t(12));
#elif defined(__linux__)
    if (size <= 32) n = std::min(n, size_t(512));
    else if (size <= 64) n = std::min(n, size_t(256));
    else if (size <= 256) n = std::min(n, size_t(256));
    else if (size <= 1024) n = std::min(n, size_t(32));
    else
    {
        n = std::min(n, size_t(32));
        if (size >= 4096)
            n = std::max(n, size_t(8));
    }
#else
    if (size <= 32) n = std::min(n, size_t(512));
    else if (size <= 64) n = std::min(n, size_t(256));
    else if (size <= 256) n = std::min(n, size_t(256));
    else if (size <= 1024) n = std::min(n, size_t(32));
    else
    {
        n = std::min(n, size_t(32));
        if (size >= 4096)
            n = std::max(n, size_t(8));
    }
#endif
    return std::max(size_t(1), n);
}

bool ThreadCache::freeSmall(void* ptr, size_t index)
{
    if (index >= FREE_LIST_SIZE)
        return false;

    *reinterpret_cast<void**>(ptr) = freeList_[index];
    freeList_[index] = ptr;
    freeListSize_[index]++;
    if (shouldReturnToCentralCache(index))
        returnToCentralCache(index);
    return true;
}

void* ThreadCache::allocate(size_t size)
{
    if (size == 0)
        size = ALIGNMENT;

    if (KAMA_UNLIKELY(size >= MAX_BYTES))
        return allocateLargeObject(size);

    const size_t index = SizeClass::getIndex(size);
    if (void* ptr = freeList_[index])
    {
        recordThreadCacheHit(index);
        void* next = *reinterpret_cast<void**>(ptr);
        freeList_[index] = next;
        --freeListSize_[index];
        if (next)
            KAMA_PREFETCH(next);
        if (kDebugGuardsEnabled)
        {
            Span* span = PageCache::getInstance().findSpan(ptr);
            if (!debugMarkAllocated(span, ptr))
                return nullptr;
        }
        return ptr;
    }
    recordThreadCacheMiss(index);
    return fetchFromCentralCache(index);
}

void* ThreadCache::allocateAligned(size_t size, size_t alignment)
{
    if (alignment == 0 || (alignment & (alignment - 1)) != 0)
        return nullptr;
    if (alignment <= ALIGNMENT)
        return allocate(size);
    if (size > SIZE_MAX - (alignment - 1))
        return nullptr;

    size_t need = (size + alignment - 1) & ~(alignment - 1);
    if (need <= MAX_BYTES)
    {
        size_t index = SizeClass::getIndex(need);
        while (index < FREE_LIST_SIZE && (SizeClass::getSize(index) % alignment) != 0)
            ++index;
        if (index < FREE_LIST_SIZE)
            need = SizeClass::getSize(index);
    }
    return allocate(need);
}

bool ThreadCache::deallocate(void* ptr)
{
    if (!ptr)
        return false;

    const size_t mappedClass = lookupSizeClass(ptr);
    if (mappedClass < FREE_LIST_SIZE)
    {
        if (kDebugGuardsEnabled)
        {
            Span* span = PageCache::getInstance().findSpan(ptr);
            if (!debugValidateAndMarkFreed(span, ptr, mappedClass))
                return false;
        }
        return freeSmall(ptr, mappedClass);
    }

    if (Span* span = PageCache::getInstance().findSpan(ptr))
    {
        if (!debugValidateAndMarkFreed(span, ptr, span->isLarge ? static_cast<size_t>(-1) : span->sizeClass))
            return false;
        if (span->isLarge)
            return freeLargeObject(span);
        return freeSmall(ptr, span->sizeClass);
    }

    if (Span* large = PageCache::getInstance().findLargeSpan(ptr))
    {
        if (!debugValidateAndMarkFreed(large, ptr, static_cast<size_t>(-1)))
            return false;
        return freeLargeObject(large);
    }
    return false;
}

bool ThreadCache::deallocate(void* ptr, size_t size)
{
    if (!ptr)
        return false;
    if (size == 0)
        size = ALIGNMENT;

    if (size >= MAX_BYTES)
    {
        Span* span = (g_lastLargeSpan && g_lastLargeSpan->pageAddr == ptr &&
                      g_lastLargeSpan->isLarge) ? g_lastLargeSpan :
                      PageCache::getInstance().findLargeSpan(ptr);
        if (!span)
            span = PageCache::getInstance().findSpan(ptr);
        if (!span || !span->isLarge)
            return false;
        if (!debugValidateAndMarkFreed(span, ptr, static_cast<size_t>(-1)))
            return false;
        return freeLargeObject(span);
    }

    const size_t index = SizeClass::getIndex(size);
    if (index >= FREE_LIST_SIZE)
        return deallocate(ptr);

    if (kDebugGuardsEnabled)
    {
        Span* span = PageCache::getInstance().findSpan(ptr);
        if (!debugValidateAndMarkFreed(span, ptr, index))
            return false;
    }
    return freeSmall(ptr, index);
}

void ThreadCache::trim()
{
    for (size_t index = 0; index < FREE_LIST_SIZE; ++index)
    {
        if (shouldReturnToCentralCache(index))
            returnToCentralCache(index);
    }
    // Keep the hot large-object cache across trim calls.  Network workloads
    // commonly call trim as a lightweight checkpoint; flushing a 512KiB span
    // on every checkpoint adds a page-cache lock and defeats reuse.  The
    // stronger releaseUnused() API still flushes it and returns pages.
}

void ThreadCache::releaseUnused()
{
    flushAll();
    g_localLargeCache.flush();
}

void ThreadCache::setLargeCacheLimit(size_t bytes)
{
    g_largeCacheLimitBytes.store(bytes, std::memory_order_relaxed);
}

size_t ThreadCache::largeCacheLimit()
{
    return g_largeCacheLimitBytes.load(std::memory_order_relaxed);
}

void* ThreadCache::fetchFromCentralCache(size_t index)
{
    const size_t size = SizeClass::getSize(index);
    const size_t batchNum = getBatchNum(size);
    size_t fetchedCount = 0;
    void* start = CentralCache::getInstance().fetchRange(index, batchNum, fetchedCount);
    if (!start)
        return nullptr;

    void* result = start;
    freeList_[index] = *reinterpret_cast<void**>(start);
    freeListSize_[index] += fetchedCount > 0 ? fetchedCount - 1 : 0;
    if (fetchedCount > 0)
        recordCentralRefill(index, fetchedCount);
    if (kDebugGuardsEnabled)
    {
        Span* span = PageCache::getInstance().findSpan(result);
        if (!debugMarkAllocated(span, result))
            return nullptr;
    }
    return result;
}

void ThreadCache::returnToCentralCache(size_t index)
{
    size_t batchNum = freeListSize_[index];
    if (batchNum <= 1)
        return;

    size_t keepNum = std::max(batchNum / 2, size_t(1));
    keepNum = std::min(keepNum, maxCachedBlocks(SizeClass::getSize(index)));
    if (keepNum >= batchNum)
        return;

    void* splitNode = freeList_[index];
    for (size_t i = 0; i < keepNum - 1; ++i)
    {
        if (!splitNode)
            return;
        splitNode = *reinterpret_cast<void**>(splitNode);
    }
    if (!splitNode)
        return;

    void* nextNode = *reinterpret_cast<void**>(splitNode);
    *reinterpret_cast<void**>(splitNode) = nullptr;
    freeListSize_[index] = keepNum;
    if (nextNode)
    {
        recordCentralFlush(index, batchNum - keepNum);
        CentralCache::getInstance().pushTransfer(nextNode, batchNum - keepNum, index);
    }
}

} // namespace Kama_memoryPool
