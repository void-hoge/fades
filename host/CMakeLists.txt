foreach(tool IN ITEMS fades_verify fades_benchmark fades_stream)
    add_executable(${tool} tools/${tool}.cpp)
    target_link_libraries(${tool} PRIVATE sudoku_reference)
    target_include_directories(${tool} PRIVATE include)
    target_compile_options(${tool} PRIVATE -Wall -Wextra -Werror)
    if(FADES_BUILD_DRIVER AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
        target_compile_options(${tool} PRIVATE -mcpu=cortex-a53)
    endif()
endforeach()

if(FADES_BUILD_DRIVER)
    set(FADES_KERNEL_BUILD "" CACHE PATH "Kernel build directory (empty uses the running kernel)")
    if(FADES_KERNEL_BUILD)
        set(kernel_build "${FADES_KERNEL_BUILD}")
    else()
        execute_process(COMMAND uname -r OUTPUT_VARIABLE kernel_release
            OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
        set(kernel_build "/lib/modules/${kernel_release}/build")
    endif()
    if(NOT EXISTS "${kernel_build}/Makefile")
        message(FATAL_ERROR "Kernel headers not found at ${kernel_build}. Install headers for the running kernel or set FADES_KERNEL_BUILD.")
    endif()
    find_program(KBUILD_MAKE_EXECUTABLE NAMES make REQUIRED)
    set(module_dir "${CMAKE_CURRENT_BINARY_DIR}/driver")
    file(MAKE_DIRECTORY "${module_dir}")
    # Copy only when contents change so Kbuild can keep unchanged objects.
    configure_file(driver/fades.c "${module_dir}/fades.c" COPYONLY)
    configure_file(include/fades_ioctl.h "${module_dir}/fades_ioctl.h" COPYONLY)
    configure_file(driver/Makefile "${module_dir}/Makefile" COPYONLY)
    # Let Kbuild check its own header/config dependencies on every invocation.
    add_custom_target(host-module ALL
        COMMAND "${KBUILD_MAKE_EXECUTABLE}" -C "${kernel_build}" "M=${module_dir}" modules
        BYPRODUCTS "${module_dir}/fades.ko"
        USES_TERMINAL VERBATIM)
endif()

if(BUILD_TESTING)
    # Built everywhere, run explicitly only with a loaded KV260 device.
    add_executable(solver_stream_board_test tests/solver_stream_board_test.cpp)
    target_include_directories(solver_stream_board_test PRIVATE include)
    target_compile_features(solver_stream_board_test PRIVATE cxx_std_20)
    target_compile_options(solver_stream_board_test PRIVATE -Wall -Wextra -Werror)
    add_executable(stream_helpers_test tests/stream_helpers_test.cpp)
    target_include_directories(stream_helpers_test PRIVATE include)
    target_compile_features(stream_helpers_test PRIVATE cxx_std_20)
    target_compile_options(stream_helpers_test PRIVATE -Wall -Wextra -Werror)
    target_link_options(stream_helpers_test PRIVATE
        -Wl,--wrap=open,--wrap=close,--wrap=ioctl,--wrap=mmap,--wrap=munmap)
    add_test(NAME host-stream-helpers COMMAND stream_helpers_test)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME host-input COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_input.py" "$<TARGET_FILE_DIR:fades_verify>")
    set_tests_properties(host-input PROPERTIES TIMEOUT 60)
endif()
