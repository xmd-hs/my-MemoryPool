// ThreadCache 实现：线程本地缓存，分配/回收的快路径
//
// 空闲链表的实现技巧：
//   每个空闲块的前 8 字节存下一个空闲块的地址（侵入式链表，无需额外内存）
//   *reinterpret_cast<void**>(ptr) 就是读/写 ptr 前 8 字节的 next 指针

#include "../include/ThreadCache.h"
#include "../include/CentralCache.h"
#include <algorithm>
#include <cstdlib>

namespace Kama_memoryPool
{

// ===== 构造/析构 =====

ThreadCache::ThreadCache()
{
    freeList_.fill(nullptr);      // 所有档位的链表都为空
    freeListSize_.fill(0);        // 计数清零
}

// 析构：线程退出时把本地剩余的块全部归还中央，否则泄漏
ThreadCache::~ThreadCache()
{
    for (size_t i = 0; i < FREE_LIST_SIZE; ++i)
    {
        if (freeList_[i] && freeListSize_[i] > 0)
        {
            // 把整条链表（共 freeListSize_[i] 块）还给中央
            size_t bytes = freeListSize_[i] * SizeClass::getSize(i);
            CentralCache::getInstance().returnRange(freeList_[i], bytes, i);
            freeList_[i] = nullptr;
            freeListSize_[i] = 0;
        }
    }
}

// ===== allocate：分配 =====

void* ThreadCache::allocate(size_t size)
{
    // 0 大小也给最小块（8B），符合 malloc 语义
    if (size == 0)
    {
        size = ALIGNMENT;
    }

    // 大对象不走池，直接 malloc
    if (size > MAX_BYTES)
    {
        return std::malloc(size);
    }

    size_t index = SizeClass::getIndex(size);  // 算出档位

    // —— 快路径：本地链表有块，直接摘 ——
    if (void* ptr = freeList_[index])
    {
        freeListSize_[index]--;
        // 链表头 = 原头的下一个
        freeList_[index] = *reinterpret_cast<void**>(ptr);
        return ptr;
    }

    // —— 慢路径：本地空了，向中央批量要 ——
    return fetchFromCentralCache(index);
}

// ===== deallocate：回收 =====

void ThreadCache::deallocate(void* ptr, size_t size)
{
    if (size > MAX_BYTES)
    {
        std::free(ptr);  // 大对象对应 malloc
        return;
    }

    size_t index = SizeClass::getIndex(size);

    // 头插法挂到本地链表
    *reinterpret_cast<void**>(ptr) = freeList_[index];
    freeList_[index] = ptr;
    freeListSize_[index]++;

    // 本地块太多，归还一部分给中央
    if (shouldReturnToCentralCache(index))
    {
        returnToCentralCache(freeList_[index], size);
    }
}

// ===== 是否该归还 =====
//
// 阈值 = max(8, 32KB / 单块大小)，上限 256
// 块越小，阈值越大（小对象可以多缓存一些）
// 块越大，阈值越小（大对象占内存，少缓存）
bool ThreadCache::shouldReturnToCentralCache(size_t index)
{
    const size_t blockSize = SizeClass::getSize(index);
    const size_t threshold = std::max(size_t(8), size_t(32 * 1024) / blockSize);
    return freeListSize_[index] > std::min(threshold, size_t(256));
}

// ===== 从中央批量获取 =====

void* ThreadCache::fetchFromCentralCache(size_t index)
{
    // 中央返回一串块（链表），第一个给用户，其余挂本地
    void* start = CentralCache::getInstance().fetchRange(index);
    if (!start) return nullptr;

    void* result = start;  // 第一个返回给调用者
    void* next = *reinterpret_cast<void**>(start);
    freeList_[index] = next;  // 剩余的挂本地链表

    // 数一下剩余多少块，更新计数
    size_t remain = 0;
    for (void* current = next; current != nullptr;
         current = *reinterpret_cast<void**>(current))
    {
        ++remain;
    }
    freeListSize_[index] += remain;

    return result;
}

// ===== 归还一部分给中央 =====
//
// 策略：保留 1/4，归还 3/4
// 这样本地总有一些存量，下次 allocate 不用再去找中央

void ThreadCache::returnToCentralCache(void* start, size_t size)
{
    size_t index = SizeClass::getIndex(size);
    size_t alignedSize = SizeClass::roundUp(size);
    size_t batchNum = freeListSize_[index];
    if (batchNum <= 1) return;  // 只有 1 块，不值得归还

    // 保留 1/4（至少 1 块）
    size_t keepNum = std::max(batchNum / 4, size_t(1));
    size_t returnNum = batchNum - keepNum;

    // 沿链表走 keepNum-1 步，找到「保留部分」的尾巴
    char* current = static_cast<char*>(start);
    char* splitNode = current;
    for (size_t i = 0; i < keepNum - 1; ++i)
    {
        splitNode = reinterpret_cast<char*>(*reinterpret_cast<void**>(splitNode));
        if (splitNode == nullptr)
        {
            // 链表提前结束（理论上不会），修正归还数量
            returnNum = batchNum - (i + 1);
            break;
        }
    }

    if (splitNode != nullptr)
    {
        // 在 splitNode 处断开：前 keepNum 块留本地，后面 returnNum 块还中央
        void* nextNode = *reinterpret_cast<void**>(splitNode);
        *reinterpret_cast<void**>(splitNode) = nullptr;

        freeList_[index] = start;             // 本地链表头不变
        freeListSize_[index] = keepNum;        // 计数更新为保留数

        if (returnNum > 0 && nextNode != nullptr)
        {
            // 把断开的后半段还给中央
            CentralCache::getInstance().returnRange(nextNode, returnNum * alignedSize, index);
        }
    }
}

} // namespace memoryPool
