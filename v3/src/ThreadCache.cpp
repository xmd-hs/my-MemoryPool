#include "ThreadCache.h"
#include "CentralCache.h"
#include "PageCache.h"

#include <algorithm>

namespace Kama_memoryPool
{

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
    constexpr size_t kMaxBytes = 32 * 1024;
    size_t n = std::max(size_t(1), kMaxBytes / std::max(size, ALIGNMENT));
    return std::min(n, size_t(256));
}

bool ThreadCache::shouldReturnToCentralCache(size_t index) const
{
    return freeListSize_[index] > maxCachedBlocks(SizeClass::getSize(index));
}

size_t ThreadCache::getBatchNum(size_t size) const
{
    constexpr size_t kMaxBatchBytes = 8 * 1024;
    size_t n = std::max(size_t(1), kMaxBatchBytes / std::max(size, ALIGNMENT));
    if (size <= 32) n = std::min(n, size_t(64));
    else if (size <= 64) n = std::min(n, size_t(32));
    else if (size <= 256) n = std::min(n, size_t(16));
    else if (size <= 1024) n = std::min(n, size_t(8));
    else n = std::min(n, size_t(4));
    return std::max(size_t(1), n);
}

void* ThreadCache::allocate(size_t size)
{
    if (size == 0)
        size = ALIGNMENT;

    if (size > MAX_BYTES)
    {
        const size_t ps = PageCache::pageSize();
        size_t numPages = (size + ps - 1) / ps;
        Span* span = PageCache::getInstance().allocateSpan(numPages);
        if (!span)
            return nullptr;
        span->isLarge = true;
        span->blockSize = size;
        span->blockCount = 1;
        span->freeCount = 0;
        span->sizeClass = static_cast<size_t>(-1);
        return span->pageAddr;
    }

    const size_t index = SizeClass::getIndex(size);
    if (void* ptr = freeList_[index])
    {
        freeListSize_[index]--;
        freeList_[index] = *reinterpret_cast<void**>(ptr);
        return ptr;
    }

    return fetchFromCentralCache(index);
}

void* ThreadCache::allocateAligned(size_t size, size_t alignment)
{
    if (alignment <= ALIGNMENT)
        return allocate(size);
    if (alignment == 0 || (alignment & (alignment - 1)) != 0)
        alignment = ALIGNMENT;

    size_t need = (size + alignment - 1) & ~(alignment - 1);
    return allocate(need);
}

void ThreadCache::deallocate(void* ptr)
{
    if (!ptr)
        return;

    Span* span = PageCache::getInstance().findSpan(ptr);
    if (!span)
        return;

    if (span->isLarge)
    {
        PageCache::getInstance().deallocateSpan(span);
        return;
    }

    const size_t index = span->sizeClass;
    if (index >= FREE_LIST_SIZE)
        return;

    *reinterpret_cast<void**>(ptr) = freeList_[index];
    freeList_[index] = ptr;
    freeListSize_[index]++;

    if (shouldReturnToCentralCache(index))
        returnToCentralCache(index);
}

void ThreadCache::deallocate(void* ptr, size_t /*size*/)
{
    deallocate(ptr);
}

void* ThreadCache::fetchFromCentralCache(size_t index)
{
    const size_t size = SizeClass::getSize(index);
    const size_t batchNum = getBatchNum(size);
    void* start = CentralCache::getInstance().fetchRange(index, batchNum);
    if (!start)
        return nullptr;

    void* result = start;
    void* next = *reinterpret_cast<void**>(start);
    freeList_[index] = next;

    size_t remain = 0;
    for (void* current = next; current != nullptr;
         current = *reinterpret_cast<void**>(current))
    {
        ++remain;
    }
    freeListSize_[index] += remain;
    return result;
}

void ThreadCache::returnToCentralCache(size_t index)
{
    size_t batchNum = freeListSize_[index];
    if (batchNum <= 1)
        return;

    size_t keepNum = std::max(batchNum / 4, size_t(1));
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
        CentralCache::getInstance().returnRange(nextNode, batchNum - keepNum, index);
}

} // namespace Kama_memoryPool
