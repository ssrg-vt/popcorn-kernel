#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <zephyr/types.h>
#include <sys/time.h>
#include "mymalloc.h"

#include "rbtree.h"

extern int md5_main();
extern int rb_tree_main();

#define BASE_SHMEM 0x60000000
#define SIZE_SHMEM 0x3F000000
/*
struct a_list
{
  struct list_head list;
  int   val;
  const char* str;
};
*/


enum request_type{
	COMPUTE_VRANLC,
	COMPUTE_RANDLC,
	COMPUTE_LOG,
	RESPOND_VRANLC,
	BLACKSCHOLES_REQ,
	OFFLOAD_MD5_THREAD

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


void * shr_addr = BASE_SHMEM;
/*
void offload_md5_thread( offload_md5_struct *  md5_struct){


	struct offload_struct  ofld_vranlc ;
	ofld_vranlc.new_request = 0xF00F0FF0;
	void * write_pointer = (void*)((char*)rw_buf.write_area + 0x1000);
	
	ofld_vranlc.args[0].location = md5_struct ;
	
		
	ofld_vranlc.type = OFFLOAD_MD5_THREAD;
	
	memcpy((void*)rw_buf.write_area , (void*)&ofld_vranlc , sizeof(struct offload_struct) );
	printf("Copied the sturct details for md5\n");
}
*/
extern struct mynode * my_search(struct rb_root *root, char *string);
extern int my_insert(struct rb_root *root, struct mynode *data);
extern void my_free(struct mynode *node);
struct rb_root mytree = RB_ROOT;
struct mynode {
        struct rb_node node;
        char *string;
};
#define NUM_NODES 32
extern struct mynode * my_rb_entry(struct rb_node *node );
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
	memset(shr_addr,0,0x10000);
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
  malloc_initialize();
  struct mynode *mn[1000];
  /* *insert */
        int i = 0;
        printf("insert node from 1 to NUM_NODES(32): \n"); 
 	for (; i < NUM_NODES; i++) {
                mn[i] = (struct mynode *)MyMalloc(sizeof(struct mynode));
                mn[i]->string = (char *)MyMalloc(sizeof(char) * 4);
                sprintf(mn[i]->string, "%d", i);
                my_insert(&mytree, mn[i]);
        } 
  /* *search */
        struct rb_node *node;
        printf("search all nodes: \n");
        for (node = rb_first(&mytree); node; node = rb_next(node))
                printf("key = %s\n", my_rb_entry(node)->string);

 /* *delete */
        printf("delete node 20: \n");
        struct mynode *data = my_search(&mytree, "20");
        if (data) {
                rb_erase(&data->node, &mytree);
                free(data);
        }

  printf("Requesting the ARM kernel to shutdown\n");
  struct offload_struct shutdown;
  shutdown.new_request = 0xF00F0FF0;
  shutdown.type = 0xDE;
//  memcpy((void*)rw_buf.write_area , (void*)&shutdown , sizeof(struct offload_struct) );
  gettimeofday(&tv,(void *)0);
  printf("Seconds recorded is %d \n",tv.tv_sec);
  printf("x86_kernel_exiting\n");
        *((uint32_t*)rw_buf.read_area) = 0x00000000 ;
      *((uint32_t*)rw_buf.write_area) = 0x00000000 ;
  return 0;
}
