`timescale 1ns/1ps
// One problem at a time; deque = pending DFS siblings followed by new problems.
// State memory is a true dual-port synchronous RAM: port A is used for DFS
// writes / head reads, port B for ingress. The head register mirrors RAM.
module fades_core #(parameter integer LOCKED_CANDIDATES = 1) (
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
    localparam integer ADDR_WIDTH = 10;
    localparam integer BRANCH_RESERVE = 324;
    localparam integer DEPTH = 2 ** ADDR_WIDTH;
    localparam [1:0] IDLE = 0, RUN = 1, BRANCH = 2, POP = 3;
    logic [1:0] state;
    logic [728:0] current;
    wire [728:0] next_candidates, child_a, child_b;
    wire next_forced_conflict, state_solved, state_contradiction;
    wire [8:0] next_row_changed;
    logic [8:0] last_row_changed;
    logic last_forced_conflict;
    wire last_changed = |last_row_changed;
    wire contradiction = state_contradiction || last_forced_conflict;
    wire solved = state_solved && !last_forced_conflict;
    // Both combinational engines read current. No inference -> validation
    // path controls current's write enable or next-state mux in this cycle.
    deduction_engine #(.LOCKED_CANDIDATES(LOCKED_CANDIDATES)) infer(current, next_candidates, state_solved, state_contradiction,
        next_forced_conflict, next_row_changed);
    branch_generator branching(current, child_a, child_b);

    (* ram_style = "block" *) logic [728:0] memory [0:DEPTH-1];
    logic [ADDR_WIDTH-1:0] head, tail;
    logic [ADDR_WIDTH:0] count, pending;
    logic [728:0] head_cache, read_data;
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
    // Each balanced CELL split reduces sum(ceil(log2(cell candidate count)))
    // by >=1. Initially this is <=81*4=324, bounding pending DFS siblings.
    // This reservation relies on branch's balanced cell splitting.
    // One physical slot stays unused.
    localparam [ADDR_WIDTH:0] DEPTH_VALUE = {1'b1, {ADDR_WIDTH{1'b0}}};
    localparam [ADDR_WIDTH:0] INPUT_LIMIT = DEPTH_VALUE - BRANCH_RESERVE[ADDR_WIDTH:0] - 1'b1;
    assign input_ready = !reset && ((count - pending) < INPUT_LIMIT);
    wire enqueue = input_valid && input_ready;
    wire read_enable = !reset && count != 0 && !head_valid && !read_pending && !push;
    wire [ADDR_WIDTH-1:0] push_address = head - 1'b1;
    wire [ADDR_WIDTH-1:0] port_a_address = push ? push_address : head;

    // No reset on memory or its data output: preserve BRAM inference.
    always @(posedge clk) begin
        if (!reset && (push || read_enable)) begin
            if (push) memory[port_a_address] <= child_b;
            read_data <= memory[port_a_address];
        end
    end
    always @(posedge clk) begin
        if (enqueue) memory[tail] <= input_candidates;
    end

    assign queued = count;
    assign pending_branches = pending;
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
            last_row_changed <= 9'h1ff;
            last_forced_conflict <= 0;
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
                    head_cache <= input_candidates;
                    head_valid <= 1;
                end
            end
            if (take) begin
                current <= head_cache;
                last_row_changed <= 9'h1ff;
                last_forced_conflict <= 0;
                head <= head + 1'b1;
                head_valid <= 0;
                state <= RUN;
            end
            if (pop_branch) pending <= pending - 1'b1;

            if (execute) begin
                if (terminal) begin
                    result_valid <= 1;
                    result_solved <= solved && !contradiction;
                    result_candidates <= current;
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
                    last_row_changed <= next_row_changed;
                    last_forced_conflict <= next_forced_conflict;
                end
            end
            if (push) begin
                current <= child_a;
                last_row_changed <= 9'h1ff;
                last_forced_conflict <= 0;
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
        if (ADDR_WIDTH < 10 || BRANCH_RESERVE < 324 || BRANCH_RESERVE >= DEPTH-1)
            $fatal(1, "Deque parameters cannot reserve the worst-case search path");
    end
    always @(posedge clk) begin
        if (!reset) begin
            if (count >= DEPTH_VALUE || pending > count || pending > BRANCH_RESERVE[ADDR_WIDTH:0])
                $fatal(1, "Deque occupancy invariant violated");
            if (head_valid && count == 0)
                $fatal(1, "Valid head of an empty deque");
            if (push && enqueue && push_address == tail)
                $fatal(1, "Simultaneous RAM write collision");
            if (read_enable && enqueue && head == tail)
                $fatal(1, "RAM read/write collision");
        end
    end
`endif
endmodule
