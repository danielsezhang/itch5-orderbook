cmake_minimum_required(VERSION 3.20)
project(itch5_orderbook LANGUAGES CXX)

option(ITCH_BUILD_TESTS "Build Catch2 validation tests" ON)
option(ITCH_BUILD_BENCHMARKS "Build ingestion and allocation benchmarks" ON)
set(ITCH_SANITIZE "" CACHE STRING "Sanitizer to enable, e.g. address,undefined or thread")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(ITCH_SANITIZE)
  add_compile_options(-fsanitize=${ITCH_SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${ITCH_SANITIZE})
endif()

add_library(itch_book
  src/mapped_file.cpp
  src/market_replay.cpp
  src/order_book.cpp
  src/order_pool.cpp
  src/sharded_replay.cpp
)
target_include_directories(itch_book PUBLIC include)
target_compile_features(itch_book PUBLIC cxx_std_20)
set(ITCH_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
target_compile_options(itch_book PRIVATE ${ITCH_WARNINGS})
find_package(Threads REQUIRED)
target_link_libraries(itch_book PUBLIC Threads::Threads)

if(ITCH_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(Catch2)
  enable_testing()
  add_executable(itch_tests
    tests/historical_tests.cpp
    tests/order_book_tests.cpp
    tests/order_pool_tests.cpp
    tests/parser_tests.cpp
    tests/sharded_replay_tests.cpp)
  target_link_libraries(itch_tests PRIVATE itch_book Catch2::Catch2WithMain)
  target_compile_options(itch_tests PRIVATE ${ITCH_WARNINGS})
  target_compile_definitions(itch_tests PRIVATE
    ITCH_DEFAULT_SAMPLE="${CMAKE_CURRENT_SOURCE_DIR}/data/20191230.BX_ITCH_50")
  include(CTest)
  include(Catch)
  catch_discover_tests(itch_tests PROPERTIES SKIP_RETURN_CODE 4)
endif()

if(ITCH_BUILD_BENCHMARKS)
  add_executable(itch_ingest benchmarks/ingestion_benchmark.cpp)
  target_link_libraries(itch_ingest PRIVATE itch_book)
  add_executable(itch_alloc_latency benchmarks/alloc_latency.cpp)
  target_link_libraries(itch_alloc_latency PRIVATE itch_book)
  target_compile_options(itch_ingest PRIVATE ${ITCH_WARNINGS})
  target_compile_options(itch_alloc_latency PRIVATE ${ITCH_WARNINGS})
endif()
