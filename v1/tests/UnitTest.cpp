#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <iostream>
#include <new>
#include <thread>
#include <vector>

#include "../include/MemoryPool.h"

using namespace Kama_memoryPool;

class P1 { public: int id_; };
class P2 { public: int id_[5]; };
class P3 { public: int id_[10]; };
class P4 { public: int id_[20]; };

static void keep(void* p)
{
#if defined(__clang__) || defined(__GNUC__)
    asm volatile("" : : "r"(p) : "memory");
#else
    volatile void* sink = p;
    (void)sink;
#endif
}

#if defined(__clang__) || defined(__GNUC__)
#define BENCH_NOINLINE __attribute__((noinline))
#else
#define BENCH_NOINLINE
#endif

BENCH_NOINLINE static void touchPool()
{
    P1* p1 = newElement<P1>();
    p1->id_ = 1;
    keep(p1);
    deleteElement<P1>(p1);

    P2* p2 = newElement<P2>();
    p2->id_[0] = 1;
    keep(p2);
    deleteElement<P2>(p2);

    P3* p3 = newElement<P3>();
    p3->id_[0] = 1;
    keep(p3);
    deleteElement<P3>(p3);

    P4* p4 = newElement<P4>();
    p4->id_[0] = 1;
    keep(p4);
    deleteElement<P4>(p4);
}

BENCH_NOINLINE static void touchNew()
{
    static void* (*const alloc)(std::size_t) = ::operator new;
    static void (*const rel)(void*) = ::operator delete;

    P1* p1 = static_cast<P1*>(alloc(sizeof(P1)));
    ::new (p1) P1();
    p1->id_ = 1;
    keep(p1);
    p1->~P1();
    rel(p1);

    P2* p2 = static_cast<P2*>(alloc(sizeof(P2)));
    ::new (p2) P2();
    p2->id_[0] = 1;
    keep(p2);
    p2->~P2();
    rel(p2);

    P3* p3 = static_cast<P3*>(alloc(sizeof(P3)));
    ::new (p3) P3();
    p3->id_[0] = 1;
    keep(p3);
    p3->~P3();
    rel(p3);

    P4* p4 = static_cast<P4*>(alloc(sizeof(P4)));
    ::new (p4) P4();
    p4->id_[0] = 1;
    keep(p4);
    p4->~P4();
    rel(p4);
}

static double BenchmarkMemoryPool(size_t ntimes, size_t nworks, size_t rounds)
{
    std::vector<std::thread> vthread(nworks);
    std::atomic<long long> total_ns;
    total_ns.store(0);

    for (size_t k = 0; k < nworks; ++k)
    {
        vthread[k] = std::thread([&]() {
            long long local = 0;
            for (size_t j = 0; j < rounds; ++j)
            {
                const auto begin = std::chrono::steady_clock::now();
                for (size_t i = 0; i < ntimes; i++)
                {
                    touchPool();
                }
                const auto end = std::chrono::steady_clock::now();
                local += std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
            }
            total_ns.fetch_add(local);
        });
    }
    for (auto& t : vthread)
        t.join();

    const double ms = total_ns.load() / 1e6;
    printf("%zu 线程 × %zu 轮 × 每轮 %zu 次 newElement/deleteElement：%.3f ms（各线程耗时之和）\n",
           nworks, rounds, ntimes, ms);
    return ms;
}

static double BenchmarkNew(size_t ntimes, size_t nworks, size_t rounds)
{
    std::vector<std::thread> vthread(nworks);
    std::atomic<long long> total_ns;
    total_ns.store(0);

    for (size_t k = 0; k < nworks; ++k)
    {
        vthread[k] = std::thread([&]() {
            long long local = 0;
            for (size_t j = 0; j < rounds; ++j)
            {
                const auto begin = std::chrono::steady_clock::now();
                for (size_t i = 0; i < ntimes; i++)
                {
                    touchNew();
                }
                const auto end = std::chrono::steady_clock::now();
                local += std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
            }
            total_ns.fetch_add(local);
        });
    }
    for (auto& t : vthread)
        t.join();

    const double ms = total_ns.load() / 1e6;
    printf("%zu 线程 × %zu 轮 × 每轮 %zu 次 new/delete：%.3f ms（各线程耗时之和）\n",
           nworks, rounds, ntimes, ms);
    return ms;
}

int main()
{
    HashBucket::initMemoryPool();

    constexpr size_t kTimes = 10000;
    constexpr size_t kRounds = 10;
    const size_t threadCounts[] = {1, 2, 5};

    std::cout << "========== v1 内存池 ==========" << std::endl;
    for (size_t n : threadCounts)
        BenchmarkMemoryPool(kTimes, n, kRounds);

    std::cout << "========== operator new/delete ==========" << std::endl;
    for (size_t n : threadCounts)
        BenchmarkNew(kTimes, n, kRounds);

    return 0;
}
