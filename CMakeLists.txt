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

add_subdirectory(src)

