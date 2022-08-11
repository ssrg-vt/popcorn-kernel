`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date: 11/02/2013 07:11:31 PM
// Design Name: 
// Module Name: pcie_2_axilite
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


module pcie_2_axilite # (

 parameter  AXIS_TDATA_WIDTH            = 256, 
   
 parameter  S_AXI_TDATA_WIDTH           = 32,   
 parameter  S_AXI_ADDR_WIDTH            = 5,  
              
 parameter  M_AXI_TDATA_WIDTH           = 64,
 parameter  M_AXI_ADDR_WIDTH            = 49,
 
 parameter  RELAXED_ORDERING            = 1'b0,
 parameter  ENABLE_CONFIG               = 1'b0,
 
 parameter  BAR2AXI0_TRANSLATION        = 64'h0000000060000000,
 parameter  BAR2AXI1_TRANSLATION        = 64'h0000000050000000,
 parameter  BAR2AXI2_TRANSLATION        = 64'h0000000040000000,
 parameter  BAR2AXI3_TRANSLATION        = 64'h0000000030000000,
 parameter  BAR2AXI4_TRANSLATION        = 64'h0000000020000000,
 parameter  BAR2AXI5_TRANSLATION        = 64'h0000000010000000,
 parameter  BAR0SIZE                    = 64'hFFFF_FFFF_FFFF_FF80,
 parameter  BAR1SIZE                    = 64'hFFFF_FFFF_FFFF_FF80,
 parameter  BAR2SIZE                    = 64'hFFFF_FFFF_FFFF_FF80,
 parameter  BAR3SIZE                    = 64'hFFFF_FFFF_FFFF_FF80,
 parameter  BAR4SIZE                    = 64'hFFFF_FFFF_FFFF_FF80,
 parameter  BAR5SIZE                    = 64'hFFFF_FFFF_FFFF_FF80,
 parameter  OUTSTANDING_READS           = 5 
 
 ) (

  input                                 axi_clk,
  input                                 axi_aresetn,  

  input  [AXIS_TDATA_WIDTH-1:0]         s_axis_cq_tdata,
  input  [84:0]                         s_axis_cq_tuser,
  input                                 s_axis_cq_tlast,
  input  [AXIS_TDATA_WIDTH/32-1:0]      s_axis_cq_tkeep,
  input                                 s_axis_cq_tvalid,
  output   [21:0]                       s_axis_cq_tready,

  output  [2*AXIS_TDATA_WIDTH-1:0]        m_axis_cc_tdata,
  output  [80:0]                        m_axis_cc_tuser,
  output                                m_axis_cc_tlast,
  output  [8*AXIS_TDATA_WIDTH/32-1:0]   m_axis_cc_tkeep,
  output                                m_axis_cc_tvalid,
  input   [3:0]                         m_axis_cc_tready,
 
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

  input [S_AXI_ADDR_WIDTH-1 : 0]        s_axi_awaddr,
  input [2 : 0]                         s_axi_awprot,
  input                                 s_axi_awvalid,
  output                                s_axi_awready,
   
  input [ S_AXI_TDATA_WIDTH-1 : 0]       s_axi_wdata,
  input [ (S_AXI_TDATA_WIDTH/8)-1 : 0]   s_axi_wstrb,
  input                                  s_axi_wvalid,
  output                                 s_axi_wready,
   
  output [1 : 0]                         s_axi_bresp,
  output                                 s_axi_bvalid,
  input                                  s_axi_bready,
  
  input [ S_AXI_ADDR_WIDTH-1 : 0]        s_axi_araddr,
  input [2 : 0]                          s_axi_arprot,
  input                                  s_axi_arvalid,
  output                                 s_axi_arready, 
   
  output [ S_AXI_TDATA_WIDTH-1 : 0]      s_axi_rdata,
  output [1 : 0]                         s_axi_rresp,
  output                                 s_axi_rvalid,
  input                                  s_axi_rready, 
  
  output [511:0]                        cc_tdata_out,
  output wire                           zynq_cc_actv
  
  );
 
  //localparam  OUTSTANDING_READS           = 5;
  
  localparam BAR0SIZE_INT                 = get_size( BAR0SIZE );
  localparam BAR1SIZE_INT                 = get_size( BAR1SIZE );
  localparam BAR2SIZE_INT                 = get_size( BAR2SIZE );
  localparam BAR3SIZE_INT                 = get_size( BAR3SIZE );
  localparam BAR4SIZE_INT                 = get_size( BAR4SIZE );
  localparam BAR5SIZE_INT                 = get_size( BAR5SIZE );
  
//Wire Declarations

//TLP Information to AXI 
wire        mem_req_valid;
wire        mem_req_ready;
wire [2:0]  mem_req_bar_hit;
wire [48:0] mem_req_pcie_address;
wire [7:0]  mem_req_byte_enable;
wire        mem_req_write_readn;
wire        mem_req_phys_func;
wire [63:0] mem_req_write_data;//
 
wire        tag_mang_write_en;              
 
wire [2:0]  tag_mang_tc_wr;   
wire [2:0]  tag_mang_attr_wr;   
wire [15:0] tag_mang_requester_id_wr;   
wire [6:0]  tag_mang_lower_addr_wr;     
wire        tag_mang_completer_func_wr; 
wire [7:0]  tag_mang_tag_wr;            
wire [3:0]  tag_mang_first_be_wr; 

wire        tag_mang_read_en;

wire [2:0]  tag_mang_tc_rd;   
wire [2:0]  tag_mang_attr_rd;   
wire [15:0] tag_mang_requester_id_rd;   
wire [6:0]  tag_mang_lower_addr_rd;     
wire        tag_mang_completer_func_rd; 
wire [7:0]  tag_mang_tag_rd;            
wire [7:0]  tag_mang_first_be_rd; 

wire        axi_cpld_valid;
wire        axi_cpld_ready;
wire [63:0] axi_cpld_data;

  wire               completion_ur_req;
  wire [7:0]         completion_ur_tag;
  wire [6:0]         completion_ur_lower_addr;
  wire [7:0]         completion_ur_first_be;
  wire [15:0]        completion_ur_requester_id;
  wire [2:0]         completion_ur_tc;
  wire [2:0]         completion_ur_attr;
  wire               completion_ur_done; 
  
  
  localparam         STAGES       = 3;
  (* ASYNC_REG="TRUE" *) 
  reg [STAGES-1:0]   axi_sreset   = {STAGES{1'b0}};
  wire               axi_sresetn;
  

  always @(posedge axi_clk or negedge axi_aresetn) begin
    if ( !axi_aresetn ) begin
      axi_sreset <= {STAGES{1'b1}};
    end else begin
      axi_sreset <= {axi_sreset[STAGES-2:0], 1'b0};
    end
  end 
  
  assign axi_sresetn = ~axi_sreset[STAGES-1];           
  
  assign cc_tdata_out = m_axis_cc_tdata;
  assign zynq_cc_actv = m_axis_cc_tvalid;
  
  saxis_controller # ( 
  	.S_AXIS_TDATA_WIDTH( AXIS_TDATA_WIDTH )
  ) saxis_controller ( 
    
    .axis_clk                            ( axi_clk ),
    .axis_aresetn                        ( axi_sresetn ),
   
    .s_axis_cq_tdata                     ( s_axis_cq_tdata ),
    .s_axis_cq_tuser                     ( s_axis_cq_tuser ),
    .s_axis_cq_tlast                     ( s_axis_cq_tlast ),
    .s_axis_cq_tkeep                     ( s_axis_cq_tkeep ),
    .s_axis_cq_tvalid                    ( s_axis_cq_tvalid ),
    .s_axis_cq_tready                    ( s_axis_cq_tready ),
    
    //TLP Information to AXI 
    .mem_req_valid                       ( mem_req_valid ),
    .mem_req_ready                       ( mem_req_ready ),
    .mem_req_bar_hit                     ( mem_req_bar_hit ),
    .mem_req_pcie_address                ( mem_req_pcie_address ),
    .mem_req_byte_enable                 ( mem_req_byte_enable ),
    .mem_req_write_readn                 ( mem_req_write_readn ),
    .mem_req_phys_func                   ( mem_req_phys_func ),
    .mem_req_write_data                  ( mem_req_write_data ),
    
    //Memory Reads Records
    .tag_mang_write_en                   ( tag_mang_write_en ),                  
           
    .tag_mang_tc_wr                      ( tag_mang_tc_wr ),   //[15:0]
    .tag_mang_attr_wr                    ( tag_mang_attr_wr ),   //[15:0]
    .tag_mang_requester_id_wr            ( tag_mang_requester_id_wr ),   //[15:0]
    .tag_mang_lower_addr_wr              ( tag_mang_lower_addr_wr ),     //[6:0]
    .tag_mang_completer_func_wr          ( tag_mang_completer_func_wr ), //[0:0]
    .tag_mang_tag_wr                     ( tag_mang_tag_wr ),            //[7:0]
    .tag_mang_first_be_wr                ( tag_mang_first_be_wr ),      //[2:0]
    
    .completion_ur_req                   ( completion_ur_req),
    .completion_ur_tag                   ( completion_ur_tag),
    .completion_ur_lower_addr            ( completion_ur_lower_addr),
    .completion_ur_first_be              ( completion_ur_first_be),
    .completion_ur_requester_id          ( completion_ur_requester_id),
    .completion_ur_tc                    ( completion_ur_tc),
    .completion_ur_attr                  ( completion_ur_attr),
    .completion_ur_done                  ( completion_ur_done) 
            
);

  maxis_controller # ( 
    .M_AXIS_TDATA_WIDTH    ( AXIS_TDATA_WIDTH )
  	)  maxis_controller ( 
    
    .axis_clk                      ( axi_clk ),
    .axis_aresetn                  ( axi_sresetn ),
      
    .m_axis_cc_tdata               ( m_axis_cc_tdata ),
    .m_axis_cc_tuser               ( m_axis_cc_tuser ),
    .m_axis_cc_tlast               ( m_axis_cc_tlast ),
    .m_axis_cc_tkeep               ( m_axis_cc_tkeep ),
    .m_axis_cc_tvalid              ( m_axis_cc_tvalid ),
    .m_axis_cc_tready              ( m_axis_cc_tready ),
       
    .axi_cpld_valid                ( axi_cpld_valid ),
    .axi_cpld_ready                ( axi_cpld_ready ),
    .axi_cpld_data                 ( axi_cpld_data ),
    
    .tag_mang_read_en              ( tag_mang_read_en ),          
         
    .tag_mang_tc_rd                ( tag_mang_tc_rd ),   
    .tag_mang_attr_rd              ( tag_mang_attr_rd ),  
    .tag_mang_requester_id_rd      ( tag_mang_requester_id_rd ),  
    .tag_mang_lower_addr_rd        ( tag_mang_lower_addr_rd ),    
    .tag_mang_completer_func_rd    ( tag_mang_completer_func_rd ), 
    .tag_mang_tag_rd               ( tag_mang_tag_rd ),           
    .tag_mang_first_be_rd          ( tag_mang_first_be_rd ),
    
    .completion_ur_req             ( completion_ur_req),
    .completion_ur_tag             ( completion_ur_tag),
    .completion_ur_lower_addr      ( completion_ur_lower_addr),
    .completion_ur_first_be        ( completion_ur_first_be),
    .completion_ur_requester_id    ( completion_ur_requester_id),
    .completion_ur_tc              ( completion_ur_tc),
    .completion_ur_attr            ( completion_ur_attr),
    .completion_ur_done            ( completion_ur_done)    
        
    
    );

  maxi_controller # (   
    .M_AXI_TDATA_WIDTH  ( M_AXI_TDATA_WIDTH ),
    .M_AXI_ADDR_WIDTH   ( M_AXI_ADDR_WIDTH ),
    .RELAXED_ORDERING   ( RELAXED_ORDERING ),
    .BAR0AXI            ( BAR2AXI0_TRANSLATION ),
    .BAR1AXI            ( BAR2AXI1_TRANSLATION ),  
    .BAR2AXI            ( BAR2AXI2_TRANSLATION ),                
    .BAR3AXI            ( BAR2AXI3_TRANSLATION ),      
    .BAR4AXI            ( BAR2AXI4_TRANSLATION ),                   
    .BAR5AXI            ( BAR2AXI5_TRANSLATION ),
    .BAR0SIZE           ( BAR0SIZE_INT ),
    .BAR1SIZE           ( BAR1SIZE_INT ),
    .BAR2SIZE           ( BAR2SIZE_INT ),
    .BAR3SIZE           ( BAR3SIZE_INT ),
    .BAR4SIZE           ( BAR4SIZE_INT ),
    .BAR5SIZE           ( BAR5SIZE_INT )              		
  	)  maxi_controller ( 
                    
    .m_axi_aclk     ( axi_clk ),
    .m_axi_aresetn  ( axi_sresetn ),
                    
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
                    
    .m_axi_araddr   ( m_axi_araddr ),
    .m_axi_arprot   ( m_axi_arprot ),
    .m_axi_arvalid  ( m_axi_arvalid ),
    .m_axi_arready  ( m_axi_arready ),
                    
    .m_axi_rdata    ( m_axi_rdata ),
    .m_axi_rresp    ( m_axi_rresp ),
    .m_axi_rvalid   ( m_axi_rvalid ),
    .m_axi_rready   ( m_axi_rready ),
    
    //Memory Request TLP Info
    .mem_req_valid        ( mem_req_valid ),
    .mem_req_ready        ( mem_req_ready ),
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


// Manages Outstanding Read Transactions    
  tag_manager # (
    .RAM_ADDR_BITS                 ( OUTSTANDING_READS )
  ) tag_manager (
  
    .clk                           ( axi_clk ),
    .reset_n                       ( axi_sresetn ),
    
    .tag_mang_write_en             ( tag_mang_write_en ),
    
    .tag_mang_tc_wr                ( tag_mang_tc_wr ),  
    .tag_mang_attr_wr              ( tag_mang_attr_wr ),   
    .tag_mang_requester_id_wr      ( tag_mang_requester_id_wr ),   //[15:0]
    .tag_mang_lower_addr_wr        ( tag_mang_lower_addr_wr ),     //[6:0]
    .tag_mang_completer_func_wr    ( tag_mang_completer_func_wr ), //[0:0]
    .tag_mang_tag_wr               ( tag_mang_tag_wr ),            //[7:0]
    .tag_mang_first_be_wr          ( tag_mang_first_be_wr ),     //[2:0]
       
    .tag_mang_read_en              ( tag_mang_read_en ),          //[?:0]  
         
    .tag_mang_tc_rd                ( tag_mang_tc_rd ),            
    .tag_mang_attr_rd              ( tag_mang_attr_rd ),         
    .tag_mang_requester_id_rd      ( tag_mang_requester_id_rd ),   //[15:0]
    .tag_mang_lower_addr_rd        ( tag_mang_lower_addr_rd ),     //[6:0]
    .tag_mang_completer_func_rd    ( tag_mang_completer_func_rd ), //[0:0]
    .tag_mang_tag_rd               ( tag_mang_tag_rd ),            //[7:0]
    .tag_mang_first_be_rd          ( tag_mang_first_be_rd )      //[2:0]
  
  );    


   generate
      if ( ENABLE_CONFIG == 1'b1 ) begin: ENABLE_CONFIG_STATUS
        s_axi_config  s_axi_config (
          .s_axi_clk                     ( axi_clk ),
          .s_axi_resetn                  ( axi_sresetn),
          .s_axi_awaddr                  ( s_axi_awaddr),
          .s_axi_awprot                  ( s_axi_awprot),
          .s_axi_awvalid                 ( s_axi_awvalid),
          .s_axi_awready                 ( s_axi_awready), 
          .s_axi_wdata                   ( s_axi_wdata),
          .s_axi_wstrb                   ( s_axi_wstrb),
          .s_axi_wvalid                  ( s_axi_wvalid),
          .s_axi_wready                  ( s_axi_wready),
          .s_axi_bresp                   ( s_axi_bresp),
          .s_axi_bvalid                  ( s_axi_bvalid),
          .s_axi_bready                  ( s_axi_bready),
          .s_axi_araddr                  ( s_axi_araddr),
          .s_axi_arprot                  ( s_axi_arprot),
          .s_axi_arvalid                 ( s_axi_arvalid),
          .s_axi_arready                 ( s_axi_arready),
          .s_axi_rdata                   ( s_axi_rdata),
          .s_axi_rresp                   ( s_axi_rresp),
          .s_axi_rvalid                  ( s_axi_rvalid),
          .s_axi_rready                  ( s_axi_rready)
        ); 

      end else begin: NO_CONFIG
  
        assign s_axi_awready = 0; 
        assign s_axi_wready  = 0;         
        assign s_axi_bresp   = 0;
        assign s_axi_bvalid  = 0;        
        assign s_axi_arready = 0;         
        assign s_axi_rdata   = 0;
        assign s_axi_rresp   = 0;
        assign s_axi_rvalid  = 0;

      end
   endgenerate
			
  function integer get_size ( input [63:0] size );
    integer ii; 
    for ( ii = 5; ii <= 63; ii= ii + 1) begin
       if ( (size[ii] == 1'b1) & (size[ii-1] == 1'b0)) begin 
         get_size = ii; 
       end
    end      
  endfunction		

  
  
endmodule
