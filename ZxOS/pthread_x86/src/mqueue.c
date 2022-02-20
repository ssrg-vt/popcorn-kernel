/*
 * Copyright (c) 2018 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <sys/printk.h>
#include <fcntl.h>
#include <sys/util.h>
#include <mqueue.h>
#include <pthread.h>

#define N_THR 2
#define STACKSZ (8192 + CONFIG_TEST_EXTRA_STACKSIZE)
#define SENDER_THREAD 0
#define RECEIVER_THREAD 1
#define MESSAGE_SIZE 16
#define MESG_COUNT_PERMQ 4

K_THREAD_STACK_ARRAY_DEFINE(stacks, N_THR, STACKSZ);

char queue[16] = "server";
char send_data[MESSAGE_SIZE] = "timed data send";

void *sender_thread(void *p1)
{
	mqd_t mqd;
	struct timespec curtime;
	for(int i = 0 ; i < 10 ; i++)
{
	mqd = mq_open(queue, O_WRONLY);
	clock_gettime(CLOCK_MONOTONIC, &curtime);
	curtime.tv_sec += 1;
	mq_timedsend(mqd, send_data, MESSAGE_SIZE, 0, &curtime);
	printf("MQ sent\n");
	usleep(USEC_PER_MSEC);
}
	mq_close(mqd);
	pthread_exit(p1);
	return NULL;

}


void *receiver_thread(void *p1)
{
	mqd_t mqd;
	char rec_data[MESSAGE_SIZE];
	struct timespec curtime;
for(int i = 0 ; i < 10; i++){
	mqd = mq_open(queue, O_RDONLY);
	clock_gettime(CLOCK_MONOTONIC, &curtime);
	curtime.tv_sec += 1;
	mq_timedreceive(mqd, rec_data, MESSAGE_SIZE, 0, &curtime);
	int res = strcmp(rec_data, send_data);
	printf("Sata compared result is %d\n",res);
	usleep(USEC_PER_MSEC);
	mq_close(mqd);
}
	pthread_exit(p1);
	return NULL;
}

void test_posix_mqueue(void)
{
	mqd_t mqd;
	struct mq_attr attrs;
	int32_t mode = 0777, flags = O_RDWR | O_CREAT, ret, i;
	void *retval;
	pthread_attr_t attr[N_THR];
	pthread_t newthread[N_THR];

	attrs.mq_msgsize = MESSAGE_SIZE;
	attrs.mq_maxmsg = MESG_COUNT_PERMQ;

	mqd = mq_open(queue, flags, mode, &attrs);

	for (i = 0; i < N_THR; i++) {
		/* Creating threads */
		if (pthread_attr_init(&attr[i]) != 0) {
			pthread_attr_init(&attr[i]);
		}
		pthread_attr_setstack(&attr[i], &stacks[i][0], STACKSZ);

		if (i % 2) {
			ret = pthread_create(&newthread[i], &attr[i],
					     sender_thread,
					     INT_TO_POINTER(i));
		} else {
			ret = pthread_create(&newthread[i], &attr[i],
					     receiver_thread,
					     INT_TO_POINTER(i));
		}

	}

	usleep(USEC_PER_MSEC * 10U);

	for (i = 0; i < N_THR; i++) {
		pthread_join(newthread[i], &retval);
	}

	mq_close(mqd);
	mq_unlink(queue);
}
