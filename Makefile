IVERILOG ?= iverilog
VVP ?= vvp
VERILATOR ?= verilator
YOSYS ?= yosys
VIVADO ?= vivado
CXX ?= g++
PYTHON ?= python3
JOBS ?= 4
DATASET ?= sim/data/17clues.txt
LIMIT ?=
BASELINE ?= build/baseline/results.csv
RTL := rtl/inference.sv rtl/branch.sv rtl/core.sv
CPP := sim/cpp
SCRIPTS := sim/scripts
PERIOD ?= 9.090909
VIVADO_OUT ?= build/vivado_single_core
CHECKPOINT ?=
TOP ?= core

.DEFAULT_GOAL := test
.PHONY: test reference-test reference-full lint synth formal formal-locked-masks core-test regression branch-compare vivado-core vivado-kv260 clean

test: build/tb_inference build/tb_branch build/reference_test
	$(VVP) build/tb_inference
	$(VVP) build/tb_branch
	build/reference_test

build/tb_inference: rtl/inference.sv sim/tb/tb_inference.sv
	mkdir -p $(@D)
	$(IVERILOG) -g2012 -Wall -Wno-sensitivity-entire-array -s tb_inference -o $@ $^

build/tb_branch: rtl/branch.sv sim/tb/tb_branch.sv
	mkdir -p $(@D)
	$(IVERILOG) -g2012 -Wall -Wno-sensitivity-entire-array -s tb_branch -o $@ $^

build/reference_test: $(CPP)/reference_test.cpp $(CPP)/reference_solver.hpp
	mkdir -p $(@D)
	$(CXX) -O3 -std=c++20 -Wall -Wextra -Werror -I$(CPP) $< -o $@

reference-test: build/reference_test
	$<

reference-full: build/reference_test
	$< "$(DATASET)"

lint:
	$(VERILATOR) --lint-only --language 1800-2017 -Wall --top-module core $(RTL)
	$(VERILATOR) --lint-only --language 1800-2017 -Wall --top-module top $(RTL) fpga/kv260/top.sv

# Structural synthesis retains the dual-port RAM, rather than mapping it to FFs.
synth:
	mkdir -p build
	$(YOSYS) -Q -q -l build/synth.log -p 'read_verilog -sv $(RTL) fpga/kv260/top.sv; hierarchy -check -top top; proc; opt; memory_dff; memory_collect; opt_clean; check -assert; select -assert-none t:$$dlatch t:$$_DLATCH_*; stat'

formal: formal-locked-masks
	mkdir -p build
	$(YOSYS) -Q -q -l build/formal_branch.log -p 'read_verilog -sv rtl/branch.sv sim/formal/formal_branch.sv; prep -top formal_branch -flatten; sat -verify -prove mismatch 0 -show-inputs'
	$(YOSYS) -Q -q -l build/formal_inference.log -p 'read_verilog -sv rtl/inference.sv sim/formal/formal_inference.sv; prep -top formal_inference -flatten; sat -verify -prove mismatch 0 -show-inputs'
	$(YOSYS) -Q -q -l build/formal_row_changes.log -p 'read_verilog -sv rtl/inference.sv sim/formal/formal_inference.sv; prep -top formal_row_changes -flatten; sat -verify -prove mismatch 0 -show-inputs'

formal-locked-masks:
	mkdir -p build
	$(YOSYS) -Q -q -l build/formal_locked_masks.log -p 'read_verilog -sv rtl/inference.sv sim/formal/formal_locked_masks.sv; prep -top formal_locked_masks -flatten; sat -verify -prove mismatch 0 -show-inputs'

build/core/core_benchmark: $(RTL) $(CPP)/core_benchmark.cpp $(CPP)/reference_solver.hpp
	mkdir -p $(@D)
	+$(VERILATOR) --cc --exe --build -j $(JOBS) --assert --language 1800-2017 --output-split 20000 --output-split-cfuncs 200 --top-module core --Mdir "$(abspath $(@D))" -CFLAGS "-O3 -std=c++20 -I$(abspath $(CPP))" -o core_benchmark $(RTL) "$(abspath $(CPP)/core_benchmark.cpp)"

core-test: build/core/core_benchmark
	$< "$(DATASET)" build/core/results.csv $(LIMIT)
	$(PYTHON) $(SCRIPTS)/core_summary.py build/core/results.csv build/core/summary.json

# Explicit baseline is required after a fresh checkout; full runs only.
regression:
	$(PYTHON) $(SCRIPTS)/compare_regression.py "$(BASELINE)" build/core/results.csv

branch-compare:
	$(PYTHON) $(SCRIPTS)/compare_branch_runs.py "$(BASELINE)" build/core/results.csv build/branch_comparison.json

vivado-core:
	mkdir -p "$(dir $(VIVADO_OUT))"
	$(VIVADO) -mode batch -log "$(VIVADO_OUT).log" -journal "$(VIVADO_OUT).jou" -source fpga/scripts/vivado_core.tcl -tclargs $(PERIOD) "$(VIVADO_OUT)" "$(CHECKPOINT)" $(TOP)

vivado-kv260:
	+$(MAKE) vivado-core TOP=top PERIOD="$(PERIOD)" VIVADO_OUT="$(VIVADO_OUT)"

clean:
	rm -rf build
