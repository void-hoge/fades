// End-to-end branch check: popcount + linear three-class priority reference.
// All 729 candidate bits are free, including zero/singleton input cells.
module formal_branch (
    input wire [728:0] candidates,
    output wire mismatch
);
    wire [728:0] child_a, child_b, expected_a, expected_b;
    logic [80:0] expected;
    integer best, count, priority_class;
    branch dut(candidates, child_a, child_b);
    always_comb begin
        expected = 0;
        best = 0;
        count = 0;
        priority_class = 0;
        for (integer i = 0; i < 81; i = i + 1) begin
            count = 0;
            for (integer d = 0; d < 9; d = d + 1)
                count = count + candidates[9*i+d];
            priority_class = count == 2 ? 2 : count >= 3 ? 1 : 0;
            if (priority_class > best) begin
                best = priority_class;
                expected = 81'b1 << i;
            end
        end
    end
    // Partitioning is unchanged; include it so the proof checks the actual
    // eligibility wiring in branch rather than a duplicate flag generator.
    for (genvar c = 0; c < 81; c = c + 1) begin : cells
        wire [8:0] mask = candidates[9*c +: 9];
        logic [3:0] candidate_count, rank;
        logic [8:0] half;
        always_comb begin
            candidate_count = 0;
            for (integer d = 0; d < 9; d = d + 1)
                candidate_count = candidate_count + {3'b0, mask[d]};
            rank = 0;
            half = 0;
            for (integer d = 0; d < 9; d = d + 1) begin
                if (mask[d]) begin
                    if (rank < (candidate_count >> 1)) half[d] = 1;
                    rank = rank + 1'b1;
                end
            end
        end
        assign expected_a[9*c +: 9] = expected[c] ? half : mask;
        assign expected_b[9*c +: 9] = expected[c] ? (mask & ~half) : mask;
    end
    assign mismatch = (child_a != expected_a) || (child_b != expected_b);
endmodule
