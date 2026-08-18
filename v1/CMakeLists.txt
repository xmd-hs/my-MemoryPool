cmake_minimum_required(VERSION 3.10)
project(MemoryPoolProject)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -g)
endif()

find_package(Threads REQUIRED)

include_directories(include)

file(GLOB SRC_FILES "src/*.cpp")
file(GLOB TEST_FILES "tests/*.cpp")

add_executable(${PROJECT_NAME} ${SRC_FILES} ${TEST_FILES})

target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)
