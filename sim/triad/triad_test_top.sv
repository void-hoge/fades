`timescale 1ns/1ps
module triad_test_top (
    input wire clk,
    input wire [1538:0] state_in,
    input wire band_phase,
    output wire [1538:0] state_out,
    output wire solved,
    output wire contradiction,
    output wire [1538:0] child_a,
    output wire [1538:0] child_b,
    output wire branch_valid,
    output wire [1538:0] pipelined_a,
    output wire [1538:0] pipelined_b,
    output wire pipelined_valid
);
    triad_engine infer(state_in, band_phase, state_out, solved, contradiction);
    triad_branch_generator branch(1'b0, state_in, child_a, child_b, branch_valid);
    triad_branch_generator #(.PIPELINED(1)) pipelined_branch(
        clk, state_in, pipelined_a, pipelined_b, pipelined_valid);
endmodule
