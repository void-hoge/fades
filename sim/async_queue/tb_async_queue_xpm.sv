`timescale 1ns/1ps
// Run with Vivado xsim and SYNTHESIS for the DUT: tests the actual XPM branch.
module tb_async_queue_xpm;
    reg wr_clk=0,rd_clk=0,reset=1;
    always #2.5 wr_clk=~wr_clk;
    always #5.3 rd_clk=~rd_clk;
    reg valid=0,ready=0,advanced=0;
    wire input_ready,output_valid;
    wire [387:0] data;
    wire [6:0] count;
    integer sent=0,received=0,age=0,stalls=0,peak=0;
    reg [387:0] held_data;
    reg held=0;
    function automatic [387:0] pattern(input integer n);
        for(integer b=0;b<388;b=b+1) pattern[b]=((n*7919+64'hfedcba9876543210)>>(b%64))&1;
    endfunction
    async_queue dut (
        .wr_clk(wr_clk),.rd_clk(rd_clk),.reset(reset),
        .input_data(pattern(sent)),.input_valid(valid),.input_ready(input_ready),
        .write_count(count),.output_data(data),.output_valid(output_valid),.output_ready(ready)
    );
    always @(negedge wr_clk) begin
        if(reset) valid=0;
        else if(!valid || advanced) valid=sent<1200 && $urandom_range(0,3)!=0;
    end
    always @(posedge wr_clk) begin
        advanced<=valid && input_ready;
        if(reset) begin sent<=0;age<=0;stalls<=0;peak<=0;end
        else begin
            age<=age+1;
            if(valid && input_ready)sent<=sent+1;
            if(valid && !input_ready)stalls<=stalls+1;
            if(count>66)$fatal(1,"Invalid XPM count");
            if(count>peak)peak<=count;
        end
    end
    always @(negedge rd_clk) ready=!reset && age>400 && $urandom_range(0,3)!=0;
    always @(posedge rd_clk) begin
        if(reset) begin received<=0;held<=0;end
        else begin
            if(held && (!output_valid || data!==held_data))$fatal(1,"XPM stall instability");
            held<=output_valid && !ready;
            if(output_valid && !ready)held_data<=data;
            if(output_valid && ready) begin
                if(data!==pattern(received))$fatal(1,"XPM order/data mismatch at %0d",received);
                received<=received+1;
            end
        end
    end
    initial begin
        #200;reset=0;
        wait(received==1200);
        if(stalls==0 || peak<64)$fatal(1,"XPM full/count was not exercised");
        #77;reset=1;#200;reset=0;
        // Abort a full FIFO and reuse un-cleared memory.
        #1500;reset=1;#200;reset=0;
        wait(received==1200);
        #100;
        if(output_valid || sent!=1200)$fatal(1,"XPM did not drain");
        $display("PASS actual XPM: async clocks, full, wrap, stalls and reset/reuse");
        $finish;
    end
    initial begin #200000;$fatal(1,"XPM timeout");end
endmodule
