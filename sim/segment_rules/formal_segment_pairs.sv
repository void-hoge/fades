// Universal assignment witness: for arbitrary 9-bit domains and any legal
// distinct assignment, neither local pruning nor external reservation may
// eliminate a value used by that assignment. No technology synthesis needed.
module formal_segment_pairs (
    input wire [8:0] a, b, c,
    input wire [3:0] da, db, dc,
    output wire mismatch
);
    wire [8:0] reserved;
    wire [26:0] local_removed;
    wire [2:0] singles, twos, empty;
    cell_cardinality fa(a, empty[0], singles[0], twos[0]);
    cell_cardinality fb(b, empty[1], singles[1], twos[1]);
    cell_cardinality fc(c, empty[2], singles[2], twos[2]);
    segment_pairs dut(a, b, c, reserved, local_removed, twos, 9'b0);
    wire legal = da < 9 && db < 9 && dc < 9 &&
        da != db && da != dc && db != dc && a[da] && b[db] && c[dc];
    wire [8:0] assigned = (9'b1 << da) | (9'b1 << db) | (9'b1 << dc);
    assign mismatch = legal && (local_removed[da] || local_removed[9+db] ||
        local_removed[18+dc] || ((reserved & ~assigned) != 0));
endmodule

// Exact equality with the original union-counting rule, even for empty or
// contradictory domains. No assumptions about feasibility are required.
module formal_pair_factorization (
    input wire [8:0] a, b, c,
    output wire mismatch
);
    function [8:0] reference_pair;
        input [8:0] bits;
        reg [8:0] rest;
        begin
            rest = bits & (bits - 9'd1);
            reference_pair = (rest != 0 && ((rest & (rest - 9'd1)) == 0)) ? bits : 9'b0;
        end
    endfunction
    wire [8:0] reserved;
    wire [26:0] local_removed;
    wire [2:0] singles, twos, empty;
    cell_cardinality fa(a, empty[0], singles[0], twos[0]);
    cell_cardinality fb(b, empty[1], singles[1], twos[1]);
    cell_cardinality fc(c, empty[2], singles[2], twos[2]);
    shared_cardinality_pairs dut(a, b, c, reserved, local_removed, singles, twos);
    wire [8:0] without_a = reference_pair(b | c);
    wire [8:0] without_b = reference_pair(a | c);
    wire [8:0] without_c = reference_pair(a | b);
    assign mismatch = (local_removed != {without_c, without_b, without_a}) ||
        (reserved != (without_a | without_b | without_c));
endmodule

// Analysis-only candidate, not instantiated by the production engine. Exact
// factorization was correct but had a larger expression DAG than the original.
module shared_cardinality_pairs (
    input wire [8:0] a, b, c,
    output wire [8:0] reserved,
    output wire [26:0] local_removed,
    input wire [2:0] singles, twos
);
    function [8:0] tight_pair;
        input [8:0] x, y;
        input x_single, y_single, x_two, y_two;
        reg x_contains_y, y_contains_x, separate_singles, use_x, use_y;
        begin
            x_contains_y = (y & ~x) == 0;
            y_contains_x = (x & ~y) == 0;
            separate_singles = x_single && y_single && !x_contains_y;
            use_x = (x_two && x_contains_y) || separate_singles;
            use_y = (y_two && y_contains_x) || separate_singles;
            tight_pair = (use_x ? x : 9'b0) | (use_y ? y : 9'b0);
        end
    endfunction
    wire [8:0] without_a = tight_pair(b, c, singles[1], singles[2], twos[1], twos[2]);
    wire [8:0] without_b = tight_pair(a, c, singles[0], singles[2], twos[0], twos[2]);
    wire [8:0] without_c = tight_pair(a, b, singles[0], singles[1], twos[0], twos[1]);
    assign local_removed = {without_c, without_b, without_a};
    assign reserved = without_a | without_b | without_c;
endmodule

module formal_bivalue_pairs (
    input wire [8:0] a, b, c,
    output wire mismatch
);
    function [8:0] reference_pair;
        input [8:0] x, y;
        reg [3:0] count;
        integer d;
        begin
            count = 0;
            for (d = 0; d < 9; d = d + 1) count = count + {3'b0, x[d]};
            reference_pair = count == 2 && x == y ? x : 9'b0;
        end
    endfunction
    wire [8:0] reserved;
    wire [26:0] local_removed;
    wire [2:0] singles, twos, empty;
    cell_cardinality fa(a, empty[0], singles[0], twos[0]);
    cell_cardinality fb(b, empty[1], singles[1], twos[1]);
    cell_cardinality fc(c, empty[2], singles[2], twos[2]);
    segment_pairs #(.BIVALUE_ONLY(1)) dut(a, b, c, reserved, local_removed, twos,
        (a & b) | (a & c) | (b & c));
    wire [8:0] without_a = reference_pair(b, c);
    wire [8:0] without_b = reference_pair(a, c);
    wire [8:0] without_c = reference_pair(a, b);
    assign mismatch = (local_removed != {without_c, without_b, without_a}) ||
        (reserved != (without_a | without_b | without_c));
endmodule

// Local Singles + identical-bivalue pairs, including reservation outside the
// three source cells. Only used by the two-round propagation proof below.
module local_bivalue_step (
    input wire [8:0] a, b, c,
    output wire [8:0] next_a, next_b, next_c, external_removed
);
    wire [2:0] empty, singles, twos;
    cell_cardinality fa(a, empty[0], singles[0], twos[0]);
    cell_cardinality fb(b, empty[1], singles[1], twos[1]);
    cell_cardinality fc(c, empty[2], singles[2], twos[2]);
    wire [8:0] fixed_a = singles[0] ? a : 9'b0;
    wire [8:0] fixed_b = singles[1] ? b : 9'b0;
    wire [8:0] fixed_c = singles[2] ? c : 9'b0;
    wire [8:0] reserved;
    wire [26:0] local_removed;
    segment_pairs #(.BIVALUE_ONLY(1)) pairs(a, b, c, reserved, local_removed, twos,
        (a & b) | (a & c) | (b & c));
    assign next_a = a & ~(fixed_b | fixed_c | local_removed[0 +: 9]);
    assign next_b = b & ~(fixed_a | fixed_c | local_removed[9 +: 9]);
    assign next_c = c & ~(fixed_a | fixed_b | local_removed[18 +: 9]);
    assign external_removed = fixed_a | fixed_b | fixed_c | reserved;
endmodule

// For any feasible three-cell domains, two local updates of the inexpensive
// rules include every removal of one full-pair+Singles update. Thus omitted
// singleton+bivalue pairs are shortcuts, not an additional closure rule.
module formal_pair_two_rounds (
    input wire [8:0] a, b, c,
    input wire [3:0] da, db, dc,
    output wire mismatch
);
    function [8:0] fixed_digit;
        input [8:0] bits;
        begin
            fixed_digit = bits != 0 && ((bits & (bits - 9'd1)) == 0) ? bits : 9'b0;
        end
    endfunction
    wire legal = da < 9 && db < 9 && dc < 9 &&
        da != db && da != dc && db != dc && a[da] && b[db] && c[dc];
    wire [8:0] a1, b1, c1, a2, b2, c2, external1, external2;
    local_bivalue_step first(a, b, c, a1, b1, c1, external1);
    local_bivalue_step second(a1, b1, c1, a2, b2, c2, external2);
    wire [8:0] reserved;
    wire [26:0] local_removed;
    segment_pairs full_pairs(a, b, c, reserved, local_removed, 3'b0, 9'b0);
    wire [8:0] fixed_a = fixed_digit(a), fixed_b = fixed_digit(b), fixed_c = fixed_digit(c);
    wire [8:0] full_a = a & ~(fixed_b | fixed_c | local_removed[0 +: 9]);
    wire [8:0] full_b = b & ~(fixed_a | fixed_c | local_removed[9 +: 9]);
    wire [8:0] full_c = c & ~(fixed_a | fixed_b | local_removed[18 +: 9]);
    wire [8:0] full_external = fixed_a | fixed_b | fixed_c | reserved;
    assign mismatch = legal && (((a2 & ~full_a) != 0) || ((b2 & ~full_b) != 0) ||
        ((c2 & ~full_c) != 0) || ((full_external & ~(external1 | external2)) != 0));
endmodule
