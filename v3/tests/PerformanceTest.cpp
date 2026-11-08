#include <kama/Allocator.h>
#include <kama/MemoryPool.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace Kama_memoryPool;
using Clock = std::chrono::steady_clock;

namespace
{

enum class Backend { Pool, NewDelete, MallocFree };
enum class FreeMode { Sized, Unsized };
enum class Verdict { Win, Tie, Lose };

constexpr double kTieTolerance = 0.05; // within 5% counts as tie (noise)

struct BenchResult
{
    std::string name;
    double poolMs = 0;
    double newMs = 0;
    double mallocMs = 0;
    Verdict vsNew = Verdict::Tie;
    Verdict vsMalloc = Verdict::Tie;
};

struct SuiteSummary
{
    std::vector<BenchResult> results;
    int wins = 0;
    int ties = 0;
    int losses = 0;
};

void* backendAllocate(Backend backend, size_t size)
{
    if (backend == Backend::Pool)
        return MemoryPool::allocate(size);
    if (backend == Backend::NewDelete)
        return static_cast<void*>(new (std::nothrow) char[size]);
    return std::malloc(size);
}

void backendFree(Backend backend, FreeMode mode, void* ptr, size_t size)
{
    if (backend == Backend::Pool)
    {
        if (mode == FreeMode::Sized)
            MemoryPool::deallocate(ptr, size);
        else
            MemoryPool::deallocate(ptr);
        return;
    }
    if (backend == Backend::NewDelete)
    {
        delete[] static_cast<char*>(ptr);
        return;
    }
    std::free(ptr);
}

class Timer
{
public:
    Timer() : start_(Clock::now()) {}

    double elapsedMs() const
    {
        const auto end = Clock::now();
        return std::chrono::duration<double, std::milli>(end - start_).count();
    }

private:
    Clock::time_point start_;
};

double median(std::vector<double> values)
{
    if (values.empty())
        return 0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

Verdict compare(double poolMs, double baselineMs)
{
    if (baselineMs <= 0)
        return Verdict::Tie;
    if (poolMs < baselineMs * (1.0 - kTieTolerance))
        return Verdict::Win;
    if (poolMs > baselineMs * (1.0 + kTieTolerance))
        return Verdict::Lose;
    return Verdict::Tie;
}

const char* verdictText(Verdict v)
{
    switch (v)
    {
    case Verdict::Win: return "WIN";
    case Verdict::Tie: return "TIE";
    case Verdict::Lose: return "LOSE";
    }
    return "?";
}

void touch(void* ptr, size_t size, size_t salt)
{
    auto* bytes = static_cast<unsigned char*>(ptr);
    bytes[0] = static_cast<unsigned char>(salt);
    if (size > 1)
        bytes[size - 1] = static_cast<unsigned char>(salt >> 8);
}

void warmup()
{
    std::vector<std::pair<void*, size_t>> ptrs;
    ptrs.reserve(12000);
    for (int round = 0; round < 4; ++round)
    {
        for (size_t size : {16u, 32u, 64u, 96u, 128u, 256u, 512u, 1024u, 2048u, 4096u})
        {
            for (int i = 0; i < 300; ++i)
            {
                void* p = MemoryPool::allocate(size);
                if (p)
                    ptrs.emplace_back(p, size);
            }
        }
        for (const auto& item : ptrs)
            MemoryPool::deallocate(item.first, item.second);
        ptrs.clear();
    }

    // Warm mixed-size MT pattern (8..256) used by core/adversarial suites.
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t)
        {
            threads.emplace_back([] {
                std::vector<void*> local;
                local.reserve(4096);
                for (int i = 0; i < 8000; ++i)
                {
                    const size_t size = 8u + static_cast<size_t>(i % 249);
                    void* p = MemoryPool::allocate(size);
                    if (p)
                        local.push_back(p);
                    if (local.size() >= 64)
                    {
                        for (void* q : local)
                            MemoryPool::deallocate(q);
                        local.clear();
                    }
                }
                for (void* q : local)
                    MemoryPool::deallocate(q);
            });
        }
        for (auto& th : threads)
            th.join();
    }

    // Warm system allocators too so first-run noise is lower.
    std::vector<void*> sys;
    sys.reserve(4000);
    for (int i = 0; i < 4000; ++i)
    {
        void* p = std::malloc(64);
        if (p)
            sys.push_back(p);
    }
    for (void* p : sys)
        std::free(p);
    for (int i = 0; i < 2000; ++i)
    {
        void* p = std::malloc(1024);
        if (p)
            std::free(p);
    }
}

template <typename Fn>
double measureMedian(size_t repeats, Fn&& fn)
{
    std::vector<double> runs;
    runs.reserve(repeats);
    for (size_t i = 0; i < repeats; ++i)
        runs.push_back(fn());
    return median(std::move(runs));
}

BenchResult makeResult(std::string name, double poolMs, double newMs, double mallocMs)
{
    BenchResult r;
    r.name = std::move(name);
    r.poolMs = poolMs;
    r.newMs = newMs;
    r.mallocMs = mallocMs;
    r.vsNew = compare(poolMs, newMs);
    r.vsMalloc = compare(poolMs, mallocMs);
    return r;
}

void record(SuiteSummary& suite, BenchResult result)
{
    if (result.vsNew == Verdict::Lose || result.vsMalloc == Verdict::Lose)
        ++suite.losses;
    else if (result.vsNew == Verdict::Win && result.vsMalloc == Verdict::Win)
        ++suite.wins;
    else
        ++suite.ties;
    suite.results.push_back(std::move(result));
}

void printResult(const BenchResult& r)
{
    const double vsMallocX = (r.poolMs > 0.0) ? (r.mallocMs / r.poolMs) : 0.0;
    const double vsNewX = (r.poolMs > 0.0) ? (r.newMs / r.poolMs) : 0.0;
    std::cout << "  " << std::left << std::setw(42) << r.name
              << std::right << std::fixed << std::setprecision(3)
              << " pool=" << std::setw(8) << r.poolMs
              << " new=" << std::setw(8) << r.newMs
              << " malloc=" << std::setw(8) << r.mallocMs
              << "  vs_new=" << verdictText(r.vsNew)
              << " vs_malloc=" << verdictText(r.vsMalloc)
              << "  x_new=" << std::setprecision(2) << vsNewX
              << " x_malloc=" << vsMallocX << '\n';
}

// ---------------------------------------------------------------------------
// Workloads
// ---------------------------------------------------------------------------

double runSameThreadFixed(Backend backend, FreeMode mode, size_t objectSize, size_t count)
{
    Timer timer;
    std::vector<void*> ptrs;
    ptrs.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        void* ptr = backendAllocate(backend, objectSize);
        if (!ptr)
            continue;
        touch(ptr, objectSize, i);
        ptrs.push_back(ptr);
        if ((i & 3u) == 0)
        {
            backendFree(backend, mode, ptrs.back(), objectSize);
            ptrs.pop_back();
        }
    }
    for (void* ptr : ptrs)
        backendFree(backend, mode, ptr, objectSize);
    return timer.elapsedMs();
}

double runMixedSizes(Backend backend, FreeMode mode, uint32_t seed, size_t count)
{
    constexpr std::array<size_t, 8> kSizes{16, 32, 64, 128, 256, 512, 1024, 2048};
    std::mt19937 rng(seed);
    std::uniform_int_distribution<size_t> pick(0, kSizes.size() - 1);
    Timer timer;
    std::vector<std::pair<void*, size_t>> ptrs;
    ptrs.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        const size_t size = kSizes[pick(rng)];
        void* ptr = backendAllocate(backend, size);
        if (!ptr)
            continue;
        touch(ptr, size, i);
        ptrs.emplace_back(ptr, size);
        if (i % 100 == 0 && !ptrs.empty())
        {
            const size_t releaseCount = std::min(ptrs.size(), size_t(20));
            for (size_t j = 0; j < releaseCount; ++j)
            {
                backendFree(backend, mode, ptrs.back().first, ptrs.back().second);
                ptrs.pop_back();
            }
        }
    }
    for (const auto& item : ptrs)
        backendFree(backend, mode, item.first, item.second);
    return timer.elapsedMs();
}

double runMultiThreaded(Backend backend, FreeMode mode, uint32_t seedBase,
                        size_t threadCount, size_t perThread)
{
    Timer timer;
    std::vector<std::thread> threads;
    threads.reserve(threadCount);
    for (size_t tid = 0; tid < threadCount; ++tid)
    {
        threads.emplace_back([=]() {
            std::mt19937 rng(seedBase + static_cast<uint32_t>(tid));
            std::uniform_int_distribution<size_t> sizeDist(8, 256);
            std::uniform_int_distribution<int> releaseDist(0, 99);
            std::vector<std::pair<void*, size_t>> ptrs;
            ptrs.reserve(perThread);
            for (size_t i = 0; i < perThread; ++i)
            {
                const size_t size = sizeDist(rng);
                void* ptr = backendAllocate(backend, size);
                if (!ptr)
                    continue;
                ptrs.emplace_back(ptr, size);
                if (!ptrs.empty() && releaseDist(rng) < 75)
                {
                    std::uniform_int_distribution<size_t> pick(0, ptrs.size() - 1);
                    const size_t index = pick(rng);
                    backendFree(backend, mode, ptrs[index].first, ptrs[index].second);
                    ptrs[index] = ptrs.back();
                    ptrs.pop_back();
                }
            }
            for (const auto& item : ptrs)
                backendFree(backend, mode, item.first, item.second);
        });
    }
    for (auto& thread : threads)
        thread.join();
    return timer.elapsedMs();
}

double runScalability(Backend backend, FreeMode mode, size_t objectSize,
                      size_t threadCount, size_t opsPerThread)
{
    constexpr size_t kLiveBatch = 256;
    Timer timer;
    std::vector<std::thread> threads;
    threads.reserve(threadCount);
    for (size_t tid = 0; tid < threadCount; ++tid)
    {
        threads.emplace_back([=]() {
            std::vector<void*> live;
            live.reserve(kLiveBatch);
            for (size_t i = 0; i < opsPerThread; ++i)
            {
                void* ptr = backendAllocate(backend, objectSize);
                if (!ptr)
                    continue;
                touch(ptr, objectSize, i);
                live.push_back(ptr);
                if (live.size() == kLiveBatch)
                {
                    for (void* item : live)
                        backendFree(backend, mode, item, objectSize);
                    live.clear();
                }
            }
            for (void* item : live)
                backendFree(backend, mode, item, objectSize);
        });
    }
    for (auto& thread : threads)
        thread.join();
    return timer.elapsedMs();
}

double runCrossThreadTransfer(Backend backend, FreeMode mode, size_t objectSize, size_t count)
{
    std::vector<void*> pointers;
    pointers.reserve(count);
    Timer timer;
    std::thread producer([&]() {
        for (size_t i = 0; i < count; ++i)
        {
            void* ptr = backendAllocate(backend, objectSize);
            if (ptr)
                pointers.push_back(ptr);
        }
    });
    producer.join();
    std::thread consumer([&]() {
        for (void* ptr : pointers)
            backendFree(backend, mode, ptr, objectSize);
    });
    consumer.join();
    return timer.elapsedMs();
}

double runProducerConsumerPipeline(Backend backend, FreeMode mode,
                                   size_t objectSize, size_t totalOps)
{
    constexpr size_t kQueueCapacity = 4096;
    std::vector<void*> queue;
    queue.reserve(kQueueCapacity);
    size_t head = 0;
    size_t tail = 0;
    size_t count = 0;
    bool done = false;
    std::mutex mu;
    std::condition_variable cv;

    Timer timer;
    std::thread producer([&]() {
        for (size_t i = 0; i < totalOps; ++i)
        {
            void* ptr = backendAllocate(backend, objectSize);
            if (!ptr)
                continue;
            touch(ptr, objectSize, i);
            std::unique_lock<std::mutex> lock(mu);
            cv.wait(lock, [&]() { return count < kQueueCapacity; });
            if (queue.size() < kQueueCapacity)
                queue.push_back(ptr);
            else
                queue[tail] = ptr;
            tail = (tail + 1) % kQueueCapacity;
            ++count;
            lock.unlock();
            cv.notify_all();
        }
        {
            std::lock_guard<std::mutex> lock(mu);
            done = true;
        }
        cv.notify_all();
    });

    std::thread consumer([&]() {
        for (;;)
        {
            void* ptr = nullptr;
            {
                std::unique_lock<std::mutex> lock(mu);
                cv.wait(lock, [&]() { return count > 0 || done; });
                if (count == 0 && done)
                    break;
                ptr = queue[head];
                head = (head + 1) % kQueueCapacity;
                --count;
            }
            cv.notify_all();
            backendFree(backend, mode, ptr, objectSize);
        }
    });

    producer.join();
    consumer.join();
    return timer.elapsedMs();
}

double runHighRetention(Backend backend, FreeMode mode, size_t objectSize, size_t count)
{
    Timer timer;
    std::vector<void*> ptrs;
    ptrs.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        void* ptr = backendAllocate(backend, objectSize);
        if (!ptr)
            continue;
        touch(ptr, objectSize, i);
        ptrs.push_back(ptr);
    }
    for (void* ptr : ptrs)
        backendFree(backend, mode, ptr, objectSize);
    return timer.elapsedMs();
}

double runAlignedAlloc(Backend backend, size_t size, size_t alignment, size_t count)
{
    Timer timer;
    std::vector<void*> ptrs;
    ptrs.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        void* ptr = nullptr;
        if (backend == Backend::Pool)
            ptr = MemoryPool::allocateAligned(size, alignment);
        else if (backend == Backend::NewDelete)
            ptr = static_cast<void*>(new (std::nothrow) char[size]);
        else
            ptr = std::malloc(size);

        if (!ptr)
            continue;
        touch(ptr, size, i);
        ptrs.push_back(ptr);
    }

    for (void* ptr : ptrs)
    {
        if (backend == Backend::Pool)
            MemoryPool::deallocate(ptr);
        else if (backend == Backend::NewDelete)
            delete[] static_cast<char*>(ptr);
        else
            std::free(ptr);
    }
    return timer.elapsedMs();
}

double runLargeObjects(Backend backend, FreeMode mode, size_t objectSize, size_t count)
{
    Timer timer;
    std::vector<void*> ptrs;
    ptrs.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        void* ptr = backendAllocate(backend, objectSize);
        if (!ptr)
            continue;
        touch(ptr, objectSize, i);
        ptrs.push_back(ptr);
        if (ptrs.size() >= 8)
        {
            for (void* p : ptrs)
                backendFree(backend, mode, p, objectSize);
            ptrs.clear();
        }
    }
    for (void* p : ptrs)
        backendFree(backend, mode, p, objectSize);
    return timer.elapsedMs();
}

double runLargeObjectsPageTouch(Backend backend, FreeMode mode, size_t objectSize, size_t count)
{
    Timer timer;
    volatile unsigned checksum = 0;
    const size_t stride = 4096;
    for (size_t i = 0; i < count; ++i)
    {
        void* ptr = backendAllocate(backend, objectSize);
        if (!ptr)
            continue;
        auto* bytes = static_cast<unsigned char*>(ptr);
        for (size_t offset = 0; offset < objectSize; offset += stride)
        {
            bytes[offset] = static_cast<unsigned char>(i + offset);
            checksum += bytes[offset];
        }
        backendFree(backend, mode, ptr, objectSize);
    }
    (void)checksum;
    return timer.elapsedMs();
}

double runLargeObjectsFullWrite(Backend backend, FreeMode mode, size_t objectSize, size_t count)
{
    Timer timer;
    volatile unsigned checksum = 0;
    for (size_t i = 0; i < count; ++i)
    {
        void* ptr = backendAllocate(backend, objectSize);
        if (!ptr)
            continue;
        std::memset(ptr, static_cast<int>(i), objectSize);
        checksum += static_cast<unsigned>(static_cast<unsigned char*>(ptr)[i % objectSize]);
        backendFree(backend, mode, ptr, objectSize);
    }
    (void)checksum;
    return timer.elapsedMs();
}

double runLargeObjectsWithTrim(size_t objectSize, size_t count)
{
    Timer timer;
    for (size_t i = 0; i < count; ++i)
    {
        void* ptr = MemoryPool::allocate(objectSize);
        if (!ptr)
            continue;
        auto* bytes = static_cast<unsigned char*>(ptr);
        bytes[0] = static_cast<unsigned char>(i);
        bytes[objectSize - 1] = static_cast<unsigned char>(i >> 8);
        MemoryPool::deallocate(ptr, objectSize);
        MemoryPool::trim();
    }
    return timer.elapsedMs();
}

double runStlAllocator(Backend backend, size_t iterations)
{
    // Direct Allocator / malloc traffic with the same sized pattern vector would
    // use after reserve(n). Avoids std::vector differences across allocators.
    constexpr size_t kCount = 256;
    Timer timer;
    unsigned checksum = 0;
    if (backend == Backend::Pool)
    {
        Allocator<int> alloc;
        for (size_t n = 0; n < iterations; ++n)
        {
            int* values = alloc.allocate(kCount);
            for (size_t i = 0; i < kCount; ++i)
                values[i] = static_cast<int>(i + n);
            checksum += static_cast<unsigned>(values[kCount - 1]);
            alloc.deallocate(values, kCount);
        }
    }
    else if (backend == Backend::NewDelete)
    {
        for (size_t n = 0; n < iterations; ++n)
        {
            int* values = new int[kCount];
            for (size_t i = 0; i < kCount; ++i)
                values[i] = static_cast<int>(i + n);
            checksum += static_cast<unsigned>(values[kCount - 1]);
            delete[] values;
        }
    }
    else
    {
        for (size_t n = 0; n < iterations; ++n)
        {
            auto* values = static_cast<int*>(std::malloc(sizeof(int) * kCount));
            for (size_t i = 0; i < kCount; ++i)
                values[i] = static_cast<int>(i + n);
            checksum += static_cast<unsigned>(values[kCount - 1]);
            std::free(values);
        }
    }
    volatile unsigned sink = checksum;
    (void)sink;
    return timer.elapsedMs();
}

// ---------------------------------------------------------------------------
// Suites
// ---------------------------------------------------------------------------

void runCoreSuite(SuiteSummary& suite, size_t repeats)
{
    std::cout << "\n== Core throughput ==\n";

    {
        auto pool = measureMedian(repeats, [] {
            return runSameThreadFixed(Backend::Pool, FreeMode::Sized, 32, 100000);
        });
        auto neu = measureMedian(repeats, [] {
            return runSameThreadFixed(Backend::NewDelete, FreeMode::Sized, 32, 100000);
        });
        auto mal = measureMedian(repeats, [] {
            return runSameThreadFixed(Backend::MallocFree, FreeMode::Sized, 32, 100000);
        });
        auto r = makeResult("same-thread 32B sized x100000", pool, neu, mal);
        printResult(r);
        record(suite, r);
    }

    {
        auto pool = measureMedian(repeats, [] {
            return runSameThreadFixed(Backend::Pool, FreeMode::Unsized, 32, 100000);
        });
        auto neu = measureMedian(repeats, [] {
            return runSameThreadFixed(Backend::NewDelete, FreeMode::Unsized, 32, 100000);
        });
        auto mal = measureMedian(repeats, [] {
            return runSameThreadFixed(Backend::MallocFree, FreeMode::Unsized, 32, 100000);
        });
        auto r = makeResult("same-thread 32B unsized x100000", pool, neu, mal);
        printResult(r);
        record(suite, r);
    }

    {
        auto pool = measureMedian(repeats, [&] {
            return runMultiThreaded(Backend::Pool, FreeMode::Sized, 0x1000u, 4, 100000);
        });
        auto neu = measureMedian(repeats, [&] {
            return runMultiThreaded(Backend::NewDelete, FreeMode::Sized, 0x1000u, 4, 100000);
        });
        auto mal = measureMedian(repeats, [&] {
            return runMultiThreaded(Backend::MallocFree, FreeMode::Sized, 0x1000u, 4, 100000);
        });
        auto r = makeResult("multi-thread 4x100000 sized", pool, neu, mal);
        printResult(r);
        record(suite, r);
    }

    {
        auto pool = measureMedian(repeats, [&] {
            return runMultiThreaded(Backend::Pool, FreeMode::Unsized, 0x1100u, 4, 100000);
        });
        auto neu = measureMedian(repeats, [&] {
            return runMultiThreaded(Backend::NewDelete, FreeMode::Unsized, 0x1100u, 4, 100000);
        });
        auto mal = measureMedian(repeats, [&] {
            return runMultiThreaded(Backend::MallocFree, FreeMode::Unsized, 0x1100u, 4, 100000);
        });
        auto r = makeResult("multi-thread 4x100000 unsized", pool, neu, mal);
        printResult(r);
        record(suite, r);
    }

    {
        auto pool = measureMedian(repeats, [] {
            return runMixedSizes(Backend::Pool, FreeMode::Sized, 0x2000u, 50000);
        });
        auto neu = measureMedian(repeats, [] {
            return runMixedSizes(Backend::NewDelete, FreeMode::Sized, 0x2000u, 50000);
        });
        auto mal = measureMedian(repeats, [] {
            return runMixedSizes(Backend::MallocFree, FreeMode::Sized, 0x2000u, 50000);
        });
        auto r = makeResult("mixed 16B-2048B sized x50000", pool, neu, mal);
        printResult(r);
        record(suite, r);
    }

    {
        auto pool = measureMedian(repeats, [] {
            return runMixedSizes(Backend::Pool, FreeMode::Unsized, 0x2100u, 50000);
        });
        auto neu = measureMedian(repeats, [] {
            return runMixedSizes(Backend::NewDelete, FreeMode::Unsized, 0x2100u, 50000);
        });
        auto mal = measureMedian(repeats, [] {
            return runMixedSizes(Backend::MallocFree, FreeMode::Unsized, 0x2100u, 50000);
        });
        auto r = makeResult("mixed 16B-2048B unsized x50000", pool, neu, mal);
        printResult(r);
        record(suite, r);
    }
}

void runOwnershipSuite(SuiteSummary& suite, size_t repeats)
{
    std::cout << "\n== Ownership transfer / pipeline ==\n";

    {
        auto pool = measureMedian(repeats, [] {
            return runCrossThreadTransfer(Backend::Pool, FreeMode::Sized, 64, 50000);
        });
        auto neu = measureMedian(repeats, [] {
            return runCrossThreadTransfer(Backend::NewDelete, FreeMode::Sized, 64, 50000);
        });
        auto mal = measureMedian(repeats, [] {
            return runCrossThreadTransfer(Backend::MallocFree, FreeMode::Sized, 64, 50000);
        });
        auto r = makeResult("cross-thread transfer sized 64Bx50000", pool, neu, mal);
        printResult(r);
        record(suite, r);
    }

    {
        auto pool = measureMedian(repeats, [] {
            return runCrossThreadTransfer(Backend::Pool, FreeMode::Unsized, 64, 50000);
        });
        auto neu = measureMedian(repeats, [] {
            return runCrossThreadTransfer(Backend::NewDelete, FreeMode::Unsized, 64, 50000);
        });
        auto mal = measureMedian(repeats, [] {
            return runCrossThreadTransfer(Backend::MallocFree, FreeMode::Unsized, 64, 50000);
        });
        auto r = makeResult("cross-thread transfer unsized 64Bx50000", pool, neu, mal);
        printResult(r);
        record(suite, r);
    }

    {
        auto pool = measureMedian(repeats, [] {
            return runProducerConsumerPipeline(Backend::Pool, FreeMode::Sized, 64, 50000);
        });
        auto neu = measureMedian(repeats, [] {
            return runProducerConsumerPipeline(Backend::NewDelete, FreeMode::Sized, 64, 50000);
        });
        auto mal = measureMedian(repeats, [] {
            return runProducerConsumerPipeline(Backend::MallocFree, FreeMode::Sized, 64, 50000);
        });
        auto r = makeResult("producer-consumer pipeline sized 64Bx50000", pool, neu, mal);
        printResult(r);
        record(suite, r);
    }

    {
        auto pool = measureMedian(repeats, [] {
            return runProducerConsumerPipeline(Backend::Pool, FreeMode::Unsized, 64, 50000);
        });
        auto neu = measureMedian(repeats, [] {
            return runProducerConsumerPipeline(Backend::NewDelete, FreeMode::Unsized, 64, 50000);
        });
        auto mal = measureMedian(repeats, [] {
            return runProducerConsumerPipeline(Backend::MallocFree, FreeMode::Unsized, 64, 50000);
        });
        auto r = makeResult("producer-consumer pipeline unsized 64Bx50000", pool, neu, mal);
        printResult(r);
        record(suite, r);
    }
}

void runShapeSuite(SuiteSummary& suite, size_t repeats)
{
    std::cout << "\n== Retention / aligned / large / STL ==\n";

    {
        auto pool = measureMedian(repeats, [] {
            return runHighRetention(Backend::Pool, FreeMode::Sized, 64, 100000);
        });
        auto neu = measureMedian(repeats, [] {
            return runHighRetention(Backend::NewDelete, FreeMode::Sized, 64, 100000);
        });
        auto mal = measureMedian(repeats, [] {
            return runHighRetention(Backend::MallocFree, FreeMode::Sized, 64, 100000);
        });
        auto r = makeResult("high-retention 64B x100000", pool, neu, mal);
        printResult(r);
        record(suite, r);
    }

    {
        auto pool = measureMedian(repeats, [] {
            return runAlignedAlloc(Backend::Pool, 96, 64, 50000);
        });
        auto neu = measureMedian(repeats, [] {
            return runAlignedAlloc(Backend::NewDelete, 96, 64, 50000);
        });
        auto mal = measureMedian(repeats, [] {
            return runAlignedAlloc(Backend::MallocFree, 96, 64, 50000);
        });
        auto r = makeResult("aligned 96B/64-align x50000", pool, neu, mal);
        printResult(r);
        record(suite, r);
    }

    {
        auto pool = measureMedian(repeats, [] {
            return runLargeObjects(Backend::Pool, FreeMode::Sized, 512 * 1024, 200);
        });
        auto neu = measureMedian(repeats, [] {
            return runLargeObjects(Backend::NewDelete, FreeMode::Sized, 512 * 1024, 200);
        });
        auto mal = measureMedian(repeats, [] {
            return runLargeObjects(Backend::MallocFree, FreeMode::Sized, 512 * 1024, 200);
        });
        auto r = makeResult("large-object 512KB x200 (batch8)", pool, neu, mal);
        printResult(r);
        record(suite, r);
    }

    {
        auto pool = measureMedian(repeats, [] {
            return runLargeObjectsPageTouch(Backend::Pool, FreeMode::Sized, 512 * 1024, 100);
        });
        auto neu = measureMedian(repeats, [] {
            return runLargeObjectsPageTouch(Backend::NewDelete, FreeMode::Sized, 512 * 1024, 100);
        });
        auto mal = measureMedian(repeats, [] {
            return runLargeObjectsPageTouch(Backend::MallocFree, FreeMode::Sized, 512 * 1024, 100);
        });
        auto r = makeResult("large-object 512KB page-touch x100", pool, neu, mal);
        printResult(r);
        record(suite, r);
    }

    {
        auto pool = measureMedian(repeats, [] {
            return runLargeObjectsPageTouch(Backend::Pool, FreeMode::Sized, 2 * 1024 * 1024, 32);
        });
        auto neu = measureMedian(repeats, [] {
            return runLargeObjectsPageTouch(Backend::NewDelete, FreeMode::Sized, 2 * 1024 * 1024, 32);
        });
        auto mal = measureMedian(repeats, [] {
            return runLargeObjectsPageTouch(Backend::MallocFree, FreeMode::Sized, 2 * 1024 * 1024, 32);
        });
        auto r = makeResult("large-object 2MB page-touch x32", pool, neu, mal);
        printResult(r);
        record(suite, r);
    }

    {
        auto pool = measureMedian(repeats, [] {
            return runLargeObjectsFullWrite(Backend::Pool, FreeMode::Sized, 2 * 1024 * 1024, 16);
        });
        auto neu = measureMedian(repeats, [] {
            return runLargeObjectsFullWrite(Backend::NewDelete, FreeMode::Sized, 2 * 1024 * 1024, 16);
        });
        auto mal = measureMedian(repeats, [] {
            return runLargeObjectsFullWrite(Backend::MallocFree, FreeMode::Sized, 2 * 1024 * 1024, 16);
        });
        auto r = makeResult("large-object 2MB full-write x16", pool, neu, mal);
        printResult(r);
        record(suite, r);
    }

    {
        auto pool = measureMedian(repeats, [] {
            return runLargeObjectsWithTrim(512 * 1024, 64);
        });
        std::cout << "  pool 512KB alloc/free + trim x64 (maintenance, informational)="
                  << std::fixed << std::setprecision(3) << pool << " ms\n";
    }

    {
        auto pool = measureMedian(repeats, [] {
            return runStlAllocator(Backend::Pool, 20000);
        });
        auto neu = measureMedian(repeats, [] {
            return runStlAllocator(Backend::NewDelete, 20000);
        });
        auto mal = measureMedian(repeats, [] {
            return runStlAllocator(Backend::MallocFree, 20000);
        });
        auto r = makeResult("STL Allocator allocate(256 ints) x20000", pool, neu, mal);
        printResult(r);
        record(suite, r);
    }
}

double runColdLargeObjects(Backend backend, FreeMode mode, size_t objectSize, size_t count)
{
    // One-live-at-a-time: no batching benefit; stresses VirtualAlloc vs page-cache recycle.
    Timer timer;
    for (size_t i = 0; i < count; ++i)
    {
        void* ptr = backendAllocate(backend, objectSize);
        if (!ptr)
            continue;
        touch(ptr, objectSize, i);
        backendFree(backend, mode, ptr, objectSize);
    }
    return timer.elapsedMs();
}

double runVectorGrowth(Backend backend, size_t finalElements, size_t cycles)
{
    // Repeated grow/destroy so absolute runtime escapes timer noise and the
    // pool's size-class / large-span recycle path can show up.
    Timer timer;
    unsigned checksum = 0;
    for (size_t cycle = 0; cycle < cycles; ++cycle)
    {
        if (backend == Backend::Pool)
        {
            std::vector<int, Allocator<int>> values;
            values.reserve(64);
            for (size_t i = 0; i < finalElements; ++i)
                values.push_back(static_cast<int>(i + cycle));
            checksum += static_cast<unsigned>(values.back());
            checksum += static_cast<unsigned>(values.size());
        }
        else
        {
            std::vector<int> values;
            values.reserve(64);
            for (size_t i = 0; i < finalElements; ++i)
                values.push_back(static_cast<int>(i + cycle));
            checksum += static_cast<unsigned>(values.back());
            checksum += static_cast<unsigned>(values.size());
        }
    }
    volatile unsigned sink = checksum;
    (void)sink;
    return timer.elapsedMs();
}

double runChurnAllSizeClasses(Backend backend, FreeMode mode, size_t rounds)
{
    Timer timer;
    std::vector<std::pair<void*, size_t>> live;
    live.reserve(512);
    for (size_t round = 0; round < rounds; ++round)
    {
        for (size_t size = 8; size <= 4096; size *= 2)
        {
            void* ptr = backendAllocate(backend, size);
            if (!ptr)
                continue;
            touch(ptr, size, round);
            live.emplace_back(ptr, size);
        }
        for (const auto& item : live)
            backendFree(backend, mode, item.first, item.second);
        live.clear();
    }
    return timer.elapsedMs();
}

double runBurstUnsized(Backend backend, size_t threadCount, size_t perThread)
{
    return runMultiThreaded(backend, FreeMode::Unsized, 0xA11u, threadCount, perThread);
}

double runLiveSetMix(Backend backend, FreeMode mode, size_t threadCount, size_t opsPerThread)
{
    constexpr std::array<size_t, 11> kSizes{16, 32, 48, 64, 96, 128, 256, 512, 1024, 2048, 4096};
    constexpr size_t kLiveCap = 2048;

    Timer timer;
    std::vector<std::thread> threads;
    threads.reserve(threadCount);
    for (size_t tid = 0; tid < threadCount; ++tid)
    {
        threads.emplace_back([=]() {
            std::mt19937 rng(0x51A7u + static_cast<unsigned>(tid) * 17u);
            std::uniform_int_distribution<size_t> pick(0, kSizes.size() - 1);
            std::vector<std::pair<void*, size_t>> live;
            live.reserve(kLiveCap);
            for (size_t op = 0; op < opsPerThread; ++op)
            {
                const bool mustFree = live.size() >= kLiveCap;
                const bool doFree = mustFree || (!live.empty() && (rng() & 1u));
                if (doFree)
                {
                    const size_t idx = rng() % live.size();
                    backendFree(backend, mode, live[idx].first, live[idx].second);
                    live[idx] = live.back();
                    live.pop_back();
                }
                else
                {
                    const size_t size = kSizes[pick(rng)];
                    void* ptr = backendAllocate(backend, size);
                    if (!ptr)
                        continue;
                    touch(ptr, size, op);
                    live.emplace_back(ptr, size);
                }
            }
            for (const auto& item : live)
                backendFree(backend, mode, item.first, item.second);
        });
    }
    for (auto& th : threads)
        th.join();
    return timer.elapsedMs();
}

void runUsageSuite()
{
    std::cout << "\n== Usage-like (kept-alive mixed sizes, informational) ==\n"
              << "  Not counted in PASS/FAIL; this is closer to app traffic than alloc/free bursts.\n";

    constexpr size_t repeats = 9;
    {
        auto pool = measureMedian(repeats, [] {
            return runLiveSetMix(Backend::Pool, FreeMode::Sized, 4, 80000);
        });
        auto neu = measureMedian(repeats, [] {
            return runLiveSetMix(Backend::NewDelete, FreeMode::Sized, 4, 80000);
        });
        auto mal = measureMedian(repeats, [] {
            return runLiveSetMix(Backend::MallocFree, FreeMode::Sized, 4, 80000);
        });
        printResult(makeResult("live-set 4T sized 16B-4KB x80000", pool, neu, mal));
    }

    {
        auto pool = measureMedian(repeats, [] {
            return runLiveSetMix(Backend::Pool, FreeMode::Unsized, 4, 80000);
        });
        auto neu = measureMedian(repeats, [] {
            return runLiveSetMix(Backend::NewDelete, FreeMode::Unsized, 4, 80000);
        });
        auto mal = measureMedian(repeats, [] {
            return runLiveSetMix(Backend::MallocFree, FreeMode::Unsized, 4, 80000);
        });
        printResult(makeResult("live-set 4T unsized 16B-4KB x80000", pool, neu, mal));
    }
}

void runAdversarialSuite(SuiteSummary& suite, size_t repeats)
{
    std::cout << "\n== Adversarial (harder Windows / CRT cases) ==\n";

    {
        auto pool = measureMedian(repeats, [] {
            return runColdLargeObjects(Backend::Pool, FreeMode::Sized, 512 * 1024, 100);
        });
        auto neu = measureMedian(repeats, [] {
            return runColdLargeObjects(Backend::NewDelete, FreeMode::Sized, 512 * 1024, 100);
        });
        auto mal = measureMedian(repeats, [] {
            return runColdLargeObjects(Backend::MallocFree, FreeMode::Sized, 512 * 1024, 100);
        });
        auto r = makeResult("cold large 512KB x100 (1-live)", pool, neu, mal);
        printResult(r);
        record(suite, r);
    }

    {
        auto pool = measureMedian(repeats, [] {
            return runVectorGrowth(Backend::Pool, 20000, 200);
        });
        auto neu = measureMedian(repeats, [] {
            return runVectorGrowth(Backend::NewDelete, 20000, 200);
        });
        auto mal = measureMedian(repeats, [] {
            return runVectorGrowth(Backend::MallocFree, 20000, 200);
        });
        auto r = makeResult("vector growth 2e4 ints x200 cycles", pool, neu, mal);
        printResult(r);
        record(suite, r);
    }

    {
        auto pool = measureMedian(repeats, [] {
            return runChurnAllSizeClasses(Backend::Pool, FreeMode::Sized, 200);
        });
        auto neu = measureMedian(repeats, [] {
            return runChurnAllSizeClasses(Backend::NewDelete, FreeMode::Sized, 200);
        });
        auto mal = measureMedian(repeats, [] {
            return runChurnAllSizeClasses(Backend::MallocFree, FreeMode::Sized, 200);
        });
        auto r = makeResult("size-class churn 8B-4KB x200", pool, neu, mal);
        printResult(r);
        record(suite, r);
    }

    {
        auto pool = measureMedian(repeats, [] {
            return runChurnAllSizeClasses(Backend::Pool, FreeMode::Unsized, 200);
        });
        auto neu = measureMedian(repeats, [] {
            return runChurnAllSizeClasses(Backend::NewDelete, FreeMode::Unsized, 200);
        });
        auto mal = measureMedian(repeats, [] {
            return runChurnAllSizeClasses(Backend::MallocFree, FreeMode::Unsized, 200);
        });
        auto r = makeResult("size-class churn unsized 8B-4KB x200", pool, neu, mal);
        printResult(r);
        record(suite, r);
    }

    {
        auto pool = measureMedian(repeats, [] {
            return runBurstUnsized(Backend::Pool, 8, 50000);
        });
        auto neu = measureMedian(repeats, [] {
            return runBurstUnsized(Backend::NewDelete, 8, 50000);
        });
        auto mal = measureMedian(repeats, [] {
            return runBurstUnsized(Backend::MallocFree, 8, 50000);
        });
        auto r = makeResult("burst unsized 8x50000", pool, neu, mal);
        printResult(r);
        record(suite, r);
    }

    {
        auto pool = measureMedian(repeats, [] {
            return runHighRetention(Backend::Pool, FreeMode::Unsized, 64, 100000);
        });
        auto neu = measureMedian(repeats, [] {
            return runHighRetention(Backend::NewDelete, FreeMode::Unsized, 64, 100000);
        });
        auto mal = measureMedian(repeats, [] {
            return runHighRetention(Backend::MallocFree, FreeMode::Unsized, 64, 100000);
        });
        auto r = makeResult("high-retention unsized 64B x100000", pool, neu, mal);
        printResult(r);
        record(suite, r);
    }
}

void runScalabilityMatrix(SuiteSummary& suite)
{
    std::cout << "\n== Scalability matrix (25000 ops/thread, median of 3) ==\n";
    std::cout << "  size threads       pool        new     malloc   vs_new vs_malloc\n";

    constexpr std::array<size_t, 7> kSizes{16, 32, 64, 256, 1024, 4096, 64 * 1024};
    constexpr std::array<size_t, 4> kThreads{1, 2, 4, 8};
    constexpr size_t kRepeats = 3;

    for (size_t size : kSizes)
    {
        for (size_t threadCount : kThreads)
        {
            auto pool = measureMedian(kRepeats, [=] {
                return runScalability(Backend::Pool, FreeMode::Sized, size, threadCount, 25000);
            });
            auto neu = measureMedian(kRepeats, [=] {
                return runScalability(Backend::NewDelete, FreeMode::Sized, size, threadCount, 25000);
            });
            auto mal = measureMedian(kRepeats, [=] {
                return runScalability(Backend::MallocFree, FreeMode::Sized, size, threadCount, 25000);
            });

            std::string name = "scale " + std::to_string(size) + "B x" + std::to_string(threadCount) + "thr";
            auto r = makeResult(std::move(name), pool, neu, mal);
            std::cout << std::setw(6) << size << std::setw(8) << threadCount
                      << std::setw(11) << std::fixed << std::setprecision(3) << pool
                      << std::setw(11) << neu << std::setw(11) << mal
                      << std::setw(9) << verdictText(r.vsNew)
                      << std::setw(10) << verdictText(r.vsMalloc) << '\n';
            record(suite, r);
        }
    }
}

void printMemoryFootprint()
{
    const MemoryPoolStats beforeTrim = MemoryPool::stats();
    MemoryPool::trim();
    const MemoryPoolStats stats = MemoryPool::stats();
    std::cout << "\n== Pool memory footprint ==\n"
              << "  trimCachedPageBytes=" << beforeTrim.cachedPageBytes
              << " -> " << stats.cachedPageBytes << '\n'
              << "  reservedBytes=" << stats.reservedBytes
              << " (" << std::fixed << std::setprecision(2)
              << (stats.reservedBytes / (1024.0 * 1024.0)) << " MiB)\n"
              << "  cachedPageBytes=" << stats.cachedPageBytes
              << " (" << (stats.cachedPageBytes / (1024.0 * 1024.0)) << " MiB)\n"
              << "  page-cache cap=" << (MemoryPool::pageCacheLimit() / (1024.0 * 1024.0)) << " MiB\n"
              << "  allocs=" << stats.allocCount
              << " frees=" << stats.freeCount
              << " live=" << stats.liveAllocs << '\n'
              << "  smallAllocs=" << stats.smallAllocCount
              << " largeAllocs=" << stats.largeAllocCount
              << " sizedFrees=" << stats.sizedFreeCount
              << " unsizedFrees=" << stats.unsizedFreeCount << '\n'
              << "  centralRefillBlocks=" << stats.centralRefillCount
              << " centralFlushBlocks=" << stats.centralFlushCount << '\n'
              << "  centralLockWaits=" << stats.centralLockWaits << std::endl;
    std::cout << "  centralByClass:";
    for (size_t i = 0; i < stats.centralRefillByClass.size(); ++i)
    {
        if (stats.centralRefillByClass[i] || stats.centralFlushByClass[i])
            std::cout << " [" << i << ":r=" << stats.centralRefillByClass[i]
                      << ",f=" << stats.centralFlushByClass[i] << "]";
    }
    std::cout << '\n';
    std::cout << "  threadCacheByClass:";
    for (size_t i = 0; i < stats.threadCacheHitByClass.size(); ++i)
    {
        if (stats.threadCacheHitByClass[i] || stats.threadCacheMissByClass[i])
            std::cout << " [" << i << ":h=" << stats.threadCacheHitByClass[i]
                      << ",m=" << stats.threadCacheMissByClass[i] << "]";
    }
    std::cout << '\n';
}

void printSuiteSummary(const SuiteSummary& suite)
{
    std::cout << "\n== Verdict summary ==\n"
              << "  scenarios=" << suite.results.size()
              << " wins=" << suite.wins
              << " ties=" << suite.ties
              << " losses=" << suite.losses
              << " (tie tolerance=" << (kTieTolerance * 100.0) << "%)\n";

    if (suite.losses > 0)
    {
        std::cout << "  Losing scenarios:\n";
        for (const auto& r : suite.results)
        {
            if (r.vsNew == Verdict::Lose || r.vsMalloc == Verdict::Lose)
            {
                std::cout << "    - " << r.name
                          << " (vs_new=" << verdictText(r.vsNew)
                          << ", vs_malloc=" << verdictText(r.vsMalloc) << ")\n";
            }
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    constexpr size_t kRepeats = 9;
    const bool reportOnly = argc > 1 && std::string(argv[1]) == "--report";

    std::cout << "Starting comprehensive performance suite...\n"
              << "Compare MemoryPool vs new/delete and malloc/free.\n"
              << "WIN = pool faster by >5%, TIE = within 5%, LOSE = pool slower by >5%.\n";

    warmup();

    SuiteSummary suite;
    runCoreSuite(suite, kRepeats);
    runOwnershipSuite(suite, kRepeats);
    runShapeSuite(suite, kRepeats);
    runAdversarialSuite(suite, kRepeats);
    runUsageSuite();
    runScalabilityMatrix(suite);
    printMemoryFootprint();
    printSuiteSummary(suite);

    if (suite.losses > 0)
    {
        std::cout << "\nRESULT: " << (reportOnly ? "OBSERVED LOSSES" : "FAIL")
                  << " (" << suite.losses << " losing scenario(s))\n";
        return reportOnly ? 0 : 1;
    }

    std::cout << "\nRESULT: PASS (no losses vs system allocators)\n";
    return 0;
}
