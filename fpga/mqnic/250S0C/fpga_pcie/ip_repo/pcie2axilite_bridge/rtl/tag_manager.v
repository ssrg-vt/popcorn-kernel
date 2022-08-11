`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date: 11/02/2013 10:51:56 PM
// Design Name: 
// Module Name: tag_manager
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


module tag_manager # (
  parameter TCQ           = 1,
  parameter RAM_ADDR_BITS = 5
)(
  input                     clk,
  input                     reset_n,
  
  input                     tag_mang_write_en,
  
  input [2:0]               tag_mang_tc_wr,             //[15:0]
  input [2:0]               tag_mang_attr_wr,           //[15:0]
  input [15:0]              tag_mang_requester_id_wr,   //[15:0]
  input [6:0]               tag_mang_lower_addr_wr,     //[6:0]
  input                     tag_mang_completer_func_wr, //[0:0]
  input [7:0]               tag_mang_tag_wr,            //[7:0]
  input [3:0]               tag_mang_first_be_wr,       //[2:0]
     
  input                     tag_mang_read_en,         
       
  output [2:0]              tag_mang_tc_rd,   //[15:0]
  output [2:0]              tag_mang_attr_rd,   //[15:0]
  output [15:0]             tag_mang_requester_id_rd,   //[15:0]
  output [6:0]              tag_mang_lower_addr_rd,     //[6:0]
  output                    tag_mang_completer_func_rd, //[0:0]
  output [7:0]              tag_mang_tag_rd,            //[7:0]
  output [3:0]              tag_mang_first_be_rd      //[2:0]
    );

    
  reg [RAM_ADDR_BITS-1:0] tag_mang_write_id;      
  reg [RAM_ADDR_BITS-1:0] tag_mang_read_id;        

  always @( posedge clk )
    if  ( !reset_n ) 
      tag_mang_write_id <= #TCQ 1;
    else if ( tag_mang_write_en ) 
      tag_mang_write_id <= #TCQ tag_mang_write_id + 1;   
      
  always @( posedge clk )
    if  ( !reset_n ) 
      tag_mang_read_id <= #TCQ 0;
    else if ( tag_mang_read_en ) 
      tag_mang_read_id <= #TCQ tag_mang_read_id + 1;
            
            
   localparam RAM_WIDTH = 42;

   (* RAM_STYLE="distributed" *)
   reg [RAM_WIDTH-1:0] tag_storage [(2**RAM_ADDR_BITS)-1:0];

   wire [RAM_WIDTH-1:0] completion_data;

   always @(posedge clk)
      if (tag_mang_write_en)
         tag_storage[tag_mang_write_id] <= #TCQ { tag_mang_attr_wr, tag_mang_tc_wr, tag_mang_requester_id_wr, tag_mang_lower_addr_wr, tag_mang_completer_func_wr, tag_mang_tag_wr, tag_mang_first_be_wr};

   assign completion_data = tag_storage[tag_mang_read_id];
   
   assign tag_mang_attr_rd           = completion_data[41:39];
   assign tag_mang_tc_rd             = completion_data[38:36];
   assign tag_mang_requester_id_rd   = completion_data[35:20];     //[15:0]
   assign tag_mang_lower_addr_rd     = completion_data[19:13];     //[6:0]
   assign tag_mang_completer_func_rd = completion_data[12];        //[0:0]
   assign tag_mang_tag_rd            = completion_data[11:4];      //[7:0]
   assign tag_mang_first_be_rd       = completion_data[3:0];       //[2:0]        



    
endmodule
