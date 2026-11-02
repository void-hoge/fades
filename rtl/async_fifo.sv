`timescale 1ns/1ps
// FWFT dual-clock FIFO. Production uses the vendor CDC/reset implementation;
// the simulation model uses Gray pointers and two-stage synchronizers.
module async_fifo #(
    parameter integer WIDTH = 388,
    parameter integer DEPTH = 64
) (
    input wire wr_clk, rd_clk, reset,
    input wire [WIDTH-1:0] input_data,
    input wire input_valid,
    output wire input_ready,
    output wire [$clog2(DEPTH):0] write_count,
    output wire [WIDTH-1:0] output_data,
    output wire output_valid,
    input wire output_ready
);
`ifdef SYNTHESIS
    wire full, empty, wr_busy, rd_busy;
    (* ASYNC_REG = "TRUE" *) logic [2:0] reset_sync = 3'b111;
    (* ASYNC_REG = "TRUE" *) logic [2:0] read_reset_sync = 3'b111;
    // XPM requires rst synchronous to its write clock, for either FIFO direction.
    always @(posedge wr_clk or posedge reset) begin
        if (reset) reset_sync <= 3'b111;
        else reset_sync <= {reset_sync[1:0],1'b0};
    end
    always @(posedge rd_clk or posedge reset) begin
        if (reset) read_reset_sync <= 3'b111;
        else read_reset_sync <= {read_reset_sync[1:0],1'b0};
    end
    xpm_fifo_async #(
        .FIFO_MEMORY_TYPE("distributed"), .FIFO_WRITE_DEPTH(DEPTH),
        .WRITE_DATA_WIDTH(WIDTH), .READ_DATA_WIDTH(WIDTH),
        .READ_MODE("fwft"), .FIFO_READ_LATENCY(0), .CDC_SYNC_STAGES(2),
        .DOUT_RESET_VALUE("0"), .ECC_MODE("no_ecc"),
        .FULL_RESET_VALUE(0), .PROG_EMPTY_THRESH(10), .PROG_FULL_THRESH(10),
        .RD_DATA_COUNT_WIDTH($clog2(DEPTH)+1),
        .WR_DATA_COUNT_WIDTH($clog2(DEPTH)+1), .RELATED_CLOCKS(0),
        .SIM_ASSERT_CHK(1), .USE_ADV_FEATURES("0004"), .WAKEUP_TIME(0)
    ) fifo (
        .rst(reset_sync[2]), .wr_clk(wr_clk), .rd_clk(rd_clk),
        .din(input_data), .wr_en(input_valid && input_ready),
        .full(full), .wr_rst_busy(wr_busy),
        .dout(output_data), .rd_en(output_valid && output_ready),
        .empty(empty), .rd_rst_busy(rd_busy),
        .sleep(1'b0), .injectsbiterr(1'b0), .injectdbiterr(1'b0),
        .almost_empty(), .almost_full(), .data_valid(), .dbiterr(),
        .overflow(), .prog_empty(), .prog_full(), .rd_data_count(),
        .sbiterr(), .underflow(), .wr_ack(), .wr_data_count(write_count)
    );
    assign input_ready = !reset_sync[2] && !wr_busy && !full;
    assign output_valid = !read_reset_sync[2] && !rd_busy && !empty;
`else
    localparam integer A = $clog2(DEPTH);
    logic [WIDTH-1:0] memory [0:DEPTH-1];
    logic [A:0] wp, rp, wg, rg;
    (* ASYNC_REG = "TRUE" *) logic [A:0] rg1, rg2, wg1, wg2;
    logic [2:0] wr_init, rd_init;
    wire [A:0] wn = wp + {{A{1'b0}}, (input_valid && input_ready)};
    wire [A:0] rn = rp + {{A{1'b0}}, (output_valid && output_ready)};
    wire [A:0] full_gray = {~rg2[A:A-1], rg2[A-2:0]};
    function automatic [A:0] gray_to_bin(input [A:0] gray);
        gray_to_bin[A] = gray[A];
        for(integer b=A-1;b>=0;b=b-1) gray_to_bin[b]=gray_to_bin[b+1]^gray[b];
    endfunction
    assign write_count = wp - gray_to_bin(rg2);
    assign input_ready = (&wr_init) && wg != full_gray;
    assign output_valid = (&rd_init) && rg != wg2;
    assign output_data = memory[rp[A-1:0]];
    always @(posedge wr_clk or posedge reset) begin
        if (reset) begin
            wp <= 0; wg <= 0; rg1 <= 0; rg2 <= 0; wr_init <= 0;
        end else begin
            wr_init <= {wr_init[1:0], 1'b1};
            rg1 <= rg; rg2 <= rg1;
            wp <= wn; wg <= (wn >> 1) ^ wn;
        end
    end
    always @(posedge wr_clk) begin
        if (input_valid && input_ready) memory[wp[A-1:0]] <= input_data;
    end
    always @(posedge rd_clk or posedge reset) begin
        if (reset) begin
            rp <= 0; rg <= 0; wg1 <= 0; wg2 <= 0; rd_init <= 0;
        end else begin
            rd_init <= {rd_init[1:0], 1'b1};
            wg1 <= wg; wg2 <= wg1;
            rp <= rn; rg <= (rn >> 1) ^ rn;
        end
    end
    initial begin
        if (DEPTH < 16 || (DEPTH & (DEPTH-1)) != 0)
            $fatal(1, "FIFO depth must be a power of two >= 16");
    end
`endif
endmodule
