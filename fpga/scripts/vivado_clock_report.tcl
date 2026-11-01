# Can be sourced after routing, or run separately with ROUTED.dcp OUTPUT_DIR.
if {![info exists out]} {
    if {$argc != 2} { error "Expected ROUTED.dcp OUTPUT_DIR" }
    set out [lindex $argv 1]
    open_checkpoint [lindex $argv 0]
    set period [get_property PERIOD [get_clocks core_clk]]
}
file mkdir $out
set slack [get_property SLACK [get_timing_paths -delay_type max -max_paths 1]]
set min_period [expr {$period - $slack}]
# Relax the clock on the SAME routing if the aggressive target failed, then
# explicitly check setup and hold. No negative-WNS clock is called operational.
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
