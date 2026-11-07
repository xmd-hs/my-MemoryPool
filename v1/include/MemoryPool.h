// v1 内存池：固定槽 + 无锁自由链表（教学版）
//
// 一句话：预先向系统要 4KB 大块，按 8/16/24/.../512 共 64 种大小切成等大块，
//        分配时先从「空闲链表」摘一个，没有再从大块里顺序切；回收就挂回链表。
//
// 三个角色：
//   HashBucket  - 路由层，按 size 选哪个池
//   MemoryPool  - 一个池管一种槽大小，内部有两条路：空闲链表 + 未用完的大块
//   Slot       - 链表节点，next 指针就藏在用户内存的前 8 字节

#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>

namespace Kama_memoryPool
{
// 64 个池，槽大小从 8B 到 512B，每 8B 一档：8, 16, 24, ..., 512
#define MEMORY_POOL_NUM 64
#define SLOT_BASE_SIZE 8   // 最小槽 8 字节（正好放一个指针）
#define MAX_SLOT_SIZE 512  // 超过 512B 不走池，直接用 new/delete


// 链表节点：空闲块的前 8 字节当 next 指针用，分配出去后这 8 字节归用户
// 所以最小槽必须 >= 8B，否则塞不下一个指针
struct Slot
{
    std::atomic<Slot*> next; // atomic 是因为多个线程会并发改它
};

// 一个 MemoryPool 只管一种槽大小（比如专门管 24B 的池）
//
// 内存布局（一个 4KB block 内）：
//   [next(8B)][padding][slot0][slot1]...[slotN]
//   ^         ^        ^
//   block头   body     curSlot_（往后挪，顺序切片）
//
// 两条分配路径：
//   1. 快路径：从 freeList_ 摘一个回收的块（无锁，CAS）
//   2. 慢路径：freeList 空了，从当前 block 的未切部分顺序切一个（加锁）
//              block 也切完了，allocateNewBlock 再要 4KB
class MemoryPool
{
public:
    MemoryPool(size_t BlockSize = 4096);
    ~MemoryPool();

    void init(size_t size);  // 设槽大小，首次分配前必须调

    void* allocate();         // 拿一个槽
    void deallocate(void*);   // 还一个槽

private:
    void allocateNewBlock();                  // 向系统要新 4KB
    size_t padPointer(char* p, size_t align); // 算对齐要补多少字节

    bool pushFreeList(Slot* slot);  // 把块挂回链表头（无锁）
    Slot* popFreeList();            // 从链表头摘一个（无锁）

private:
    int   BlockSize_;    // 一个 block 多大（默认 4KB）
    int   SlotSize_;     // 这个池的槽多大（8/16/.../512）
    Slot* firstBlock_;   // 所有 block 的链表头，析构时顺着它释放
    Slot* curSlot_;      // 当前 block 里下一个没用过的槽（往后挪）
    Slot* lastSlot_;     // 当前 block 的尾哨兵，curSlot_ >= 它说明 block 用完

    // 带版本号的指针：指针 + 一个计数器
    // 作用是防 ABA 问题（下面解释）
    struct TaggedPtr
    {
        Slot*     ptr;  // 真正的指针
        uintptr_t tag;  // 版本号，每次改完 +1
    };
    std::atomic<TaggedPtr> freeList_;  // 空闲链表头

    // 扩块锁：只在「freeList 空 + block 用完」时才加锁
    // 这是 v1 多线程慢的根源：所有线程挤这一把锁
    std::mutex mutexForBlock_;
};

// ABA 问题（为什么需要 tag）：
//   线程1 读到链表头是 A，准备把头换成 A->next（B）
//   这时线程2 把 A 弹出、把 B 弹出、又把 A 压回去
//   线程1 的 CAS 发现头还是 A，就以为没人动过，把头换成 B
//   但 A->next 这时已经不是 B 了，链表坏了
//
// 解法：每次改完 tag +1。线程2 压回 A 时 tag 变了，
//       线程1 的 tag 对不上，CAS 失败重试，不会误判
//
// 「CAS」= Compare And Swap：比较并交换，硬件级的原子操作
//   语义是「如果当前值 == 我读到的旧值，就把它换成新值，返回成功；否则返回失败」
//   用 CAS 写循环就能实现无锁数据结构：读到旧值 -> 算新值 -> CAS 换，失败就重来


// 路由层：按 size 选哪个池
//
// 路由公式：(size + 7) / 8 - 1
//   size=8   -> 0   落 8B 池
//   size=9   -> 1   落 16B 池（向上取整到 8 的倍数）
//   size=100 -> 12  落 104B 池
//
// 超过 512B 直接走 new/delete，不进池
class HashBucket
{
public:
    static void initMemoryPool();  // 初始化 64 个池
    static MemoryPool& getMemoryPool(int index);  // 取第 i 个池

    static void* useMemory(size_t size)
    {
        if (size == 0)
            return nullptr;
        if (size > MAX_SLOT_SIZE)  // > 512B 直接 new
            return operator new(size);

        // (size+7)/8 是向上取整到 8 的倍数，再 -1 得池下标
        return getMemoryPool(((size + 7) / SLOT_BASE_SIZE) - 1).allocate();
    }

    static void freeMemory(void* ptr, size_t size)
    {
        if (!ptr)
            return;
        if (size > MAX_SLOT_SIZE)
        {
            operator delete(ptr);
            return;
        }

        getMemoryPool(((size + 7) / SLOT_BASE_SIZE) - 1).deallocate(ptr);
    }

    // 友元让下面的 newElement/deleteElement 能访问 useMemory/freeMemory
    template<typename T, typename... Args>
    friend T* newElement(Args&&... args);
    template<typename T>
    friend void deleteElement(T* p);
};

// 等价于 new T(args) / delete p，但底层走池
//
// 注意 v1 这里没有异常安全：如果构造函数抛异常，已分配的内存会泄漏
// （v3 才补上 try/catch + 回收）
template<typename T, typename... Args>
T* newElement(Args&&... args)
{
    T* p = nullptr;
    // 1. 按 sizeof(T) 路由到对应池，分配内存
    if ((p = reinterpret_cast<T*>(HashBucket::useMemory(sizeof(T)))) != nullptr)
        // 2. 在这块内存上构造对象（placement new）
        new(p) T(std::forward<Args>(args)...);

    return p;
}

template<typename T>
void deleteElement(T* p)
{
    if (p)
    {
        p->~T();  // 1. 显式析构
        HashBucket::freeMemory(reinterpret_cast<void*>(p), sizeof(T));  // 2. 回收内存
    }
}

} // namespace Kama_memoryPool
