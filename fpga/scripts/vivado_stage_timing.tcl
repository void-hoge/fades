# Per-cone timing on an existing implementation. Can also be sourced after
# routing by vivado_core.tcl (where `out` and the current design already exist).
if {![info exists out]} {
    if {$argc != 2} { error "Expected ROUTED.dcp OUTPUT_DIR" }
    set out [file normalize [lindex $argv 1]]
    open_checkpoint [lindex $argv 0]
}
file mkdir $out
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
