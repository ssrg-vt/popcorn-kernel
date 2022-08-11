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
 * Xilinx UltraScale PCIe interface adapter (Requester reQuest)
 */
module pcie_us_rqrc #
(
    // Width of PCIe AXI stream interfaces in bits
    parameter AXIS_PCIE_DATA_WIDTH = 512,
    // PCIe AXI stream tkeep signal width (words per cycle)
    parameter AXIS_PCIE_KEEP_WIDTH = (AXIS_PCIE_DATA_WIDTH/32), //16
    // PCIe AXI stream RQ tuser signal width
    parameter AXIS_PCIE_RQ_USER_WIDTH = AXIS_PCIE_DATA_WIDTH < 512 ? 60 : 137,
    parameter AXIS_PCIE_RC_USER_WIDTH = AXIS_PCIE_DATA_WIDTH < 512 ? 75 : 161,
    // RQ sequence number width
    //parameter RQ_SEQ_NUM_WIDTH = AXIS_PCIE_RQ_USER_WIDTH == 60 ? 4 : 6
    // TLP segment count
    parameter TLP_SEG_COUNT = 1,
    // TLP segment data width
    parameter TLP_SEG_DATA_WIDTH = AXIS_PCIE_DATA_WIDTH/TLP_SEG_COUNT,
    // TLP segment strobe width
    //parameter TLP_SEG_STRB_WIDTH = TLP_SEG_DATA_WIDTH/32,
    // TLP segment header width
    parameter TLP_SEG_HDR_WIDTH = 128,
    
    parameter AXI_MM_DATA_WIDTH = 64
    // TX sequence number count
    //parameter TX_SEQ_NUM_COUNT = AXIS_PCIE_DATA_WIDTH < 512 ? 1 : 2,
    // TX sequence number width
    //parameter TX_SEQ_NUM_WIDTH = RQ_SEQ_NUM_WIDTH-1
)
(
    input  wire                                          clk,
    input  wire                                          rst,

    /*
     * AXI output (RQ)
     */
    output wire [AXIS_PCIE_DATA_WIDTH-1:0]               m_axis_rq_tdata,
    output wire [63 : 0]                                 m_axis_rq_tkeep,
    output wire                                          m_axis_rq_tvalid,
    input  wire                                          m_axis_rq_tready,
    output wire                                          m_axis_rq_tlast,
    output wire [AXIS_PCIE_RQ_USER_WIDTH-1:0]            m_axis_rq_tuser,

    input wire [AXIS_PCIE_DATA_WIDTH-1:0]               s_axis_rc_tdata,
    input wire [AXIS_PCIE_KEEP_WIDTH-1:0]               s_axis_rc_tkeep,
    input wire                                          s_axis_rc_tvalid,
    output wire                                         s_axis_rc_tready,
    input wire                                          s_axis_rc_tlast,
    input wire [AXIS_PCIE_RC_USER_WIDTH-1:0]            s_axis_rc_tuser,

    
    /*Read address channel*/
    input wire [39:0] s_axi_araddr,
    input wire s_axi_arvalid,
    output wire s_axi_arready,
    input wire [2:0] s_axi_arprot,
    //unused ports
//    input wire [15:0] s_axi_arid,   
//    input wire [7:0] s_axi_arlen,
//    input wire [2:0] s_axi_arsize, 
//    input wire [1:0] s_axi_arburst,
//    input wire s_axi_arlock,
//    input wire [3:0] s_axi_arcache,
//    input wire [15:0] s_axi_aruser,
    
    /*Read data channel*/
    output wire [AXI_MM_DATA_WIDTH-1:0] s_axi_rdata,
    output wire s_axi_rvalid,
    output wire[1:0] s_axi_rresp,
    input wire s_axi_rready,
    //unused ports
//    output wire [15:0] s_axi_rid,
//    output wire s_axi_rlast,
    
    /*Write data channel*/
    input wire[AXI_MM_DATA_WIDTH-1:0] s_axi_wdata, 
    input wire s_axi_wvalid,
    output wire s_axi_wready,
    input wire [15:0] s_axi_wstrb,
    //unused ports
//    input wire s_axi_wlast,
    
    /*Write address channel*/
    input wire[39:0] s_axi_awaddr,
    input wire[2:0] s_axi_awprot,
    input wire s_axi_awvalid, 
    output wire s_axi_awready,
    //unused ports
//    input wire [15:0] s_axi_awid,
//    input wire [7:0] s_axi_awlen,
//    input wire [2:0] s_axi_awsize, 
//    input wire [1:0] s_axi_awburst,
//    input wire s_axi_awlock,
//    input wire [3:0] s_axi_awcache,
//    input wire [15:0] s_axi_awuser,
    
    /*Write response channel*/
    output wire [1 : 0] s_axi_bresp,
    output wire s_axi_bvalid,
    input wire s_axi_bready,
    //unused ports
//    output wire[15:0] s_axi_bid,
    
    input wire [511:0] cc_data_in,
    output wire zynq_rq_actv
    //output wire tag_addr_out
);

parameter OUTPUT_FIFO_ADDR_WIDTH = 5;

// bus width assertions

localparam [3:0]
    REQ_MEM_READ = 4'b0000,
    REQ_MEM_WRITE = 4'b0001,
    REQ_IO_READ = 4'b0010,
    REQ_IO_WRITE = 4'b0011;
    
reg tx_rd_req_tlp_ready_cmb;

//wire [TLP_SEG_COUNT*RQ_SEQ_NUM_WIDTH-1:0] tx_rd_req_tlp_seq_int = {1'b1, tx_rd_req_tlp_seq};

reg tx_wr_req_tlp_ready_cmb;

//wire [TLP_SEG_COUNT*RQ_SEQ_NUM_WIDTH-1:0] tx_wr_req_tlp_seq_int = {1'b0, tx_wr_req_tlp_seq};

assign s_axi_arready = tx_rd_req_tlp_ready_cmb;

assign s_axi_wready = tx_wr_req_tlp_ready_cmb;
assign s_axi_awready = tx_wr_req_tlp_ready_cmb;


localparam [1:0]
    TLP_OUTPUT_STATE_IDLE = 2'd0,
    TLP_OUTPUT_STATE_RD_HEADER = 2'd1,
    TLP_OUTPUT_STATE_WR_HEADER = 2'd2,
    TLP_OUTPUT_STATE_WR_PAYLOAD = 2'd3;

reg [1:0] tlp_output_state_reg = TLP_OUTPUT_STATE_IDLE, tlp_output_state_next;

reg [TLP_SEG_COUNT*TLP_SEG_DATA_WIDTH-1:0] out_tlp_data_reg = 0, out_tlp_data_next;
//reg [TLP_SEG_COUNT*TLP_SEG_STRB_WIDTH-1:0] out_tlp_strb_reg = 0, out_tlp_strb_next;
reg [TLP_SEG_COUNT-1:0] out_tlp_eop_reg = 0, out_tlp_eop_next;

reg [127:0] tlp_header_data_rd;
reg [AXIS_PCIE_RQ_USER_WIDTH-1:0] tlp_tuser_rd = 0;
reg [127:0] tlp_header_data_wr;
reg [AXIS_PCIE_RQ_USER_WIDTH-1:0] tlp_tuser_wr = 0;

reg  [AXIS_PCIE_DATA_WIDTH-1:0]    m_axis_rq_tdata_int = 0;
reg  [AXIS_PCIE_KEEP_WIDTH-1:0]    m_axis_rq_tkeep_int = 0;
reg                                m_axis_rq_tvalid_int = 0;
wire                               m_axis_rq_tready_int;
reg                                m_axis_rq_tlast_int = 0;
reg  [AXIS_PCIE_RQ_USER_WIDTH-1:0] m_axis_rq_tuser_int = 0;

reg [1:0]axi_bresp = 0;
reg axi_awready;
reg axi_bvalid = 0;
reg axi_wready;
reg [39:0] axi_awaddr;
wire slv_reg_wren;
reg axi_arready;
reg [39:0] axi_araddr;
//reg axi_rvalid;
//reg axi_rresp;
reg [3:0] tag_data = 0;
//reg tag_addr [2**8-1:0];

//reg[1:0] cnt = 2'b00;
reg cnt = 1'b0;
reg [63:0] phy_addr = 64'd0;

/*Get the physical address from the RAM*/
//assign tag_addr_out = tag_addr[tag_data];

always @(posedge clk) begin
    if(cc_data_in[6:0] == 7'h0 && cc_data_in[127:96] != 0 && cnt != 1'b1) begin
    
     /*   if((cc_data_in[3:0] == 4'h0) && (cnt != 2'b01)) begin
            phy_addr[63:32] = cc_data_in[127:96];
            cnt = 2'b01;
        end*/
        //else if ((cc_data_in[3:0] == 4'h4) && (cnt != 2'b11)) begin
            phy_addr[31:0] = cc_data_in[127:96];
            cnt = 1'b1;
        //end
        
    end

end

always @* begin
    tlp_output_state_next = TLP_OUTPUT_STATE_IDLE;

    out_tlp_data_next = out_tlp_data_reg;
    //out_tlp_strb_next = out_tlp_strb_reg;
    out_tlp_eop_next = out_tlp_eop_reg;

    tx_rd_req_tlp_ready_cmb = 1'b0;
    tx_wr_req_tlp_ready_cmb = 1'b0;

    // TLP header and sideband data
    //tlp_header_data_rd[1:0] = 2'b00; //tx_rd_req_tlp_hdr[107:106]; // address type
    tlp_header_data_rd[63:0] = {phy_addr[63:16], s_axi_araddr[15:0]};//{46'd0, 4'h4, s_axi_araddr[11:0]}; //tx_rd_req_tlp_hdr[63:2]; // address
    tlp_header_data_rd[74:64] = 11'h002; //(tx_rd_req_tlp_hdr[105:96] != 0) ? tx_rd_req_tlp_hdr[105:96] : 11'd1024; // DWORD count
    if (s_axi_arvalid) begin
        tlp_header_data_rd[78:75] = REQ_MEM_READ; // request type - memory read
    end
    tlp_header_data_rd[79] = 1'b0; //tx_rd_req_tlp_hdr[110]; // poisoned request
    tlp_header_data_rd[95:80] = {8'h00, 5'b00000, 3'b000}; //tx_rd_req_tlp_hdr[95:80]; // requester ID
    tlp_header_data_rd[103:96] = {4'hF, tag_data}; //tx_rd_req_tlp_hdr[79:72]; // tag //redefine
    tlp_header_data_rd[119:104] = 16'd0; // completer ID
    tlp_header_data_rd[120] = 1'b0; // requester ID enable
    tlp_header_data_rd[123:121] = 3'd0; //tx_rd_req_tlp_hdr[118:116]; // traffic class
    tlp_header_data_rd[126:124] = 3'd0; //{tx_rd_req_tlp_hdr[114], tx_rd_req_tlp_hdr[109:108]}; // attr
    tlp_header_data_rd[127] = 1'b0; // force ECRC
    //store address using tag as index and set the valid bit.  
    //tag_addr[tag_data] = 1'b1;

    if (AXIS_PCIE_DATA_WIDTH == 512) begin
        //tlp_tuser_rd = 137'd0;
        tlp_tuser_rd[3:0] = 4'hF; //tx_rd_req_tlp_hdr[67:64]; // first BE 0
        tlp_tuser_rd[7:4] = 4'd0; // first BE 1
        tlp_tuser_rd[11:8] = 4'hF;//tx_rd_req_tlp_hdr[71:68]; // last BE 0
        tlp_tuser_rd[15:12] = 4'd0; // last BE 1
        tlp_tuser_rd[19:16] = 3'd0; // addr_offset
        tlp_tuser_rd[21:20] = 2'b01; // is_sop
        tlp_tuser_rd[23:22] = 2'd0; // is_sop0_ptr
        tlp_tuser_rd[25:24] = 2'd0; // is_sop1_ptr
        tlp_tuser_rd[27:26] = 2'b01; // is_eop
        tlp_tuser_rd[31:28]  = 4'd4; // is_eop0_ptr
        tlp_tuser_rd[35:32] = 4'd0; // is_eop1_ptr
        tlp_tuser_rd[36] = 1'b0; // discontinue
        tlp_tuser_rd[38:37] = 2'b00; // tph_present
        tlp_tuser_rd[42:39] = 4'b0000; // tph_type
        tlp_tuser_rd[44:43] = 2'b00; // tph_indirect_tag_en
        tlp_tuser_rd[60:45] = 16'd0; // tph_st_tag
        tlp_tuser_rd[66:61] = 6'd0; // seq_num0
        tlp_tuser_rd[72:67] = 6'd0; // seq_num1
        tlp_tuser_rd[136:73] = 64'd0; // parity
    end

    //tlp_header_data_wr[1:0] = 2'b0; //tx_wr_req_tlp_hdr[107:106]; // address type
    tlp_header_data_wr[63:0] = {phy_addr[63:16], s_axi_awaddr[15:0]};//{46'd0, 4'h1, s_axi_awaddr[11:0]};//s_axi_awaddr; //tx_wr_req_tlp_hdr[63:2]; // address
    tlp_header_data_wr[74:64] = 11'h002; //(tx_wr_req_tlp_hdr[105:96] != 0) ? tx_wr_req_tlp_hdr[105:96] : 11'd1024; // DWORD count
    if (s_axi_awvalid) begin
        tlp_header_data_wr[78:75] = REQ_MEM_WRITE; // request type - memory write
    end
    tlp_header_data_wr[79] = 1'b0; //tx_wr_req_tlp_hdr[110]; // poisoned request
    tlp_header_data_wr[95:80] = {8'h00, 5'b00000, 3'b000}; //tx_wr_req_tlp_hdr[95:80]; // requester ID
    tlp_header_data_wr[103:96] = 8'd0; //tx_wr_req_tlp_hdr[79:72]; // tag
    tlp_header_data_wr[119:104] = 16'd0; // completer ID
    tlp_header_data_wr[120] = 1'b0; // requester ID enable
    tlp_header_data_wr[123:121] = 3'd0; //tx_wr_req_tlp_hdr[118:116]; // traffic class
    tlp_header_data_wr[126:124] = 3'd0; //{tx_wr_req_tlp_hdr[114], tx_wr_req_tlp_hdr[109:108]}; // attr
    tlp_header_data_wr[127] = 1'b0; // force ECRC

    if (AXIS_PCIE_DATA_WIDTH == 512) begin
        //tlp_tuser_wr = 137'd0;
        tlp_tuser_wr[3:0] = 4'hF;//tx_wr_req_tlp_hdr[67:64]; // first BE 0
        tlp_tuser_wr[7:4] = 4'h0; // first BE 1
        tlp_tuser_wr[11:8] = 4'hF; //tx_wr_req_tlp_hdr[71:68]; // last BE 0
        tlp_tuser_wr[15:12] = 4'd0; // last BE 1
        tlp_tuser_wr[19:16] = 3'd0; // addr_offset
        tlp_tuser_wr[21:20] = 2'b01; // is_sop
        tlp_tuser_wr[23:22] = 2'd0; // is_sop0_ptr
        tlp_tuser_wr[25:24] = 2'd0; // is_sop1_ptr
        tlp_tuser_wr[27:26] = 2'b01; // is_eop
        tlp_tuser_wr[31:28]  = 4'd5; // is_eop0_ptr
        tlp_tuser_wr[35:32] = 4'd0; // is_eop1_ptr
        tlp_tuser_wr[36] = 1'b0; // discontinue
        tlp_tuser_wr[38:37] = 2'b00; // tph_present
        tlp_tuser_wr[42:39] = 4'b0000; // tph_type
        tlp_tuser_wr[44:43] = 2'b00; // tph_indirect_tag_en
        tlp_tuser_wr[60:45] = 16'd0; // tph_st_tag
        tlp_tuser_wr[66:61] = 6'd0; // seq_num0
        tlp_tuser_wr[72:67] = 6'd0; // seq_num1
        tlp_tuser_wr[136:73] = 64'd0; // parity 
    end

    // TLP output
    m_axis_rq_tdata_int = 0;
    m_axis_rq_tkeep_int = 0;
    m_axis_rq_tvalid_int = 1'b0;
    m_axis_rq_tlast_int = 1'b0;
    m_axis_rq_tuser_int = 0;

    // combine header and payload, merge in read request TLPs
    case (tlp_output_state_reg)
        TLP_OUTPUT_STATE_IDLE: begin
            // idle state

            if (s_axi_arvalid && m_axis_rq_tready_int) begin
                    // wider interface, send complete header (read request)
                    m_axis_rq_tdata_int = tlp_header_data_rd;
                    m_axis_rq_tkeep_int = 16'h000F;
                    m_axis_rq_tvalid_int = 1'b1;
                    m_axis_rq_tlast_int = 1'b1;
                    m_axis_rq_tuser_int = tlp_tuser_rd;
                    if (tag_data == 15) begin
                        tag_data = 4'd0;
                    end 
                    else begin 
                        tag_data = tag_data + 1;
                    end 
                    tx_rd_req_tlp_ready_cmb = 1'b1;
                    tlp_output_state_next = TLP_OUTPUT_STATE_IDLE;
                
            end else if (s_axi_awvalid && s_axi_wvalid && m_axis_rq_tready_int) begin
                    // wider interface, send header and start of payload (write request)
                    m_axis_rq_tdata_int = {s_axi_wdata, tlp_header_data_wr};
                    m_axis_rq_tkeep_int = 16'h003F;
                    m_axis_rq_tvalid_int = 1'b1;
                    m_axis_rq_tlast_int = 1'b1;
                    m_axis_rq_tuser_int = tlp_tuser_wr;
                    //wenable = 1'b0;
                    tx_wr_req_tlp_ready_cmb = 1'b1;

                    out_tlp_data_next = s_axi_wdata;
                    //out_tlp_strb_next = tx_wr_req_tlp_strb;
                    //out_tlp_eop_next = tx_wr_req_tlp_eop;

                    //if (((tx_wr_req_tlp_strb >> (TLP_DATA_WIDTH_DWORDS-4)) == 0)) begin
                        //m_axis_rq_tlast_int = 1'b1;
                        tlp_output_state_next = TLP_OUTPUT_STATE_IDLE;
                    //end else begin
                      //  tlp_output_state_next = TLP_OUTPUT_STATE_WR_PAYLOAD;
                    //end
            end else begin
                tlp_output_state_next = TLP_OUTPUT_STATE_IDLE;
            end
        end
    endcase
end

always @(posedge clk) begin
    tlp_output_state_reg <= tlp_output_state_next;

    out_tlp_data_reg <= out_tlp_data_next;
    //out_tlp_strb_reg <= out_tlp_strb_next;
    out_tlp_eop_reg <= out_tlp_eop_next;

    if (!rst) begin
        tlp_output_state_reg <= TLP_OUTPUT_STATE_IDLE;
    end
end
	
	// Implement write response logic generation
	// The write response and response valid signals are asserted by the slave 
	// when axi_wready, S_AXI_WVALID, axi_wready and S_AXI_WVALID are asserted.  
	// This marks the acceptance of address and indicates the status of 
	// write transaction.
   /* always @( posedge clk )
    begin
      if ( !rst ) begin
          axi_bvalid  <= 0;
          axi_bresp   <= 2'b0;
        end 
      else begin    
          if (~axi_bvalid && m_axis_rq_tvalid)
            begin
              // indicates a valid write response is available
              axi_bvalid <= 1'b1;
              axi_bresp  <= 2'b0; // 'OKAY' response 
            end                   // work error responses in future
          else begin
              if (s_axi_bready && axi_bvalid)  begin
                //check if bready is asserted while bvalid is high) 
                //(there is a possibility that bready is always asserted high)   
                    axi_bvalid <= 1'b0; 
                end  
            end
        end
    end*/

// output datapath logic (PCIe TLP)
reg [AXIS_PCIE_DATA_WIDTH-1:0]    m_axis_rq_tdata_reg = {AXIS_PCIE_DATA_WIDTH{1'b0}};
reg [AXIS_PCIE_KEEP_WIDTH-1:0]    m_axis_rq_tkeep_reg = {AXIS_PCIE_KEEP_WIDTH{1'b0}};
reg                               m_axis_rq_tvalid_reg = 1'b0, m_axis_rq_tvalid_next;
reg                               m_axis_rq_tlast_reg = 1'b0;
reg [AXIS_PCIE_RQ_USER_WIDTH-1:0] m_axis_rq_tuser_reg = {AXIS_PCIE_RQ_USER_WIDTH{1'b0}};

reg [OUTPUT_FIFO_ADDR_WIDTH+1-1:0] out_fifo_wr_ptr_reg = 0;
reg [OUTPUT_FIFO_ADDR_WIDTH+1-1:0] out_fifo_rd_ptr_reg = 0;
reg out_fifo_half_full_reg = 1'b0;

wire out_fifo_full = out_fifo_wr_ptr_reg == (out_fifo_rd_ptr_reg ^ {1'b1, {OUTPUT_FIFO_ADDR_WIDTH{1'b0}}});
wire out_fifo_empty = out_fifo_wr_ptr_reg == out_fifo_rd_ptr_reg;

(* ram_style = "distributed" *)
reg [AXIS_PCIE_DATA_WIDTH-1:0]    out_fifo_tdata[2**OUTPUT_FIFO_ADDR_WIDTH-1:0];
(* ram_style = "distributed" *)
reg [AXIS_PCIE_KEEP_WIDTH-1:0]    out_fifo_tkeep[2**OUTPUT_FIFO_ADDR_WIDTH-1:0];
(* ram_style = "distributed" *)
reg                               out_fifo_tlast[2**OUTPUT_FIFO_ADDR_WIDTH-1:0];
(* ram_style = "distributed" *)
reg [AXIS_PCIE_RQ_USER_WIDTH-1:0] out_fifo_tuser[2**OUTPUT_FIFO_ADDR_WIDTH-1:0];

assign m_axis_rq_tready_int = !out_fifo_half_full_reg;

assign m_axis_rq_tdata = m_axis_rq_tdata_reg;
assign m_axis_rq_tkeep = {48'd0, m_axis_rq_tkeep_reg};
assign m_axis_rq_tvalid = m_axis_rq_tvalid_reg;
assign zynq_rq_actv = m_axis_rq_tvalid_reg;
assign m_axis_rq_tlast = m_axis_rq_tlast_reg;
assign m_axis_rq_tuser = m_axis_rq_tuser_reg;
assign s_axi_bvalid = axi_bvalid;
assign s_axi_bresp = axi_bresp;

always @(posedge clk) begin
    m_axis_rq_tvalid_reg <= m_axis_rq_tvalid_reg && !m_axis_rq_tready;
    //axi_bvalid <= m_axis_rq_tvalid_reg && !m_axis_rq_tready;

    out_fifo_half_full_reg <= $unsigned(out_fifo_wr_ptr_reg - out_fifo_rd_ptr_reg) >= 2**(OUTPUT_FIFO_ADDR_WIDTH-1);

    if (!out_fifo_full && m_axis_rq_tvalid_int) begin
        out_fifo_tdata[out_fifo_wr_ptr_reg[OUTPUT_FIFO_ADDR_WIDTH-1:0]] <= m_axis_rq_tdata_int;
        out_fifo_tkeep[out_fifo_wr_ptr_reg[OUTPUT_FIFO_ADDR_WIDTH-1:0]] <= m_axis_rq_tkeep_int;
        out_fifo_tlast[out_fifo_wr_ptr_reg[OUTPUT_FIFO_ADDR_WIDTH-1:0]] <= m_axis_rq_tlast_int;
        out_fifo_tuser[out_fifo_wr_ptr_reg[OUTPUT_FIFO_ADDR_WIDTH-1:0]] <= m_axis_rq_tuser_int;
        out_fifo_wr_ptr_reg <= out_fifo_wr_ptr_reg + 1;
    end

    if (!out_fifo_empty && (!m_axis_rq_tvalid_reg || m_axis_rq_tready)) begin
        m_axis_rq_tdata_reg <= out_fifo_tdata[out_fifo_rd_ptr_reg[OUTPUT_FIFO_ADDR_WIDTH-1:0]];
        m_axis_rq_tkeep_reg <= out_fifo_tkeep[out_fifo_rd_ptr_reg[OUTPUT_FIFO_ADDR_WIDTH-1:0]];
        m_axis_rq_tvalid_reg <= 1'b1;
        m_axis_rq_tlast_reg <= out_fifo_tlast[out_fifo_rd_ptr_reg[OUTPUT_FIFO_ADDR_WIDTH-1:0]];
        m_axis_rq_tuser_reg <= out_fifo_tuser[out_fifo_rd_ptr_reg[OUTPUT_FIFO_ADDR_WIDTH-1:0]];
        out_fifo_rd_ptr_reg <= out_fifo_rd_ptr_reg + 1;
        axi_bvalid <= 1'b1;
        axi_bresp <= 2'b0;
    end

    if (!rst) begin
        out_fifo_wr_ptr_reg <= 0;
        out_fifo_rd_ptr_reg <= 0;
        m_axis_rq_tvalid_reg <= 1'b0;
        axi_bvalid <= 1'b0;
        axi_bresp <= 2'b0;
    end
end

/*Read logic implementation*/
assign s_axi_rdata	= axi_rdata;
assign s_axi_rresp	= axi_rresp;
//assign s_axi_rlast	= axi_rlast;
//assign s_axi_ruser	= axi_ruser;
assign s_axi_rvalid	= axi_rvalid;
assign s_axis_rc_tready = s_axi_rready;

reg [AXI_MM_DATA_WIDTH-1 : 0] 	  axi_rdata;
reg [1 : 0] 	                  axi_rresp;
reg  	                          axi_rlast;
reg  	                          axi_rvalid;

always @(posedge clk) begin 
    if(!rst) begin 
        axi_rdata <= 64'd0;
        axi_rvalid = 1'b0;
        axi_rresp = 1'b0;
    end
    else if(s_axis_rc_tvalid && ~axi_rvalid) begin
        axi_rdata <= s_axis_rc_tdata[159:96]; 
        axi_rvalid = 1'b1;
        axi_rresp = 1'b0;
    end
    else begin
        axi_rvalid = 1'b0;
        axi_rresp = 1'b0; 
    end
end
/*
always @(posedge clk) begin
        if(!rst) begin 
            axi_rdata <= 64'd0;
        end 
        if(axi_rvalid && s_axi_rready) begin 
            axi_rdata <= s_axis_rc_tdata[159:96];
        end
end
*/
	// Implement axi_arvalid generation

	// axi_rvalid is asserted for one S_AXI_ACLK clock cycle when both 
	// S_AXI_ARVALID and axi_arready are asserted. The slave registers 
	// data are available on the axi_rdata bus at this instance. The 
	// assertion of axi_rvalid marks the validity of read data on the 
	// bus and axi_rresp indicates the status of read transaction.axi_rvalid 
	// is deasserted on reset (active low). axi_rresp and axi_rdata are 
	// cleared to zero on reset (active low).  
/*
	always @( posedge clk )
	begin
	  if ( rst == 1'b0 )
	    begin
	      axi_rvalid <= 0;
	      axi_rresp  <= 0;
	    end   
	  else 
	   begin
	       if (tx_rd_req_tlp_ready_cmb && ~axi_rvalid)
	        begin
	          //axi_rdata <= s_axis_rc_tdata[159:96];
	          axi_rvalid <= 1'b1;
	          axi_rresp  <= 2'b0; 
	          // 'OKAY' response
	        end   
	      else if (axi_rvalid && s_axi_rready)
	        begin
	          axi_rvalid <= 1'b0;
	          
	        end  
	    end         
	end
	
	always @(s_axis_rc_tdata, axi_rvalid)
	begin
	  if (axi_rvalid) 
	    begin
	      // Read address mux
	      axi_rdata <= s_axis_rc_tdata[159:96];
	    end   
	  else
	    begin
	      axi_rdata <= 64'd0;
	    end       
	end
*/
endmodule

`resetall