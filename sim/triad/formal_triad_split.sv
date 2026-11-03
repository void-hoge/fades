module formal_triad_split (
    input wire [5:0] present,
    output wire mismatch
);
    wire [5:0] a, b;
    triad_split6 dut(present, a);
    assign b = present & ~a;
    integer i, count, ac, bc;
    always @* begin
        count = 0; ac = 0; bc = 0;
        for (i = 0; i < 6; i = i + 1) begin
            count = count + {31'b0, present[i]};
            ac = ac + {31'b0, a[i]};
            bc = bc + {31'b0, b[i]};
        end
    end
    assign mismatch = ((a | b) != present) || (|(a & b)) ||
        (ac != count/2) || (bc != count-count/2) ||
        ((count >= 2) && ((ac == 0) || (bc == 0) || (ac >= count) || (bc >= count)));
endmodule
