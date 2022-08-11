`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date: 11/03/2013 02:04:30 PM
// Design Name: 
// Module Name: axi_read_controller
// Project Name: 
// Target Devices: 
// Tool Versions: 
// Description: 
// 
// Dependencies: 
// 
// Revision:
// Revision 0.01 - File Created
// Additional Comments:
// 
//////////////////////////////////////////////////////////////////////////////////


module axi_read_controller # (
  parameter TCQ                        = 1,
  parameter M_AXI_TDATA_WIDTH          = 64,
  parameter M_AXI_ADDR_WIDTH           = 49,
  parameter OUTSTANDING_READS          = 5,
  parameter  BAR0AXI                   = 64'h00000000,
  parameter  BAR1AXI                   = 64'h00000000,
  parameter  BAR2AXI                   = 64'h00000000,
  parameter  BAR3AXI                   = 64'h00000000,
  parameter  BAR4AXI                   = 64'h00000000,
  parameter  BAR5AXI                   = 64'h00000000, 
  parameter  BAR0SIZE                  = 12,
  parameter  BAR1SIZE                  = 12,
  parameter  BAR2SIZE                  = 12,
  parameter  BAR3SIZE                  = 12,
  parameter  BAR4SIZE                  = 12,
  parameter  BAR5SIZE                  = 12
  
) (

  input                                 m_axi_aclk,
  input                                 m_axi_aresetn,
  
  output [M_AXI_TDATA_WIDTH-1 : 0]      m_axi_araddr,
  output [2 : 0]                        m_axi_arprot,
  output                                m_axi_arvalid,
  input                                 m_axi_arready,
 
  input  [M_AXI_TDATA_WIDTH-1 : 0]      m_axi_rdata,
  input  [1 : 0]                        m_axi_rresp,
  input                                 m_axi_rvalid,
  output                                m_axi_rready,
  
  //Memory Request TLP Info
  input                                 mem_req_valid,
  output                                mem_req_ready,
  input [2:0]                           mem_req_bar_hit,
  input [31:0]                          mem_req_pcie_address,
  input [3:0]                           mem_req_byte_enable,
  input                                 mem_req_write_readn,
  input                                 mem_req_phys_func,
  input [31:0]                          mem_req_write_data,
  
  //Completion Data Coming back
  output                                axi_cpld_valid,
  input                                 axi_cpld_ready,
  output [63:0]                         axi_cpld_data 

    );
    
  reg [31:0] m_axi_addr_c;   
  reg [31:0] m_axi_araddr_r;   
  reg        m_axi_arvalid_r;   
  reg        mem_req_ready_r;    
    
  localparam IDLE      = 4'b0001;
  localparam READ_REQ = 4'b0010;
  //localparam <state3> = 4'b0100;
  //localparam <state4> = 4'b1000;
 
  reg [3:0] aximm_ar_sm = IDLE;
  reg [3:0] aximm_rd_sm = IDLE;
 
  always @(posedge m_axi_aclk)
     if ( !m_axi_aresetn ) begin
        aximm_ar_sm <= IDLE;
        m_axi_araddr_r    <= #TCQ 32'd0;
        m_axi_arvalid_r   <= #TCQ 1'b0;
        mem_req_ready_r   <= #TCQ 1'b0;
     end else
        case (aximm_ar_sm)
           IDLE : begin
              if ( mem_req_valid & !mem_req_write_readn ) begin
                aximm_ar_sm     <= #TCQ READ_REQ;
                m_axi_araddr_r    <= #TCQ m_axi_addr_c;
                m_axi_arvalid_r <= #TCQ 1'b1;
                mem_req_ready_r <= #TCQ 1'b0;
              end else begin
                m_axi_arvalid_r <= #TCQ 1'b0;
                mem_req_ready_r <= #TCQ 1'b1;
              end
           end
           READ_REQ : begin
              if (m_axi_arready) begin
                aximm_ar_sm   <= #TCQ IDLE;
                m_axi_arvalid_r <= #TCQ 1'b0;
                mem_req_ready_r <= #TCQ 1'b1;
              end 
           end
           default : begin  // Fault Recovery
              aximm_ar_sm <= #TCQ IDLE;
           end   
        endcase
        
 
  assign m_axi_arvalid  = m_axi_arvalid_r;
  assign m_axi_arprot   = 0;
  assign m_axi_araddr   = m_axi_araddr_r;
  assign mem_req_ready  = mem_req_ready_r;
  
  assign axi_cpld_valid  = m_axi_rvalid;
  assign m_axi_rready    = axi_cpld_ready;
  assign axi_cpld_data   = m_axi_rdata;
    							
  always @(mem_req_bar_hit, mem_req_pcie_address)    
     case (mem_req_bar_hit)
        3'b000: m_axi_addr_c <= { BAR0AXI[M_AXI_ADDR_WIDTH-1:BAR0SIZE], mem_req_pcie_address[BAR0SIZE-1:2],2'b00};
        3'b001: m_axi_addr_c <= { BAR1AXI[M_AXI_ADDR_WIDTH-1:BAR1SIZE], mem_req_pcie_address[BAR1SIZE-1:2],2'b00};
        3'b010: m_axi_addr_c <= { BAR2AXI[M_AXI_ADDR_WIDTH-1:BAR2SIZE], mem_req_pcie_address[BAR2SIZE-1:2],2'b00};
        3'b011: m_axi_addr_c <= { BAR3AXI[M_AXI_ADDR_WIDTH-1:BAR3SIZE], mem_req_pcie_address[BAR3SIZE-1:2],2'b00};
        3'b100: m_axi_addr_c <= { BAR4AXI[M_AXI_ADDR_WIDTH-1:BAR4SIZE], mem_req_pcie_address[BAR4SIZE-1:2],2'b00};
        3'b101: m_axi_addr_c <= { BAR5AXI[M_AXI_ADDR_WIDTH-1:BAR5SIZE], mem_req_pcie_address[BAR5SIZE-1:2],2'b00};
        3'b110: m_axi_addr_c <= 32'd0;
        3'b111: m_axi_addr_c <= 32'd0;
     endcase

endmodule