find_program(VIVADO_EXECUTABLE NAMES vivado HINTS "$ENV{XILINX_VIVADO}/bin" REQUIRED)
find_program(DTC_EXECUTABLE NAMES dtc REQUIRED)
set(FADES_VIVADO_JOBS 4 CACHE STRING "Vivado parallel jobs")
set(out "${CMAKE_CURRENT_BINARY_DIR}")
set(scripts "${CMAKE_CURRENT_SOURCE_DIR}/scripts")
# Operating rates shared by the PS configuration and Linux overlay. Timing
# periods are rounded down to 1 ps, so signoff is never slower than operation.
set(core_hz 115384615)
set(dma_hz 187500000)
foreach(clock IN ITEMS core dma)
    math(EXPR period_ps "1000000000000 / ${${clock}_hz}")
    math(EXPR period_ns "${period_ps} / 1000")
    math(EXPR fraction "${period_ps} % 1000 + 1000")
    string(SUBSTRING "${fraction}" 1 3 fraction)
    set(${clock}_period "${period_ns}.${fraction}")
endforeach()
configure_file(constraints/clocks.xdc.in "${out}/clocks.xdc" @ONLY)
configure_file(scripts/configuration.tcl.in "${out}/configuration.tcl" @ONLY)
configure_file(overlay.dts.in "${out}/overlay.dts" @ONLY)
# Configured input invalidates every FPGA stage when the core count changes.
file(CONFIGURE OUTPUT "${out}/configuration.txt" CONTENT "CORES=@FADES_CORES@\n" @ONLY)
set(inputs ${FADES_AXI_RTL} "${CMAKE_CURRENT_SOURCE_DIR}/rtl/fades_kv260.sv"
    "${scripts}/build.tcl" "${scripts}/export.tcl"
    "${out}/configuration.tcl" "${out}/clocks.xdc"
    "${CMAKE_CURRENT_SOURCE_DIR}/constraints/fan.xdc"
    "${CMAKE_CURRENT_SOURCE_DIR}/constraints/solver_cdc.xdc"
    "${out}/configuration.txt")
foreach(stage IN ITEMS bd synth bit)
    if(stage STREQUAL "bit")
        set(stage_dir "${out}/implementation")
        set(target kv260-bitstream)
        set(byproducts "${stage_dir}/build_complete" "${stage_dir}/fades.bit.bin" "${stage_dir}/fades.xsa")
    else()
        set(stage_dir "${out}/${stage}")
        set(target kv260-${stage})
        set(byproducts)
    endif()
    add_custom_command(OUTPUT "${stage_dir}/complete.stamp"
        BYPRODUCTS ${byproducts}
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${stage_dir}"
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${stage_dir}/complete.stamp" "${stage_dir}/build_complete"
        COMMAND "${VIVADO_EXECUTABLE}" -mode batch -log "${stage_dir}/vivado.log"
            -journal "${stage_dir}/vivado.jou" -source "${scripts}/build.tcl"
            -tclargs "${stage_dir}" ${stage} ${FADES_VIVADO_JOBS} ${FADES_CORES}
                "${out}/configuration.tcl"
        COMMAND "${CMAKE_COMMAND}" -E touch "${stage_dir}/complete.stamp"
        DEPENDS ${inputs} WORKING_DIRECTORY "${out}" USES_TERMINAL VERBATIM)
    add_custom_target(${target} DEPENDS "${stage_dir}/complete.stamp")
endforeach()
set(firmware "${out}/firmware/fades")
add_custom_command(OUTPUT "${firmware}/fades.bit.bin" "${firmware}/fades.dtbo" "${firmware}/shell.json"
    COMMAND "${CMAKE_COMMAND}" -DFADES_IMPLEMENTATION_DIR=${out}/implementation
        -P "${scripts}/check_complete.cmake"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${firmware}"
    COMMAND "${DTC_EXECUTABLE}" -@ -I dts -O dtb -o "${firmware}/fades.dtbo"
        "${out}/overlay.dts"
    COMMAND "${CMAKE_COMMAND}" -E copy "${out}/implementation/fades.bit.bin"
        "${CMAKE_CURRENT_SOURCE_DIR}/shell.json" "${firmware}"
    DEPENDS "${out}/implementation/complete.stamp" "${out}/implementation/build_complete"
        "${out}/implementation/fades.bit.bin" "${out}/overlay.dts"
        "${CMAKE_CURRENT_SOURCE_DIR}/shell.json" "${scripts}/check_complete.cmake"
    VERBATIM)
add_custom_target(kv260-package ALL
    DEPENDS "${firmware}/fades.bit.bin" "${firmware}/fades.dtbo" "${firmware}/shell.json")
