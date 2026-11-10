#include <xmd/MemoryPool.h>
#include <xmd/Allocator.h>
#include <xmd/xmd_memory_pool.h>
#include "Common.h"
#include "PageCache.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>

using namespace xmd_memoryPool;

#define REQUIRE(cond)                                                                 \
    do                                                                                \
    {                                                                                 \
        if (!(cond))                                                                  \
        {                                                                             \
            std::cerr << "REQUIRE failed: " #cond " (" << __FILE__ << ":" << __LINE__ \
                      << ")" << std::endl;                                            \
            std::abort();                                                             \
        }                                                                             \
    } while (0)

namespace
{

void fillPattern(void* ptr, size_t size, std::uint32_t seed)
{
    auto* bytes = static_cast<unsigned char*>(ptr);
    for (size_t i = 0; i < size; ++i)
        bytes[i] = static_cast<unsigned char>(seed + static_cast<std::uint32_t>(i));
}

bool checkPattern(void* ptr, size_t size, std::uint32_t seed)
{
    const auto* bytes = static_cast<const unsigned char*>(ptr);
    for (size_t i = 0; i < size; ++i)
    {
        if (bytes[i] != static_cast<unsigned char>(seed + static_cast<std::uint32_t>(i)))
            return false;
    }
    return true;
}

} // namespace

// 基础分配测试
void testBasicAllocation() 
{
    std::cout << "Running basic allocation test..." << std::endl;
    
    // 测试小内存分配
    void* ptr1 = MemoryPool::allocate(8);
    REQUIRE(ptr1 != nullptr);
    MemoryPool::deallocate(ptr1, 8);

    // 测试中等大小内存分配
    void* ptr2 = MemoryPool::allocate(1024);
    REQUIRE(ptr2 != nullptr);
    MemoryPool::deallocate(ptr2, 1024);

    // 测试大内存分配（超过MAX_BYTES）
    void* ptr3 = MemoryPool::allocate(1024 * 1024);
    REQUIRE(ptr3 != nullptr);
    MemoryPool::deallocate(ptr3, 1024 * 1024);

    std::cout << "Basic allocation test passed!" << std::endl;
}

// 内存写入测试
void testMemoryWriting() 
{
    std::cout << "Running memory writing test..." << std::endl;

    // 分配并写入数据
    const size_t size = 128;
    char* ptr = static_cast<char*>(MemoryPool::allocate(size));
    REQUIRE(ptr != nullptr);

    // 写入数据
    for (size_t i = 0; i < size; ++i) 
    {
        ptr[i] = static_cast<char>(i % 256);
    }

    // 验证数据
    for (size_t i = 0; i < size; ++i) 
    {
        REQUIRE(ptr[i] == static_cast<char>(i % 256));
    }

    MemoryPool::deallocate(ptr, size);
    std::cout << "Memory writing test passed!" << std::endl;
}

// 多线程测试
void testMultiThreading() 
{
    std::cout << "Running multi-threading test..." << std::endl;

    const int NUM_THREADS = 4;
    const int ALLOCS_PER_THREAD = 1000;
    std::atomic<bool> has_error{false};
    
    auto threadFunc = [&has_error, ALLOCS_PER_THREAD]() 
    {
        try 
        {
            std::vector<std::pair<void*, size_t>> allocations;
            allocations.reserve(ALLOCS_PER_THREAD);
            
            for (int i = 0; i < ALLOCS_PER_THREAD && !has_error; ++i) 
            {
                size_t size = (rand() % 256 + 1) * 8;
                void* ptr = MemoryPool::allocate(size);
                
                if (!ptr) 
                {
                    std::cerr << "Allocation failed for size: " << size << std::endl;
                    has_error = true;
                    break;
                }
                
                allocations.push_back({ptr, size});
                
                if (rand() % 2 && !allocations.empty()) 
                {
                    size_t index = rand() % allocations.size();
                    MemoryPool::deallocate(allocations[index].first, 
                                         allocations[index].second);
                    allocations.erase(allocations.begin() + index);
                }
            }
            
            for (const auto& alloc : allocations) 
            {
                MemoryPool::deallocate(alloc.first, alloc.second);
            }
        }
        catch (const std::exception& e) 
        {
            std::cerr << "Thread exception: " << e.what() << std::endl;
            has_error = true;
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; ++i) 
    {
        threads.emplace_back(threadFunc);
    }

    for (auto& thread : threads) 
    {
        thread.join();
    }

    std::cout << "Multi-threading test passed!" << std::endl;
}

// 边界测试
void testEdgeCases() 
{
    std::cout << "Running edge cases test..." << std::endl;
    
    // 测试0大小分配
    void* ptr1 = MemoryPool::allocate(0);
    REQUIRE(ptr1 != nullptr);
    MemoryPool::deallocate(ptr1, 0);
    
    // 测试最小对齐大小
    void* ptr2 = MemoryPool::allocate(1);
    REQUIRE(ptr2 != nullptr);
    REQUIRE((reinterpret_cast<uintptr_t>(ptr2) & (ALIGNMENT - 1)) == 0);
    MemoryPool::deallocate(ptr2, 1);
    
    // 测试最大大小边界
    void* ptr3 = MemoryPool::allocate(MAX_BYTES);
    REQUIRE(ptr3 != nullptr);
    MemoryPool::deallocate(ptr3, MAX_BYTES);
    
    // 测试超过最大大小
    void* ptr4 = MemoryPool::allocate(MAX_BYTES + 1);
    REQUIRE(ptr4 != nullptr);
    MemoryPool::deallocate(ptr4, MAX_BYTES + 1);
    
    std::cout << "Edge cases test passed!" << std::endl;
}

// 压力测试
void testStress() 
{
    std::cout << "Running stress test..." << std::endl;

    const int NUM_ITERATIONS = 10000;
    std::vector<std::pair<void*, size_t>> allocations;
    allocations.reserve(NUM_ITERATIONS);

    for (int i = 0; i < NUM_ITERATIONS; ++i) 
    {
        size_t size = (rand() % 1024 + 1) * 8;
        void* ptr = MemoryPool::allocate(size);
        REQUIRE(ptr != nullptr);
        allocations.push_back({ptr, size});
    }

    // 随机顺序释放
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(allocations.begin(), allocations.end(), g);
    for (const auto& alloc : allocations) 
    {
        MemoryPool::deallocate(alloc.first, alloc.second);
    }

    std::cout << "Stress test passed!" << std::endl;
}

void testSizeClasses()
{
    std::cout << "Running size class test..." << std::endl;
    REQUIRE(SizeClass::classCount() > 0);
    REQUIRE(SizeClass::getSize(0) == ALIGNMENT);
    REQUIRE(SizeClass::getSize(SizeClass::classCount() - 1) == MAX_BYTES);

    for (size_t n = 1; n <= 4096; ++n)
    {
        const size_t rounded = SizeClass::roundUp(n);
        REQUIRE(rounded >= n);
        REQUIRE(rounded == SizeClass::getSize(SizeClass::getIndex(n)));
        (void)rounded;
    }

    std::cout << "Size class test passed!" << std::endl;
}

void testUnsizedDeallocate()
{
    std::cout << "Running unsized deallocate test..." << std::endl;

    void* p1 = MemoryPool::allocate(32);
    REQUIRE(p1 != nullptr);
    MemoryPool::deallocate(p1);

    void* p2 = MemoryPool::allocate(1024 * 1024);
    REQUIRE(p2 != nullptr);
    MemoryPool::deallocate(p2);

    std::cout << "Unsized deallocate test passed!" << std::endl;
}

void testAlignedAndTyped()
{
    std::cout << "Running aligned / typed allocation test..." << std::endl;

    void* p = MemoryPool::allocateAligned(24, 16);
    REQUIRE(p != nullptr);
    REQUIRE((reinterpret_cast<uintptr_t>(p) & 15) == 0);
    MemoryPool::deallocate(p);

    struct alignas(16) Vec4 { float v[4]; };
    Vec4* v = newElement<Vec4>();
    REQUIRE(v != nullptr);
    REQUIRE((reinterpret_cast<uintptr_t>(v) & 15) == 0);
    v->v[0] = 1.0f;
    deleteElement(v);

    std::cout << "Aligned / typed allocation test passed!" << std::endl;
}

void testThreadExitReturnsMemory()
{
    std::cout << "Running thread-exit return test..." << std::endl;

    auto worker = []() {
        for (int i = 0; i < 256; ++i)
        {
            void* p = MemoryPool::allocate(64);
            REQUIRE(p != nullptr);
            MemoryPool::deallocate(p);
        }
        // 离开作用域时 ThreadCache 析构，应把残留块还给 CentralCache
        for (int i = 0; i < 32; ++i)
            (void)MemoryPool::allocate(128);
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i)
        threads.emplace_back(worker);
    for (auto& t : threads)
        t.join();

    void* p = MemoryPool::allocate(128);
    REQUIRE(p != nullptr);
    MemoryPool::deallocate(p);

    std::cout << "Thread-exit return test passed!" << std::endl;
}

void testSdkWrappers()
{
    std::cout << "Running SDK wrapper test..." << std::endl;

    void* p = xmd_alloc(48);
    REQUIRE(p != nullptr);
    static_cast<char*>(p)[0] = 1;
    xmd_free(p);
    xmd_trim();

    std::vector<int, Allocator<int>> nums;
    nums.push_back(1);
    nums.push_back(2);
    REQUIRE(nums.size() == 2);
    REQUIRE(nums[0] == 1);

    std::cout << "SDK wrapper test passed!" << std::endl;
}

void testStatsAndSizedFreePath()
{
    std::cout << "Running stats / sized free test..." << std::endl;

#if !XMD_MEMORY_POOL_STATS_ENABLED
    {
        std::cout << "Statistics disabled, skipping." << std::endl;
        return;
    }
#else
#if XMD_MEMORY_POOL_STATS_ENABLED
    const MemoryPoolStats before = MemoryPool::stats();
#endif

    void* p1 = MemoryPool::allocate(24);
    void* p2 = MemoryPool::allocate(512 * 1024);
    REQUIRE(p1 != nullptr);
    REQUIRE(p2 != nullptr);

    MemoryPool::deallocate(p1, 24);
    MemoryPool::deallocate(p2, 512 * 1024);

#if XMD_MEMORY_POOL_STATS_ENABLED
    const MemoryPoolStats after = MemoryPool::stats();
    REQUIRE(after.allocCount >= before.allocCount + 2);
    REQUIRE(after.freeCount >= before.freeCount + 2);
    REQUIRE(after.smallAllocCount >= before.smallAllocCount + 1);
    REQUIRE(after.largeAllocCount >= before.largeAllocCount + 1);
    REQUIRE(after.sizedFreeCount >= before.sizedFreeCount + 2);
    REQUIRE(after.centralRefillCount >= before.centralRefillCount);
    REQUIRE(after.centralFlushCount >= before.centralFlushCount);

    std::cout << "Stats / sized free test passed!" << std::endl;
#endif
#endif
}

void testCrossThreadSizedFree()
{
    std::cout << "Running cross-thread sized free test..." << std::endl;

    std::vector<void*> ptrs;
    ptrs.reserve(1024);
    for (int i = 0; i < 1024; ++i)
    {
        void* p = MemoryPool::allocate(64);
        REQUIRE(p != nullptr);
        ptrs.push_back(p);
    }

    std::thread releaser([&ptrs]() {
        for (void* p : ptrs)
            MemoryPool::deallocate(p, 64);
    });
    releaser.join();

    void* p = MemoryPool::allocate(64);
    REQUIRE(p != nullptr);
    MemoryPool::deallocate(p, 64);

    std::cout << "Cross-thread sized free test passed!" << std::endl;
}

void testNewElementExceptionSafety()
{
    std::cout << "Running newElement exception safety test..." << std::endl;

    struct ThrowOnConstruct
    {
        ThrowOnConstruct() { throw std::runtime_error("boom"); }
    };

#if XMD_MEMORY_POOL_STATS_ENABLED
    const MemoryPoolStats before = MemoryPool::stats();
#endif
    bool thrown = false;
    try
    {
        (void)MemoryPool::newElement<ThrowOnConstruct>();
    }
    catch (const std::runtime_error&)
    {
        thrown = true;
    }
    REQUIRE(thrown);
    (void)thrown;

#if XMD_MEMORY_POOL_STATS_ENABLED
    {
        const MemoryPoolStats after = MemoryPool::stats();
        REQUIRE(after.allocCount >= before.allocCount + 1);
        REQUIRE(after.freeCount >= before.freeCount + 1);
        REQUIRE(after.liveAllocs == before.liveAllocs);
    }
#endif

    std::cout << "newElement exception safety test passed!" << std::endl;
}

void testNoLeaksAfterMixedTraffic()
{
    std::cout << "Running no-leak mixed traffic test..." << std::endl;

#if XMD_MEMORY_POOL_STATS_ENABLED
    const MemoryPoolStats before = MemoryPool::stats();
#endif

    {
        std::vector<std::pair<void*, size_t>> ptrs;
        ptrs.reserve(4096);

        for (size_t i = 1; i <= 2048; ++i)
        {
            const size_t size = (i % 2 == 0) ? ((i % 256) + 1) * 8 : (64 + (i % 8) * 64);
            void* p = MemoryPool::allocate(size);
            REQUIRE(p != nullptr);
            ptrs.emplace_back(p, size);
        }

        for (size_t i = 0; i < ptrs.size(); i += 3)
            MemoryPool::deallocate(ptrs[i].first, ptrs[i].second);

        std::vector<std::thread> threads;
        for (size_t t = 0; t < 4; ++t)
        {
            threads.emplace_back([&, t]() {
                for (size_t i = t; i < ptrs.size(); i += 4)
                {
                    if (i % 3 != 0)
                        MemoryPool::deallocate(ptrs[i].first, ptrs[i].second);
                }
            });
        }
        for (auto& thread : threads)
            thread.join();
    }

    void* big = MemoryPool::allocate(1024 * 1024);
    REQUIRE(big != nullptr);
    MemoryPool::deallocate(big, 1024 * 1024);

#if XMD_MEMORY_POOL_STATS_ENABLED
    const MemoryPoolStats after = MemoryPool::stats();
    REQUIRE(after.liveAllocs == before.liveAllocs);
    REQUIRE(after.cachedPageBytes <= 64ull * 1024 * 1024);
    REQUIRE(after.reservedBytes >= after.cachedPageBytes);
#endif

    std::cout << "No-leak mixed traffic test passed!" << std::endl;
}

void testDebugGuards()
{
    std::cout << "Running debug guard test..." << std::endl;

#if !XMD_MEMORY_POOL_DEBUG_GUARDS
    {
        std::cout << "Debug guards disabled, skipping." << std::endl;
        return;
    }
#else

    const MemoryPoolStats before = MemoryPool::stats();

    void* p = MemoryPool::allocate(64);
    REQUIRE(p != nullptr);
    MemoryPool::deallocate(p, 64);
    MemoryPool::deallocate(p, 64); // double free should be rejected

    void* q = MemoryPool::allocate(64);
    REQUIRE(q != nullptr);
    MemoryPool::deallocate(static_cast<char*>(q) + 8, 64); // interior pointer should be rejected
    MemoryPool::deallocate(q, 64);

    alignas(64) char foreign[64]{};
    MemoryPool::deallocate(foreign, 64); // foreign pointer should be ignored

    const MemoryPoolStats after = MemoryPool::stats();
    REQUIRE(after.liveAllocs == before.liveAllocs);

    std::cout << "Debug guard test passed!" << std::endl;
#endif
}

void testLargeSpanLifetime()
{
    std::cout << "Running large span lifetime test..." << std::endl;

    void* first = MemoryPool::allocate(300 * 1024);
    REQUIRE(first != nullptr);
    MemoryPool::deallocate(first);

    void* reused = MemoryPool::allocate(280 * 1024);
    void* neighbor = MemoryPool::allocate(16 * 1024);
    REQUIRE(reused != nullptr);
    REQUIRE(neighbor != nullptr);
    static_cast<unsigned char*>(neighbor)[0] = 0x5a;
    MemoryPool::deallocate(reused);
    static_cast<unsigned char*>(neighbor)[0] = 0xa5;
    MemoryPool::deallocate(neighbor);

    std::cout << "Large span lifetime test passed!" << std::endl;
}

void testInvalidInputs()
{
    std::cout << "Running invalid input test..." << std::endl;
    REQUIRE(MemoryPool::allocateAligned(64, 0) == nullptr);
    REQUIRE(MemoryPool::allocateAligned(64, 3) == nullptr);
    REQUIRE(MemoryPool::allocateAligned(SIZE_MAX, 64) == nullptr);
    REQUIRE(MemoryPool::allocate(SIZE_MAX) == nullptr);
    std::cout << "Invalid input test passed!" << std::endl;
}

void testAlignmentSweep()
{
    std::cout << "Running alignment sweep test..." << std::endl;
    for (size_t alignment : {size_t(8), size_t(16), size_t(32), size_t(64),
                             size_t(128), size_t(256), size_t(512), size_t(4096)})
    {
        for (size_t size : {size_t(1), size_t(7), size_t(31), size_t(255),
                            size_t(1023), size_t(4097), size_t(64 * 1024)})
        {
            void* ptr = MemoryPool::allocateAligned(size, alignment);
            REQUIRE(ptr != nullptr);
            REQUIRE((reinterpret_cast<uintptr_t>(ptr) & (alignment - 1)) == 0);
            std::memset(ptr, 0xa5, size);
            MemoryPool::deallocate(ptr);
        }
    }
    std::cout << "Alignment sweep test passed!" << std::endl;
}

void testEverySizeClassBoundary()
{
    std::cout << "Running size class boundary test..." << std::endl;
    for (size_t index = 0; index < SizeClass::classCount(); ++index)
    {
        const size_t boundary = SizeClass::getSize(index);
        for (size_t size : {boundary > 1 ? boundary - 1 : boundary, boundary})
        {
            void* ptr = MemoryPool::allocate(size);
            REQUIRE(ptr != nullptr);
            std::memset(ptr, static_cast<int>(index), size);
            MemoryPool::deallocate(ptr, size);
        }
    }
    std::cout << "Size class boundary test passed!" << std::endl;
}

void testLongRandomTraffic()
{
    std::cout << "Running long random traffic test..." << std::endl;
    std::mt19937 rng(0x51a7u);
    std::uniform_int_distribution<size_t> sizeDist(1, 256 * 1024);
    std::vector<std::pair<void*, size_t>> live;
    live.reserve(8192);

    for (size_t i = 0; i < 100000; ++i)
    {
        if (!live.empty() && (rng() % 3u == 0))
        {
            const size_t index = rng() % live.size();
            REQUIRE(checkPattern(live[index].first, live[index].second,
                                 static_cast<std::uint32_t>(live[index].second)));
            MemoryPool::deallocate(live[index].first, live[index].second);
            live[index] = live.back();
            live.pop_back();
        }
        else
        {
            const size_t size = sizeDist(rng);
            void* ptr = MemoryPool::allocate(size);
            REQUIRE(ptr != nullptr);
            fillPattern(ptr, size, static_cast<std::uint32_t>(size));
            live.emplace_back(ptr, size);
        }
    }

    for (const auto& item : live)
    {
        REQUIRE(checkPattern(item.first, item.second, static_cast<std::uint32_t>(item.second)));
        MemoryPool::deallocate(item.first, item.second);
    }
    std::cout << "Long random traffic test passed!" << std::endl;
}

void testCrossThreadUnsizedFree()
{
    std::cout << "Running cross-thread unsized free test..." << std::endl;

    std::vector<void*> ptrs;
    ptrs.reserve(2048);
    for (int i = 0; i < 2048; ++i)
    {
        void* p = MemoryPool::allocate(96);
        REQUIRE(p != nullptr);
        ptrs.push_back(p);
    }

    std::thread releaser([&ptrs]() {
        for (void* p : ptrs)
            MemoryPool::deallocate(p); // unsized path + findSpan
    });
    releaser.join();

    void* p = MemoryPool::allocate(96);
    REQUIRE(p != nullptr);
    std::memset(p, 0x11, 96);
    MemoryPool::deallocate(p);

    std::cout << "Cross-thread unsized free test passed!" << std::endl;
}

void testCrossThreadFreeOnThreadExit()
{
    std::cout << "Running cross-thread free on thread-exit test..." << std::endl;

    // The freeing thread owns these blocks in its TLS cache until thread exit.
    constexpr int kCount = 40;
    std::vector<void*> ptrs;
    ptrs.reserve(kCount);
    for (int i = 0; i < kCount; ++i)
    {
        void* p = MemoryPool::allocate(64);
        REQUIRE(p != nullptr);
        ptrs.push_back(p);
    }

    {
        std::thread releaser([&ptrs]() {
            for (void* p : ptrs)
                MemoryPool::deallocate(p, 64);
        });
        releaser.join();
    }

    for (int i = 0; i < kCount; ++i)
    {
        void* p = MemoryPool::allocate(64);
        REQUIRE(p != nullptr);
        static_cast<unsigned char*>(p)[0] = 0x22;
        MemoryPool::deallocate(p, 64);
    }

    std::cout << "Cross-thread free on thread-exit test passed!" << std::endl;
}

void testMultiConsumerCrossThreadFree()
{
    std::cout << "Running multi-consumer cross-thread free test..." << std::endl;

    constexpr int kProducers = 4;
    constexpr int kPerProducer = 512;
    std::vector<std::vector<void*>> bins(kProducers);

    std::vector<std::thread> producers;
    for (int t = 0; t < kProducers; ++t)
    {
        producers.emplace_back([&bins, t, kPerProducer]() {
            bins[t].reserve(kPerProducer);
            for (int i = 0; i < kPerProducer; ++i)
            {
                void* p = MemoryPool::allocate(128);
                REQUIRE(p != nullptr);
                bins[t].push_back(p);
            }
        });
    }
    for (auto& th : producers)
        th.join();

    std::vector<std::thread> consumers;
    for (int t = 0; t < kProducers; ++t)
    {
        consumers.emplace_back([&bins, t, kProducers]() {
            // Consumer t frees blocks produced by ((t+1)%N).
            const int src = (t + 1) % kProducers;
            for (void* p : bins[src])
                MemoryPool::deallocate(p, 128);
        });
    }
    for (auto& th : consumers)
        th.join();

    void* p = MemoryPool::allocate(128);
    REQUIRE(p != nullptr);
    MemoryPool::deallocate(p, 128);

    std::cout << "Multi-consumer cross-thread free test passed!" << std::endl;
}

void testCApiAlignedAndSizedFree()
{
    std::cout << "Running C API aligned / sized free test..." << std::endl;

    void* p = xmd_alloc_aligned(100, 64);
    REQUIRE(p != nullptr);
    REQUIRE((reinterpret_cast<uintptr_t>(p) & 63) == 0);
    std::memset(p, 0x33, 100);
    xmd_free_sized(p, 100);

    void* q = xmd_alloc(256);
    REQUIRE(q != nullptr);
    xmd_free_sized(q, 256);

    REQUIRE(xmd_alloc_aligned(32, 0) == nullptr);
    REQUIRE(xmd_alloc_aligned(32, 3) == nullptr);

    std::cout << "C API aligned / sized free test passed!" << std::endl;
}

void testWrongSizeFreeRejectedWithGuards()
{
    std::cout << "Running wrong-size free guard test..." << std::endl;

#if !XMD_MEMORY_POOL_DEBUG_GUARDS
    std::cout << "Debug guards disabled, skipping." << std::endl;
#else
    void* p = MemoryPool::allocate(64);
    REQUIRE(p != nullptr);
    // Wrong size class should be rejected; pointer remains allocated.
    MemoryPool::deallocate(p, 32);
    std::memset(p, 0x44, 64);
    MemoryPool::deallocate(p, 64);
    std::cout << "Wrong-size free guard test passed!" << std::endl;
#endif
}

void testPageCacheCapAndReuse()
{
    std::cout << "Running page-cache cap / reuse test..." << std::endl;

    std::vector<void*> ptrs;
    ptrs.reserve(128);
    for (int i = 0; i < 128; ++i)
    {
        void* p = MemoryPool::allocate(512 * 1024);
        REQUIRE(p != nullptr);
        ptrs.push_back(p);
    }
    for (void* p : ptrs)
        MemoryPool::deallocate(p);

    const MemoryPoolStats afterFree = MemoryPool::stats();
    REQUIRE(afterFree.cachedPageBytes <= 64ull * 1024 * 1024);

    for (int i = 0; i < 32; ++i)
    {
        void* p = MemoryPool::allocate(400 * 1024);
        REQUIRE(p != nullptr);
        MemoryPool::deallocate(p);
    }

    std::cout << "Page-cache cap / reuse test passed!" << std::endl;
}

void testAllocatorGrowthAndClear()
{
    std::cout << "Running Allocator growth / clear test..." << std::endl;

    {
        std::vector<int, Allocator<int>> values;
        for (int i = 0; i < 10000; ++i)
            values.push_back(i);
        REQUIRE(values.size() == 10000);
        REQUIRE(values.front() == 0);
        REQUIRE(values.back() == 9999);
        values.clear();
        values.shrink_to_fit();
    }

    {
        std::vector<std::pair<int, int>, Allocator<std::pair<int, int>>> pairs;
        for (int i = 0; i < 2000; ++i)
            pairs.emplace_back(i, i * 2);
        REQUIRE(pairs.size() == 2000);
    }

    std::cout << "Allocator growth / clear test passed!" << std::endl;
}

void testMixedSizedAndUnsizedTraffic()
{
    std::cout << "Running mixed sized/unsized traffic test..." << std::endl;

    std::mt19937 rng(0xC0FFEEu);
    std::vector<std::pair<void*, size_t>> sized;
    std::vector<void*> unsized;
    sized.reserve(2048);
    unsized.reserve(2048);

    for (int i = 0; i < 4000; ++i)
    {
        const size_t size = 16 + (rng() % 512);
        void* p = MemoryPool::allocate(size);
        REQUIRE(p != nullptr);
        if (rng() & 1u)
            sized.emplace_back(p, size);
        else
            unsized.push_back(p);
    }

    for (const auto& item : sized)
        MemoryPool::deallocate(item.first, item.second);
    for (void* p : unsized)
        MemoryPool::deallocate(p);

    std::cout << "Mixed sized/unsized traffic test passed!" << std::endl;
}

void testPageReuseThenUnsizedFree()
{
    std::cout << "Running page-reuse unsized free integrity test..." << std::endl;

    std::vector<void*> small;
    small.reserve(24000);
    for (int i = 0; i < 24000; ++i)
    {
        void* p = MemoryPool::allocate(32);
        REQUIRE(p != nullptr);
        fillPattern(p, 32, static_cast<std::uint32_t>(i));
        small.push_back(p);
    }
    for (int i = 0; i < 24000; ++i)
    {
        REQUIRE(checkPattern(small[static_cast<size_t>(i)], 32, static_cast<std::uint32_t>(i)));
        MemoryPool::deallocate(small[static_cast<size_t>(i)]);
    }
    small.clear();

    const size_t kBigger[] = {128, 512, 1024, 4096};
    for (size_t size : kBigger)
    {
        std::vector<void*> blocks;
        blocks.reserve(1500);
        for (int i = 0; i < 1500; ++i)
        {
            void* p = MemoryPool::allocate(size);
            REQUIRE(p != nullptr);
            fillPattern(p, size, 0xA5000000u + static_cast<std::uint32_t>(size) + static_cast<std::uint32_t>(i));
            blocks.push_back(p);
        }
        for (int i = 0; i < 1500; ++i)
        {
            REQUIRE(checkPattern(blocks[static_cast<size_t>(i)], size,
                                 0xA5000000u + static_cast<std::uint32_t>(size) + static_cast<std::uint32_t>(i)));
            MemoryPool::deallocate(blocks[static_cast<size_t>(i)]);
        }
    }

    for (int i = 0; i < 4000; ++i)
    {
        void* p = MemoryPool::allocate(32);
        REQUIRE(p != nullptr);
        fillPattern(p, 32, 0x32u + static_cast<std::uint32_t>(i));
        REQUIRE(checkPattern(p, 32, 0x32u + static_cast<std::uint32_t>(i)));
        MemoryPool::deallocate(p);
    }

    std::cout << "Page-reuse unsized free integrity test passed!" << std::endl;
}

void testMultiThreadPayloadIntegrity()
{
    std::cout << "Running multi-thread payload integrity test..." << std::endl;

    constexpr int kThreads = 8;
    std::atomic<int> failures{0};

    auto worker = [&failures](int tid) {
        constexpr int kLocalOps = 6000;
        std::mt19937 rng(static_cast<std::uint32_t>(0xC0DE0000u + tid));
        std::vector<std::tuple<void*, size_t, std::uint32_t>> live;
        live.reserve(1024);
        for (int i = 0; i < kLocalOps; ++i)
        {
            const bool doFree = !live.empty() && ((rng() % 3u) != 0u);
            if (doFree)
            {
                const size_t idx = rng() % live.size();
                const auto item = live[idx];
                void* ptr = std::get<0>(item);
                const size_t size = std::get<1>(item);
                const std::uint32_t seed = std::get<2>(item);
                if (!checkPattern(ptr, size, seed))
                    failures.fetch_add(1, std::memory_order_relaxed);
                if ((rng() & 1u) != 0u)
                    MemoryPool::deallocate(ptr, size);
                else
                    MemoryPool::deallocate(ptr);
                live[idx] = live.back();
                live.pop_back();
            }
            else
            {
                const size_t size = 8 + (rng() % 2048);
                void* ptr = MemoryPool::allocate(size);
                if (!ptr)
                {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                const std::uint32_t seed = rng();
                fillPattern(ptr, size, seed);
                live.emplace_back(ptr, size, seed);
            }
        }
        for (const auto& item : live)
        {
            void* ptr = std::get<0>(item);
            const size_t size = std::get<1>(item);
            const std::uint32_t seed = std::get<2>(item);
            if (!checkPattern(ptr, size, seed))
                failures.fetch_add(1, std::memory_order_relaxed);
            MemoryPool::deallocate(ptr, size);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back(worker, t);
    for (auto& th : threads)
        th.join();

    REQUIRE(failures.load() == 0);
    std::cout << "Multi-thread payload integrity test passed!" << std::endl;
}

void testThreadChurn()
{
    std::cout << "Running thread-churn test..." << std::endl;

    std::atomic<int> failures{0};
    for (int wave = 0; wave < 24; ++wave)
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t)
        {
            threads.emplace_back([&failures, wave, t]() {
                std::vector<void*> ptrs;
                ptrs.reserve(400);
                for (int i = 0; i < 400; ++i)
                {
                    const size_t size = 16u << (static_cast<unsigned>(i + wave + t) % 8u);
                    void* p = MemoryPool::allocate(size);
                    if (!p)
                    {
                        failures.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                    fillPattern(p, size, static_cast<std::uint32_t>(i + 1));
                    ptrs.push_back(p);
                }
                for (int i = 0; i < 400; ++i)
                {
                    const size_t size = 16u << (static_cast<unsigned>(i + wave + t) % 8u);
                    if (!checkPattern(ptrs[static_cast<size_t>(i)], size, static_cast<std::uint32_t>(i + 1)))
                        failures.fetch_add(1, std::memory_order_relaxed);
                    MemoryPool::deallocate(ptrs[static_cast<size_t>(i)]);
                }
            });
        }
        for (auto& th : threads)
            th.join();
    }

    REQUIRE(failures.load() == 0);
    std::cout << "Thread-churn test passed!" << std::endl;
}

void testGrowCopyFree()
{
    std::cout << "Running grow-copy-free test..." << std::endl;

    void* p = MemoryPool::allocate(16);
    REQUIRE(p != nullptr);
    fillPattern(p, 16, 1);
    size_t cap = 16;
    for (int step = 0; step < 12; ++step)
    {
        const size_t next = cap * 2;
        void* q = MemoryPool::allocate(next);
        REQUIRE(q != nullptr);
        std::memcpy(q, p, cap);
        REQUIRE(checkPattern(q, cap, 1));
        fillPattern(q, next, 1);
        MemoryPool::deallocate(p);
        p = q;
        cap = next;
    }
    REQUIRE(checkPattern(p, cap, 1));
    MemoryPool::deallocate(p);
    std::cout << "Grow-copy-free test passed!" << std::endl;
}

void testAlignedUnsizedFree()
{
    std::cout << "Running aligned unsized free test..." << std::endl;

    for (size_t align : {16u, 32u, 64u, 128u})
    {
        void* p = MemoryPool::allocateAligned(96, align);
        REQUIRE(p != nullptr);
        REQUIRE((reinterpret_cast<std::uintptr_t>(p) & (align - 1)) == 0);
        fillPattern(p, 96, static_cast<std::uint32_t>(align));
        REQUIRE(checkPattern(p, 96, static_cast<std::uint32_t>(align)));
        MemoryPool::deallocate(p);
    }
    std::cout << "Aligned unsized free test passed!" << std::endl;
}

void testExplicitTrim()
{
    std::cout << "Running explicit thread-cache trim test..." << std::endl;

    std::vector<void*> ptrs;
    ptrs.reserve(4096);
    for (size_t i = 0; i < 4096; ++i)
    {
        void* p = MemoryPool::allocate(64);
        REQUIRE(p != nullptr);
        fillPattern(p, 64, static_cast<std::uint32_t>(i));
        ptrs.push_back(p);
    }
    for (void* p : ptrs)
        MemoryPool::deallocate(p, 64);

    MemoryPool::trim();

    void* probe = MemoryPool::allocate(64);
    REQUIRE(probe != nullptr);
    fillPattern(probe, 64, 0xCACEu);
    REQUIRE(checkPattern(probe, 64, 0xCACEu));
    MemoryPool::deallocate(probe, 64);

    std::cout << "Explicit thread-cache trim test passed!" << std::endl;
}

void testCacheBudgetsAndReclamation()
{
    std::cout << "Running cache budget / reclamation test..." << std::endl;
    constexpr size_t MiB = 1024 * 1024;
    const size_t oldPageLimit = MemoryPool::pageCacheLimit();
    const size_t oldLargeLimit = MemoryPool::largeCacheLimit();

    xmd_set_page_cache_limit(128 * MiB);
    xmd_set_large_cache_limit(0);
    REQUIRE(xmd_page_cache_limit() == 128 * MiB);
    REQUIRE(xmd_large_cache_limit() == 0);
    MemoryPool::releaseUnused();

    void* active = MemoryPool::allocate(24 * MiB);
    void* idle = MemoryPool::allocate(24 * MiB);
    REQUIRE(active != nullptr && idle != nullptr);
    fillPattern(active, 64, 0xCAFEu);
    MemoryPool::deallocate(idle, 24 * MiB);
    const auto cached = MemoryPool::stats();
    REQUIRE(cached.cachedPageBytes >= 24 * MiB);

    MemoryPool::setPageCacheLimit(0);
    const auto limited = MemoryPool::stats();
    REQUIRE(limited.cachedPageBytes + 24 * MiB <= cached.cachedPageBytes);
    REQUIRE(checkPattern(active, 64, 0xCAFEu));
    REQUIRE(limited.reservedBytes >= 24 * MiB);

    MemoryPool::setPageCacheLimit(128 * MiB);
    void* idleAgain = MemoryPool::allocate(24 * MiB);
    REQUIRE(idleAgain != nullptr);
    MemoryPool::deallocate(idleAgain, 24 * MiB);
    const auto beforeRelease = MemoryPool::stats();
    xmd_release_unused();
    const auto afterRelease = MemoryPool::stats();
    REQUIRE(afterRelease.cachedPageBytes + 24 * MiB <= beforeRelease.cachedPageBytes);
    REQUIRE(checkPattern(active, 64, 0xCAFEu));

    MemoryPool::deallocate(active, 24 * MiB);
    MemoryPool::setPageCacheLimit(oldPageLimit);
    MemoryPool::setLargeCacheLimit(oldLargeLimit);
    MemoryPool::releaseUnused();
    std::cout << "Cache budget / reclamation test passed!" << std::endl;
}

void testSplitLargeSpanReclamation()
{
    PageCache& cache = PageCache::getInstance();
    const size_t oldLimit = cache.cachedPageLimit();
    const size_t pageSize = PageCache::pageSize();
    cache.setCachedPageLimit(0);
    cache.releaseCachedPages();
    const size_t baseline = cache.reservedBytes();
    cache.setCachedPageLimit(16 * 1024 * 1024);

    for (bool largeFirst : {true, false})
    {
        Span* origin = cache.allocateSpan(521);
        REQUIRE(origin != nullptr);
        REQUIRE(origin->pageAddr == origin->originAddr);
        REQUIRE(origin->numPages == origin->originPages);
        void* base = origin->pageAddr;
        const size_t originPages = origin->numPages;
        cache.deallocateSpan(origin);

        Span* small = nullptr;
        if (!largeFirst)
        {
            small = cache.allocateSpan(128);
            REQUIRE(small != nullptr && small->pageAddr == base);
        }
        Span* large = cache.allocateLargeSpan(128);
        REQUIRE(large != nullptr);
        REQUIRE(cache.findSpan(large->pageAddr) == nullptr);
        if (largeFirst)
            REQUIRE(large->pageAddr == base);
        else
            REQUIRE(large->pageAddr == static_cast<char*>(base) + 128 * pageSize);
        if (small)
            cache.deallocateSpan(small);
        cache.deallocateSpan(large);

        Span* merged = cache.findSpan(base);
        REQUIRE(merged != nullptr);
        REQUIRE(merged->pageAddr == base && merged->numPages == originPages);
        REQUIRE(cache.releaseCachedPages() >= originPages * pageSize);
        REQUIRE(cache.findSpan(base) == nullptr);
        REQUIRE(cache.reservedBytes() == baseline);
    }
    cache.setCachedPageLimit(oldLimit);
}

void testLargeCacheLimitAndCrossThreadFree()
{
    const size_t oldPageLimit = MemoryPool::pageCacheLimit();
    const size_t oldLargeLimit = MemoryPool::largeCacheLimit();
    MemoryPool::setLargeCacheLimit(0);
    MemoryPool::setPageCacheLimit(0);
    MemoryPool::releaseUnused();
    const size_t baseline = MemoryPool::stats().reservedBytes;

    MemoryPool::setLargeCacheLimit(8 * 1024 * 1024);
    void* retained = MemoryPool::allocate(2 * 1024 * 1024);
    REQUIRE(retained != nullptr);
    MemoryPool::deallocate(retained, 2 * 1024 * 1024);
    REQUIRE(MemoryPool::stats().reservedBytes >= baseline + 2 * 1024 * 1024);
    MemoryPool::setLargeCacheLimit(0);
    REQUIRE(MemoryPool::stats().reservedBytes == baseline);

    for (size_t size : {size_t(512 * 1024), size_t(1024 * 1024),
                        size_t(2 * 1024 * 1024)})
    {
        void* first = MemoryPool::allocate(size);
        void* second = MemoryPool::allocate(size);
        REQUIRE(first != nullptr && second != nullptr);
        std::thread releaser([second, size] { MemoryPool::deallocate(second, size); });
        releaser.join();
        MemoryPool::deallocate(first, size);
        REQUIRE(MemoryPool::stats().reservedBytes == baseline);
    }

    MemoryPool::setPageCacheLimit(oldPageLimit);
    MemoryPool::setLargeCacheLimit(oldLargeLimit);
}

void testConcurrentCacheReclamation()
{
    std::cout << "Running concurrent cache reclamation test..." << std::endl;
    const size_t oldPageLimit = MemoryPool::pageCacheLimit();
    const size_t oldLargeLimit = MemoryPool::largeCacheLimit();
    MemoryPool::setLargeCacheLimit(0);
    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t)
    {
        workers.emplace_back([&start, t]() {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int i = 0; i < 1000; ++i)
            {
                const size_t size = i % 40 == 0 ? 1 * 1024 * 1024 : 64;
                void* p = MemoryPool::allocate(size);
                REQUIRE(p != nullptr);
                fillPattern(p, 64, static_cast<std::uint32_t>(t * 1000 + i));
                REQUIRE(checkPattern(p, 64, static_cast<std::uint32_t>(t * 1000 + i)));
                MemoryPool::deallocate(p, size);
            }
        });
    }
    std::thread maintainer([&start]() {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (int i = 0; i < 80; ++i)
        {
            MemoryPool::setPageCacheLimit(i % 2 == 0 ? 0 : 8 * 1024 * 1024);
            MemoryPool::releaseUnused();
        }
    });
    start.store(true, std::memory_order_release);
    for (auto& worker : workers)
        worker.join();
    maintainer.join();
    MemoryPool::setPageCacheLimit(oldPageLimit);
    MemoryPool::setLargeCacheLimit(oldLargeLimit);
    MemoryPool::releaseUnused();
    std::cout << "Concurrent cache reclamation test passed!" << std::endl;
}

int main() 
{
    try 
    {
        std::cout << "Starting memory pool tests..." << std::endl;

        // Core API
        testBasicAllocation();
        testMemoryWriting();
        testEdgeCases();
        testInvalidInputs();
        testSizeClasses();
        testEverySizeClassBoundary();
        testUnsizedDeallocate();
        testAlignedAndTyped();
        testAlignmentSweep();
        testCApiAlignedAndSizedFree();
        testAllocatorGrowthAndClear();
        testSdkWrappers();
        testNewElementExceptionSafety();

        // Concurrency / ownership transfer
        testMultiThreading();
        testThreadExitReturnsMemory();
        testCrossThreadSizedFree();
        testCrossThreadUnsizedFree();
        testCrossThreadFreeOnThreadExit();
        testMultiConsumerCrossThreadFree();

        // Lifetime / cache / guards
        testSplitLargeSpanReclamation();
        testLargeSpanLifetime();
        testPageCacheCapAndReuse();
        testStatsAndSizedFreePath();
        testNoLeaksAfterMixedTraffic();
        testDebugGuards();
        testWrongSizeFreeRejectedWithGuards();

        // Stress
        testStress();
        testMixedSizedAndUnsizedTraffic();
        testLongRandomTraffic();
        testPageReuseThenUnsizedFree();
        testMultiThreadPayloadIntegrity();
        testThreadChurn();
        testGrowCopyFree();
        testAlignedUnsizedFree();
        testExplicitTrim();
        testCacheBudgetsAndReclamation();
        testLargeCacheLimitAndCrossThreadFree();
        testConcurrentCacheReclamation();

        std::cout << "All tests passed successfully!" << std::endl;
        return 0;
    }
    catch (const std::exception& e) 
    {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
