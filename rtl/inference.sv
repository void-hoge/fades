`timescale 1ns/1ps
// Inference engine: every rule and validation read the same current state.
// The core registers the successor and metadata; validation is NOT cascaded
// after inference. Helpers below are local building blocks of this engine.
module inference (
    input wire [728:0] candidates_in,
    output wire [728:0] candidates_out,
    output wire solved,
    output wire contradiction,
    output wire forced_conflict,
    output wire [8:0] row_changed
);
    wire [728:0] fixed_digits;
    wire [242:0] unit_used, unit_unique;
    analysis analyze(candidates_in, fixed_digits, unit_used, unit_unique,
        solved, contradiction);
    inference_rules rules(candidates_in, fixed_digits, unit_used, unit_unique,
        candidates_out, forced_conflict);
    genvar row;
    generate for (row = 0; row < 9; row = row + 1) begin : changes
        assign row_changed[row] = candidates_in[81*row +: 81] !=
            candidates_out[81*row +: 81];
    end endgenerate
endmodule

// Singles and locked candidates, consuming the shared analysis.
/* verilator lint_off DECLFILENAME */
module inference_rules (
    input wire [728:0] candidates_in,
    input wire [728:0] fixed_digits,
    input wire [242:0] unit_used,
    input wire [242:0] unit_unique,
    output wire [728:0] candidates_out,
    output wire forced_conflict
);
    wire [8:0] candidates [0:80];
    wire [8:0] row_used [0:8], col_used [0:8], box_used [0:8];
    wire [8:0] row_unique [0:8], col_unique [0:8], box_unique [0:8];
    wire [8:0] row_segment [0:26], col_segment [0:26];
    wire [8:0] row_pointing [0:26], col_pointing [0:26];
    wire [8:0] row_claiming [0:26], col_claiming [0:26];
    wire [8:0] row_locked [0:26], col_locked [0:26];
    wire [80:0] bad_forced;
    function single;
        input [8:0] bits;
        begin
            single = (bits != 0) && ((bits & (bits - 9'd1)) == 0);
        end
    endfunction
    genvar u, s, i;
    generate
        for (u = 0; u < 9; u = u + 1) begin : units
            assign row_used[u] = unit_used[9*u +: 9];
            assign col_used[u] = unit_used[9*(9+u) +: 9];
            assign box_used[u] = unit_used[9*(18+u) +: 9];
            assign row_unique[u] = unit_unique[9*u +: 9];
            assign col_unique[u] = unit_unique[9*(9+u) +: 9];
            assign box_unique[u] = unit_unique[9*(18+u) +: 9];
            for (s = 0; s < 3; s = s + 1) begin : segments
                localparam integer OTHER1 = (u / 3) * 3 + (u + 1) % 3;
                localparam integer OTHER2 = (u / 3) * 3 + (u + 2) % 3;
                assign row_segment[u*3+s] = candidates[u*9+s*3] |
                    candidates[u*9+s*3+1] | candidates[u*9+s*3+2];
                assign col_segment[u*3+s] = candidates[s*27+u] |
                    candidates[s*27+9+u] | candidates[s*27+18+u];
                assign row_pointing[u*3+s] = row_segment[u*3+s] &
                    ~(row_segment[OTHER1*3+s] | row_segment[OTHER2*3+s]);
                assign col_pointing[u*3+s] = col_segment[u*3+s] &
                    ~(col_segment[OTHER1*3+s] | col_segment[OTHER2*3+s]);
                assign row_claiming[u*3+s] = row_segment[u*3+s] &
                    ~(row_segment[u*3+(s+1)%3] | row_segment[u*3+(s+2)%3]);
                assign col_claiming[u*3+s] = col_segment[u*3+s] &
                    ~(col_segment[u*3+(s+1)%3] | col_segment[u*3+(s+2)%3]);
                // Each mask applies to all three cells of the target segment.
                // Pointing comes from the other blocks on the same line;
                // claiming comes from the other lines inside this block.
                assign row_locked[u*3+s] = row_pointing[u*3+(s+1)%3] |
                    row_pointing[u*3+(s+2)%3] |
                    row_claiming[OTHER1*3+s] | row_claiming[OTHER2*3+s];
                assign col_locked[u*3+s] = col_pointing[u*3+(s+1)%3] |
                    col_pointing[u*3+(s+2)%3] |
                    col_claiming[OTHER1*3+s] | col_claiming[OTHER2*3+s];
            end
        end
        for (i = 0; i < 81; i = i + 1) begin : cells
            localparam integer R = i / 9;
            localparam integer C = i % 9;
            localparam integer B = (R / 3) * 3 + C / 3;
            wire [8:0] forced, removed, peer_digits;
            assign candidates[i] = candidates_in[9*i +: 9];
            assign peer_digits = (row_used[R] | col_used[C] | box_used[B]) & ~fixed_digits[9*i +: 9];
            assign removed = peer_digits |
                row_locked[R*3+C/3] | col_locked[C*3+R/3];
            assign forced = candidates[i] & (row_unique[R] | col_unique[C] | box_unique[B]);
            assign candidates_out[9*i +: 9] = candidates[i] & ~removed &
                ((forced != 0) ? forced : 9'h1ff);
            assign bad_forced[i] = (forced != 0) && !single(forced);
        end
    endgenerate
    assign forced_conflict = |bad_forced;
endmodule

// Shared candidate/fixed-digit occurrence analysis. Unit order is rows,
// columns, blocks. Fixed-digit duplicates are contradictions, candidate
// multiplicity is normal.
module analysis (
    input wire [728:0] candidates,
    output wire [728:0] fixed_digits,
    output wire [242:0] unit_used,
    output wire [242:0] unit_unique,
    output wire solved,
    output wire contradiction
);
    wire [80:0] empty_cell, filled;
    wire [26:0] bad_unit;
    genvar i, u, p;
    generate
        for (i = 0; i < 81; i = i + 1) begin : cells
            wire [8:0] mask = candidates[9*i +: 9];
            assign empty_cell[i] = mask == 9'b0;
            assign filled[i] = !empty_cell[i] && ((mask & (mask - 9'd1)) == 9'b0);
            assign fixed_digits[9*i +: 9] = filled[i] ? mask : 9'b0;
        end
        for (u = 0; u < 27; u = u + 1) begin : units
            wire [80:0] candidate_masks, fixed_masks;
            wire [8:0] candidate_seen, candidate_multi, fixed_seen, fixed_multi;
            for (p = 0; p < 9; p = p + 1) begin : positions
                localparam integer CELL = (u < 9) ? u*9+p :
                    ((u < 18) ? p*9+(u-9) :
                    ((u-18)/3)*27+((u-18)%3)*3+(p/3)*9+p%3);
                assign candidate_masks[9*p +: 9] = candidates[9*CELL +: 9];
                assign fixed_masks[9*p +: 9] = fixed_digits[9*CELL +: 9];
            end
            unit_occurrences candidate_summary(candidate_masks, candidate_seen, candidate_multi);
            unit_occurrences fixed_summary(fixed_masks, fixed_seen, fixed_multi);
            assign unit_used[9*u +: 9] = fixed_seen;
            assign unit_unique[9*u +: 9] = candidate_seen & ~candidate_multi;
            assign bad_unit[u] = (candidate_seen != 9'h1ff) || (|fixed_multi);
        end
    endgenerate
    assign contradiction = (|empty_cell) || (|bad_unit);
    assign solved = (&filled) && !contradiction;
endmodule

// Nine 9-bit masks -> presence and repeated occurrence, in two ternary levels.
module unit_occurrences (
    input wire [80:0] masks,
    output wire [8:0] seen,
    output wire [8:0] multi
);
    genvar level, n;
    generate
        for (level = 0; level < 3; level = level + 1) begin : levels
            localparam integer WIDTH = 9 / (3 ** level);
            wire [8:0] node_seen [0:WIDTH-1];
            wire [8:0] node_multi [0:WIDTH-1];
            for (n = 0; n < WIDTH; n = n + 1) begin : nodes
                if (level == 0) begin : leaf
                    assign node_seen[n] = masks[9*n +: 9];
                    assign node_multi[n] = 0;
                end else begin : merge
                    wire [8:0] a = levels[level-1].node_seen[3*n];
                    wire [8:0] b = levels[level-1].node_seen[3*n+1];
                    wire [8:0] c = levels[level-1].node_seen[3*n+2];
                    assign node_seen[n] = a | b | c;
                    assign node_multi[n] = levels[level-1].node_multi[3*n] |
                        levels[level-1].node_multi[3*n+1] |
                        levels[level-1].node_multi[3*n+2] |
                        (a & b) | (a & c) | (b & c);
                end
            end
        end
    endgenerate
    assign seen = levels[2].node_seen[0];
    assign multi = levels[2].node_multi[0];
endmodule
