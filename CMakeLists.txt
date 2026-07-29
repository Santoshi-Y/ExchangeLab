cmake_minimum_required(VERSION 3.20)

project(
    ExchangeLab
    VERSION 0.1.0
    DESCRIPTION "A C++20 limit-order matching engine"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Header-only ExchangeLab library.
add_library(exchange_lab_lib INTERFACE)

target_include_directories(
    exchange_lab_lib
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(
    exchange_lab_lib
    INTERFACE
        cxx_std_20
)

# Demo executable.
add_executable(
    exchange_lab
    src/main.cpp
)

target_link_libraries(
    exchange_lab
    PRIVATE
        exchange_lab_lib
)

# Helpful compiler warnings.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(
        exchange_lab
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
endif()

# Testing is enabled by default.
option(EXCHANGELAB_BUILD_TESTS "Build ExchangeLab tests" ON)

if(EXCHANGELAB_BUILD_TESTS)
    include(CTest)
    enable_testing()

    include(FetchContent)

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
    )

    target_link_libraries(
        exchange_lab_tests
        PRIVATE
            exchange_lab_lib
            GTest::gtest_main
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