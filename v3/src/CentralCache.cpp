#include "CentralCache.h"
#include "Platform.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace Kama_memoryPool
{

CentralCache::CentralCache()
{
    for (auto& shard : shards_)
    {
        shard.lock.clear();
        shard.partial = nullptr;
    }
    for (auto& remote : remoteFreeLists_)
        remote.head.store(nullptr, std::memory_order_relaxed);
    for (auto& pending : remotePending_)
        pending.store(0, std::memory_order_relaxed);
    for (auto& bin : transfer_)
    {
        bin.head.store(nullptr, std::memory_order_relaxed);
        bin.count.store(0, std::memory_order_relaxed);
    }
}

size_t CentralCache::localShardId() const
{
    static thread_local const size_t shard =
        std::hash<std::thread::id>{}(std::this_thread::get_id()) % kCentralShardCount;
    return shard;
}

size_t CentralCache::shardIndex(size_t sizeClassIndex, size_t shardId) const
{
    return sizeClassIndex * kCentralShardCount + shardId;
}

Span* CentralCache::createSpan(size_t index)
{
    // createSpan uses SizeClass::slotSize — for usable > 256 that equals usable
    // (no header); for small classes it includes kBlockHeaderSize.
    const size_t blockSize = SizeClass::slotSize(index);
    const size_t usable = SizeClass::getSize(index);
    const size_t ps = PageCache::pageSize();
    size_t targetBytes = 0;
    if (usable <= 256)
        targetBytes = 32 * 1024;
    else if (usable <= 1024)
        targetBytes = 128 * 1024;
    else if (usable <= 4096)
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
    void* head = nullptr;
    size_t n = 0;
    size_t blocksPerPage = 0;
    char* start = static_cast<char*>(span->pageAddr);

    if (blockSize != 0 && blockSize <= 256 && blockSize <= ps - kPageTagBytes)
    {
        blocksPerPage = (ps - kPageTagBytes) / blockSize;
        char* prev = nullptr;
        for (size_t p = 0; p < span->numPages; ++p)
        {
            char* page = start + p * ps;
            const uintptr_t pageBase = reinterpret_cast<uintptr_t>(page);
            auto* tag = reinterpret_cast<PageTag*>(page + ps - kPageTagBytes);
            tag->magic = pageTagCookie(pageBase);
            tag->sizeClass = static_cast<uint32_t>(index);
            tag->_pad = 0;

            for (size_t i = 0; i < blocksPerPage; ++i)
            {
                char* blk = page + i * blockSize;
                if (prev)
                    *reinterpret_cast<void**>(prev) = blk;
                else
                    head = blk;
                prev = blk;
                ++n;
            }
        }
        if (prev)
            *reinterpret_cast<void**>(prev) = nullptr;
    }
    else
    {
        n = blockSize ? bytes / blockSize : 0;
        if (n == 0)
        {
            PageCache::getInstance().deallocateSpan(span);
            return nullptr;
        }
        for (size_t i = 0; i + 1 < n; ++i)
            *reinterpret_cast<void**>(start + i * blockSize) = start + (i + 1) * blockSize;
        *reinterpret_cast<void**>(start + (n - 1) * blockSize) = nullptr;
        head = start;
    }

    if (n == 0)
    {
        PageCache::getInstance().deallocateSpan(span);
        return nullptr;
    }

    span->blockSize = blockSize;
    span->blockCount = n;
    span->freeCount = n;
    span->freeList = head;
    span->blocksPerPage = blocksPerPage;
    span->sizeClass = index;
    span->isLarge = false;
    span->next = nullptr;
    // Home shard is assigned by the caller under that shard's lock.
    span->centralShard.store(static_cast<size_t>(-1), std::memory_order_relaxed);
    span->debugLargeAllocated = false;
    if constexpr (kDebugGuardsEnabled)
        span->debugAllocMap.assign(n, 0);
    // sizeClass is now final — refresh packed page-map tags for unsized free.
    PageCache::getInstance().refreshPageTags(span);
    return span;
}

void CentralCache::unlinkPartial(size_t shardIdx, Span* span)
{
    Span** cursor = &shards_[shardIdx].partial;
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

size_t CentralCache::popRangeFromShardLocked(size_t shardIdx, size_t limit,
                                             void*& head, void*& tail)
{
    head = nullptr;
    tail = nullptr;
    if (limit == 0)
        return 0;

    Span* span = shards_[shardIdx].partial;
    if (!span || !span->freeList)
        return 0;

    head = span->freeList;
    tail = head;
    size_t count = 1;
    while (count < limit && *reinterpret_cast<void**>(tail))
    {
        tail = *reinterpret_cast<void**>(tail);
        ++count;
    }

    span->freeList = *reinterpret_cast<void**>(tail);
    *reinterpret_cast<void**>(tail) = nullptr;
    span->freeCount -= count;
    if (span->freeCount == 0)
    {
        shards_[shardIdx].partial = span->next;
        span->next = nullptr;
    }
    return count;
}

CentralCache::ReturnAction CentralCache::returnSpanGroupLocked(
    Span* span, void* head, size_t shardIdx, size_t sizeClassIndex)
{
    if (!span || !head || span->sizeClass != sizeClassIndex)
        return ReturnAction::None;
    if (span->centralShard.load(std::memory_order_relaxed) != shardIdx)
        return ReturnAction::Retry;

    size_t count = 0;
    for (void* cur = head; cur; cur = *reinterpret_cast<void**>(cur))
        ++count;

    const bool wasEmpty = (span->freeCount == 0);
    void* tail = head;
    while (*reinterpret_cast<void**>(tail))
        tail = *reinterpret_cast<void**>(tail);

    *reinterpret_cast<void**>(tail) = span->freeList;
    span->freeList = head;
    span->freeCount += count;

    if (span->freeCount == span->blockCount)
    {
        unlinkPartial(shardIdx, span);
        return ReturnAction::FullSpan;
    }
    if (wasEmpty)
    {
        span->next = shards_[shardIdx].partial;
        shards_[shardIdx].partial = span;
    }
    return ReturnAction::None;
}

void CentralCache::returnGroups(size_t index, size_t preferredShardId,
                                const std::vector<SpanFreeGroup>& groups)
{
    if (groups.empty())
        return;

    // Stack-local: must not use thread_local vectors here — ThreadCache TLS
    // destruction can run after other TLS vectors and then call flush/return.
    std::vector<Span*> fullSpans;
    fullSpans.reserve(groups.size());
    const size_t preferredShard = shardIndex(index, preferredShardId);

    for (const auto& entry : groups)
    {
        Span* span = entry.span;
        void* head = entry.head;
        if (!span || !head)
            continue;

        for (;;)
        {
            size_t home = span->centralShard.load(std::memory_order_acquire);
            if (home == static_cast<size_t>(-1))
                home = preferredShard;

            ReturnAction action = ReturnAction::None;
            {
                SpinLock guard(shards_[home].lock);
                // Span must already have an immutable home before any block was
                // handed out. If not yet published, attach under preferred shard.
                size_t current = span->centralShard.load(std::memory_order_relaxed);
                if (current == static_cast<size_t>(-1))
                {
                    span->centralShard.store(home, std::memory_order_relaxed);
                    current = home;
                }
                if (current != home)
                    continue;
                action = returnSpanGroupLocked(span, head, home, index);
            }
            if (action == ReturnAction::Retry)
                continue;
            if (action == ReturnAction::FullSpan)
                fullSpans.push_back(span);
            break;
        }
    }

    for (Span* span : fullSpans)
        PageCache::getInstance().deallocateSpan(span);
}

void CentralCache::collectGroups(void* list, size_t index, std::vector<SpanFreeGroup>& groups)
{
    groups.clear();
    PageCache& cache = PageCache::getInstance();
    Span* lastSpan = nullptr;
    size_t lastIdx = static_cast<size_t>(-1);
    uintptr_t lastPage = ~uintptr_t(0);
    const size_t shift = systemPageShift();

    void* cur = list;
    while (cur)
    {
        void* next = *reinterpret_cast<void**>(cur);
        const uintptr_t page = reinterpret_cast<uintptr_t>(cur) >> shift;
        Span* span = (page == lastPage) ? lastSpan : cache.findSpan(cur);
        lastPage = page;
        if (span && span->sizeClass == index)
        {
            size_t idx = static_cast<size_t>(-1);
            if (span == lastSpan)
            {
                idx = lastIdx;
            }
            else
            {
                for (size_t i = 0; i < groups.size(); ++i)
                {
                    if (groups[i].span == span)
                    {
                        idx = i;
                        break;
                    }
                }
                if (idx == static_cast<size_t>(-1))
                {
                    groups.push_back(SpanFreeGroup{span, nullptr});
                    idx = groups.size() - 1;
                }
                lastSpan = span;
                lastIdx = idx;
            }
            *reinterpret_cast<void**>(cur) = groups[idx].head;
            groups[idx].head = cur;
        }
        else
        {
            lastSpan = span;
            lastIdx = static_cast<size_t>(-1);
        }
        cur = next;
    }
}

void CentralCache::mergeRemoteFrees(size_t index, size_t shardId)
{
    remotePending_[index].store(0, std::memory_order_relaxed);
    std::vector<SpanFreeGroup> groups;
    groups.reserve(16);

    for (size_t remoteShard = 0; remoteShard < kCentralShardCount; ++remoteShard)
    {
        void* remoteHead = remoteFreeLists_[index * kCentralShardCount + remoteShard]
                               .head.exchange(nullptr, std::memory_order_acq_rel);
        if (!remoteHead)
            continue;

        PageCache& cache = PageCache::getInstance();
        Span* lastSpan = nullptr;
        size_t lastIdx = static_cast<size_t>(-1);
        void* cur = remoteHead;
        while (cur)
        {
            void* next = *reinterpret_cast<void**>(cur);
            Span* span = cache.findSpan(cur);
            if (span && span->sizeClass == index)
            {
                size_t idx = static_cast<size_t>(-1);
                if (span == lastSpan)
                {
                    idx = lastIdx;
                }
                else
                {
                    for (size_t i = 0; i < groups.size(); ++i)
                    {
                        if (groups[i].span == span)
                        {
                            idx = i;
                            break;
                        }
                    }
                    if (idx == static_cast<size_t>(-1))
                    {
                        groups.push_back(SpanFreeGroup{span, nullptr});
                        idx = groups.size() - 1;
                    }
                    lastSpan = span;
                    lastIdx = idx;
                }
                *reinterpret_cast<void**>(cur) = groups[idx].head;
                groups[idx].head = cur;
            }
            cur = next;
        }
    }

    returnGroups(index, shardId, groups);
}

size_t CentralCache::transferCap(size_t index) const
{
    const size_t bytes = SizeClass::getSize(index);
    if (bytes <= 256)
        return 256;
    if (bytes <= 4096)
        return 64;
    return 16;
}

size_t CentralCache::popTransfer(size_t index, size_t limit, void*& head, void*& tail)
{
    head = nullptr;
    tail = nullptr;
    if (limit == 0)
        return 0;

    auto& bin = transfer_[index];
    if (!bin.head.load(std::memory_order_relaxed))
        return 0;

    void* list = bin.head.exchange(nullptr, std::memory_order_acq_rel);
    if (!list)
        return 0;

    head = list;
    tail = list;
    size_t got = 1;
    while (got < limit)
    {
        void* next = *reinterpret_cast<void**>(tail);
        if (!next)
            break;
        tail = next;
        ++got;
    }

    void* rest = *reinterpret_cast<void**>(tail);
    *reinterpret_cast<void**>(tail) = nullptr;

    if (rest)
    {
        void* restTail = rest;
        while (void* n = *reinterpret_cast<void**>(restTail))
            restTail = n;
        void* old = bin.head.load(std::memory_order_relaxed);
        do
        {
            *reinterpret_cast<void**>(restTail) = old;
        } while (!bin.head.compare_exchange_weak(
            old, rest, std::memory_order_release, std::memory_order_relaxed));
    }

    // Approximate occupancy: never store 0 (that races with a concurrent push
    // that already fetch_add'd) and never wrap unsigned count.
    size_t oldCount = bin.count.load(std::memory_order_relaxed);
    while (true)
    {
        const size_t next = oldCount >= got ? oldCount - got : 0;
        if (bin.count.compare_exchange_weak(
                oldCount, next, std::memory_order_relaxed, std::memory_order_relaxed))
            break;
    }
    return got;
}

void CentralCache::pushTransfer(void* head, size_t count, size_t index)
{
    if (!head || count == 0 || index >= FREE_LIST_SIZE)
        return;

    const size_t cap = transferCap(index);
    auto& bin = transfer_[index];
    const size_t used = bin.count.load(std::memory_order_relaxed);
    const size_t room = used < cap ? cap - used : 0;
    if (room == 0)
    {
        returnRange(head, count, index);
        return;
    }

    void* transferHead = head;
    size_t transferCount = count;
    void* overflowHead = nullptr;
    size_t overflowCount = 0;
    if (count > room)
    {
        void* split = head;
        for (size_t i = 1; i < room; ++i)
            split = *reinterpret_cast<void**>(split);
        overflowHead = *reinterpret_cast<void**>(split);
        *reinterpret_cast<void**>(split) = nullptr;
        transferCount = room;
        overflowCount = count - room;
    }

    void* tail = transferHead;
    while (void* next = *reinterpret_cast<void**>(tail))
        tail = next;

    void* old = bin.head.load(std::memory_order_relaxed);
    do
    {
        *reinterpret_cast<void**>(tail) = old;
    } while (!bin.head.compare_exchange_weak(
        old, transferHead, std::memory_order_release, std::memory_order_relaxed));
    bin.count.fetch_add(transferCount, std::memory_order_relaxed);

    if (overflowHead)
        returnRange(overflowHead, overflowCount, index);
}

void CentralCache::pushRemoteFree(void* ptr, size_t index)
{
    if (!ptr || index >= FREE_LIST_SIZE)
        return;

    const size_t shard = localShardId();
    auto& remote = remoteFreeLists_[index * kCentralShardCount + shard].head;
    void* head = remote.load(std::memory_order_relaxed);
    do
    {
        *reinterpret_cast<void**>(ptr) = head;
    } while (!remote.compare_exchange_weak(
        head, ptr, std::memory_order_release, std::memory_order_relaxed));

    recordRemoteFree(1);
    remotePending_[index].store(1, std::memory_order_release);
}

void CentralCache::pushRemoteFreeRange(void* head, size_t count, size_t index)
{
    if (!head || count == 0 || index >= FREE_LIST_SIZE)
        return;

    void* tail = head;
    while (void* next = *reinterpret_cast<void**>(tail))
        tail = next;

    const size_t shard = localShardId();
    auto& remote = remoteFreeLists_[index * kCentralShardCount + shard].head;
    void* old = remote.load(std::memory_order_relaxed);
    do
    {
        *reinterpret_cast<void**>(tail) = old;
    } while (!remote.compare_exchange_weak(
        old, head, std::memory_order_release, std::memory_order_relaxed));

    recordRemoteFree(count);
    remotePending_[index].store(1, std::memory_order_release);
}

void* CentralCache::fetchRange(size_t index, size_t batchNum, size_t& fetchedCount)
{
    fetchedCount = 0;
    if (index >= FREE_LIST_SIZE || batchNum == 0)
        return nullptr;

    const size_t shardId = localShardId();
    const size_t primaryShard = shardIndex(index, shardId);

    void* head = nullptr;
    void* tail = nullptr;
    size_t got = 0;
    bool merged = false;

    while (got < batchNum)
    {
        void* rangeHead = nullptr;
        void* rangeTail = nullptr;
        size_t rangeCount = 0;
        {
            SpinLock guard(shards_[primaryShard].lock);
            rangeCount = popRangeFromShardLocked(primaryShard, batchNum - got,
                                                 rangeHead, rangeTail);
        }

        if (!rangeCount)
            rangeCount = popTransfer(index, batchNum - got, rangeHead, rangeTail);

        if (!rangeCount)
        {
            for (size_t i = 1; i < kCentralShardCount && !rangeCount; ++i)
            {
                const size_t otherShard = shardIndex(index, (shardId + i) % kCentralShardCount);
                SpinLock guard(shards_[otherShard].lock);
                rangeCount = popRangeFromShardLocked(otherShard, batchNum - got,
                                                     rangeHead, rangeTail);
            }
        }

        if (!rangeCount && !merged)
        {
            if (remotePending_[index].load(std::memory_order_relaxed))
                mergeRemoteFrees(index, shardId);
            merged = true;
            continue;
        }

        if (rangeCount)
        {
            if (!head)
                head = rangeHead;
            else
                *reinterpret_cast<void**>(tail) = rangeHead;
            tail = rangeTail;
            got += rangeCount;
            continue;
        }

        Span* span = createSpan(index);
        if (!span)
            break;

        {
            SpinLock guard(shards_[primaryShard].lock);
            span->centralShard.store(primaryShard, std::memory_order_relaxed);
            span->next = shards_[primaryShard].partial;
            shards_[primaryShard].partial = span;

            rangeHead = nullptr;
            rangeTail = nullptr;
            rangeCount = popRangeFromShardLocked(primaryShard, batchNum - got,
                                                 rangeHead, rangeTail);
        }

        if (rangeCount)
        {
            if (!head)
                head = rangeHead;
            else
                *reinterpret_cast<void**>(tail) = rangeHead;
            tail = rangeTail;
            got += rangeCount;
        }
    }

    fetchedCount = got;
    return head;
}

void CentralCache::returnRange(void* start, size_t blockCount, size_t index)
{
    if (!start || index >= FREE_LIST_SIZE)
        return;

    (void)blockCount;
    std::vector<SpanFreeGroup> groups;
    collectGroups(start, index, groups);
    returnGroups(index, localShardId(), groups);
}

} // namespace Kama_memoryPool
