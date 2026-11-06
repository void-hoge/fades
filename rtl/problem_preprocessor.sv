`timescale 1ns/1ps
// Shared, two-stage elastic input pipeline in the core clock domain.
// Stage 1 decodes cells; stage 2 propagates given digits once. Tags travel
// with their board, and both stages hold their payload under backpressure.
module problem_preprocessor (
    input wire clk,
    input wire reset,
    input wire [387:0] input_data,
    input wire input_valid,
    output wire input_ready,
    output logic [728:0] output_candidates,
    output logic [64:0] output_tag,
    output logic output_valid,
    input wire output_ready
);
    logic [728:0] decoded_candidates;
    logic invalid_cells;
    always_comb begin
        decoded_candidates = 0;
        invalid_cells = 0;
        for (integer c = 0; c < 81; c = c+1) begin
            if (input_data[64+4*c +: 4] == 0)
                decoded_candidates[9*c +: 9] = 9'h1ff;
            else if (input_data[64+4*c +: 4] <= 9)
                decoded_candidates[9*c +: 9] =
                    9'b1 << (input_data[64+4*c +: 4] - 1'b1);
            else invalid_cells = 1;
        end
    end

    logic [728:0] decode_candidates;
    logic [64:0] decode_tag;
    logic decode_valid;
    wire [728:0] propagated_candidates;
    naked_single_preload singles(decode_candidates, propagated_candidates);
    wire output_slot = !output_valid || output_ready;
    wire decode_slot = !decode_valid || output_slot;
    assign input_ready = !reset && decode_slot;

    // Payloads need no reset; only valid bits determine whether they are used.
    always_ff @(posedge clk) begin
        if (reset) begin
            decode_valid <= 0;
            output_valid <= 0;
        end else begin
            if (decode_slot) begin
                decode_valid <= input_valid;
                if (input_valid) begin
                    decode_candidates <= decoded_candidates;
                    decode_tag <= {invalid_cells, input_data[63:0]};
                end
            end
            if (output_slot) begin
                output_valid <= decode_valid;
                if (decode_valid) begin
                    // Invalid records still use the ordered result path,
                    // with an empty candidate board to suppress search.
                    output_candidates <= decode_tag[64] ? 729'b0 : propagated_candidates;
                    output_tag <= decode_tag;
                end
            end
        end
    end
endmodule
