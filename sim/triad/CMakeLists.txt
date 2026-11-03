add_executable(triad_test triad_test.cpp)
add_executable(triad_benchmark triad_benchmark.cpp)
target_link_libraries(triad_benchmark PRIVATE sudoku_reference)
target_link_libraries(triad_test PRIVATE sudoku_reference)
verilate(triad_test SOURCES ${FADES_CORE_RTL} "${CMAKE_CURRENT_SOURCE_DIR}/triad_test_top.sv"
    TOP_MODULE triad_test_top PREFIX Vtriad_test_top
    VERILATOR_ARGS --assert --language 1800-2017 --output-split 20000 --output-split-cfuncs 200)
add_test(NAME triad-model COMMAND triad_test "${FADES_DATASET}")
set_tests_properties(triad-model PROPERTIES LABELS "triad;deduction_engine;branch_generator" TIMEOUT 600)
fades_formal(formal-triad-box formal_triad_box
    "${PROJECT_SOURCE_DIR}/rtl/triad_engine.sv" "${CMAKE_CURRENT_SOURCE_DIR}/formal_triad_box.sv")
fades_formal(formal-triad-branch formal_triad_branch
    "${PROJECT_SOURCE_DIR}/rtl/triad_engine.sv" "${CMAKE_CURRENT_SOURCE_DIR}/formal_triad_branch.sv")
fades_formal(formal-triad-split formal_triad_split
    "${PROJECT_SOURCE_DIR}/rtl/triad_engine.sv" "${CMAKE_CURRENT_SOURCE_DIR}/formal_triad_split.sv")
