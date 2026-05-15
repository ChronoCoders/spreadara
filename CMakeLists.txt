cmake_minimum_required(VERSION 3.26)
project(spreadara CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(BUILD_TESTS "Build tests" ON)
option(BUILD_BENCHMARKS "Build benchmarks" ON)

add_compile_options(-O3 -march=native -Wall -Wextra -Werror)

include(FetchContent)
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
)
FetchContent_MakeAvailable(tomlplusplus)

find_package(Boost REQUIRED COMPONENTS system)
find_package(simdjson REQUIRED)
find_package(FlatBuffers REQUIRED)
find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

find_program(FLATC_EXECUTABLE flatc REQUIRED)

set(FBS_SRC ${CMAKE_SOURCE_DIR}/src/schemas/market_snapshot.fbs)
set(FBS_GEN ${CMAKE_BINARY_DIR}/generated/market_snapshot_generated.h)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated)
add_custom_command(
    OUTPUT ${FBS_GEN}
    COMMAND ${FLATC_EXECUTABLE} --cpp -o ${CMAKE_BINARY_DIR}/generated ${FBS_SRC}
    DEPENDS ${FBS_SRC}
    VERBATIM
)
add_custom_target(generate_fbs DEPENDS ${FBS_GEN})

set(LIB_SOURCES
    src/infra/config.cpp
    src/infra/logger.cpp
    src/infra/cpu_affinity.cpp
    src/infra/rdtsc.cpp
    src/market_data/orderbook.cpp
    src/market_data/binance_ws_client.cpp
    src/market_data/tick_processor.cpp
)

add_library(spreadara_lib STATIC ${LIB_SOURCES})
add_dependencies(spreadara_lib generate_fbs)

target_include_directories(spreadara_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)
target_include_directories(spreadara_lib SYSTEM PUBLIC
    ${Boost_INCLUDE_DIRS}
)

target_link_libraries(spreadara_lib PUBLIC
    Boost::system
    simdjson::simdjson
    flatbuffers::flatbuffers
    spdlog::spdlog
    fmt::fmt
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    tomlplusplus::tomlplusplus
)

target_compile_definitions(spreadara_lib PUBLIC
    $<$<CONFIG:Release>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO>
)

add_executable(spreadara src/main.cpp)
target_link_libraries(spreadara PRIVATE spreadara_lib)

if(BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
    add_executable(test_orderbook tests/test_orderbook.cpp)
    target_link_libraries(test_orderbook PRIVATE spreadara_lib GTest::gtest GTest::gtest_main)
    add_test(NAME test_orderbook COMMAND test_orderbook)

    add_executable(test_spsc tests/test_spsc.cpp)
    target_link_libraries(test_spsc PRIVATE spreadara_lib GTest::gtest GTest::gtest_main)
    add_test(NAME test_spsc COMMAND test_spsc)
endif()

if(BUILD_BENCHMARKS)
    find_package(benchmark REQUIRED)
    add_executable(bench_tick_processor benchmarks/bench_tick_processor.cpp)
    target_link_libraries(bench_tick_processor PRIVATE spreadara_lib benchmark::benchmark)
endif()
