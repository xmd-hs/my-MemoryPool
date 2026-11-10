// 线程本地缓存（ThreadCache）—— 三层架构的第一层，热路径所在
//
// 职责：
//   - 每个线程一个实例（thread_local），分配/回收时无锁，最快
//   - 维护每个 size class 的空闲链表 + 计数
//   - 本地链表空了，向 CentralCache 批量要一批
//   - 本地链表太多，归还一部分给 CentralCache
//
// 为什么要这一层：
//   v1 所有线程共享一组锁，竞争激烈。v2 让每个线程先在本地缓存玩，
//   玩完了才去找中央，把大多数操作变成无锁。

#pragma once
#include "Common.h"

namespace xmd_memoryPool
{

class ThreadCache
{
public:
    // 每个线程一个实例，thread_local 保证无锁
    static ThreadCache* getInstance()
    {
        static thread_local ThreadCache instance;
        return &instance;
    }

    void* allocate(size_t size);
    void deallocate(void* ptr, size_t size);
private:
    ThreadCache();
    ~ThreadCache();

    // 从中心缓存批量获取内存（本地链表空时调用）
    void* fetchFromCentralCache(size_t index);
    // 归还内存到中心缓存（本地链表太多时调用）
    void returnToCentralCache(void* start, size_t size);

    // 判断是否该归还（超过阈值）
    bool shouldReturnToCentralCache(size_t index);
private:
    // 每个 size class 的空闲链表头
    std::array<void*, FREE_LIST_SIZE>  freeList_;
    // 对应链表的块数（用于判断是否归还）
    std::array<size_t, FREE_LIST_SIZE> freeListSize_;
};

} // namespace memoryPool
