# Full KV260 board design, independent of the solver OOC flow.
# Arguments: output directory, stage (bd/synth/bit/export), parallel jobs.
set root [file normalize [file dirname [info script]]/../..]
set design_name fades
set app_name fades
set out [file normalize [expr {$argc > 0 ? [lindex $argv 0] : "$root/build/kv260"}]]
set stage [expr {$argc > 1 ? [lindex $argv 1] : "bit"}]
set jobs [expr {$argc > 2 ? [lindex $argv 2] : 4}]
if {$stage ni {bd synth bit export}} { error "Stage must be bd, synth, bit or export" }
set boards [file join $::env(XILINX_VIVADO) data xhub boards XilinxBoardStore boards Xilinx]
if {![file isdirectory $boards]} { error "KV260 board repository not found: $boards" }
set_param board.repoPaths [list $boards]
set_param general.maxThreads $jobs
file mkdir $out
if {$stage eq "export"} {
    source $root/fpga/scripts/vivado_kv260_export.tcl
    exit
}
file delete -force $out/build_complete
create_project -force $design_name $out/project -part xck26-sfvc784-2LV-c
set_property board_part xilinx.com:kv260_som:part0:1.4 [current_project]
set_property XPM_LIBRARIES {XPM_CDC XPM_MEMORY XPM_FIFO} [current_project]
add_files [list $root/rtl/inference.sv $root/rtl/branch.sv $root/rtl/core.sv \
    $root/rtl/async_fifo.sv $root/rtl/solver_stream.sv $root/rtl/solver_axi.sv \
    $root/fpga/kv260/fades_kv260.v]
update_compile_order -fileset sources_1
add_files -fileset constrs_1 $root/fpga/constraints/solver_cdc.xdc
set_property USED_IN_SYNTHESIS false [get_files solver_cdc.xdc]
set_property PROCESSING_ORDER LATE [get_files solver_cdc.xdc]
create_bd_design system

create_bd_cell -type ip -vlnv xilinx.com:ip:zynq_ultra_ps_e:3.5 ps
apply_bd_automation -rule xilinx.com:bd_rule:zynq_ultra_ps_e \
    -config {apply_board_preset "1"} [get_bd_cells ps]
set_property -dict [list \
    CONFIG.PSU__USE__M_AXI_GP0 {1} \
    CONFIG.PSU__USE__M_AXI_GP1 {0} \
    CONFIG.PSU__USE__M_AXI_GP2 {0} \
    CONFIG.PSU__USE__S_AXI_GP0 {0} \
    CONFIG.PSU__USE__S_AXI_GP1 {0} \
    CONFIG.PSU__USE__S_AXI_GP2 {1} \
    CONFIG.PSU__USE__S_AXI_GP3 {1} \
    CONFIG.PSU__USE__S_AXI_GP4 {0} \
    CONFIG.PSU__USE__S_AXI_GP5 {0} \
    CONFIG.PSU__USE__S_AXI_GP6 {0} \
    CONFIG.PSU__SAXIGP2__DATA_WIDTH {128} \
    CONFIG.PSU__SAXIGP3__DATA_WIDTH {128} \
    CONFIG.PSU__TTC0__WAVEOUT__ENABLE {1} \
    CONFIG.PSU__TTC0__WAVEOUT__IO {EMIO} \
    CONFIG.PSU__USE__IRQ0 {1} \
    CONFIG.PSU__CRL_APB__PL0_REF_CTRL__FREQMHZ {200}] [get_bd_cells ps]
set_property -dict [list CONFIG.PSU__FPGA_PL1_ENABLE {1} \
    CONFIG.PSU__CRL_APB__PL1_REF_CTRL__FREQMHZ {100}] [get_bd_cells ps]

# Preserve Linux's TTC0 channel 2 fan PWM across a full PL reconfiguration.
# Matches AMD's k26_base_starter_kit fan_en_b connection.
create_bd_port -dir O fan_en_b
create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice:1.0 fan_pwm
set_property -dict [list CONFIG.DIN_WIDTH {3} CONFIG.DIN_FROM {2} \
    CONFIG.DIN_TO {2} CONFIG.DOUT_WIDTH {1}] [get_bd_cells fan_pwm]
connect_bd_net [get_bd_pins ps/emio_ttc0_wave_o] [get_bd_pins fan_pwm/Din]
connect_bd_net [get_bd_pins fan_pwm/Dout] [get_bd_ports fan_en_b]
read_xdc $root/fpga/constraints/kv260_fan.xdc

create_bd_cell -type ip -vlnv xilinx.com:ip:axi_dma:7.1 dma
set_property -dict [list \
    CONFIG.c_include_sg {1} \
    CONFIG.c_sg_include_stscntrl_strm {0} \
    CONFIG.c_addr_width {40} \
    CONFIG.c_sg_length_width {23} \
    CONFIG.c_m_axi_mm2s_data_width {128} \
    CONFIG.c_m_axis_mm2s_tdata_width {128} \
    CONFIG.c_m_axi_s2mm_data_width {128} \
    CONFIG.c_s_axis_s2mm_tdata_width {128} \
    CONFIG.c_mm2s_burst_size {64} \
    CONFIG.c_s2mm_burst_size {64} \
    CONFIG.c_include_mm2s_dre {0} \
    CONFIG.c_include_s2mm_dre {0}] [get_bd_cells dma]

create_bd_cell -type module -reference fades_kv260 solver
set_property CONFIG.FREQ_HZ [get_property CONFIG.FREQ_HZ [get_bd_pins ps/pl_clk0]] [get_bd_pins solver/dma_clk]
set_property CONFIG.FREQ_HZ [get_property CONFIG.FREQ_HZ [get_bd_pins ps/pl_clk1]] [get_bd_pins solver/core_clk]
connect_bd_net [get_bd_pins ps/pl_clk1] [get_bd_pins solver/core_clk]
connect_bd_intf_net [get_bd_intf_pins dma/M_AXIS_MM2S] [get_bd_intf_pins solver/S_AXIS]
connect_bd_intf_net [get_bd_intf_pins solver/M_AXIS] [get_bd_intf_pins dma/S_AXIS_S2MM]

foreach {name inputs} {control 1 read_mem 2 write_mem 1} {
    create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 $name
    set_property CONFIG.NUM_SI $inputs [get_bd_cells $name]
    set_property CONFIG.NUM_MI 1 [get_bd_cells $name]
}
set_property CONFIG.NUM_MI 2 [get_bd_cells control]
connect_bd_intf_net [get_bd_intf_pins control/M01_AXI] [get_bd_intf_pins solver/S_AXI]
foreach {from to} {
    ps/M_AXI_HPM0_FPD control/S00_AXI
    control/M00_AXI dma/S_AXI_LITE
    dma/M_AXI_MM2S read_mem/S00_AXI
    dma/M_AXI_SG read_mem/S01_AXI
    read_mem/M00_AXI ps/S_AXI_HP0_FPD
    dma/M_AXI_S2MM write_mem/S00_AXI
    write_mem/M00_AXI ps/S_AXI_HP1_FPD
} { connect_bd_intf_net [get_bd_intf_pins $from] [get_bd_intf_pins $to] }

create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 reset
create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 locked
set_property CONFIG.CONST_VAL 1 [get_bd_cells locked]
connect_bd_net [get_bd_pins locked/dout] [get_bd_pins reset/dcm_locked]
connect_bd_net [get_bd_pins ps/pl_resetn0] [get_bd_pins reset/ext_reset_in]
foreach pin {
    ps/maxihpm0_fpd_aclk ps/saxihp0_fpd_aclk ps/saxihp1_fpd_aclk
    dma/s_axi_lite_aclk dma/m_axi_sg_aclk dma/m_axi_mm2s_aclk dma/m_axi_s2mm_aclk
    control/aclk read_mem/aclk write_mem/aclk reset/slowest_sync_clk
} { connect_bd_net [get_bd_pins ps/pl_clk0] [get_bd_pins $pin] }
connect_bd_net [get_bd_pins ps/pl_clk0] [get_bd_pins solver/dma_clk]
connect_bd_net [get_bd_pins reset/peripheral_aresetn] [get_bd_pins solver/aresetn]
foreach pin {dma/axi_resetn control/aresetn read_mem/aresetn write_mem/aresetn} {
    connect_bd_net [get_bd_pins reset/peripheral_aresetn] [get_bd_pins $pin]
}

create_bd_cell -type ip -vlnv xilinx.com:ip:xlconcat:2.1 interrupts
set_property -dict [list CONFIG.NUM_PORTS {3} CONFIG.IN2_WIDTH {6}] [get_bd_cells interrupts]
create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 irq_unused
set_property -dict [list CONFIG.CONST_WIDTH {6} CONFIG.CONST_VAL {0}] [get_bd_cells irq_unused]
connect_bd_net [get_bd_pins dma/mm2s_introut] [get_bd_pins interrupts/In0]
connect_bd_net [get_bd_pins dma/s2mm_introut] [get_bd_pins interrupts/In1]
connect_bd_net [get_bd_pins irq_unused/dout] [get_bd_pins interrupts/In2]
connect_bd_net [get_bd_pins interrupts/dout] [get_bd_pins ps/pl_ps_irq0]

assign_bd_address -offset 0xA0000000 -range 64K -target_address_space \
    [get_bd_addr_spaces ps/Data] [get_bd_addr_segs dma/S_AXI_LITE/Reg] -force
assign_bd_address -offset 0xA0010000 -range 4K -target_address_space \
    [get_bd_addr_spaces ps/Data] [get_bd_addr_segs solver/S_AXI/reg0] -force
foreach {space hp} {Data_MM2S 0 Data_SG 0 Data_S2MM 1} {
    assign_bd_address -offset 0 -range 2G -target_address_space \
        [get_bd_addr_spaces dma/$space] [get_bd_addr_segs ps/SAXIGP[expr {$hp+2}]/HP${hp}_DDR_LOW]
    assign_bd_address -offset 0x800000000 -range 32G -target_address_space \
        [get_bd_addr_spaces dma/$space] [get_bd_addr_segs ps/SAXIGP[expr {$hp+2}]/HP${hp}_DDR_HIGH]
}
validate_bd_design
save_bd_design
write_bd_tcl -force $out/system.tcl
set address_report [open $out/addresses.rpt w]
foreach seg [get_bd_addr_segs -of_objects [get_bd_addr_spaces]] {
    puts $address_report "$seg [get_property OFFSET $seg] [get_property RANGE $seg]"
}
close $address_report
generate_target all [get_files system.bd]
set wrapper [make_wrapper -files [get_files system.bd] -top]
add_files -norecurse $wrapper
set_property top system_wrapper [current_fileset]
update_compile_order -fileset sources_1
if {$stage eq "bd"} { exit }

launch_runs synth_1 -jobs $jobs
wait_on_run synth_1
if {[get_property PROGRESS [get_runs synth_1]] ne "100%"} { error "Synthesis failed" }
if {[get_property NEEDS_REFRESH [get_runs synth_1]]} {
    error "Sources changed during synthesis; rerun with a stable source tree"
}
if {$stage eq "synth"} { exit }
set_property STEPS.PHYS_OPT_DESIGN.IS_ENABLED true [get_runs impl_1]
launch_runs impl_1 -to_step write_bitstream -jobs $jobs
wait_on_run impl_1
if {[get_property PROGRESS [get_runs impl_1]] ne "100%"} { error "Implementation failed" }
source $root/fpga/scripts/vivado_kv260_export.tcl
