// v3 基础设施：size class 表 + 自旋锁 + CPU pause + 统计
//
// v3 相比 v2 的改进：
//   1. kMaxBytes 从 256KB 提到 512KB
//   2. 新增 kIndexMap：0~512KB 所有大小都能 O(1) 查档位（v2 只有 0~256）
//   3. 自旋锁 yield 阈值固定为 1024，减少内核切换
//   4. 新增 KAMA_LIKELY/KAMA_UNLIKELY 分支预测宏
//   5. 新增 KAMA_PREFETCH 预取宏
//   6. 新增 kBlockHeaderSize（预留，目前未使用 per-block header）
//   7. 统计功能可通过宏开关

#pragma once  // 防止重复包含

#include <kama/MemoryPool.h>  // 引入 kAlignment / kMaxBytes

#include <algorithm>   // std::max
#include <atomic>      // std::atomic_flag
#include <cstddef>     // size_t
#include <cstdint>     // uint8_t
#include <thread>      // std::this_thread::yield

#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))  // MSVC + x86/x64
#include <intrin.h>  // _mm_pause / _mm_prefetch
#endif

namespace Kama_memoryPool  // 命名空间
{

// 调试护栏宏：开启后会检查重复分配/重复释放
#ifndef KAMA_MEMORY_POOL_DEBUG_GUARDS
#define KAMA_MEMORY_POOL_DEBUG_GUARDS 0
#endif

// 统计宏：开启后记录各种计数
#ifndef KAMA_MEMORY_POOL_STATS_ENABLED
#define KAMA_MEMORY_POOL_STATS_ENABLED 0
#endif

constexpr bool kDebugGuardsEnabled = KAMA_MEMORY_POOL_DEBUG_GUARDS != 0;  // 调试护栏是否开启
constexpr bool kStatsEnabled = KAMA_MEMORY_POOL_STATS_ENABLED != 0;        // 统计是否开启

constexpr size_t ALIGNMENT = kAlignment;     // 对齐基准 8（从 MemoryPool.h 引入）
constexpr size_t MAX_BYTES = kMaxBytes;      // 切块上限 512KB（≥ 这个走大对象；中间那段叫中对象）
constexpr size_t kMaxSizeClasses = 96;       // size class 表容量

// 分支预测宏：告诉编译器哪个分支更可能，优化指令缓存
#if defined(__GNUC__) || defined(__clang__)
#define KAMA_LIKELY(x) __builtin_expect(!!(x), 1)    // x 很可能为真
#define KAMA_UNLIKELY(x) __builtin_expect(!!(x), 0)  // x 很可能为假
#define KAMA_PREFETCH(p) __builtin_prefetch(static_cast<const void*>(p), 1, 3)  // 读预取，高局部性
#else
#define KAMA_LIKELY(x) (x)
#define KAMA_UNLIKELY(x) (x)
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))  // MSVC + x86
#define KAMA_PREFETCH(p) _mm_prefetch(static_cast<const char*>(static_cast<const void*>(p)), _MM_HINT_T0)
#else
#define KAMA_PREFETCH(p) ((void)0)  // 其他架构不预取
#endif
#endif

// CPU 空转指令：让 CPU 等一下，不浪费流水线
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))  // MSVC + x86/x64
inline void cpuPause() { _mm_pause(); }
#elif defined(__x86_64__) || defined(__i386__)                   // GCC/Clang + x86
inline void cpuPause() { __builtin_ia32_pause(); }
#elif defined(__aarch64__)                                        // ARM64
inline void cpuPause() { asm volatile("yield"); }
#else                                                             // 其他架构
inline void cpuPause() { std::this_thread::yield(); }
#endif

// 统计函数声明（定义在 MemoryPool.cpp）
void recordCentralLockWait(std::uint64_t count);

// 自旋锁：拿不到锁时 PAUSE 空转，转够次数后 yield
class SpinLock
{
public:
    explicit SpinLock(std::atomic_flag& flag) : flag_(flag)  // 绑定外部 flag
    {
        int spins = 0;  // 空转计数
#if defined(__linux__)
        constexpr int kYieldThreshold = 256;
#else
        constexpr int kYieldThreshold = 1024;
#endif
        while (flag_.test_and_set(std::memory_order_acquire))  // 原子尝试加锁
        {
            ++spins;
            cpuPause();  // 拿不到就 PAUSE
            if (spins >= kYieldThreshold)  // 空转够次数还拿不到
                std::this_thread::yield();  // 让出 CPU
        }
        if (spins > 0)  // 有过等待，记录统计
            recordCentralLockWait(1);
    }

    ~SpinLock()
    {
        flag_.clear(std::memory_order_release);  // 析构释放锁
    }

    SpinLock(const SpinLock&) = delete;            // 禁止拷贝
    SpinLock& operator=(const SpinLock&) = delete; // 禁止赋值

private:
    std::atomic_flag& flag_;  // 引用外部的原子标志
};

// size class 表结构
struct SizeClassTable
{
    size_t sizes[kMaxSizeClasses]{};  // 各档位字节数
    size_t count = 0;                              // 实际档位数
};

// 编译期生成 size class 表（几何步进：每 8 档步长翻倍）
constexpr SizeClassTable makeSizeClassTable()
{
    SizeClassTable table{};
    size_t align = ALIGNMENT;   // 当前步长，初始 8
    size_t size = 0;            // 当前档位大小
    while (table.count < kMaxSizeClasses)
    {
        size += align;          // 下一档 = 当前 + 步长
        if (size > MAX_BYTES)   // 超过上限就停
            break;
        table.sizes[table.count++] = size;
        if (size >= align * 8)  // 攒够 8 档，步长翻倍
            align *= 2;
    }
    return table;
}

constexpr SizeClassTable kSizeClassTable = makeSizeClassTable();  // 编译期算好
constexpr size_t FREE_LIST_SIZE = kSizeClassTable.count;          // 实际档位数

// 预留的块头大小（目前未使用 per-block header，slotSize == getSize）
constexpr size_t kBlockHeaderSize = 8;

// 编译期生成小对象查表：bytes 0~256 直接得到档位下标
struct SmallIndexMap
{
    uint8_t values[257]{};
    constexpr uint8_t& operator[](size_t i) { return values[i]; }
    constexpr const uint8_t& operator[](size_t i) const { return values[i]; }
};

constexpr SmallIndexMap makeSmallIndexMap()
{
    SmallIndexMap map{};
    size_t index = 0;
    for (size_t n = 0; n <= 256; ++n)
    {
        size_t bytes = n == 0 ? ALIGNMENT : n;
        while (index + 1 < kSizeClassTable.count &&
               kSizeClassTable.sizes[index] < bytes)
            ++index;
        map[n] = static_cast<uint8_t>(index);
    }
    return map;
}

constexpr SmallIndexMap kSmallIndexMap = makeSmallIndexMap();

// v3 新增：大对象也用查表。0~512KB 所有 8 字节对齐的大小都能 O(1) 查档位
constexpr size_t kIndexMapEntries = MAX_BYTES / ALIGNMENT + 1;  // 512KB/8 + 1 = 65537

struct IndexMap
{
    uint8_t values[kIndexMapEntries]{};
    constexpr uint8_t& operator[](size_t i) { return values[i]; }
    constexpr const uint8_t& operator[](size_t i) const { return values[i]; }
};

constexpr IndexMap makeIndexMap()
{
    IndexMap map{};
    size_t index = 0;
    for (size_t unit = 0; unit < kIndexMapEntries; ++unit)
    {
        const size_t bytes = unit == 0 ? ALIGNMENT : unit * ALIGNMENT;  // 0->8, 1->8, 2->16, ...
        while (index + 1 < kSizeClassTable.count &&
               kSizeClassTable.sizes[index] < bytes)
            ++index;
        map[unit] = static_cast<uint8_t>(index);
    }
    return map;
}

constexpr IndexMap kIndexMap = makeIndexMap();

// 编译期校验
static_assert(FREE_LIST_SIZE > 0 && FREE_LIST_SIZE <= kMaxSizeClasses, "invalid size class count");
static_assert(kSizeClassTable.sizes[0] == ALIGNMENT, "first size class must be ALIGNMENT");
static_assert(kSizeClassTable.sizes[FREE_LIST_SIZE - 1] == MAX_BYTES, "last size class must cover MAX_BYTES");

// SizeClass 工具类
class SizeClass
{
public:
    static constexpr size_t classCount() { return FREE_LIST_SIZE; }  // 总档位数

    static constexpr size_t getSize(size_t index)  // 档位 -> 实际大小
    {
        return kSizeClassTable.sizes[index];
    }

    // 物理槽大小 = 可用大小（v3 无 per-block header）
    static constexpr size_t slotSize(size_t index)
    {
        return getSize(index);
    }

    static constexpr bool usesHeader(size_t) { return false; }  // 不使用块头

    static size_t roundUp(size_t bytes)  // 向上取整到最近档位
    {
        if (bytes > MAX_BYTES)
            return (bytes + ALIGNMENT - 1) & ~(ALIGNMENT - 1);  // 大对象 8 字节对齐
        return getSize(getIndex(bytes));
    }

    static size_t getIndex(size_t bytes)  // 大小 -> 档位下标
    {
        if (bytes <= 256)
            return kSmallIndexMap[bytes];  // 小对象查表
        if (bytes > MAX_BYTES)
            return FREE_LIST_SIZE;         // 大对象标记
        return kIndexMap[(bytes + ALIGNMENT - 1) / ALIGNMENT];  // 大对象也查表
    }
};

// 统计函数声明
void recordAllocation(size_t size);
void recordFree(bool sized);
void recordCentralRefill(size_t index, size_t count);
void recordCentralFlush(size_t index, size_t count);
void recordThreadCacheHit(size_t index);
void recordThreadCacheMiss(size_t index);

} // namespace Kama_memoryPool
