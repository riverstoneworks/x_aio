/*
 * aio_service.c
 *
 *  Created on: Oct 2, 2017
 *      Author: xpc
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/timerfd.h>
#include <sys/syscall.h>
#include <sys/eventfd.h>
#include <linux/aio_abi.h>
#include <error.h>

#include"include/aio_service.h"

#define errorExit(msg) \
           do { perror(msg); exit(EXIT_FAILURE); } while (0)

typedef struct AIO_PARAMETERS{
	Aio_param apm;
//	unsigned aioQueueCapacity;
//	unsigned io_event_wait_min_num;
//	struct timespec timeout_io_get_event; //The waiting time of a call to io_getevent
//	struct itimerspec io_delay; //the longest delay before submitting iocbs array

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
//	void *(*on_io_complete) (struct io_event *,int n); //how to deal the eventsQueue exported by io_getevents

	pthread_t pt[3];
	int efd_signal_stop_srv;
}Aio_parameters;

static inline void io_batch_submit(Aio_parameters* aiop) {
	eventfd_t u=0;
	//临界区开始
	eventfd_read(aiop->efd_iocbs_ind_access,&u);
	if(aiop->iocbs_end_index>0){
		//等待已经获得席位的iocbs装入提交队列.
		eventfd_write(aiop->efd_iocbs_added,aiop->iocbs_end_index);
		//if io_submit failed or submit num < iocbs_end_index
		for(int m=aiop->iocbs_end_index,n=0;m>0;m-=n){
			if(1>(n=syscall(SYS_io_submit,aiop->ctx, m, aiop->iocbs+n))){
				char s[100];
				sprintf(s,"syscall(SYS_io_submit,%lu, %d, %x).",aiop->ctx,aiop->iocbs_end_index,(unsigned int)(aiop->iocbs));
				perror(s);
				eventfd_write(aiop->efd_signal_stop_srv,1);
				return ;
			}
			printf("batch submit %d, left %d\n",n,m-n);
		}
		eventfd_write(aiop->efd_iocbs_left,aiop->iocbs_end_index);
		aiop->iocbs_end_index=0;
	}
	eventfd_write(aiop->efd_iocbs_ind_access,1);
	//临界区结束
}

/*
 * aio_batch_submit Daemon thread
 */
static int aio_batch_submit(Aio_parameters* aiop){
	uint64_t u;
	int n=0;
	//阻塞直到读取定时器到时计数,阻塞时长为定时时长
	while(-1!=(n=read(aiop->tfd_iocbs_sumbit,&u,sizeof(uint64_t)))){
		//以下判断为了应对 队列满的时候,定时器正好到期,如果此时read,然后设置timer立即到期的逻辑紧接着运行.这样可能就会连续执行两次read,而其实是针对同一次队列提交操作.加入席位占用判断,确定是否有需要处理的io请求,避免死锁风险
		if(aiop->iocbs_end_index<1)
			continue;
		io_batch_submit(aiop);
		//TODO 调时策略
	}

	return n<0?-1:0;
}


static Aio_parameters* aio_parameters_init(const Aio_param ap){

	Aio_parameters * aiop=malloc(sizeof(Aio_parameters));
	aiop->apm=ap;
//	aiop->aioQueueCapacity=ap->aioQueueCapacity;
//	aiop->io_event_wait_min_num=ap->io_event_wait_min_num;
//	aiop->io_delay=ap->io_delay;
//	aiop->timeout_io_get_event=ap->timeout_io_get_event;
//	aiop->on_io_complete=ap->on_io_complete;

	int n=aiop->apm.aioQueueCapacity;
	aiop->eventProc=malloc(n*sizeof(EventProc));
	aiop->eventsQueue=malloc(n*sizeof(struct io_event));
	aiop->iocbs=malloc(n*sizeof(struct iocb));
	aiop->iocbs_end_index=0;

	//init efd for iocbs
	if (-1==(aiop->efd_iocbs_ind_access=eventfd(1,0))){
		perror("aiop->efd_iocbs_ind_access create failed:\n"); free(aiop);return NULL;
	}

	if (-1==(aiop->efd_iocbs_left=eventfd(aiop->apm.aioQueueCapacity,EFD_SEMAPHORE))){
		perror("aiop->efd_iocbs_left create failed:\n"); free(aiop);return NULL;
	}

	if (-1==(aiop->efd_iocbs_added=eventfd(0,EFD_SEMAPHORE))){
		perror("aiop->efd_iocbs_added create failed:\n"); free(aiop);return NULL;
	}else
		eventfd_write(aiop->efd_iocbs_added,0xfffffffffffffffe);

	//Create timerfd
	if(-1==(aiop->tfd_iocbs_sumbit=timerfd_create(CLOCK_MONOTONIC, 0))){
		perror("aiop->tfd_iocbs_sumbit create failed:\n"); free(aiop);return NULL;
	}
	if(-1==(aiop->efd_signal_stop_srv=eventfd(0,EFD_SEMAPHORE))){
			perror("aiop->efd_signal create failed:\n"); free(aiop);return NULL;
	}

	//init aio ctx
	memset(&(aiop->ctx),0, sizeof(aio_context_t));  // It's necessary
	/*Syscall IO_setup*/
	if (-1==syscall(SYS_io_setup, aiop->apm.aioQueueCapacity, &(aiop->ctx))){
		perror("SYS_io_setup failed:\n"); free(aiop);return NULL;
	}
printf("aio parm inited.\n");
	aiop->RUNFLAG=1;
	return aiop;
}

static int aio_parameters_destroy(Aio_parameters * aiop){
	int ret=0;
	if(-1==syscall(SYS_io_destroy,aiop->ctx)){
		perror("failure to destroy aiop->ctx:\n"); ret= -1;
	}
	if(-1==close(aiop->efd_iocbs_added)){
		perror("failure to close aiop->efd_iocbs_added:\n"); ret= -1;
	}
	if(-1==close(aiop->efd_iocbs_ind_access)){
		perror("failure to close aiop->efd_iocbs_ind_access:\n"); ret= -1;
	}
	if(-1==close(aiop->efd_iocbs_left)){
		perror("failure to close aiop->efd_iocbs_left:\n"); ret= -1;
	}
	if(-1==close(aiop->tfd_iocbs_sumbit)){
		perror("failure to close aiop->tfd_iocbs_sumbit:\n");ret= -1;
	}
	if(-1==close(aiop->efd_signal_stop_srv)){
		perror("failure to close aiop->efd_signal:\n");ret= -1;
	}

	free(aiop->eventProc);
	free(aiop->eventsQueue);
	free(aiop->iocbs);

	free(aiop);

	printf("aio_parameters_destroy finished.\n");
	return ret;
}

static void sighandler_getIoEvent(int x){
	printf("cancel thread getIoEvent. result: %d\n",pthread_cancel(pthread_self()));
}
static int get_io_event(Aio_parameters * aiop) {
	//register signal for interrupt io_getevent
	signal(SIGIO,(__sighandler_t)sighandler_getIoEvent);
	//get_event
	int n = 0;
	unsigned min = aiop->apm.io_event_wait_min_num, max = aiop->apm.aioQueueCapacity;
	struct timespec *timeout = &(aiop->apm.timeout_io_get_event);
	do {
		//NOTE:can not wake up io_getevents from block with pthread_cancel(), must send signal (such as SIGIO etc.)
		n = syscall(SYS_io_getevents, aiop->ctx, min, max, aiop->eventsQueue,
				timeout);
		printf("io_event get:%d\n", n);
		if(n>0){
			//deal with io_event array exported by io_getevents
			aiop->apm.on_io_complete(aiop->eventsQueue,n);
			if(min == 1){
				min = aiop->apm.io_event_wait_min_num;
				timeout = &(aiop->apm.timeout_io_get_event);
			}
		}else if(n<1){
			timeout = NULL;
			min = 1;
		}else if(-1==n){
			perror("SYS_io_getevents failed.\n");
		}

	} while (1);

	return 0;
}

static int aio_service(Aio_parameters * aiop){
	//开辟一个线程, 用于批量提交iocbs array中的io请求.
	//create a thread for io_batch_submit(aiop);
	pthread_t* tid_aio_batch_submit=&(aiop->pt[1]);
	int err = pthread_create(tid_aio_batch_submit, NULL, (void *(*) (void *))aio_batch_submit, aiop);
	if (err != 0) {
		fprintf(stderr, "can't create thread aio_batch_submit: %s\n", strerror(err));
		return -1;
	}

	pthread_t* tid_get_io_event=&(aiop->pt[2]);
	err = pthread_create(tid_get_io_event, NULL, (void *(*) (void *))get_io_event, aiop);
	if (err != 0) {
		fprintf(stderr, "can't create thread get_io_event: %s\n", strerror(err));
		return -1;
	}

	//wait stop signal
	eventfd_t n=0;
	while(1){
		int x=eventfd_read(aiop->efd_signal_stop_srv,&n);
		if(x==0)
			break;
		else{
			perror("eventfd_read(aiop->efd_signal,&n)\n");
			break;
		}
	}

	// stop service
	aiop->RUNFLAG=0;
	void* thread_return;

	/*
	 * send signal to thread get_io_event to interrupt io_getevent,
	 * then cancel thread itself with a signal Handler predefined
	 */
	printf("kill thread %lu ; result: %d\n",*tid_get_io_event,pthread_kill(*tid_get_io_event,SIGIO));
	printf("join thread %lu ; result: %d\n",*tid_get_io_event,pthread_join(*tid_get_io_event,&thread_return));

	sleep(3);

	printf("cancel thread %lu ; result: %d\n",*tid_aio_batch_submit,pthread_cancel(*tid_aio_batch_submit));
	printf("join thread %lu ; result: %d\n",*tid_aio_batch_submit,pthread_join(*tid_aio_batch_submit,&thread_return));

	printf("aio service has stopped!\n");
	return 0;
}
/*
 * thread_band_cpu_num : array
 */
const char* aio_service_start(const Aio_param ap,cpu_set_t* cpuset) {
	Aio_parameters* aiop=aio_parameters_init(ap);
	if(NULL==aiop)	{perror("aiop init failed!.\n");return NULL;}

	pthread_t *th_aio_service=&aiop->pt[0];
	pthread_attr_t attr,*attrp=NULL;
	pthread_attr_init(&attr);
	if(cpuset!=NULL){
		pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
		pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), cpuset); //绑定逻辑核心
		attrp = &attr;
	}
	if (-1== pthread_create(th_aio_service, attrp,	(void *(*)(void *)) aio_service, aiop))
		perror("pthread_create(th_aio_service, &attr, (void *(*) (void *))aio_service_start, aiop).");
	else
		printf("aio_service_started.\n");

	pthread_attr_destroy(&attr);

	return (const char*)aiop;
}

int aio_service_stop(const char * const aiop){
	Aio_parameters* p=(Aio_parameters*)aiop;
	eventfd_write(p->efd_signal_stop_srv,1);
	pthread_join(p->pt[0],NULL);
	aio_parameters_destroy(p);
	return 0;
}

//外部接口,用于用户提交io请求
int submit_io_task(char* aps,int fd, unsigned io_cmd, void* data, void(*callback)(void*),__u64 buf,__s64 offset){
	Aio_parameters * const aiop=(Aio_parameters*)aps;
	if(!aiop->RUNFLAG){ printf("AIO service is not running!");return -1;}

	uint64_t n=0;
	if(eventfd_read(aiop->efd_iocbs_left,&n)!=0) //eventfd-1 使用信号量模式
		return -1||printf("Failed read efd");

	//iocbs array index临界区开始
	if(eventfd_read(aiop->efd_iocbs_ind_access,&n)!=0)
		return -1||printf("Failed read efd");

	int ind=aiop->iocbs_end_index++;

	if(eventfd_write(aiop->efd_iocbs_ind_access,1)!=0)
		return -1||printf("Failed write efd");
	//临界区结束

	struct iocb *p=aiop->iocbs+ind;
    memset(p,0,sizeof(struct iocb));

    EventProc *ep=aiop->eventProc+ind;
    ep->callback=callback;
    ep->data=data;

    p->aio_data       = (__u64)ep;
    p->aio_lio_opcode = io_cmd;//IO_CMD_PWRITE; //
    p->aio_reqprio    = sysconf(_SC_ARG_MAX);//sysconf(_SC_ARG_MAX);
    p->aio_fildes     = fd;
    p->aio_buf    = (__u64)buf;
    p->aio_nbytes = sysconf(_SC_PAGESIZE);//这个值必须按page字节对齐
    p->aio_offset = offset; // 这个值必须按page字节对齐

    // the num of pending iocbs +1
	if(eventfd_read(aiop->efd_iocbs_added,&n)!=0)
		return -1||printf("Failed read efd");

	//若待提交队列已满 修改timer, 立即唤醒线程io_batch_submit
	if(ind==aiop->apm.aioQueueCapacity){
		struct itimerspec z;
		memset(&z,0,sizeof(struct itimerspec));
		z.it_value.tv_nsec=1;
		timerfd_settime(aiop->tfd_iocbs_sumbit,0,&z,NULL);
	}else if(ind==1){
		//队列中加入第一个请求,启动定时器
		timerfd_settime(aiop->tfd_iocbs_sumbit,0,&(aiop->apm.io_delay),NULL);
	}
	return 0;
}
