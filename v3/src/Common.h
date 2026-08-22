#pragma once

#include <kama/MemoryPool.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
#include <intrin.h>
#endif

namespace Kama_memoryPool
{

#ifndef KAMA_MEMORY_POOL_DEBUG_GUARDS
#define KAMA_MEMORY_POOL_DEBUG_GUARDS 0
#endif

#ifndef KAMA_MEMORY_POOL_STATS_ENABLED
#define KAMA_MEMORY_POOL_STATS_ENABLED 0
#endif

constexpr bool kDebugGuardsEnabled = KAMA_MEMORY_POOL_DEBUG_GUARDS != 0;
constexpr bool kStatsEnabled = KAMA_MEMORY_POOL_STATS_ENABLED != 0;

constexpr size_t ALIGNMENT = kAlignment;
constexpr size_t MAX_BYTES = kMaxBytes;
constexpr size_t kMaxSizeClasses = 64;

#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
inline void cpuPause() { _mm_pause(); }
#elif defined(__x86_64__) || defined(__i386__)
inline void cpuPause() { __builtin_ia32_pause(); }
#elif defined(__aarch64__)
inline void cpuPause() { asm volatile("yield"); }
#else
inline void cpuPause() { std::this_thread::yield(); }
#endif

class SpinLock
{
public:
    explicit SpinLock(std::atomic_flag& flag) : flag_(flag)
    {
        int spins = 0;
        while (flag_.test_and_set(std::memory_order_acquire))
        {
            cpuPause();
            // Central critical sections are short; on Linux a premature
            // scheduler yield costs more than a few additional pauses.
            if (++spins >= 1024)
                std::this_thread::yield();
        }
    }

    ~SpinLock()
    {
        flag_.clear(std::memory_order_release);
    }

    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

private:
    std::atomic_flag& flag_;
};

struct SizeClassTable
{
    std::array<size_t, kMaxSizeClasses> sizes{};
    size_t count = 0;
};

constexpr SizeClassTable makeSizeClassTable()
{
    SizeClassTable table{};
    size_t align = ALIGNMENT;
    size_t size = 0;
    while (table.count < kMaxSizeClasses)
    {
        size += align;
        if (size > MAX_BYTES)
            break;
        table.sizes[table.count++] = size;
        if (size >= align * 8)
            align *= 2;
    }
    return table;
}

constexpr SizeClassTable kSizeClassTable = makeSizeClassTable();
constexpr size_t FREE_LIST_SIZE = kSizeClassTable.count;

constexpr std::array<uint8_t, 257> makeSmallIndexMap()
{
    std::array<uint8_t, 257> map{};
    size_t index = 0;
    for (size_t n = 0; n <= 256; ++n)
    {
        size_t bytes = n == 0 ? ALIGNMENT : n;
        while (index + 1 < kSizeClassTable.count &&
               kSizeClassTable.sizes[index] < bytes)
        {
            ++index;
        }
        map[n] = static_cast<uint8_t>(index);
    }
    return map;
}

constexpr std::array<uint8_t, 257> kSmallIndexMap = makeSmallIndexMap();

constexpr size_t kIndexMapEntries = MAX_BYTES / ALIGNMENT + 1;

constexpr std::array<uint8_t, kIndexMapEntries> makeIndexMap()
{
    std::array<uint8_t, kIndexMapEntries> map{};
    size_t index = 0;
    for (size_t unit = 0; unit < kIndexMapEntries; ++unit)
    {
        const size_t bytes = unit == 0 ? ALIGNMENT : unit * ALIGNMENT;
        while (index + 1 < kSizeClassTable.count &&
               kSizeClassTable.sizes[index] < bytes)
            ++index;
        map[unit] = static_cast<uint8_t>(index);
    }
    return map;
}

constexpr std::array<uint8_t, kIndexMapEntries> kIndexMap = makeIndexMap();

static_assert(FREE_LIST_SIZE > 0 && FREE_LIST_SIZE <= kMaxSizeClasses, "invalid size class count");
static_assert(kSizeClassTable.sizes[0] == ALIGNMENT, "first size class must be ALIGNMENT");
static_assert(kSizeClassTable.sizes[FREE_LIST_SIZE - 1] == MAX_BYTES, "last size class must cover MAX_BYTES");

class SizeClass
{
public:
    static constexpr size_t classCount() { return FREE_LIST_SIZE; }

    static constexpr size_t getSize(size_t index)
    {
        return kSizeClassTable.sizes[index];
    }

    static size_t roundUp(size_t bytes)
    {
        if (bytes > MAX_BYTES)
            return (bytes + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
        return getSize(getIndex(bytes));
    }

    static size_t getIndex(size_t bytes)
    {
        if (bytes <= 256)
            return kSmallIndexMap[bytes];
        if (bytes > MAX_BYTES)
            return FREE_LIST_SIZE;
        return kIndexMap[(bytes + ALIGNMENT - 1) / ALIGNMENT];
    }
};

void recordAllocation(size_t size);
void recordFree(bool sized);
void recordCentralRefill(size_t count);
void recordCentralFlush(size_t count);

} // namespace Kama_memoryPool
