#include "../include/MemoryPool.h"

namespace xmd_memoryPool
{
// ============================================================================
// 构造：只设默认 block 大小，其他字段等 init() 才设
// 注意 freeList_ 用 TaggedPtr{nullptr, 0} 初始化，tag 从 0 开始
// ============================================================================
MemoryPool::MemoryPool(size_t BlockSize)
    : BlockSize_ (BlockSize)
    , SlotSize_ (0)          // init() 才会设真正的槽大小
    , firstBlock_ (nullptr)  // 还没有任何 block
    , curSlot_ (nullptr)
    , lastSlot_ (nullptr)
    , freeList_ (TaggedPtr{nullptr, 0})  // 空闲链表初始为空
{}

// ============================================================================
// 析构：顺着 firstBlock_ 链表把所有 block 都释放
// ============================================================================
MemoryPool::~MemoryPool()
{
    Slot* cur = firstBlock_;
    while (cur)
    {
        // 先记下下一个 block 的地址，因为下面释放后 cur 就不能用了
        Slot* next = cur->next;
        // 用 operator delete 释放，不是 free，因为申请时用的是 operator new
        // 转成 void* 是告诉编译器：这块内存里没对象，别调析构函数，直接释放空间
        operator delete(reinterpret_cast<void*>(cur));
        cur = next;
    }
}

// ============================================================================
// init：设槽大小，把状态清零
// 每个 HashBucket 在 initMemoryPool() 里会给池 i 调 init((i+1)*8)
// 例：池0 调 init(8)，池1 调 init(16)，...
// ============================================================================
void MemoryPool::init(size_t size)
{
    assert(size > 0);
    SlotSize_ = size;        // 槽大小，比如 24
    firstBlock_ = nullptr;   // 还没有 block
    curSlot_ = nullptr;      // 还没有可切的槽
    lastSlot_ = nullptr;     // 还没有尾哨兵
    // freeList_ 清空。relaxed 是因为这时通常还没人并发用（init 阶段）
    freeList_.store(TaggedPtr{nullptr, 0}, std::memory_order_relaxed);
}

// ============================================================================
// allocate：拿一个槽
// 顺序：先从空闲链表摘（无锁，快）-> 没有就从当前 block 切（加锁，慢）
// ============================================================================
void* MemoryPool::allocate()
{
    // 1. 快路径：先试无锁链表
    Slot* slot = popFreeList();
    if (slot != nullptr)
        return slot;  // 命中，直接返回

    // 2. 慢路径：链表空了，要从 block 里切
    Slot* temp;
    {
        // 加锁：切 block 时不能并发，否则两个线程都申请一块就浪费
        std::lock_guard<std::mutex> lock(mutexForBlock_);

        // 检查当前 block 还有没有空间
        // curSlot_ 是「下一个可切的位置」，lastSlot_ 是「最后能切的位置」
        // curSlot_ >= lastSlot_ 说明这 block 切完了
        if (curSlot_ >= lastSlot_)
        {
            // 当前 block 没空间，向系统要新 4KB
            allocateNewBlock();
        }

        // 切一个槽：返回当前 curSlot_，然后 curSlot_ 往后挪 SlotSize_ 字节
        temp = curSlot_;
        // 不能写 curSlot_ += 1，因为 curSlot_ 是 Slot* 类型，+1 实际跳 sizeof(Slot) 字节
        // 我们要跳的是 SlotSize_ 字节，所以要换算成「几个 Slot 宽度」
        // 例：SlotSize_=24, sizeof(Slot)=8 -> 24/8=3 -> curSlot_ += 3（跳 24 字节）
        curSlot_ += SlotSize_ / sizeof(Slot);
    }

    return temp;
}

// ============================================================================
// deallocate：还一个槽，直接挂回链表头（无锁）
// ============================================================================
void MemoryPool::deallocate(void* ptr)
{
    if (!ptr) return;

    // 把 void* 当成 Slot* 解释，因为前 8 字节就是 next 指针的位置
    Slot* slot = reinterpret_cast<Slot*>(ptr);
    pushFreeList(slot);
}

// ============================================================================
// allocateNewBlock：向系统要新 4KB，切成槽
// 注意：这个函数假设调用方已经持有 mutexForBlock_（在 allocate 里加的锁）
// ============================================================================
void MemoryPool::allocateNewBlock()
{
    // 向系统要 BlockSize_（默认 4096）字节
    void* newBlock = operator new(BlockSize_);

    // 头插法：新 block 指向原来的 firstBlock_，然后 firstBlock_ 指向新 block
    // 头插是为了析构时能从 firstBlock_ 顺着 next 释放所有 block
    reinterpret_cast<Slot*>(newBlock)->next = firstBlock_;
    firstBlock_ = reinterpret_cast<Slot*>(newBlock);

    // body 跳过第一个 Slot（那 8 字节存 next 指针，不能给用户用）
    char* body = reinterpret_cast<char*>(newBlock) + sizeof(Slot*);

    // body 不一定对齐到槽大小，要算补多少字节
    // 例：SlotSize_=24，body 地址 mod 24 = 5，那要补 24-5=19 字节
    size_t paddingSize = padPointer(body, SlotSize_);
    curSlot_ = reinterpret_cast<Slot*>(body + paddingSize);

    // lastSlot_ 是最后能切槽的位置
    // 公式：block 起点 + block 大小 - 槽大小 + 1
    // +1 是因为判断用 >=，所以这里 +1 让边界刚好够切最后一个槽
    lastSlot_ = reinterpret_cast<Slot*>(
        reinterpret_cast<size_t>(newBlock) + BlockSize_ - SlotSize_ + 1);
}

// ============================================================================
// padPointer：算 p 到下一个对齐位置要补多少字节
// ============================================================================
// 例：align=8, p=0x1005，p % 8 = 5，要补 8-5=3 字节到 0x1008
// 但当 p 已经对齐时（p%align==0），公式 align-0 = align，补一整块，不对
// 注意：这个实现当 p 已对齐时会补 align 字节，相当于浪费一个 align
//       教学代码，可以接受
size_t MemoryPool::padPointer(char* p, size_t align)
{
    return align - (reinterpret_cast<size_t>(p) % align);
}

// ============================================================================
// pushFreeList：把 slot 挂到链表头（无锁，CAS 循环）
// ============================================================================
bool MemoryPool::pushFreeList(Slot* slot)
{
    // 读当前链表头（relaxed：第一次读，失败也无所谓，下面 CAS 会重读）
    TaggedPtr oldHead = freeList_.load(std::memory_order_relaxed);
    TaggedPtr newHead;
    do
    {
        // 把要插入的 slot 的 next 指向当前头
        // relaxed：这次写不是直接发布的，下面 CAS 成功才算正式发布
        slot->next.store(oldHead.ptr, std::memory_order_relaxed);

        // 新头就是 slot，tag +1（防 ABA）
        newHead.ptr = slot;
        newHead.tag = oldHead.tag + 1;

        // CAS：如果 freeList_ 还是 oldHead，就换成 newHead
        //   成功用 release：保证上面 slot->next 的写对其他线程可见
        //   失败用 relaxed：只是重读，不需要同步
        // compare_exchange_weak 允许虚假失败，配合 do-while 比 strong 更便宜
    } while (!freeList_.compare_exchange_weak(oldHead, newHead,
             std::memory_order_release, std::memory_order_relaxed));
    return true;
}

// ============================================================================
// popFreeList：从链表头摘一个槽（无锁，CAS 循环）
// ============================================================================
Slot* MemoryPool::popFreeList()
{
    // 读当前链表头（acquire：要看到 push 方写入的 slot->next）
    TaggedPtr oldHead = freeList_.load(std::memory_order_acquire);
    TaggedPtr newHead;
    do
    {
        // 链表空，返回 nullptr
        if (oldHead.ptr == nullptr)
            return nullptr;

        // 新头是当前头的 next
        // relaxed：只是读一个指针，真正的同步靠下面 CAS 的 acquire
        newHead.ptr = oldHead.ptr->next.load(std::memory_order_relaxed);
        newHead.tag = oldHead.tag + 1;  // tag +1 防 ABA
    } while (!freeList_.compare_exchange_weak(oldHead, newHead,
             std::memory_order_acquire, std::memory_order_relaxed));

    // 返回弹出的槽
    return oldHead.ptr;
}


// ============================================================================
// HashBucket 实现
// ============================================================================

// 初始化 64 个池：池 i 的槽大小 = (i+1)*8
// 池0=8B, 池1=16B, ..., 池63=512B
void HashBucket::initMemoryPool()
{
    for (int i = 0; i < MEMORY_POOL_NUM; i++)
    {
        getMemoryPool(i).init((i + 1) * SLOT_BASE_SIZE);
    }
}

// 单例：64 个 MemoryPool 的静态数组，进程级唯一
// static 局部变量在 C++11 起线程安全初始化（Meyers 单例）
MemoryPool& HashBucket::getMemoryPool(int index)
{
    static MemoryPool memoryPool[MEMORY_POOL_NUM];
    return memoryPool[index];
}

} // namespace xmd_memoryPool
