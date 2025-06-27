

set(NN_SRCS
  nn-cache.cpp
  nn-core.cpp
  nn-cpu.cpp
  nn-cpu-ops.cpp
  nn-executor.cpp
  nn-network.cpp
  nn-quants.cpp
  llamafile/sgemm.cpp
)

set(NN_HDRS
  nn-cache.hpp
  nn-config-builder.hpp
  nn-core.hpp
  nn-cpu.hpp
  nn-cpu-ops.hpp
  nn-executor.hpp
  nn-network.hpp
  nn-quants.hpp
  llamafile/sgemm.cpp
)

add_library(nn OBJECT ${NN_SRCS})
set_target_properties(nn PROPERTIES PUBLIC_HEADER ${NN_HDRS})
target_include_directories(nn PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(nn Threads::Threads)

