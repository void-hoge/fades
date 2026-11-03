CXX ?= g++
OUT ?= ../build/host
REFERENCE_INCLUDE ?= ../reference
CXXFLAGS ?= -O3 -std=c++20 -Wall -Wextra -Werror
CPPFLAGS += -Iinclude -I"$(REFERENCE_INCLUDE)"

.PHONY: all tools module
all: tools
tools: $(OUT)/fades_verify $(OUT)/fades_benchmark

$(OUT)/fades_verify: tools/fades_verify.cpp include/fades_ioctl.h $(REFERENCE_INCLUDE)/sudoku.hpp
	mkdir -p "$(@D)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o "$@"

$(OUT)/fades_benchmark: tools/fades_benchmark.cpp include/fades_ioctl.h $(REFERENCE_INCLUDE)/sudoku.hpp
	mkdir -p "$(@D)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o "$@"

module:
	$(MAKE) -C driver OUT="$(abspath $(OUT)/driver)"
