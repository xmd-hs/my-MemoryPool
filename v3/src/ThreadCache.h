// ThreadCache 第一层头文件（第三版）
// 每个线程一份（thread_local），热路径不加锁
// 小对象（≤256B）和中对象（256B < size < 512KB）：都按档位挂 freeList_（侵入式块链表，不是 span 链）
// 大对象（≥512KB）：cpp 里的 g_localLargeCache（Span* 链，按页数匹配，合计不超过 8MiB），不进 CentralCache
//
// 相较于第二版的优化：
//   1. 切块上限从 256KB 提到 512KB（中对象也走线程缓存/中央），覆盖 vector 几何扩容常见区间
//   2. 新增不带大小的 deallocate(ptr)：页尾标签（小对象 ≤256B）→ 页表（中对象）→ 大对象表
//   3. 大对象不再 malloc/free，走本线程 LocalLargeCache（8MiB）+ PageCache 整段 Span
//   4. 本线程溢出 / 线程退出改 pushTransfer，切好的块留给别的线程，不再 returnRange 拆回 Span
//   5. 向中央批发时带 batchNum，中央回传 fetchedCount，不必自己扫链数块
//   6. 命中本地链后预取下一块（KAMA_PREFETCH）；大对象路径用 KAMA_UNLIKELY
//   7. 本地水位按档放大（32B 可囤 4096，不再是「32KB/块大小、上限 256」）
//   8. 新增 allocateAligned：抬到块大小能被 alignment 整除的档，再复用 allocate
//   9. 新增 trim()：峰值过后主动把超水位小对象/中对象和大对象本线程缓存交出去

#pragma once                             // 头文件只包含一次
#include "Common.h"                      // 包含公共定义
#include <array>                         // 包含 std::array

namespace Kama_memoryPool                // 进入 Kama_memoryPool 命名空间
{                                        // 命名空间开始

class ThreadCache                        // 定义线程缓存类
{                                        // 类开始
public:                                  // 公有接口开始
    // 本线程单例。thread_local，热路径不加锁
    static ThreadCache* getInstance()    // 获取当前线程的 ThreadCache
    {                                    // 函数开始
        static thread_local ThreadCache instance; // 每个线程一份静态实例
        return &instance;                // 返回这份实例的地址
    }                                    // 函数结束

    // 对外分配入口。大于等于 512KB 走大对象；否则（小对象/中对象）查档后本地摘块，未命中则向中央批发
    void* allocate(size_t size);         // 分配 size 字节

    // 对齐分配。alignment 必须是 2 的幂；不大于 8 走 allocate，否则抬到能被 alignment 整除的档再 allocate
    void* allocateAligned(size_t size, size_t alignment); // 按指定对齐分配

    // 不带大小释放。先查档走 freeSmall（小对象/中对象）；查不到再查页表 / 大对象表
    bool  deallocate(void* ptr);         // 不带大小地释放一块内存

    // 带大小释放。大于等于 512KB 找回大对象 Span；否则按字节数换档后走 freeSmall
    bool  deallocate(void* ptr, size_t size); // 带大小地释放一块内存

    // 空闲点回收。超水位小对象/中对象推 Transfer，并清空大对象本线程缓存
    void  trim();                        // 主动收缩本线程缓存
    void  releaseUnused();               // 交出本线程全部空闲块与大对象缓存
    static void setLargeCacheLimit(size_t bytes); // 进程范围设置每线程大对象缓存预算
    static size_t largeCacheLimit();     // 查询每线程大对象缓存预算

private:                                 // 私有成员开始
    ThreadCache();                       // 构造函数
    ~ThreadCache();                      // 析构函数

    ThreadCache(const ThreadCache&) = delete; // 禁止拷贝构造
    ThreadCache& operator=(const ThreadCache&) = delete; // 禁止拷贝赋值

    // 本地空了向中央批发。第一块给用户，其余挂 freeList_
    void* fetchFromCentralCache(size_t index); // 从中央缓存批发一批（小对象/中对象）

    // 超水位：留一半，其余 pushTransfer，不拆回 span
    void  returnToCentralCache(size_t index); // 把本档多余空闲块推回中央 Transfer

    // 线程退出：整表小对象/中对象推进 Transfer
    void  flushAll();                    // 清空本线程全部切块缓存

    // 小对象/中对象释放。头插本线程 freeList_[index]，超水位再往外推
    bool  freeSmall(void* ptr, size_t index); // 把头插入当前线程这一档空闲链

    // 一次向中央要多少块。块越小批量越大
    size_t getBatchNum(size_t size) const; // 计算向中央批发时一次要多少块

    // 本档最多囤多少块。超过才推 Transfer
    size_t maxCachedBlocks(size_t size) const; // 计算这一档最多允许缓存多少块

    // 本档块数是否已超过 maxCachedBlocks
    bool   shouldReturnToCentralCache(size_t index) const; // 判断这一档是否已经超过本地水位

    // 每档一条空闲块链表（块里前 8 字节存 next），不是 span
    std::array<void*, FREE_LIST_SIZE> freeList_{}; // 每档一条空闲块链表头
    std::array<size_t, FREE_LIST_SIZE> freeListSize_{}; // 每档当前有多少个空闲块
};                                       // 类结束

} // Kama_memoryPool 命名空间结束
