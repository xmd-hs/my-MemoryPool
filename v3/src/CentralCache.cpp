#include "CentralCache.h"

#include <algorithm>
#include <thread>
#include <unordered_map>
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
        remote.store(nullptr, std::memory_order_relaxed);
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
    const size_t blockSize = SizeClass::getSize(index);
    const size_t ps = PageCache::pageSize();
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
    // Home shard is assigned by the caller under that shard's lock.
    span->centralShard.store(static_cast<size_t>(-1), std::memory_order_relaxed);
    span->debugLargeAllocated = false;
    if constexpr (kDebugGuardsEnabled)
        span->debugAllocMap.assign(n, 0);
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

void* CentralCache::popOneFromShardLocked(size_t shardIdx)
{
    Span* span = shards_[shardIdx].partial;
    if (!span || !span->freeList)
        return nullptr;

    void* obj = span->freeList;
    span->freeList = *reinterpret_cast<void**>(obj);
    span->freeCount--;
    *reinterpret_cast<void**>(obj) = nullptr;

    if (span->freeCount == 0)
    {
        shards_[shardIdx].partial = span->next;
        span->next = nullptr;
        // Keep centralShard as immutable home for later returns.
    }
    return obj;
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
                                const std::unordered_map<Span*, void*>& groups)
{
    if (groups.empty())
        return;

    std::vector<Span*> fullSpans;
    fullSpans.reserve(groups.size());
    const size_t preferredShard = shardIndex(index, preferredShardId);

    for (const auto& entry : groups)
    {
        Span* span = entry.first;
        void* head = entry.second;

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

void CentralCache::mergeRemoteFrees(size_t index, size_t shardId)
{
    void* remoteHead = remoteFreeLists_[index].exchange(nullptr, std::memory_order_acq_rel);
    if (!remoteHead)
        return;

    PageCache& cache = PageCache::getInstance();
    std::unordered_map<Span*, void*> groups;
    groups.reserve(16);

    void* cur = remoteHead;
    while (cur)
    {
        void* next = *reinterpret_cast<void**>(cur);
        Span* span = cache.findSpan(cur);
        if (span && span->sizeClass == index)
        {
            *reinterpret_cast<void**>(cur) = groups[span];
            groups[span] = cur;
        }
        cur = next;
    }

    returnGroups(index, shardId, groups);
}

void CentralCache::pushRemoteFree(void* ptr, size_t index)
{
    if (!ptr || index >= FREE_LIST_SIZE)
        return;

    void* head = remoteFreeLists_[index].load(std::memory_order_relaxed);
    do
    {
        *reinterpret_cast<void**>(ptr) = head;
    } while (!remoteFreeLists_[index].compare_exchange_weak(
        head, ptr, std::memory_order_release, std::memory_order_relaxed));

    recordRemoteFree(1);
}

void* CentralCache::fetchRange(size_t index, size_t batchNum, size_t& fetchedCount)
{
    fetchedCount = 0;
    if (index >= FREE_LIST_SIZE || batchNum == 0)
        return nullptr;

    const size_t shardId = localShardId();
    const size_t primaryShard = shardIndex(index, shardId);

    mergeRemoteFrees(index, shardId);

    void* head = nullptr;
    void* tail = nullptr;
    size_t got = 0;

    while (got < batchNum)
    {
        void* obj = nullptr;
        {
            SpinLock guard(shards_[primaryShard].lock);
            obj = popOneFromShardLocked(primaryShard);
        }

        if (!obj)
        {
            for (size_t i = 1; i < kCentralShardCount && !obj; ++i)
            {
                const size_t otherShard = shardIndex(index, (shardId + i) % kCentralShardCount);
                SpinLock guard(shards_[otherShard].lock);
                obj = popOneFromShardLocked(otherShard);
            }
        }

        if (obj)
        {
            if (!head)
                head = obj;
            else
                *reinterpret_cast<void**>(tail) = obj;
            tail = obj;
            ++got;
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
        }
    }

    fetchedCount = got;
    return head;
}

void CentralCache::returnRange(void* start, size_t /*blockCount*/, size_t index)
{
    if (!start || index >= FREE_LIST_SIZE)
        return;

    PageCache& cache = PageCache::getInstance();
    std::unordered_map<Span*, void*> groups;
    groups.reserve(16);

    void* cur = start;
    while (cur)
    {
        void* next = *reinterpret_cast<void**>(cur);
        Span* span = cache.findSpan(cur);
        if (span && span->sizeClass == index)
        {
            *reinterpret_cast<void**>(cur) = groups[span];
            groups[span] = cur;
        }
        cur = next;
    }

    returnGroups(index, localShardId(), groups);
}

} // namespace Kama_memoryPool
