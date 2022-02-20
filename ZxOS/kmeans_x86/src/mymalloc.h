#ifndef _MYMALLOC_H_
#define _MYMALLOC_H_
#include<stdio.h>
#include<stddef.h>

//char * memory_malloc = 0x60000000;

struct block{
 size_t size;
 int free;
 struct block *next; 
};

//struct block *freeList=(void*)0x60000000;

void malloc_initialize();
void split(struct block *fitting_slot,size_t size);
void *MyMalloc(size_t noOfBytes);
void merge();
void MyFree(void* ptr);

#endif
