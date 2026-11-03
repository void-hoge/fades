add_executable(stream_test stream_test.cpp)
target_link_libraries(stream_test PRIVATE sudoku_reference)
target_compile_definitions(stream_test PRIVATE FADES_CORES=${FADES_CORES})
verilate(stream_test SOURCES ${FADES_STREAM_RTL} TOP_MODULE fades
    PREFIX Vfades VERILATOR_ARGS --assert --language 1800-2017 -GCORES=${FADES_CORES}
    -GSEGMENT_RULES=${FADES_SEGMENT_RULES}
    --output-split 20000 --output-split-cfuncs 200)
add_test(NAME fades COMMAND stream_test "${FADES_DATASET}" 0)
set_tests_properties(fades PROPERTIES LABELS fades TIMEOUT 3600)
add_test(NAME fades-hard COMMAND stream_test "${PROJECT_SOURCE_DIR}/sim/data/HardestDatabase110626.txt" 0)
set_tests_properties(fades-hard PROPERTIES LABELS fades TIMEOUT 3600)
