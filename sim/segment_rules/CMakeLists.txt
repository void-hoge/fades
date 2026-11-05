add_executable(segment_benchmark segment_benchmark.cpp)
target_link_libraries(segment_benchmark PRIVATE sudoku_reference)
fades_formal(formal-segment-pairs formal_segment_pairs
    "${PROJECT_SOURCE_DIR}/rtl/deduction_engine.sv"
    "${CMAKE_CURRENT_SOURCE_DIR}/formal_segment_pairs.sv")
fades_formal(formal-pair-factorization formal_pair_factorization
    "${PROJECT_SOURCE_DIR}/rtl/deduction_engine.sv"
    "${CMAKE_CURRENT_SOURCE_DIR}/formal_segment_pairs.sv")
fades_formal(formal-bivalue-pairs formal_bivalue_pairs
    "${PROJECT_SOURCE_DIR}/rtl/deduction_engine.sv"
    "${CMAKE_CURRENT_SOURCE_DIR}/formal_segment_pairs.sv")
fades_formal(formal-pair-two-rounds formal_pair_two_rounds
    "${PROJECT_SOURCE_DIR}/rtl/deduction_engine.sv"
    "${CMAKE_CURRENT_SOURCE_DIR}/formal_segment_pairs.sv")
foreach(mode RANGE 0 4)
    set(target "segment_model_test_${mode}")
    add_executable(${target} segment_model_test.cpp)
    target_link_libraries(${target} PRIVATE sudoku_reference)
    target_compile_definitions(${target} PRIVATE SEGMENT_RULES_MODE=${mode})
    verilate(${target} SOURCES "${PROJECT_SOURCE_DIR}/rtl/deduction_engine.sv"
        TOP_MODULE deduction_engine PREFIX Vdeduction_engine
        VERILATOR_ARGS --assert --language 1800-2017 -GSEGMENT_RULES=${mode})
    add_test(NAME segment-model-${mode} COMMAND ${target})
    set_tests_properties(segment-model-${mode} PROPERTIES LABELS segment_rules TIMEOUT 120)
endforeach()

foreach(mode RANGE 1 4)
    set(target "core_test_segment_${mode}")
    add_executable(${target} "${PROJECT_SOURCE_DIR}/sim/fades_core/core_test.cpp")
    target_link_libraries(${target} PRIVATE sudoku_reference)
    target_compile_definitions(${target} PRIVATE FADES_SEGMENT_RULES=${mode})
    verilate(${target} SOURCES ${FADES_CORE_RTL} TOP_MODULE fades_core
        PREFIX Vfades_core VERILATOR_ARGS --assert --language 1800-2017
        -GSEGMENT_RULES=${mode} --output-split 20000 --output-split-cfuncs 200)
    add_test(NAME segment-core-${mode} COMMAND ${target} "${FADES_DATASET}"
        "${CMAKE_CURRENT_BINARY_DIR}/core-${mode}.csv" 0)
    add_test(NAME segment-core-hard-${mode} COMMAND ${target}
        "${PROJECT_SOURCE_DIR}/sim/data/HardestDatabase110626.txt"
        "${CMAKE_CURRENT_BINARY_DIR}/core-hard-${mode}.csv" 0)
    set_tests_properties(segment-core-${mode} segment-core-hard-${mode}
        PROPERTIES LABELS segment_rules TIMEOUT 1800)
    add_test(NAME lint-segment-core-${mode} COMMAND "${VERILATOR_BIN}" --lint-only
        --language 1800-2017 -Wall --top-module fades_core -GSEGMENT_RULES=${mode} ${FADES_CORE_RTL})
    set_tests_properties(lint-segment-core-${mode} PROPERTIES LABELS segment_rules TIMEOUT 120)
endforeach()
