fades_iverilog(axi tb_fades_axi OPTIONS -Ptb_fades_axi.CORES=${FADES_CORES}
    -Ptb_fades_axi.SEGMENT_RULES=${FADES_SEGMENT_RULES}
    SOURCES ${FADES_AXI_RTL} "${CMAKE_CURRENT_SOURCE_DIR}/tb_fades_axi.sv")
