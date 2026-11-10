cmake_minimum_required(VERSION 3.14)
project(xmdMemoryPoolBinaryConsumer LANGUAGES C CXX)

find_package(xmdMemoryPool 2 CONFIG REQUIRED)

add_executable(binary_cpp_consumer cpp_consumer.cpp)
target_compile_features(binary_cpp_consumer PRIVATE cxx_std_14)
target_link_libraries(binary_cpp_consumer PRIVATE xmdMemoryPool::memory_pool)

add_executable(binary_c_consumer c_consumer.c)
target_link_libraries(binary_c_consumer PRIVATE xmdMemoryPool::memory_pool)
