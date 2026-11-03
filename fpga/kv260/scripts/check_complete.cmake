if(NOT EXISTS "${FADES_IMPLEMENTATION_DIR}/build_complete")
    message(FATAL_ERROR "The board design has not passed implementation/export checks")
endif()
