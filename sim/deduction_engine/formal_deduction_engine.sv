// Independent prefix-scan specification for the shared analysis.
// Unlike the DUT's 27-unit wiring and ternary merges, this scans rows, columns
// and blocks separately in the original order. All 729 input bits are free.
module formal_deduction_engine (
    input wire [728:0] candidates,
    output wire mismatch
);
    wire [728:0] fixed_digits, expected_fixed;
    wire [242:0] unit_used, unit_unique, expected_used, expected_unique;
    wire solved, contradiction;
    wire [80:0] empty_cell, filled;
    wire [8:0] bad_unit;
    analysis dut(candidates, fixed_digits, unit_used, unit_unique, solved, contradiction);
    function single;
        input [8:0] bits;
        begin
            single = (bits != 0) && ((bits & (bits - 9'd1)) == 0);
        end
    endfunction
    genvar i, u;
    generate
        for (i = 0; i < 81; i = i + 1) begin : cells
            assign empty_cell[i] = candidates[9*i +: 9] == 0;
            assign filled[i] = single(candidates[9*i +: 9]);
            assign expected_fixed[9*i +: 9] = filled[i] ? candidates[9*i +: 9] : 9'b0;
        end
        for (u = 0; u < 9; u = u + 1) begin : units
            localparam integer BOX_START = (u/3)*27+(u%3)*3;
            reg [8:0] row_seen, col_seen, box_seen;
            reg [8:0] row_multi, col_multi, box_multi;
            reg [8:0] row_used, col_used, box_used;
            reg duplicate;
            integer n;
            always @* begin
                row_seen = 0;
                col_seen = 0;
                box_seen = 0;
                row_multi = 0;
                col_multi = 0;
                box_multi = 0;
                row_used = 0;
                col_used = 0;
                box_used = 0;
                duplicate = 0;
                for (n = 0; n < 9; n = n + 1) begin
                    if (((row_used & expected_fixed[9*(u*9+n) +: 9]) != 0) ||
                        ((col_used & expected_fixed[9*(n*9+u) +: 9]) != 0) ||
                        ((box_used & expected_fixed[9*(BOX_START+(n/3)*9+n%3) +: 9]) != 0))
                        duplicate = 1;
                    row_used = row_used | expected_fixed[9*(u*9+n) +: 9];
                    col_used = col_used | expected_fixed[9*(n*9+u) +: 9];
                    box_used = box_used | expected_fixed[9*(BOX_START+(n/3)*9+n%3) +: 9];
                    row_multi = row_multi | (row_seen & candidates[9*(u*9+n) +: 9]);
                    col_multi = col_multi | (col_seen & candidates[9*(n*9+u) +: 9]);
                    box_multi = box_multi | (box_seen & candidates[9*(BOX_START+(n/3)*9+n%3) +: 9]);
                    row_seen = row_seen | candidates[9*(u*9+n) +: 9];
                    col_seen = col_seen | candidates[9*(n*9+u) +: 9];
                    box_seen = box_seen | candidates[9*(BOX_START+(n/3)*9+n%3) +: 9];
                end
            end
            assign expected_used[9*u +: 9] = row_used;
            assign expected_used[9*(9+u) +: 9] = col_used;
            assign expected_used[9*(18+u) +: 9] = box_used;
            assign expected_unique[9*u +: 9] = row_seen & ~row_multi;
            assign expected_unique[9*(9+u) +: 9] = col_seen & ~col_multi;
            assign expected_unique[9*(18+u) +: 9] = box_seen & ~box_multi;
            assign bad_unit[u] = duplicate || (row_seen != 9'h1ff) ||
                (col_seen != 9'h1ff) || (box_seen != 9'h1ff);
        end
    endgenerate
    wire expected_bad = (|empty_cell) || (|bad_unit);
    wire expected_solved = (&filled) && !expected_bad;
    assign mismatch = (fixed_digits != expected_fixed) || (unit_used != expected_used) ||
        (unit_unique != expected_unique) || (contradiction != expected_bad) ||
        (solved != expected_solved);
endmodule

// Partitioned comparison and scalar/vector metadata relation for update modes.
module formal_row_changes (
    input wire [728:0] candidates_in,
    input wire [8:0] previous_rows,
    input wire reset, load, advance,
    output wire mismatch
);
    wire [728:0] candidates_out;
    wire solved, contradiction, forced_conflict;
    wire [8:0] rows;
    deduction_engine dut(candidates_in, candidates_out, solved, contradiction, forced_conflict, rows);
    wire scalar = candidates_in != candidates_out;
    wire [8:0] next_rows = (reset || load) ? 9'h1ff : (advance ? rows : previous_rows);
    wire next_scalar = (reset || load) ? 1'b1 : (advance ? scalar : (|previous_rows));
    assign mismatch = ((|rows) != scalar) || ((|next_rows) != next_scalar);
endmodule
