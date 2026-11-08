set(FADES_KERNEL_BUILD "" CACHE PATH "Kernel headers (empty uses the running kernel)")
if(FADES_KERNEL_BUILD)
    set(kernel_build "${FADES_KERNEL_BUILD}")
else()
    execute_process(COMMAND uname -r OUTPUT_VARIABLE kernel_release
        OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    set(kernel_build "/lib/modules/${kernel_release}/build")
endif()
if(NOT EXISTS "${kernel_build}/Makefile")
    message(FATAL_ERROR "Kernel headers not found: ${kernel_build}")
endif()

find_program(KBUILD_MAKE_EXECUTABLE NAMES make REQUIRED)
set(module_dir "${CMAKE_CURRENT_BINARY_DIR}/driver")
file(MAKE_DIRECTORY "${module_dir}")
configure_file(driver/fades.c "${module_dir}/fades.c" COPYONLY)
configure_file(include/fades_ioctl.h "${module_dir}/fades_ioctl.h" COPYONLY)
configure_file(driver/Makefile "${module_dir}/Makefile" COPYONLY)
add_custom_target(host-module ALL
    COMMAND "${KBUILD_MAKE_EXECUTABLE}" -C "${kernel_build}" "M=${module_dir}" modules
    BYPRODUCTS "${module_dir}/fades.ko"
    USES_TERMINAL VERBATIM)
