#include <pthread.h>
#include <sys/types.h>
#include <unistd.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

extern void test_posix_mqueue(void);

#define STACKSZ (1024 + CONFIG_TEST_EXTRA_STACKSIZE)
K_THREAD_STACK_ARRAY_DEFINE(stacks_mt, 2, STACKSZ);

struct handshake
{       
        uint32_t  present;
        char arch[10];
};

struct shared_area
{
        volatile void * write_area;
        volatile void * read_area;
};

volatile struct shared_area rw_buf; 

void *print_hello(void *arg)
{
	//while(1)
	for(int i = 0 ; i < 100 ; i++)
	{
		printk("Hello \n");
		usleep(USEC_PER_MSEC * 10U);
	}
}

void *print_again(void *arg)
{
	//while(1)
	for(int i = 0 ; i < 100 ; i++)
	{
		printk("Hello again\n");
		usleep(USEC_PER_MSEC * 10U);
	}
}

int main()
{
	volatile struct shared_area rw_buf = {
                .write_area = (void*)0x50000000,
                .read_area  = (void*)0x5f000000
        };
	struct handshake hnsk = {
                .present = 0x1FF1F11F,
        };
/*
	pthread_attr_t attrp[2] ;
	pthread_t new_thread[2] ;
	pthread_attr_init(&attrp[0]);
	pthread_attr_setstack(&attrp[0], &stacks_mt[0][0], STACKSZ);
	int r = pthread_create(&new_thread[0], &attrp[0], print_hello, (void*)0);
	printf("Thread one ID is %d\n",r);
	pthread_attr_init(&attrp[1]);
	pthread_attr_setstack(&attrp[1], &stacks_mt[0][1], STACKSZ);
	int s = pthread_create(&new_thread[1], &attrp[1], print_again, (void*)0);
	printf("Thread two ID is %d\n",s);
	pthread_join(new_thread[1], NULL);
	pthread_join(new_thread[0], NULL);
*/

/*	struct handshake in_hnsk;
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
*/
//	test_posix_mqueue();
	blackscholes_main();


	printf("x86_kernel_exiting\n");
        *((uint32_t*)rw_buf.read_area) = 0x00000000 ;
        *((uint32_t*)rw_buf.write_area) = 0x00000000 ;
	while(1);
}
