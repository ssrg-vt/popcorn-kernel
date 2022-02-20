#include "list.h"
#include <sys/types.h>
struct a_list
{
  struct list_head list;
  const char* str;
};

void MyLock(char *lock) {
    while (__atomic_test_and_set(lock,__ATOMIC_SEQ_CST) == 1)
	{
//		printf("waiting for lock\n");
	}
}

void MyUnlock(char * lock)
{
        __atomic_clear(lock,__ATOMIC_RELAXED);
}
static void append(struct a_list* ptr,const char* str, int val)           
{
  struct a_list* tmp;
  tmp = (struct a_list*)MyMalloc(sizeof(struct a_list));
  if(!tmp) {
    perror("malloc");
    exit(1);
  }
  tmp->str = str;
  list_add_tail( &(tmp->list), &(ptr->list) );
}
uint32_t write_c = 0 , read_c =0;
struct a_list *  blist;
#define NUM_ENTRY 10000
int list_main(  void *  thelist , char * lock)
{
  struct a_list* iter; 
  blist = (struct a_list *)thelist;
  MyLock(lock); 
  *(uint32_t*)0x9f000000 = 0x1a2b3c4d;
/*
  for (int i = 0 ; i < NUM_ENTRY ; i++)
  {
	struct a_list* tmp;
  	tmp = (struct a_list*)MyMalloc(sizeof(struct a_list));
	tmp->str = (char *)MyMalloc(4* sizeof(char));
	sprintf(tmp->str , "%x",i);
	list_add_tail( &(tmp->list), &(blist->list) );		
  }
*/
  iter = blist ; 
  int offset_blist = offsetof(struct a_list , list) ;
  int count = 0;
  do
  {
			
	printf("%s \n", iter->str);
	iter = iter->list.next - offset_blist ;  
	read_c++;
	count++;
	if(count > 4999)
		break;				
 
   }while(iter != blist);
  printf("Number of entries is %d\n",count); 
  count = 0;
  printf("Number of entries is %d\n",count); 
  while( !list_empty(&blist->list.next) ) {
    iter = list_entry(blist->list.next,struct a_list,list);
    printf("Deleting %p\n",iter->list);
    list_del(&iter->list);
    write_c++;
    MyFree(iter);
    if(count == 5000)
	break;
    count++;
  }

  MyUnlock(lock);
  printf("Write count %lu and Read count %lu\n",write_c,read_c);
  return 0;
}
