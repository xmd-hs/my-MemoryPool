#include "PageCache.h"

#include <algorithm>
#include <unordered_set>

namespace Kama_memoryPool
{

PageCache::~PageCache()
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::unordered_set<Span*> spans;
    for (auto& kv : pageMap_)
        spans.insert(kv.second);
    pageMap_.clear();
    freeSpans_.clear();

    for (Span* span : spans)
        delete span;

    for (auto& alloc : systemAllocs_)
        freePages(alloc.first, alloc.second);
    systemAllocs_.clear();
}

uintptr_t PageCache::pageId(void* ptr) const
{
    return reinterpret_cast<uintptr_t>(ptr) / systemPageSize();
}

void PageCache::registerPages(Span* span)
{
    std::unique_lock<std::shared_mutex> lock(pageMapMutex_);
    const size_t ps = systemPageSize();
    auto id = reinterpret_cast<uintptr_t>(span->pageAddr) / ps;
    for (size_t i = 0; i < span->numPages; ++i)
        pageMap_[id + i] = span;
}

void PageCache::unregisterPages(Span* span)
{
    std::unique_lock<std::shared_mutex> lock(pageMapMutex_);
    const size_t ps = systemPageSize();
    auto id = reinterpret_cast<uintptr_t>(span->pageAddr) / ps;
    for (size_t i = 0; i < span->numPages; ++i)
        pageMap_.erase(id + i);
}

void PageCache::redirectPages(Span* from, Span* to)
{
    std::unique_lock<std::shared_mutex> lock(pageMapMutex_);
    const size_t ps = systemPageSize();
    auto id = reinterpret_cast<uintptr_t>(from->pageAddr) / ps;
    for (size_t i = 0; i < from->numPages; ++i)
        pageMap_[id + i] = to;
}

Span* PageCache::findSpanLocked(void* ptr) const
{
    auto it = pageMap_.find(pageId(ptr));
    if (it == pageMap_.end())
        return nullptr;
    return it->second;
}

Span* PageCache::findSpan(void* ptr)
{
    if (!ptr)
        return nullptr;
    std::shared_lock<std::shared_mutex> lock(pageMapMutex_);
    return findSpanLocked(ptr);
}

size_t PageCache::reservedBytes()
{
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total = 0;
    for (const auto& allocation : systemAllocs_)
        total += allocation.second;
    return total;
}

size_t PageCache::cachedPageBytes()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return cachedFreePages_ * systemPageSize();
}

void* PageCache::systemAlloc(size_t& numPages)
{
    const size_t ps = systemPageSize();
    const size_t gran = systemAllocGranularity();
    if (numPages > SIZE_MAX / ps)
        return nullptr;
    size_t bytes = numPages * ps;
    if (bytes > SIZE_MAX - (gran - 1))
        return nullptr;
    bytes = (bytes + gran - 1) & ~(gran - 1);
    numPages = bytes / ps;

    void* ptr = allocPages(bytes);
    if (!ptr)
        return nullptr;

    systemAllocs_.emplace_back(ptr, bytes);
    return ptr;
}

void PageCache::releaseOrigin(Span* span)
{
    unregisterPages(span);

    void* addr = span->originAddr;
    size_t bytes = span->originPages * systemPageSize();
    auto it = std::find_if(systemAllocs_.begin(), systemAllocs_.end(),
                           [addr](const std::pair<void*, size_t>& item) {
                               return item.first == addr;
                           });
    if (it != systemAllocs_.end())
        systemAllocs_.erase(it);

    delete span;
    freePages(addr, bytes);
}

Span* PageCache::allocateSpan(size_t numPages)
{
    if (numPages == 0)
        numPages = 1;

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = freeSpans_.lower_bound(numPages);
    if (it != freeSpans_.end())
    {
        Span* span = it->second;
        cachedFreePages_ -= span->numPages;
        if (span->next)
            freeSpans_[it->first] = span->next;
        else
            freeSpans_.erase(it);
        span->next = nullptr;
        span->inFreeList = false;

        if (span->numPages > numPages)
        {
            Span* rest = new Span;
            rest->pageAddr = static_cast<char*>(span->pageAddr) + numPages * systemPageSize();
            rest->numPages = span->numPages - numPages;
            rest->originAddr = span->originAddr;
            rest->originPages = span->originPages;
            rest->inFreeList = true;
            rest->next = nullptr;

            auto& list = freeSpans_[rest->numPages];
            rest->next = list;
            list = rest;
            cachedFreePages_ += rest->numPages;
            registerPages(rest);

            span->numPages = numPages;
            registerPages(span);
        }

        return span;
    }

    size_t allocPagesCount = numPages;
    void* memory = systemAlloc(allocPagesCount);
    if (!memory)
        return nullptr;

    Span* span = new Span;
    span->pageAddr = memory;
    span->numPages = allocPagesCount;
    span->originAddr = memory;
    span->originPages = allocPagesCount;
    registerPages(span);
    return span;
}

void PageCache::deallocateSpan(Span* span)
{
    if (!span)
        return;

    std::lock_guard<std::mutex> lock(mutex_);

    // A large allocation may have been carved from a larger origin span.
    // Release the origin directly only when this span owns the whole origin.
    const bool returnDirect = span->isLarge &&
                              span->pageAddr == span->originAddr &&
                              span->numPages == span->originPages;
    span->freeList = nullptr;
    span->blockSize = 0;
    span->blockCount = 0;
    span->freeCount = 0;
    span->sizeClass = 0;
    span->isLarge = false;
    span->next = nullptr;
    span->debugAllocMap.clear();
    span->debugLargeAllocated = false;

    if (returnDirect)
    {
        releaseOrigin(span);
        return;
    }

    void* nextAddr = static_cast<char*>(span->pageAddr) + span->numPages * systemPageSize();
    Span* nextSpan = findSpanLocked(nextAddr);
    if (nextSpan && nextSpan != span && nextSpan->inFreeList &&
        nextSpan->pageAddr == nextAddr &&
        nextSpan->originAddr == span->originAddr)
    {
        if (removeFromFreeList(nextSpan))
        {
            span->numPages += nextSpan->numPages;
            redirectPages(nextSpan, span);
            delete nextSpan;
        }
    }

    Span* prevCandidate = nullptr;
    if (span->pageAddr != span->originAddr)
    {
        void* prevPtr = static_cast<char*>(span->pageAddr) - 1;
        prevCandidate = findSpanLocked(prevPtr);
    }
    if (prevCandidate && prevCandidate != span && prevCandidate->inFreeList &&
        prevCandidate->originAddr == span->originAddr)
    {
        void* prevEnd = static_cast<char*>(prevCandidate->pageAddr) +
                        prevCandidate->numPages * systemPageSize();
        if (prevEnd == span->pageAddr && removeFromFreeList(prevCandidate))
        {
            redirectPages(span, prevCandidate);
            prevCandidate->numPages += span->numPages;
            delete span;
            span = prevCandidate;
        }
    }

    constexpr size_t kMaxCachedBytes = 64 * 1024 * 1024;
    const size_t maxCachedPages = kMaxCachedBytes / systemPageSize();
    if (span->originAddr == span->pageAddr && span->numPages == span->originPages &&
        cachedFreePages_ + span->numPages > maxCachedPages)
    {
        releaseOrigin(span);
        return;
    }

    span->inFreeList = true;
    auto& list = freeSpans_[span->numPages];
    span->next = list;
    list = span;
    cachedFreePages_ += span->numPages;
}

bool PageCache::removeFromFreeList(Span* span)
{
    auto it = freeSpans_.find(span->numPages);
    if (it == freeSpans_.end())
        return false;

    Span*& list = it->second;
    if (list == span)
    {
        cachedFreePages_ -= span->numPages;
        list = span->next;
        if (!list)
            freeSpans_.erase(it);
        span->next = nullptr;
        span->inFreeList = false;
        return true;
    }

    Span* prev = list;
    while (prev && prev->next)
    {
        if (prev->next == span)
        {
            cachedFreePages_ -= span->numPages;
            prev->next = span->next;
            span->next = nullptr;
            span->inFreeList = false;
            return true;
        }
        prev = prev->next;
    }
    return false;
}

} // namespace Kama_memoryPool
