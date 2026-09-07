#pragma once
#include "Common.h"
#include "Platform.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
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
    // Non-zero when blocks are packed per page with a trailing PageTag.
    size_t blocksPerPage = 0;
    void*  freeList = nullptr;

    Span* next = nullptr;
    bool  inFreeList = false;
    bool  isLarge = false;
    std::atomic<size_t> centralShard{static_cast<size_t>(-1)};

    std::vector<std::uint8_t> debugAllocMap;
    bool debugLargeAllocated = false;
    std::mutex debugMutex;
};

// Trailing per-page tag so unsized free can recover the size class from the
// same page as the object (glibc-style) instead of walking the radix map.
constexpr uint64_t kPageTagMagic = 0x4B614D61506F6F6CULL; // "KaMaPool"
constexpr size_t kPageTagBytes = 64;

struct PageTag
{
    uint64_t magic;
    uint32_t sizeClass;
    uint32_t _pad;
};

inline uint64_t pageTagCookie(uintptr_t pageBase)
{
    return kPageTagMagic ^ pageBase ^ (pageBase >> 17);
}

inline PageTag* pageTagOf(void* ptr, size_t pageSize)
{
    const uintptr_t mask = static_cast<uintptr_t>(pageSize) - 1;
    const uintptr_t page = reinterpret_cast<uintptr_t>(ptr) & ~mask;
    return reinterpret_cast<PageTag*>(page + pageSize - kPageTagBytes);
}

inline const PageTag* pageTagIfValid(const void* ptr, size_t pageSize)
{
    const auto* tag = pageTagOf(const_cast<void*>(ptr), pageSize);
    const uintptr_t mask = static_cast<uintptr_t>(pageSize) - 1;
    const uintptr_t page = reinterpret_cast<uintptr_t>(ptr) & ~mask;
    if (tag->magic != pageTagCookie(page))
        return nullptr;
    if (tag->sizeClass >= FREE_LIST_SIZE)
        return nullptr;
    return tag;
}

// 3-level radix page map with lock-free reads (tcmalloc-style).
// page_id = addr / pageSize; covers 36 bits of page-id (48-bit VA / 4KiB pages).
// Each leaf slot packs Span* in the low 48 bits and sizeClass in bits 48..55
// so unsized free can recover the size class with a single atomic load.
class PageMap
{
public:
    static constexpr size_t kBits = 12;
    static constexpr size_t kSize = size_t(1) << kBits; // 4096
    static constexpr uintptr_t kSpanMask = (uintptr_t(1) << 48) - 1;
    static constexpr unsigned kSizeClassShift = 48;

    PageMap() = default;
    ~PageMap();

    PageMap(const PageMap&) = delete;
    PageMap& operator=(const PageMap&) = delete;

    void set(uintptr_t pageId, Span* span);
    void clear(uintptr_t pageId);
    Span* get(uintptr_t pageId) const;
    // Returns size class for a mapped small span, or FREE_LIST_SIZE on miss.
    size_t getSizeClass(uintptr_t pageId) const;
    // Same as get/getSizeClass but reuse a TLS-cached leaf (16MiB coverage at 4K).
    Span* getCached(uintptr_t pageId, void*& cachedLeaf, uintptr_t& cachedLeafId) const;
    size_t getSizeClassCached(uintptr_t pageId, void*& cachedLeaf, uintptr_t& cachedLeafId) const;

private:
    struct Leaf
    {
        std::atomic<uintptr_t> slots[kSize];
        Leaf()
        {
            for (auto& s : slots)
                s.store(0, std::memory_order_relaxed);
        }
    };

    struct Node
    {
        std::atomic<void*> slots[kSize]; // Leaf* or Node*
        Node()
        {
            for (auto& s : slots)
                s.store(nullptr, std::memory_order_relaxed);
        }
    };

    static uintptr_t pack(Span* span)
    {
        auto bits = reinterpret_cast<uintptr_t>(span);
        // Store sizeClass+1 so class 0 is distinguishable from "untagged".
        if (span && !span->isLarge && span->sizeClass + 1 < 256)
            bits |= (static_cast<uintptr_t>(span->sizeClass + 1) << kSizeClassShift);
        return bits;
    }

    static Span* unpackSpan(uintptr_t bits)
    {
        return reinterpret_cast<Span*>(bits & kSpanMask);
    }

    static size_t unpackSizeClass(uintptr_t bits)
    {
        const size_t tagged = static_cast<size_t>(bits >> kSizeClassShift);
        return tagged == 0 ? FREE_LIST_SIZE : tagged - 1;
    }

    Node* getOrCreateNode(std::atomic<void*>& slot);
    Leaf* getOrCreateLeaf(std::atomic<void*>& slot);
    uintptr_t loadPacked(uintptr_t pageId) const;
    uintptr_t loadPackedCached(uintptr_t pageId, void*& cachedLeaf, uintptr_t& cachedLeafId) const;

    mutable std::mutex growMutex_;
    std::atomic<Node*> root_[kSize]{};
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
    Span* allocateLargeSpan(size_t numPages);
    void  deallocateSpan(Span* span);
    Span* findSpan(void* ptr);
    Span* findLargeSpan(void* ptr);
    // Lock-free size-class lookup for small spans (unsized free hot path).
    size_t findSizeClass(void* ptr) const;
    // Re-pack page-map tags after sizeClass is assigned on a small span.
    void refreshPageTags(Span* span);
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
    Span* findSpanFromPageMap(void* ptr) const;
    uintptr_t pageId(void* ptr) const;

    std::map<size_t, Span*> freeSpans_;
    PageMap pageMap_;
    std::unordered_map<void*, Span*> largeSpans_;
    std::vector<std::pair<void*, size_t>> systemAllocs_;
    size_t cachedFreePages_ = 0;
    std::mutex mutex_;
};

} // namespace Kama_memoryPool
