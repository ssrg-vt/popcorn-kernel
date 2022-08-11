`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date: 11/02/2013 08:41:31 PM
// Design Name: 
// Module Name: maxis_controller
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


module maxis_controller # (
  parameter TCQ                         = 1,   
  parameter M_AXIS_TDATA_WIDTH          = 256,
  parameter OUTSTANDING_READS           = 5 
 ) (
 
  input                                 axis_clk,
  input                                 axis_aresetn,
  
  output  [2*M_AXIS_TDATA_WIDTH-1:0]    m_axis_cc_tdata, //512
  output  [80:0]                        m_axis_cc_tuser,
  output                                m_axis_cc_tlast,
  output  [8*M_AXIS_TDATA_WIDTH/32-1:0] m_axis_cc_tkeep, 
  output                                m_axis_cc_tvalid,
  input   [3:0]                         m_axis_cc_tready,

  input                                 axi_cpld_valid,
  output                                axi_cpld_ready,
  input [63:0]                          axi_cpld_data, 
                             
  output                                tag_mang_read_en,         
                                  
  input [2:0]                           tag_mang_tc_rd,  
  input [2:0]                           tag_mang_attr_rd,  
  input [15:0]                          tag_mang_requester_id_rd,  
  input [6:0]                           tag_mang_lower_addr_rd,    
  input                                 tag_mang_completer_func_rd, 
  input [7:0]                           tag_mang_tag_rd,            
  input [3:0]                           tag_mang_first_be_rd,
  
  input                                 completion_ur_req,
  input [7:0]                           completion_ur_tag,
  input [6:0]                           completion_ur_lower_addr,
  input [3:0]                           completion_ur_first_be,
  input [15:0]                          completion_ur_requester_id,
  input [2:0]                           completion_ur_tc,
  input [2:0]                           completion_ur_attr,
  output reg                            completion_ur_done
  
  );
  
  localparam IDLE             = 5'b00001;
  localparam TLP_BEAT1_64     = 5'b00010;
  localparam TLP_BEAT1_128    = 5'b00010;
  localparam TLP_BEAT1_256    = 5'b00010;
  localparam TLP_BEAT2_64     = 5'b00100; 
  localparam TLP_BEAT1_UR_64  = 5'b01000; 
  localparam TLP_BEAT1_UR_128 = 5'b01000; 
  localparam TLP_BEAT1_UR_256 = 5'b01000; 
  localparam TLP_BEAT2_UR_64  = 5'b10000; 
  
  reg [4:0] maxis_sm;
  
  reg [2:0]                      tag_mang_byte_count;
  reg [2:0]                      completion_ur_byte_count;
  reg [OUTSTANDING_READS-1:0]    tag_mang_read_id_r;
  reg                            axi_cpld_ready_r;
  reg                            tag_mang_read_en_r;
  reg [63:0]                     axi_cpld_data_r; //31->63

  reg                               m_axis_cc_tvalid_r;
  reg                               m_axis_cc_tlast_r;
  reg [M_AXIS_TDATA_WIDTH-1:0]      m_axis_cc_tdata_r;
  reg [M_AXIS_TDATA_WIDTH/32-1:0]   m_axis_cc_tkeep_r;
  
  wire [31:0] dw1_header_32;
  wire [31:0] dw2_header_32;
  wire [31:0] dw3_header_32;
  wire [31:0] dw1_header_32_ur;
  wire [31:0] dw2_header_32_ur;
  wire [31:0] dw3_header_32_ur;
  
  generate
    /* if ( M_AXIS_TDATA_WIDTH == 64 ) begin: M_AXIS_TDATA_WIDTH_64  
  
  always @(posedge axis_clk)
     if (!axis_aresetn) begin
        maxis_sm            <= #TCQ IDLE; 
        axi_cpld_ready_r    <= #TCQ 1'b0;        
        m_axis_cc_tvalid_r  <= #TCQ 1'b0;        
        m_axis_cc_tlast_r   <= #TCQ 1'b0; 
        m_axis_cc_tkeep_r   <= #TCQ 2'h3; 
        completion_ur_done <=  #TCQ 1'b0;      
     end
     else
        case (maxis_sm)
           IDLE : begin
             if ( axi_cpld_valid  ) begin
               axi_cpld_ready_r <= #TCQ 1'b0; 
               maxis_sm         <= #TCQ TLP_BEAT1_64;              
             end else if ( completion_ur_req & ~completion_ur_done ) begin
               maxis_sm         <= #TCQ TLP_BEAT1_UR_64;
               axi_cpld_ready_r <= #TCQ 1'b0;
             end else begin
               axi_cpld_ready_r   <= #TCQ 1'b1;
             end            
             m_axis_cc_tvalid_r <= #TCQ 1'b0;
             completion_ur_done <= #TCQ 1'b0;
             m_axis_cc_tkeep_r  <= #TCQ 2'h3;
           end
           TLP_BEAT1_64 : begin
             if ( (m_axis_cc_tready[0] == 1'b1 ) & m_axis_cc_tvalid ) begin
               maxis_sm           <= #TCQ TLP_BEAT2_64;
               m_axis_cc_tdata_r  <= #TCQ { axi_cpld_data_r, dw3_header_32 };
               m_axis_cc_tlast_r  <= #TCQ 1'b1;
             end else begin 
               m_axis_cc_tdata_r  <= #TCQ { dw2_header_32, dw1_header_32 }; 
             end
             m_axis_cc_tvalid_r <= #TCQ 1'b1;                         
           end
           TLP_BEAT2_64 : begin
             if ( (m_axis_cc_tready[0] == 1'b1 ) & m_axis_cc_tvalid & !completion_ur_req ) begin
               maxis_sm           <= #TCQ IDLE; 
               m_axis_cc_tvalid_r <= #TCQ 1'b0;  
               m_axis_cc_tlast_r  <= #TCQ 1'b0; 
               axi_cpld_ready_r   <= #TCQ 1'b1;           
             end else if ( (m_axis_cc_tready[0] == 1'b1 ) & m_axis_cc_tvalid & completion_ur_req ) begin
               maxis_sm           <= #TCQ TLP_BEAT1_UR_64;
               axi_cpld_ready_r   <= #TCQ 1'b0;
               m_axis_cc_tvalid_r <= #TCQ 1'b0;  
               m_axis_cc_tlast_r  <= #TCQ 1'b0;                
             end
             m_axis_cc_tdata_r  <= #TCQ { axi_cpld_data_r, dw3_header_32 }; 
           end
           TLP_BEAT1_UR_64 : begin
             if ( (m_axis_cc_tready[0] == 1'b1 ) & m_axis_cc_tvalid ) begin
               maxis_sm           <= #TCQ TLP_BEAT2_UR_64;
               m_axis_cc_tdata_r  <= #TCQ { 32'd0, dw3_header_32_ur };
               m_axis_cc_tlast_r  <= #TCQ 1'b1;
               m_axis_cc_tkeep_r  <= #TCQ 2'h1;
             end else begin 
               m_axis_cc_tdata_r  <= #TCQ { dw2_header_32_ur, dw1_header_32_ur }; 
             end
             m_axis_cc_tvalid_r   <= #TCQ 1'b1;
           end
           TLP_BEAT2_UR_64 : begin
             if ( (m_axis_cc_tready[0] == 1'b1 ) & m_axis_cc_tvalid ) begin
               maxis_sm           <= #TCQ IDLE; 
               m_axis_cc_tvalid_r <= #TCQ 1'b0;  
               m_axis_cc_tlast_r  <= #TCQ 1'b0; 
               axi_cpld_ready_r   <= #TCQ 1'b1;
               completion_ur_done <= #TCQ 1'b1; 
               m_axis_cc_tkeep_r  <= #TCQ 2'h3;          
             end else begin            
               m_axis_cc_tvalid_r <= #TCQ 1'b1; 
             end                         
           end          
           default: begin  // Fault Recovery
              maxis_sm <= #TCQ IDLE;
           end
        endcase  
        
    end else if (M_AXIS_TDATA_WIDTH == 128) begin: M_AXIS_TDATA_WIDTH_128
    
    always @(posedge axis_clk)
       if (!axis_aresetn) begin
          maxis_sm            <= #TCQ IDLE; 
          axi_cpld_ready_r    <= #TCQ 1'b0;        
          m_axis_cc_tvalid_r  <= #TCQ 1'b0;        
          m_axis_cc_tlast_r   <= #TCQ 1'b0; 
          m_axis_cc_tkeep_r   <= #TCQ 4'hF;    
          completion_ur_done <= #TCQ 1'b0;   
       end
       else
          case (maxis_sm)
             IDLE : begin
               if ( axi_cpld_valid  ) begin
                 axi_cpld_ready_r <= #TCQ 1'b0; 
                 maxis_sm         <= #TCQ TLP_BEAT1_128;   
                 m_axis_cc_tkeep_r   <= #TCQ 4'hF;           
               end else if ( completion_ur_req & ~completion_ur_done ) begin
                 axi_cpld_ready_r <= #TCQ 1'b0; 
                 maxis_sm         <= #TCQ TLP_BEAT1_UR_128;
                 m_axis_cc_tkeep_r   <= #TCQ 4'h7; //no payload for UR Completion              
               end else begin
                 axi_cpld_ready_r   <= #TCQ 1'b1;
               end            
               m_axis_cc_tvalid_r  <= #TCQ 1'b0; 
               completion_ur_done  <= #TCQ 1'b0;              
             end
             TLP_BEAT1_128 : begin
               if ( (m_axis_cc_tready[0] == 1'b1 ) & m_axis_cc_tvalid & completion_ur_req ) begin 
                 maxis_sm           <= #TCQ TLP_BEAT1_UR_128;                
                 m_axis_cc_tlast_r  <= #TCQ 1'b0;
                 m_axis_cc_tvalid_r <= #TCQ 1'b0;  
                 m_axis_cc_tkeep_r   <= #TCQ 4'h7;  //no payload for UR Completion             
               end else if ( (m_axis_cc_tready[0] == 1'b1 ) & m_axis_cc_tvalid ) begin
                 maxis_sm           <= #TCQ IDLE;                
                 m_axis_cc_tlast_r  <= #TCQ 1'b0;
                 m_axis_cc_tvalid_r <= #TCQ 1'b0; 
               end else begin 
                 m_axis_cc_tlast_r  <= #TCQ 1'b1;
                 m_axis_cc_tvalid_r <= #TCQ 1'b1;   
               end               
                 m_axis_cc_tdata_r  <= #TCQ { axi_cpld_data_r, dw3_header_32, dw2_header_32, dw1_header_32 };                       
             end
             TLP_BEAT1_UR_128 : begin
               if ( (m_axis_cc_tready[0] == 1'b1 ) & m_axis_cc_tvalid ) begin
                 maxis_sm           <= #TCQ IDLE;                
                 m_axis_cc_tlast_r  <= #TCQ 1'b0;
                 m_axis_cc_tvalid_r <= #TCQ 1'b0; 
                 completion_ur_done <=  #TCQ 1'b1;
               end else begin 
                 m_axis_cc_tlast_r  <= #TCQ 1'b1;
                 m_axis_cc_tvalid_r <= #TCQ 1'b1;   
               end               
                 m_axis_cc_tdata_r  <= #TCQ { 32'd0, dw3_header_32_ur, dw2_header_32_ur, dw1_header_32_ur };                       
             end
             default: begin  // Fault Recovery
                maxis_sm <= #TCQ IDLE;
             end
          endcase      
      
    end */
    if (M_AXIS_TDATA_WIDTH == 256) begin: M_AXIS_TDATA_WIDTH_256
     
      always @(posedge axis_clk)
         if (!axis_aresetn) begin
            maxis_sm            <= #TCQ IDLE; 
            axi_cpld_ready_r    <= #TCQ 1'b0;        
            m_axis_cc_tvalid_r  <= #TCQ 1'b0;        
            m_axis_cc_tlast_r   <= #TCQ 1'b0;        
            m_axis_cc_tkeep_r   <= #TCQ 8'h0F; 
            completion_ur_done <=  #TCQ 1'b0;      
         end
         else
            case (maxis_sm)
              IDLE : begin
                if ( axi_cpld_valid ) begin
                  axi_cpld_ready_r <= #TCQ 1'b0; 
                  maxis_sm         <= #TCQ TLP_BEAT1_256;  
                  m_axis_cc_tkeep_r   <= #TCQ 8'h1F; 
                 
                end else if ( completion_ur_req & ~completion_ur_done ) begin
                  axi_cpld_ready_r <= #TCQ 1'b0; 
                  maxis_sm         <= #TCQ TLP_BEAT1_UR_256;  
                  m_axis_cc_tkeep_r   <= #TCQ 8'h07;                                                  
                end else begin
                  axi_cpld_ready_r   <= #TCQ 1'b1;
                end            
                m_axis_cc_tvalid_r <= #TCQ 1'b0; 
                completion_ur_done  <= #TCQ 1'b0;
             end
             TLP_BEAT1_256 : begin
               if ( (m_axis_cc_tready[0] == 1'b1 ) & m_axis_cc_tvalid & completion_ur_req ) begin
                 maxis_sm           <= #TCQ TLP_BEAT1_UR_256;                
                 m_axis_cc_tlast_r  <= #TCQ 1'b0;
                 m_axis_cc_tvalid_r <= #TCQ 1'b0; 
                 m_axis_cc_tkeep_r   <= #TCQ 8'h07;               
               end else if ( (m_axis_cc_tready[0] == 1'b1 ) & m_axis_cc_tvalid ) begin
                 maxis_sm           <= #TCQ IDLE;                
                 m_axis_cc_tlast_r  <= #TCQ 1'b0;
                 m_axis_cc_tvalid_r <= #TCQ 1'b0; 
               end else begin 
                 m_axis_cc_tlast_r  <= #TCQ 1'b1;
                 m_axis_cc_tvalid_r <= #TCQ 1'b1;   
               end               
                 m_axis_cc_tdata_r  <= #TCQ { axi_cpld_data_r, dw3_header_32, dw2_header_32, dw1_header_32 };                       
             end
             TLP_BEAT1_UR_256 : begin
               if ( (m_axis_cc_tready[0] == 1'b1 ) & m_axis_cc_tvalid ) begin
                 maxis_sm           <= #TCQ IDLE;                
                 m_axis_cc_tlast_r  <= #TCQ 1'b0;
                 m_axis_cc_tvalid_r <= #TCQ 1'b0; 
                 completion_ur_done <=  #TCQ 1'b1;
               end else begin 
                 m_axis_cc_tlast_r  <= #TCQ 1'b1;
                 m_axis_cc_tvalid_r <= #TCQ 1'b1;   
               end               
                 m_axis_cc_tdata_r  <= #TCQ { 64'd0, dw3_header_32_ur, dw2_header_32_ur, dw1_header_32_ur };                       
             end
             default: begin  // Fault Recovery
                maxis_sm <= #TCQ IDLE;
             end
           endcase        
      end
  endgenerate  

  
  always @(posedge axis_clk) begin  
    if (axi_cpld_valid & axi_cpld_ready) begin
      axi_cpld_data_r      <= #TCQ axi_cpld_data;
    end 
  end
  
  assign tag_mang_read_en = (axi_cpld_valid & axi_cpld_ready) ? 1'b1 : 1'b0;
  
  always @( tag_mang_first_be_rd )    
     casex ( tag_mang_first_be_rd )
        4'b1xx1: tag_mang_byte_count <= 3'b100;
        4'b01x1: tag_mang_byte_count <= 3'b011;
        4'b1x10: tag_mang_byte_count <= 3'b011;
        4'b0011: tag_mang_byte_count <= 3'b010;
        4'b0110: tag_mang_byte_count <= 3'b010;
        4'b1100: tag_mang_byte_count <= 3'b010;
        4'b0001: tag_mang_byte_count <= 3'b001;
        4'b0010: tag_mang_byte_count <= 3'b001;
        4'b0100: tag_mang_byte_count <= 3'b001;
        4'b1000: tag_mang_byte_count <= 3'b001;
        4'b0000: tag_mang_byte_count <= 3'b001;
        default: tag_mang_byte_count <= 3'b000;
     endcase   

  always @( completion_ur_first_be )    
     casex ( completion_ur_first_be )
        4'b1xx1: completion_ur_byte_count <= 3'b100;
        4'b01x1: completion_ur_byte_count <= 3'b011;
        4'b1x10: completion_ur_byte_count <= 3'b011;
        4'b0011: completion_ur_byte_count <= 3'b010;
        4'b0110: completion_ur_byte_count <= 3'b010;
        4'b1100: completion_ur_byte_count <= 3'b010;
        4'b0001: completion_ur_byte_count <= 3'b001;
        4'b0010: completion_ur_byte_count <= 3'b001;
        4'b0100: completion_ur_byte_count <= 3'b001;
        4'b1000: completion_ur_byte_count <= 3'b001;
        4'b0000: completion_ur_byte_count <= 3'b001;
        default: completion_ur_byte_count <= 3'b000;
     endcase 
  
  assign tag_mang_read_id        = tag_mang_read_id_r;
  assign axi_cpld_ready          = axi_cpld_ready_r;
//  assign tag_mang_read_en          = tag_mang_read_en_r;
/*
  assign dw1_header_32           = { 12'd0 ,4'd8, 9'd0, tag_mang_lower_addr_rd };
  assign dw2_header_32           = { tag_mang_requester_id_rd, 16'd1 };
  assign dw3_header_32           = { 1'b0, tag_mang_attr_rd, tag_mang_tc_rd, 1'd0, 16'd0, tag_mang_tag_rd };
*/
  assign dw1_header_32           = { 12'd0 ,4'd8 , 9'd0, tag_mang_lower_addr_rd };
  assign dw2_header_32           = { tag_mang_requester_id_rd, 2'b0, 3'b000, 11'd2 };
  assign dw3_header_32           = { 1'b0, tag_mang_attr_rd, tag_mang_tc_rd, 1'd0, 16'd0, tag_mang_tag_rd };
  
  assign dw1_header_32_ur           = { 12'd0 ,4'd8, 9'd0, completion_ur_lower_addr };
  assign dw2_header_32_ur           = { completion_ur_requester_id, 5'd1 ,11'd0 }; //Sets the UR bit
  assign dw3_header_32_ur           = { 1'b0, completion_ur_attr, completion_ur_tc, 1'd0, 16'd0, completion_ur_tag };
  
  assign m_axis_cc_tuser         = 81'd0;//81'h0000000000000000041;  //
  assign m_axis_cc_tkeep         = {56'd0, m_axis_cc_tkeep_r};
  assign m_axis_cc_tlast         = m_axis_cc_tlast_r;
  assign m_axis_cc_tdata         = {256'd0, m_axis_cc_tdata_r};
  assign m_axis_cc_tvalid        = m_axis_cc_tvalid_r;
  
endmodule
