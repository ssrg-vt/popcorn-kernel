/*
 * Copyright (C) 2013 Michael Andersch <michael.andersch@mailbox.tu-berlin.de>
 *
 * This file is part of Starbench.
 *
 * Starbench is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Starbench is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Starbench.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <zephyr.h>

#define STACKSZ (1024 + CONFIG_TEST_EXTRA_STACKSIZE)
static K_THREAD_STACK_ARRAY_DEFINE(stacks_mt, 1, STACKSZ);

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <pthread.h>


#include "md5.h"
#include "md5_bmark.h"

typedef struct timeval timer;

#define TIME(x) gettimeofday(&x, NULL)

/* Function declarations */
int initialize(md5bench_t* args);
int finalize(md5bench_t* args);
void run(md5bench_t* args);
void process(uint8_t* in, uint8_t* out, int bufsize);
void listInputs();
long timediff(timer* starttime, timer* finishtime);

static char* usage = "Usage: %s <options>\n"
                     "-i inputnum          choose input data set\n"
                     "-c iterations        specify benchmark iterations\n"
                     "-s                   show available input sets\n"
                     "-t                   number of threads\n"
                     "-p                   thread pinning\n"
                     "-h                   this help text\n";

// Input configurations
static data_t datasets[] = {
    {64, 512, 0},
    {64, 1024, 0},
    {64, 2048, 0},
    {64, 4096, 0},
    {128, 1024*512, 1},
    {128, 1024*1024, 1},
    {128, 1024*2048, 1},
    {128, 1024*4096, 1},
};

pthread_t* threads;
int *  next_buf ;

/*
*   Function: initialize
*   --------------------
*   To initialize the benchmark parameters. Generates the input buffers from random data.
*/
int initialize(md5bench_t* args) {
    int index = args->input_set;
    if(index < 0 || index >= sizeof(datasets)/sizeof(datasets[0])) {
        printf( "Invalid input set specified! Clamping to set 0\n");
        index = 0;
    }

    args->numinputs = datasets[index].numbufs;
    args->size = datasets[index].bufsize;
    args->inputs = (uint8_t**)MyMalloc(args->numinputs * sizeof(uint8_t*));
    args->out = (uint8_t*)MyMalloc(args->numinputs * DIGEST_SIZE);
    threads = (pthread_t*)MyMalloc(args->numthreads* sizeof(pthread_t));
    if(args->inputs == NULL || args->out == NULL || threads == NULL) {
        printf( "Memory Allocation Error\n");
        return -1;
    }

    printf( "Reading input set: %d buffers, %d bytes per buffer\n   ", datasets[index].numbufs, datasets[index].bufsize);
    printf( "Using %d parallel threads\n", args->numthreads);

    // Now the input buffers need to be generated, for replicability, use same seed
    srand(datasets[index].rseed);

    for(int i = 0; i < args->numinputs; i++) {
        args->inputs[i] = (uint8_t*)MyMalloc(sizeof(uint8_t)*datasets[index].bufsize);
	uint8_t* p = args->inputs[i];
        if(p == NULL) {
            printf("Memory Allocation Error here\n");
            return -1;
        }
        for(int j = 0; j < datasets[index].bufsize; j++)
            *p++ = rand() % 255;
    }

    return 0;
}

/*
*   Function: process
*   -----------------
*   Processes one input buffer, delivering the digest into out.
*/
void process(uint8_t* in, uint8_t* out, int bufsize) {
    MD5_CTX context;
    uint8_t digest[16];

    MD5_Init(&context);
    MD5_Update(&context, in, bufsize);
    MD5_Final(digest, &context);

    memcpy(out, digest, DIGEST_SIZE);
}

void* md5_thread(void* arg) {
    int buf_to_process;
    threadarg_t* args = (threadarg_t*)arg;


    while((buf_to_process = __sync_fetch_and_add(next_buf, 1)) < args->numbufs) {
         printf("Processing buf %d\n", buf_to_process);
        process(args->in[buf_to_process], args->out + buf_to_process * DIGEST_SIZE, args->size);
    }

    return NULL;
}

/*
*   Function: run
*   --------------------
*   Main benchmarking function. If called, processes buffers with MD5
*   until no more buffers available. The resulting message digests
*   are written into consecutive locations in the preallocated output
*   buffer.
*/
void run(md5bench_t* args) {

	pthread_attr_t attrp[2] ;
	pthread_t new_thread[2] ;
    	threadarg_t threadargs[args->numthreads];
    	for(int iter = 0; iter < args->iterations; iter++) {
	*next_buf = 0;
        for(int i = 0; i < args->numthreads; i++) {
            threadargs[i].in = args->inputs;
            threadargs[i].out = args->out;
            threadargs[i].size = args->size;
            threadargs[i].numbufs = args->numinputs;
            threadargs[i].tid = i;
            threadargs[i].next_buf = next_buf;
	    pthread_attr_init(&attrp[i]);
	    pthread_attr_setstack(&attrp[i], &stacks_mt[0][i], STACKSZ);
	
	if(i%2){
	    pthread_create(&threads[i], &attrp[i], md5_thread, &threadargs[i]);
        }
	else{
	   offload_md5_struct * md5_struct =  (offload_md5_struct *) MyMalloc( sizeof(offload_md5_struct)); 
	   md5_struct->arg_location  = (threadarg_t *)  MyMalloc( sizeof(threadarg_t)); 
	   memcpy(md5_struct->arg_location , &threadargs[i], sizeof(threadarg_t) );
	   
	   offload_md5_thread(md5_struct);
	}
	}
	}
        for(int i = 0; i < args->numthreads; i++) {
            if(i%2)
		pthread_join(threads[i], NULL);
        }
}

/*
*   Function: finalize
*   ------------------
*   Cleans up memory used by the benchmark for input and output buffers.
*/
int finalize(md5bench_t* args) {

    char outname[] = "output.txt";
    char buffer[64];
    int offset = 0;
    FILE* fp;

    if(args->outflag) {
        fp = fopen(outname, "w+");

        for(int i = 0; i < args->numinputs; i++) {
            sprintf(buffer, "Buffer %d has checksum", i);
            fwrite(buffer, sizeof(char), strlen(buffer)+1, fp);
            for(int j = 0; j < DIGEST_SIZE*2; j+=2) {
                sprintf(buffer+j, "%x", args->out[DIGEST_SIZE*i+j/2] & 0xf);
                sprintf(buffer+j+1, "%x", args->out[DIGEST_SIZE*i+j/2] & 0xf0);
            }
            buffer[32] = '\0';
            fwrite(buffer, sizeof(char), 32, fp);
            fputc('\n', fp);
        }

        fclose(fp);
    }

    if(args->inputs) {
        for(int i = 0; i < args->numinputs; i++) {
            if(args->inputs[i])
                free(args->inputs[i]);
        }

        free(args->inputs);
    }

    if(args->out)
        free(args->out);

	free(threads);

    return 0;
}

/*
*   Function: listInputs
*   --------------------
*   Lists all available input configurations of the benchmark on stderr.
*/
void listInputs() {
    double datasize_mb;
    printf( "Available input configurations:\n");

    for(int i = 0; i < sizeof(datasets)/sizeof(datasets[0]); i++) {
        datasize_mb = (double)(datasets[i].numbufs * datasets[i].bufsize)/1000000.0;
        printf( "Index %d: %d buffers of %d bytes, totalling %.1fMB\n", i, datasets[i].numbufs, datasets[i].bufsize, datasize_mb);
    }
}

/*
*   Function: timediff
*   ------------------
*   Compute the difference between timers starttime and finishtime in msecs.
*/
long timediff(timer* starttime, timer* finishtime)
{
    long msec;
    msec=(finishtime->tv_sec-starttime->tv_sec)*1000;
    msec+=(finishtime->tv_usec-starttime->tv_usec)/1000;
    return msec;
}

/** MAIN **/
int md5_main() {

    int opt;
    extern char* optarg;
    extern int optind;

    timer io_start, b_start, b_end;
    
    md5bench_t args;
    
    next_buf = (int*)MyMalloc( sizeof(int));
    *next_buf = 0; 
	// Default values for benchmark params
    args.input_set = 6;
    args.iterations = 1;
    args.outflag = 0;
    args.numthreads = 2;
    args.pinning = 0;

    // Who we are
    printf( "StarBench - MD5 Kernel\n");
/*    args.input_set = 4;
    args.iterations = 40;
    args.numthreads = 2;
    args.pinning = 0;
*/         
     listInputs();

	if(args.iterations < 1 || args.numthreads < 1) {
		printf( "Bad argument given\n");
		exit(EXIT_FAILURE);
	}

    TIME(io_start);

    // Parameter initialization
    if(initialize(&args)) {
        printf( "Initialization Error\n");
        exit(EXIT_FAILURE);
    }
    TIME(b_start);

    run(&args);
    
    TIME(b_end);

    // Free memory
/*    if(finalize(&args)) {
		printf( "Finalization Error\n");
		exit(EXIT_FAILURE);
	}
*/
    double io_time = (double)timediff(&io_start, &b_start)/1000;
    double b_time = (double)timediff(&b_start, &b_end)/1000;

    printf("\nI/O time: %.3f\nTime: %.3f\n", io_time, b_time);

    return 0;
}
