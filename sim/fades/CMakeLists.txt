add_executable(stream_test stream_test.cpp)
target_link_libraries(stream_test PRIVATE sudoku_reference)
target_compile_definitions(stream_test PRIVATE FADES_CORES=${FADES_CORES})
verilate(stream_test SOURCES ${FADES_STREAM_RTL} TOP_MODULE fades
    PREFIX Vfades VERILATOR_ARGS --assert --language 1800-2017 -GCORES=${FADES_CORES}
    --output-split 20000 --output-split-cfuncs 200)
add_test(NAME fades COMMAND stream_test "${FADES_DATASET}" 0)
set_tests_properties(fades PROPERTIES LABELS fades TIMEOUT 3600)
