`timescale 1ns/1ps
module tb_inference;
    logic [728:0] candidates_in = {729{1'b1}};
    logic [728:0] expected;
    wire [728:0] candidates_out;
    wire changed, solved, contradiction, forced_conflict;
    wire [8:0] row_changed;
    assign changed = |row_changed;
    integer r, c;

    inference dut (
        .candidates_in(candidates_in), .candidates_out(candidates_out),
        .row_changed(row_changed), .solved(solved), .contradiction(contradiction),
        .forced_conflict(forced_conflict)
    );

    task check_elimination;
        begin
            #1;
            // Check the whole candidate state: both required removals and
            // preservation of every unrelated candidate.
            if (candidates_out !== expected ||
                !changed || solved || contradiction)
                $fatal(1, "Locked candidate elimination failed");
            candidates_in = candidates_out;
            #1;
            if (changed || candidates_out !== expected || contradiction)
                $fatal(1, "Locked candidate fixed point failed");
        end
    endtask

    initial begin
        #1;
        // Pointing, row: block 1's digit 1 is confined to row 1.
        for (r = 1; r < 3; r = r + 1)
            for (c = 0; c < 3; c = c + 1)
                candidates_in[9*(r*9+c)] = 0;
        expected = candidates_in;
        for (c = 3; c < 9; c = c + 1) expected[9*c] = 0;
        check_elimination;
        $display("PASS: pointing row, candidate-only progress and fixed point");

        // Pointing, column: block 9's digit 9 is confined to column 9.
        candidates_in = {729{1'b1}};
        for (r = 6; r < 9; r = r + 1)
            for (c = 6; c < 8; c = c + 1)
                candidates_in[9*(r*9+c)+8] = 0;
        expected = candidates_in;
        for (r = 0; r < 6; r = r + 1) expected[9*(r*9+8)+8] = 0;
        check_elimination;
        $display("PASS: pointing column");

        // Claiming, row: row 4's digit 2 is confined to block 5.
        candidates_in = {729{1'b1}};
        for (c = 0; c < 9; c = c + 1)
            if (c < 3 || c > 5) candidates_in[9*(3*9+c)+1] = 0;
        expected = candidates_in;
        for (r = 4; r < 6; r = r + 1)
            for (c = 3; c < 6; c = c + 1)
                expected[9*(r*9+c)+1] = 0;
        check_elimination;
        $display("PASS: claiming row");

        // Claiming, column: column 2's digit 5 is confined to block 7.
        candidates_in = {729{1'b1}};
        for (r = 0; r < 6; r = r + 1) candidates_in[9*(r*9+1)+4] = 0;
        expected = candidates_in;
        for (r = 6; r < 9; r = r + 1)
            for (c = 0; c < 3; c = c + 1)
                if (c != 1) expected[9*(r*9+c)+4] = 0;
        check_elimination;
        $display("PASS: claiming column");

        // A locked elimination creates a singleton. Its effect on peers
        // must not propagate until the next inference step.
        candidates_in = {729{1'b1}};
        for (r = 1; r < 3; r = r + 1)
            for (c = 0; c < 3; c = c + 1)
                candidates_in[9*(r*9+c)] = 0;
        candidates_in[9*3 +: 9] = 9'b000000011;
        #1;
        if (candidates_out[9*3 +: 9] !== 9'b10 ||
            candidates_out[9*12+1] !== 1'b1 ||
            contradiction || !changed)
            $fatal(1, "Locked elimination cascaded within a step");
        candidates_in = candidates_out;
        #1;
        if (candidates_out[9*3 +: 9] !== 9'b10 ||
            candidates_out[9*12+1] !== 1'b0 || contradiction)
            $fatal(1, "Singleton was not propagated on the next step");
        $display("PASS: locked elimination feeds singleton propagation on the next step");

        // Hidden single: digit 9 has only one possible position in row 1.
        candidates_in = {729{1'b1}};
        for (c = 1; c < 9; c = c + 1) candidates_in[9*c+8] = 0;
        #1;
        if (candidates_out[8:0] !== 9'b100000000 ||
            candidates_out[9*27+8] !== 1'b1 || contradiction)
            $fatal(1, "Hidden single or step boundary failed");
        candidates_in = candidates_out;
        #1;
        if (candidates_out[9*27+8] !== 1'b0 || contradiction)
            $fatal(1, "Hidden singleton was not propagated");
        $display("PASS: hidden single and next-step propagation");
        // Contradictions produced by simultaneous hidden-single forces are
        // separate metadata, not necessarily visible in the candidate matrix.
        candidates_in = {729{1'b1}};
        #1;
        if (changed || solved || contradiction || forced_conflict)
            $fatal(1, "Empty board must be a valid fixed point");
        for (c = 1; c < 9; c = c + 1) begin
            candidates_in[9*c +: 9] = 9'h1fe;
            candidates_in[81*c +: 9] = 9'h1fd;
        end
        #1;
        if (contradiction || !forced_conflict || candidates_out[8:0] != 9'b11)
            $fatal(1, "Conflicting forces require deferred metadata");
        candidates_in = {729{1'b1}};
        candidates_in[8:0] = 0;
        #1;
        if (!contradiction || solved) $fatal(1, "Zero cell not detected");
        candidates_in = {729{1'b1}};
        candidates_in[8:0] = 1;
        candidates_in[17:9] = 1;
        #1;
        if (!contradiction || solved) $fatal(1, "Duplicate not detected");

        solve_known(
            {"530070000", "600195000", "098000060", "800060003", "400803001",
             "700020006", "060000280", "000419005", "000080079"},
            {"534678912", "672195348", "198342567", "859761423", "426853791",
             "713924856", "961537284", "287419635", "345286179"}, 5);
        solve_known(
            {"030000070", "000000000", "001080000", "000010900", "070000020",
             "004000800", "009000401", "000300000", "060702000"},
            {"538129674", "697435218", "241687395", "352816947", "876954123",
             "914273856", "729568431", "485391762", "163742589"}, 17);
        $display("All inference tests passed.");
        $finish;
    end

    function automatic [728:0] parse(input [647:0] text);
        logic [7:0] ch;
        for (int n = 0; n < 81; n++) begin
            ch = text[8*(80-n) +: 8];
            parse[9*n +: 9] = ch == "0" ? 9'h1ff : 9'b1 << (ch - "1");
        end
    endfunction

    task automatic solve_known(input [647:0] puzzle, answer, input integer steps);
        logic [728:0] reference_state, previous;
        integer count;
        begin
            candidates_in = parse(puzzle);
            reference_state = parse(answer);
            count = 0;
            #1;
            while (!solved && count < 730) begin
                if (contradiction || forced_conflict || !changed)
                    $fatal(1, "Known puzzle halted before its solution");
                previous = candidates_in;
                if ((candidates_out & reference_state) !== reference_state ||
                    (candidates_out & ~previous) != 0)
                    $fatal(1, "Candidate preservation failed");
                for (int i = 0; i < 81; i++)
                    if ((previous[9*i +: 9] & (previous[9*i +: 9]-9'd1)) == 0 &&
                        candidates_out[9*i +: 9] !== previous[9*i +: 9])
                        $fatal(1, "Singleton changed");
                candidates_in = candidates_out;
                count++;
                #1;
            end
            if (!solved || contradiction || candidates_in !== reference_state || count != steps)
                $fatal(1, "Known puzzle solution or inference count changed");
            $display("PASS: known puzzle, %0d inference steps", count);
        end
    endtask
endmodule
