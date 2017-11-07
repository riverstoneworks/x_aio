/*
 * test.c
 *
 *  Created on: Oct 15, 2017
 *      Author: Like.Z
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

void *on_io_complete (struct io_event * ie,int n){
	return (void*)0;
}

int main(int argc, char *argv[]) {
	Aio_param ap={
			.aioQueueCapacity=10,
			.io_event_wait_min_num=5,
			.io_delay={{0,1000000},{0,0}},
			.timeout_io_get_event={0,2000000},
			.on_io_complete=on_io_complete
	};

	const char* const aiop=aio_service_start(ap,NULL);

	sleep(3);

	aio_service_stop(aiop);

	sleep(3);

	exit(EXIT_SUCCESS);
}
