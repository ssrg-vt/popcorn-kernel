`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date: 11/02/2013 08:41:31 PM
// Design Name: 
// Module Name: saxis_controller
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


module saxis_controller # (
  parameter TCQ                         = 1,
  parameter S_AXIS_TDATA_WIDTH          = 64,
  parameter OUTSTANDING_READS           = 5 
 ) (
 
  input                                 axis_clk,
  input                                 axis_aresetn,
  
  input  [S_AXIS_TDATA_WIDTH-1:0]       s_axis_cq_tdata,
  input  [84:0]                         s_axis_cq_tuser,
  input                                 s_axis_cq_tlast,
  input  [S_AXIS_TDATA_WIDTH/32-1:0]    s_axis_cq_tkeep,
  input                                 s_axis_cq_tvalid,
  output [21:0]                         s_axis_cq_tready,

  //TLP Information to AXI 
  output                               mem_req_valid,
  input                                mem_req_ready,
  output [2:0]                         mem_req_bar_hit,
  output [48:0]                        mem_req_pcie_address,
  output [7:0]                         mem_req_byte_enable,
  output                               mem_req_write_readn,
  output                               mem_req_phys_func,
  output [63:0]                        mem_req_write_data,
    
  //Memory Reads Records
  output                               tag_mang_write_en,     
                                
  output [2:0]                         tag_mang_tc_wr,
  output [2:0]                         tag_mang_attr_wr,
  output [15:0]                        tag_mang_requester_id_wr,
  output [6:0]                         tag_mang_lower_addr_wr,   
  output                               tag_mang_completer_func_wr, 
  output [7:0]                         tag_mang_tag_wr,           
  output [7:0]                         tag_mang_first_be_wr, 

  output reg                           completion_ur_req,
  output [7:0]                         completion_ur_tag,
  output [6:0]                         completion_ur_lower_addr,
  output [7:0]                         completion_ur_first_be,
  output [15:0]                        completion_ur_requester_id,
  output [2:0]                         completion_ur_tc,
  output [2:0]                         completion_ur_attr,
  input                                completion_ur_done      
  
  );

   localparam IDLE                = 7'b0000001;
   localparam DW2_PROCESS_64      = 7'b0000010;
   localparam READ_PROCESS_64     = 7'b0000100;
   localparam READ_PROCESS_128    = 7'b0000010;
   localparam READ_PROCESS_256    = 7'b0000010;
   localparam WRITE_DATA_64       = 7'b0001000;
   localparam WRITE_DATA_128      = 7'b0000100;
   localparam WRITE_DATA_256      = 7'b0000100;
   localparam HOLD_VALID          = 7'b0010000;
   localparam COMPLETION_UR       = 7'b0100000;
   localparam WAIT_FOR_LAST       = 7'b1000000;

   reg [6:0] saxis_sm = IDLE;
   reg [6:0] saxis_sm_r = IDLE;
   
   reg [255:0]               s_axis_cq_tdata_wide_r;
   reg [3:0]                 first_be_r; 
   reg [3:0]                 last_be_r;
   reg                       mem_req_write_readn_r; 
   reg                       mem_read_pulse;
   reg                       s_axis_cq_tready_r;
   reg                       mem_req_valid_r;

   always @(posedge axis_clk)
      saxis_sm_r <= saxis_sm;

   generate/*
      if ( S_AXIS_TDATA_WIDTH == 64 ) begin: S_AXIS_TDATA_WIDTH_64

      always @(posedge axis_clk)
         if (!axis_aresetn) begin
            saxis_sm            <= #TCQ IDLE;
            mem_read_pulse      <= #TCQ 1'b0;
            s_axis_cq_tready_r  <= #TCQ 1'b0; 
            mem_req_valid_r     <= #TCQ 1'b0;
            completion_ur_req     <= #TCQ 1'b0;
            
         end
         else
            case (saxis_sm)
               IDLE : begin
                  if (s_axis_cq_tvalid & s_axis_cq_tready[0] ) begin
                      saxis_sm <= #TCQ DW2_PROCESS_64;
                      s_axis_cq_tdata_wide_r[63:0] <= #TCQ s_axis_cq_tdata[63:0];  
                      first_be_r <= #TCQ s_axis_cq_tuser[3:0];
                  end
                  s_axis_cq_tready_r <= #TCQ 1'b1;
                  mem_req_valid_r <= #TCQ 1'b0;
               end
               DW2_PROCESS_64 : begin
                  if (s_axis_cq_tvalid & s_axis_cq_tready[0] ) begin                    
                    if ( s_axis_cq_tdata[14:11] == 4'h0 ) begin         //If Memory Read
                      if ( s_axis_cq_tdata[10:0] == 11'd1 ) begin       // Only 1DW Reads are Supported
                        saxis_sm <= #TCQ READ_PROCESS_64;
                        mem_req_write_readn_r <= #TCQ 1'b0;
                        mem_read_pulse <= #TCQ 1'b1;
                        mem_req_valid_r <= #TCQ 1'b1;
                        s_axis_cq_tready_r <= #TCQ 1'b0;
                      end else begin // Only 1DW packets are supported.  Larger reads will get a completion with UR.
                        saxis_sm <= #TCQ COMPLETION_UR;
                        s_axis_cq_tready_r <= #TCQ 1'b0;
                      end
                    end else if ( (s_axis_cq_tdata[14:11] == 4'h1) ) begin //If Memory Write
                      if ( s_axis_cq_tdata[10:0] == 11'd1 ) begin // Only 1DW Reads are Supported
                        saxis_sm <= #TCQ WRITE_DATA_64;
                        mem_req_write_readn_r <= #TCQ 1'b1;
                        mem_req_valid_r <= #TCQ 1'b0;
                      end else begin // Only 1DW packets are supported.  Larger packets are ignored.
                        if (s_axis_cq_tlast) begin
                          saxis_sm <= #TCQ IDLE;
                        end else begin
                          saxis_sm <= #TCQ WAIT_FOR_LAST;
                        end
                      end
                    end
                    s_axis_cq_tdata_wide_r[127:64] <= #TCQ s_axis_cq_tdata[63:0]; 
                  end 
               end
               READ_PROCESS_64 : begin
                  if (mem_req_ready) begin
                    saxis_sm <= #TCQ IDLE;
                    s_axis_cq_tready_r <= #TCQ 1'b1;
                    mem_req_valid_r <= #TCQ 1'b0;
                  end else begin
                    s_axis_cq_tready_r <= #TCQ 1'b0;
                  end
                  mem_read_pulse <= #TCQ 1'b0;   
               end
               WRITE_DATA_64: begin
                  if (s_axis_cq_tvalid) begin
                    s_axis_cq_tdata_wide_r [159:128] = #TCQ s_axis_cq_tdata[31:0];
                    if (mem_req_ready) begin
                      saxis_sm <= #TCQ IDLE;
                      mem_req_valid_r <= #TCQ 1'b1;
                      s_axis_cq_tready_r <= #TCQ 1'b1; 
                    end else begin 
                      s_axis_cq_tready_r <= #TCQ 1'b0;
                      saxis_sm <= #TCQ HOLD_VALID;  
                      mem_req_valid_r <= #TCQ 1'b1;                
                    end
                  end
               end
               WAIT_FOR_LAST: begin
                 if (s_axis_cq_tlast) begin
                   saxis_sm <= #TCQ IDLE;
                 end 
               end 
               COMPLETION_UR: begin
                 if (completion_ur_done) begin
                   saxis_sm <= #TCQ IDLE;
                   completion_ur_req <= 1'b0;
                 end else begin
                   completion_ur_req <= 1'b1;
                 end
               end 
               HOLD_VALID: begin
                 if (mem_req_ready) begin
                   saxis_sm <= #TCQ IDLE;
                   mem_req_valid_r <= #TCQ 1'b0; 
                   s_axis_cq_tready_r <= #TCQ 1'b1;
                 end 
               end                             
               default: begin  // Fault Recovery
                  saxis_sm <= #TCQ IDLE;
	       end
         endcase
         
       end else if (S_AXIS_TDATA_WIDTH == 128) begin: S_AXIS_TDATA_WIDTH_128
       
       always @(posedge axis_clk)
          if (!axis_aresetn) begin
             saxis_sm            <= #TCQ IDLE;
             mem_read_pulse      <= #TCQ 1'b0;
             s_axis_cq_tready_r  <= #TCQ 1'b0; 
             mem_req_valid_r     <= #TCQ 1'b0;
             completion_ur_req   <= #TCQ 1'b0;
             
             
          end
          else
             case ( saxis_sm )
                IDLE : begin
		   
                  mem_req_valid_r               <= #TCQ 1'b0;
		   
                  if ( s_axis_cq_tvalid & s_axis_cq_tready[0] & (s_axis_cq_tdata[78:75] == 4'h0)  ) begin // If Memory Read
                    if ( s_axis_cq_tdata[10+64:0+64] == 11'd1 ) begin       // Only 1DW Reads are Supported
                      saxis_sm                      <= #TCQ READ_PROCESS_128; 
                      mem_req_write_readn_r         <= #TCQ 1'b0;
                      mem_read_pulse                <= #TCQ 1'b1;
                      mem_req_valid_r               <= #TCQ 1'b1;
                      s_axis_cq_tdata_wide_r[127:0] <= #TCQ s_axis_cq_tdata[127:0]; 
                      first_be_r                    <= #TCQ s_axis_cq_tuser[3:0];
                      s_axis_cq_tready_r            <= #TCQ 1'b0;
                    end else begin
                      s_axis_cq_tdata_wide_r[127:0] <= #TCQ s_axis_cq_tdata[127:0];
                      first_be_r                    <= #TCQ s_axis_cq_tuser[3:0];
                      saxis_sm                      <= #TCQ COMPLETION_UR;
                      s_axis_cq_tready_r            <= #TCQ 1'b0;
                    end
                  end else if (s_axis_cq_tvalid & s_axis_cq_tready[0] & (s_axis_cq_tdata[78:75] == 4'h1)) begin // If Memory Write
                    if ( s_axis_cq_tdata[10+64:0+64] == 11'd1 ) begin       // Only 1DW Writes are Supported
                      saxis_sm                      <= #TCQ WRITE_DATA_128;
                      mem_req_write_readn_r         <= #TCQ 1'b1;
                      mem_req_valid_r               <= #TCQ 1'b0; 
                      s_axis_cq_tdata_wide_r[127:0] <= #TCQ s_axis_cq_tdata[127:0]; 
                      first_be_r                    <= #TCQ s_axis_cq_tuser[3:0]; 
                      s_axis_cq_tready_r            <= #TCQ 1'b1;  
                    end else begin
                      if (s_axis_cq_tlast) begin
                        saxis_sm <= #TCQ IDLE;
                      end else begin
                        saxis_sm <= #TCQ WAIT_FOR_LAST;
                      end                     
                    end              
                  end else begin
                      s_axis_cq_tready_r            <= #TCQ 1'b1;
                  end                 
                end
                READ_PROCESS_128 : begin
                  if (mem_req_ready) begin
                    saxis_sm <= #TCQ IDLE;
                    s_axis_cq_tready_r <= #TCQ 1'b1;
                    mem_req_valid_r <= #TCQ 1'b0;
                  end else begin
                    s_axis_cq_tready_r <= #TCQ 1'b0;
                  end
                    mem_read_pulse <= #TCQ 1'b0;   
                end
                WRITE_DATA_128: begin
                  if (s_axis_cq_tvalid) begin
                    s_axis_cq_tdata_wide_r [159:128] = #TCQ s_axis_cq_tdata[31:0];
                    if (mem_req_ready) begin
                      saxis_sm <= #TCQ IDLE;
                      mem_req_valid_r    <= #TCQ 1'b1;
                      s_axis_cq_tready_r <= #TCQ 1'b1; 
                    end else begin 
                      s_axis_cq_tready_r <= #TCQ 1'b0;
                      saxis_sm <= #TCQ HOLD_VALID;  
                      mem_req_valid_r <= #TCQ 1'b1;                
                    end
                  end
                end
                WAIT_FOR_LAST: begin
                  if ( s_axis_cq_tlast ) begin
                    saxis_sm <= #TCQ IDLE;
                  end 
                end 
                COMPLETION_UR: begin
                  if ( completion_ur_done ) begin
                    saxis_sm <= #TCQ IDLE;
                    completion_ur_req <= 1'b0;
                  end else begin
                    completion_ur_req <= 1'b1;
                  end
                end 
                HOLD_VALID: begin
                  if (mem_req_ready) begin
                    saxis_sm <= #TCQ IDLE;
                    mem_req_valid_r <= #TCQ 1'b0; 
                    s_axis_cq_tready_r <= #TCQ 1'b1;
                  end 
                end 
                default: begin  // Fault Recovery
                   saxis_sm <= #TCQ IDLE;
         end
          endcase    
                  
       end */
       if (S_AXIS_TDATA_WIDTH == 256) begin: S_AXIS_TDATA_WIDTH_256 // This is going to look similar to the 128-bit interface
       
       always @(posedge axis_clk)
          if (!axis_aresetn) begin
             saxis_sm            <= #TCQ IDLE;
             mem_read_pulse      <= #TCQ 1'b0;
             s_axis_cq_tready_r  <= #TCQ 1'b0; 
             mem_req_valid_r     <= #TCQ 1'b0;
             completion_ur_req   <= #TCQ 1'b0;
             
          end
          else
             case ( saxis_sm )
                IDLE : begin
                  if ( s_axis_cq_tvalid & s_axis_cq_tready[0] & (s_axis_cq_tdata[78:75] == 4'h0) ) begin // If Memory Read
                    if ( s_axis_cq_tdata[10+64:0+64] == 11'd2 || s_axis_cq_tdata[10+64:0+64] == 11'd1) begin       // Only 1DW Reads are Supported                  
                    //if ( s_axis_cq_tdata[10+64:0+64] == 11'd1) begin
                      saxis_sm                      <= #TCQ READ_PROCESS_256; 
                      mem_req_write_readn_r         <= #TCQ 1'b0;
                      mem_read_pulse                <= #TCQ 1'b1;
                      mem_req_valid_r               <= #TCQ 1'b1;
                      s_axis_cq_tdata_wide_r[127:0] <= #TCQ s_axis_cq_tdata[127:0]; 
                      first_be_r                    <= #TCQ s_axis_cq_tuser[3:0];
                      last_be_r                     <= #TCQ s_axis_cq_tuser[7:4];
                      s_axis_cq_tready_r            <= #TCQ 1'b0;
                    end else begin
                      s_axis_cq_tdata_wide_r[127:0] <= #TCQ s_axis_cq_tdata[127:0];
                      first_be_r                    <= #TCQ s_axis_cq_tuser[3:0];
                      saxis_sm <= #TCQ COMPLETION_UR;
                      s_axis_cq_tready_r <= #TCQ 1'b0;
                    end
                  end else if (s_axis_cq_tvalid & s_axis_cq_tready[0] & (s_axis_cq_tdata[78:75] == 4'h1)) begin // If Memory Write
                    if ( s_axis_cq_tdata[10+64:0+64] == 11'd2 || s_axis_cq_tdata[10+64:0+64] == 11'd1) begin       // Only 1DW Writes are Supported
                    //if ( s_axis_cq_tdata[10+64:0+64] == 11'd1) begin
                      saxis_sm                      <= #TCQ WRITE_DATA_256;
                      mem_req_write_readn_r         <= #TCQ 1'b1;
                      mem_req_valid_r               <= #TCQ 1'b1; 
                      s_axis_cq_tdata_wide_r[191:0] <= #TCQ s_axis_cq_tdata[191:0]; //2D word
                      first_be_r                    <= #TCQ s_axis_cq_tuser[3:0]; 
                      last_be_r                     <= #TCQ s_axis_cq_tuser[11:8];
                      s_axis_cq_tready_r            <= #TCQ 1'b0;   
                    end else begin
                      if (s_axis_cq_tlast) begin
                        saxis_sm <= #TCQ IDLE;
                      end else begin
                        saxis_sm <= #TCQ WAIT_FOR_LAST;
                      end                    
                    end             
                  end else begin
                     s_axis_cq_tready_r            <= #TCQ 1'b1;
                  end                 
                end
                READ_PROCESS_256 : begin
                  if (mem_req_ready) begin
                    saxis_sm <= #TCQ IDLE;
                    s_axis_cq_tready_r <= #TCQ 1'b1;
                    mem_req_valid_r <= #TCQ 1'b0;
                  end else begin
                    s_axis_cq_tready_r <= #TCQ 1'b0;
                  end
                    mem_read_pulse <= #TCQ 1'b0;   
                end
                WRITE_DATA_256: begin
                  if (mem_req_ready) begin
                    saxis_sm <= #TCQ IDLE;
                    mem_req_valid_r <= #TCQ 1'b0;
                    s_axis_cq_tready_r <= #TCQ 1'b1; 
                  end else begin 
                    s_axis_cq_tready_r <= #TCQ 1'b0;
                    saxis_sm <= #TCQ HOLD_VALID;  
                    mem_req_valid_r <= #TCQ 1'b1;                
                  end
                end
                WAIT_FOR_LAST: begin
                  if ( s_axis_cq_tlast ) begin
                    saxis_sm <= #TCQ IDLE;
                  end 
                end 
                COMPLETION_UR: begin
                  if ( completion_ur_done ) begin
                    saxis_sm <= #TCQ IDLE;
                    completion_ur_req <= 1'b0;
                  end else begin
                    completion_ur_req <= 1'b1;
                  end
                end 
                HOLD_VALID: begin
                  if (mem_req_ready) begin
                    saxis_sm <= #TCQ IDLE;
                    mem_req_valid_r <= #TCQ 1'b0; 
                    s_axis_cq_tready_r <= #TCQ 1'b1;
                  end 
                end 
                default: begin  // Fault Recovery
                   saxis_sm <= #TCQ IDLE;
         end
          endcase             
       end
    endgenerate         
         

  assign  mem_req_valid                = mem_req_valid_r;
  
  assign  mem_req_bar_hit              = s_axis_cq_tdata_wide_r[114:112];
  assign  mem_req_pcie_address         = s_axis_cq_tdata_wide_r[48:0];//
  assign  mem_req_byte_enable          = {first_be_r, last_be_r};
  assign  mem_req_write_readn          = mem_req_write_readn_r;
  assign  mem_req_phys_func            = s_axis_cq_tdata_wide_r[64];
  assign  mem_req_write_data           = s_axis_cq_tdata_wide_r[191:128];//
    
  //Memory Reads Records
  assign   tag_mang_write_en            = mem_read_pulse;    
                                
  assign   tag_mang_tc_wr               = s_axis_cq_tdata_wide_r[123:121];
  assign   tag_mang_attr_wr             = s_axis_cq_tdata_wide_r[126:124];
  assign   tag_mang_requester_id_wr     = s_axis_cq_tdata_wide_r[95:80];
  assign   tag_mang_lower_addr_wr       = s_axis_cq_tdata_wide_r[6:0];  
  assign   tag_mang_completer_func_wr   = s_axis_cq_tdata_wide_r[104];
  assign   tag_mang_tag_wr              = s_axis_cq_tdata_wide_r[103:96];
  assign   tag_mang_first_be_wr         = {first_be_r, last_be_r};


  assign   completion_ur_tag            = s_axis_cq_tdata_wide_r[103:96];
  assign   completion_ur_lower_addr     = s_axis_cq_tdata_wide_r[6:0]; 
  assign   completion_ur_first_be       = {first_be_r, last_be_r};
  assign   completion_ur_requester_id   = s_axis_cq_tdata_wide_r[95:80];
  assign   completion_ur_tc             = s_axis_cq_tdata_wide_r[123:121];
  assign   completion_ur_attr           = s_axis_cq_tdata_wide_r[126:124];
  
  assign   s_axis_cq_tready             = {22{s_axis_cq_tready_r}};
  
endmodule
