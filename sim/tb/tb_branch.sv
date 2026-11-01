`timescale 1ns/1ps
module tb_branch;
    logic [728:0] candidates;
    wire [728:0] child_a, child_b;
    branch dut(candidates, child_a, child_b);
    wire [80:0] selected = dut.selected;

    function automatic integer bit_count(input logic [8:0] bits);
        bit_count = 0;
        for (int d = 0; d < 9; d++) bit_count += int'(bits[d]);
    endfunction

    // Check full child matrices, balanced partitioning and digit order using
    // an independent lowest-set-bit loop, rather than the RTL's rank scan.
    task automatic check;
        integer best, chosen, n, priority_class;
        logic [8:0] mask, remaining, first_half, bit_mask;
        logic [728:0] expected_a, expected_b;
        logic [80:0] expected_selected;
        begin
            #1;
            best = 0;
            chosen = -1;
            for (int i = 0; i < 81; i++) begin
                n = bit_count(candidates[9*i +: 9]);
                priority_class = n == 2 ? 2 : n >= 3 ? 1 : 0;
                if (priority_class > best) begin
                    best = priority_class;
                    chosen = i;
                end
            end
            expected_a = candidates;
            expected_b = candidates;
            expected_selected = 0;
            if (chosen >= 0) begin
                expected_selected[chosen] = 1;
                mask = candidates[9*chosen +: 9];
                n = bit_count(mask);
                remaining = mask;
                first_half = 0;
                for (int k = 0; k < n/2; k++) begin
                    bit_mask = remaining & (~remaining + 9'd1);
                    first_half |= bit_mask;
                    remaining ^= bit_mask;
                end
                expected_a[9*chosen +: 9] = first_half;
                expected_b[9*chosen +: 9] = remaining;
            end
            if (selected !== expected_selected || child_a !== expected_a || child_b !== expected_b)
                $fatal(1, "Three-class selection/split mismatch: chosen=%0d selected=%h expected=%h",
                    chosen, selected, expected_selected);
        end
    endtask

    initial begin
        // Exhaustive 9-bit masks, at every cell position (including leaf 80).
        for (int position = 0; position < 81; position++) begin
            for (int m = 0; m < 512; m++) begin
                candidates = 0;
                candidates[9*position +: 9] = 9'(m);
                check;
            end
        end
        // Class priorities and row-major ties across tournament boundaries.
        candidates = {81{9'h1ff}};
        candidates[9*80 +: 9] = 9'h103;
        check;
        candidates[9*63 +: 9] = 9'h081;
        check;
        candidates[9*0 +: 9] = 9'h003;
        check;
        // Three-or-more class is row-major, deliberately not full MRV.
        candidates = {81{9'h1ff}};
        candidates[9*80 +: 9] = 9'h007;
        check;
        $display("PASS: three-class selector, all masks/positions and balanced child generation");
        $finish;
    end
endmodule
