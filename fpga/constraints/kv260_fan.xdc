# KV260 fan control, matching AMD kria-vitis-platforms k26_base_starter_kit.
# Linux pwm-fan drives PS TTC0 channel 2, routed through EMIO to this PL pin.
set_property PACKAGE_PIN A12 [get_ports fan_en_b]
set_property IOSTANDARD LVCMOS33 [get_ports fan_en_b]
set_property SLEW SLOW [get_ports fan_en_b]
set_property DRIVE 4 [get_ports fan_en_b]
# This is the asynchronous PS TTC PWM, not a PL-clocked data output.
set_false_path -to [get_ports fan_en_b]
