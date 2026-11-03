IVERILOG ?= iverilog
VVP ?= vvp
VERILATOR ?= verilator
YOSYS ?= yosys
VIVADO ?= vivado
CXX ?= g++
PYTHON ?= python3
JOBS ?= 4
DATASET ?= sim/data/17puz49158.txt
LIMIT ?=
BASELINE ?= build/baseline/results.csv
RTL := rtl/inference.sv rtl/branch.sv rtl/core.sv
CPP := sim/cpp
REFERENCE := reference/sudoku.hpp
REFERENCE_DIR := reference
SCRIPTS := sim/scripts
PERIOD ?= 9.090909
VIVADO_OUT ?= build/vivado_single_core
CHECKPOINT ?=
KV260_OUT ?= build/kv260
KV260_STAGE ?= bit
DTC ?= dtc
STREAM_RTL := $(RTL) rtl/async_fifo.sv rtl/solver_stream.sv
AXI_RTL := $(STREAM_RTL) rtl/solver_axi.sv

.DEFAULT_GOAL := test
.PHONY: help test reference-test reference-full lint synth formal formal-locked-masks core-test regression vivado-core vivado-kv260 package-kv260 stream-test axi-test fifo-test host host-input-test host-module clean

help:
	$(info Use on KV260:  make host; build/host/fades_benchmark FILE SECONDS)
	$(info Verify output: build/host/fades_verify FILE RECORDS_PER_PACKET SLOTS ROUNDS [mixed])
	$(info Prepare board: make vivado-kv260; make package-kv260; make host-module)
	$(info Test RTL:      make test lint synth; make core-test stream-test; make formal fifo-test)
	$(info More details:  README.md, host/README.md, fpga/kv260/README.md, sim/README.md)
	@:

test: build/tb_inference build/tb_branch build/reference_test axi-test
	$(VVP) build/tb_inference
	$(VVP) build/tb_branch
	build/reference_test

build/tb_inference: rtl/inference.sv sim/tb/tb_inference.sv
	mkdir -p $(@D)
	$(IVERILOG) -g2012 -Wall -Wno-sensitivity-entire-array -s tb_inference -o $@ $^

build/tb_branch: rtl/branch.sv sim/tb/tb_branch.sv
	mkdir -p $(@D)
	$(IVERILOG) -g2012 -Wall -Wno-sensitivity-entire-array -s tb_branch -o $@ $^

build/reference_test: $(CPP)/reference_test.cpp $(REFERENCE)
	mkdir -p $(@D)
	$(CXX) -O3 -std=c++20 -Wall -Wextra -Werror -I$(REFERENCE_DIR) $< -o $@

reference-test: build/reference_test
	$<

reference-full: build/reference_test
	$< "$(DATASET)"

lint:
	$(VERILATOR) --lint-only --language 1800-2017 -Wall --top-module core $(RTL)
	$(VERILATOR) --lint-only --language 1800-2017 -Wall --top-module solver_stream $(STREAM_RTL)
	$(VERILATOR) --lint-only --language 1800-2017 -Wall --top-module solver_axi $(AXI_RTL)
	$(VERILATOR) --lint-only --language 1800-2017 -Wall --top-module fades_kv260 $(AXI_RTL) fpga/kv260/fades_kv260.sv

# Structural synthesis retains the dual-port RAM, rather than mapping it to FFs.
synth:
	mkdir -p build
	$(YOSYS) -Q -q -l build/synth.log -p 'read_verilog -sv $(RTL); hierarchy -check -top core; proc; opt; memory_dff; memory_collect; opt_clean; check -assert; select -assert-none t:$$dlatch t:$$_DLATCH_*; stat'

formal: formal-locked-masks
	mkdir -p build
	$(YOSYS) -Q -q -l build/formal_branch.log -p 'read_verilog -sv rtl/branch.sv sim/formal/formal_branch.sv; prep -top formal_branch -flatten; sat -verify -prove mismatch 0 -show-inputs'
	$(YOSYS) -Q -q -l build/formal_inference.log -p 'read_verilog -sv rtl/inference.sv sim/formal/formal_inference.sv; prep -top formal_inference -flatten; sat -verify -prove mismatch 0 -show-inputs'
	$(YOSYS) -Q -q -l build/formal_row_changes.log -p 'read_verilog -sv rtl/inference.sv sim/formal/formal_inference.sv; prep -top formal_row_changes -flatten; sat -verify -prove mismatch 0 -show-inputs'

formal-locked-masks:
	mkdir -p build
	$(YOSYS) -Q -q -l build/formal_locked_masks.log -p 'read_verilog -sv rtl/inference.sv sim/formal/formal_locked_masks.sv; prep -top formal_locked_masks -flatten; sat -verify -prove mismatch 0 -show-inputs'

build/core/core_test: $(RTL) $(CPP)/core_test.cpp $(REFERENCE)
	mkdir -p $(@D)
	+$(VERILATOR) --cc --exe --build -j $(JOBS) --assert --language 1800-2017 --output-split 20000 --output-split-cfuncs 200 --top-module core --Mdir "$(abspath $(@D))" -CFLAGS "-O3 -std=c++20 -I$(abspath $(REFERENCE_DIR))" -o core_test $(RTL) "$(abspath $(CPP)/core_test.cpp)"

core-test: build/core/core_test
	$< "$(DATASET)" build/core/results.csv $(LIMIT)
	$(PYTHON) $(SCRIPTS)/core_results.py summary build/core/results.csv build/core/summary.json

# Explicit baseline is required after a fresh checkout; full runs only.
regression:
	$(PYTHON) $(SCRIPTS)/core_results.py compare "$(BASELINE)" build/core/results.csv

vivado-core:
	mkdir -p "$(dir $(VIVADO_OUT))"
	$(VIVADO) -mode batch -log "$(VIVADO_OUT).log" -journal "$(VIVADO_OUT).jou" -source fpga/kv260/build_core.tcl -tclargs $(PERIOD) "$(VIVADO_OUT)" "$(CHECKPOINT)"

vivado-kv260:
	mkdir -p "$(KV260_OUT)"
	$(VIVADO) -mode batch -log "$(KV260_OUT)/vivado.log" -journal "$(KV260_OUT)/vivado.jou" -source fpga/kv260/build.tcl -tclargs "$(KV260_OUT)" "$(KV260_STAGE)" $(JOBS)

package-kv260:
	test -f "$(KV260_OUT)/build_complete"
	mkdir -p "$(KV260_OUT)/firmware/fades"
	$(DTC) -@ -I dts -O dtb -o "$(KV260_OUT)/firmware/fades/fades.dtbo" fpga/kv260/overlay.dts
	cp "$(KV260_OUT)/fades.bit.bin" fpga/kv260/shell.json "$(KV260_OUT)/firmware/fades/"

host:
	$(MAKE) -C host

host-module:
	$(MAKE) -C host module

host-input-test: host
	$(PYTHON) host/tests/check_input.py

clean:
	rm -rf build

build/stream/stream_test: $(STREAM_RTL) $(CPP)/stream_test.cpp $(REFERENCE)
	mkdir -p $(@D)
	+$(VERILATOR) --cc --exe --build -j $(JOBS) --assert --language 1800-2017 --output-split 20000 --output-split-cfuncs 200 --top-module solver_stream --Mdir "$(abspath $(@D))" -CFLAGS "-O3 -std=c++20 -I$(abspath $(REFERENCE_DIR))" -o stream_test $(STREAM_RTL) "$(abspath $(CPP)/stream_test.cpp)"

stream-test: build/stream/stream_test
	$< "$(DATASET)" $(LIMIT)

axi-test:
	mkdir -p build/stream
	$(IVERILOG) -g2012 -s tb_solver_axi -o build/stream/tb_axi $(AXI_RTL) sim/tb/tb_solver_axi.sv
	$(VVP) build/stream/tb_axi

fifo-test:
	$(PYTHON) sim/scripts/xpm_fifo_test.py
