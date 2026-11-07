// MemoryPool 对外门面（v3）
//
// 三层架构：ThreadCache → CentralCache → PageCache
//   - ThreadCache：每个线程独占的缓存，无锁快路径
//   - CentralCache：分片自旋锁，跨线程共享，提供批量块
//   - PageCache：和操作系统打交道，管理 Span（连续几页），合并空闲页
//
// 本头文件只暴露 allocate / deallocate / trim / stats 等接口，
// 具体实现委托给 ThreadCache。

#pragma once  // 防止头文件重复包含

#include "export.h"  // KAMA_API 导出宏

#include <cstddef>    // size_t
#include <cstdint>    // uint64_t
#include <new>        // placement new
#include <utility>    // std::forward
#include <array>      // std::array

namespace Kama_memoryPool  // 命名空间，避免符号冲突
{

constexpr std::size_t kAlignment = 8;  // 对齐基准：所有小对象 8 字节对齐
// size-class 路径覆盖中等大小（如 ~400KiB vector 扩容）。
// 超过 kMaxBytes 走大对象 TLS 回收路径。
constexpr std::size_t kMaxBytes = 512 * 1024;  // 切块上限 512KB（≥ 这个走大对象）

// 统计信息结构（kStatsEnabled=1 时有效）
struct MemoryPoolStats
{
    std::uint64_t allocCount{0};          // 总分配次数
    std::uint64_t freeCount{0};           // 总释放次数
    std::uint64_t liveAllocs{0};          // 存活分配数（allocCount - freeCount）
    std::uint64_t smallAllocCount{0};     // 小对象/中对象分配次数（< kMaxBytes）
    std::uint64_t largeAllocCount{0};     // 大对象分配次数（>= kMaxBytes）
    std::uint64_t sizedFreeCount{0};      // 带大小的释放次数（deallocate(ptr, size)）
    std::uint64_t unsizedFreeCount{0};    // 不带大小的释放次数（deallocate(ptr)）
    std::uint64_t centralRefillCount{0};  // 从中央取块总次数
    std::uint64_t centralFlushCount{0};   // 归还中央总次数
    std::uint64_t centralLockWaits{0};    // 中央锁等待次数
    std::uint64_t remoteFreeCount{0};     // 兼容字段；当前未单独统计跨线程释放，始终为 0
    std::uint64_t reservedBytes{0};       // 向系统预留的总字节
    std::uint64_t cachedPageBytes{0};     // PageCache 缓存的空闲页字节
    std::array<std::uint64_t, 96> centralRefillByClass{};  // 各档位中央取块次数
    std::array<std::uint64_t, 96> centralFlushByClass{};   // 各档位归还中央次数
    std::array<std::uint64_t, 96> threadCacheHitByClass{}; // 各档位 TLS 命中次数
    std::array<std::uint64_t, 96> threadCacheMissByClass{};// 各档位 TLS 未命中次数
};

// 对外门面类，全是静态方法
class KAMA_API MemoryPool
{
public:
    static void* allocate(std::size_t size);                          // 分配 size 字节
    static void* allocateAligned(std::size_t size, std::size_t alignment);  // 按指定对齐分配
    static void  deallocate(void* ptr);                                // 释放（不带大小）
    static void  deallocate(void* ptr, std::size_t size);              // 释放（带大小，更快）
    // 空闲时归还当前线程的多余缓存
    static void  trim();
    // 页缓存预算（全进程）与大对象预算（每线程）；默认分别为 64MiB 和 8MiB
    static void setPageCacheLimit(std::size_t bytes);
    static std::size_t pageCacheLimit();
    static void setLargeCacheLimit(std::size_t bytes);
    static std::size_t largeCacheLimit();
    // 交出调用线程的全部空闲块、大对象缓存，排空当前可见 Transfer，并回收完整空闲页段
    // 其他线程的 TLS 缓存与仍有活动分配的 origin 不受影响；并发调用是尽力回收
    static void releaseUnused();
    static MemoryPoolStats stats();                                    // 获取统计信息

    // 分配内存并构造 T 对象（异常安全：构造失败会释放内存）
    template<typename T, typename... Args>
    static T* newElement(Args&&... args)
    {
        const std::size_t align = alignof(T);  // T 的对齐要求
        // 对齐 <= 8 走普通 allocate，否则走对齐分配
        void* mem = (align <= kAlignment)
                        ? allocate(sizeof(T))
                        : allocateAligned(sizeof(T), align);
        if (!mem)
            return nullptr;
        try
        {
            // placement new：在已分配内存上构造 T
            return ::new (mem) T(std::forward<Args>(args)...);
        }
        catch (...)
        {
            deallocate(mem);  // 构造失败，释放内存再抛异常
            throw;
        }
    }

    // 析构 T 并释放内存
    template<typename T>
    static void deleteElement(T* p)
    {
        if (!p)
            return;
        p->~T();               // 显式调用析构
        deallocate(p);         // 释放内存
    }
};

// 自由函数版本，方便使用：newElement<T>(...) / deleteElement<T>(p)
template<typename T, typename... Args>
T* newElement(Args&&... args)
{
    return MemoryPool::newElement<T>(std::forward<Args>(args)...);
}

template<typename T>
void deleteElement(T* p)
{
    MemoryPool::deleteElement(p);
}

} // namespace Kama_memoryPool
