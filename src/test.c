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
	Aio_param ap;
	ap.aioQueueCapacity=10;
	ap.io_event_wait_min_num=5;
	memset(&(ap.timeout_io_get_event),0,sizeof(ap.timeout_io_get_event));
	memset(&(ap.io_delay),0,sizeof(ap.io_delay));
	ap.io_delay.it_interval.tv_nsec=1000000;
	ap.timeout_io_get_event.tv_nsec=2000000;

	const char* const aiop=aio_service_start(&ap,NULL);

	sleep(10);
	aio_service_stop(&aiop);
	exit(EXIT_SUCCESS);
}
