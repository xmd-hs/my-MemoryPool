// =============================================================================
// v2 基础设施：size class 表 + 自旋锁 + CPU pause
// =============================================================================
//
// v2 相比 v1 的核心改进：
//   1. size class 从「等距 8B 步进」改成「几何步进」（每 8 档 align 翻倍）
//      这样大对象的内部碎片比例控制在 ~12.5%，而不是随大小线性增长。
//      例如 v1 申请 200KB 会被向上取整到 200KB+（浪费接近整块），
//      v2 则落在最近的档位，浪费不超过 12.5%。
//   2. 引入编译期查表（kSmallIndexMap），小对象（0~256B）O(1) 定位档位
//   3. 自旋锁（SpinLock）替代 mutex，临界区短时更高效
//
// 这个文件只放「编译期常量 + 无状态工具」，不含任何运行期全局变量，
// 因此可以被任意 .cpp 安全 include 而不引入重复定义。

#pragma once  // 防止头文件被重复包含（编译器级别的 include guard）

#include <algorithm>  // std::max 等算法
#include <array>      // std::array 固定数组
#include <atomic>     // std::atomic_flag 原子标志
#include <cstddef>    // size_t
#include <cstdint>    // uint8_t 等
#include <thread>     // std::this_thread::yield

#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))  // MSVC + x86/x64
#include <intrin.h>  // _mm_pause 等内联函数
#endif

namespace xmd_memoryPool  // 命名空间，避免符号冲突
{

// ---- 全局常量 ----

// 对齐基准：所有 size class 都是 8 的倍数。
// 选 8 是因为 64 位平台上指针是 8 字节，8 字节对齐能保证
// 任何对象的首地址都能一次缓存行访问拿到指针字段。
constexpr size_t ALIGNMENT = 8;

// 小对象上限：超过 256KB 的申请直接走 malloc（PageCache 大对象路径），
// 不进入 size class 切分，避免在 CentralCache 里切出巨型 span。
constexpr size_t MAX_BYTES = 256 * 1024;

// size class 表容量上限，实际档位数 FREE_LIST_SIZE ≤ 64。
// 几何步进下 256KB 大约只需要 50 多档，64 留有余量。
constexpr size_t kMaxSizeClasses = 64;

// ---- CPU 空转指令 ----
//
// 自旋等待时不能直接忙等（会吃满一个核心，还可能触发内存顺序重排），
// 也不能立刻 yield（让出 CPU 代价高，临界区短时反而更慢）。
// PAUSE 指令的作用：
//   - 告诉 CPU「我在自旋」，CPU 会降低功耗、避免流水线 speculative 执行出错
//   - 不会真正让出 CPU，锁一旦释放能立刻抢到
// 不同架构有各自的等价指令，这里用宏统一封装成 cpuPause()。
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))  // MSVC + x86/x64
inline void cpuPause() { _mm_pause(); }                          // MSVC 的 PAUSE 内联函数
#elif defined(__x86_64__) || defined(__i386__)                   // GCC/Clang + x86
inline void cpuPause() { __builtin_ia32_pause(); }               // GCC 的 PAUSE 内建函数
#elif defined(__aarch64__)                                        // ARM64
inline void cpuPause() { asm volatile("yield"); }                // ARM 的 yield 指令
#else                                                             // 其他架构
inline void cpuPause() { std::this_thread::yield(); }            // 用线程让步代替
#endif

// =============================================================================
// 自旋锁 SpinLock
// =============================================================================
//
// 设计要点：
//   - RAII：构造时加锁，析构时解锁，离开作用域自动释放，不会因异常而死锁
//   - 不持有状态：锁状态存在外部传入的 atomic_flag 里，SpinLock 对象本身只是个 guard
//   - 两级等待：先 PAUSE 空转 64 次（适合临界区极短的场景），
//     还拿不到再 yield 让出 CPU（避免长时间占着核心饿死其他线程）
//
// 为什么用 atomic_flag 而不是 atomic<bool>：
//   atomic_flag 是 C++ 标准里唯一保证无锁（lock-free）的原子类型，
//   它的 test_and_set / clear 直接映射到 CPU 的 XCHG 指令，开销最小。
//
// 内存序：
//   - 加锁用 acquire：保证临界区内的读操作能看到其他线程释放锁前的写
//   - 解锁用 release：保证临界区内的写操作对其他线程可见
class SpinLock
{
public:
    // 绑定外部的 flag 并立即加锁
    explicit SpinLock(std::atomic_flag& flag) : flag_(flag)
    {
        int spins = 0;  // 空转计数
        // test_and_set：原子地把 flag 设为 true，返回旧值。
        // 返回 true 表示之前已经被设过（锁被别人占着），需要继续等。
        while (flag_.test_and_set(std::memory_order_acquire))
        {
            cpuPause();  // 拿不到就 PAUSE 一下，降低功耗并提示 CPU
            if (++spins >= 64)                          // 空转 64 次还拿不到
                std::this_thread::yield();              // 让出 CPU，避免饿死其他线程
        }
    }

    ~SpinLock()
    {
        // 析构时释放锁：把 flag 清零，下一个线程就能抢到
        flag_.clear(std::memory_order_release);
    }

    // 禁止拷贝和赋值：一个 guard 只能管一把锁，拷贝会导致重复解锁
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

private:
    std::atomic_flag& flag_;  // 引用外部的原子标志（锁本身不存状态）
};

// =============================================================================
// size class 表
// =============================================================================
//
// size class 就是「档位表」：把申请大小向上取整到最近的档位大小。
// 例如档位是 8, 16, 24, ...，申请 20 字节会被取整到 24 字节那一档。
// 用档位而不是精确大小的好处：
//   - 相同档位的内存块可以放在同一个 free list 里复用
//   - 减少外部碎片（大量不同大小的释放块能被归类到少数几个档位）
//
// v2 用「几何步进」生成档位：每 8 档步长翻倍。
// 前几档步长是 8：  8, 16, 24, 32, 40, 48, 56, 64   （8 档）
// 接下来步长 16：   80, 96, 112, 128, 144, 160, 176, 192  （8 档）
// 步长 32：         224, 256, 288, ... 
// 这样档位越密，小对象（常用）粒度细，大对象粒度粗，整体内部碎片 ≤ 12.5%。

// 表结构：存所有档位的字节数 + 实际档位数
struct SizeClassTable
{
    std::array<size_t, kMaxSizeClasses> sizes{};  // 各档位的字节数（如 8, 16, 24, ...）
    size_t count = 0;                              // 实际档位数
};

// 编译期生成 size class 表（几何步进：每 8 档步长翻倍）
// 用 constexpr 让编译器在编译时算好，运行期零开销。
constexpr SizeClassTable makeSizeClassTable()
{
    SizeClassTable table{};            // 空表
    size_t align = ALIGNMENT;          // 当前步长，初始 8
    size_t size = 0;                   // 当前档位大小
    while (table.count < kMaxSizeClasses)  // 不超过 64 档
    {
        size += align;                 // 下一档 = 当前 + 步长
        if (size > MAX_BYTES)          // 超过 256KB 就停
            break;
        table.sizes[table.count++] = size;  // 记录这一档
        if (size >= align * 8)         // 攒够 8 档（步长的 8 倍）
            align *= 2;                // 步长翻倍
    }
    return table;
}

// 编译期算好的档位表，放在 .rodata 只读段，所有线程共享同一份
constexpr SizeClassTable kSizeClassTable = makeSizeClassTable();
constexpr size_t FREE_LIST_SIZE = kSizeClassTable.count;  // 实际档位数

// =============================================================================
// 小对象查表 kSmallIndexMap
// =============================================================================
//
// 问题：getIndex(bytes) 要找「第一个 >= bytes 的档位」，如果每次都遍历表是 O(n)。
// 优化：0~256 字节的小对象最常用，预先算好一个 257 项的查表，O(1) 直接定位。
// 256B 以上的对象相对少，用二分查找 O(log n) 也够快。

// 编译期生成小对象查表：bytes 0~256 直接得到档位下标，O(1)
constexpr std::array<uint8_t, 257> makeSmallIndexMap()
{
    std::array<uint8_t, 257> map{};  // 0~256 每个大小对应一个档位
    size_t index = 0;                 // 当前档位下标
    for (size_t n = 0; n <= 256; ++n) // 遍历 0~256 字节
    {
        size_t bytes = n == 0 ? ALIGNMENT : n;  // 0 当成 8 处理（不能分配 0 字节）
        // 因为 n 递增，index 只会单调不减，所以不用每次从头找，
        // 只需把 index 推进到第一个 sizes[index] >= bytes 的位置。
        while (index + 1 < kSizeClassTable.count &&
               kSizeClassTable.sizes[index] < bytes)
        {
            ++index;
        }
        map[n] = static_cast<uint8_t>(index);  // 记录 n 字节对应哪个档位
    }
    return map;
}

constexpr std::array<uint8_t, 257> kSmallIndexMap = makeSmallIndexMap();  // 编译期算好

// 编译期校验：至少 1 档、首档是 8、末档覆盖 256KB。
// 用 static_assert 在编译期检查，不满足直接报错，避免运行期踩坑。
static_assert(FREE_LIST_SIZE > 0 && FREE_LIST_SIZE <= kMaxSizeClasses, "invalid size class count");
static_assert(kSizeClassTable.sizes[0] == ALIGNMENT, "first size class must be ALIGNMENT");
static_assert(kSizeClassTable.sizes[FREE_LIST_SIZE - 1] == MAX_BYTES, "last size class must cover MAX_BYTES");

// =============================================================================
// SizeClass 工具类：提供 档位 <-> 大小 的转换
// =============================================================================
//
// 全部是静态方法，无状态，直接操作编译期算好的表。
// 调用方：ThreadCache（分配/释放时算档位）、CentralCache（切 span 时算块数）

class SizeClass
{
public:
    // 总档位数（编译期常量）
    static constexpr size_t classCount() { return FREE_LIST_SIZE; }

    // 档位下标 -> 该档位的实际字节大小
    static constexpr size_t getSize(size_t index)
    {
        return kSizeClassTable.sizes[index];
    }

    // 任意大小 -> 向上取整到最近的档位大小
    // 例如 roundUp(20) = 24（24 是 >= 20 的最小档位）
    static size_t roundUp(size_t bytes)
    {
        if (bytes > MAX_BYTES)
            // 大对象不走 size class，直接 8 字节对齐（位运算版的向上取整）
            return (bytes + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
        return getSize(getIndex(bytes));  // 小对象先算档位再取档位大小
    }

    // 任意大小 -> 档位下标（即 free list 的数组下标）
    static size_t getIndex(size_t bytes)
    {
        if (bytes <= 256)
            return kSmallIndexMap[bytes];  // 小对象（0~256B）直接查表，O(1)

        // 大对象（256B ~ 256KB）用二分查找，O(log n)
        bytes = std::max(bytes, ALIGNMENT);  // 至少 8（0 或更小都按 8 算）
        // 左闭右开区间 [lo, hi)，找第一个 sizes[mid] >= bytes 的位置
        size_t lo = 0;
        size_t hi = FREE_LIST_SIZE;
        while (lo < hi)
        {
            size_t mid = (lo + hi) / 2;
            if (kSizeClassTable.sizes[mid] < bytes)
                lo = mid + 1;  // 中间档太小，往右找
            else
                hi = mid;      // 中间档够大，往左缩
        }
        return lo;  // 循环结束时 lo == hi，就是第一个 >= bytes 的档位下标
    }
};

} // namespace xmd_memoryPool
