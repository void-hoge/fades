`timescale 1ns/1ps
module tb_problem_preprocessor;
    reg clk = 0, reset = 1;
    always #5 clk = ~clk;
    reg [387:0] input_data = 0;
    reg input_valid = 0, output_ready = 0;
    wire input_ready, output_valid;
    wire [728:0] output_candidates;
    wire [64:0] output_tag;
    problem_preprocessor dut(.*);

    reg [387:0] records [0:255];
    reg [728:0] answers [0:255];
    reg [64:0] tags [0:255];
    reg [31:0] rng = 32'h7292048;
    function automatic [31:0] random_word(input [31:0] x);
        reg [31:0] y;
        begin
            y = x ^ (x << 13); y = y ^ (y >> 17);
            random_word = y ^ (y << 5);
        end
    endfunction

    // Independent pairwise reference: no unit masks or DUT helpers are used.
    function automatic [728:0] expected(input [387:0] record_data);
        integer c, p, digit;
        reg invalid;
        reg [8:0] mask;
        begin
            expected = 0;
            invalid = 0;
            for (c = 0; c < 81; c = c+1) begin
                digit = {28'b0, record_data[64+4*c +: 4]};
                if (digit > 9) invalid = 1;
                if (digit == 0) begin
                    mask = 9'h1ff;
                    for (p = 0; p < 81; p = p+1) begin
                        digit = {28'b0, record_data[64+4*p +: 4]};
                        if ((c/9 == p/9 || c%9 == p%9 ||
                            (c/27 == p/27 && (c%9)/3 == (p%9)/3)) &&
                            digit >= 1 && digit <= 9)
                            mask = mask & ~(9'b1 << (digit-1));
                    end
                end else mask = 9'b1 << (digit-1);
                expected[9*c +: 9] = mask;
            end
            if (invalid) expected = 0;
        end
    endfunction

    task automatic clear_pipeline;
        begin
            @(negedge clk); reset = 1; input_valid = 0; output_ready = 0;
            repeat (2) @(negedge clk);
            if (input_ready || output_valid) $fatal(1, "Reset did not invalidate pipeline");
            reset = 0;
        end
    endtask

    task automatic run_cases(input integer count, input bit stress);
        integer sent, got, cycles, input_stalls, output_stalls;
        reg tx, rx, held;
        reg [728:0] held_candidates;
        reg [64:0] held_tag;
        begin
            sent = 0; got = 0; cycles = 0;
            input_stalls = 0; output_stalls = 0; held = 0;
            while (got < count && cycles < 4000) begin
                @(negedge clk);
                rng = random_word(rng);
                if (!input_valid && sent < count && (!stress || rng[0])) begin
                    input_data = records[sent]; input_valid = 1;
                end
                output_ready = !stress || (cycles > 30 && rng[2:1] != 0);
                #1;
                if (held && (!output_valid || output_candidates !== held_candidates ||
                    output_tag !== held_tag)) $fatal(1, "Stalled output changed");
                if (!stress && got != 0 && !output_valid)
                    $fatal(1, "Pipeline cannot sustain one record per cycle");
                tx = input_valid && input_ready;
                rx = output_valid && output_ready;
                if (rx) begin
                    if (output_tag !== tags[got] || output_candidates !== answers[got])
                        $fatal(1, "Candidate/tag mismatch at record %0d", got);
                    got = got+1;
                end
                held = output_valid && !output_ready;
                held_candidates = output_candidates; held_tag = output_tag;
                if (input_valid && !input_ready) input_stalls = input_stalls+1;
                if (held) output_stalls = output_stalls+1;
                @(posedge clk); #1;
                if (tx) begin sent = sent+1; input_valid = 0; end
                cycles = cycles+1;
            end
            if (got != count || sent != count) $fatal(1, "Pipeline timeout/lost record");
            if (stress && (input_stalls == 0 || output_stalls == 0))
                $fatal(1, "Missing stall coverage");
            if (!stress && cycles != count+2) $fatal(1, "Incorrect two-stage latency");
            repeat (3) @(negedge clk);
            if (output_valid) $fatal(1, "Extra output record");
            $display("PASS shared preprocessor: records=%0d stress=%0d cycles=%0d",
                count, stress, cycles);
        end
    endtask

    integer i, c, digit;
    initial begin
        for (i = 0; i < 256; i = i+1) begin
            records[i] = 0;
            records[i][63:0] = 64'hbadc0ffe00000000 + 64'(i);
            for (c = 0; c < 81; c = c+1) begin
                rng = random_word(rng);
                digit = rng[3:0] < 4 ? (int'(rng[15:8])%9)+1 : 0;
                if (i%19 == 0) digit = (c/9*3+c/27+c%9)%9+1;
                if (i%23 == 0) digit = 0;
                records[i][64+4*c +: 4] = 4'(digit);
            end
            if (i%13 == 0) records[i][64+4*(i%81) +: 4] = 4'(10+i%6);
            if (i%17 == 0) begin
                records[i][67:64] = 1; records[i][71:68] = 1;
            end
            answers[i] = expected(records[i]);
            tags[i] = {1'b0, records[i][63:0]};
            for (c = 0; c < 81; c = c+1)
                if (records[i][64+4*c +: 4] > 9) tags[i][64] = 1;
        end
        clear_pipeline();
        run_cases(256, 0);
        clear_pipeline();
        run_cases(256, 1);

        // Both stages occupied when reset arrives: neither old record may leak.
        clear_pipeline();
        @(negedge clk); input_valid = 1; input_data = records[0];
        @(negedge clk); input_data = records[1];
        @(negedge clk); input_valid = 0;
        if (!output_valid || input_ready) $fatal(1, "Reset case did not fill both stages");
        clear_pipeline();
        run_cases(256, 0);
        $finish;
    end
    initial begin #100000; $fatal(1, "Test timeout"); end
endmodule
