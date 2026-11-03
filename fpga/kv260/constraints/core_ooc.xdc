# Synchronous PL-facing interfaces. External interface timing is a property of
# the eventual PS/DMA wrapper, and is excluded from this isolated core run.
# Default for standalone use. build_core.tcl generates a copy with the requested
# numeric period before synthesis (XDC accepts only a restricted Tcl subset).
create_clock -name core_clk -period 10.0 [get_ports clk]
# Model a parent-design global clock buffer for OOC clock delay/skew analysis.
set_property HD.CLK_SRC BUFGCE_X0Y0 [get_ports clk]
set_false_path -from [get_ports -filter {DIRECTION == IN && NAME != clk}]
set_false_path -to [get_ports -filter {DIRECTION == OUT}]
