# Routed placement footprint by hierarchy. A CLB can be shared across scopes,
# so these footprints are not additive. UltraScale+ has one SLICE per CLB.
set out [file normalize [lindex $argv 0]]
open_checkpoint $out/verified_routed.dcp
set csv [open $out/clb_footprint.csv w]
puts $csv "scope,instance,occupied_clbs"
proc occupied_clbs {cells} {
    set sites [get_sites -quiet -of_objects $cells -filter {SITE_TYPE =~ SLICE*}]
    return [llength [lsort -unique $sites]]
}
puts $csv "board,system_wrapper,[occupied_clbs [get_cells -hier -filter {IS_PRIMITIVE == 1}]]"
set cores [get_cells -hier -regexp {.*/solvers\[[0-9]+\]\.solver$}]
foreach core $cores {
    set name [get_property NAME $core]
    set escaped [string map [list {[} {\[} {]} {\]} {.} {\.}] $name]
    set cells [get_cells -hier -regexp "$escaped/.*" -filter {IS_PRIMITIVE == 1}]
    puts $csv "core,$name,[occupied_clbs $cells]"
    set index [lindex [regexp -inline {solvers\[([0-9]+)\]} $name] 1]
    report_utilization -cells $core -file $out/core${index}_utilization.rpt
    set infer_cells [get_cells -hier -regexp "$escaped/infer/.*" -filter {IS_PRIMITIVE == 1}]
    puts $csv "infer,$name/infer,[occupied_clbs $infer_cells]"
}
close $csv
