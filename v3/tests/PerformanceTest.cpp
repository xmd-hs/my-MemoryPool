#include <kama/MemoryPool.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace Kama_memoryPool;
using Clock = std::chrono::steady_clock;

namespace
{

enum class Backend { Pool, NewDelete, MallocFree };

void* backendAllocate(Backend backend, size_t size)
{
    if (backend == Backend::Pool)
        return MemoryPool::allocate(size);
    if (backend == Backend::NewDelete)
        return static_cast<void*>(new (std::nothrow) char[size]);
    return std::malloc(size);
}

void backendFree(Backend backend, void* ptr, size_t size)
{
    if (backend == Backend::Pool)
        MemoryPool::deallocate(ptr, size);
    else if (backend == Backend::NewDelete)
        delete[] static_cast<char*>(ptr);
    else
        std::free(ptr);
}

class Timer
{
public:
    Timer() : start_(Clock::now()) {}

    double elapsedMs() const
    {
        const auto end = Clock::now();
        return std::chrono::duration_cast<std::chrono::microseconds>(end - start_).count() / 1000.0;
    }

private:
    Clock::time_point start_;
};

double median(std::vector<double> values)
{
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

struct ScenarioResult
{
    std::vector<double> memPoolRuns;
    std::vector<double> systemRuns;
    std::vector<double> mallocRuns;
};

void printRunSummary(const std::string& name, const ScenarioResult& result)
{
    std::cout << "\n" << name << '\n';
    std::cout << "  Memory Pool runs:";
    for (double value : result.memPoolRuns)
        std::cout << ' ' << std::fixed << std::setprecision(3) << value;
    std::cout << " ms\n";

    std::cout << "  New/Delete runs:";
    for (double value : result.systemRuns)
        std::cout << ' ' << std::fixed << std::setprecision(3) << value;
    std::cout << " ms\n";

    if (!result.mallocRuns.empty())
    {
        std::cout << "  Malloc/Free runs:";
        for (double value : result.mallocRuns)
            std::cout << ' ' << std::fixed << std::setprecision(3) << value;
        std::cout << " ms\n";
    }

    std::cout << "  Median: pool " << median(result.memPoolRuns)
              << " ms vs system " << median(result.systemRuns) << " ms\n";
    if (!result.mallocRuns.empty())
        std::cout << "  Median: malloc " << median(result.mallocRuns) << " ms\n";
}

double runMallocSmallObjects()
{
    constexpr size_t kAllocations = 100000;
    constexpr size_t kObjectSize = 32;
    Timer timer;
    std::vector<void*> ptrs;
    ptrs.reserve(kAllocations);
    for (size_t i = 0; i < kAllocations; ++i)
    {
        void* ptr = std::malloc(kObjectSize);
        if (!ptr)
            return -1.0;
        ptrs.push_back(ptr);
        if ((i & 3u) == 0)
        {
            std::free(ptrs.back());
            ptrs.pop_back();
        }
    }
    for (void* ptr : ptrs)
        std::free(ptr);
    return timer.elapsedMs();
}

double runMallocMultiThreaded(uint32_t seedBase)
{
    constexpr size_t kThreadCount = 4;
    constexpr size_t kAllocationsPerThread = 25000;
    Timer timer;
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);
    for (size_t tid = 0; tid < kThreadCount; ++tid)
    {
        threads.emplace_back([seed = seedBase + static_cast<uint32_t>(tid),
                              kAllocationsPerThread]() {
            std::mt19937 rng(seed);
            std::uniform_int_distribution<size_t> sizeDist(8, 256);
            std::uniform_int_distribution<int> releaseDist(0, 99);
            std::vector<std::pair<void*, size_t>> ptrs;
            ptrs.reserve(kAllocationsPerThread);
            for (size_t i = 0; i < kAllocationsPerThread; ++i)
            {
                const size_t size = sizeDist(rng);
                void* ptr = std::malloc(size);
                if (!ptr)
                    continue;
                ptrs.emplace_back(ptr, size);
                if (releaseDist(rng) < 75)
                {
                    std::uniform_int_distribution<size_t> pick(0, ptrs.size() - 1);
                    const size_t index = pick(rng);
                    std::free(ptrs[index].first);
                    ptrs[index] = ptrs.back();
                    ptrs.pop_back();
                }
            }
            for (const auto& item : ptrs)
                std::free(item.first);
        });
    }
    for (auto& thread : threads)
        thread.join();
    return timer.elapsedMs();
}

double runMallocMixedSizes(uint32_t seed)
{
    constexpr size_t kAllocations = 50000;
    constexpr std::array<size_t, 8> kSizes{16, 32, 64, 128, 256, 512, 1024, 2048};
    std::mt19937 rng(seed);
    std::uniform_int_distribution<size_t> pick(0, kSizes.size() - 1);
    Timer timer;
    std::vector<std::pair<void*, size_t>> ptrs;
    ptrs.reserve(kAllocations);
    for (size_t i = 0; i < kAllocations; ++i)
    {
        const size_t size = kSizes[pick(rng)];
        void* ptr = std::malloc(size);
        if (!ptr)
            return -1.0;
        ptrs.emplace_back(ptr, size);
        if (i % 100 == 0)
        {
            const size_t releaseCount = std::min(ptrs.size(), size_t(20));
            for (size_t j = 0; j < releaseCount; ++j)
            {
                std::free(ptrs.back().first);
                ptrs.pop_back();
            }
        }
    }
    for (const auto& item : ptrs)
        std::free(item.first);
    return timer.elapsedMs();
}

double runScalabilityCase(Backend backend, size_t objectSize, size_t threadCount)
{
    constexpr size_t kOperationsPerThread = 25000;
    constexpr size_t kLiveBatch = 256;
    Timer timer;
    std::vector<std::thread> threads;
    threads.reserve(threadCount);
    for (size_t tid = 0; tid < threadCount; ++tid)
    {
        threads.emplace_back([backend, objectSize,
                              kLiveBatch, kOperationsPerThread]() {
            std::vector<void*> live;
            live.reserve(kLiveBatch);
            for (size_t i = 0; i < kOperationsPerThread; ++i)
            {
                void* ptr = backendAllocate(backend, objectSize);
                if (!ptr)
                    continue;
                auto* bytes = static_cast<unsigned char*>(ptr);
                bytes[0] = static_cast<unsigned char>(i);
                bytes[objectSize - 1] = static_cast<unsigned char>(i >> 8);
                live.push_back(ptr);
                if (live.size() == kLiveBatch)
                {
                    for (void* item : live)
                        backendFree(backend, item, objectSize);
                    live.clear();
                }
            }
            for (void* item : live)
                backendFree(backend, item, objectSize);
        });
    }
    for (auto& thread : threads)
        thread.join();
    return timer.elapsedMs();
}

void printScalabilityMatrix()
{
    constexpr std::array<size_t, 7> kSizes{16, 32, 64, 256, 1024, 4096, 64 * 1024};
    constexpr std::array<size_t, 4> kThreads{1, 2, 4, 8};
    std::cout << "\nScalability matrix (25000 operations per thread, median of 3)\n";
    std::cout << "  size threads       pool        new     malloc   pool/new pool/malloc\n";
    for (size_t size : kSizes)
    {
        for (size_t threadCount : kThreads)
        {
            std::vector<double> poolRuns;
            std::vector<double> newRuns;
            std::vector<double> mallocRuns;
            for (size_t repeat = 0; repeat < 3; ++repeat)
            {
                poolRuns.push_back(runScalabilityCase(Backend::Pool, size, threadCount));
                newRuns.push_back(runScalabilityCase(Backend::NewDelete, size, threadCount));
                mallocRuns.push_back(runScalabilityCase(Backend::MallocFree, size, threadCount));
            }
            const double poolMs = median(poolRuns);
            const double newMs = median(newRuns);
            const double mallocMs = median(mallocRuns);
            std::cout << std::setw(6) << size << std::setw(8) << threadCount
                      << std::setw(11) << std::fixed << std::setprecision(3) << poolMs
                      << std::setw(11) << newMs << std::setw(11) << mallocMs
                      << std::setw(11) << poolMs / newMs
                      << std::setw(12) << poolMs / mallocMs << '\n';
        }
    }
}

double runCrossThreadCase(Backend backend, size_t objectSize)
{
    constexpr size_t kOperations = 50000;
    std::vector<void*> pointers;
    pointers.reserve(kOperations);
    Timer timer;
    std::thread producer([&]() {
        for (size_t i = 0; i < kOperations; ++i)
        {
            void* ptr = backendAllocate(backend, objectSize);
            if (ptr)
                pointers.push_back(ptr);
        }
    });
    producer.join();
    std::thread consumer([&]() {
        for (void* ptr : pointers)
            backendFree(backend, ptr, objectSize);
    });
    consumer.join();
    return timer.elapsedMs();
}

void printCrossThreadComparison()
{
    std::vector<double> poolRuns;
    std::vector<double> newRuns;
    std::vector<double> mallocRuns;
    for (size_t repeat = 0; repeat < 5; ++repeat)
    {
        poolRuns.push_back(runCrossThreadCase(Backend::Pool, 64));
        newRuns.push_back(runCrossThreadCase(Backend::NewDelete, 64));
        mallocRuns.push_back(runCrossThreadCase(Backend::MallocFree, 64));
    }
    const double poolMs = median(poolRuns);
    const double newMs = median(newRuns);
    const double mallocMs = median(mallocRuns);
    std::cout << "\nCross-thread ownership transfer (64B x 50000, median of 5)\n"
              << "  pool=" << poolMs << " ms new=" << newMs
              << " ms malloc=" << mallocMs << " ms\n"
              << "  pool/new=" << poolMs / newMs
              << " pool/malloc=" << poolMs / mallocMs << '\n';
}

void warmup()
{
    std::vector<std::pair<void*, size_t>> ptrs;
    ptrs.reserve(5000);
    for (int i = 0; i < 1000; ++i)
    {
        for (size_t size : {16u, 32u, 64u, 128u, 256u})
        {
            void* p = MemoryPool::allocate(size);
            if (p)
                ptrs.emplace_back(p, size);
        }
    }

    for (const auto& [ptr, size] : ptrs)
        MemoryPool::deallocate(ptr, size);
}

double runSmallObjects(bool useMemPool)
{
    constexpr size_t kAllocations = 100000;
    constexpr size_t kObjectSize = 32;

    Timer timer;
    std::vector<void*> ptrs;
    ptrs.reserve(kAllocations);

    for (size_t i = 0; i < kAllocations; ++i)
    {
        void* ptr = useMemPool ? MemoryPool::allocate(kObjectSize)
                               : static_cast<void*>(new char[kObjectSize]);
        ptrs.push_back(ptr);
        if ((i & 3u) == 0)
        {
            if (useMemPool)
                MemoryPool::deallocate(ptrs.back(), kObjectSize);
            else
                delete[] static_cast<char*>(ptrs.back());
            ptrs.pop_back();
        }
    }

    for (void* ptr : ptrs)
    {
        if (useMemPool)
            MemoryPool::deallocate(ptr, kObjectSize);
        else
            delete[] static_cast<char*>(ptr);
    }

    return timer.elapsedMs();
}

double runMixedSizes(bool useMemPool, uint32_t seed)
{
    constexpr size_t kAllocations = 50000;
    constexpr std::array<size_t, 8> kSizes{16, 32, 64, 128, 256, 512, 1024, 2048};

    std::mt19937 rng(seed);
    std::uniform_int_distribution<size_t> pick(0, kSizes.size() - 1);

    Timer timer;
    std::vector<std::pair<void*, size_t>> ptrs;
    ptrs.reserve(kAllocations);

    for (size_t i = 0; i < kAllocations; ++i)
    {
        const size_t size = kSizes[pick(rng)];
        void* ptr = useMemPool ? MemoryPool::allocate(size)
                               : static_cast<void*>(new char[size]);
        ptrs.emplace_back(ptr, size);

        if (i % 100 == 0 && !ptrs.empty())
        {
            const size_t releaseCount = std::min(ptrs.size(), size_t(20));
            for (size_t j = 0; j < releaseCount; ++j)
            {
                auto [lastPtr, lastSize] = ptrs.back();
                if (useMemPool)
                    MemoryPool::deallocate(lastPtr, lastSize);
                else
                    delete[] static_cast<char*>(lastPtr);
                ptrs.pop_back();
            }
        }
    }

    for (const auto& [ptr, size] : ptrs)
    {
        if (useMemPool)
            MemoryPool::deallocate(ptr, size);
        else
            delete[] static_cast<char*>(ptr);
    }

    return timer.elapsedMs();
}

double runMultiThreaded(bool useMemPool, uint32_t seedBase)
{
    constexpr size_t kThreadCount = 4;
    constexpr size_t kAllocationsPerThread = 25000;

    Timer timer;
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);

    for (size_t tid = 0; tid < kThreadCount; ++tid)
    {
        threads.emplace_back([useMemPool,
                              seed = seedBase + static_cast<uint32_t>(tid),
                              kAllocationsPerThread]() {
            std::mt19937 rng(seed);
            std::uniform_int_distribution<size_t> sizeDist(8, 256);
            std::uniform_int_distribution<int> releaseDist(0, 99);
            std::vector<std::pair<void*, size_t>> ptrs;
            ptrs.reserve(kAllocationsPerThread);

            for (size_t i = 0; i < kAllocationsPerThread; ++i)
            {
                const size_t size = sizeDist(rng);
                void* ptr = useMemPool ? MemoryPool::allocate(size)
                                       : static_cast<void*>(new char[size]);
                ptrs.emplace_back(ptr, size);

                if (!ptrs.empty() && releaseDist(rng) < 75)
                {
                    std::uniform_int_distribution<size_t> pickIndex(0, ptrs.size() - 1);
                    const size_t index = pickIndex(rng);
                    auto [oldPtr, oldSize] = ptrs[index];
                    if (useMemPool)
                        MemoryPool::deallocate(oldPtr, oldSize);
                    else
                        delete[] static_cast<char*>(oldPtr);
                    ptrs[index] = ptrs.back();
                    ptrs.pop_back();
                }
            }

            for (const auto& [ptr, size] : ptrs)
            {
                if (useMemPool)
                    MemoryPool::deallocate(ptr, size);
                else
                    delete[] static_cast<char*>(ptr);
            }
        });
    }

    for (auto& thread : threads)
        thread.join();

    return timer.elapsedMs();
}

ScenarioResult measureScenario(double (*memPoolRun)(bool),
                               double (*systemRun)(bool),
                               size_t repeats)
{
    ScenarioResult result;
    result.memPoolRuns.reserve(repeats);
    result.systemRuns.reserve(repeats);

    for (size_t i = 0; i < repeats; ++i)
    {
        result.memPoolRuns.push_back(memPoolRun(true));
        result.systemRuns.push_back(systemRun(false));
    }

    return result;
}

} // namespace

int main()
{
    constexpr size_t kRepeats = 5;

    std::cout << "Starting performance tests...\n";
    warmup();

    ScenarioResult small = measureScenario(runSmallObjects,
                                           runSmallObjects,
                                           kRepeats);
    for (size_t i = 0; i < kRepeats; ++i)
        small.mallocRuns.push_back(runMallocSmallObjects());
    printRunSummary("Small objects (32B x 100000)", small);

    ScenarioResult multi;
    for (size_t i = 0; i < kRepeats; ++i)
    {
        multi.memPoolRuns.push_back(runMultiThreaded(true, 0x1000u + static_cast<uint32_t>(i * 17)));
        multi.systemRuns.push_back(runMultiThreaded(false, 0x1000u + static_cast<uint32_t>(i * 17)));
        multi.mallocRuns.push_back(runMallocMultiThreaded(0x1000u + static_cast<uint32_t>(i * 17)));
    }
    printRunSummary("Multi-threaded (4 threads x 25000)", multi);

    ScenarioResult mixed;
    for (size_t i = 0; i < kRepeats; ++i)
    {
        mixed.memPoolRuns.push_back(runMixedSizes(true, 0x2000u + static_cast<uint32_t>(i * 17)));
        mixed.systemRuns.push_back(runMixedSizes(false, 0x2000u + static_cast<uint32_t>(i * 17)));
        mixed.mallocRuns.push_back(runMallocMixedSizes(0x2000u + static_cast<uint32_t>(i * 17)));
    }
    printRunSummary("Mixed sizes (50000 allocs)", mixed);

    printScalabilityMatrix();
    printCrossThreadComparison();

    const MemoryPoolStats stats = MemoryPool::stats();
    std::cout << "\nPool stats:\n"
              << "  allocCount=" << stats.allocCount
              << " freeCount=" << stats.freeCount
              << " liveAllocs=" << stats.liveAllocs << '\n'
              << "  smallAllocCount=" << stats.smallAllocCount
              << " largeAllocCount=" << stats.largeAllocCount << '\n'
              << "  sizedFreeCount=" << stats.sizedFreeCount
              << " unsizedFreeCount=" << stats.unsizedFreeCount << '\n'
              << "  centralRefillCount=" << stats.centralRefillCount
              << " centralFlushCount=" << stats.centralFlushCount << '\n'
              << "  reservedBytes=" << stats.reservedBytes
              << " cachedPageBytes=" << stats.cachedPageBytes << '\n';

    return 0;
}
