#include "PageCache.h"

#include <algorithm>
#include <unordered_set>

namespace Kama_memoryPool
{

PageMap::~PageMap()
{
    for (size_t r = 0; r < kSize; ++r)
    {
        Node* mid = root_[r].exchange(nullptr, std::memory_order_relaxed);
        if (!mid)
            continue;
        for (size_t m = 0; m < kSize; ++m)
        {
            auto* leaf = static_cast<Leaf*>(mid->slots[m].exchange(nullptr, std::memory_order_relaxed));
            delete leaf;
        }
        delete mid;
    }
}

PageMap::Node* PageMap::getOrCreateNode(std::atomic<void*>& slot)
{
    void* existing = slot.load(std::memory_order_acquire);
    if (existing)
        return static_cast<Node*>(existing);

    std::lock_guard<std::mutex> lock(growMutex_);
    existing = slot.load(std::memory_order_relaxed);
    if (existing)
        return static_cast<Node*>(existing);
    Node* node = new Node();
    slot.store(node, std::memory_order_release);
    return node;
}

PageMap::Leaf* PageMap::getOrCreateLeaf(std::atomic<void*>& slot)
{
    void* existing = slot.load(std::memory_order_acquire);
    if (existing)
        return static_cast<Leaf*>(existing);

    std::lock_guard<std::mutex> lock(growMutex_);
    existing = slot.load(std::memory_order_relaxed);
    if (existing)
        return static_cast<Leaf*>(existing);
    Leaf* leaf = new Leaf();
    slot.store(leaf, std::memory_order_release);
    return leaf;
}

void PageMap::set(uintptr_t pageId, Span* span)
{
    const uintptr_t rootIndex = (pageId >> (2 * kBits)) & (kSize - 1);
    const uintptr_t midIndex = (pageId >> kBits) & (kSize - 1);
    const uintptr_t leafIndex = pageId & (kSize - 1);

    Node* mid = root_[rootIndex].load(std::memory_order_acquire);
    if (!mid)
    {
        std::lock_guard<std::mutex> lock(growMutex_);
        mid = root_[rootIndex].load(std::memory_order_relaxed);
        if (!mid)
        {
            mid = new Node();
            root_[rootIndex].store(mid, std::memory_order_release);
        }
    }

    Leaf* leaf = getOrCreateLeaf(mid->slots[midIndex]);
    leaf->slots[leafIndex].store(pack(span), std::memory_order_release);
}

void PageMap::clear(uintptr_t pageId)
{
    const uintptr_t rootIndex = (pageId >> (2 * kBits)) & (kSize - 1);
    const uintptr_t midIndex = (pageId >> kBits) & (kSize - 1);
    const uintptr_t leafIndex = pageId & (kSize - 1);

    Node* mid = root_[rootIndex].load(std::memory_order_acquire);
    if (!mid)
        return;
    auto* leaf = static_cast<Leaf*>(mid->slots[midIndex].load(std::memory_order_acquire));
    if (!leaf)
        return;
    leaf->slots[leafIndex].store(0, std::memory_order_release);
}

uintptr_t PageMap::loadPacked(uintptr_t pageId) const
{
    const uintptr_t rootIndex = (pageId >> (2 * kBits)) & (kSize - 1);
    const uintptr_t midIndex = (pageId >> kBits) & (kSize - 1);
    const uintptr_t leafIndex = pageId & (kSize - 1);

    Node* mid = root_[rootIndex].load(std::memory_order_relaxed);
    if (!mid)
        return 0;
    auto* leaf = static_cast<Leaf*>(mid->slots[midIndex].load(std::memory_order_relaxed));
    if (!leaf)
        return 0;
    return leaf->slots[leafIndex].load(std::memory_order_relaxed);
}

uintptr_t PageMap::loadPackedCached(uintptr_t pageId, void*& cachedLeaf, uintptr_t& cachedLeafId) const
{
    const uintptr_t leafId = pageId >> kBits;
    const uintptr_t leafIndex = pageId & (kSize - 1);
    Leaf* leaf = nullptr;
    if (cachedLeaf && cachedLeafId == leafId)
    {
        leaf = static_cast<Leaf*>(cachedLeaf);
    }
    else
    {
        const uintptr_t rootIndex = (pageId >> (2 * kBits)) & (kSize - 1);
        const uintptr_t midIndex = leafId & (kSize - 1);
        Node* mid = root_[rootIndex].load(std::memory_order_relaxed);
        if (!mid)
        {
            cachedLeaf = nullptr;
            cachedLeafId = leafId;
            return 0;
        }
        leaf = static_cast<Leaf*>(mid->slots[midIndex].load(std::memory_order_relaxed));
        cachedLeaf = leaf;
        cachedLeafId = leafId;
        if (!leaf)
            return 0;
    }
    return leaf->slots[leafIndex].load(std::memory_order_relaxed);
}

Span* PageMap::get(uintptr_t pageId) const
{
    return unpackSpan(loadPacked(pageId));
}

Span* PageMap::getCached(uintptr_t pageId, void*& cachedLeaf, uintptr_t& cachedLeafId) const
{
    return unpackSpan(loadPackedCached(pageId, cachedLeaf, cachedLeafId));
}

size_t PageMap::getSizeClass(uintptr_t pageId) const
{
    const uintptr_t bits = loadPacked(pageId);
    if (bits == 0)
        return FREE_LIST_SIZE;
    const size_t sc = unpackSizeClass(bits);
    if (sc < FREE_LIST_SIZE)
        return sc;
    Span* span = unpackSpan(bits);
    if (!span || span->isLarge)
        return FREE_LIST_SIZE;
    return span->sizeClass;
}

size_t PageMap::getSizeClassCached(uintptr_t pageId, void*& cachedLeaf, uintptr_t& cachedLeafId) const
{
    const uintptr_t bits = loadPackedCached(pageId, cachedLeaf, cachedLeafId);
    if (bits == 0)
        return FREE_LIST_SIZE;
    const size_t sc = unpackSizeClass(bits);
    if (sc < FREE_LIST_SIZE)
        return sc;
    Span* span = unpackSpan(bits);
    if (!span || span->isLarge)
        return FREE_LIST_SIZE;
    return span->sizeClass;
}

PageCache::~PageCache()
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::unordered_set<Span*> spans;
    for (auto& kv : largeSpans_)
        spans.insert(kv.second);
    largeSpans_.clear();

    for (auto& kv : freeSpans_)
    {
        for (Span* span = kv.second; span; span = span->next)
            spans.insert(span);
    }
    freeSpans_.clear();

    // Any remaining mapped spans (in-use at exit) are collected via reserved
    // origins below; Span objects for in-use pages are still in `spans` only if
    // tracked. Walk system allocations is enough to release OS memory.
    for (Span* span : spans)
        delete span;

    for (auto& alloc : systemAllocs_)
        freePages(alloc.first, alloc.second);
    systemAllocs_.clear();
}

uintptr_t PageCache::pageId(void* ptr) const
{
    return reinterpret_cast<uintptr_t>(ptr) >> systemPageShift();
}

void PageCache::registerPages(Span* span)
{
    const size_t ps = systemPageSize();
    auto id = reinterpret_cast<uintptr_t>(span->pageAddr) / ps;
    for (size_t i = 0; i < span->numPages; ++i)
        pageMap_.set(id + i, span);
}

void PageCache::refreshPageTags(Span* span)
{
    if (!span || span->isLarge)
        return;
    registerPages(span);
}

void PageCache::unregisterPages(Span* span)
{
    const size_t ps = systemPageSize();
    auto id = reinterpret_cast<uintptr_t>(span->pageAddr) / ps;
    for (size_t i = 0; i < span->numPages; ++i)
        pageMap_.clear(id + i);
}

void PageCache::redirectPages(Span* from, Span* to)
{
    const size_t ps = systemPageSize();
    auto id = reinterpret_cast<uintptr_t>(from->pageAddr) / ps;
    for (size_t i = 0; i < from->numPages; ++i)
        pageMap_.set(id + i, to);
}

Span* PageCache::findSpanFromPageMap(void* ptr) const
{
    return pageMap_.get(pageId(ptr));
}

Span* PageCache::findLargeSpan(void* ptr)
{
    if (!ptr)
        return nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = largeSpans_.find(ptr);
    if (it == largeSpans_.end())
        return nullptr;
    return it->second;
}

namespace
{
thread_local void* t_mapLeaf = nullptr;
thread_local uintptr_t t_mapLeafId = ~uintptr_t(0);
}

Span* PageCache::findSpan(void* ptr)
{
    if (!ptr)
        return nullptr;
    // Small / size-class spans only. Large objects are looked up via
    // findLargeSpan by callers that need them, so unsized small frees never
    // take mutex_.
    return pageMap_.getCached(pageId(ptr), t_mapLeaf, t_mapLeafId);
}

size_t PageCache::findSizeClass(void* ptr) const
{
    if (!ptr)
        return FREE_LIST_SIZE;
    const size_t ps = systemPageSize();
    if (const PageTag* tag = pageTagIfValid(ptr, ps))
        return tag->sizeClass;
    return pageMap_.getSizeClassCached(pageId(ptr), t_mapLeaf, t_mapLeafId);
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
    largeSpans_.erase(span->pageAddr);
    if (findSpanFromPageMap(span->pageAddr) == span)
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
        else if (findSpanFromPageMap(span->pageAddr) != span)
        {
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

Span* PageCache::allocateLargeSpan(size_t numPages)
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
            if (findSpanFromPageMap(span->pageAddr) == span)
                unregisterPages(span);
        }
        else if (findSpanFromPageMap(span->pageAddr) == span)
        {
            unregisterPages(span);
        }

        span->isLarge = true;
        largeSpans_[span->pageAddr] = span;
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
    span->isLarge = true;
    largeSpans_[span->pageAddr] = span;
    return span;
}

void PageCache::deallocateSpan(Span* span)
{
    if (!span)
        return;

    std::lock_guard<std::mutex> lock(mutex_);

    const bool wasLarge = span->isLarge;
    if (wasLarge)
        largeSpans_.erase(span->pageAddr);

    span->freeList = nullptr;
    span->blockSize = 0;
    span->blockCount = 0;
    span->freeCount = 0;
    span->sizeClass = 0;
    if (span->blocksPerPage > 0)
    {
        const size_t ps = systemPageSize();
        auto* base = static_cast<char*>(span->pageAddr);
        for (size_t p = 0; p < span->numPages; ++p)
        {
            auto* tag = reinterpret_cast<PageTag*>(base + (p + 1) * ps - kPageTagBytes);
            tag->magic = 0;
            tag->sizeClass = 0;
        }
    }
    span->blocksPerPage = 0;
    span->isLarge = false;
    span->next = nullptr;
    span->debugAllocMap.clear();
    span->debugLargeAllocated = false;

    if (!wasLarge)
    {
        void* nextAddr = static_cast<char*>(span->pageAddr) + span->numPages * systemPageSize();
        Span* nextSpan = findSpanFromPageMap(nextAddr);
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

        if (span->pageAddr != span->originAddr)
        {
            void* prevPtr = static_cast<char*>(span->pageAddr) - 1;
            Span* prevCandidate = findSpanFromPageMap(prevPtr);
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
