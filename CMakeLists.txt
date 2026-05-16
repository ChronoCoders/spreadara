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

# WHY: Phase-6 AddressSanitizer / UndefinedBehaviorSanitizer build type. Use:
#   cmake -DCMAKE_BUILD_TYPE=Asan ..
# CMAKE_CXX_FLAGS_ASAN drops to -O1 to keep stacks readable while sanitizers run.
set(CMAKE_CXX_FLAGS_ASAN
    "-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined"
    CACHE STRING "Asan build flags" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_ASAN
    "-fsanitize=address,undefined"
    CACHE STRING "Asan link flags" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_ASAN
    "-fsanitize=address,undefined"
    CACHE STRING "Asan shared link flags" FORCE)
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    Debug Release RelWithDebInfo MinSizeRel Asan)

# WHY: keep Release strict (-Werror); under Asan, sanitizer-injected paths may
# produce -Wmaybe-uninitialized at -O1, so drop -Werror to avoid spurious failures.
if(CMAKE_BUILD_TYPE STREQUAL "Asan")
    add_compile_options(-march=native -Wall -Wextra)
else()
    add_compile_options(-O3 -march=native -Wall -Wextra -Werror)
endif()

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
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPQXX REQUIRED IMPORTED_TARGET libpqxx)

find_program(FLATC_EXECUTABLE flatc REQUIRED)

set(FBS_SRC_SNAPSHOT ${CMAKE_SOURCE_DIR}/src/schemas/market_snapshot.fbs)
set(FBS_GEN_SNAPSHOT ${CMAKE_BINARY_DIR}/generated/market_snapshot_generated.h)
set(FBS_SRC_QUOTE ${CMAKE_SOURCE_DIR}/src/schemas/quote_update.fbs)
set(FBS_GEN_QUOTE ${CMAKE_BINARY_DIR}/generated/quote_update_generated.h)
set(FBS_SRC_FILL ${CMAKE_SOURCE_DIR}/src/schemas/fill_event.fbs)
set(FBS_GEN_FILL ${CMAKE_BINARY_DIR}/generated/fill_event_generated.h)
set(FBS_SRC_RISK ${CMAKE_SOURCE_DIR}/src/schemas/risk_event.fbs)
set(FBS_GEN_RISK ${CMAKE_BINARY_DIR}/generated/risk_event_generated.h)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated)
add_custom_command(
    OUTPUT ${FBS_GEN_SNAPSHOT}
    COMMAND ${FLATC_EXECUTABLE} --cpp -o ${CMAKE_BINARY_DIR}/generated ${FBS_SRC_SNAPSHOT}
    DEPENDS ${FBS_SRC_SNAPSHOT}
    VERBATIM
)
add_custom_command(
    OUTPUT ${FBS_GEN_QUOTE}
    COMMAND ${FLATC_EXECUTABLE} --cpp -o ${CMAKE_BINARY_DIR}/generated ${FBS_SRC_QUOTE}
    DEPENDS ${FBS_SRC_QUOTE}
    VERBATIM
)
add_custom_command(
    OUTPUT ${FBS_GEN_FILL}
    COMMAND ${FLATC_EXECUTABLE} --cpp -o ${CMAKE_BINARY_DIR}/generated ${FBS_SRC_FILL}
    DEPENDS ${FBS_SRC_FILL}
    VERBATIM
)
add_custom_command(
    OUTPUT ${FBS_GEN_RISK}
    COMMAND ${FLATC_EXECUTABLE} --cpp -o ${CMAKE_BINARY_DIR}/generated ${FBS_SRC_RISK}
    DEPENDS ${FBS_SRC_RISK}
    VERBATIM
)
add_custom_target(generate_fbs DEPENDS ${FBS_GEN_SNAPSHOT} ${FBS_GEN_QUOTE} ${FBS_GEN_FILL} ${FBS_GEN_RISK})

set(LIB_SOURCES
    src/infra/config.cpp
    src/infra/logger.cpp
    src/infra/cpu_affinity.cpp
    src/infra/rdtsc.cpp
    src/market_data/orderbook.cpp
    src/market_data/binance_ws_client.cpp
    src/market_data/okx_ws_client.cpp
    src/market_data/tick_processor.cpp
    src/strategy/inventory_manager.cpp
    src/strategy/signal_aggregator.cpp
    src/strategy/spread_model.cpp
    src/strategy/market_maker.cpp
    src/risk/position_tracker.cpp
    src/risk/risk_manager.cpp
    src/risk/circuit_breaker.cpp
    src/execution/rest_client.cpp
    src/execution/okx_rest_client.cpp
    src/execution/order_manager.cpp
    src/execution/fix_client.cpp
    src/execution/simulated_rest_client.cpp
    src/db/pg_reporter.cpp
    src/dashboard/dashboard_server.cpp
    src/backtest/historical_data_loader.cpp
    src/backtest/replay_engine.cpp
    src/backtest/backtest_reporter.cpp
    src/backtest/backtest_runner.cpp
    src/backtest/recorder.cpp
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
    PkgConfig::LIBPQXX
)

target_compile_definitions(spreadara_lib PUBLIC
    $<$<CONFIG:Release>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO>
)

add_executable(spreadara src/main.cpp)
target_link_libraries(spreadara PRIVATE spreadara_lib)

# WHY: standalone Binance CSV → length-prefixed FB-archive converter. Not a
# library member; lives in tools/ so we can build it without coupling the
# main executable to the loader's CLI affordances.
add_executable(convert_binance_csv tools/convert_binance_csv.cpp)
target_link_libraries(convert_binance_csv PRIVATE spreadara_lib)

if(BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
    add_executable(test_orderbook tests/test_orderbook.cpp)
    target_link_libraries(test_orderbook PRIVATE spreadara_lib GTest::gtest GTest::gtest_main)
    add_test(NAME test_orderbook COMMAND test_orderbook)

    add_executable(test_spsc tests/test_spsc.cpp)
    target_link_libraries(test_spsc PRIVATE spreadara_lib GTest::gtest GTest::gtest_main)
    add_test(NAME test_spsc COMMAND test_spsc)

    add_executable(test_strategy tests/test_strategy.cpp)
    target_link_libraries(test_strategy PRIVATE spreadara_lib GTest::gtest GTest::gtest_main)
    add_test(NAME test_strategy COMMAND test_strategy)

    add_executable(test_risk tests/test_risk.cpp)
    target_link_libraries(test_risk PRIVATE spreadara_lib GTest::gtest GTest::gtest_main)
    add_test(NAME test_risk COMMAND test_risk)

    add_executable(test_execution tests/test_execution.cpp)
    target_link_libraries(test_execution PRIVATE spreadara_lib GTest::gtest GTest::gtest_main)
    add_test(NAME test_execution COMMAND test_execution)

    add_executable(test_reporter tests/test_reporter.cpp)
    target_link_libraries(test_reporter PRIVATE spreadara_lib GTest::gtest GTest::gtest_main)
    add_test(NAME test_reporter COMMAND test_reporter)
    # WHY: peer auth on this WSL Postgres uses the calling user. If the env var
    # is set externally we honour it; the default below works when role
    # 'chronocoder' + db 'chronocoder' exist (see README "Postgres setup for tests").
    set_tests_properties(test_reporter PROPERTIES
        ENVIRONMENT "SPREADARA_PG_DSN=host=/var/run/postgresql dbname=chronocoder user=chronocoder")

    # Phase-6 unit tests for backtest building blocks.
    add_executable(test_backtest tests/test_backtest.cpp)
    target_link_libraries(test_backtest PRIVATE spreadara_lib GTest::gtest GTest::gtest_main)
    add_test(NAME test_backtest COMMAND test_backtest)

    # Phase-6 end-to-end integration test. Run from repo root so the synthetic
    # fixture path under data/historical/ is discoverable.
    add_executable(test_integration tests/test_integration.cpp)
    target_link_libraries(test_integration PRIVATE spreadara_lib GTest::gtest GTest::gtest_main)
    add_test(NAME test_integration
             COMMAND test_integration
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

if(BUILD_BENCHMARKS)
    find_package(benchmark REQUIRED)
    add_executable(bench_tick_processor benchmarks/bench_tick_processor.cpp)
    target_link_libraries(bench_tick_processor PRIVATE spreadara_lib benchmark::benchmark)
endif()
