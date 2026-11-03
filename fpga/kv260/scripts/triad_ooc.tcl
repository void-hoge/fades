# Single-core resource / timing exploration; not a board-loadable circuit.
# vivado -mode batch -source triad_ooc.tcl -tclargs OUTPUT_DIRECTORY
set root [file normalize [file dirname [info script]]/../../..]
set out [file normalize [lindex $argv 0]]
file mkdir $out
# Vivado writes placement diagnostics into its working directory.
cd $out
set_param general.maxThreads 2
read_verilog -sv [list $root/rtl/deduction_engine.sv $root/rtl/triad_engine.sv \
    $root/rtl/fades_core.sv]
synth_design -mode out_of_context -top fades_core -part xck26-sfvc784-2LV-c
create_clock -name core_clk -period 10.000 [get_ports clk]
report_utilization -file $out/synth_utilization.rpt
write_checkpoint -force $out/synth.dcp
opt_design
place_design
phys_opt_design
route_design
report_utilization -file $out/utilization.rpt
report_timing_summary -file $out/timing.rpt
write_checkpoint -force $out/routed.dcp
