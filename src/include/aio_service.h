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

typedef struct AIO_PARAMETERS{
	unsigned aioQueueCapacity;
	unsigned io_event_wait_min_num;
	struct timespec timeout_io_get_event; //The waiting time of a call to io_getevent
	struct itimerspec io_delay; //the longest delay before submitting iocbs array

	unsigned RUNFLAG;

	aio_context_t ctx;
	int efd_iocbs_ind_access; // eventfd for iocbs array
	int efd_iocbs_left; //eventfd for iocbs array capacity left
	int efd_iocbs_added; //eventfd for iocbs array added num

	int tfd_iocbs_sumbit; //timerfd for iocbs batch submit
	struct iocb* iocbs;
	int iocbs_end_index;
	EventProc * eventProc;
	struct io_event *eventsQueue;

	pthread_t pt[2];
	int efd_signal;
}Aio_parameters;

extern int aio_service(Aio_parameters * aiop) __THROWNL __nonnull ((1));;
extern int submit_io_task(Aio_parameters* aiop,int fd, unsigned io_cmd, void* data, void(*callback)(void*),__u64 buf,__s64 offset) __THROWNL __nonnull ((1,4,5));
extern int aio_service_stop(Aio_parameters * aiop) __THROWNL __nonnull ((1));;

#endif /* SRC_INCLUDE_AIO_SERVICE_H_ */
