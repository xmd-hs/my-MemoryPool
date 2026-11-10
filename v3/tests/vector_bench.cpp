#include <xmd/Allocator.h>
#include <xmd/MemoryPool.h>
#include <chrono>
#include <iostream>
#include <vector>
using namespace xmd_memoryPool;
using Clock = std::chrono::steady_clock;
int main() {
  for (int warm=0; warm<3; ++warm) {
    std::vector<int, Allocator<int>> v; v.reserve(64);
    for (int i=0;i<100000;++i) v.push_back(i);
  }
  auto t0 = Clock::now();
  unsigned cs=0;
  for (int c=0;c<30;++c) {
    std::vector<int, Allocator<int>> v; v.reserve(64);
    for (int i=0;i<100000;++i) v.push_back(i+c);
    cs += (unsigned)v.back();
  }
  auto ms = std::chrono::duration<double,std::milli>(Clock::now()-t0).count();
  std::cout << "pool " << ms << " sink=" << cs << "\n";
  t0 = Clock::now(); cs=0;
  for (int c=0;c<30;++c) {
    std::vector<int> v; v.reserve(64);
    for (int i=0;i<100000;++i) v.push_back(i+c);
    cs += (unsigned)v.back();
  }
  ms = std::chrono::duration<double,std::milli>(Clock::now()-t0).count();
  std::cout << "std  " << ms << " sink=" << cs << "\n";
}
