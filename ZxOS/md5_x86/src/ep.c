
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <zephyr/types.h>
#include <sys/time.h>

extern int md5_main();

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

extern int freqmine_main(int argc, char **argv);


void vranlc_dist( int n, double *x, double a, double y[] ){

	printf("Size of double is %d\n",sizeof(double));

	struct offload_struct  ofld_vranlc ;
	ofld_vranlc.new_request = 0xF00F0FF0;
	void * write_pointer = (void*)((char*)rw_buf.write_area + 0x1000);
	
	*((int*)write_pointer)  = n; 
	ofld_vranlc.args[0].location =  (uint32_t*)((char*)write_pointer + 0x40000000);
	printf("Location of the int is %p\n",ofld_vranlc.args[0].location);
	write_pointer =  (void*)((char*)write_pointer +  sizeof(int));
	
	printf("Size after incrementing is %p\n",write_pointer);	
	*((double*)write_pointer) = *x;
        ofld_vranlc.args[1].location =  (uint32_t*)((char*)write_pointer + 0x40000000) ;
	printf("Location of the float pointer is %p\n",ofld_vranlc.args[1].location);
	write_pointer  =  (void*)((char*)write_pointer +  sizeof(double));
        
	*((double*)write_pointer) = a;
	ofld_vranlc.args[2].location =  (uint32_t*)((char*)write_pointer + 0x40000000) ;
	write_pointer  =  (void*)((char*)write_pointer +  sizeof(double));
	
	*((double*)write_pointer) = *y;	
        ofld_vranlc.args[3].location = (uint32_t*)((char*)write_pointer + 0x40000000)   ;
	
		
	ofld_vranlc.type = COMPUTE_VRANLC;
	ofld_vranlc.args[0].size = 1;
	ofld_vranlc.args[0].type = INT; 
	ofld_vranlc.args[1].size = 1;
        ofld_vranlc.args[1].type = DOUBLE;
	ofld_vranlc.args[2].size = 1;
        ofld_vranlc.args[2].type = DOUBLE;
	ofld_vranlc.args[3].size = 1;
        ofld_vranlc.args[3].type = DOUBLE;
	
	memcpy((void*)rw_buf.write_area , (void*)&ofld_vranlc , sizeof(struct offload_struct) );
	printf("Copied the sturct details\n");
	printf("Args are %d , %f , %f and %f\n", n,*x,a,*y);	
	int request_feedback = 1;
	while(1)
	{
		if((*((uint32_t*)rw_buf.write_area) == ~(0xF00F0FF0) ) && request_feedback )
		{
			printf("ARM has started calculating vranlc\n");
			request_feedback = 0;
		}
		else
		{
			struct offload_struct *  outp = (struct offload_struct *)rw_buf.read_area ;
			if(outp->new_request == 0xF00F0FF0)
			{
				*x = *(double*)outp->args[1].location;
				*y = *(double*)outp->args[3].location;
				printf("retrived values from ARM machine \n");
				break;
			}
			
		}

	}	
}
int main() 
{

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
  md5_main();
    
  printf("Requesting the ARM kernel to shutdown\n");
  struct offload_struct shutdown;
  shutdown.new_request = 0xF00F0FF0;
  shutdown.type = 0xDE;
  memcpy((void*)rw_buf.write_area , (void*)&shutdown , sizeof(struct offload_struct) );

  gettimeofday(&tv,(void *)0);
  printf("Seconds recorded is %d \n",tv.tv_sec);
  memset((void*)0x30000000, 'n' , 8196);  
  printf("x86_kernel_exiting\n");
        *((uint32_t*)rw_buf.read_area) = 0x00000000 ;
//        *((uint32_t*)rw_buf.write_area) = 0x00000000 ;
  return 0;
}
