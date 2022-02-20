#include "md5_bmark.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <zephyr/types.h>
#include <sys/time.h>
#include "mymalloc.h"
#include "rbtree.h"

extern int rb_tree_main();
extern int kernel_rb_main();

#define BASE_SHMEM 0x60000000
#define SIZE_SHMEM 0x3F000000

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

struct offload_struct{
	uint32_t new_request ;
	enum request_type type;
	struct argument args[8];
};

struct shared_area
{
        volatile void * write_area;
        volatile void * read_area;
};


volatile struct shared_meta const * shared_struct = BASE_SHMEM + SIZE_SHMEM - 0x100;

struct handshake
{
        uint32_t  present;
        char arch[10];
};
volatile struct shared_area rw_buf; 

void wait_for_other_core()
{

	while(*(uint32_t*)0x9f000000 != 0x1a2b3c4d )
	{
		while(0);
	}
	printf("ARM core is done with addition");
}


void * shr_addr = BASE_SHMEM;

void offload_md5_thread( offload_md5_struct *  md5_struct){


	struct offload_struct  ofld_vranlc ;
	ofld_vranlc.new_request = 0xF00F0FF0;
	void * write_pointer = (void*)((char*)rw_buf.write_area + 0x1000);
	
	ofld_vranlc.args[0].location = md5_struct ;
	
		
	ofld_vranlc.type = OFFLOAD_MD5_THREAD;
	
	memcpy((void*)rw_buf.write_area , (void*)&ofld_vranlc , sizeof(struct offload_struct) );
	printf("Copied the sturct details for md5\n");
}

void  offload_rbtree_load(int * i,struct rb_root * mytree , struct mynode ** nodes  , char * global_mtx)
{
	
	struct offload_struct  ofld_vranlc ;
        ofld_vranlc.new_request = 0xF00F0FF0;
        void * write_pointer = (void*)((char*)rw_buf.write_area + 0x1000);

        ofld_vranlc.args[0].location = i ;

	ofld_vranlc.args[1].location	= mytree;
	ofld_vranlc.args[2].location	= nodes;
	ofld_vranlc.args[3].location    = global_mtx;

	printf("Addresses are i is at %p , mytree is at %p and nodes is at %p\n",ofld_vranlc.args[0].location , ofld_vranlc.args[1].location ,  ofld_vranlc.args[2].location );
        ofld_vranlc.type = OFFLOAD_RBTREE;

        memcpy((void*)rw_buf.write_area , (void*)&ofld_vranlc , sizeof(struct offload_struct) );
        printf("Copied the sturct details for rbtree \n");
//	wait_for_other_core();	

}
int main() 
{

  printf("x86 kernel started\n");
  rw_buf.write_area = (void*)0x90000000;
  rw_buf.read_area  = (void*)0x9f000000;
  struct handshake hnsk = {
                .present = 0x1FF1F11F,
        };

        struct handshake in_hnsk;
        *((uint32_t*)rw_buf.write_area) = 0x00000000 ;
        memcpy(hnsk.arch, "x86",4);
        memcpy((void*)&in_hnsk,rw_buf.read_area,sizeof(struct shared_area));
//                printf("%c",*(char*)shr_addr);

	if(in_hnsk.present == 0xE00E0EE0)
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
                        if(other->present == 0xE00E0EE0)
                        {
                                printf("Other core of type %s conected \n",other->arch);
                                break;
                        }
                        else
                        {
                                if(l % 9000000 == 0)
                                {
                                        printf("waiting for other core\n");
                                        memcpy(rw_buf.write_area,(void*)&hnsk,sizeof(struct shared_area));
                                }
                                l++;
                        }
                }
        }

	printf("x86_kernel_ shraed memory area exiting\n");
	struct timeval tv;
  gettimeofday(&tv,(void *)0);
  printf("Seconds recorded is %d \n\n",tv.tv_sec);
  kernel_rb_main();
   wait_for_other_core();
  printf("Requesting the ARM kernel to shutdown\n");
  struct offload_struct shutdown;
  shutdown.new_request = 0xF00F0FF0;
  shutdown.type = 0xDE;
  memcpy((void*)rw_buf.write_area , (void*)&shutdown , sizeof(struct offload_struct) );
  gettimeofday(&tv,(void *)0);
  printf("Seconds recorded is %d \n",tv.tv_sec);
  printf("x86_kernel_exiting\n");
        *((uint32_t*)rw_buf.read_area) = 0x00000000 ;
      *((uint32_t*)rw_buf.write_area) = 0x00000000 ;
  return 0;
}
