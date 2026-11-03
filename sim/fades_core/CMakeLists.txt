add_executable(core_test core_test.cpp)
target_link_libraries(core_test PRIVATE sudoku_reference)
verilate(core_test SOURCES ${FADES_CORE_RTL} TOP_MODULE fades_core
    PREFIX Vfades_core VERILATOR_ARGS --assert --language 1800-2017
    --output-split 20000 --output-split-cfuncs 200)
foreach(mode IN ITEMS quick full)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${mode}")
    if(mode STREQUAL "quick")
        set(limit 512)
    else()
        set(limit 0)
    endif()
    add_test(NAME core-${mode} COMMAND core_test "${FADES_DATASET}"
        "${CMAKE_CURRENT_BINARY_DIR}/${mode}/results.csv" ${limit})
    set_tests_properties(core-${mode} PROPERTIES LABELS ${mode} TIMEOUT 1800)
endforeach()
add_custom_target(core-summary COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/core_results.py"
    summary "${CMAKE_CURRENT_BINARY_DIR}/full/results.csv" "${CMAKE_CURRENT_BINARY_DIR}/full/summary.json"
    VERBATIM)
if(FADES_BASELINE)
    add_custom_target(regression COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/core_results.py"
        compare "${FADES_BASELINE}" "${CMAKE_CURRENT_BINARY_DIR}/full/results.csv" VERBATIM)
endif()
set(synth_sources ${FADES_CORE_RTL})
list(TRANSFORM synth_sources PREPEND "\"")
list(TRANSFORM synth_sources APPEND "\"")
string(JOIN " " rtl_sources ${synth_sources})
add_test(NAME synth-core COMMAND "${YOSYS_EXECUTABLE}" -Q -q -l "${CMAKE_CURRENT_BINARY_DIR}/synth.log"
    -p "read_verilog -sv ${rtl_sources}; hierarchy -check -top fades_core; proc; opt; memory_dff; memory_collect; opt_clean; check -assert; select -assert-none t:$dlatch t:$_DLATCH_*; stat")
set_tests_properties(synth-core PROPERTIES LABELS "quick;synth" TIMEOUT 120)
add_custom_target(synth COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure -R "^synth-core$"
    WORKING_DIRECTORY "${PROJECT_BINARY_DIR}" USES_TERMINAL)
