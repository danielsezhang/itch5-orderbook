cmake_minimum_required(VERSION 3.20)
project(itch5_orderbook LANGUAGES CXX)

option(ITCH_BUILD_TESTS "Build state validation tests" ON)
option(ITCH_BUILD_BENCHMARKS "Build latency benchmarks" ON)

add_library(itch_book
  src/mapped_file.cpp
  src/parser.cpp
  src/order_book.cpp
  src/order_pool.cpp
)
target_include_directories(itch_book PUBLIC include)
target_compile_features(itch_book PUBLIC cxx_std_20)
target_compile_options(itch_book PRIVATE -Wall -Wextra -Wpedantic)

if(ITCH_BUILD_TESTS OR ITCH_BUILD_BENCHMARKS)
  include(FetchContent)
endif()

if(ITCH_BUILD_TESTS)
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(Catch2)
  enable_testing()
  add_executable(itch_tests tests/order_book_tests.cpp tests/parser_tests.cpp)
  target_link_libraries(itch_tests PRIVATE itch_book Catch2::Catch2WithMain)
  include(CTest)
  include(Catch)
  catch_discover_tests(itch_tests)
endif()

if(ITCH_BUILD_BENCHMARKS)
  FetchContent_Declare(benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.1
    GIT_SHALLOW TRUE)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)
  add_executable(itch_benchmark benchmarks/ingestion_benchmark.cpp)
  target_link_libraries(itch_benchmark PRIVATE itch_book benchmark::benchmark_main)
endif()
