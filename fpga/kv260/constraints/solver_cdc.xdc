# KV260 solver synchronizer first stages. Do not false-path the clock groups:
# the held snapshot bundle has an explicit physical datapath bound below.
set first_stages [get_pins -hier -regexp {.*(req_sync_reg\[0\]|ack_sync_reg\[0\]|enable_sync_reg\[0\])/D}]
set_false_path -to $first_stages
set reset_pins [get_pins -hier -regexp {.*reset_sync_reg\[[0-9]+\]/(CLR|PRE)}]
set_false_path -to $reset_pins
# Core captures data and toggles ack on the same edge. DMA samples data only
# after the two-stage ack synchronizer and a further control-register edge.
# Data stays unchanged until the next host snapshot request. Bound bus flight
# time to one DMA period, comfortably within that bundled-data settling time.
set snapshot_from [get_cells -hier -regexp {.*core_snapshot_reg\[[0-9]+\]}]
set snapshot_to [get_cells -hier -regexp {.*[/]snapshot_reg\[[0-9]+\]}]
set_max_delay 5.0 -datapath_only -from $snapshot_from -to $snapshot_to
set_bus_skew 5.0 -from $snapshot_from -to $snapshot_to
