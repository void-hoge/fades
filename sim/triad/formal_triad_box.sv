module formal_triad_box (
    input wire [80:0] extra_cells,
    input wire [53:0] extra_negative,
    output wire mismatch
);
    wire [80:0] truth_cells, cells_in, cells_out;
    wire [53:0] truth_negative, negative_in, negative_out;
    wire contradiction;
    genvar c, t;
    generate
        // Digit relabeling makes this a representative of every solved box.
        for (c = 0; c < 9; c = c + 1) begin : cells
            assign truth_cells[9*c +: 9] = 9'b1 << c;
        end
        for (t = 0; t < 6; t = t + 1) begin : triads
            localparam integer C0 = (t < 3) ? 3*t : t-3;
            localparam integer C1 = C0 + ((t < 3) ? 1 : 3);
            localparam integer C2 = C0 + ((t < 3) ? 2 : 6);
            assign truth_negative[9*t +: 9] =
                ~((9'b1 << C0) | (9'b1 << C1) | (9'b1 << C2));
        end
    endgenerate
    assign cells_in = truth_cells | extra_cells;
    assign negative_in = truth_negative | extra_negative;
    triad_box dut(cells_in, negative_in, cells_out, negative_out, contradiction);
    assign mismatch = contradiction ||
        ((cells_out & truth_cells) != truth_cells) ||
        ((negative_out & truth_negative) != truth_negative) ||
        (|(cells_out & ~cells_in)) || (|(negative_out & ~negative_in));
endmodule
