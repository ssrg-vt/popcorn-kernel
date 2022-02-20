/*
 * =============================================================================
 *
 *       Filename:  rbtree-tst.c
 *
 *    Description:  rbtree testcase.
 *
 *        Created:  09/02/2012 11:39:34 PM
 *
 *         Author:  Fu Haiping (forhappy), haipingf@gmail.com
 *        Company:  ICT ( Institute Of Computing Technology, CAS )
 *
 * =============================================================================
 */

#include "rbtree.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

extern void wait_for_other_core();
extern void  offload_rbtree_load(int * i,struct rb_root * mytree , struct mynode ** nodes , bool* my_mutex);

bool * global_lock;

struct mynode {
  	struct rb_node node;
  	char *string;
};

void MyLock(bool *lock) { 
    while (__atomic_test_and_set(lock,__ATOMIC_RELAXED) == 1); 
}

void MyUnlock(bool * lock)
{
	__atomic_clear(lock,__ATOMIC_RELAXED);
}

struct rb_root * mytree;

struct mynode * my_search(struct rb_root *root, char *string)
{
  	struct rb_node *node = root->rb_node;
	
  	while (node) {
  		struct mynode *data = container_of(node, struct mynode, node);
		int result;

		result = strcmp(string, data->string);

		if (result < 0)
  			node = node->rb_left;
		else if (result > 0)
  			node = node->rb_right;
		else
  			return data;
	}
	return NULL;
}

int my_insert(struct rb_root *root, struct mynode *data)
{
  	MyLock(global_lock);
	struct rb_node **new = &(root->rb_node), *parent = NULL;

  	/* Figure out where to put new node */
  	while (*new) {
  		struct mynode *this = container_of(*new, struct mynode, node);
  		int result = strcmp(data->string, this->string);

		parent = *new;
  		if (result < 0)
  			new = &((*new)->rb_left);
  		else if (result > 0)
  			new = &((*new)->rb_right);
  		else
  			return 0;
  	}

  	/* Add new node and rebalance tree. */
  	rb_link_node(&data->node, parent, new);
  	rb_insert_color(&data->node, root);
	MyUnlock(global_lock);

	return 1;
}

void my_free(struct mynode *node)
{
	MyLock(global_lock);
	if (node != NULL) {
		if (node->string != NULL) {
			MyFree(node->string);
			node->string = NULL;
		}
		MyFree(node);
		node = NULL;
	}
	MyUnlock(global_lock);
}

#define NUM_NODES 40

int kernel_rb_main()
{

	struct mynode **mn = (struct mynode**)MyMalloc(NUM_NODES* sizeof(struct mynode *));
	mytree = (struct rb_root *)MyMalloc(sizeof(struct rb_root));
	*mytree = RB_ROOT;
	/* *insert */
	int * i = (int *)MyMalloc(sizeof(int) );
	bool * my_mutex;
	my_mutex = (bool *) MyMalloc(sizeof(bool));

	*my_mutex = 0;
	global_lock = my_mutex; 	

	printf("Size of rb node is %d\n",sizeof(struct rb_node));
	*i = 0;
	printf("insert node from 1 to %d: \n",NUM_NODES);
	for (; *i < NUM_NODES; (*i) += 1) {
                mn[*i] = (struct mynode *)MyMalloc(sizeof(struct mynode));
                memset(mn[*i],0,sizeof(struct mynode));
		mn[*i]->string = (char *)MyMalloc(sizeof(char) * 4);
                sprintf(mn[*i]->string, "%x", *i);
                printf("Inserting %d\n",*i);
                my_insert(mytree, mn[*i]);
        }
	offload_rbtree_load(i,mytree , mn , my_mutex);
	for (; *i < NUM_NODES + 50 ; (*i) += 1) {
		mn[*i] = (struct mynode *)MyMalloc(sizeof(struct mynode));
		mn[*i]->string = (char *)MyMalloc(sizeof(char) * 4);
		sprintf(mn[*i]->string, "%x", *i);
		printf("Inserting %d\n",*i);
		my_insert(mytree, mn[*i]);
	}
	/* *search */
	struct rb_node *node;
	printf("search all nodes: \n");
	for (node = rb_first(mytree); node; node = rb_next(node))
		printf("key = %s\n", rb_entry(node, struct mynode, node)->string);
	/* *delete */
	printf("delete node 20: \n");
	struct mynode *data = my_search(mytree, "20");
	if (data) {
		rb_erase(&data->node, mytree);
		my_free(data);
	}


	uint32_t nnode = 0;	
	/* *search again*/
	printf("search again:\n");
	for (node = rb_first(mytree); node; node = rb_next(node))
	{	printf("key = %s\n", rb_entry(node, struct mynode, node)->string);
		nnode++;
	}
	printf("Remaining nodes are %d\n",nnode);
	return 0;
}


