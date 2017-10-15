/*
 * test.c
 *
 *  Created on: Oct 15, 2017
 *      Author: xpc
 */


#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/timerfd.h>
#include <sys/syscall.h>
#include <sys/eventfd.h>
#include <linux/aio_abi.h>
#include <error.h>
#include "include/aio_service.h"

#define handle_error_en(en, msg) \
               do { errno = en; perror(msg); exit(EXIT_FAILURE); } while (0)

int main(int argc, char *argv[]) {
	Aio_parameters aiop;
	aiop.aioQueueCapacity=10;
	aiop.io_event_wait_min_num=5;
	memset(&(aiop.timeout_io_get_event),0,sizeof(aiop.timeout_io_get_event));
	memset(&(aiop.io_delay),0,sizeof(aiop.io_delay));
	aiop.io_delay.it_interval.tv_nsec=1000000;
	aiop.timeout_io_get_event.tv_nsec=2000000;

	pthread_t th_aio_service;

	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(0, &cpuset);
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setinheritsched(&attr,PTHREAD_EXPLICIT_SCHED);
	pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpuset); //绑定逻辑核心
	if(-1== pthread_create(&th_aio_service, &attr, (void *(*) (void *))aio_service, &aiop))
		perror("pthread_create(th_aio_service, &attr, (void *(*) (void *))aio_service_start, aiop).");
	else
		printf("aio_service_started.\n");
	pthread_attr_destroy(&attr);

	sleep(10);
	aio_service_stop(&aiop);
	void* rr;
	pthread_join(th_aio_service,&rr);
	printf("%d\n",(int)rr);
	exit(EXIT_SUCCESS);
}

