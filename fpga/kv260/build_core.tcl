# Out-of-context implementation of one solver core.
# Arguments: clock period in ns (default 10), output directory,
# optional checkpoint (empty string for fresh synthesis).
set period [expr {$argc > 0 ? [lindex $argv 0] : 10.0}]
set root [file normalize [file dirname [info script]]/../..]
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
    set f [open $root/fpga/kv260/constraints/core_ooc.xdc r]
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
# Development reports for the branch, inference and terminal-check cones.
set branch_nets [get_nets -quiet -regexp {child_[ab]\[[0-9]+\]}]
if {![llength $branch_nets]} { error "No branch output nets found" }
report_timing -through $branch_nets -max_paths 5 -path_type full_clock_expanded -file $out/branch_paths.rpt
set inference_nets [get_nets -quiet -regexp {next_candidates\[[0-9]+\]}]
if {[llength $inference_nets]} {
    report_timing -through $inference_nets -max_paths 5 -file $out/inference_paths.rpt
}
set check_nets [get_nets -quiet {state_contradiction state_solved}]
if {[llength $check_nets]} {
    report_timing -through $check_nets -max_paths 5 -file $out/check_paths.rpt
}
report_drc -file $out/drc.rpt
write_checkpoint -force $out/routed.dcp
# Record the requested timing and, if needed, a relaxed period on the same
# routing. The slack-based limit is an estimate, not a verified faster run.
set slack [get_property SLACK [get_timing_paths -delay_type max -max_paths 1]]
set min_period [expr {$period - $slack}]
set verified_period $period
if {$slack < 0} {
    set verified_period [expr {ceil($min_period * 2.0) / 2.0 + 0.5}]
    create_clock -name core_clk -period $verified_period [get_ports clk]
}
set verified_period [get_property PERIOD [get_clocks core_clk]]
report_timing_summary -delay_type min_max -max_paths 10 -file $out/verified_timing_summary.rpt
set verified_slack [get_property SLACK [get_timing_paths -delay_type max -max_paths 1]]
set hold_slack [get_property SLACK [get_timing_paths -delay_type min -max_paths 1]]
write_checkpoint -force $out/verified_routed.dcp
set f [open $out/summary.txt w]
puts $f "part=xck26-sfvc784-2LV-c"
puts $f "target_period_ns=$period"
puts $f "setup_wns_ns=$slack"
puts $f "estimated_setup_limit_mhz=[expr {1000.0/$min_period}]"
puts $f "verified_period_ns=$verified_period"
puts $f "verified_clock_mhz=[expr {1000.0/$verified_period}]"
puts $f "verified_setup_wns_ns=$verified_slack"
puts $f "verified_hold_whs_ns=$hold_slack"
puts $f "design=[get_property NAME [current_design]]"
puts $f "NOTE: routed OOC design, internal timing only. Limit is a slack-based estimate."
close $f
if {$verified_slack < 0 || $hold_slack < 0} {
    error "Routed core still fails setup or hold timing; inspect reports."
}
