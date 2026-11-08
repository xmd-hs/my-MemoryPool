#include <kama/MemoryPool.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
struct Block { void* ptr; size_t size; };
struct Mailbox { std::mutex mutex; std::vector<Block> blocks; };

uint64_t nextRandom(uint64_t& state)
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

size_t chooseSize(uint64_t value)
{
    const unsigned n = static_cast<unsigned>(value % 1000);
    if (n < 400) return 64;
    if (n < 650) return 128;
    if (n < 800) return 256;
    if (n < 900) return 1024;
    if (n < 970) return 4096;
    if (n < 995) return 64 * 1024;
    return 512 * 1024;
}

size_t linuxStatusKiB(const std::string& field)
{
#ifdef __linux__
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line))
        if (line.compare(0, field.size(), field) == 0)
            return static_cast<size_t>(std::strtoull(line.c_str() + field.size(), nullptr, 10));
#else
    (void)field;
#endif
    return 0;
}

uint64_t percentile(std::vector<uint64_t>& values, double fraction)
{
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[static_cast<size_t>(fraction * (values.size() - 1))];
}
} // namespace

int main(int argc, char** argv)
{
    const std::string backend = argc > 1 ? argv[1] : "pool";
    const size_t threads = argc > 2 ? static_cast<size_t>(std::strtoull(argv[2], nullptr, 10)) : 8;
    const size_t ops = argc > 3 ? static_cast<size_t>(std::strtoull(argv[3], nullptr, 10)) : 500000;
    if ((backend != "pool" && backend != "malloc") || threads == 0 || threads > 64 ||
        ops == 0 || ops > 100000000)
    {
        std::cerr << "usage: workload_bench [pool|malloc] [threads:1..64] [ops_per_thread]\n";
        return 2;
    }
    const bool pool = backend == "pool";
    std::unique_ptr<Mailbox[]> mailboxes(new Mailbox[threads]);
    std::vector<std::vector<uint64_t>> samples(threads);
    std::atomic<size_t> liveBytes{0}, peakLiveBytes{0}, failures{0}, remoteFrees{0}, queueFull{0};
    auto release = [&](Block block) {
        if (pool) Kama_memoryPool::MemoryPool::deallocate(block.ptr, block.size);
        else std::free(block.ptr);
        liveBytes.fetch_sub(block.size, std::memory_order_relaxed);
    };
    auto allocate = [&](size_t size) -> void* {
        void* ptr = pool ? Kama_memoryPool::MemoryPool::allocate(size) : std::malloc(size);
        if (!ptr) { failures.fetch_add(1, std::memory_order_relaxed); return nullptr; }
        const size_t now = liveBytes.fetch_add(size, std::memory_order_relaxed) + size;
        size_t peak = peakLiveBytes.load(std::memory_order_relaxed);
        while (now > peak && !peakLiveBytes.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {}
        return ptr;
    };

    const auto start = Clock::now();
    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            uint64_t rng = 0x9e3779b97f4a7c15ULL ^ ((tid + 1) * 0xbf58476d1ce4e5b9ULL);
            std::vector<Block> live, inbox;
            live.reserve(256);
            samples[tid].reserve(ops / 1024 + 1);
            auto drain = [&] {
                { std::lock_guard<std::mutex> lock(mailboxes[tid].mutex); inbox.swap(mailboxes[tid].blocks); }
                for (Block block : inbox) release(block);
                inbox.clear();
            };
            for (size_t op = 0; op < ops; ++op)
            {
                if ((op & 63u) == 0) drain();
                const bool sample = (op & 1023u) == 511;
                const auto sampleStart = sample ? Clock::now() : Clock::time_point{};
                const uint64_t choice = nextRandom(rng);
                if (!live.empty() && (live.size() == 256 || (choice & 1u)))
                {
                    const size_t index = static_cast<size_t>(nextRandom(rng) % live.size());
                    const Block block = live[index];
                    live[index] = live.back();
                    live.pop_back();
                    if ((choice & 15u) == 1 && threads > 1)
                    {
                        Mailbox& target = mailboxes[(tid + 1) % threads];
                        bool queued = false;
                        {
                            std::lock_guard<std::mutex> lock(target.mutex);
                            if (target.blocks.size() < 256)
                            {
                                target.blocks.push_back(block);
                                queued = true;
                            }
                        }
                        if (queued) remoteFrees.fetch_add(1, std::memory_order_relaxed);
                        else { queueFull.fetch_add(1, std::memory_order_relaxed); release(block); }
                    }
                    else release(block);
                }
                else
                {
                    const size_t size = chooseSize(nextRandom(rng));
                    if (void* ptr = allocate(size))
                    {
                        volatile unsigned char* bytes = static_cast<volatile unsigned char*>(ptr);
                        bytes[0] = static_cast<unsigned char>(op);
                        bytes[size - 1] = static_cast<unsigned char>(op >> 8);
                        live.push_back({ptr, size});
                    }
                }
                if (sample)
                    samples[tid].push_back(static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - sampleStart).count()));
            }
            for (Block block : live) release(block);
            drain();
        });
    }
    for (auto& worker : workers) worker.join();
    for (size_t tid = 0; tid < threads; ++tid)
        for (Block block : mailboxes[tid].blocks) release(block);
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    const size_t peakRSSKiB = linuxStatusKiB("VmHWM:");
    const size_t rssAfterFreeKiB = linuxStatusKiB("VmRSS:");
    const size_t reservedBeforeRelease = pool ? Kama_memoryPool::MemoryPool::stats().reservedBytes : 0;
    if (pool) Kama_memoryPool::MemoryPool::releaseUnused();
    const size_t rssAfterReleaseKiB = linuxStatusKiB("VmRSS:");
    const size_t reservedAfterRelease = pool ? Kama_memoryPool::MemoryPool::stats().reservedBytes : 0;

    std::vector<uint64_t> allSamples;
    for (auto& workerSamples : samples)
        allSamples.insert(allSamples.end(), workerSamples.begin(), workerSamples.end());
    const uint64_t p50 = percentile(allSamples, 0.50);
    const uint64_t p99 = percentile(allSamples, 0.99);
    std::cout << std::fixed << std::setprecision(2)
              << "backend=" << backend << " threads=" << threads
              << " ops_per_thread=" << ops << " seconds=" << seconds
              << " mops=" << (threads * ops / seconds / 1e6)
              << " sampled_p50_ns=" << p50 << " sampled_p99_ns=" << p99
              << " peak_live_mib=" << (peakLiveBytes.load() / 1048576.0);
    if (peakRSSKiB)
        std::cout << " peak_rss_mib=" << (peakRSSKiB / 1024.0)
                  << " rss_after_free_mib=" << (rssAfterFreeKiB / 1024.0)
                  << " rss_after_release_mib=" << (rssAfterReleaseKiB / 1024.0);
    else
        std::cout << " rss_metrics=unavailable";
    std::cout
              << " reserved_before_release_mib=" << (reservedBeforeRelease / 1048576.0)
              << " reserved_after_release_mib=" << (reservedAfterRelease / 1048576.0)
              << " remote_frees=" << remoteFrees.load()
              << " queue_full_fallbacks=" << queueFull.load()
              << " failures=" << failures.load() << " live_bytes=" << liveBytes.load() << '\n';
    return failures.load() == 0 && liveBytes.load() == 0 ? 0 : 1;
}
