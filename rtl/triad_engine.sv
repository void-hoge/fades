`timescale 1ns/1ps
// Packed search state: C[728:0], N[1214:729], P[1538:1215].
// N is a candidate for the proposition "digit absent from this triad".
// Each invocation performs one registered phase, not a recursive closure.
module triad_engine (
    input wire [1538:0] state_in,
    input wire band_phase,
    output wire [1538:0] state_out,
    output wire solved,
    output wire contradiction
);
    wire [728:0] fixed_digits;
    wire [242:0] used_digits, unique_digits;
    wire cell_bad;
    analysis validate(state_in[728:0], fixed_digits, used_digits, unique_digits,
        solved, cell_bad);
    wire [728:0] box_cells, h_cells, v_cells;
    wire [485:0] box_negative, h_negative, v_negative;
    wire [323:0] configurations;
    wire [8:0] box_bad;
    wire [5:0] band_bad;
    genvar b, p, o, k;
    generate
        for (b = 0; b < 9; b = b + 1) begin : boxes
            wire [80:0] cells_in, cells_out;
            for (p = 0; p < 9; p = p + 1) begin : cells
                localparam integer CELL = (b/3)*27+(b%3)*3+(p/3)*9+p%3;
                assign cells_in[9*p +: 9] = state_in[9*CELL +: 9];
                assign box_cells[9*CELL +: 9] = cells_out[9*p +: 9];
            end
            triad_box propagate(cells_in, state_in[729+54*b +: 54],
                cells_out, box_negative[54*b +: 54], box_bad[b]);
        end
        for (o = 0; o < 2; o = o + 1) begin : orientations
            for (b = 0; b < 3; b = b + 1) begin : bands
                wire [242:0] cells_in, cells_out;
                wire [80:0] negative_in, negative_out;
                for (p = 0; p < 3; p = p + 1) begin : peers
                    localparam integer BOX = (o == 0) ? b*3+p : p*3+b;
                    for (k = 0; k < 9; k = k + 1) begin : cells
                        localparam integer CELL = (BOX/3)*27+(BOX%3)*3+(k/3)*9+k%3;
                        assign cells_in[81*p+9*k +: 9] = state_in[9*CELL +: 9];
                        if (o == 0) begin : horizontal
                            assign h_cells[9*CELL +: 9] = cells_out[81*p+9*k +: 9];
                        end else begin : vertical
                            assign v_cells[9*CELL +: 9] = cells_out[81*p+9*k +: 9];
                        end
                    end
                    for (k = 0; k < 3; k = k + 1) begin : triads
                        assign negative_in[27*p+9*k +: 9] = state_in[729+54*BOX+27*o+9*k +: 9];
                        if (o == 0) begin : horizontal
                            assign h_negative[54*BOX+9*k +: 9] = negative_out[27*p+9*k +: 9];
                            assign h_negative[54*BOX+27+9*k +: 9] = state_in[729+54*BOX+27+9*k +: 9];
                        end else begin : vertical
                            assign v_negative[54*BOX+27+9*k +: 9] = negative_out[27*p+9*k +: 9];
                            assign v_negative[54*BOX+9*k +: 9] = state_in[729+54*BOX+9*k +: 9];
                        end
                    end
                end
                triad_band #(.VERTICAL(o)) propagate(cells_in, negative_in,
                    state_in[1215+54*(3*o+b) +: 54], cells_out, negative_out,
                    configurations[54*(3*o+b) +: 54], band_bad[3*o+b]);
            end
        end
    endgenerate
    assign state_out = band_phase ?
        {configurations, h_negative & v_negative, h_cells & v_cells} :
        {state_in[1538:1215], box_negative, box_cells};
    // All checks consume the same input version. Phase-local conflicts are
    // inspected on their own phase; cross-phase restrictions travel in state.
    assign contradiction = cell_bad || (band_phase ? (|band_bad) : (|box_bad));
    // Validation side products are intentionally unused: propagation is triad-based.
    wire unused_analysis = ^{fixed_digits, used_digits, unique_digits};
endmodule

/* verilator lint_off DECLFILENAME */
module triad_box (
    input wire [80:0] cells_in,
    input wire [53:0] negative_in,
    output reg [80:0] cells_out,
    output reg [53:0] negative_out,
    output reg contradiction
);
    function integer count9(input [8:0] mask);
        integer d;
        begin
            count9 = 0;
            for (d = 0; d < 9; d = d + 1) count9 = count9 + {31'b0, mask[d]};
        end
    endfunction
    reg [8:0] c [0:8];
    reg [8:0] n [0:5];
    reg [8:0] ca [0:8];
    reg [8:0] na [0:5];
    reg [8:0] seen, multi, a, x, y, z, removed;
    integer i, t, j, c0, c1, c2;
    wire unused_indices = ^{c1, c2};
    always @* begin
        contradiction = 0;
        cells_out = cells_in;
        negative_out = negative_in;
        seen = 0; multi = 0; a = 0; x = 0; y = 0; z = 0; removed = 0;
        c0 = 0; c1 = 0; c2 = 0;
        for (i = 0; i < 9; i = i + 1) begin
            c[i] = cells_in[9*i +: 9];
            ca[i] = (count9(c[i]) == 1) ? c[i] : 9'b0;
            if (c[i] == 0) contradiction = 1;
        end
        for (t = 0; t < 6; t = t + 1) begin
            n[t] = negative_in[9*t +: 9];
            na[t] = (count9(n[t]) == 6) ? n[t] : 9'b0;
            if (count9(n[t]) < 6) contradiction = 1;
            c0 = (t < 3) ? t*3 : t-3;
            c1 = c0 + ((t < 3) ? 1 : 3);
            c2 = c0 + ((t < 3) ? 2 : 6);
            x = c[c0]; y = c[c1]; z = c[c2];
            // Exactly one of the three cells and the negative triad, per digit.
            if ((x | y | z | n[t]) != 9'h1ff) contradiction = 1;
            ca[c0] = ca[c0] | (x & ~(y | z | n[t]));
            ca[c1] = ca[c1] | (y & ~(x | z | n[t]));
            ca[c2] = ca[c2] | (z & ~(x | y | n[t]));
            na[t] = na[t] | (n[t] & ~(x | y | z));
        end
        for (i = 0; i < 9; i = i + 1) begin
            if (count9(ca[i]) > 1) contradiction = 1;
            multi = multi | (seen & ca[i]);
            seen = seen | ca[i];
        end
        if (multi != 0) contradiction = 1;
        for (t = 0; t < 6; t = t + 1) begin
            c0 = (t < 3) ? t*3 : t-3;
            c1 = c0 + ((t < 3) ? 1 : 3);
            c2 = c0 + ((t < 3) ? 2 : 6);
            a = ca[c0] | ca[c1] | ca[c2];
            if ((a & na[t]) != 0 || count9(na[t]) > 6) contradiction = 1;
            negative_out[9*t +: 9] = n[t] & ~a &
                ((count9(na[t]) == 6) ? na[t] : 9'h1ff);
        end
        for (i = 0; i < 9; i = i + 1) begin
            removed = na[i/3] | na[3+i%3];
            for (j = 0; j < 9; j = j + 1)
                if (j != i) removed = removed | ca[j];
            cells_out[9*i +: 9] = c[i] & ~removed &
                ((ca[i] != 0) ? ca[i] : 9'h1ff);
        end
    end
endmodule

module triad_band #(parameter integer VERTICAL = 0) (
    input wire [242:0] cells_in,
    input wire [80:0] negative_in,
    input wire [53:0] configurations_in,
    output reg [242:0] cells_out,
    output reg [80:0] negative_out,
    output reg [53:0] configurations_out,
    output reg contradiction
);
    function integer position(input integer cfg, input integer peer);
        begin
            case (cfg)
                0: position = peer;
                1: position = (peer+1)%3;
                2: position = (peer+2)%3;
                3: position = 2-peer;
                4: position = (peer == 0) ? 0 : 3-peer;
                default: position = (peer == 2) ? 2 : 1-peer;
            endcase
        end
    endfunction
    function integer count9(input [8:0] mask);
        integer d;
        begin
            count9 = 0;
            for (d = 0; d < 9; d = d + 1) count9 = count9 + {31'b0, mask[d]};
        end
    endfunction
    reg [8:0] triads [0:8];
    reg [8:0] possible, negative, forbidden, required, any_config;
    integer peer, elem, cfg, k, cell_index;
    always @* begin
        cells_out = cells_in;
        negative_out = negative_in;
        configurations_out = configurations_in;
        contradiction = 0;
        possible = 0; negative = 0; forbidden = 0; required = 0; any_config = 0;
        cell_index = 0;
        for (peer = 0; peer < 3; peer = peer + 1) begin
            for (elem = 0; elem < 3; elem = elem + 1) begin
                triads[3*peer+elem] = 0;
                for (cfg = 0; cfg < 6; cfg = cfg + 1)
                    if (position(cfg, peer) == elem)
                        triads[3*peer+elem] = triads[3*peer+elem] |
                            configurations_in[9*cfg +: 9];
            end
        end
        for (peer = 0; peer < 3; peer = peer + 1) begin
            for (elem = 0; elem < 3; elem = elem + 1) begin
                possible = 0;
                for (k = 0; k < 3; k = k + 1) begin
                    cell_index = (VERTICAL == 0) ? 3*elem+k : 3*k+elem;
                    possible = possible | cells_in[81*peer+9*cell_index +: 9];
                    cells_out[81*peer+9*cell_index +: 9] =
                        cells_in[81*peer+9*cell_index +: 9] & triads[3*peer+elem];
                end
                negative = negative_in[27*peer+9*elem +: 9];
                forbidden = ~possible | ((count9(negative) == 6) ? negative : 9'b0);
                required = ~negative |
                    ((count9(triads[3*peer+elem]) == 3) ? triads[3*peer+elem] : 9'b0);
                if (count9(triads[3*peer+elem]) < 3) contradiction = 1;
                negative_out[27*peer+9*elem +: 9] = negative &
                    (triads[3*peer+(elem+1)%3] | triads[3*peer+(elem+2)%3]);
                for (cfg = 0; cfg < 6; cfg = cfg + 1) begin
                    if (position(cfg, peer) == elem)
                        configurations_out[9*cfg +: 9] =
                            configurations_out[9*cfg +: 9] & ~forbidden;
                    else
                        configurations_out[9*cfg +: 9] =
                            configurations_out[9*cfg +: 9] & ~required;
                end
            end
        end
        for (cfg = 0; cfg < 6; cfg = cfg + 1)
            any_config = any_config | configurations_in[9*cfg +: 9];
        if (any_config != 9'h1ff) contradiction = 1;
    end
endmodule

module triad_branch_generator #(parameter integer PIPELINED = 0) (
    input wire clk,
    input wire [1538:0] state_in,
    output wire [1538:0] child_a,
    output wire [1538:0] child_b,
    output wire valid
);
    wire [53:0] band_configs [0:5];
    wire [5:0] totals [0:5];
    reg [53:0] configs_next;
    reg [5:0] band_next;
    reg band_valid_next;
    wire [53:0] selected_configs;
    wire [5:0] band_stage, selected_band;
    wire band_valid;
    reg [8:0] digit_next;
    reg [5:0] present_next;
    wire [8:0] selected_digit;
    wire [5:0] present;
    wire [5:0] first_half;
    integer band, digit, cfg, total, best_total, best_band;
    integer count, rank, best_rank, best_digit, selected_count;
    function [5:0] count54(input [53:0] bits);
        integer i;
        begin
            count54 = 0;
            for (i = 0; i < 54; i = i + 1) count54 = count54 + {5'b0, bits[i]};
        end
    endfunction
    // In the core, selection runs speculatively while inference proceeds. A
    // full unchanged box+band round guarantees two stable input clocks before
    // the halt cycle, so both selection stages are current when DFS commits.
    generate if (PIPELINED != 0) begin : pipeline
        reg [53:0] configs_q;
        reg [5:0] band_q, final_band_q, present_q;
        reg [8:0] digit_q;
        reg valid_q, final_valid_q;
        always @(posedge clk) begin
            configs_q <= configs_next;
            band_q <= band_next;
            valid_q <= band_valid_next;
            digit_q <= digit_next;
            present_q <= present_next;
            final_band_q <= band_stage;
            final_valid_q <= valid_q;
        end
        assign selected_configs = configs_q;
        assign band_stage = band_q;
        assign band_valid = valid_q;
        assign selected_band = final_band_q;
        assign selected_digit = digit_q;
        assign present = present_q;
        assign valid = final_valid_q;
    end else begin : combinational
        wire unused_clk = clk;
        assign selected_configs = configs_next;
        assign band_stage = band_next;
        assign band_valid = band_valid_next;
        assign selected_band = band_stage;
        assign selected_digit = digit_next;
        assign present = present_next;
        assign valid = band_valid;
    end endgenerate
    triad_split6 split(present, first_half);
    genvar b, f, d;
    generate for (b = 0; b < 6; b = b + 1) begin : bands
        assign band_configs[b] = state_in[1215+54*b +: 54];
        assign totals[b] = count54(band_configs[b]);
        for (f = 0; f < 6; f = f + 1) begin : configs
            for (d = 0; d < 9; d = d + 1) begin : digits
                localparam integer BIT = 1215+54*b+9*f+d;
                wire selected = valid && selected_band[b] && selected_digit[d];
                assign child_a[BIT] = state_in[BIT] && (!selected || first_half[f]);
                assign child_b[BIT] = state_in[BIT] && (!selected || !first_half[f]);
            end
        end
    end endgenerate
    assign child_a[1214:0] = state_in[1214:0];
    assign child_b[1214:0] = state_in[1214:0];
    always @* begin
        band_valid_next = 0;
        configs_next = 0; band_next = 0;
        best_total = 55; best_band = 0;
        total = 0;
        for (band = 0; band < 6; band = band + 1) begin
            total = {26'b0, totals[band]};
            if (total >= 10 && total < best_total) begin
                best_total = total;
                best_band = band;
                band_valid_next = 1;
            end
        end
        band_next[best_band] = band_valid_next;
        configs_next = band_configs[best_band];
    end
    always @* begin
        digit_next = 0; present_next = 0;
        count = 0; rank = 0; best_rank = 7; best_digit = 0; selected_count = 0;
        for (digit = 0; digit < 9; digit = digit + 1) begin
            count = 0;
            for (cfg = 0; cfg < 6; cfg = cfg + 1)
                count = count + {31'b0, selected_configs[9*cfg+digit]};
            rank = (count >= 4) ? 4 : count;
            if (count >= 2 && rank < best_rank) begin
                best_rank = rank;
                best_digit = digit;
                selected_count = count;
            end
        end
        digit_next[best_digit] = band_valid;
        for (cfg = 0; cfg < 6; cfg = cfg + 1) begin
            present_next[cfg] = |(selected_configs[9*cfg +: 9] & digit_next);
        end
    end
    wire unused_selection = ^{best_band, best_digit, selected_count};
endmodule

module triad_split6 (
    input wire [5:0] present,
    output reg [5:0] first_half
);
    integer count, rank, i;
    always @* begin
        count = 0; rank = 0; first_half = 0;
        for (i = 0; i < 6; i = i + 1) count = count + {31'b0, present[i]};
        for (i = 0; i < 6; i = i + 1) begin
            if (present[i]) begin
                first_half[i] = rank < count/2;
                rank = rank + 1;
            end
        end
    end
endmodule
