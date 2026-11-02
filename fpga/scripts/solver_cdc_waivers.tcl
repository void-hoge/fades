# Reviewed solver protocol CDCs. No unknown data crossing is waived.
# The board reset and software reset are ORed only as asynchronous reset
# sources. They feed three-stage local reset synchronizers, never datapaths.
set reset_first [get_pins -hier -regexp {.*[/]stream/(core_reset_sync_reg\[0\]|input_queue/read_reset_sync_reg\[0\]|output_queue/reset_sync_reg\[0\])/PRE}]
if {![llength $reset_first]} { error "Asynchronous reset synchronizers missing" }
create_waiver -type CDC -id CDC-10 -to $reset_first -user fades \
    -desc {Board reset OR software reset drives async assertion only; local three-stage release is synchronized. Board reset changes only during global initialization.}
create_waiver -type CDC -id CDC-11 -to $reset_first -user fades \
    -desc {Asynchronous reset intentionally fans out to independent local reset-release synchronizers. FIFO reset-busy and local reset gates tolerate differing release cycles; reset is held throughout reinitialization.}
# Total count is programmed while all cores are held in reset and is immutable
# throughout a run. The two-stage bus is settled before dispatch is possible.
create_waiver -type CDC -id CDC-6 \
    -to [get_pins -hier -regexp {.*total_meta_reg\[[0-9]+\]/D}] -user fades \
    -desc {Configuration is held stable through reset release and the complete run; both bus stages settle before any problem is dispatched.}
# The ack has two synchronizer stages plus the receiver control edge. Source
# data is held until the next request, and physical delay/skew are <= 5 ns.
create_waiver -type CDC -id CDC-15 \
    -from [get_pins -hier -regexp {.*core_snapshot_reg\[[0-9]+\]/C}] \
    -to [get_pins -hier -regexp {.*[/]snapshot_reg\[[0-9]+\]/D}] -user fades \
    -desc {Bundled snapshot data is held until the next request; receiver waits for synchronized ack. Datapath delay and bus skew are constrained to 5 ns.}
