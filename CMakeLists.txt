cmake_minimum_required(VERSION 3.15)
project(distributed-llama)

option(DLLAMA_VULKAN "Build with vulkan support (currently unsupported)" OFF)
option(DLLAMA_MARCH_NATIVE "Optimize for current cpu" ON)

set(CMAKE_CXX_STANDARD 17)
if (DLLAMA_MARCH_NATIVE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native -mtune=native")
endif()
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror -Wformat -Werror=format-security")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")

find_package(Threads REQUIRED)

set(WITH_GFLAGS OFF CACHE BOOL "" FORCE)
set(WITH_BENCHMARK_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_CORE_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_TOOLS OFF CACHE BOOL "" FORCE)
set(ROCKSDB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/rocksdb EXCLUDE_FROM_ALL)

add_subdirectory(src)

