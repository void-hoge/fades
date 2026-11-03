`timescale 1ns/1ps
// One problem at a time; deque = pending DFS siblings followed by new problems.
// State memory is a 512-deep simple dual-port RAM: one arbitrated write port
// for DFS / ingress and one synchronous read port. The head register mirrors RAM.
module fades_core (
    input wire clk,
    input wire reset,
    input wire enable,
    input wire input_valid,
    output wire input_ready,
    input wire [728:0] input_candidates,
    output logic result_valid,
    input wire result_ready,
    output logic result_solved,
    output logic [728:0] result_candidates,
    output wire [10:0] queued,
    output wire [10:0] pending_branches,
    output wire busy,
    // Exclusive cycle classes, sampled BEFORE each rising edge.
    output wire event_infer,
    output wire event_branch,
    output wire event_load,
    output wire event_memory_wait,
    output wire event_output_wait,
    output wire event_contradiction,
    output wire event_finish,
    output wire event_skip,
    // Subset of event_infer: successor calculation is discarded on a halt.
    output wire event_wasted_infer
);
    localparam integer ADDR_WIDTH = 9;
    localparam integer BRANCH_RESERVE = 162;
    localparam integer DEPTH = 2 ** ADDR_WIDTH;
    localparam [1:0] IDLE = 0, RUN = 1, BRANCH = 2, POP = 3;
    logic [1:0] state;
    logic [1538:0] current;
    wire [1538:0] next_candidates, child_a, child_b;
    wire state_solved, contradiction, branch_valid;
    logic band_phase, round_changed, last_changed;
    wire phase_changed = current != next_candidates;
    wire solved = state_solved && !last_changed && !contradiction;
    triad_engine infer(current, band_phase, next_candidates, state_solved, contradiction);
    triad_branch_generator #(.PIPELINED(1)) branching(clk, current, child_a, child_b, branch_valid);

    (* ram_style = "block" *) logic [1538:0] memory [0:DEPTH-1];
    logic [ADDR_WIDTH-1:0] head, tail;
    logic [ADDR_WIDTH:0] count, pending;
    logic [1538:0] head_cache, read_data;
    logic head_valid, read_pending;
    logic [ADDR_WIDTH-1:0] read_tag;
    wire slot_available = !result_valid || result_ready;
    wire terminal = solved || (contradiction && pending == 0);
    wire execute = enable && state == RUN && (!terminal || slot_available);
    wire finish = execute && terminal;
    wire push = enable && state == BRANCH;
    wire pop_branch = enable && head_valid &&
        ((state == RUN && contradiction && pending != 0) || state == POP);
    wire pop_new = enable && head_valid &&
        (state == IDLE || (finish && pending == 0));
    wire take = pop_branch || pop_new;
    wire [ADDR_WIDTH:0] discard = finish ? pending : {(ADDR_WIDTH+1){1'b0}};
    // Balanced CONFIGURATION splits reduce sum(ceil(log2(config count))).
    // There are 6 bands * 9 digits, each with <=6 configurations: <=54*3=162.
    // One physical slot stays unused.
    localparam [ADDR_WIDTH:0] DEPTH_VALUE = {1'b1, {ADDR_WIDTH{1'b0}}};
    localparam [ADDR_WIDTH:0] INPUT_LIMIT = DEPTH_VALUE - BRANCH_RESERVE[ADDR_WIDTH:0] - 1'b1;
    assign input_ready = !reset && !push && ((count - pending) < INPUT_LIMIT);
    wire enqueue = input_valid && input_ready;
    wire read_enable = !reset && count != 0 && !head_valid && !read_pending && !push;
    wire [ADDR_WIDTH-1:0] push_address = head - 1'b1;
    wire [ADDR_WIDTH-1:0] write_address = push ? push_address : tail;
    wire [1538:0] input_state = {{810{1'b1}}, input_candidates};
    wire [1538:0] write_data = push ? child_b : input_state;

    // No reset on memory or its data output: preserve BRAM inference.
    always @(posedge clk) begin
        if (!reset && (push || enqueue)) memory[write_address] <= write_data;
        if (read_enable) read_data <= memory[head];
    end

    assign queued = {1'b0, count};
    assign pending_branches = {1'b0, pending};
    assign busy = state != IDLE;
    assign event_infer = execute;
    assign event_branch = push;
    assign event_load = enable && (state == IDLE || state == POP) && head_valid;
    assign event_memory_wait = enable &&
        ((state == POP && !head_valid) || (state == IDLE && count != 0 && !head_valid));
    assign event_output_wait = enable && state == RUN && terminal && !slot_available;
    assign event_contradiction = execute && contradiction;
    assign event_finish = finish;
    assign event_skip = finish && pending != 0;
    assign event_wasted_infer = execute && (solved || contradiction || !last_changed);

    always_ff @(posedge clk) begin
        if (reset) begin
            state <= IDLE;
            current <= 0;
            band_phase <= 0;
            round_changed <= 0;
            last_changed <= 1;
            head <= 0;
            tail <= 0;
            count <= 0;
            pending <= 0;
            head_valid <= 0;
            head_cache <= 0;
            read_pending <= 0;
            read_tag <= 0;
            result_valid <= 0;
            result_solved <= 0;
            result_candidates <= 0;
        end else begin
            read_pending <= read_enable;
            if (read_enable) read_tag <= head;
            if (read_pending && read_tag == head && count != 0) begin
                head_cache <= read_data;
                head_valid <= 1;
            end
            if (result_valid && result_ready) result_valid <= 0;

            count <= count + {{ADDR_WIDTH{1'b0}}, enqueue}
                + {{ADDR_WIDTH{1'b0}}, push}
                - {{ADDR_WIDTH{1'b0}}, take} - discard;
            if (enqueue) begin
                tail <= tail + 1'b1;
                if (count == 0) begin
                    head_cache <= input_state;
                    head_valid <= 1;
                end
            end
            if (take) begin
                current <= head_cache;
                band_phase <= 0;
                round_changed <= 0;
                last_changed <= 1;
                head <= head + 1'b1;
                head_valid <= 0;
                state <= RUN;
            end
            if (pop_branch) pending <= pending - 1'b1;

            if (execute) begin
                if (terminal) begin
                    result_valid <= 1;
                    result_solved <= solved && !contradiction;
                    result_candidates <= current[728:0];
                    pending <= 0;
                    if (!pop_new) state <= IDLE;
                    if (pending != 0) begin
                        head <= head + pending[ADDR_WIDTH-1:0];
                        head_valid <= 0;
                    end
                end else if (contradiction) begin
                    if (!pop_branch) state <= POP;
                end else if (!last_changed) begin
                    state <= BRANCH;
                end else begin
                    current <= next_candidates;
                    band_phase <= !band_phase;
                    if (!band_phase) round_changed <= phase_changed;
                    else last_changed <= round_changed || phase_changed;
                end
            end
            if (push) begin
                current <= child_a;
                band_phase <= 0;
                round_changed <= 0;
                last_changed <= 1;
                head <= push_address;
                head_cache <= child_b;
                head_valid <= 1;
                pending <= pending + 1'b1;
                state <= RUN;
            end
        end
    end

`ifndef SYNTHESIS
    initial begin
        if (ADDR_WIDTH != 9 || BRANCH_RESERVE < 162 || BRANCH_RESERVE >= DEPTH-1)
            $fatal(1, "Deque parameters cannot reserve the worst-case search path");
    end
    always @(posedge clk) begin
        if (!reset) begin
            if (count >= DEPTH_VALUE || pending > count || pending > BRANCH_RESERVE[ADDR_WIDTH:0])
                $fatal(1, "Deque occupancy invariant violated");
            if (head_valid && count == 0)
                $fatal(1, "Valid head of an empty deque");
            if (push && (enqueue || !branch_valid))
                $fatal(1, "Invalid branch or simultaneous RAM writes");
            if (read_enable && enqueue && head == tail)
                $fatal(1, "RAM read/write collision");
        end
    end
`endif
endmodule
