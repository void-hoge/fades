# Out-of-context implementation of one solver core.
# Arguments: clock period in ns (default 10), output directory,
# optional checkpoint (empty string for fresh synthesis).
set period [expr {$argc > 0 ? [lindex $argv 0] : 10.0}]
set scripts [file dirname [file normalize [info script]]]
set root [file normalize $scripts/../..]
set out [file normalize [expr {$argc > 1 ? [lindex $argv 1] : "build/vivado_single_core"}]]
set checkpoint ""
if {$argc > 2 && [lindex $argv 2] ne ""} { set checkpoint [file normalize [lindex $argv 2]] }
set top core
file mkdir $out
# Keep Vivado's auxiliary placement/congestion files alongside its reports.
cd $out
set_param general.maxThreads 4
if {$checkpoint ne ""} {
    open_checkpoint $checkpoint
} else {
    create_project -in_memory -part xck26-sfvc784-2LV-c
    read_verilog -sv [list $root/rtl/inference.sv $root/rtl/branch.sv $root/rtl/core.sv]
    set f [open $root/fpga/constraints/core_ooc.xdc r]
    set constraints [read $f]
    close $f
    if {[regsub -all -line {^create_clock -name core_clk -period [0-9.]+} $constraints \
        "create_clock -name core_clk -period $period" constraints] != 1} {
        error "Expected exactly one core clock in core_ooc.xdc"
    }
    set f [open $out/synthesis_constraints.xdc w]
    puts $f $constraints
    close $f
    read_xdc $out/synthesis_constraints.xdc
    synth_design -top $top -mode out_of_context -part xck26-sfvc784-2LV-c
}
report_clocks -file $out/synthesis_clocks.rpt
create_clock -name core_clk -period $period [get_ports clk]
set period [get_property PERIOD [get_clocks core_clk]]
set_property HD.CLK_SRC BUFGCE_X0Y0 [get_ports clk]
# The parallel interfaces connect to synchronous logic inside the PL. This OOC
# run measures register-to-register timing, not an external 729-bit pin bus.
set_false_path -from [get_ports -filter {DIRECTION == IN && NAME != clk}]
set_false_path -to [get_ports -filter {DIRECTION == OUT}]
report_utilization -file $out/post_synth_utilization.rpt
report_utilization -hierarchical -file $out/post_synth_hierarchy.rpt
write_checkpoint -force $out/post_synth.dcp
opt_design
place_design
phys_opt_design
route_design
report_route_status -file $out/route_status.rpt
report_utilization -file $out/utilization.rpt
report_utilization -hierarchical -file $out/hierarchy.rpt
report_timing_summary -delay_type min_max -max_paths 10 -file $out/timing_summary.rpt
report_clock_utilization -file $out/clocks.rpt
report_timing -max_paths 10 -path_type full_clock_expanded -file $out/critical_paths.rpt
if {$top eq "core"} { source $scripts/vivado_stage_timing.tcl }
report_drc -file $out/drc.rpt
write_checkpoint -force $out/routed.dcp
source $scripts/vivado_clock_report.tcl
