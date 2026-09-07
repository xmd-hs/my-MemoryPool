#include <kama/MemoryPool.h>
#include <kama/Allocator.h>
#include <kama/kama_memory_pool.h>
#include "Common.h"
#include <iostream>
#include <vector>
#include <thread>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <random>
#include <algorithm>
#include <atomic>
#include <stdexcept>

using namespace Kama_memoryPool;

// 基础分配测试
void testBasicAllocation() 
{
    std::cout << "Running basic allocation test..." << std::endl;
    
    // 测试小内存分配
    void* ptr1 = MemoryPool::allocate(8);
    assert(ptr1 != nullptr);
    MemoryPool::deallocate(ptr1, 8);

    // 测试中等大小内存分配
    void* ptr2 = MemoryPool::allocate(1024);
    assert(ptr2 != nullptr);
    MemoryPool::deallocate(ptr2, 1024);

    // 测试大内存分配（超过MAX_BYTES）
    void* ptr3 = MemoryPool::allocate(1024 * 1024);
    assert(ptr3 != nullptr);
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
    assert(ptr != nullptr);

    // 写入数据
    for (size_t i = 0; i < size; ++i) 
    {
        ptr[i] = static_cast<char>(i % 256);
    }

    // 验证数据
    for (size_t i = 0; i < size; ++i) 
    {
        assert(ptr[i] == static_cast<char>(i % 256));
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
    assert(ptr1 != nullptr);
    MemoryPool::deallocate(ptr1, 0);
    
    // 测试最小对齐大小
    void* ptr2 = MemoryPool::allocate(1);
    assert(ptr2 != nullptr);
    assert((reinterpret_cast<uintptr_t>(ptr2) & (ALIGNMENT - 1)) == 0);
    MemoryPool::deallocate(ptr2, 1);
    
    // 测试最大大小边界
    void* ptr3 = MemoryPool::allocate(MAX_BYTES);
    assert(ptr3 != nullptr);
    MemoryPool::deallocate(ptr3, MAX_BYTES);
    
    // 测试超过最大大小
    void* ptr4 = MemoryPool::allocate(MAX_BYTES + 1);
    assert(ptr4 != nullptr);
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
        assert(ptr != nullptr);
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
    assert(SizeClass::classCount() > 0);
    assert(SizeClass::getSize(0) == ALIGNMENT);
    assert(SizeClass::getSize(SizeClass::classCount() - 1) == MAX_BYTES);

    for (size_t n = 1; n <= 4096; ++n)
    {
        const size_t rounded = SizeClass::roundUp(n);
        assert(rounded >= n);
        assert(rounded == SizeClass::getSize(SizeClass::getIndex(n)));
        (void)rounded;
    }

    std::cout << "Size class test passed!" << std::endl;
}

void testUnsizedDeallocate()
{
    std::cout << "Running unsized deallocate test..." << std::endl;

    void* p1 = MemoryPool::allocate(32);
    assert(p1 != nullptr);
    MemoryPool::deallocate(p1);

    void* p2 = MemoryPool::allocate(1024 * 1024);
    assert(p2 != nullptr);
    MemoryPool::deallocate(p2);

    std::cout << "Unsized deallocate test passed!" << std::endl;
}

void testAlignedAndTyped()
{
    std::cout << "Running aligned / typed allocation test..." << std::endl;

    void* p = MemoryPool::allocateAligned(24, 16);
    assert(p != nullptr);
    assert((reinterpret_cast<uintptr_t>(p) & 15) == 0);
    MemoryPool::deallocate(p);

    struct alignas(16) Vec4 { float v[4]; };
    Vec4* v = newElement<Vec4>();
    assert(v != nullptr);
    assert((reinterpret_cast<uintptr_t>(v) & 15) == 0);
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
            assert(p != nullptr);
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
    assert(p != nullptr);
    MemoryPool::deallocate(p);

    std::cout << "Thread-exit return test passed!" << std::endl;
}

void testSdkWrappers()
{
    std::cout << "Running SDK wrapper test..." << std::endl;

    void* p = kama_alloc(48);
    assert(p != nullptr);
    static_cast<char*>(p)[0] = 1;
    kama_free(p);

    std::vector<int, Allocator<int>> nums;
    nums.push_back(1);
    nums.push_back(2);
    assert(nums.size() == 2);
    assert(nums[0] == 1);

    std::cout << "SDK wrapper test passed!" << std::endl;
}

void testStatsAndSizedFreePath()
{
    std::cout << "Running stats / sized free test..." << std::endl;

#if !KAMA_MEMORY_POOL_STATS_ENABLED
    {
        std::cout << "Statistics disabled, skipping." << std::endl;
        return;
    }
#else
#if KAMA_MEMORY_POOL_STATS_ENABLED
    const MemoryPoolStats before = MemoryPool::stats();
#endif

    void* p1 = MemoryPool::allocate(24);
    void* p2 = MemoryPool::allocate(512 * 1024);
    assert(p1 != nullptr);
    assert(p2 != nullptr);

    MemoryPool::deallocate(p1, 24);
    MemoryPool::deallocate(p2, 512 * 1024);

#if KAMA_MEMORY_POOL_STATS_ENABLED
    const MemoryPoolStats after = MemoryPool::stats();
    assert(after.allocCount >= before.allocCount + 2);
    assert(after.freeCount >= before.freeCount + 2);
    assert(after.smallAllocCount >= before.smallAllocCount + 1);
    assert(after.largeAllocCount >= before.largeAllocCount + 1);
    assert(after.sizedFreeCount >= before.sizedFreeCount + 2);
    assert(after.centralRefillCount >= before.centralRefillCount);
    assert(after.centralFlushCount >= before.centralFlushCount);

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
        assert(p != nullptr);
        ptrs.push_back(p);
    }

    std::thread releaser([&ptrs]() {
        for (void* p : ptrs)
            MemoryPool::deallocate(p, 64);
    });
    releaser.join();

    void* p = MemoryPool::allocate(64);
    assert(p != nullptr);
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

#if KAMA_MEMORY_POOL_STATS_ENABLED
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
    assert(thrown);
    (void)thrown;

#if KAMA_MEMORY_POOL_STATS_ENABLED
    {
        const MemoryPoolStats after = MemoryPool::stats();
        assert(after.allocCount >= before.allocCount + 1);
        assert(after.freeCount >= before.freeCount + 1);
        assert(after.liveAllocs == before.liveAllocs);
    }
#endif

    std::cout << "newElement exception safety test passed!" << std::endl;
}

void testNoLeaksAfterMixedTraffic()
{
    std::cout << "Running no-leak mixed traffic test..." << std::endl;

#if KAMA_MEMORY_POOL_STATS_ENABLED
    const MemoryPoolStats before = MemoryPool::stats();
#endif

    {
        std::vector<std::pair<void*, size_t>> ptrs;
        ptrs.reserve(4096);

        for (size_t i = 1; i <= 2048; ++i)
        {
            const size_t size = (i % 2 == 0) ? ((i % 256) + 1) * 8 : (64 + (i % 8) * 64);
            void* p = MemoryPool::allocate(size);
            assert(p != nullptr);
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
    assert(big != nullptr);
    MemoryPool::deallocate(big, 1024 * 1024);

#if KAMA_MEMORY_POOL_STATS_ENABLED
    const MemoryPoolStats after = MemoryPool::stats();
    assert(after.liveAllocs == before.liveAllocs);
    assert(after.cachedPageBytes <= 64ull * 1024 * 1024);
    assert(after.reservedBytes >= after.cachedPageBytes);
#endif

    std::cout << "No-leak mixed traffic test passed!" << std::endl;
}

void testDebugGuards()
{
    std::cout << "Running debug guard test..." << std::endl;

#if !KAMA_MEMORY_POOL_DEBUG_GUARDS
    {
        std::cout << "Debug guards disabled, skipping." << std::endl;
        return;
    }
#else

    const MemoryPoolStats before = MemoryPool::stats();

    void* p = MemoryPool::allocate(64);
    assert(p != nullptr);
    MemoryPool::deallocate(p, 64);
    MemoryPool::deallocate(p, 64); // double free should be rejected

    void* q = MemoryPool::allocate(64);
    assert(q != nullptr);
    MemoryPool::deallocate(static_cast<char*>(q) + 8, 64); // interior pointer should be rejected
    MemoryPool::deallocate(q, 64);

    alignas(64) char foreign[64]{};
    MemoryPool::deallocate(foreign, 64); // foreign pointer should be ignored

    const MemoryPoolStats after = MemoryPool::stats();
    assert(after.liveAllocs == before.liveAllocs);

    std::cout << "Debug guard test passed!" << std::endl;
#endif
}

void testLargeSpanLifetime()
{
    std::cout << "Running large span lifetime test..." << std::endl;

    void* first = MemoryPool::allocate(300 * 1024);
    assert(first != nullptr);
    MemoryPool::deallocate(first);

    void* reused = MemoryPool::allocate(280 * 1024);
    void* neighbor = MemoryPool::allocate(16 * 1024);
    assert(reused != nullptr);
    assert(neighbor != nullptr);
    static_cast<unsigned char*>(neighbor)[0] = 0x5a;
    MemoryPool::deallocate(reused);
    static_cast<unsigned char*>(neighbor)[0] = 0xa5;
    MemoryPool::deallocate(neighbor);

    std::cout << "Large span lifetime test passed!" << std::endl;
}

void testInvalidInputs()
{
    std::cout << "Running invalid input test..." << std::endl;
    assert(MemoryPool::allocateAligned(64, 0) == nullptr);
    assert(MemoryPool::allocateAligned(64, 3) == nullptr);
    assert(MemoryPool::allocateAligned(SIZE_MAX, 64) == nullptr);
    assert(MemoryPool::allocate(SIZE_MAX) == nullptr);
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
            assert(ptr != nullptr);
            assert((reinterpret_cast<uintptr_t>(ptr) & (alignment - 1)) == 0);
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
            assert(ptr != nullptr);
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
            MemoryPool::deallocate(live[index].first, live[index].second);
            live[index] = live.back();
            live.pop_back();
        }
        else
        {
            const size_t size = sizeDist(rng);
            void* ptr = MemoryPool::allocate(size);
            assert(ptr != nullptr);
            static_cast<unsigned char*>(ptr)[0] = 0x3c;
            live.emplace_back(ptr, size);
        }
    }

    for (const auto& item : live)
        MemoryPool::deallocate(item.first, item.second);
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
        assert(p != nullptr);
        ptrs.push_back(p);
    }

    std::thread releaser([&ptrs]() {
        for (void* p : ptrs)
            MemoryPool::deallocate(p); // unsized path + findSpan
    });
    releaser.join();

    void* p = MemoryPool::allocate(96);
    assert(p != nullptr);
    std::memset(p, 0x11, 96);
    MemoryPool::deallocate(p);

    std::cout << "Cross-thread unsized free test passed!" << std::endl;
}

void testRemoteBatchPartialFlushOnThreadExit()
{
    std::cout << "Running remote-batch partial flush test..." << std::endl;

    // Fewer than the remote batch limit so frees stay in the TLS buffer until
    // the freeing thread exits and flushes the remainder.
    constexpr int kCount = 40;
    std::vector<void*> ptrs;
    ptrs.reserve(kCount);
    for (int i = 0; i < kCount; ++i)
    {
        void* p = MemoryPool::allocate(64);
        assert(p != nullptr);
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
        assert(p != nullptr);
        static_cast<unsigned char*>(p)[0] = 0x22;
        MemoryPool::deallocate(p, 64);
    }

    std::cout << "Remote-batch partial flush test passed!" << std::endl;
}

void testMultiConsumerRemoteFree()
{
    std::cout << "Running multi-consumer remote free test..." << std::endl;

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
                assert(p != nullptr);
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
            // Consumer t frees producer ((t+1)%N) blocks → forced remote path.
            const int src = (t + 1) % kProducers;
            for (void* p : bins[src])
                MemoryPool::deallocate(p, 128);
        });
    }
    for (auto& th : consumers)
        th.join();

    void* p = MemoryPool::allocate(128);
    assert(p != nullptr);
    MemoryPool::deallocate(p, 128);

    std::cout << "Multi-consumer remote free test passed!" << std::endl;
}

void testCApiAlignedAndSizedFree()
{
    std::cout << "Running C API aligned / sized free test..." << std::endl;

    void* p = kama_alloc_aligned(100, 64);
    assert(p != nullptr);
    assert((reinterpret_cast<uintptr_t>(p) & 63) == 0);
    std::memset(p, 0x33, 100);
    kama_free_sized(p, 100);

    void* q = kama_alloc(256);
    assert(q != nullptr);
    kama_free_sized(q, 256);

    assert(kama_alloc_aligned(32, 0) == nullptr);
    assert(kama_alloc_aligned(32, 3) == nullptr);

    std::cout << "C API aligned / sized free test passed!" << std::endl;
}

void testWrongSizeFreeRejectedWithGuards()
{
    std::cout << "Running wrong-size free guard test..." << std::endl;

#if !KAMA_MEMORY_POOL_DEBUG_GUARDS
    std::cout << "Debug guards disabled, skipping." << std::endl;
#else
    void* p = MemoryPool::allocate(64);
    assert(p != nullptr);
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
        assert(p != nullptr);
        ptrs.push_back(p);
    }
    for (void* p : ptrs)
        MemoryPool::deallocate(p);

    const MemoryPoolStats afterFree = MemoryPool::stats();
    assert(afterFree.cachedPageBytes <= 64ull * 1024 * 1024);

    for (int i = 0; i < 32; ++i)
    {
        void* p = MemoryPool::allocate(400 * 1024);
        assert(p != nullptr);
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
        assert(values.size() == 10000);
        assert(values.front() == 0);
        assert(values.back() == 9999);
        values.clear();
        values.shrink_to_fit();
    }

    {
        std::vector<std::pair<int, int>, Allocator<std::pair<int, int>>> pairs;
        for (int i = 0; i < 2000; ++i)
            pairs.emplace_back(i, i * 2);
        assert(pairs.size() == 2000);
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
        assert(p != nullptr);
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
        testRemoteBatchPartialFlushOnThreadExit();
        testMultiConsumerRemoteFree();

        // Lifetime / cache / guards
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

        std::cout << "All tests passed successfully!" << std::endl;
        return 0;
    }
    catch (const std::exception& e) 
    {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
