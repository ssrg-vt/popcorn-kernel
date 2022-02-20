#include<stdio.h>
#include<stddef.h>
#include "mymalloc.h"

char * memory_malloc = 0x60000000;
struct block *freeList=(void*)0x60000000;

static char status = 0;

void malloc_initialize(){
 freeList->size=0x30000000-sizeof(struct block);
 freeList->free=1;
 freeList->next=NULL;
}

void split(struct block *fitting_slot,size_t size){
 struct block *new=(void*)((void*)fitting_slot+size+sizeof(struct block));
 new->size=(fitting_slot->size)-size-sizeof(struct block);
 new->free=1;
 new->next=fitting_slot->next;
 fitting_slot->size=size;
 fitting_slot->free=0;
 fitting_slot->next=new;
}


void *MyMalloc(size_t noOfBytes){
 struct block *curr,*prev;
 void *result;
 printf("MAlloc called for %lu bytes\n",noOfBytes);
 if(!(freeList->size) || (status == 0)){
  malloc_initialize();
  printf("Memory initialized\n");
  status = 1;
 }
 curr=freeList;
 while((((curr->size)<noOfBytes)||((curr->free)==0))&&(curr->next!=NULL)){
  prev=curr;
  curr=curr->next;
  //printf("One block checked\n");
 }
 if((curr->size)==noOfBytes){
  curr->free=0;
  result=(void*)(++curr);
  //printf("Exact fitting block allocated\n");
  return result;
 }
 else if((curr->size)>(noOfBytes+sizeof(struct block))){
  split(curr,noOfBytes);
  result=(void*)(++curr);
  //printf("Fitting block allocated with a split\n");
  return result;
 }
 else{
  result=NULL;
  //printf("Sorry. No sufficient memory to allocate\n");
  return result;
 }
}

void merge(){
 struct block *curr,*prev;
 curr=freeList;
 while((curr->next)!=NULL){
  if((curr->free) && (curr->next->free)){
   curr->size+=(curr->next->size)+sizeof(struct block);
   curr->next=curr->next->next;
  }
  prev=curr;
  curr=curr->next;
 }
}

void MyFree(void* ptr){
 if(((void*)memory_malloc<=ptr)&&(ptr<=(void*)(memory_malloc +0x30000000))){
  struct block* curr=ptr;
  --curr;
  curr->free=1;
  merge();
 }
 else printf("Please provide a valid pointer allocated by MyMalloc\n");
}
