`timescale 1ns/1ps
// Branch generator: pair-priority cell selection and balanced candidate splitting.
module branch_generator (
    input wire [728:0] candidates,
    output wire [728:0] child_a,
    output wire [728:0] child_b
);
    wire [80:0] selected;
    genvar c, s;
    generate for (c = 0; c < 81; c = c + 1) begin : cells
        wire [8:0] mask = candidates[9*c +: 9];
        logic [3:0] count, rank;
        logic [8:0] half;
        integer d;
        always_comb begin
            count = 0;
            for (d = 0; d < 9; d = d + 1)
                count = count + {3'b0, mask[d]};
            rank = 0;
            half = 0;
            for (d = 0; d < 9; d = d + 1) begin
                if (mask[d]) begin
                    if (rank < (count >> 1)) half[d] = 1;
                    rank = rank + 1'b1;
                end
            end
        end
        assign child_a[9*c +: 9] = selected[c] ? half : mask;
        assign child_b[9*c +: 9] = selected[c] ? (mask & ~half) : mask;
    end
        // Keep eligibility outside the selector. This boundary and factoring
        // preserve the smaller mapping measured with the original pair policy.
        if (1) begin : pair_policy
            wire [80:0] unresolved, pair;
            for (s = 0; s < 81; s = s + 1) begin : eligibility
                wire [8:0] mask = candidates[9*s +: 9];
                wire [8:0] without_first = mask & (mask - 9'd1);
                assign unresolved[s] = without_first != 0;
                assign pair[s] = (without_first != 0) &&
                    ((without_first & (without_first - 9'd1)) == 0);
            end
            branch_select selection(unresolved, pair, selected);
        end
    endgenerate
endmodule

// Seven binary levels over 81 cells and 47 invalid padding leaves.
// Classes: 2 = two candidates, 1 = three or more, 0 = invalid.
// Eligibility comes from the generator, independently of its split popcount.
// A tie always keeps the leftmost (row-major) cell.
/* verilator lint_off DECLFILENAME */
module branch_select (
    input wire [80:0] unresolved,
    input wire [80:0] pair,
    output wire [80:0] selected
);
    genvar level, node, cell_index;
    generate
        for (level = 0; level < 8; level = level + 1) begin : rounds
            wire [1:0] node_class [0:(128 >> level)-1];
            wire [6:0] node_index [0:(128 >> level)-1];
            for (node = 0; node < (128 >> level); node = node + 1) begin : bracket
                if (level == 0) begin : leaf
                    if (node < 81) begin : real_cell
                        assign node_class[node] = pair[node] ? 2'd2 :
                            (unresolved[node] ? 2'd1 : 2'd0);
                        assign node_index[node] = node[6:0];
                    end else begin : padding
                        assign node_class[node] = 0;
                        assign node_index[node] = 0;
                    end
                end else begin : match_pair
                    wire left_wins = rounds[level-1].node_class[2*node] >=
                        rounds[level-1].node_class[2*node+1];
                    assign node_class[node] = left_wins ?
                        rounds[level-1].node_class[2*node] : rounds[level-1].node_class[2*node+1];
                    assign node_index[node] = left_wins ?
                        rounds[level-1].node_index[2*node] : rounds[level-1].node_index[2*node+1];
                end
            end
        end
        for (cell_index = 0; cell_index < 81; cell_index = cell_index + 1) begin : decode
            assign selected[cell_index] = (rounds[7].node_class[0] != 0) &&
                (rounds[7].node_index[0] == cell_index[6:0]);
        end
    endgenerate
endmodule
