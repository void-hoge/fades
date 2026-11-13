`timescale 1ns/1ps
// AXI-Lite control at dma_clk; independent AW/W buffering and held responses.
module fades_axi #(
    parameter integer CORES = 6
) (
    (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF S_AXI:S_AXIS:M_AXIS, ASSOCIATED_RESET aresetn, FREQ_HZ 200000000" *)
    input wire dma_clk,
    (* X_INTERFACE_PARAMETER = "FREQ_HZ 100000000" *) input wire core_clk,
    /* verilator lint_off SYNCASYNCNET */
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *) input wire aresetn,
    /* verilator lint_on SYNCASYNCNET */
    input wire [11:0] s_axi_awaddr,
    input wire [2:0] s_axi_awprot,
    input wire s_axi_awvalid, output wire s_axi_awready,
    input wire [31:0] s_axi_wdata, input wire [3:0] s_axi_wstrb,
    input wire s_axi_wvalid, output wire s_axi_wready,
    output wire [1:0] s_axi_bresp, output logic s_axi_bvalid,
    input wire s_axi_bready,
    input wire [11:0] s_axi_araddr, input wire [2:0] s_axi_arprot,
    input wire s_axi_arvalid, output wire s_axi_arready,
    output logic [31:0] s_axi_rdata, output wire [1:0] s_axi_rresp,
    output logic s_axi_rvalid, input wire s_axi_rready,
    input wire [127:0] s_axis_tdata, input wire [15:0] s_axis_tkeep,
    input wire s_axis_tvalid, s_axis_tlast, output wire s_axis_tready,
    output wire [127:0] m_axis_tdata, output wire [15:0] m_axis_tkeep,
    output wire m_axis_tvalid, m_axis_tlast, input wire m_axis_tready
);
    logic soft_reset, run_enable;
    logic [31:0] slot_records;
    logic [63:0] submitted_records;
    wire reset = !aresetn || soft_reset;
    (* ASYNC_REG = "TRUE" *) logic [1:0] enable_sync = 0;
    // Each stream clock domain generates its own synchronous reset release.
    always @(posedge core_clk) enable_sync <= {enable_sync[0],run_enable};
    wire framing_error;
    wire [63:0] accepted, emitted, dispatched, collected, input_stall, queue_empty;
    wire [63:0] queue_peak, processing_cycles;
    // Keep the six-slot counter ABI stable, padding unimplemented cores with zero.
    wire [383:0] core_dispatched, core_collected, core_busy, core_wait;
    wire [383:0] core_input_wait, core_max_inflight;
    if (CORES < 6) begin : unused_counters
        assign core_dispatched[383:CORES*64] = 0;
        assign core_collected[383:CORES*64] = 0;
        assign core_busy[383:CORES*64] = 0;
        assign core_wait[383:CORES*64] = 0;
        assign core_input_wait[383:CORES*64] = 0;
        assign core_max_inflight[383:CORES*64] = 0;
    end
`ifndef SYNTHESIS
    initial if (CORES < 1 || CORES > 6) $fatal(1, "CORES must be between 1 and 6");
`endif
    fades #(.CORES(CORES)) stream (
        .dma_clk(dma_clk), .core_clk(core_clk), .reset(reset),
        .enable(enable_sync[1]), .slot_records(slot_records), .submitted_records(submitted_records),
        .s_axis_tdata(s_axis_tdata), .s_axis_tkeep(s_axis_tkeep),
        .s_axis_tvalid(s_axis_tvalid), .s_axis_tlast(s_axis_tlast), .s_axis_tready(s_axis_tready),
        .m_axis_tdata(m_axis_tdata), .m_axis_tkeep(m_axis_tkeep),
        .m_axis_tvalid(m_axis_tvalid), .m_axis_tlast(m_axis_tlast), .m_axis_tready(m_axis_tready),
        .framing_error(framing_error), .accepted_count(accepted), .output_count(emitted),
        .dispatch_count(dispatched), .collected_count(collected),
        .input_stall_cycles(input_stall), .queue_empty_cycles(queue_empty),
        .queue_peak(queue_peak), .processing_cycles(processing_cycles),
        .core_dispatch_counts(core_dispatched[CORES*64-1:0]), .core_result_counts(core_collected[CORES*64-1:0]),
        .core_busy_cycles(core_busy[CORES*64-1:0]), .core_output_wait_cycles(core_wait[CORES*64-1:0]),
        .core_input_wait_cycles(core_input_wait[CORES*64-1:0]), .core_max_inflight(core_max_inflight[CORES*64-1:0])
    );
    // Snapshot handshake. Core bundle is held until a new request; ack takes
    // two DMA clocks to cross before the bundle is captured by the DMA side.
    logic snapshot_req, snapshot_ack;
    (* ASYNC_REG = "TRUE" *) logic [1:0] req_sync, ack_sync;
    logic [2559:0] core_snapshot;
    logic [2815:0] snapshot;
    logic snapshot_pending;
    (* ASYNC_REG = "TRUE" *) logic [2:0] board_core_reset_sync = 3'b111;
    always @(posedge core_clk or negedge aresetn) begin
        if (!aresetn) board_core_reset_sync <= 7;
        else board_core_reset_sync <= {board_core_reset_sync[1:0],1'b0};
    end
    always @(posedge core_clk) begin
        if (board_core_reset_sync[2]) begin req_sync <= 0; snapshot_ack <= 0; core_snapshot <= 0; end
        else begin
            req_sync <= {req_sync[0],snapshot_req};
            if (req_sync[1] != snapshot_ack) begin
                core_snapshot <= {core_max_inflight,core_input_wait,processing_cycles,core_wait,core_busy,core_collected,core_dispatched,queue_empty,collected,dispatched};
                snapshot_ack <= req_sync[1];
            end
        end
    end
    logic aw_pending, w_pending;
    logic [11:0] awaddr;
    logic [31:0] wdata;
    logic [3:0] wstrb;
    assign s_axi_awready = !aw_pending && !s_axi_bvalid;
    assign s_axi_wready = !w_pending && !s_axi_bvalid;
    assign s_axi_arready = !s_axi_rvalid;
    assign s_axi_bresp = 0;
    assign s_axi_rresp = 0;
    // Protection attributes are not used by these non-secure control registers.
    wire unused_prot = ^{s_axi_awprot,s_axi_arprot};
    always @(posedge dma_clk) begin
        if (!aresetn) begin
            soft_reset <= 1; run_enable <= 0; slot_records <= 0; submitted_records <= 0;
            aw_pending <= 0; w_pending <= 0; awaddr <= 0; wdata <= 0; wstrb <= 0;
            s_axi_bvalid <= 0; s_axi_rvalid <= 0; s_axi_rdata <= 0;
            snapshot_req <= 0; ack_sync <= 0; snapshot_pending <= 0; snapshot <= 0;
        end else begin
            if (soft_reset) submitted_records <= 0;
            ack_sync <= {ack_sync[0],snapshot_ack};
            if (snapshot_pending && ack_sync[1] == snapshot_req) begin
                snapshot <= {core_snapshot[2559:1728],queue_peak,core_snapshot[1727:0],input_stall,emitted,accepted};
                snapshot_pending <= 0;
            end
            if (s_axi_awvalid && s_axi_awready) begin aw_pending <= 1; awaddr <= s_axi_awaddr; end
            if (s_axi_wvalid && s_axi_wready) begin w_pending <= 1; wdata <= s_axi_wdata; wstrb <= s_axi_wstrb; end
            if (s_axi_bvalid && s_axi_bready) s_axi_bvalid <= 0;
            if (aw_pending && w_pending && !s_axi_bvalid) begin
                aw_pending <= 0; w_pending <= 0; s_axi_bvalid <= 1;
                case (awaddr)
                    12'h000: if (wstrb[0]) begin soft_reset <= wdata[0]; run_enable <= wdata[1]; end
                    12'h004: if (soft_reset && wstrb == 4'hf && wdata != 0) slot_records <= wdata;
                    12'h008: if (!soft_reset && wstrb == 4'hf) submitted_records <= submitted_records + {32'b0,wdata};
                    12'h010: if (wstrb[0] && wdata[0] && !snapshot_pending) begin snapshot_req <= !snapshot_req; snapshot_pending <= 1; end
                    default: ;
                endcase
            end
            if (s_axi_rvalid && s_axi_rready) s_axi_rvalid <= 0;
            if (s_axi_arvalid && s_axi_arready) begin
                s_axi_rvalid <= 1;
                case (s_axi_araddr)
                    12'h000: s_axi_rdata <= {30'b0,run_enable,soft_reset};
                    12'h004: s_axi_rdata <= slot_records;
                    12'h008: s_axi_rdata <= submitted_records[31:0];
                    12'h00c: s_axi_rdata <= {30'b0,snapshot_pending,framing_error};
                    12'h014: s_axi_rdata <= 32'h46414404;
                    12'h018: s_axi_rdata <= submitted_records[63:32];
                    default: begin
                        s_axi_rdata <= 0;
                        if(s_axi_araddr >= 12'h100 && s_axi_araddr < 12'h260 && s_axi_araddr[1:0] == 0)
                            s_axi_rdata <= snapshot[32*((int'(s_axi_araddr)-256)/4) +: 32];
                    end
                endcase
            end
        end
    end
endmodule
