/*
 * aio_service.h
 *
 *  Created on: Oct 2, 2017
 *      Author: xpc
 */

#ifndef SRC_INCLUDE_AIO_SERVICE_H_
#define SRC_INCLUDE_AIO_SERVICE_H_

typedef struct EVENT_PROC{
	void* data;
	void(*callback)(void*);
}EventProc;

typedef struct AIO_PARAM{
	unsigned aioQueueCapacity;
	unsigned io_event_wait_min_num;
	struct timespec timeout_io_get_event; //The waiting time of a call to io_getevent
	struct itimerspec io_delay; //the longest delay before submitting iocbs array
	void *(*on_io_complete) (struct io_event *,int n); //how to deal the eventsQueue exported by io_getevents
}Aio_param;

const char* aio_service_start(const Aio_param * const ap,cpu_set_t* cpuset) __THROWNL __nonnull ((1));;
extern int submit_io_task(char* aiop,int fd, unsigned io_cmd, void* data, void(*callback)(void*),__u64 buf,__s64 offset) __THROWNL __nonnull ((1,4,5));
extern int aio_service_stop(const char* const aiop) __THROWNL __nonnull ((1));;

#endif /* SRC_INCLUDE_AIO_SERVICE_H_ */
