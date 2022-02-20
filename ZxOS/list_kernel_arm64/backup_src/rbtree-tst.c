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

struct mynode {
  	struct rb_node node;
  	char *string;
};

bool * global_lock;

void MyLock(bool *lock) {
    while (__atomic_test_and_set(lock,__ATOMIC_RELAXED) == 1);
}

void MyUnlock(bool * lock)
{
        __atomic_clear(lock,__ATOMIC_RELAXED);
}

struct rb_root mytree = RB_ROOT;

struct mynode * my_search(struct rb_root *root, char *string)
{
  	struct rb_node *node = root->rb_node;
//	k_mutex_lock(global_lock, K_FOREVER);
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
//	k_mutex_unlock(global_lock);
	return NULL;
}

int my_insert(struct rb_root *root, struct mynode *data)
{
//	k_mutex_lock(global_lock, K_FOREVER);
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
//	k_mutex_unlock(global_lock);

	return 1;
}

void my_free(struct mynode *node)
{
//	k_mutex_lock(global_lock, K_FOREVER);
	static int free_called;
	free_called++;
	if (node != NULL) {
		if (node->string != NULL) {
			MyFree(node->string);
			node->string = NULL;
		}
		MyFree(node);
		node = NULL;
	}
	printf("Free called %d times\n",free_called);
//	k_mutex_unlock(global_lock);
	
}

#define NUM_NODES 500

int kernel_rb_main(int * counter , struct rb_root * mytree_in , struct mynode ** mn_in , bool * my_mutex)
{

	struct mynode **mn = (struct mynode **)mn_in;
	global_lock = my_mutex ;	
	MyLock(my_mutex);
	/* *insert */
	int * i = counter;
	static int removal; 	
	struct mynode *data = my_search(mytree_in, "20");
	while(removal < 15){
	for(int dd = 0 ; dd < 3900 ; dd++){
		char text[4];
		sprintf(text,"%x",dd);
        	data = my_search(mytree_in, text);
        	if (data) {
	        	printf("delete node %d: \n",dd);
                	rb_erase(&data->node, mytree_in);
			removal++;
			my_free(data);
        	}
	
	}
	}
	
	/* *search */
	struct rb_node *node;
	printf("search all nodes: \n");
	for (node = rb_first(mytree_in); node; node = rb_next(node))
		printf("key = %s\n", rb_entry(node, struct mynode, node)->string);


	return 0;
}


