cmake_minimum_required(VERSION 3.25)
project(fades LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(BUILD_TESTING "Build and register tests" OFF)
option(FADES_BUILD_HOST "Build Linux userspace tools" ON)
option(FADES_BUILD_SIM "Build RTL simulations and checks" OFF)
option(FADES_BUILD_FPGA "Enable KV260 implementation targets" OFF)
set(FADES_CORES 6 CACHE STRING "Number of solver cores (1..6)")
if(NOT FADES_CORES MATCHES "^[1-6]$")
    message(FATAL_ERROR "FADES_CORES must be an integer between 1 and 6")
endif()
include(CTest)

add_library(sudoku_reference INTERFACE)
target_include_directories(sudoku_reference INTERFACE "${PROJECT_SOURCE_DIR}/reference")
set(FADES_CORE_RTL
    "${PROJECT_SOURCE_DIR}/rtl/deduction_engine.sv"
    "${PROJECT_SOURCE_DIR}/rtl/branch_generator.sv"
    "${PROJECT_SOURCE_DIR}/rtl/fades_core.sv")
set(FADES_STREAM_RTL ${FADES_CORE_RTL}
    "${PROJECT_SOURCE_DIR}/rtl/async_queue.sv"
    "${PROJECT_SOURCE_DIR}/rtl/fades.sv")
set(FADES_AXI_RTL ${FADES_STREAM_RTL} "${PROJECT_SOURCE_DIR}/rtl/fades_axi.sv")
if(FADES_BUILD_HOST)
    add_subdirectory(host)
endif()
if(FADES_BUILD_SIM)
    if(NOT BUILD_TESTING)
        message(FATAL_ERROR "FADES_BUILD_SIM requires BUILD_TESTING=ON")
    endif()
    add_subdirectory(sim)
endif()
if(FADES_BUILD_FPGA)
    add_subdirectory(fpga/kv260)
endif()
