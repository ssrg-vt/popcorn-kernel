#include <pthread.h>
#include <sys/types.h>
#include <unistd.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

extern void test_posix_mqueue(void);

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

#define STACKSZ (1024 + CONFIG_TEST_EXTRA_STACKSIZE)
K_THREAD_STACK_ARRAY_DEFINE(stacks_mt, 2, STACKSZ);

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
                .write_area = (void*)0x9f000000,
                .read_area  = (void*)0x90000000
        };
//	*((uint32_t*)rw_buf.write_area) = 0x00000000 ;
/*        struct handshake hnsk = {
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
*/
//	test_posix_mqueue();
	blackscholes_main();
}
