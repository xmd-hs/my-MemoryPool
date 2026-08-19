#pragma once
#include "Common.h"
#include "Platform.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Kama_memoryPool
{

struct Span
{
    void*  pageAddr = nullptr;
    size_t numPages = 0;
    void*  originAddr = nullptr;
    size_t originPages = 0;

    size_t blockSize = 0;
    size_t blockCount = 0;
    size_t freeCount = 0;
    size_t sizeClass = 0;
    void*  freeList = nullptr;

    Span* next = nullptr;
    bool  inFreeList = false;
    bool  isLarge = false;

    std::vector<std::uint8_t> debugAllocMap;
    bool debugLargeAllocated = false;
    std::mutex debugMutex;
};

class PageCache
{
public:
    static PageCache& getInstance()
    {
        static PageCache instance;
        return instance;
    }

    static size_t pageSize() { return systemPageSize(); }

    Span* allocateSpan(size_t numPages);
    void  deallocateSpan(Span* span);
    Span* findSpan(void* ptr);
    size_t reservedBytes();
    size_t cachedPageBytes();

private:
    PageCache() = default;
    ~PageCache();

    PageCache(const PageCache&) = delete;
    PageCache& operator=(const PageCache&) = delete;

    void* systemAlloc(size_t& numPages);
    bool  removeFromFreeList(Span* span);
    void  registerPages(Span* span);
    void  unregisterPages(Span* span);
    void  redirectPages(Span* from, Span* to);
    void  releaseOrigin(Span* span);
    Span* findSpanLocked(void* ptr) const;
    uintptr_t pageId(void* ptr) const;

    std::map<size_t, Span*> freeSpans_;
    std::unordered_map<uintptr_t, Span*> pageMap_;
    std::vector<std::pair<void*, size_t>> systemAllocs_;
    size_t cachedFreePages_ = 0;
    mutable std::shared_mutex pageMapMutex_;
    std::mutex mutex_;
};

} // namespace Kama_memoryPool
