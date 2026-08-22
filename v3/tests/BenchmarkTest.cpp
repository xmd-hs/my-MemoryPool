#include <benchmark/benchmark.h>

#include <kama/MemoryPool.h>

#include <array>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

using Kama_memoryPool::MemoryPool;

// ---------------------------------------------------------------------------
// 辅助：系统分配器后端
// ---------------------------------------------------------------------------

enum class Backend { Pool, NewDelete, MallocFree };

void* backendAlloc(Backend b, size_t size)
{
    switch (b)
    {
    case Backend::Pool:       return MemoryPool::allocate(size);
    case Backend::NewDelete:  return static_cast<void*>(new (std::nothrow) char[size]);
    case Backend::MallocFree: return std::malloc(size);
    }
    return nullptr;
}

void backendFree(Backend b, void* p, size_t size)
{
    switch (b)
    {
    case Backend::Pool:       MemoryPool::deallocate(p, size); break;
    case Backend::NewDelete:  delete[] static_cast<char*>(p);  break;
    case Backend::MallocFree: std::free(p);                    break;
    }
}

// ---------------------------------------------------------------------------
// 1. 32B 小对象，100000 次分配/释放
// ---------------------------------------------------------------------------

template <Backend BackendT>
void BM_SmallObjects(benchmark::State& state)
{
    constexpr size_t kSize       = 32;
    constexpr size_t kBatch      = 100000;
    const size_t       kLiveBatch = 256;
    std::vector<void*> live;
    live.reserve(kLiveBatch);

    for (auto _ : state)
    {
        for (size_t i = 0; i < kBatch; ++i)
        {
            void* p = backendAlloc(BackendT, kSize);
            if (!p)
                continue;
            auto* bytes = static_cast<unsigned char*>(p);
            bytes[0]              = static_cast<unsigned char>(i);
            bytes[kSize - 1]     = static_cast<unsigned char>(i >> 8);
            live.push_back(p);
            if (live.size() == kLiveBatch)
            {
                for (void* item : live)
                    backendFree(BackendT, item, kSize);
                live.clear();
            }
        }
        for (void* item : live)
            backendFree(BackendT, item, kSize);
        live.clear();
    }
    state.SetItemsProcessed(state.iterations() * kBatch);
}
BENCHMARK_TEMPLATE(BM_SmallObjects, Backend::Pool);
BENCHMARK_TEMPLATE(BM_SmallObjects, Backend::NewDelete);
BENCHMARK_TEMPLATE(BM_SmallObjects, Backend::MallocFree);

// ---------------------------------------------------------------------------
// 2. 4 线程 × 25000 次，随机 8-256B
// ---------------------------------------------------------------------------

template <Backend BackendT>
void BM_MultiThreaded(benchmark::State& state)
{
    constexpr size_t kOpsPerThread = 25000;
    constexpr size_t kLiveBatch    = 256;
    const size_t      threadCount  = static_cast<size_t>(state.threads());
    const uint32_t    seedBase     = 0x1000u;

    for (auto _ : state)
    {
        std::vector<std::thread> threads;
        threads.reserve(threadCount);
        for (size_t tid = 0; tid < threadCount; ++tid)
        {
            threads.emplace_back([seed = seedBase + static_cast<uint32_t>(tid),
                                  kOpsPerThread, kLiveBatch]() {
                std::mt19937 rng(seed);
                std::uniform_int_distribution<size_t> sizeDist(8, 256);
                std::vector<std::pair<void*, size_t>> live;
                live.reserve(kLiveBatch);
                for (size_t i = 0; i < kOpsPerThread; ++i)
                {
                    const size_t sz = sizeDist(rng);
                    void* p = backendAlloc(BackendT, sz);
                    if (!p)
                        continue;
                    auto* bytes = static_cast<unsigned char*>(p);
                    bytes[0]          = static_cast<unsigned char>(i);
                    bytes[sz - 1]    = static_cast<unsigned char>(i >> 8);
                    live.emplace_back(p, sz);
                    if (live.size() == kLiveBatch)
                    {
                        for (const auto& item : live)
                            backendFree(BackendT, item.first, item.second);
                        live.clear();
                    }
                }
                for (const auto& item : live)
                    backendFree(BackendT, item.first, item.second);
            });
        }
        for (auto& t : threads)
            t.join();
    }
    state.SetItemsProcessed(state.iterations() * kOpsPerThread * threadCount);
}
BENCHMARK_TEMPLATE(BM_MultiThreaded, Backend::Pool)->Threads(4)->Threads(8);
BENCHMARK_TEMPLATE(BM_MultiThreaded, Backend::NewDelete)->Threads(4)->Threads(8);
BENCHMARK_TEMPLATE(BM_MultiThreaded, Backend::MallocFree)->Threads(4)->Threads(8);

// ---------------------------------------------------------------------------
// 3. 16B-2048B 混合尺寸，50000 次
// ---------------------------------------------------------------------------

template <Backend BackendT>
void BM_MixedSizes(benchmark::State& state)
{
    constexpr size_t kBatch = 50000;
    constexpr std::array<size_t, 8> kSizes{16, 32, 64, 128, 256, 512, 1024, 2048};
    const uint32_t seed = 0x2000u;
    const size_t   kLiveBatch = 256;
    std::vector<void*> live;
    live.reserve(kLiveBatch);

    for (auto _ : state)
    {
        std::mt19937 rng(seed);
        std::uniform_int_distribution<size_t> pick(0, kSizes.size() - 1);
        for (size_t i = 0; i < kBatch; ++i)
        {
            const size_t sz = kSizes[pick(rng)];
            void* p = backendAlloc(BackendT, sz);
            if (!p)
                continue;
            auto* bytes = static_cast<unsigned char*>(p);
            bytes[0]       = static_cast<unsigned char>(i);
            bytes[sz - 1]  = static_cast<unsigned char>(i >> 8);
            live.push_back(p);
            if (live.size() == kLiveBatch)
            {
                for (void* item : live)
                    backendFree(BackendT, item, 0);
                live.clear();
            }
        }
        for (void* item : live)
            backendFree(BackendT, item, 0);
        live.clear();
    }
    state.SetItemsProcessed(state.iterations() * kBatch);
}
BENCHMARK_TEMPLATE(BM_MixedSizes, Backend::Pool);
BENCHMARK_TEMPLATE(BM_MixedSizes, Backend::NewDelete);
BENCHMARK_TEMPLATE(BM_MixedSizes, Backend::MallocFree);

// ---------------------------------------------------------------------------
// 4. 扩展矩阵：不同尺寸 × 不同线程
// ---------------------------------------------------------------------------

template <Backend BackendT>
void BM_Scalability(benchmark::State& state)
{
    const size_t objSize = static_cast<size_t>(state.range(0));
    constexpr size_t kOpsPerThread = 25000;
    constexpr size_t kLiveBatch = 256;
    const size_t threadCount = static_cast<size_t>(state.threads());
    std::vector<void*> live;
    live.reserve(kLiveBatch);

    for (auto _ : state)
    {
        for (size_t i = 0; i < kOpsPerThread; ++i)
        {
            void* p = backendAlloc(BackendT, objSize);
            if (!p)
                continue;
            auto* bytes = static_cast<unsigned char*>(p);
            bytes[0]           = static_cast<unsigned char>(i);
            bytes[objSize - 1] = static_cast<unsigned char>(i >> 8);
            live.push_back(p);
            if (live.size() == kLiveBatch)
            {
                for (void* item : live)
                    backendFree(BackendT, item, objSize);
                live.clear();
            }
        }
        for (void* item : live)
            backendFree(BackendT, item, objSize);
        live.clear();
    }
    state.SetItemsProcessed(state.iterations() * kOpsPerThread * threadCount);
}

// 扩展矩阵：16/32/64/256/1024/4096/65536 × 1/2/4/8 线程
BENCHMARK_TEMPLATE(BM_Scalability, Backend::Pool)
    ->Args({16})->Args({32})->Args({64})->Args({256})
    ->Args({1024})->Args({4096})->Args({65536})
    ->Threads(1)->Threads(2)->Threads(4)->Threads(8);
BENCHMARK_TEMPLATE(BM_Scalability, Backend::NewDelete)
    ->Args({16})->Args({32})->Args({64})->Args({256})
    ->Args({1024})->Args({4096})->Args({65536})
    ->Threads(1)->Threads(2)->Threads(4)->Threads(8);
BENCHMARK_TEMPLATE(BM_Scalability, Backend::MallocFree)
    ->Args({16})->Args({32})->Args({64})->Args({256})
    ->Args({1024})->Args({4096})->Args({65536})
    ->Threads(1)->Threads(2)->Threads(4)->Threads(8);

// ---------------------------------------------------------------------------
// 5. 跨线程所有权转移（64B × 50000）
// ---------------------------------------------------------------------------

template <Backend BackendT>
void BM_CrossThread(benchmark::State& state)
{
    constexpr size_t kOps = 50000;
    constexpr size_t kSize = 64;

    for (auto _ : state)
    {
        std::vector<void*> pointers;
        pointers.reserve(kOps);

        std::thread producer([&]() {
            for (size_t i = 0; i < kOps; ++i)
            {
                void* p = backendAlloc(BackendT, kSize);
                if (p)
                    pointers.push_back(p);
            }
        });
        producer.join();

        std::thread consumer([&]() {
            for (void* p : pointers)
                backendFree(BackendT, p, kSize);
        });
        consumer.join();
    }
    state.SetItemsProcessed(state.iterations() * kOps);
}
BENCHMARK_TEMPLATE(BM_CrossThread, Backend::Pool);
BENCHMARK_TEMPLATE(BM_CrossThread, Backend::NewDelete);
BENCHMARK_TEMPLATE(BM_CrossThread, Backend::MallocFree);

BENCHMARK_MAIN();
