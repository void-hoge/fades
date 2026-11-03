module formal_triad_branch (
    input wire [1538:0] state_in,
    output wire mismatch
);
    wire [1538:0] a, b;
    wire valid;
    triad_branch_generator dut(1'b0, state_in, a, b, valid);
    // Whole-state preservation/coverage. Balanced strict progress is proved
    // separately for split6; target selection is checked against the model.
    assign mismatch = ((a | b) != state_in) ||
        (a[1214:0] != state_in[1214:0]) || (b[1214:0] != state_in[1214:0]) ||
        (!valid && ((a != state_in) || (b != state_in)));
endmodule
