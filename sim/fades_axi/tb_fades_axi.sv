`timescale 1ns/1ps
module tb_fades_axi;
    parameter integer CORES = 6;
    parameter integer LOCKED_CANDIDATES = 1;
    reg dma_clk=0, core_clk=0, aresetn=0;
    always #2.5 dma_clk=~dma_clk;
    always #5 core_clk=~core_clk;
    reg [11:0] awaddr=0, araddr=0;
    reg [31:0] wdata=0;
    reg [3:0] wstrb=0;
    reg awvalid=0,wvalid=0,bready=0,arvalid=0,rready=0;
    wire awready,wready,bvalid,arready,rvalid;
    wire [31:0] rdata;
    fades_axi #(.CORES(CORES), .LOCKED_CANDIDATES(LOCKED_CANDIDATES)) dut (
        .dma_clk(dma_clk),.core_clk(core_clk),.aresetn(aresetn),
        .s_axi_awaddr(awaddr),.s_axi_awprot(3'b0),.s_axi_awvalid(awvalid),.s_axi_awready(awready),
        .s_axi_wdata(wdata),.s_axi_wstrb(wstrb),.s_axi_wvalid(wvalid),.s_axi_wready(wready),
        .s_axi_bresp(),.s_axi_bvalid(bvalid),.s_axi_bready(bready),
        .s_axi_araddr(araddr),.s_axi_arprot(3'b0),.s_axi_arvalid(arvalid),.s_axi_arready(arready),
        .s_axi_rdata(rdata),.s_axi_rresp(),.s_axi_rvalid(rvalid),.s_axi_rready(rready),
        .s_axis_tdata(128'b0),.s_axis_tkeep(16'hffff),.s_axis_tvalid(1'b0),.s_axis_tlast(1'b0),.s_axis_tready(),
        .m_axis_tdata(),.m_axis_tkeep(),.m_axis_tvalid(),.m_axis_tlast(),.m_axis_tready(1'b0)
    );
    task automatic write_reg(input [11:0] addr,input [31:0] data,input [3:0] strobe,input bit wfirst);
        @(negedge dma_clk);
        awaddr=addr;wdata=data;wstrb=strobe;
        if(wfirst) begin
            wvalid=1; do @(posedge dma_clk); while(!wready);
            @(negedge dma_clk);wvalid=0;
            repeat(3) @(negedge dma_clk);
            awvalid=1;do @(posedge dma_clk);while(!awready);
            @(negedge dma_clk);awvalid=0;
        end else begin
            awvalid=1;do @(posedge dma_clk);while(!awready);
            @(negedge dma_clk);awvalid=0;
            repeat(3) @(negedge dma_clk);
            wvalid=1;do @(posedge dma_clk);while(!wready);
            @(negedge dma_clk);wvalid=0;
        end
        wait(bvalid);repeat(3) begin @(negedge dma_clk);if(!bvalid)$fatal(1,"B response lost");end
        bready=1;@(negedge dma_clk);bready=0;
    endtask
    task automatic read_reg(input [11:0] addr,output [31:0] value);
        @(negedge dma_clk);araddr=addr;arvalid=1;
        do @(posedge dma_clk);while(!arready);
        @(negedge dma_clk);arvalid=0;
        wait(rvalid);value=rdata;
        repeat(3) begin @(negedge dma_clk);if(!rvalid || rdata!==value)$fatal(1,"R response not held");end
        rready=1;@(negedge dma_clk);rready=0;
    endtask
    reg [31:0] value;
    integer group_index, slot;
    initial begin
        repeat(20)@(negedge dma_clk);aresetn=1;
        read_reg(0,value);if(value!==1)$fatal(1,"Reset default");
        read_reg(20,value);if(value!==32'h46414401)$fatal(1,"Version");
        write_reg(4,32'h11223344,15,0);
        write_reg(4,32'haabbccdd,5,1);
        read_reg(4,value);if(value!==32'h11bb33dd)$fatal(1,"Byte strobes");
        write_reg(8,17,15,1);write_reg(0,2,1,0);
        write_reg(4,1,15,0);
        read_reg(4,value);if(value!==32'h11bb33dd)$fatal(1,"Live config changed");
        repeat(100)@(negedge dma_clk);
        write_reg(16,1,1,1);
        repeat(20)@(negedge dma_clk);
        read_reg(12,value);if(value!==0)$fatal(1,"Snapshot did not complete");
        read_reg(12'h128,value);if(value==0)$fatal(1,"Core snapshot did not cross");
        read_reg(12'h100,value);if(value!==0)$fatal(1,"Spurious input count");
        // Idle enabled cores accumulate input wait; unimplemented ABI slots stay zero.
        read_reg(12'h200,value);if(value==0)$fatal(1,"Active core input wait missing");
        for(group_index=0;group_index<6;group_index=group_index+1)
            for(slot=CORES;slot<6;slot=slot+1) begin
                read_reg(12'(256+8*((group_index<4 ? 6+6*group_index : 32+6*(group_index-4))+slot)),value);
                if(value!==0)$fatal(1,"Unimplemented core counter nonzero");
                read_reg(12'(260+8*((group_index<4 ? 6+6*group_index : 32+6*(group_index-4))+slot)),value);
                if(value!==0)$fatal(1,"Unimplemented core counter high word nonzero");
            end
        write_reg(0,1,1,1);
        read_reg(0,value);if(value!==1)$fatal(1,"Soft reset");
        $display("PASS AXI-Lite independent AW/W, strobes, response stalls, config lock, snapshot");
        $finish;
    end
    initial begin #100000;$fatal(1,"Timeout");end
endmodule
