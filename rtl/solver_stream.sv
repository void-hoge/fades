`timescale 1ns/1ps
// Little-endian 64-byte records; see docs/protocol.md.
module solver_stream #(
    parameter integer CORES = 6,
    parameter integer INFLIGHT_LIMIT = 2,
    parameter integer FIFO_DEPTH = 64
) (
    input wire dma_clk, core_clk,
    /* verilator lint_off SYNCASYNCNET */
    input wire reset,
    /* verilator lint_on SYNCASYNCNET */
    input wire enable,
    // Held constant throughout a run, programmed while reset is asserted.
    input wire [31:0] total_records, packet_records,
    input wire [127:0] s_axis_tdata,
    input wire [15:0] s_axis_tkeep,
    input wire s_axis_tvalid, s_axis_tlast,
    output wire s_axis_tready,
    output wire [127:0] m_axis_tdata,
    output wire [15:0] m_axis_tkeep,
    output wire m_axis_tvalid, m_axis_tlast,
    input wire m_axis_tready,
    output logic framing_error,
    output logic [63:0] accepted_count, output_count,
    output logic [63:0] dispatch_count, collected_count,
    output logic [63:0] input_stall_cycles, queue_empty_cycles,
    output logic [63:0] queue_peak, processing_cycles,
    output logic [CORES*64-1:0] core_dispatch_counts,
    output logic [CORES*64-1:0] core_result_counts,
    output logic [CORES*64-1:0] core_busy_cycles,
    output logic [CORES*64-1:0] core_output_wait_cycles,
    output logic [CORES*64-1:0] core_input_wait_cycles, core_max_inflight
);
    localparam integer CW = $clog2(CORES);
    localparam integer IW = $clog2(INFLIGHT_LIMIT+1);
    localparam integer IP = (INFLIGHT_LIMIT > 1) ? $clog2(INFLIGHT_LIMIT) : 1;
    (* ASYNC_REG = "TRUE" *) logic [2:0] dma_reset_sync = 3'b111;
    (* ASYNC_REG = "TRUE" *) logic [2:0] core_reset_sync = 3'b111;
    always @(posedge dma_clk or posedge reset) begin
        if (reset) dma_reset_sync <= 7;
        else dma_reset_sync <= {dma_reset_sync[1:0],1'b0};
    end
    always @(posedge core_clk or posedge reset) begin
        if (reset) core_reset_sync <= 7;
        else core_reset_sync <= {core_reset_sync[1:0],1'b0};
    end
    wire dma_reset = dma_reset_sync[2];
    wire core_reset = core_reset_sync[2];
    // Configuration is held throughout reset and the complete run. Synchronize
    // it before use in the core domain; reset release provides the settling time.
    (* ASYNC_REG = "TRUE" *) logic [31:0] total_meta, total_core;
    always @(posedge core_clk) begin total_meta <= total_records; total_core <= total_meta; end
    logic [383:0] assembly;
    logic [1:0] input_beat;
    wire [387:0] queue_data;
    wire queue_valid, queue_ready, queue_write_ready;
    wire [$clog2(FIFO_DEPTH):0] queue_write_count;
    // Only the low four bits of beat 3 contain cells; drop reserved bits.
    wire [387:0] assembled = {s_axis_tdata[3:0], assembly};
    wire bad_beat = s_axis_tkeep != 16'hffff ||
        (s_axis_tlast && input_beat != 3);
    assign s_axis_tready = !dma_reset && !framing_error &&
        (input_beat != 3 || queue_write_ready);
    wire input_fire = s_axis_tvalid && s_axis_tready;
    async_fifo #(.WIDTH(388), .DEPTH(FIFO_DEPTH)) input_queue (
        .wr_clk(dma_clk), .rd_clk(core_clk), .reset(reset),
        .input_data(assembled),
        .input_valid(input_fire && input_beat == 3 && !bad_beat),
        .input_ready(queue_write_ready), .write_count(queue_write_count), .output_data(queue_data),
        .output_valid(queue_valid), .output_ready(queue_ready)
    );
    always @(posedge dma_clk) begin
        if (dma_reset) begin
            assembly <= 0; input_beat <= 0; framing_error <= 0;
            accepted_count <= 0; input_stall_cycles <= 0;
            queue_peak <= 0;
        end else begin
            if (64'(queue_write_count) > queue_peak) queue_peak <= 64'(queue_write_count);
            if (s_axis_tvalid && !s_axis_tready)
                input_stall_cycles <= input_stall_cycles + 1'b1;
            if (input_fire) begin
                if (bad_beat) framing_error <= 1;
                if (input_beat != 3)
                    assembly[128*input_beat +: 128] <= s_axis_tdata;
                input_beat <= input_beat + 1'b1;
                if (input_beat == 3 && !bad_beat)
                    accepted_count <= accepted_count + 1'b1;
            end
        end
    end

    logic [728:0] candidates;
    logic invalid_cells;
    always_comb begin
        candidates = 0;
        invalid_cells = 0;
        for (integer c = 0; c < 81; c = c+1) begin
            if (queue_data[64+4*c +: 4] == 0)
                candidates[9*c +: 9] = 9'h1ff;
            else if (queue_data[64+4*c +: 4] <= 9)
                candidates[9*c +: 9] = 9'b1 << (queue_data[64+4*c +: 4] - 1'b1);
            else invalid_cells = 1;
        end
        // Invalid input takes the normal ordered result path, without search.
        if (invalid_cells) candidates = 0;
    end
    wire [CORES-1:0] core_ready, result_valid, result_solved, core_busy, output_wait;
    wire [728:0] result_candidates [0:CORES-1];
    logic [CORES-1:0] core_valid, result_ready;
    logic [IW-1:0] inflight [0:CORES-1];
    logic [IP-1:0] id_head [0:CORES-1], id_tail [0:CORES-1];
    logic [64:0] ids [0:CORES-1][0:INFLIGHT_LIMIT-1];
    logic [CW-1:0] rr_head, result_head;
    integer selected, winner;
    logic [511:0] result_buffer;
    logic result_buffer_valid;
    wire result_buffer_ready;
    wire collect_ready = !result_buffer_valid || result_buffer_ready;
    logic [CORES-1:0] eligible, available, dispatch_mask, collect_mask;
    always_comb begin
        for(integer n=0;n<CORES;n=n+1) begin
            eligible[n] = core_ready[n] && int'(inflight[n]) < INFLIGHT_LIMIT;
            available[n] = result_valid[n] && inflight[n] != 0;
        end
        dispatch_mask = eligible & ({CORES{1'b1}} << rr_head);
        if (!(|dispatch_mask)) dispatch_mask = eligible;
        collect_mask = available & ({CORES{1'b1}} << result_head);
        if (!(|collect_mask)) collect_mask = available;
        selected = -1;
        for (integer n = CORES-1; n >= 0; n = n-1)
            if (dispatch_mask[n]) selected = n;
        core_valid = 0;
        if (!core_reset && queue_valid && selected != -1) core_valid[selected] = 1;
        winner = -1;
        for (integer n = CORES-1; n >= 0; n = n-1)
            if (collect_mask[n]) winner = n;
        result_ready = 0;
        if (!core_reset && collect_ready && winner != -1) result_ready[winner] = 1;
    end
    assign queue_ready = !core_reset && selected != -1;
    wire dispatch = queue_valid && queue_ready;
    wire collect = !core_reset && winner != -1 && collect_ready;
    logic [511:0] encoded_result;
    always_comb begin
        encoded_result = 0;
        if (winner != -1) begin
            encoded_result[63:0] = ids[winner][id_head[winner]][63:0];
            encoded_result[71:64] = ids[winner][id_head[winner]][64] ? 8'd2 :
                (result_solved[winner] ? 8'd0 : 8'd1);
            if (result_solved[winner] && !ids[winner][id_head[winner]][64]) begin
                for (integer c = 0; c < 81; c = c+1)
                    for (integer d = 1; d <= 9; d = d+1)
                        encoded_result[72+4*c +: 4] |=
                            {4{result_candidates[winner][9*c+d-1]}} & 4'(d);
            end
        end
    end
    for (genvar g = 0; g < CORES; g = g+1) begin : solvers
        /* verilator lint_off PINCONNECTEMPTY */
        core solver (
            .clk(core_clk), .reset(core_reset), .enable(enable),
            .input_valid(core_valid[g]), .input_ready(core_ready[g]),
            .input_candidates(candidates), .result_valid(result_valid[g]),
            .result_ready(result_ready[g]), .result_solved(result_solved[g]),
            .result_candidates(result_candidates[g]), .queued(), .pending_branches(),
            .busy(core_busy[g]), .event_infer(), .event_branch(), .event_load(),
            .event_memory_wait(), .event_output_wait(output_wait[g]),
            .event_contradiction(), .event_finish(), .event_skip(), .event_wasted_infer()
        );
        /* verilator lint_on PINCONNECTEMPTY */
        wire push = dispatch && selected == g;
        wire pop = collect && winner == g;
        always @(posedge core_clk) begin
            if (core_reset) begin
                inflight[g] <= 0; id_head[g] <= 0; id_tail[g] <= 0;
                core_dispatch_counts[64*g +: 64] <= 0;
                core_result_counts[64*g +: 64] <= 0;
                core_busy_cycles[64*g +: 64] <= 0;
                core_output_wait_cycles[64*g +: 64] <= 0;
                core_input_wait_cycles[64*g +: 64] <= 0;
                core_max_inflight[64*g +: 64] <= 0;
            end else begin
                inflight[g] <= inflight[g] + IW'(push) - IW'(pop);
                if (push) begin
                    ids[g][id_tail[g]] <= {invalid_cells, queue_data[63:0]};
                    id_tail[g] <= int'(id_tail[g]) == INFLIGHT_LIMIT-1 ? 0 : id_tail[g]+1'b1;
                    core_dispatch_counts[64*g +: 64] <= core_dispatch_counts[64*g +: 64]+1'b1;
                end
                if (pop) begin
                    id_head[g] <= int'(id_head[g]) == INFLIGHT_LIMIT-1 ? 0 : id_head[g]+1'b1;
                    core_result_counts[64*g +: 64] <= core_result_counts[64*g +: 64]+1'b1;
                end
                if (core_busy[g]) core_busy_cycles[64*g +: 64] <= core_busy_cycles[64*g +: 64]+1'b1;
                if (output_wait[g]) core_output_wait_cycles[64*g +: 64] <= core_output_wait_cycles[64*g +: 64]+1'b1;
                if (enable && !core_busy[g] && inflight[g] == 0)
                    core_input_wait_cycles[64*g +: 64] <= core_input_wait_cycles[64*g +: 64]+1'b1;
                if (64'(inflight[g]) > core_max_inflight[64*g +: 64])
                    core_max_inflight[64*g +: 64] <= 64'(inflight[g]);
            end
        end
    end
    logic work_started, work_done;
    always @(posedge core_clk) begin
        if (core_reset) begin
            rr_head <= 0; result_head <= 0;
            result_buffer <= 0; result_buffer_valid <= 0;
            dispatch_count <= 0; collected_count <= 0; queue_empty_cycles <= 0;
            processing_cycles <= 0; work_started <= 0; work_done <= 0;
        end else begin
            if (!work_done && (work_started || dispatch)) begin
                work_started <= 1;
                processing_cycles <= processing_cycles+1'b1;
                if (collect && collected_count+1'b1 == {32'b0,total_core}) work_done <= 1;
            end
            if (!queue_valid) queue_empty_cycles <= queue_empty_cycles + 1'b1;
            if (dispatch) begin
                rr_head <= selected == CORES-1 ? 0 : CW'(selected+1);
                dispatch_count <= dispatch_count + 1'b1;
            end
            if (collect_ready) begin
                result_buffer_valid <= collect;
                if (collect) begin
                    result_buffer <= encoded_result;
                    result_head <= winner == CORES-1 ? 0 : CW'(winner+1);
                    collected_count <= collected_count + 1'b1;
                end
            end
        end
    end
    wire [511:0] output_record;
    wire output_record_valid, output_record_ready;
    async_fifo #(.WIDTH(512), .DEPTH(FIFO_DEPTH)) output_queue (
        .wr_clk(core_clk), .rd_clk(dma_clk), .reset(reset),
        .input_data(result_buffer), .input_valid(result_buffer_valid),
        .input_ready(result_buffer_ready),
        /* verilator lint_off PINCONNECTEMPTY */
        .write_count(),
        /* verilator lint_on PINCONNECTEMPTY */
        .output_data(output_record),
        .output_valid(output_record_valid), .output_ready(output_record_ready)
    );
    logic [1:0] output_beat;
    logic [31:0] packet_position;
    assign m_axis_tdata = output_record[128*output_beat +: 128];
    assign m_axis_tkeep = 16'hffff;
    assign m_axis_tvalid = !dma_reset && output_record_valid && output_count < {32'b0,total_records};
    assign m_axis_tlast = output_beat == 3 &&
        (packet_position+1'b1 == packet_records || output_count+1'b1 == {32'b0,total_records});
    assign output_record_ready = m_axis_tvalid && m_axis_tready && output_beat == 3;
    always @(posedge dma_clk) begin
        if (dma_reset) begin
            output_beat <= 0; packet_position <= 0; output_count <= 0;
        end else if (m_axis_tvalid && m_axis_tready) begin
            output_beat <= output_beat+1'b1;
            if (output_beat == 3) begin
                output_count <= output_count+1'b1;
                packet_position <= m_axis_tlast ? 0 : packet_position+1'b1;
            end
        end
    end
`ifndef SYNTHESIS
    initial begin
        if (CORES < 2 || INFLIGHT_LIMIT < 2 || INFLIGHT_LIMIT > 4)
            $fatal(1, "Unsupported core count or inflight limit");
    end
    for (genvar a = 0; a < CORES; a = a+1) begin : checks
        always @(posedge core_clk) if (!core_reset) begin
            if (int'(inflight[a]) > INFLIGHT_LIMIT) $fatal(1, "Inflight limit exceeded");
            if (result_valid[a] && inflight[a] == 0) $fatal(1, "Result without ID");
        end
    end
`endif
endmodule
