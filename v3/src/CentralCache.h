// CentralCache 第二层头文件（第三版）
// 全局一份，所有线程共享，按 size class × 8 分片降低锁竞争
// 每个分片挂的是 Span 链（partial），块在 span->freeList 上，不是把一块内存切成 8 份
// 空分片只占元数据；线程用 hash(tid)%8 选自己的片，没有再 steal 别的片
// 线程本地溢出 / 线程退出的切好块进 TransferBin
//
// 相较于第二版的优化：
//   1. 每档一把锁改成每档 8 片锁，减少线程撞同一把锁
//   2. 中央不再维护「一档一条混合块链表」，块回到所属 Span 的 freeList
//   3. span 全空立刻还 PageCache，不再延迟扫描 SpanTracker（v2 要攒 48 次或 1 秒）
//   4. 新增 TransferBin：线程退出 / 超水位的切好块留给后来者，先不拆回 span
//   6. span 记 centralShard，归还回到归属分片；分片变了会 Retry
//   7. fetchRange 带 batchNum，用 fetchedCount 告诉实际个数；步骤：本片 → Transfer → steal → createSpan
//   8. createSpan 对 ≤256B 档在页尾写标签，给不带大小的释放用
//   9. 允许从其他分片的头 span 偷块（span 仍挂在原分片）

#pragma once                             // 头文件只包含一次
#include "Common.h"                      // 包含公共定义
#include "PageCache.h"                   // 包含页缓存（Span / 向第三层要页）

#include <array>                         // 包含 std::array
#include <atomic>                        // 包含原子类型
#include <vector>                        // 包含 std::vector

namespace xmd_memoryPool                // 进入 xmd_memoryPool 命名空间
{                                        // 命名空间开始

constexpr size_t kCentralShardCount = 16; // Linux 高并发下进一步分片，降低同档位锁竞争

// 单个分片：一把自旋锁 + 还有空闲块的 span 链表头
struct CacheShard                        // 定义一个中央分片
{                                        // 结构体开始
    alignas(64) std::atomic_flag lock;   // 自旋锁，独占一行缓存，避免假共享
    Span* partial;                       // 本片里还有空闲块的 span 链表
};                                       // 结构体结束

// 归还时按 span 分组：同一段 span 的块收成一串
struct SpanFreeGroup                     // 定义「一段 span + 要还回去的块链」
{                                        // 结构体开始
    Span* span = nullptr;                // 这些块属于哪一段 span
    void* head = nullptr;                // 这一组归还链表的头
};                                       // 结构体结束

// TransferBin：跨线程杂志
// 线程退出 / 超水位把切好的块放这里，其他线程 fetchRange 可以直接拿
struct alignas(64) TransferBin           // 定义 Transfer 箱子
{                                        // 结构体开始
    std::atomic<void*> head{nullptr};    // 块链表头
    std::atomic<size_t> count{0};        // 箱里大概有多少块（近似值）
};                                       // 结构体结束

class CentralCache                       // 定义中央缓存类
{                                        // 类开始
public:                                  // 公有接口开始
    // 进程内单例，所有线程共用这一份
    static CentralCache& getInstance()   // 获取全局 CentralCache
    {                                    // 函数开始
        static CentralCache instance;    // 第一次调用时构造唯一实例
        return instance;                 // 返回这份实例的引用
    }                                    // 函数结束

    // 向这一档要 batchNum 个块，fetchedCount 写出实际拿到的个数
    void* fetchRange(size_t index, size_t batchNum, size_t& fetchedCount); // 批量取块

    // 把一串块按所属 span 分组后还回各分片；span 全空则还 PageCache
    void  returnRange(void* start, size_t blockCount, size_t index); // 归还一批块

    // 线程退出 / 超水位：切好的块推进 TransferBin，满了再 returnRange
    void  pushTransfer(void* head, size_t count, size_t index); // 把一块链表推进 Transfer
    void  drainTransfers();              // 显式回收：把各档 Transfer 块还回所属 Span

private:                                 // 私有成员开始
    CentralCache();                      // 构造函数
    CentralCache(const CentralCache&) = delete; // 禁止拷贝构造
    CentralCache& operator=(const CentralCache&) = delete; // 禁止拷贝赋值

    // 归还一组块到 span 之后要做什么
    enum class ReturnAction              // 定义归还结果
    {                                    // 枚举开始
        None,                            // 还回去了，span 还没满
        FullSpan,                        // span 全空，可以还 PageCache
        Retry                            // 归属分片变了，要换锁重试
    };                                   // 枚举结束

    size_t localShardId() const;         // 本线程偏好的分片号（tid 哈希）
    size_t shardIndex(size_t sizeClassIndex, size_t shardId) const; // 算出 shards_ 下标

    Span* createSpan(size_t index);      // 向 PageCache 要一段页并切成块链表
    void  unlinkPartial(size_t shardIndex, Span* span); // 从该片 partial 链上摘掉这段 span
    // 已持该片锁：从 partial 头 span 的 freeList 摘最多 limit 个块
    size_t popRangeFromShardLocked(size_t shardIdx, size_t limit,
                                   void*& head, void*& tail); // 从分片弹出一段块

    size_t popTransfer(size_t index, size_t limit, void*& head, void*& tail); // 从 TransferBin 拿最多 limit 个块
    size_t transferCap(size_t index) const; // 这一档 TransferBin 最多囤多少块

    // 已持锁：把一串块接到 span->freeList
    ReturnAction returnSpanGroupLocked(Span* span, void* head, size_t shardIndex, size_t sizeClassIndex);

    // 多组块还回各自 span 所在分片；满 span 集中还 PageCache
    void returnGroups(size_t index, size_t preferredShardId,
                      const std::vector<SpanFreeGroup>& groups);

    // 把一条块链表按所属 span 拆成若干组
    void collectGroups(void* list, size_t index, std::vector<SpanFreeGroup>& groups);

    // 分片表：档位数 × 16，下标 = 档位 * 分片数 + 片号
    std::array<CacheShard, FREE_LIST_SIZE * kCentralShardCount> shards_{}; // 所有档的全部分片
    // TransferBin：每档一个，不是每片一个
    std::array<TransferBin, FREE_LIST_SIZE> transfer_{}; // 每档一个 Transfer 箱子
};                                       // 类结束

} // xmd_memoryPool 命名空间结束
