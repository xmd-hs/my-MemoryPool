// PageCache 第三层头文件（第三版）
// 全局一份，和操作系统打交道，管的是一段段连续的页（每个 Span 就是连续几页）
//
// 三种大小（约定，代码里没有单独的「中对象」类型）：
//   小对象  ≤256B              ：中央切块，页号进页表，页尾写标签
//   中对象  256B < size < 512KB：同样切块、进页表，但页尾不写标签
//   大对象  ≥512KB             ：不切块，进 largeSpans_，不进页表
// 小对象/中对象：allocateSpan 给中央去切块
// 大对象：allocateLargeSpan
// 归还：同一 origin 的相邻空闲 Span 合并；完整空闲 origin 可按预算归还系统
//
// 相较于第二版的优化：
//   1. 页大小改问系统，不再写死 4KB
//   2. allocateSpan 返回 Span*，不再只给 void*；deallocateSpan 收 Span*
//   3. 新增页尾标签（每页 16 字节）：不带大小释放先读档位，不必先抢页锁
//   4. 按起始地址的表改成三级页表：任意页内指针都能无锁查到这段 Span
//   5. 叶子槽打包 Span* + 档位，不带大小释放也可以走页表一次拿到档位
//   6. Span 记下当初向系统要的起点 originAddr 和页数 originPages，合并只允许同一块内部相邻
//   7. 空闲页预算 32MiB，完整空闲 origin 可归还系统
//   8. 大对象单独一张起始地址表，和页表分开，避免小对象/中对象不带大小释放去抢大对象那把锁
//   9. Span 带切块字段 / centralShard / 调试位图，给中央和护栏用

#pragma once                             // 头文件只包含一次
#include "Common.h"                      // 包含公共定义
#include "Platform.h"                    // 包含系统页分配接口

#include <atomic>                        // 包含原子类型
#include <cstdint>                       // 包含定宽整数
#include <map>                           // 包含按页数分桶的 map
#include <mutex>                         // 包含互斥锁
#include <unordered_map>                 // 包含大对象表
#include <utility>                       // 包含 pair
#include <vector>                        // 包含 vector

namespace Kama_memoryPool                // 进入 Kama_memoryPool 命名空间
{                                        // 命名空间开始

// Span：连续几页捆在一起。中央在上面切小块；大对象就把这几页整段给用户
struct Span                              // 定义 Span
{                                        // 结构体开始
    void*  pageAddr = nullptr;           // 这段虚存的起始地址（页对齐）
    size_t numPages = 0;                 // 这段现在占多少页（合并 / 拆分会变）
    void*  originAddr = nullptr;         // 当初向系统要这一大段时的起始地址
    size_t originPages = 0;              // 当初向系统要这一大段时的页数

    size_t blockSize = 0;                // 中央切块后的块大小
    size_t blockCount = 0;               // 一共切了多少块
    size_t freeCount = 0;                // 现在空闲多少块
    size_t sizeClass = 0;                // 属于哪一档
    size_t blocksPerPage = 0;            // 非 0 表示每页末尾有页尾标签，值是每页几块
    void*  freeList = nullptr;           // 这段上还没发出去的块链表头

    Span* next = nullptr;                // 空闲桶里下一段，或中央分片上的下一段
    bool  inFreeList = false;            // 是否挂在 PageCache 的 freeSpans_ 上
    bool  isLarge = false;               // 是否是大对象整段    
    std::atomic<size_t> centralShard{static_cast<size_t>(-1)}; // 中央归属片，-1 表示还没发布

    std::vector<std::uint8_t> debugAllocMap; // 调试：每块是否已分配
    bool debugLargeAllocated = false;    // 调试：大对象是否处于已分配
    std::mutex debugMutex;               // 调试位图用的锁
};                                       // 结构体结束

// 页尾标签：不带大小释放时先读这个，不用查页表
constexpr uint64_t kPageTagMagic = 0x4B614D61506F6F6CULL; // 校验底数（字母拼成 KaMaPool）

struct PageTag                           // 定义页尾标签
{                                        // 结构体开始
    uint64_t magic;                      // 校验值：必须等于按本页起始现算的 pageTagCookie
    uint32_t sizeClass;                  // 这一页当前是哪一档（下标，不是字节数）
    uint32_t _pad;                       // 凑满 16 字节
};                                       // 结构体结束

constexpr size_t kPageTagBytes = sizeof(PageTag); // 标签大小，16 字节

// 算出这一页的 magic 应该是多少
// 切块时写进 tag->magic；释放时再算一遍，必须和页尾里存的一样
// 混进页起始地址，是为了让每一页的数都不同，抄一个固定数字过不了
inline uint64_t pageTagCookie(uintptr_t pageBase) // 入参：这一页的起始地址
{                                        // 函数开始
    return kPageTagMagic ^ pageBase ^ (pageBase >> 17); // 底数、页起始、页起始右移 17 位，三者按位异或
}                                        // 函数结束

// 只算「标签放在哪」，不检查有没有效
// 第一个参数是页里任意字节都行；第二个是一页多长（一般 4096）
// 例如指针=0x1A30、页=4KB → 页头 0x1000 → 标签在 0x1FF0（页末 16 字节）
inline PageTag* pageTagOf(void* ptr, size_t pageSize) // 入参：指针、页大小；返回：指向页尾那 16 字节
{                                        // 函数开始
    const uintptr_t mask = static_cast<uintptr_t>(pageSize) - 1; // 4KB 页时是 0xFFF，用来抠出「页内偏移」
    const uintptr_t page = reinterpret_cast<uintptr_t>(ptr) & ~mask; // 清掉页内偏移，得到这一页开头
    return reinterpret_cast<PageTag*>(page + pageSize - kPageTagBytes); // 页开头 + 一页长度 - 16 = 页尾标签
}                                        // 函数结束

// 给「不带大小的释放」用：这页尾巴上是不是我们写过的真标签
// 先找到页尾，再核对 magic、sizeClass
// 通过才返回标签（外面读 sizeClass）；失败返回空，调用方再去查页表
inline const PageTag* pageTagIfValid(const void* ptr, size_t pageSize) // 入参同「找页尾」；有效返回标签，否则空
{                                        // 函数开始
    const auto* tag = pageTagOf(const_cast<void*>(ptr), pageSize); // 先定位到页尾那 16 字节
    const uintptr_t mask = static_cast<uintptr_t>(pageSize) - 1; // 同样的页内偏移掩码
    const uintptr_t page = reinterpret_cast<uintptr_t>(ptr) & ~mask; // 再算出页开头，用来重算 magic
    if (tag->magic != pageTagCookie(page)) // 页尾里存的 magic，和「按这一页起始现算的」不一致
        return nullptr;                  // 当这页没有标签（用户数据 / 垃圾 / 已清掉的旧标签）
    if (tag->sizeClass >= FREE_LIST_SIZE) // sizeClass 超出表长度，同样不信
        return nullptr;                  // 当这页没有标签
    return tag;                          // 真标签，外面可以用 tag->sizeClass
}                                        // 函数结束

// 三级页表（类似 tcmalloc）：页号 = 地址 / 页大小
// 覆盖约 36 位页号。叶子槽：低 48 位 Span*，48~55 位档位+1（0 表示没打包档位）
class PageMap                            // 定义页表
{                                        // 类开始
public:                                  // 公有接口开始
    static constexpr size_t kBits = 12;  // 每一级用 12 位
    static constexpr size_t kSize = size_t(1) << kBits; // 每一级 4096 个槽
    static constexpr uintptr_t kSpanMask = (uintptr_t(1) << 48) - 1; // 取出 Span* 的掩码
    static constexpr unsigned kSizeClassShift = 48; // 档位从第 48 位开始放

    PageMap() = default;                 // 默认构造
    ~PageMap();                          // 析构：释放所有中间节点和叶子

    PageMap(const PageMap&) = delete;    // 禁止拷贝构造
    PageMap& operator=(const PageMap&) = delete; // 禁止拷贝赋值

    void set(uintptr_t pageId, Span* span); // 登记：这个页号属于这段 span
    void clear(uintptr_t pageId);        // 清掉这个页号的登记
    Span* get(uintptr_t pageId) const;   // 查出这个页号对应的 span
    size_t getSizeClass(uintptr_t pageId) const; // 查出档位；未映射返回 FREE_LIST_SIZE
    Span* getCached(uintptr_t pageId, void*& cachedLeaf, uintptr_t& cachedLeafId) const; // 带线程本地叶子缓存查 Span
    size_t getSizeClassCached(uintptr_t pageId, void*& cachedLeaf, uintptr_t& cachedLeafId) const; // 带缓存查档位

private:                                 // 私有成员开始
    struct Leaf                          // 叶子：4096 个打包槽
    {                                    // 结构体开始
        std::atomic<uintptr_t> slots[kSize]; // 每个槽是 Span* 和档位的打包值
        Leaf()                           // 叶子构造：槽全清零
        {                                // 函数开始
            for (auto& s : slots)        // 遍历每个槽
                s.store(0, std::memory_order_relaxed); // 写成未映射
        }                                // 函数结束
    };                                   // 结构体结束

    struct Node                          // 中间层：4096 个子指针
    {                                    // 结构体开始
        std::atomic<void*> slots[kSize]; // 指向下一层 Node 或 Leaf
        Node()                           // 中间节点构造：指针全空
        {                                // 函数开始
            for (auto& s : slots)        // 遍历每个槽
                s.store(nullptr, std::memory_order_relaxed); // 写成空
        }                                // 函数结束
    };                                   // 结构体结束

    // 打包：指针放低 48 位；小对象且档位+1 < 256 时，高位再放档位+1（0 留给「没档」）
    static uintptr_t pack(Span* span)    // 把 span 打成一个整数
    {                                    // 函数开始
        auto bits = reinterpret_cast<uintptr_t>(span); // 先放指针
        if (span && !span->isLarge && span->sizeClass + 1 < 256) // 如果是可打包的小对象
            bits |= (static_cast<uintptr_t>(span->sizeClass + 1) << kSizeClassShift); // 高位写上档位+1
        return bits;                     // 返回打包值
    }                                    // 函数结束

    static Span* unpackSpan(uintptr_t bits) // 从打包值取出 Span*
    {                                    // 函数开始
        return reinterpret_cast<Span*>(bits & kSpanMask); // 只留低 48 位当指针
    }                                    // 函数结束

    static size_t unpackSizeClass(uintptr_t bits) // 从打包值取出档位
    {                                    // 函数开始
        const size_t tagged = static_cast<size_t>(bits >> kSizeClassShift); // 取出高位
        return tagged == 0 ? FREE_LIST_SIZE : tagged - 1; // 0 表示没打包，否则减 1 还原档位
    }                                    // 函数结束

    Node* getOrCreateNode(std::atomic<void*>& slot); // 槽里没有中间节点就新建
    Leaf* getOrCreateLeaf(std::atomic<void*>& slot); // 槽里没有叶子就新建
    uintptr_t loadPacked(uintptr_t pageId) const; // 按页号读出打包值
    uintptr_t loadPackedCached(uintptr_t pageId, void*& cachedLeaf, uintptr_t& cachedLeafId) const; // 带叶子缓存读打包值

    mutable std::mutex growMutex_;       // 只有扩表（新建节点）时才加锁
    std::atomic<Node*> root_[kSize]{};   // 第一级：4096 个中间节点指针
};                                       // 类结束

class PageCache                          // 定义页缓存
{                                        // 类开始
public:                                  // 公有接口开始
    static PageCache& getInstance()      // 获取全局唯一页缓存
    {                                    // 函数开始
        static PageCache instance;       // 第一次调用时构造
        return instance;                 // 返回这份引用
    }                                    // 函数结束

    static size_t pageSize() { return systemPageSize(); } // 系统页大小

    Span* allocateSpan(size_t numPages); // 给中央：要至少这么多页，给小对象/中对象切块
    Span* allocateLargeSpan(size_t numPages); // 给大对象：整段页，登记到大对象表
    void  deallocateSpan(Span* span);    // 还一段 Span：能合并就合并，超限还给系统
    Span* findSpan(void* ptr);           // 无锁：小对象/中对象指针 → 所属 Span（走页表）
    Span* findLargeSpan(void* ptr);      // 加锁：必须是大对象起始地址
    size_t findSizeClass(void* ptr) const; // 先页尾标签，再页表；找不到返回档位表长度
    void refreshPageTags(Span* span);    // 档位定了之后重新登记页表（刷新打包的档位）
    size_t reservedBytes();              // 向系统要过、还没还回去的总字节
    size_t cachedPageBytes();            // 现在空闲链表里缓存的页字节
    void setCachedPageLimit(size_t bytes); // 修改空闲页预算，并回收超额的完整 origin
    size_t cachedPageLimit();            // 当前空闲页预算（字节）
    size_t releaseCachedPages();         // 归还所有可回收的完整空闲 origin，返回归还字节数

private:                                 // 私有成员开始
    PageCache() = default;               // 默认构造
    ~PageCache();                        // 析构：删 Span，把系统映射还回去

    PageCache(const PageCache&) = delete; // 禁止拷贝构造
    PageCache& operator=(const PageCache&) = delete; // 禁止拷贝赋值

    void* systemAlloc(size_t& numPages); // 向系统申请，页数可能因粒度被抬高
    bool  removeFromFreeList(Span* span); // 从空闲桶对应链表里摘掉这段
    void  registerPages(Span* span);     // 这段的每一页都写入页表
    void  unregisterPages(Span* span);   // 这段的每一页都从页表清掉
    void  redirectPages(Span* from, Span* to); // 合并：被吞掉那段的页改指向活下来的那段
    void  releaseOrigin(Span* span);     // 把当初向系统要的那一整段还回去，并删 Span
    size_t reclaimCachedPagesLocked(size_t targetPages); // 已持 mutex_，回收至目标页数
    Span* findSpanFromPageMap(void* ptr) const; // 不加线程本地叶子缓存地查页表
    uintptr_t pageId(void* ptr) const;   // 地址右移得到页号

    std::map<size_t, Span*> freeSpans_;  // 空闲 Span，键是页数，值是该页数的链表头
    PageMap pageMap_;                    // 小对象/中对象页表
    std::unordered_map<void*, Span*> largeSpans_; // 大对象表：起始地址 → Span*
    std::vector<std::pair<void*, size_t>> systemAllocs_; // 每次系统申请的（地址, 字节），析构要还
    size_t cachedFreePages_ = 0;         // 空闲链表上总共多少页
    size_t cachedPageLimitBytes_ = 32 * 1024 * 1024; // 默认空闲页预算 32MiB
    std::mutex mutex_;                   // 页缓存全局锁（查小对象/中对象页表不走这把锁）
};                                       // 类结束

} // Kama_memoryPool 命名空间结束
