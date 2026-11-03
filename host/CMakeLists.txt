foreach(tool IN ITEMS fades_verify fades_benchmark)
    add_executable(${tool} tools/${tool}.cpp)
    target_link_libraries(${tool} PRIVATE sudoku_reference)
    target_include_directories(${tool} PRIVATE include)
    target_compile_options(${tool} PRIVATE -Wall -Wextra -Werror)
endforeach()

set(FADES_KERNEL_BUILD "" CACHE PATH "Kernel build directory (empty uses the running kernel)")
find_program(KBUILD_MAKE_EXECUTABLE NAMES make REQUIRED)
set(module_dir "${CMAKE_CURRENT_BINARY_DIR}/driver")
set(kbuild_args)
if(FADES_KERNEL_BUILD)
    list(APPEND kbuild_args "KDIR=${FADES_KERNEL_BUILD}")
endif()
add_custom_target(host-module
    COMMAND "${KBUILD_MAKE_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}/driver"
        "OUT=${module_dir}" ${kbuild_args}
    BYPRODUCTS "${module_dir}/fades.ko"
    USES_TERMINAL VERBATIM)

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME host-input COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_input.py" "$<TARGET_FILE_DIR:fades_verify>")
    set_tests_properties(host-input PROPERTIES LABELS quick TIMEOUT 60)
endif()
