`timescale 1ns/1ps
// Frozen pre-sharing specification: combine all eight locked-candidate
// terms separately at every cell, independently of the DUT's segment masks.
module reference_inference_rules (
    input wire [728:0] candidates_in,
    input wire [728:0] fixed_digits,
    input wire [242:0] unit_used,
    input wire [242:0] unit_unique,
    output wire [728:0] candidates_out,
    output wire changed,
    output wire forced_conflict
);
    wire [8:0] candidates [0:80];
    wire [8:0] row_used [0:8], col_used [0:8], box_used [0:8];
    wire [8:0] row_unique [0:8], col_unique [0:8], box_unique [0:8];
    wire [8:0] row_segment [0:26], col_segment [0:26];
    wire [8:0] row_pointing [0:26], col_pointing [0:26];
    wire [8:0] row_claiming [0:26], col_claiming [0:26];
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
                row_pointing[R*3+(C/3+1)%3] |
                row_pointing[R*3+(C/3+2)%3] |
                col_pointing[C*3+(R/3+1)%3] |
                col_pointing[C*3+(R/3+2)%3] |
                row_claiming[((R/3)*3+(R+1)%3)*3+C/3] |
                row_claiming[((R/3)*3+(R+2)%3)*3+C/3] |
                col_claiming[((C/3)*3+(C+1)%3)*3+R/3] |
                col_claiming[((C/3)*3+(C+2)%3)*3+R/3];
            assign forced = candidates[i] & (row_unique[R] | col_unique[C] | box_unique[B]);
            assign candidates_out[9*i +: 9] = candidates[i] & ~removed &
                ((forced != 0) ? forced : 9'h1ff);
            assign bad_forced[i] = (forced != 0) && !single(forced);
        end
    endgenerate
    assign changed = candidates_out != candidates_in;
    assign forced_conflict = |bad_forced;
endmodule

// Features are unconstrained as well as candidates: prove exact equivalence
// even for malformed states and inconsistent shared-analysis inputs.
module formal_locked_masks (
    input wire [728:0] candidates_in,
    input wire [728:0] fixed_digits,
    input wire [242:0] unit_used,
    input wire [242:0] unit_unique,
    output wire mismatch
);
    wire [728:0] actual, expected;
    wire actual_conflict, expected_conflict, expected_changed;
    inference_rules dut(candidates_in, fixed_digits, unit_used, unit_unique,
        actual, actual_conflict);
    reference_inference_rules reference_rules(candidates_in, fixed_digits,
        unit_used, unit_unique, expected, expected_changed, expected_conflict);
    assign mismatch = (actual != expected) || (actual_conflict != expected_conflict) ||
        ((actual != candidates_in) != expected_changed);
endmodule
