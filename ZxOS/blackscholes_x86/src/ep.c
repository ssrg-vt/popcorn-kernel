
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "type.h"
#include <pthread.h>
#include "npbparams.h"
#include "randdp.h"
#include "timers.h"
#include "print_results.h"
#include "device.h"
#include <zephyr/types.h>
#include <sys/time.h>

#include "inc.h"

#define MAX(X,Y)  (((X) > (Y)) ? (X) : (Y))
uint32_t * linear_shmem = 0x30000000;

extern fptype *prices;

#define MK        16
#define MM        (M - MK)
#define NN        (1 << MM)
#define NK        (1 << MK)
#define NQ        10
#define EPSILON   1.0e-8
#define A         1220703125.0
#define S         271828183.0

#define DISTRIBUTION_ENABLE 1

static double x[2*NK];
static double q[NQ]; 

enum request_type{
	COMPUTE_VRANLC,
	COMPUTE_RANDLC,
	COMPUTE_LOG,
	RESPOND_VRANLC,
	BLACKSCHOLES_REQ,

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

struct handshake
{
        uint32_t  present;
        char arch[10];
};
volatile struct shared_area rw_buf; 


void bs_dist(  int tid , void * prices , void * buffer , void* buffer2){


	struct offload_struct  ofld_vranlc ;
	ofld_vranlc.new_request = 0xF00F0FF0;
	void * write_pointer = (void*)((char*)rw_buf.write_area + 0x1000);

	//these functions needs to be automated
	printf("addresses are %p , %p and %p\n",prices,buffer,buffer2);	
	*((pthread_t*)write_pointer)  = tid; 
	ofld_vranlc.args[0].location =  (uint32_t*)((char*)write_pointer + + 0x40000000);
	write_pointer =  (void*)((char*)write_pointer +  sizeof(int));
	printf("write pointer is %p\n",write_pointer);	
	*((uint32_t*)write_pointer)  = (uint32_t)prices; 
	ofld_vranlc.args[1].location =  (uint32_t*)((char*)write_pointer + + 0x40000000);
	write_pointer =  (void*)((char*)write_pointer +  sizeof(void*));

	printf("write pointer is %p\n",write_pointer);	
		
	*((uint32_t*)write_pointer)  = buffer; 
	ofld_vranlc.args[2].location =  (uint32_t*)((char*)write_pointer + + 0x40000000);
	write_pointer =  (void*)((char*)write_pointer +  sizeof(void*));

	printf("write pointer is %p\n",write_pointer);	
	
	*((uint32_t*)write_pointer)  = buffer2; 
	ofld_vranlc.args[3].location =  (uint32_t*)((char*)write_pointer + + 0x40000000);
	write_pointer =  (void*)((char*)write_pointer +  sizeof(void*));

	printf("write pointer is %p\n",write_pointer);	
	ofld_vranlc.new_request = 0xF00F0FF0;		
	ofld_vranlc.type = BLACKSCHOLES_REQ;
	
	memcpy((void*)rw_buf.write_area , (void*)&ofld_vranlc , sizeof(struct offload_struct) );
	printf("Copied the sturct details\n");
	printf("Args are %d. TID alone needs to be sent\n", tid);	
	int request_feedback = 1;
	while(1)
	{
		if((*((uint32_t*)rw_buf.write_area) == ~(0xF00F0FF0) ) && request_feedback )
		{
			printf("ARM has started calculating blackscholes\n");

			request_feedback = 0;
			break;
		}

	}	
}


int main() 
{
  double Mops, t1, t2, t3, t4, x1, x2;
  double sx, sy, tm, an, tt, gc;
  double sx_verify_value, sy_verify_value, sx_err, sy_err;
  int    np;
  int    i, ik, kk, l, k, nit;
  int    k_offset, j;
  logical verified, timers_enabled;

  printf("x86 kernel started\n");
  rw_buf.write_area = (void*)0x50000000;
  rw_buf.read_area  = (void*)0x5f000000;
  struct handshake hnsk = {
                .present = 0x1FF1F11F,
        };
/*	for(int li = 0 ; li < 20 ; li++)
	{
		printf("char at %p is %c\n",((char *)rw_buf.read_area + li) , *((char *)rw_buf.read_area + li));
	}
*/	
        struct handshake in_hnsk;
        *((uint32_t*)rw_buf.write_area) = 0x00000000 ;
        memcpy(hnsk.arch, "x86",4);
        memcpy((void*)&in_hnsk,rw_buf.read_area,sizeof(struct shared_area));
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
	*((uint32_t*)rw_buf.read_area) = 0x00000000 ;
        *((uint32_t*)rw_buf.write_area) = 0x00000000 ;
	printf("x86_kernel_ shraed memory area exiting\n");
	struct timeval tv;
  gettimeofday(&tv,(void *)0);
  printf("Seconds recorded is %d \n\n",tv.tv_sec);

  blackscholes_main();
    

  gettimeofday(&tv,(void *)0);
  for(i=0; i<1000; i++) {
	printf("%.18f %d \n", prices[i],i);
  } 
  printf("Seconds recorded is %d \n",tv.tv_sec);
  
  printf("Requesting the ARM kernel to shutdown\n");
  struct offload_struct shutdown;
  shutdown.new_request = 0xF00F0FF0;
  shutdown.type = 0xDE;
  memcpy((void*)rw_buf.write_area , (void*)&shutdown , sizeof(struct offload_struct) );
  printf("x86_kernel_exiting\n");
        *((uint32_t*)rw_buf.read_area) = 0x00000000 ;
//        *((uint32_t*)rw_buf.write_area) = 0x00000000 ;
  return 0;
}
