set(rtl "${PROJECT_SOURCE_DIR}/rtl/branch_generator.sv")
fades_iverilog(branch-generator tb_branch_generator SOURCES "${rtl}" "${CMAKE_CURRENT_SOURCE_DIR}/tb_branch_generator.sv")
fades_formal(formal-branch-generator formal_branch_generator "${rtl}" "${CMAKE_CURRENT_SOURCE_DIR}/formal_branch_generator.sv")
