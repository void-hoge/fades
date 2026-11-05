# Called by build.tcl after implementation or for an explicit export.
# Checks the routed board design and writes loadable firmware/XSA/reports.
proc export_firmware {root out} {
    file delete -force $out/build_complete
    if {[current_project -quiet] eq ""} { open_project $out/project/fades.xpr }
    if {[get_property PROGRESS [get_runs impl_1]] ne "100%" ||
        ![string match "write_bitstream Complete*" [get_property STATUS [get_runs impl_1]]]} {
        error "A completed write_bitstream run is required for export"
    }
    open_run impl_1
    set fan_port [get_ports -quiet fan_en_b]
    if {[llength $fan_port] != 1 || [get_property PACKAGE_PIN $fan_port] ne "A12" ||
        [get_property IOSTANDARD $fan_port] ne "LVCMOS33" ||
        [get_property DRIVE $fan_port] != 4 || [get_property SLEW $fan_port] ne "SLOW"} {
        error "KV260 fan requires A12 / LVCMOS33 / 4 mA / SLOW; rebuild the board design"
    }
    set summary [report_timing_summary -delay_type min_max -return_string -file $out/timing_summary.rpt]
    report_utilization -file $out/utilization.rpt
    report_utilization -hierarchical -hierarchical_depth 12 -file $out/hierarchy.rpt
    report_io -file $out/io.rpt
    report_drc -file $out/drc.rpt
    report_route_status -file $out/route_status.rpt
    foreach pattern {{.*(req_sync_reg\[0\]|ack_sync_reg\[0\]|enable_sync_reg\[0\]|total_meta_reg\[[0-9]+\])/D} {.*reset_sync_reg\[[0-9]+\]/(CLR|PRE)}} {
        if {![llength [get_pins -hier -regexp $pattern]]} { error "Solver CDC synchronizers missing: $pattern" }
    }

    # Reviewed protocol CDCs; static delay/skew bounds are in constraints/solver_cdc.xdc.
    # Reset intentionally fans out through an OR to local 3-stage release
    # synchronizers. Only asynchronous reset inputs are waived, not datapaths.
    set reset_first [get_pins -hier -regexp {.*[/]stream/(core_reset_sync_reg\[0\]|input_queue/read_reset_sync_reg\[0\]|output_queue/reset_sync_reg\[0\])/PRE}]
    if {![llength $reset_first]} { error "Asynchronous reset synchronizers missing" }
    create_waiver -type CDC -id CDC-10 -to $reset_first -user fades \
        -desc {Board reset OR software reset drives async assertion only; local three-stage release is synchronized. Board reset changes only during global initialization.}
    create_waiver -type CDC -id CDC-11 -to $reset_first -user fades \
        -desc {Asynchronous reset intentionally fans out to independent local reset-release synchronizers. FIFO reset-busy and local reset gates tolerate differing release cycles; reset is held throughout reinitialization.}
    # Total count is immutable through reset release and the complete run.
    create_waiver -type CDC -id CDC-6 \
        -to [get_pins -hier -regexp {.*total_meta_reg\[[0-9]+\]/D}] -user fades \
        -desc {Configuration is held stable through reset release and the complete run; both bus stages settle before any problem is dispatched.}
    # Snapshot stays held until the next request; receiver waits for synchronized ack.
    create_waiver -type CDC -id CDC-15 \
        -from [get_pins -hier -regexp {.*core_snapshot_reg\[[0-9]+\]/C}] \
        -to [get_pins -hier -regexp {.*[/]snapshot_reg\[[0-9]+\]/D}] -user fades \
        -desc {Bundled snapshot data is held until the next request; receiver waits for synchronized ack. Datapath delay and bus skew are constrained to 5 ns.}
    set cdc_report [report_cdc -details -return_string -file $out/cdc.rpt]
    if {[regexp -line {^CDC-[0-9]+[ \t]+Critical[ \t]+[1-9]} $cdc_report]} {
        error "Unreviewed critical CDC paths; inspect $out/cdc.rpt"
    }
    set bus_skew_report [report_bus_skew -return_string -file $out/bus_skew.rpt]
    if {[string match {*Slack (VIOLATED)*} $bus_skew_report]} {
        error "CDC bus skew failed; inspect $out/bus_skew.rpt"
    }
    report_clock_interaction -file $out/clock_interaction.rpt
    if {![regexp {All user specified timing constraints are met} $summary]} {
        error "Timing closure failed; inspect $out/timing_summary.rpt"
    }
    foreach delay {max min} {
        set paths [get_timing_paths -delay_type $delay -max_paths 1]
        if {[llength $paths] == 0 || [get_property SLACK $paths] < 0} {
            error "Timing closure failed ($delay); inspect $out/timing_summary.rpt"
        }
    }
    write_checkpoint -force $out/verified_routed.dcp
    set bit [get_property DIRECTORY [get_runs impl_1]]/system_wrapper.bit
    write_cfgmem -force -format bin -interface SMAPx32 -disablebitswap \
        -loadbit "up 0x0 $bit" -file $out/fades.bit.bin
    write_hw_platform -fixed -include_bit -force -file $out/fades.xsa
    set stamp [open $out/build_complete w]
    puts $stamp "Vivado [version -short], KV260 fades, DMA 200 MHz target, fan PWM on A12"
    close $stamp
}
