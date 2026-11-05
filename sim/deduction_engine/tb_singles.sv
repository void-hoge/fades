`timescale 1ns/1ps
module tb_singles;
    logic [728:0] candidates = {729{1'b1}};
    wire [728:0] successor;
    wire solved, contradiction, conflict;
    wire [8:0] changed;
    deduction_engine #(.LOCKED_CANDIDATES(0)) dut(
        candidates, successor, solved, contradiction, conflict, changed);
    integer r, c;
    initial begin
        // A pointing pattern alone must be a fixed point without locked rules.
        for (r = 1; r < 3; r = r + 1)
            for (c = 0; c < 3; c = c + 1)
                candidates[9*(r*9+c)] = 0;
        #1;
        if (successor !== candidates || changed || solved || contradiction || conflict)
            $fatal(1, "Singles mode applied pointing");
        // A claiming pattern must also be preserved.
        candidates = {729{1'b1}};
        for (c = 3; c < 9; c = c + 1) candidates[9*c] = 0;
        #1;
        if (successor !== candidates || changed || contradiction || conflict)
            $fatal(1, "Singles mode applied claiming");
        // Hidden Single and the following Naked Single peer elimination remain.
        candidates = {729{1'b1}};
        for (c = 1; c < 9; c = c + 1) candidates[9*c+8] = 0;
        #1;
        if (successor[8:0] !== 9'h100 || successor[9*27+8] !== 1'b1 || contradiction)
            $fatal(1, "Hidden Single failed or cascaded");
        candidates = successor;
        #1;
        if (successor[9*27+8] !== 1'b0 || contradiction)
            $fatal(1, "Naked Single peer elimination failed");
        candidates = {729{1'b1}};
        candidates[8:0] = 0;
        #1;
        if (!contradiction || solved) $fatal(1, "Empty domain not detected");
        candidates = {729{1'b1}};
        candidates[8:0] = 1;
        candidates[17:9] = 1;
        #1;
        if (!contradiction || solved) $fatal(1, "Duplicate not detected");
        $display("PASS: singles-only rule selection, step boundary and contradictions");
        $finish;
    end
endmodule
