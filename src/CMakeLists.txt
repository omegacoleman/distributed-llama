
add_subdirectory(nn)

set(APP_SRCS
  app.cpp
  llm.cpp
  tokenizer.cpp
)

add_library(rocksdb-cache OBJECT rocksdb-cache.cpp city.cc)
target_link_libraries(rocksdb-cache nn rocksdb)

add_library(app OBJECT ${APP_SRCS})
target_link_libraries(app nn rocksdb-cache)

add_executable(dllama-api dllama-api.cpp)
target_link_libraries(dllama-api app nn rocksdb-cache)

add_executable(dllama dllama.cpp)
target_link_libraries(dllama app nn rocksdb-cache)

