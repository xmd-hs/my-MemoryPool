#include "CentralCache.h"

#include <algorithm>

namespace Kama_memoryPool
{

CentralCache::CentralCache()
{
    for (auto& shard : shards_)
    {
        shard.lock.clear();
        shard.partial = nullptr;
    }
}

Span* CentralCache::createSpan(size_t index)
{
    const size_t blockSize = SizeClass::getSize(index);
    const size_t ps = PageCache::pageSize();
    // Larger spans amortize CentralCache/PageCache synchronization for the
    // medium-size classes that dominate mixed allocation workloads.
    size_t targetBytes = 0;
    if (blockSize <= 256)
        targetBytes = 32 * 1024;
    else if (blockSize <= 1024)
        targetBytes = 128 * 1024;
    else if (blockSize <= 4096)
        targetBytes = 512 * 1024;
    else
        targetBytes = 2 * 1024 * 1024;
    targetBytes = std::max(ps, targetBytes);
    if (blockSize > targetBytes)
        targetBytes = ((blockSize + ps - 1) / ps) * ps;
    size_t numPages = targetBytes / ps;
    if (numPages == 0)
        numPages = 1;

    Span* span = PageCache::getInstance().allocateSpan(numPages);
    if (!span)
        return nullptr;

    const size_t bytes = span->numPages * ps;
    const size_t n = bytes / blockSize;
    if (n == 0)
    {
        PageCache::getInstance().deallocateSpan(span);
        return nullptr;
    }

    char* start = static_cast<char*>(span->pageAddr);
    for (size_t i = 0; i + 1 < n; ++i)
        *reinterpret_cast<void**>(start + i * blockSize) = start + (i + 1) * blockSize;
    *reinterpret_cast<void**>(start + (n - 1) * blockSize) = nullptr;

    span->blockSize = blockSize;
    span->blockCount = n;
    span->freeCount = n;
    span->freeList = start;
    span->sizeClass = index;
    span->isLarge = false;
    span->next = nullptr;
    span->debugLargeAllocated = false;
    if constexpr (kDebugGuardsEnabled)
        span->debugAllocMap.assign(n, 0);
    return span;
}

void CentralCache::unlinkPartial(size_t index, Span* span)
{
    Span** cursor = &shards_[index].partial;
    while (*cursor)
    {
        if (*cursor == span)
        {
            *cursor = span->next;
            span->next = nullptr;
            return;
        }
        cursor = &(*cursor)->next;
    }
}

void* CentralCache::fetchRange(size_t index, size_t batchNum, size_t& fetchedCount)
{
    fetchedCount = 0;
    if (index >= FREE_LIST_SIZE || batchNum == 0)
        return nullptr;

    SpinLock guard(shards_[index].lock);

    void* head = nullptr;
    void* tail = nullptr;
    size_t got = 0;

    while (got < batchNum)
    {
        Span* span = shards_[index].partial;
        if (!span)
        {
            span = createSpan(index);
            if (!span)
                break;
            shards_[index].partial = span;
        }

        void* obj = span->freeList;
        span->freeList = *reinterpret_cast<void**>(obj);
        span->freeCount--;
        *reinterpret_cast<void**>(obj) = nullptr;

        if (!head)
            head = obj;
        else
            *reinterpret_cast<void**>(tail) = obj;
        tail = obj;
        ++got;

        if (span->freeCount == 0)
        {
            shards_[index].partial = span->next;
            span->next = nullptr;
        }
    }

    fetchedCount = got;
    return head;
}

void CentralCache::returnRange(void* start, size_t /*blockCount*/, size_t index)
{
    if (!start || index >= FREE_LIST_SIZE)
        return;

    SpinLock guard(shards_[index].lock);
    PageCache& cache = PageCache::getInstance();

    void* cur = start;
    while (cur)
    {
        void* next = *reinterpret_cast<void**>(cur);
        Span* span = cache.findSpan(cur);
        if (!span || span->sizeClass != index)
        {
            cur = next;
            continue;
        }

        const bool wasEmpty = (span->freeCount == 0);
        *reinterpret_cast<void**>(cur) = span->freeList;
        span->freeList = cur;
        span->freeCount++;

        if (span->freeCount == span->blockCount)
        {
            unlinkPartial(index, span);
            cache.deallocateSpan(span);
        }
        else if (wasEmpty)
        {
            span->next = shards_[index].partial;
            shards_[index].partial = span;
        }

        cur = next;
    }
}

} // namespace Kama_memoryPool
