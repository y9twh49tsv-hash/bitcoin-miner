cmake_minimum_required(VERSION 3.20)

project(azd_bitcoin_miner
        VERSION 0.1.0
        DESCRIPTION "AZD Bitcoin Miner - SHA-256d miner with an optional CUDA backend"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
#
# CUDA is OFF by default and MUST stay that way: the cloud CI container has no
# nvcc, and the project has to build and test there without it. CUDA is enabled
# explicitly on a machine that has the NVIDIA CUDA Toolkit:
#
#     cmake -S . -B build -DAZD_ENABLE_CUDA=ON
#
option(AZD_ENABLE_CUDA "Build the CUDA GPU mining backend (requires the CUDA Toolkit)" OFF)
option(AZD_ENABLE_NVML "Use NVML for GPU temperature/power telemetry (requires CUDA)" OFF)
option(AZD_BUILD_TESTS "Build the unit tests" ON)
option(AZD_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---------------------------------------------------------------------------
# Core library (portable C++, no CUDA, no third-party dependencies)
# ---------------------------------------------------------------------------

add_library(azd_core STATIC
    src/util/hex.cpp
    src/util/json.cpp
    src/util/logging.cpp
    src/core/config.cpp
    src/bitcoin/sha256.cpp
    src/bitcoin/hash.cpp
    src/bitcoin/uint256.cpp
    src/bitcoin/block_header.cpp
    src/bitcoin/difficulty.cpp
    src/bitcoin/merkle.cpp
    src/mining/work.cpp
    src/mining/stats.cpp
    src/mining/miner.cpp
    src/stratum/message.cpp
    src/stratum/client.cpp
    src/api/server.cpp
)

target_include_directories(azd_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

find_package(Threads REQUIRED)
target_link_libraries(azd_core PUBLIC Threads::Threads)

if(WIN32)
    # Sockets for the stratum client and the dashboard server.
    target_link_libraries(azd_core PUBLIC ws2_32)
endif()

if(MSVC)
    target_compile_options(azd_core PRIVATE /W4 /permissive-)
    target_compile_definitions(azd_core PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    if(AZD_WARNINGS_AS_ERRORS)
        target_compile_options(azd_core PRIVATE /WX)
    endif()
else()
    target_compile_options(azd_core PRIVATE -Wall -Wextra -Wpedantic)
    if(AZD_WARNINGS_AS_ERRORS)
        target_compile_options(azd_core PRIVATE -Werror)
    endif()
endif()

# ---------------------------------------------------------------------------
# CUDA backend (optional)
# ---------------------------------------------------------------------------

if(AZD_ENABLE_CUDA)
    include(CheckLanguage)
    check_language(CUDA)
    if(NOT CMAKE_CUDA_COMPILER)
        message(FATAL_ERROR
            "AZD_ENABLE_CUDA=ON but no CUDA compiler was found.\n"
            "Install the NVIDIA CUDA Toolkit, or configure with -DAZD_ENABLE_CUDA=OFF.")
    endif()

    enable_language(CUDA)
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)

    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
        # 120 = Blackwell (RTX 50 series, including the RTX 5070).
        # 89  = Ada (RTX 40 series), 86 = Ampere (RTX 30 series).
        # Adjust for your card; listing several produces a fatbin that runs on all.
        set(CMAKE_CUDA_ARCHITECTURES 86 89 120)
    endif()

    message(STATUS "CUDA enabled: ${CMAKE_CUDA_COMPILER}")
    message(STATUS "CUDA architectures: ${CMAKE_CUDA_ARCHITECTURES}")

    add_library(azd_cuda STATIC src/cuda/cuda_backend.cu)
    target_include_directories(azd_cuda PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(azd_cuda PUBLIC azd_core)
    set_target_properties(azd_cuda PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
        POSITION_INDEPENDENT_CODE ON)
    target_compile_definitions(azd_cuda PUBLIC AZD_ENABLE_CUDA=1)

    if(AZD_ENABLE_NVML)
        find_library(NVML_LIBRARY NAMES nvml nvidia-ml
                     HINTS ${CUDAToolkit_LIBRARY_DIR}
                           $ENV{CUDA_PATH}/lib/x64
                           /usr/lib/x86_64-linux-gnu)
        if(NVML_LIBRARY)
            target_compile_definitions(azd_cuda PRIVATE AZD_ENABLE_NVML=1)
            target_link_libraries(azd_cuda PRIVATE ${NVML_LIBRARY})
            message(STATUS "NVML telemetry enabled: ${NVML_LIBRARY}")
        else()
            message(WARNING
                "AZD_ENABLE_NVML=ON but NVML was not found. GPU temperature, power and "
                "utilization will be reported as unavailable (never as fake values).")
        endif()
    endif()

    target_link_libraries(azd_core PUBLIC azd_cuda)
else()
    # No CUDA: compile the honest stub. It reports the backend as unavailable
    # and never simulates a device, a hashrate or any telemetry.
    target_sources(azd_core PRIVATE src/cuda/cuda_backend_stub.cpp)
    message(STATUS "CUDA disabled (AZD_ENABLE_CUDA=OFF) - building the portable CPU core only")
endif()

# ---------------------------------------------------------------------------
# Miner executable
# ---------------------------------------------------------------------------

add_executable(azd-miner src/main.cpp)
target_link_libraries(azd-miner PRIVATE azd_core)

# Copy the dashboard next to the binary so `azd-miner` finds it after a build.
add_custom_command(TARGET azd-miner POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/dashboard
            $<TARGET_FILE_DIR:azd-miner>/dashboard
    COMMENT "Copying dashboard files")

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(AZD_BUILD_TESTS)
    enable_testing()

    set(AZD_TEST_SOURCES
        test_sha256
        test_block_header
        test_difficulty
        test_merkle
        test_stratum
        test_work
        test_json
        test_stats
        test_api
    )

    foreach(test_name IN LISTS AZD_TEST_SOURCES)
        add_executable(${test_name} tests/${test_name}.cpp)
        target_link_libraries(${test_name} PRIVATE azd_core)
        target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        add_test(NAME ${test_name} COMMAND ${test_name})
        set_tests_properties(${test_name} PROPERTIES TIMEOUT 120)
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

message(STATUS "")
message(STATUS "AZD Bitcoin Miner ${PROJECT_VERSION}")
message(STATUS "  Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  CUDA       : ${AZD_ENABLE_CUDA}")
message(STATUS "  Tests      : ${AZD_BUILD_TESTS}")
message(STATUS "")
