`timescale 1ns/1ps
// Single-core K26 wrapper. Parallel candidate buses connect
// to on-chip logic; they are not carrier-board pins. Reset is synchronous high.
module top (
    input wire clk,
    input wire reset,
    input wire enable,
    input wire input_valid,
    output wire input_ready,
    input wire [728:0] input_candidates,
    output wire result_valid,
    input wire result_ready,
    output wire result_solved,
    output wire [728:0] result_candidates,
    output wire [10:0] queued,
    output wire [10:0] pending_branches,
    output wire busy,
    output wire event_infer,
    output wire event_branch,
    output wire event_load,
    output wire event_memory_wait,
    output wire event_output_wait,
    output wire event_contradiction,
    output wire event_finish,
    output wire event_skip,
    output wire event_wasted_infer
);
    core solver (.*);
endmodule
