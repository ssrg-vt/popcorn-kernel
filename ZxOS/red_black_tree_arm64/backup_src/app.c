#include "md5_bmark.h"
#include <zephyr.h>
#include <drivers/virtualization/ivshmem.h>
#include <stdio.h>
#include <pthread.h>
#include "rbtree.h"

extern int *  next_buf ;

#define BASE_SHMEM 0x60000000
#define SIZE_SHMEM 0x3F000000

extern int kernel_rb_main(int * counter , void * mytree_in , struct mynode ** mn_in , bool * my_mutex);

#define STACKSIZ ((8192 *2) + CONFIG_TEST_EXTRA_STACKSIZE)

K_THREAD_STACK_ARRAY_DEFINE(blasch_stacks, 1 , STACKSIZ);

extern int bs_thread(void *tid_ptr);

void *  shr_addr = BASE_SHMEM;

#define MAX_THREADS 128
extern int _M4_threadsTableAllocated[MAX_THREADS];
enum request_type{
        COMPUTE_VRANLC,
        COMPUTE_RANDLC,
        COMPUTE_LOG,
	RESPOND_VRANLC,
	BLACKSCHOLES_REQ,
	OFFLOAD_MD5_THREAD,
	OFFLOAD_RBTREE

};
enum arg_type {
        CHAR,
        CHAR_P,
        DOUBLE,
        DOUBLE_P,
	INT

};
struct argument {
        uint32_t * location;
        uint32_t size;
        enum arg_type type;
};

struct shared_meta
{
        uint32_t count;
        uint32_t my_malloc_addr_curr;

};

volatile struct shared_meta const * shared_struct = (char*)BASE_SHMEM + 0x200000 - 0x100;

struct offload_struct{
        uint32_t new_request ;
	enum request_type type;
        struct argument args[8];
};
struct handshake
{
        uint32_t present;
        char arch[10];
};
struct shared_area
{
        volatile void * write_area;
        volatile void * read_area;
};
  typedef struct {
      unsigned int mantissa_low:32;     
      unsigned int mantissa_high:20;
      unsigned int exponent:11;        
      unsigned int sign:1;
    } tDoubleStruct;
int tid;





void main()
{
	printf("arm64 app started\n");
	volatile struct shared_area rw_buf = {
                .write_area = (void*)0x9f000000,
                .read_area  = (void*)0x90000000
        };
	*((uint32_t*)rw_buf.write_area) = 0x00000000 ;
	struct handshake hnsk = {
                .present = 0xE00E0EE0,
        };
	struct handshake in_hnsk;
	
	memcpy(hnsk.arch, "ARM",4);
	memcpy((void*)&in_hnsk,rw_buf.read_area,sizeof(struct shared_area)); 	
	if(in_hnsk.present  == 0x1FF1F11F)
        {
        	printf("Already other core %s present in memory.\n",in_hnsk.arch);
		memcpy(rw_buf.write_area,(void*)&hnsk,sizeof(struct shared_area));
	}
	else{
                printf("Attempting connection with other core ::\n");
                 static uint64_t l = 0;
		while(1)
                {
                        volatile struct handshake * other = (struct handshake*)rw_buf.read_area;
			if(other->present == 0x1FF1F11F)
			{
                                printf("Other core of type x86 now conected \n");
                        	break;
			}
			else
			{
				if(l % 9000000 == 0){
                                        printf("waiting for other core\n");
                        		memcpy((void*)rw_buf.write_area,(void*)&hnsk,sizeof(struct shared_area));
				}
                                l++;
			}
                }
        }
//	md5_main();	
	while(1)
	{
		struct offload_struct *  inp = (struct offload_struct *)rw_buf.read_area ;
		if(inp->new_request == 0xf00f0ff0)
		{
		if (inp->type == 0xDE)
		  {
			printf("x86 wants to go away , I am leaving too\n");
			break;
		  }

		else if(inp->type == 9)	{	
		   }
		  else if(inp->type == OFFLOAD_RBTREE)	{	
			printf("A request of type OFFLOAD_RBTREE came\n");
			//do the work here
		
			*(uint32_t*)0x9f000000 = 0x1a2b3c4d;
				
			//indicate that you have consumed the message
			*(uint32_t*)inp = ~(0xF00F0FF0);
			
			//Start replying
			int *  ij = (int *) inp->args[0].location;
			struct rb_root* treeroot = (struct rb_root*)inp->args[1].location;
			struct mynode ** mn =( struct mynode **)inp->args[2].location;		
			bool * my_mutex = (bool *) inp->args[3].location; 
			kernel_rb_main(ij , treeroot , mn , my_mutex);
			

				
			void * write_pointer = (void*)((char*)rw_buf.write_area + 0x1000);
			


        		//memcpy((void*)rw_buf.write_area , (void*)&ofld_vranlc , sizeof(struct offload_struct) );				
		   }
		   
	     }
	}
	tid = 1;
	printf("ARM_kernel_exiting\n");
     	*((uint32_t*)rw_buf.read_area) = 0x00000000 ;
     	*((uint32_t*)rw_buf.write_area) = 0x00000000 ;
	while(1);
}
