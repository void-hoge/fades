`timescale 1ns/1ps
// Measure the unchanged core with the same once-preprocessed input supplied
// by the shared stream pipeline. Preprocessing is outside the measured core.
module fades_core_preprocessed (
    input wire clk, reset, enable,
    input wire input_valid,
    output wire input_ready,
    input wire [728:0] input_candidates,
    output wire result_valid,
    input wire result_ready,
    output wire result_solved,
    output wire [728:0] result_candidates,
    output wire [10:0] queued, pending_branches,
    output wire busy,
    output wire event_infer, event_branch, event_load, event_memory_wait,
    output wire event_output_wait, event_contradiction, event_finish,
    output wire event_skip, event_wasted_infer
);
    wire [728:0] preprocessed_candidates;
    naked_single_preload preprocess(input_candidates, preprocessed_candidates);
    fades_core core(.input_candidates(preprocessed_candidates), .*);
endmodule
