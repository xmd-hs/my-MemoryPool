#pragma once
#include "Common.h"
#include "PageCache.h"

#include <array>
#include <atomic>

namespace Kama_memoryPool
{

struct CacheShard
{
    alignas(64) std::atomic_flag lock;
    Span* partial;
};

class CentralCache
{
public:
    static CentralCache& getInstance()
    {
        static CentralCache instance;
        return instance;
    }

    void* fetchRange(size_t index, size_t batchNum, size_t& fetchedCount);
    void returnRange(void* start, size_t blockCount, size_t index);

private:
    CentralCache();
    CentralCache(const CentralCache&) = delete;
    CentralCache& operator=(const CentralCache&) = delete;

    Span* createSpan(size_t index);
    void unlinkPartial(size_t index, Span* span);

    std::array<CacheShard, FREE_LIST_SIZE> shards_{};
};

} // namespace Kama_memoryPool
