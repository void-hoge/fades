# Helpers declare only tool mechanics; each test owns its sources and outputs.
function(fades_iverilog name top)
    cmake_parse_arguments(TEST "" "" "SOURCES;OPTIONS" ${ARGN})
    set(output "${CMAKE_CURRENT_BINARY_DIR}/${name}.vvp")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${IVERILOG_EXECUTABLE}" -g2012 -Wall -Wno-sensitivity-entire-array
            -s "${top}" -o "${output}" ${TEST_OPTIONS} ${TEST_SOURCES}
        DEPENDS ${TEST_SOURCES} VERBATIM)
    add_custom_target(${name}-build ALL DEPENDS "${output}")
    add_test(NAME ${name} COMMAND "${VVP_EXECUTABLE}" "${output}")
    get_filename_component(suite "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
    set_tests_properties(${name} PROPERTIES LABELS "${suite}" TIMEOUT 120)
endfunction()

function(fades_formal name top)
    list(TRANSFORM ARGN PREPEND "\"")
    list(TRANSFORM ARGN APPEND "\"")
    string(JOIN " " sources ${ARGN})
    set(script "read_verilog -sv ${sources}; prep -top ${top} -flatten; sat -verify -prove mismatch 0 -show-inputs")
    add_test(NAME ${name} COMMAND "${YOSYS_EXECUTABLE}" -Q -q
        -l "${CMAKE_CURRENT_BINARY_DIR}/${name}.log" -p "${script}")
    get_filename_component(suite "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
    set_tests_properties(${name} PROPERTIES LABELS "${suite}" TIMEOUT 600)
endfunction()
