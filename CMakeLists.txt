cmake_minimum_required(VERSION 3.14)
project(queue LANGUAGES CXX)

# Header-only INTERFACE library. Consumers add the include dir and get C++20.
add_library(queue INTERFACE)
add_library(queue::queue ALIAS queue)
target_include_directories(queue INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(queue INTERFACE cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(queue INTERFACE Threads::Threads)

# Only build the tests, sanitizer targets, and benchmark when this repo is the
# top-level project, so consumers (e.g. via FetchContent) don't build our checks.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()

  # (a) Correctness: single-threaded API coverage.
  add_executable(queue_test test/test.cc)
  target_link_libraries(queue_test PRIVATE queue)
  add_test(NAME queue COMMAND queue_test)

  # (b) Concurrent stress: N producers x M consumers, exactly-once accounting.
  add_executable(queue_concurrent test/concurrent.cc)
  target_link_libraries(queue_concurrent PRIVATE queue)
  add_test(NAME queue_concurrent COMMAND queue_concurrent)

  # Sanitizer builds of the concurrent stress test. These are separate targets
  # (not run by ctest by default) because they want the Homebrew LLVM toolchain
  # and a scaled item count. Build with:
  #   cmake -B build -DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++
  #   cmake --build build --target queue_concurrent_asan queue_concurrent_tsan
  add_executable(queue_concurrent_asan EXCLUDE_FROM_ALL test/concurrent.cc)
  target_link_libraries(queue_concurrent_asan PRIVATE queue)
  target_compile_options(queue_concurrent_asan PRIVATE -fsanitize=address -g -O1)
  target_link_options(queue_concurrent_asan PRIVATE -fsanitize=address)

  add_executable(queue_concurrent_tsan EXCLUDE_FROM_ALL test/concurrent.cc)
  target_link_libraries(queue_concurrent_tsan PRIVATE queue)
  target_compile_options(queue_concurrent_tsan PRIVATE -fsanitize=thread -g -O1)
  target_link_options(queue_concurrent_tsan PRIVATE -fsanitize=thread)

  # Throughput benchmark (queue vs mutex+condvar baseline vs moodycamel). Built
  # at -O3 -DNDEBUG; needs the bundled concurrentqueue headers. Excluded from the
  # default build so a plain `cmake --build` stays test-only:
  #   cmake --build build --target queue_bench && ./build/queue_bench
  set(MOODYCAMEL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/concurrentqueue)
  if(EXISTS ${MOODYCAMEL_DIR}/blockingconcurrentqueue.h)
    add_executable(queue_bench EXCLUDE_FROM_ALL benchmark/bench.cc)
    target_link_libraries(queue_bench PRIVATE queue)
    target_include_directories(queue_bench PRIVATE ${MOODYCAMEL_DIR})
    target_compile_options(queue_bench PRIVATE -O3 -DNDEBUG)
  endif()
endif()
