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
#include <pthread.h>
#include <sys/timerfd.h>
#include <sys/syscall.h>
#include <sys/eventfd.h>
#include <linux/aio_abi.h>
#include <error.h>

#include"include/aio_service.h"

#define errorExit(msg) \
           do { perror(msg); exit(EXIT_FAILURE); } while (0)


static inline void io_batch_submit(Aio_parameters* aiop) {
	uint64_t u=0;
	//临界区开始
	eventfd_read(aiop->efd_iocbs_ind_access,&u);
	if(aiop->iocbs_end_index>0){
		//等待已经获得席位的iocbs装入提交队列.
		eventfd_write(aiop->efd_iocbs_added,aiop->iocbs_end_index);

		if(-1==syscall(SYS_io_submit,aiop->ctx, aiop->iocbs_end_index, aiop->iocbs)){
			char s[100];
			sprintf(s,"syscall(SYS_io_submit,%lu, %d, %x).",aiop->ctx,aiop->iocbs_end_index,(unsigned int)(aiop->iocbs));
			perror(s);
			exit(EXIT_FAILURE);
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


static inline int aio_parameters_init(Aio_parameters * aiop){
	int n=aiop->aioQueueCapacity;
	aiop->eventProc=malloc(n*sizeof(EventProc));
	aiop->eventsQueue=malloc(n*sizeof(struct io_event));
	aiop->iocbs=malloc(n*sizeof(struct iocb));
	aiop->iocbs_end_index=0;

	//init efd for iocbs
	if (-1==(aiop->efd_iocbs_ind_access=eventfd(1,0))){
		perror("aiop->efd_iocbs_ind_access create failed:\n"); return -1;
	}

	if (-1==(aiop->efd_iocbs_left=eventfd(aiop->aioQueueCapacity,EFD_SEMAPHORE))){
		perror("aiop->efd_iocbs_left create failed:\n"); return -1;
	}

	if (-1==(aiop->efd_iocbs_added=eventfd(0,EFD_SEMAPHORE))){
		perror("aiop->efd_iocbs_added create failed:\n"); return -1;
	}else
		eventfd_write(aiop->efd_iocbs_added,0xfffffffffffffffe);

	//Create timerfd
	if(-1==(aiop->tfd_iocbs_sumbit=timerfd_create(CLOCK_MONOTONIC, 0))){
		perror("aiop->tfd_iocbs_sumbit create failed:\n"); return -1;
	}
	if(-1==(aiop->efd_signal=eventfd(0,EFD_SEMAPHORE))){
			perror("aiop->efd_signal create failed:\n"); return -1;
	}

	//init aio ctx
	memset(&(aiop->ctx),0, sizeof(aio_context_t));  // It's necessary
	/*Syscall IO_setup*/
	if (-1==syscall(SYS_io_setup, aiop->aioQueueCapacity, &(aiop->ctx))){
		perror("SYS_io_setup failed:\n"); return -1;
	}
printf("aio parm inited.\n");
	aiop->RUNFLAG=1;
	return 0;
}

static inline int aio_parameters_destroy(Aio_parameters * aiop){
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
		perror("failure to close aiop->tfd_iocbs_sumbit:\n");
		return -1;
	}

	free(aiop->eventProc);
	free(aiop->eventsQueue);
	free(aiop->iocbs);

	printf("aio_parameters_destroy finished.\n");
	return ret;
}

static int get_io_event(Aio_parameters * aiop){
	//get_event
		int n=0;
		unsigned min=aiop->io_event_wait_min_num, max=aiop->aioQueueCapacity;
		struct timespec *timeout=&(aiop->timeout_io_get_event);
		do{
			n=syscall(SYS_io_getevents,aiop->ctx, min, max, aiop->eventsQueue, timeout);
		    if(-1==n){
		    	perror("SYS_io_getevents failed."); return -1;
		    }else if(n<1){
		    	timeout=NULL;
		    	min=1;
		    }else if(min==1){
		    	min=aiop->io_event_wait_min_num;
		    	timeout=&(aiop->timeout_io_get_event);
		    }
		    //分派接收到的完成事件
		    //TODO dispatch
		    printf("io_event get:%d\n",n);
		}while(aiop->RUNFLAG);

		return 0;
}

int aio_service(Aio_parameters * aiop){
	if(-1==aio_parameters_init(aiop))
		return -1;

	//开辟一个线程, 用于批量提交iocbs array中的io请求.
	//create a thread for io_batch_submit(aiop);
	pthread_t* tid_aio_batch_submit=&(aiop->pt[0]);
	int err = pthread_create(tid_aio_batch_submit, NULL, (void *(*) (void *))aio_batch_submit, aiop);
	if (err != 0) {
		fprintf(stderr, "can't create thread aio_batch_submit: %s\n", strerror(err));
		return -1;
	}

	pthread_t* tid_get_io_event=&(aiop->pt[1]);
	err = pthread_create(tid_get_io_event, NULL, (void *(*) (void *))get_io_event, aiop);
	if (err != 0) {
		fprintf(stderr, "can't create thread get_io_event: %s\n", strerror(err));
		return -1;
	}

	//wait stop signal
	uint64_t n;
	while(0<eventfd_read(aiop->efd_signal,&n)){
		if(n>0)
			break;
	}

	// stop service
	aiop->RUNFLAG=0;
	void* thread_return;
	for(int i=sizeof(aiop->pt);i<0;--i){
		printf("cancel thread %d ; result: %d\n",i,pthread_cancel(aiop->pt[i]));
		printf("join thread %d ; result: %d\n",i,pthread_join(aiop->pt[i],&thread_return));
		printf("%d\n",(int)thread_return);
	}
	aio_parameters_destroy(aiop);
	printf("aio service has stopped!\n");

	return 0;
}

int aio_service_stop(Aio_parameters * aiop){
	eventfd_write(aiop->efd_signal,1);
	return 0;
}

//外部接口,用于用户提交io请求
int submit_io_task(Aio_parameters* aiop,int fd, unsigned io_cmd, void* data, void(*callback)(void*),__u64 buf,__s64 offset){
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
	if(ind==aiop->aioQueueCapacity){
		struct itimerspec z;
		memset(&z,0,sizeof(struct itimerspec));
		z.it_value.tv_nsec=1;
		timerfd_settime(aiop->tfd_iocbs_sumbit,0,&z,NULL);
	}else if(ind==1){
		//队列中加入第一个请求,启动定时器
		timerfd_settime(aiop->tfd_iocbs_sumbit,0,&(aiop->io_delay),NULL);
	}
	return 0;
}
