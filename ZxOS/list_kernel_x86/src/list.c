#include "list.h"
#include <sys/types.h>"

void MyLock(char *lock) {
     while (__atomic_test_and_set(lock,__ATOMIC_SEQ_CST) == 1){
        printf("Waiting for the lock\n");
        }
}

void MyUnlock(char * lock)
{
        __atomic_clear(lock,__ATOMIC_RELAXED);
}

struct a_list
{
  struct list_head list;
  const char* str;
};

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
int list_main()
{
  struct a_list* iter; 
  blist = (struct a_list*)MyMalloc(sizeof(struct a_list));
  INIT_LIST_HEAD(&blist->list);
  blist->str = (char *)MyMalloc(4* sizeof(char));
  

  for (int i = 0 ; i < NUM_ENTRY ; i++)
  {
	struct a_list* tmp;
  	tmp = (struct a_list*)MyMalloc(sizeof(struct a_list));
	tmp->str = (char *)MyMalloc(4* sizeof(char));
	sprintf(tmp->str , "%x",i);
	list_add_tail( &(tmp->list), &(blist->list) );		
  	write_c++;
  }
  char * thislock = MyMalloc(sizeof(int));
	*thislock = 0;
  printf("Address of this lock is %p and value is %c\n",thislock,*thislock);
  iter = blist ; 
  offload_list_load(blist, thislock);
  int offset_blist = offsetof(struct a_list , list) ;
  static int count = 0; 
/* do
  {
	count++;	
	printf("%s \n", iter->str);
	iter = iter->list.next - offset_blist ; 
	read_c++; 
  }while(iter != blist);

  count = 0; 
  do
  {
	count++;	
	printf("%s \n", iter->str);
	iter = iter->list.next - offset_blist ; 
	read_c++;
	if(count ==5000)
		break; 
  }while(iter != blist);
*/  MyLock(thislock);
  printf("Write count till now %lu\n",write_c);
  /* remove all items in the list */
  while( !list_empty(&blist->list) ) {
    iter = list_entry(blist->list.next,struct a_list,list);
    list_del(&iter->list);
    write_c++;
    MyFree(iter);
  }
  MyUnlock(thislock);
  printf("Write count %lu and Read count %lu\n",write_c,read_c);
  return 0;
}
