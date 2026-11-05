fades_iverilog(axi tb_fades_axi OPTIONS -Ptb_fades_axi.CORES=${FADES_CORES}
    -Ptb_fades_axi.LOCKED_CANDIDATES=${FADES_LOCKED_CANDIDATES}
    SOURCES ${FADES_AXI_RTL} "${CMAKE_CURRENT_SOURCE_DIR}/tb_fades_axi.sv")
