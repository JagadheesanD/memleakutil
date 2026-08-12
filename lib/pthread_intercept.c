/*
 * Copyright [2026] [Jagadheesan.D@gmail.com]
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
//#include <stddef.h>
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>
//#include <unistd.h>
#include "memfns_wrap.h"

void intercept_pthread_create();
typedef int (*pthread_create_type)(pthread_t*, const pthread_attr_t *,  void* (*start_routine)(void*), void*);

pthread_create_type pthread_create_fnptr = NULL;
LIST_pthread *pthreadhead, *pthreadtail;
STATIC pthread_mutex_t pthreadlock;

void sendPthreadIntercept(mqd_t mqsend)
{
	msg_resp msgresp;
	// Plan is to write this as header for checking compatibility during offline analysis
	hp_walk_header hpwHdr = {MEMWRAP_MSG_RESP_VERSION,0};

	pthread_mutex_lock(&pthreadlock);
	LIST_pthread *tmp = pthreadhead;

	msgresp.numItemOrInfo = HEAPWALK_EMPTY;
	if (NULL == tmp) {
		mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);
	}
	else {
		while (tmp)
		{
			if (tmp->pthread_id) {
				pthread_attr_t attr;
				void *stackaddr;
				size_t stacksize;

				if (0 == pthread_getattr_np(tmp->pthread_id, &attr)) {
					if (0 == pthread_attr_getstack(&attr, &stackaddr, &stacksize))  {
						dbg(PRINT_INFO, "still alive, pthread %ld: Stack %p Size %lu\n", tmp->pthread_id, stackaddr, stacksize);
					}
					else {
						dbg(PRINT_ERROR, "pthread exited?? %ld: Stack %p Size %lu\n", tmp->pthread_id, tmp->stack_addr_bottom, tmp->size);
						tmp->pthread_id = 0; // TODO 0 can be valid id
					}
				}
				else {
					dbg(PRINT_ERROR, "pthread_attr_init failed!! %d[%s]\n", errno, strerror(errno));
					dbg(PRINT_ERROR, "%ld pthread exited?? Stack %p Size %lu\n", tmp->pthread_id, tmp->stack_addr_bottom, tmp->size);
					tmp->pthread_id = 0; // TODO 0 can be valid id
				}
			}
			msgresp.xfer[msgresp.numItemOrInfo].pthread_id = tmp->pthread_id;
			msgresp.xfer[msgresp.numItemOrInfo].stack_addr_bottom = tmp->stack_addr_bottom;
			msgresp.xfer[msgresp.numItemOrInfo].start_routine = tmp->start_routine;
			msgresp.xfer[msgresp.numItemOrInfo].size = tmp->size;
			msgresp.xfer[msgresp.numItemOrInfo++].seconds = tmp->time;
			hpwHdr.totalEntries++;

			// Check if we've reached max size to transfer
			if (MAX_MSG_XFER <= msgresp.numItemOrInfo) {
				if (tmp->next) {
					msgresp.numItemOrInfo |= HEAPWALK_ITEM_CONTN;
				}
				else {
					msgresp.numItemOrInfo |= HEAPWALK_ENDOF_LIST;
					mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp),0);
				}
				msgresp.numItemOrInfo = HEAPWALK_EMPTY;
			}
			tmp = tmp->next;
		}
		if (msgresp.numItemOrInfo) { 
			msgresp.numItemOrInfo |= HEAPWALK_ENDOF_LIST;
			mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);
		}
	}
	pthread_mutex_unlock(&pthreadlock);
}

void dispPthreadList()
{
	pthread_mutex_lock(&pthreadlock);
	LIST_pthread *listPtr = pthreadhead;
	while (listPtr) {
		PRINT("Id: %lx stack: %p size: %lx start_routine: %p time: %lu\n",
		listPtr->pthread_id, listPtr->stack_addr_bottom, listPtr->size, listPtr->start_routine, listPtr->time);
		listPtr = listPtr->next;	
	}
	pthread_mutex_unlock(&pthreadlock);
}

void appendItemToPthreadList(pthread_t id, void *addr, unsigned long size, void *start_routine)
{
	LIST_pthread *listPtr;
	listPtr = (LIST_pthread *)malloc(sizeof(LIST_pthread));
	if (listPtr) {
		listPtr->pthread_id = id;
		listPtr->stack_addr_bottom = addr;
		listPtr->size = size;
		listPtr->start_routine = start_routine;
		listPtr->time = time(0);
		pthread_mutex_lock(&pthreadlock);
		if (pthreadtail) {
			pthreadtail->next = listPtr;
			pthreadtail = listPtr;
		}
		else {
			pthreadhead = pthreadtail = listPtr;
		}
		pthread_mutex_unlock(&pthreadlock);
	}
	else {
		dbg(PRINT_ERROR, "%s: Error allocating %d [%s]\n", __FUNCTION__, errno, strerror(errno));
	}
}

__attribute__((visibility("default")))
int pthread_create(pthread_t *thread, const pthread_attr_t *attrs, void* (*start_routine)(void*), void *arg) {
	int pthread_ret;
	if (NULL != pthread_create_fnptr) {
		pthread_ret = pthread_create_fnptr(thread, attrs, start_routine, arg);
	}
	else {
		intercept_pthread_create();
		if (NULL != pthread_create_fnptr) {
			pthread_ret = pthread_create_fnptr(thread, attrs, start_routine, arg);
		}
		else {
			dbg(PRINT_ERROR, "Couldn't intercept pthread...exiting..\n");
			exit(0);
		}
	}
	if (0 == pthread_ret) {
		pthread_attr_t attr;
		void *stackaddr;
		size_t stacksize;

		if (0 == pthread_getattr_np(*thread, &attr)) {
			if (0 == pthread_attr_getstack(&attr, &stackaddr, &stacksize))  {
				dbg(PRINT_INFO, "pthread %ld: Stack %p Size %lu\n", *thread, stackaddr, stacksize);
				//appendItemToPthreadList(*thread, stackaddr, stacksize, __builtin_return_address(0));
				appendItemToPthreadList(*thread, stackaddr, stacksize, (void*)start_routine);
			}
		}
		else {
			dbg(PRINT_ERROR, "pthread_attr_init failed!! %d[%s]\n", errno, strerror(errno));
		}
	}
	else {
		dbg(PRINT_ERROR, "pthread_create failed..%d[%s]\n", errno, strerror(errno));
	}
	return pthread_ret;
}

__attribute__((constructor))
void intercept_pthread_create() {

	if (NULL == pthread_create_fnptr) {
		pthread_create_fnptr = dlsym(RTLD_NEXT, "pthread_create");
		if (pthread_create_fnptr) {
		        pthread_mutexattr_t mutexattr;
		        pthread_mutexattr_init(&mutexattr);
		        pthread_mutexattr_settype(&mutexattr, PTHREAD_MUTEX_RECURSIVE);
		        pthread_mutex_init(&pthreadlock, &mutexattr);
		}
	}
	printf("At intercept_pthread_create, %p\n", pthread_create_fnptr);
}

