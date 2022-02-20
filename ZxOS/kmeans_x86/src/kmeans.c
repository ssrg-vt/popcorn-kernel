#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <getopt.h>
#include <errno.h>
#include <sys/time.h>

#include "utils.h"

#define STACKSIZ ((8192 *2) + CONFIG_TEST_EXTRA_STACKSIZE)

K_THREAD_STACK_ARRAY_DEFINE(stacks, 2 , STACKSIZ);

int num_points = 5000000;	/* number of vectors */
int num_means = 100;		/* number of clusters */
int dim = 3;				/* Dimension of each vector */
int grid_size = 1000;		/* size of each dimension of vector space */

int num_nodes = 1;			/* Number of nodes to use */
int threads_per_node = 2;	/* Threads per node */

int modified = true;
pthread_barrier_t barr;		/* Synchronization with main thread */

int *points;
int *means;
int *clusters;


typedef struct {
	/* Work splitting for calculating cluster */
	int cluster_start_idx;
	int cluster_num_pts;

	/* Work splitting for updating means */
	int means_start_idx;
	int means_num_pts;

	/* Node id to run on */
	int nid;
#ifdef _ALIGN_VARIABLES
	char _padding[PAGE_SIZE
		- sizeof(int) * 5
	];
#endif
}  thread_arg;


/**
 * dump_points()
 *  Helper function to print out the points
 */
void dump_points(int *vals, int rows)
{
	int i, j;

	for (i = 0; i < rows; i++) {
		for (j = 0; j < dim; j++) {
			PRINTF("%5d ",vals[i * dim + j]);
		}
		PRINTF("\n");
	}
}

/**
 * parse_args()
 *  Parse the user arguments
 */
void parse_args() 
{

	printf("Dimension = %d\n", dim);
	printf("Number of clusters = %d\n", num_means);
	printf("Number of points = %d\n", num_points);
	printf("Size of each dimension = %d\n", grid_size);
	printf("Number of nodes = %d\n", num_nodes);
	printf("Threads per node = %d\n", threads_per_node);
}

/**
 * generate_points()
 *  Generate the points
 */
void generate_points(int *pts, int size) 
{   
	int i, j;

	for (i=0; i<size; i++) 
	{
		for (j=0; j<dim; j++) 
		{
			pts[i * dim + j] = rand() % grid_size;
		}
	}
}

/**
 * get_sq_dist()
 *  Get the squared distance between 2 points
 */
static inline unsigned int get_sq_dist(int *v1, int *v2)
{
	int i;

	unsigned int sum = 0;
	for (i = 0; i < dim; i++) 
	{
		sum += ((v1[i] - v2[i]) * (v1[i] - v2[i])); 
	}
	return sum;
}

/**
 * add_to_sum()
 *	Helper function to update the total distance sum
 */
void add_to_sum(int *sum, int *point)
{
	int i;

	for (i = 0; i < dim; i++)
	{
		sum[i] += point[i];   
	}   
}

/**
 * find_clusters()
 *  Find the cluster that is most suitable for a given set of points
 */
void find_clusters(int start_idx, int end_idx) 
{
	int i, j;
	unsigned int min_dist, cur_dist;
	int min_idx;
#ifdef _ALIGN_VARIABLES
	int local_modified = false;
#endif

	for (i = start_idx; i < end_idx; i++) 
	{
		min_dist = get_sq_dist(&points[i * dim], &means[0]);
		min_idx = 0; 
		for (j = 1; j < num_means; j++)
		{
			cur_dist = get_sq_dist(&points[i * dim], &means[j * dim]);
			if (cur_dist < min_dist) 
			{
				min_dist = cur_dist;
				min_idx = j;   
			}
		}

		if (clusters[i] != min_idx) 
		{
			clusters[i] = min_idx;
#ifndef _ALIGN_VARIABLES
			modified = true;
#else
			local_modified = true;
#endif
		}
	}
#ifdef _ALIGN_VARIABLES
	modified |= local_modified;
#endif
}

/**
 * calc_means()
 *  Compute the means for the various clusters
 */
void calc_means(int start_idx, int end_idx, int *sum)
{
	int i, j, grp_size;

	for (i = start_idx; i < end_idx; i++) 
	{
		memset(sum, 0, dim * sizeof(int));
		grp_size = 0;

		for (j = 0; j < num_points; j++)
		{
			if (clusters[j] == i) 
			{
				add_to_sum(sum, &points[j * dim]);
				grp_size++;
			}   
		}

		for (j = 0; j < dim; j++)
		{
			//dprintf("div sum = %d, grp size = %d\n", sum[j], grp_size);
			if (grp_size != 0)
			{ 
				means[i * dim + j] = sum[j] / grp_size;
			}
		}
	}
}

static void *thread_loop(void *args)
{
	thread_arg *targ = (thread_arg *)args;
	int cluster_end = targ->cluster_start_idx + targ->cluster_num_pts;
	int means_end = targ->means_start_idx + targ->means_num_pts;
	int *sum;

#ifdef _ALIGN_VARIABLES
	sum = MyMalloc(sizeof(int) * dim);
#else
	sum = (int *)MyMalloc(sizeof(int) * dim);
#endif
//	migrate(targ->nid, NULL, NULL);

	/* Iterative loop */
	while(modified)
	{
		/* Make sure everybody enters loop with previous modified value */
		pthread_barrier_wait(&barr);

		/* Wait for main thread to reset modified */
		pthread_barrier_wait(&barr);
		find_clusters(targ->cluster_start_idx, cluster_end);

		/* Wait for all cluster updates */
		pthread_barrier_wait(&barr);
		calc_means(targ->means_start_idx, means_end, sum);
	}

	free(sum);
//	migrate(0, NULL, NULL);

	return NULL;
}

int kmeans_main()
{
	struct timeval beginT, startT, endT;
	int num_procs, curr_cluster, curr_mean, cluster_per_thread,
		mean_per_thread, i, excess_cluster, excess_mean;
	int iter = 0;
	pthread_t *pid;
	pthread_attr_t attr[2];
	thread_arg *arg;

	parse_args();

	points = (int *)MyMalloc(sizeof(int) * num_points * dim);
	PRINTF("Generating points\n");
	generate_points(points, num_points);

	means = (int *)MyMalloc(sizeof(int) * num_means * dim);
	PRINTF("Generating means\n");
	generate_points(means, num_means);

	clusters = (int *)MyMalloc(sizeof(int) * num_points);
	memset(clusters, -1, sizeof(int) * num_points);

	num_procs = num_nodes * threads_per_node;

	pid = (pthread_t *)MyMalloc(sizeof(pthread_t) * num_procs);
	pthread_barrier_init(&barr, NULL, num_procs + 1);
	arg = (thread_arg *)MyMalloc(sizeof(thread_arg) * num_procs);

	modified = true;

	/* Calculate clustering/mean update parameters & start threads. */
	cluster_per_thread = num_points / num_procs;
	excess_cluster = num_points % num_procs;
	curr_cluster = 0;
	mean_per_thread = num_means / num_procs;
	excess_mean = num_means % num_procs;
	curr_mean = 0;
	for(i = 0; i < num_procs; i++)
	{
		arg[i].cluster_start_idx = curr_cluster;
		arg[i].cluster_num_pts = cluster_per_thread;
		if (excess_cluster > 0) {
			arg[i].cluster_num_pts++;
			excess_cluster--;
		}
		curr_cluster += arg[i].cluster_num_pts;

		arg[i].means_start_idx = curr_mean;
		arg[i].means_num_pts = mean_per_thread;
		if (excess_mean > 0) {
			arg[i].means_num_pts++;
			excess_mean--;
		}
		curr_mean += arg[i].means_num_pts;

		arg[i].nid = i / threads_per_node;
		pthread_attr_init(&attr[i]);
		pthread_attr_setstack(&attr[i], &stacks[i][0], STACKSIZ);
		pthread_create(&pid[i], &attr[i], thread_loop, &arg[i]);
	}

	printf("Starting iterative algorithm\n");

	gettimeofday(&beginT, NULL);
	/* Create the threads to process the distances between the various
	   points and repeat until modified is no longer valid */
	while (modified) 
	{
		/* Make sure all threads (including this one) see updates to modified
		 * before continuing */
		gettimeofday(&startT, NULL);
		pthread_barrier_wait(&barr);
		modified = false;
		pthread_barrier_wait(&barr);
		/* Find cluster */
		pthread_barrier_wait(&barr);
		/* Calculate means */
		gettimeofday(&endT, NULL);
		PRINTF("%d  %.6lf  %.6lf\n", iter++,
				stopwatch_elapsed(&startT, &endT),
				stopwatch_elapsed(&beginT, &endT));
	}

	for (i = 0; i < num_procs; i++) {
		pthread_join(pid[i], NULL);   
	}

	PRINTF("\n\nFinal means:\n");
	dump_points(means, num_means);
	printf("kmeans: Completed %.6lf\n\n", stopwatch_elapsed(&beginT, &endT));

	free(pid);
	free(arg);
	free(points);
	free(means);
	free(clusters);

	return 0;
}
