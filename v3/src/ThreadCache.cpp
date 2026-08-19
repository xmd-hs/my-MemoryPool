#include "ThreadCache.h"
#include "CentralCache.h"
#include "PageCache.h"

#include <algorithm>

namespace Kama_memoryPool
{

namespace
{

size_t debugBlockIndex(const Span* span, const void* ptr)
{
    const auto* start = static_cast<const char*>(span->pageAddr);
    const auto* target = static_cast<const char*>(ptr);
    if (target < start)
        return static_cast<size_t>(-1);
    const size_t offset = static_cast<size_t>(target - start);
    if (span->blockSize == 0 || offset % span->blockSize != 0)
        return static_cast<size_t>(-1);
    return offset / span->blockSize;
}

bool debugMarkAllocated(Span* span, void* ptr)
{
    if constexpr (!kDebugGuardsEnabled)
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

} // namespace

ThreadCache::ThreadCache()
{
    freeList_.fill(nullptr);
    freeListSize_.fill(0);
}

ThreadCache::~ThreadCache()
{
    flushAll();
}

void ThreadCache::flushAll()
{
    for (size_t i = 0; i < FREE_LIST_SIZE; ++i)
    {
        if (freeList_[i] && freeListSize_[i] > 0)
        {
            CentralCache::getInstance().returnRange(freeList_[i], freeListSize_[i], i);
            freeList_[i] = nullptr;
            freeListSize_[i] = 0;
        }
    }
}

size_t ThreadCache::maxCachedBlocks(size_t size) const
{
    constexpr size_t kBaseCacheBytes = 256 * 1024;
    size_t n = std::max(size_t(1), kBaseCacheBytes / std::max(size, ALIGNMENT));
    if (size <= 32) return 4096;
    if (size <= 64) return 2048;
    if (size <= 256) return 1024;
    if (size <= 1024) return 256;
    if (size <= 4096) return 512;
    return std::min(n, size_t(128));
}

bool ThreadCache::shouldReturnToCentralCache(size_t index) const
{
    return freeListSize_[index] > maxCachedBlocks(SizeClass::getSize(index));
}

size_t ThreadCache::getBatchNum(size_t size) const
{
    constexpr size_t kMaxBatchBytes = 64 * 1024;
    size_t n = std::max(size_t(1), kMaxBatchBytes / std::max(size, ALIGNMENT));
    if (size <= 32) n = std::min(n, size_t(512));
    else if (size <= 64) n = std::min(n, size_t(256));
    else if (size <= 256) n = std::min(n, size_t(128));
    else if (size <= 1024) n = std::min(n, size_t(32));
    else n = std::min(n, size_t(16));
    return std::max(size_t(1), n);
}

void* ThreadCache::allocate(size_t size)
{
    if (size == 0)
        size = ALIGNMENT;

    if (size > MAX_BYTES)
    {
        const size_t ps = PageCache::pageSize();
        if (size > SIZE_MAX - (ps - 1))
            return nullptr;
        size_t numPages = (size + ps - 1) / ps;
        Span* span = PageCache::getInstance().allocateSpan(numPages);
        if (!span)
            return nullptr;
        span->isLarge = true;
        span->blockSize = size;
        span->blockCount = 1;
        span->freeCount = 0;
        span->sizeClass = static_cast<size_t>(-1);
        span->debugAllocMap.clear();
        span->debugLargeAllocated = true;
        return span->pageAddr;
    }

    const size_t index = SizeClass::getIndex(size);
    if (void* ptr = freeList_[index])
    {
        freeListSize_[index]--;
        freeList_[index] = *reinterpret_cast<void**>(ptr);
        if constexpr (kDebugGuardsEnabled)
        {
            Span* span = PageCache::getInstance().findSpan(ptr);
            if (!debugMarkAllocated(span, ptr))
                return nullptr;
        }
        return ptr;
    }

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
    return allocate(need);
}

bool ThreadCache::deallocate(void* ptr)
{
    if (!ptr)
        return false;

    Span* span = PageCache::getInstance().findSpan(ptr);
    if (!span)
        return false;

    if (!debugValidateAndMarkFreed(span, ptr, span->isLarge ? static_cast<size_t>(-1) : span->sizeClass))
        return false;

    if (span->isLarge)
    {
        PageCache::getInstance().deallocateSpan(span);
        return true;
    }

    const size_t index = span->sizeClass;
    if (index >= FREE_LIST_SIZE)
        return false;

    *reinterpret_cast<void**>(ptr) = freeList_[index];
    freeList_[index] = ptr;
    freeListSize_[index]++;

    if (shouldReturnToCentralCache(index))
        returnToCentralCache(index);
    return true;
}

bool ThreadCache::deallocate(void* ptr, size_t size)
{
    if (!ptr)
        return false;

    if (size == 0)
        size = ALIGNMENT;

    if (size > MAX_BYTES)
    {
        if (kDebugGuardsEnabled)
        {
            Span* span = PageCache::getInstance().findSpan(ptr);
            if (!debugValidateAndMarkFreed(span, ptr, static_cast<size_t>(-1)))
                return false;
            if (span && span->isLarge)
            {
                PageCache::getInstance().deallocateSpan(span);
                return true;
            }
        }
        return deallocate(ptr);
    }

    const size_t rounded = SizeClass::roundUp(size);
    const size_t index = SizeClass::getIndex(rounded);
    if (index >= FREE_LIST_SIZE)
    {
        return deallocate(ptr);
    }

    if (kDebugGuardsEnabled)
    {
        Span* span = PageCache::getInstance().findSpan(ptr);
        if (!debugValidateAndMarkFreed(span, ptr, index))
            return false;
    }

    *reinterpret_cast<void**>(ptr) = freeList_[index];
    freeList_[index] = ptr;
    freeListSize_[index]++;

    if (shouldReturnToCentralCache(index))
        returnToCentralCache(index);
    return true;
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
        recordCentralRefill(fetchedCount);
    if constexpr (kDebugGuardsEnabled)
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
        recordCentralFlush(batchNum - keepNum);
        CentralCache::getInstance().returnRange(nextNode, batchNum - keepNum, index);
    }
}

} // namespace Kama_memoryPool
