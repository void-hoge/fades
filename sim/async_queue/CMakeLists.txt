find_program(XVLOG_EXECUTABLE NAMES xvlog HINTS "$ENV{XILINX_VIVADO}/bin")
if(XVLOG_EXECUTABLE)
    get_filename_component(xsim_bin "${XVLOG_EXECUTABLE}" DIRECTORY)
    add_test(NAME async-queue-xpm COMMAND "${CMAKE_COMMAND}" -E env "PATH=${xsim_bin}:$ENV{PATH}"
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/xpm_fifo_test.py"
        "${CMAKE_CURRENT_BINARY_DIR}/xsim")
    set_tests_properties(async-queue-xpm PROPERTIES LABELS async_queue TIMEOUT 600)
    add_custom_target(fifo-test COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure -L "^async_queue$"
        WORKING_DIRECTORY "${PROJECT_BINARY_DIR}" USES_TERMINAL VERBATIM)
else()
    message(STATUS "xvlog not found: XPM test disabled; set XVLOG_EXECUTABLE to enable it")
endif()
