/*

Copyright (c) 2021 Alex Forencich

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

*/

// Language: Verilog 2001

`resetall
`timescale 1ns / 1ps
`default_nettype none

/*
 * Xilinx UltraScale PCIe interface adapter (Requester Completion)
 */
module pcie_us_if_rc #
(
    // Width of PCIe AXI stream interfaces in bits
    parameter AXIS_PCIE_DATA_WIDTH = 512,
    // PCIe AXI stream tkeep signal width (words per cycle)
    parameter AXIS_PCIE_KEEP_WIDTH = (AXIS_PCIE_DATA_WIDTH/32),
    // PCIe AXI stream RC tuser signal width
    parameter AXIS_PCIE_RC_USER_WIDTH = AXIS_PCIE_DATA_WIDTH < 512 ? 75 : 161,
    // TLP segment count
    parameter TLP_SEG_COUNT = 1,
    // TLP segment data width
    parameter TLP_SEG_DATA_WIDTH = AXIS_PCIE_DATA_WIDTH/8 //64bits

)
(
    input  wire                                         clk,
    input  wire                                         rst,

    /*
     * AXI input (RC)
     */
    input  wire [AXIS_PCIE_DATA_WIDTH-1:0]              s_axis_rc_tdata,
    input  wire [AXIS_PCIE_KEEP_WIDTH-1:0]              s_axis_rc_tkeep,
    input  wire                                         s_axis_rc_tvalid,
    output wire                                         s_axis_rc_tready,
    input  wire                                         s_axis_rc_tlast,
    input  wire [AXIS_PCIE_RC_USER_WIDTH-1:0]           s_axis_rc_tuser,

    /*
     * TLP output (completion to DMA)
     */
    output wire [TLP_SEG_COUNT*TLP_SEG_DATA_WIDTH-1:0]  s_axi_rdata, //s_axi_rdata
    //output wire [TLP_SEG_COUNT*TLP_SEG_HDR_WIDTH-1:0]   rx_cpl_tlp_hdr, 
    //output wire [TLP_SEG_COUNT*4-1:0]                   rx_cpl_tlp_error,
    output wire [TLP_SEG_COUNT-1:0]                     s_axi_rvalid, //s_axi_rvalid
    output wire [TLP_SEG_COUNT:0]                       s_axi_rresp, //s_axi_rresp
    //output wire [TLP_SEG_COUNT-1:0]                     rx_cpl_tlp_eop,
    input  wire                                         s_axi_rready //s_axi_rready
);

reg [TLP_SEG_COUNT*TLP_SEG_DATA_WIDTH-1:0] s_axi_rdata_reg = 0, s_axi_rdata_next;
reg [TLP_SEG_COUNT-1:0] s_axi_rvalid_reg = 0, s_axi_rvalid_next;
reg [TLP_SEG_COUNT:0] s_axi_rresp_reg = 0, s_axi_rresp_next;

assign s_axi_rdata = s_axi_rdata_reg;
assign s_axi_rvalid = s_axi_rvalid_reg;
assign s_axi_rresp = s_axi_rresp_reg;

localparam [1:0]
    TLP_INPUT_STATE_IDLE = 2'd0,
    TLP_INPUT_STATE_HEADER = 2'd1,
    TLP_INPUT_STATE_PAYLOAD = 2'd2;

reg [1:0] tlp_input_state_reg = TLP_INPUT_STATE_IDLE, tlp_input_state_next;

reg s_axis_rc_tready_cmb;

reg tlp_input_frame_reg = 1'b0, tlp_input_frame_next;

reg [AXIS_PCIE_DATA_WIDTH-1:0] rc_tdata_int_reg = {AXIS_PCIE_DATA_WIDTH{1'b0}}, rc_tdata_int_next;
reg rc_tvalid_int_reg = 1'b0, rc_tvalid_int_next;
reg rc_tlast_int_reg = 1'b0, rc_tlast_int_next;

wire [AXIS_PCIE_DATA_WIDTH*2-1:0] rc_tdata = {s_axis_rc_tdata, rc_tdata_int_reg};

assign s_axis_rc_tready = s_axis_rc_tready_cmb;

always @* begin
    tlp_input_state_next = TLP_INPUT_STATE_IDLE;

    s_axi_rdata_next = s_axi_rdata_reg;
    s_axi_rvalid_next = s_axi_rvalid_reg && !s_axi_rready;
    s_axi_rresp_next = s_axi_rresp_reg;

    s_axis_rc_tready_cmb = s_axi_rready;

    tlp_input_frame_next = tlp_input_frame_reg;

    rc_tdata_int_next = rc_tdata_int_reg;
    rc_tvalid_int_next = rc_tvalid_int_reg;
    rc_tlast_int_next = rc_tlast_int_reg;

    case (tlp_input_state_reg)
        TLP_INPUT_STATE_IDLE: begin
            s_axis_rc_tready_cmb = s_axi_rready;


                if (AXIS_PCIE_DATA_WIDTH > 64) begin
                    s_axi_rdata_next = rc_tdata[159:96]; //[AXIS_PCIE_DATA_WIDTH+96-1:96];
                    s_axi_rresp_next = 1'b1;

                    tlp_input_frame_next = 1'b1;

                    if (rc_tlast_int_reg) begin
                        s_axi_rvalid_next = 1'b1;
                        rc_tvalid_int_next = 1'b0;
                        tlp_input_frame_next = 1'b0;
                        tlp_input_state_next = TLP_INPUT_STATE_IDLE;
                    end else if (s_axis_rc_tready && s_axis_rc_tvalid) begin
                        s_axi_rvalid_next = 1'b1;
                        tlp_input_state_next = TLP_INPUT_STATE_PAYLOAD;
                    end else begin
                        tlp_input_state_next = TLP_INPUT_STATE_IDLE;
                    end
                end else begin
                    if (rc_tlast_int_reg) begin
                        rc_tvalid_int_next = 1'b0;
                        tlp_input_frame_next = 1'b0;
                        tlp_input_state_next = TLP_INPUT_STATE_IDLE;
                    end else if (s_axis_rc_tready && s_axis_rc_tvalid) begin
                        tlp_input_state_next = TLP_INPUT_STATE_PAYLOAD;
                    end else begin
                        tlp_input_state_next = TLP_INPUT_STATE_IDLE;
                    end
                end
                tlp_input_state_next = TLP_INPUT_STATE_IDLE;
        end
        TLP_INPUT_STATE_PAYLOAD: begin
            s_axis_rc_tready_cmb = s_axi_rready;

            if (rc_tvalid_int_reg && s_axi_rready) begin

                if (AXIS_PCIE_DATA_WIDTH > 64) begin
                    s_axi_rdata_next = rc_tdata[AXIS_PCIE_DATA_WIDTH+96-1:96];
                    s_axi_rresp_next = 1'b0;
                end else begin
                    s_axi_rdata_next = rc_tdata[AXIS_PCIE_DATA_WIDTH+32-1:32];
                    s_axi_rresp_next = !tlp_input_frame_reg;
                end

                if (rc_tlast_int_reg) begin
                    s_axi_rvalid_next = 1'b1;
                    rc_tvalid_int_next = 1'b0;
                    tlp_input_frame_next = 1'b0;
                    tlp_input_state_next = TLP_INPUT_STATE_IDLE;
                end else if (s_axis_rc_tready && s_axis_rc_tvalid) begin
                    s_axi_rvalid_next = 1'b1;
                    tlp_input_frame_next = 1'b1;
                    tlp_input_state_next = TLP_INPUT_STATE_PAYLOAD;
                end else begin
                    tlp_input_state_next = TLP_INPUT_STATE_PAYLOAD;
                end
            end else begin
                tlp_input_state_next = TLP_INPUT_STATE_PAYLOAD;
            end
        end
    endcase

    if (s_axis_rc_tready && s_axis_rc_tvalid) begin
        rc_tdata_int_next = s_axis_rc_tdata;
        rc_tvalid_int_next = s_axis_rc_tvalid;
        rc_tlast_int_next = s_axis_rc_tlast;
    end
end

always @(posedge clk) begin
    tlp_input_state_reg <= tlp_input_state_next;

    s_axi_rdata_reg <= s_axi_rdata_next;
    s_axi_rvalid_reg <= s_axi_rvalid_next;
    s_axi_rresp_reg <= s_axi_rresp_next;

    tlp_input_frame_reg <= tlp_input_frame_next;

    rc_tdata_int_reg <= rc_tdata_int_next;
    rc_tvalid_int_reg <= rc_tvalid_int_next;
    rc_tlast_int_reg <= rc_tlast_int_next;

    if (rst) begin
        tlp_input_state_reg <= TLP_INPUT_STATE_IDLE;

        s_axi_rvalid_reg <= 0;

        rc_tvalid_int_reg <= 1'b0;
    end
end

endmodule

`resetall
