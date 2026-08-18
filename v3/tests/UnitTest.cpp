#include <kama/MemoryPool.h>
#include <kama/Allocator.h>
#include <kama/kama_memory_pool.h>
#include "Common.h"
#include <iostream>
#include <vector>
#include <thread>
#include <cassert>
#include <cstdint>
#include <random>
#include <algorithm>
#include <atomic>

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
    
    auto threadFunc = [&has_error]() 
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
        size_t rounded = SizeClass::roundUp(n);
        assert(rounded >= n);
        assert(rounded == SizeClass::getSize(SizeClass::getIndex(n)));
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

int main() 
{
    try 
    {
        std::cout << "Starting memory pool tests..." << std::endl;

        testBasicAllocation();
        testMemoryWriting();
        testMultiThreading();
        testEdgeCases();
        testStress();
        testSizeClasses();
        testUnsizedDeallocate();
        testAlignedAndTyped();
        testThreadExitReturnsMemory();
        testSdkWrappers();

        std::cout << "All tests passed successfully!" << std::endl;
        return 0;
    }
    catch (const std::exception& e) 
    {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}