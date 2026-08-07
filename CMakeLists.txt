cmake_minimum_required(VERSION 3.20)

project(
    ExchangeLab
    VERSION 0.1.0
    DESCRIPTION "A C++20 exchange matching engine"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

find_package(Threads REQUIRED)

add_library(
    exchange_lab_lib
    src/tcp_server.cpp
    src/exchange_server.cpp
    src/multicast_publisher.cpp
    src/journal.cpp
    src/replay.cpp
)

target_include_directories(
    exchange_lab_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(
    exchange_lab_lib
    PUBLIC
        cxx_std_20
)

target_link_libraries(
    exchange_lab_lib
    PUBLIC
        Threads::Threads
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(
        exchange_lab_lib
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )
elseif(MSVC)
    target_compile_options(
        exchange_lab_lib
        PRIVATE
            /W4
    )
endif()

add_executable(
    exchange_lab
    src/main.cpp
)

target_link_libraries(
    exchange_lab
    PRIVATE
        exchange_lab_lib
)

add_executable(
    exchange_replay
    tools/exchange_replay.cpp
)

target_link_libraries(
    exchange_replay
    PRIVATE
        exchange_lab_lib
)

add_executable(
    exchange_client
    tools/exchange_client.cpp
)

target_link_libraries(
    exchange_client
    PRIVATE
        exchange_lab_lib
)

add_executable(
    exchange_market_data_listener
    tools/exchange_market_data_listener.cpp
)

target_link_libraries(
    exchange_market_data_listener
    PRIVATE
        exchange_lab_lib
)

add_executable(
    exchange_websocket_gateway
    tools/exchange_websocket_gateway.cpp
    src/websocket_gateway.cpp
)

target_link_libraries(
    exchange_websocket_gateway
    PRIVATE
        exchange_lab_lib
        Threads::Threads
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(
        exchange_lab
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )

    target_compile_options(
        exchange_replay
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )

    target_compile_options(
        exchange_client
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )

    target_compile_options(
        exchange_market_data_listener
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )

    target_compile_options(
        exchange_websocket_gateway
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )
elseif(MSVC)
    target_compile_options(
        exchange_lab
        PRIVATE
            /W4
    )

    target_compile_options(
        exchange_replay
        PRIVATE
            /W4
    )

    target_compile_options(
        exchange_client
        PRIVATE
            /W4
    )

    target_compile_options(
        exchange_market_data_listener
        PRIVATE
            /W4
    )

    target_compile_options(
        exchange_websocket_gateway
        PRIVATE
            /W4
    )
endif()

option(
    EXCHANGELAB_BUILD_TESTS
    "Build ExchangeLab tests"
    ON
)

option(
    EXCHANGELAB_BUILD_BENCHMARKS
    "Build ExchangeLab benchmarks"
    ON
)

if(EXCHANGELAB_BUILD_TESTS)
    include(CTest)
    enable_testing()

    FetchContent_Declare(
        googletest
        URL
            https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )

    FetchContent_MakeAvailable(googletest)

    add_executable(
        exchange_lab_tests
        tests/price_level_test.cpp
        tests/order_book_test.cpp
        tests/matching_engine_test.cpp
        tests/order_replace_test.cpp
        tests/protocol_test.cpp
        tests/tcp_server_test.cpp
        tests/exchange_server_test.cpp
        tests/replay_test.cpp
        tests/startup_recovery_test.cpp
        tests/multicast_publisher_test.cpp
        tests/websocket_gateway_test.cpp
        tests/multi_symbol_test.cpp
        src/websocket_gateway.cpp
    )

    target_link_libraries(
        exchange_lab_tests
        PRIVATE
            exchange_lab_lib
            GTest::gtest_main
            Threads::Threads
    )

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(
            exchange_lab_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
    elseif(MSVC)
        target_compile_options(
            exchange_lab_tests
            PRIVATE
                /W4
        )
    endif()

    include(GoogleTest)

    gtest_discover_tests(exchange_lab_tests)
endif()

if(EXCHANGELAB_BUILD_BENCHMARKS)
    set(
        BENCHMARK_ENABLE_TESTING
        OFF
        CACHE BOOL
        "Disable Google Benchmark tests"
        FORCE
    )

    set(
        BENCHMARK_ENABLE_GTEST_TESTS
        OFF
        CACHE BOOL
        "Disable Google Benchmark GoogleTest tests"
        FORCE
    )

    set(
        BENCHMARK_ENABLE_INSTALL
        OFF
        CACHE BOOL
        "Disable Google Benchmark installation"
        FORCE
    )

    FetchContent_Declare(
        googlebenchmark
        URL
            https://github.com/google/benchmark/archive/refs/tags/v1.9.5.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )

    FetchContent_MakeAvailable(googlebenchmark)

    add_executable(
        exchange_lab_benchmarks
        benchmarks/exchange_benchmark.cpp
    )

    target_link_libraries(
        exchange_lab_benchmarks
        PRIVATE
            exchange_lab_lib
            benchmark::benchmark_main
            Threads::Threads
    )

    add_executable(
        exchange_lab_latency_benchmark
        benchmarks/end_to_end_latency.cpp
    )

    target_link_libraries(
        exchange_lab_latency_benchmark
        PRIVATE
            exchange_lab_lib
            Threads::Threads
    )

    add_executable(
        exchange_lab_execution_latency
        benchmarks/end_to_end_execution_latency.cpp
    )

    target_link_libraries(
        exchange_lab_execution_latency
        PRIVATE
            exchange_lab_lib
            Threads::Threads
    )

    add_executable(
        exchange_lab_allocation_profile
        benchmarks/allocation_profile.cpp
    )

    target_link_libraries(
        exchange_lab_allocation_profile
        PRIVATE
            exchange_lab_lib
            Threads::Threads
    )

    add_executable(
        exchange_lab_throughput_benchmark
        benchmarks/throughput_benchmark.cpp
    )

    target_link_libraries(
        exchange_lab_throughput_benchmark
        PRIVATE
            exchange_lab_lib
            Threads::Threads
    )

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(
            exchange_lab_throughput_benchmark
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )

        target_compile_options(
            exchange_lab_benchmarks
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )

        target_compile_options(
            exchange_lab_latency_benchmark
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )

        target_compile_options(
            exchange_lab_execution_latency
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )

        target_compile_options(
            exchange_lab_allocation_profile
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
    elseif(MSVC)
        target_compile_options(
            exchange_lab_throughput_benchmark
            PRIVATE
                /W4
        )

        target_compile_options(
            exchange_lab_benchmarks
            PRIVATE
                /W4
        )

        target_compile_options(
            exchange_lab_latency_benchmark
            PRIVATE
                /W4
        )

        target_compile_options(
            exchange_lab_execution_latency
            PRIVATE
                /W4
        )

        target_compile_options(
            exchange_lab_allocation_profile
            PRIVATE
                /W4
        )
    endif()
endif()