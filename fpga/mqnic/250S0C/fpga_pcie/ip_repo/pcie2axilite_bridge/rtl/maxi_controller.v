`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date: 11/02/2013 08:41:31 PM
// Design Name: 
// Module Name: maxi_controller
// Project Name: 
// Target Devices: 
// Tool Versions: 
// Description: 
// 
// Dependencies: 
// 
// Revision:
// Revision 0.01 - File Created
// Revision 0.02 - Parameterized M_AXI_ADDR_WIDTH to the read and write controllers
// Additional Comments:
// 
//////////////////////////////////////////////////////////////////////////////////



module maxi_controller # (   
  
  parameter M_AXI_TDATA_WIDTH          = 64,
  parameter M_AXI_ADDR_WIDTH           = 49,
  parameter OUTSTANDING_READS          = 5,
  parameter RELAXED_ORDERING           = 1'b0,
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

  output [M_AXI_ADDR_WIDTH-1 : 0]       m_axi_awaddr,
  output [2 : 0]                        m_axi_awprot,
  output                                m_axi_awvalid,
  input                                 m_axi_awready,
  
  output [M_AXI_TDATA_WIDTH-1 : 0]      m_axi_wdata,
  output [M_AXI_TDATA_WIDTH/8-1 : 0]    m_axi_wstrb,
  output                                m_axi_wvalid,
  input                                 m_axi_wready,
  
  input  [1 : 0]                        m_axi_bresp,
  input                                 m_axi_bvalid,
  output                                m_axi_bready,
  
  output [M_AXI_ADDR_WIDTH-1 : 0]       m_axi_araddr,
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
  input [7:0]                           mem_req_byte_enable,
  input                                 mem_req_write_readn,
  input                                 mem_req_phys_func,
  input [63:0]                          mem_req_write_data,
  
  //Completion Data Coming back
  output                                 axi_cpld_valid,
  input                                  axi_cpld_ready,
  output [63:0]                          axi_cpld_data 
  );
  
  wire mem_req_ready_r;
  wire mem_req_ready_w;
  wire mem_req_valid_wr;
    
  generate
     if ( RELAXED_ORDERING == 1'b1 ) begin: RELAXED_ORDERING_ENABLED
        assign mem_req_ready = mem_req_write_readn ? mem_req_ready_w : mem_req_ready_r;
        assign mem_req_valid_wr = mem_req_valid ;
     end else begin: RELAXED_ORDERING_DISABLED
        assign mem_req_ready = mem_req_ready_w & mem_req_ready_r;
        assign mem_req_valid_wr = (mem_req_ready_w & mem_req_ready_r) ? mem_req_valid: 1'b0 ;         
     end       
  endgenerate  

  axi_read_controller # (
    .M_AXI_TDATA_WIDTH ( M_AXI_TDATA_WIDTH ),
	.M_AXI_ADDR_WIDTH  ( M_AXI_ADDR_WIDTH ),
    .OUTSTANDING_READS ( OUTSTANDING_READS ),
    .BAR0AXI           ( BAR0AXI ),
    .BAR1AXI           ( BAR1AXI ),  
    .BAR2AXI           ( BAR2AXI ),                
    .BAR3AXI           ( BAR3AXI ),      
    .BAR4AXI           ( BAR4AXI ),                   
    .BAR5AXI           ( BAR5AXI ),
    .BAR0SIZE          ( BAR0SIZE ),
    .BAR1SIZE          ( BAR1SIZE ),
    .BAR2SIZE          ( BAR2SIZE ),
    .BAR3SIZE          ( BAR3SIZE ),
    .BAR4SIZE          ( BAR4SIZE ),
    .BAR5SIZE          ( BAR5SIZE ) 
  ) axi_read_controller (
    .m_axi_aclk              (m_axi_aclk),   
    .m_axi_aresetn           (m_axi_aresetn),
  
    .m_axi_araddr            (m_axi_araddr),
    .m_axi_arprot            (m_axi_arprot),
    .m_axi_arvalid           (m_axi_arvalid),
    .m_axi_arready           (m_axi_arready),
            
    .m_axi_rdata             (m_axi_rdata),
    .m_axi_rresp             (m_axi_rresp), 
    .m_axi_rvalid            (m_axi_rvalid),
    .m_axi_rready            (m_axi_rready),
    
    //Memory Request TLP Info
    .mem_req_valid        ( mem_req_valid_wr ),
    .mem_req_ready        ( mem_req_ready_r ),
    .mem_req_bar_hit      ( mem_req_bar_hit ),
    .mem_req_pcie_address ( mem_req_pcie_address ),
    .mem_req_byte_enable  ( mem_req_byte_enable ),
    .mem_req_write_readn  ( mem_req_write_readn ),
    .mem_req_phys_func    ( mem_req_phys_func ),
    .mem_req_write_data   ( mem_req_write_data ),       
    
    //Completion TLP Info
    .axi_cpld_valid       ( axi_cpld_valid ),
    .axi_cpld_ready       ( axi_cpld_ready ),
    .axi_cpld_data        ( axi_cpld_data )  
  );


  axi_write_controller #(
  .M_AXI_TDATA_WIDTH ( M_AXI_TDATA_WIDTH ),
  .M_AXI_ADDR_WIDTH  ( M_AXI_ADDR_WIDTH ),
  .BAR0AXI           ( BAR0AXI ),
  .BAR1AXI           ( BAR1AXI ),  
  .BAR2AXI           ( BAR2AXI ),                
  .BAR3AXI           ( BAR3AXI ),      
  .BAR4AXI           ( BAR4AXI ),                   
  .BAR5AXI           ( BAR5AXI ),
  .BAR0SIZE          ( BAR0SIZE ),
  .BAR1SIZE          ( BAR1SIZE ),
  .BAR2SIZE          ( BAR2SIZE ),
  .BAR3SIZE          ( BAR3SIZE ),
  .BAR4SIZE          ( BAR4SIZE ),
  .BAR5SIZE          ( BAR5SIZE )  
  ) axi_write_controller (
    .m_axi_aclk     ( m_axi_aclk ),
    .m_axi_aresetn  ( m_axi_aresetn ),
    
    .m_axi_awaddr   ( m_axi_awaddr ),
    .m_axi_awprot   ( m_axi_awprot ),
    .m_axi_awvalid  ( m_axi_awvalid ),
    .m_axi_awready  ( m_axi_awready ),
    
    .m_axi_wdata    ( m_axi_wdata ),
    .m_axi_wstrb    ( m_axi_wstrb ),
    .m_axi_wvalid   ( m_axi_wvalid ),
    .m_axi_wready   ( m_axi_wready ),
    
    .m_axi_bresp    ( m_axi_bresp ),
    .m_axi_bvalid   ( m_axi_bvalid ),
    .m_axi_bready   ( m_axi_bready ),

    //Memory Request TLP Info
    .mem_req_valid        ( mem_req_valid_wr ),
    .mem_req_ready        ( mem_req_ready_w ),
    .mem_req_bar_hit      ( mem_req_bar_hit ),
    .mem_req_pcie_address ( mem_req_pcie_address ),
    .mem_req_byte_enable  ( mem_req_byte_enable ),
    .mem_req_write_readn  ( mem_req_write_readn ),
    .mem_req_phys_func    ( mem_req_phys_func ),
    .mem_req_write_data   ( mem_req_write_data )

);

  
endmodule
