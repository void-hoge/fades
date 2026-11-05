`timescale 1ns/1ps
// Inference engine: every rule and validation read the same current state.
// The core registers the successor and metadata; validation is NOT cascaded
// after inference. Helpers below are local building blocks of this engine.
// SEGMENT_RULES is an opt-in fixed-segment experiment: 0=baseline, 1=local
// three-cell Hall contradiction, 2=also remove fixed-segment naked triples,
// 3=also apply all two-cell Hall subsets inside each fixed segment,
// 4=restrict pairs to two identical bivalue cells, sharing cell features.
module deduction_engine #(parameter integer SEGMENT_RULES = 0,
    parameter integer LOCKED_CANDIDATES = 1) (
    input wire [728:0] candidates_in,
    output wire [728:0] candidates_out,
    output wire solved,
    output wire contradiction,
    output wire forced_conflict,
    output wire [8:0] row_changed
);
    wire [728:0] fixed_digits;
    wire [242:0] unit_used, unit_unique;
    wire [242:0] row_segments, col_segments;
    wire [242:0] row_multis, col_multis;
    wire [80:0] cell_two;
    wire cell_solved, cell_contradiction, segment_contradiction;
    analysis analyze(candidates_in, fixed_digits, unit_used, unit_unique,
        cell_solved, cell_contradiction, row_segments, col_segments, cell_two,
        row_multis, col_multis);
    inference_rules #(.SEGMENT_RULES(SEGMENT_RULES), .LOCKED_CANDIDATES(LOCKED_CANDIDATES)) rules(
        candidates_in, fixed_digits, unit_used, unit_unique,
        candidates_out, forced_conflict, segment_contradiction,
        row_segments, col_segments, cell_two, row_multis, col_multis);
    assign contradiction = cell_contradiction || segment_contradiction;
    assign solved = cell_solved && !segment_contradiction;
    genvar row;
    generate for (row = 0; row < 9; row = row + 1) begin : changes
        assign row_changed[row] = candidates_in[81*row +: 81] !=
            candidates_out[81*row +: 81];
    end endgenerate
endmodule

// Singles and locked candidates, consuming the shared analysis.
/* verilator lint_off DECLFILENAME */
module inference_rules #(parameter integer SEGMENT_RULES = 0,
    parameter integer LOCKED_CANDIDATES = 1) (
    input wire [728:0] candidates_in,
    input wire [728:0] fixed_digits,
    input wire [242:0] unit_used,
    input wire [242:0] unit_unique,
    output wire [728:0] candidates_out,
    output wire forced_conflict,
    output wire segment_contradiction,
    input wire [242:0] row_segments,
    input wire [242:0] col_segments,
    input wire [80:0] cell_two,
    input wire [242:0] row_multis, col_multis
);
    wire [8:0] candidates [0:80];
    wire [8:0] row_used [0:8], col_used [0:8], box_used [0:8];
    wire [8:0] row_unique [0:8], col_unique [0:8], box_unique [0:8];
    wire [8:0] row_segment [0:26], col_segment [0:26];
    wire [8:0] row_pointing [0:26], col_pointing [0:26];
    wire [8:0] row_claiming [0:26], col_claiming [0:26];
    wire [8:0] row_locked [0:26], col_locked [0:26];
    wire [8:0] row_triple [0:26], col_triple [0:26];
    wire [8:0] row_reserved [0:26], col_reserved [0:26];
    wire [26:0] row_pair_local [0:26], col_pair_local [0:26];
    wire [8:0] row_extra [0:26], col_extra [0:26];
    wire [53:0] bad_segment;
    wire [80:0] bad_forced;
    // Optional mode-4 features share the interface with earlier modes.
    wire unused_pair_features = ^{cell_two, row_multis, col_multis};
    function single;
        input [8:0] bits;
        begin
            single = (bits != 0) && ((bits & (bits - 9'd1)) == 0);
        end
    endfunction
    function [3:0] count9;
        input [8:0] bits;
        integer d;
        begin
            count9 = 0;
            for (d = 0; d < 9; d = d + 1) count9 = count9 + {3'b0, bits[d]};
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
                assign row_segment[u*3+s] = row_segments[9*(u*3+s) +: 9];
                assign col_segment[u*3+s] = col_segments[9*(u*3+s) +: 9];
                if (SEGMENT_RULES >= 3) begin : pair_rules
                    wire [8:0] row_pair, col_pair;
                    segment_pairs #(.BIVALUE_ONLY(SEGMENT_RULES == 4 ? 1 : 0)) row_pairs(candidates[u*9+s*3],
                        candidates[u*9+s*3+1], candidates[u*9+s*3+2],
                        row_pair, row_pair_local[u*3+s],
                        cell_two[u*9+s*3 +: 3], row_multis[9*(u*3+s) +: 9]);
                    segment_pairs #(.BIVALUE_ONLY(SEGMENT_RULES == 4 ? 1 : 0)) col_pairs(candidates[s*27+u],
                        candidates[s*27+9+u], candidates[s*27+18+u],
                        col_pair, col_pair_local[u*3+s],
                        {cell_two[s*27+18+u], cell_two[s*27+9+u], cell_two[s*27+u]},
                        col_multis[9*(u*3+s) +: 9]);
                    assign row_reserved[u*3+s] = row_triple[u*3+s] | row_pair;
                    assign col_reserved[u*3+s] = col_triple[u*3+s] | col_pair;
                end else begin : no_pairs
                    assign row_reserved[u*3+s] = row_triple[u*3+s];
                    assign col_reserved[u*3+s] = col_triple[u*3+s];
                    assign row_pair_local[u*3+s] = 0;
                    assign col_pair_local[u*3+s] = 0;
                end
                if (SEGMENT_RULES != 0) begin : segment_rules
                    wire [3:0] row_count = count9(row_segment[u*3+s]);
                    wire [3:0] col_count = count9(col_segment[u*3+s]);
                    assign bad_segment[u*3+s] = row_count < 3;
                    assign bad_segment[27+u*3+s] = col_count < 3;
                    assign row_triple[u*3+s] = (SEGMENT_RULES >= 2 && row_count == 3) ?
                        row_segment[u*3+s] : 9'b0;
                    assign col_triple[u*3+s] = (SEGMENT_RULES >= 2 && col_count == 3) ?
                        col_segment[u*3+s] : 9'b0;
                end else begin : baseline
                    assign bad_segment[u*3+s] = 0;
                    assign bad_segment[27+u*3+s] = 0;
                    assign row_triple[u*3+s] = 0;
                    assign col_triple[u*3+s] = 0;
                end
                // A tight pair/triple occupies these digits in both line
                // and box. External segments share the fixed distribution;
                // inside the source, only the third cell receives a pair.
                assign row_extra[u*3+s] = row_reserved[u*3+(s+1)%3] |
                    row_reserved[u*3+(s+2)%3] |
                    row_reserved[OTHER1*3+s] | row_reserved[OTHER2*3+s];
                assign col_extra[u*3+s] = col_reserved[u*3+(s+1)%3] |
                    col_reserved[u*3+(s+2)%3] |
                    col_reserved[OTHER1*3+s] | col_reserved[OTHER2*3+s];
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
                (LOCKED_CANDIDATES != 0 ? (row_locked[R*3+C/3] | col_locked[C*3+R/3]) : 9'b0) |
                row_extra[R*3+C/3] | col_extra[C*3+R/3] |
                row_pair_local[R*3+C/3][9*(C%3) +: 9] |
                col_pair_local[C*3+R/3][9*(R%3) +: 9];
            assign forced = candidates[i] & (row_unique[R] | col_unique[C] | box_unique[B]);
            assign candidates_out[9*i +: 9] = candidates[i] & ~removed &
                ((forced != 0) ? forced : 9'h1ff);
            assign bad_forced[i] = (forced != 0) && !single(forced);
        end
    endgenerate
    assign forced_conflict = |bad_forced;
    assign segment_contradiction = |bad_segment;
endmodule

// All two-cell subsets of a fixed segment, or identical-bivalue pairs only.
// In the latter case both cardinalities are known: x subset y implies x=y.
module segment_pairs #(parameter integer BIVALUE_ONLY = 0) (
    input wire [8:0] a, b, c,
    output wire [8:0] reserved,
    output wire [26:0] local_removed,
    input wire [2:0] twos,
    input wire [8:0] multi
);
    function [8:0] tight_pair;
        input [8:0] bits;
        reg [8:0] rest;
        begin
            rest = bits & (bits - 9'd1);
            tight_pair = (rest != 0 && ((rest & (rest - 9'd1)) == 0)) ? bits : 9'b0;
        end
    endfunction
    generate if (BIVALUE_ONLY != 0) begin : bivalue
        wire ab = twos[0] && twos[1] && ((a & ~b) == 0);
        wire ac = twos[0] && twos[2] && ((a & ~c) == 0);
        wire bc = twos[1] && twos[2] && ((b & ~c) == 0);
        assign local_removed = {ab ? a : 9'b0, ac ? a : 9'b0, bc ? b : 9'b0};
        // If any two domains are identical, their common mask equals the
        // per-digit repeated-occurrence mask of all three cells. Reuse the
        // M already needed by Hidden Single instead of ORing three masks.
        assign reserved = (ab || ac || bc) ? multi : 9'b0;
    end else begin : full_pairs
        wire unused_shared_features = ^{twos, multi};
        wire [8:0] without_a = tight_pair(b | c);
        wire [8:0] without_b = tight_pair(a | c);
        wire [8:0] without_c = tight_pair(a | b);
        assign local_removed = {without_c, without_b, without_a};
        assign reserved = without_a | without_b | without_c;
    end endgenerate
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
    output wire contradiction,
    output wire [242:0] row_segments,
    output wire [242:0] col_segments,
    output wire [80:0] cell_two,
    output wire [242:0] row_multis, col_multis
);
    wire [80:0] empty_cell, filled;
    wire [26:0] bad_unit;
    wire [8:0] row_multi [0:26], col_multi [0:26];
    genvar i, u, p;
    generate
        for (i = 0; i < 81; i = i + 1) begin : cells
            wire [8:0] mask = candidates[9*i +: 9];
            cell_cardinality features(mask, empty_cell[i], filled[i], cell_two[i]);
            assign fixed_digits[9*i +: 9] = filled[i] ? mask : 9'b0;
        end
        // Shared first ternary level: the same presence bits feed unit
        // occurrence analysis, locked candidates and fixed-segment rules.
        for (i = 0; i < 27; i = i + 1) begin : segments
            localparam integer R = (i/3)*9+(i%3)*3;
            localparam integer C = (i%3)*27+i/3;
            segment_occurrences row_summary(candidates[9*R +: 9],
                candidates[9*(R+1) +: 9], candidates[9*(R+2) +: 9],
                row_segments[9*i +: 9], row_multi[i]);
            assign row_multis[9*i +: 9] = row_multi[i];
            segment_occurrences col_summary(candidates[9*C +: 9],
                candidates[9*(C+9) +: 9], candidates[9*(C+18) +: 9],
                col_segments[9*i +: 9], col_multi[i]);
            assign col_multis[9*i +: 9] = col_multi[i];
        end
        for (u = 0; u < 27; u = u + 1) begin : units
            wire [80:0] fixed_masks;
            wire [8:0] segment_seen [0:2], segment_multi [0:2];
            wire [8:0] candidate_seen, candidate_multi, fixed_seen, fixed_multi;
            for (p = 0; p < 3; p = p + 1) begin : segments
                // Blocks use horizontal segments only; their vertical
                // partition is not computed again for unit analysis.
                localparam integer S = (u < 18) ? (u%9)*3+p :
                    ((u-18)/3)*9+p*3+(u-18)%3;
                if (u >= 9 && u < 18) begin : column
                    assign segment_seen[p] = col_segments[9*S +: 9];
                    assign segment_multi[p] = col_multi[S];
                end else begin : row_or_box
                    assign segment_seen[p] = row_segments[9*S +: 9];
                    assign segment_multi[p] = row_multi[S];
                end
            end
            for (p = 0; p < 9; p = p + 1) begin : positions
                localparam integer CELL = (u < 9) ? u*9+p :
                    ((u < 18) ? p*9+(u-9) :
                    ((u-18)/3)*27+((u-18)%3)*3+(p/3)*9+p%3);
                assign fixed_masks[9*p +: 9] = fixed_digits[9*CELL +: 9];
            end
            assign candidate_seen = segment_seen[0] | segment_seen[1] | segment_seen[2];
            assign candidate_multi = segment_multi[0] | segment_multi[1] | segment_multi[2] |
                (segment_seen[0] & segment_seen[1]) |
                (segment_seen[0] & segment_seen[2]) |
                (segment_seen[1] & segment_seen[2]);
            unit_occurrences fixed_summary(fixed_masks, fixed_seen, fixed_multi);
            assign unit_used[9*u +: 9] = fixed_seen;
            assign unit_unique[9*u +: 9] = candidate_seen & ~candidate_multi;
            assign bad_unit[u] = (candidate_seen != 9'h1ff) || (|fixed_multi);
        end
    endgenerate
    assign contradiction = (|empty_cell) || (|bad_unit);
    assign solved = (&filled) && !contradiction;
endmodule

// The first clear-lowest-bit result is shared by existing Naked Single and
// the new bivalue feature. The latter feeds four fixed cell pairs per cell.
module cell_cardinality (
    input wire [8:0] mask,
    output wire empty, one, two
);
    wire [8:0] rest = mask & (mask - 9'd1);
    assign empty = mask == 0;
    assign one = !empty && rest == 0;
    assign two = rest != 0 && ((rest & (rest - 9'd1)) == 0);
endmodule

// Three cells -> per-digit presence and repeated occurrence, without state.
module segment_occurrences (
    input wire [8:0] a, b, c,
    output wire [8:0] seen, multi
);
    assign seen = a | b | c;
    assign multi = (a & b) | (a & c) | (b & c);
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
