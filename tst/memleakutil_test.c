#ifndef SELF_TEST

#error "Define SELF_TEST"

#else

#define _GNU_SOURCE
#include <pthread.h>
#include <mqueue.h>

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>


#include <semaphore.h>
#include <sys/mman.h>
#include "memfns_wrap.h"

extern mqd_t createMq(void);
extern void storeHeapwalk(mqd_t mqrecv, int cmd, int pid, bool isSelfTest);
extern void processHeapwalk(int cmd, int pid, int tid, bool isSelfTest, LIST *resp, int *listIndex, MMAP_anon *mmapIn);

/* Just run a test thread, that allocates and deallocates, so that
 * a testrun shall be done to see heap walk and other options */
static void* test_thread_start(void *arg)
{
	char *x[8];
	int alloc = 1;
	int index = 0;
	int list_size = 3;
#if defined(__USE_XOPEN2K)
	list_size++;
#endif
#if defined(__USE_ISOC11)
	list_size++;
#endif
#if defined(USE_DEPRECATED_MEMALIGN)    
	list_size++;
#endif
	while (1)
	{
		if (1 == alloc) {
			if (index < list_size)
			{
				x[index++] = malloc(sizeof(int));
				dbg(PRINT_MUST, "x[%d] is malloc'd %p\n", index-1, x[index-1]);
				sleep(5);
				x[index++] = calloc(1,sizeof(int));
				dbg(PRINT_MUST, "x[%d] is calloc'd %p\n", index-1, x[index-1]);
				sleep(5);
				x[index] = realloc(x[index-2], 16);
				x[index-2] = 0;
				index++;
				dbg(PRINT_MUST, "x[%d] is realloc'd %p\n", index-1, x[index-1]);
				sleep(5);
#if defined(__USE_XOPEN2K)
				int memaligned = posix_memalign((void**)&x[index++], 16, 32);
				dbg(PRINT_MUST, "x[%d] is posix memalign'd %p, %d\n", index-1, x[index-1], memaligned);
				sleep(5);
#endif
#if defined(__USE_ISOC11)
				x[index++] = aligned_alloc(32, 64);
				dbg(PRINT_MUST, "x[%d] is aligned alloc %p\n", index-1, x[index-1]);
				sleep(5);
#endif
#if defined(USE_DEPRECATED_MEMALIGN)    
				x[index++] = memalign(64, 64);
				dbg(PRINT_MUST, "x[%d] is memalign %p\n", index-1, x[index-1]);
				sleep(5);
#endif
			}
			else {
				alloc = 0;
			}
		}
		else {
			sleep(10);
			for (int i=0; i<list_size; i++) {
				if (x[i]) 
				{
					dbg(PRINT_MUST, "Freeing x[%d] %p\n", i, x[i]);
					free(x[i]);
					x[i] = 0;
				}else{
					dbg(PRINT_MUST, "Not Freeing x[%d] %p\n", i, x[i]);
				}
			}
			alloc = 1;
			index = 0;
		}
	}
	return NULL;
}

void spawntestrunthread()
{
        pthread_t ptd;
	pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 8*1024);

        pthread_create(&ptd, &attr, &test_thread_start, NULL);
}

#ifndef MAINTAIN_SINGLE_LIST
/*
Having the below 2 functions redundantly here because in selftest, certain tests are failing due to 
compiler optimization, since they were present in library. In places where memhead, memtail is swapped with wmemhead and wmemtail, 
the old values are retained, thus fails. Tried volatile, -O0 optimization, but didn't help
 */
void heapwalkMarkalllocal()
{
	pthread_mutex_lock(&lock);
	if (wmemtail){
		wmemtail->next = memhead;
		if (memhead) {
#ifdef PREPEND_LISTDATA 
			memhead->prev = wmemtail;
#endif                  
			wmemtail = memtail;
		}
	}
	else {
		wmemhead = memhead;
		wmemtail = memtail;
	}

	memhead = memtail = NULL;
	pthread_mutex_unlock(&lock);
}

void heapwalkResetLocal()
{
	pthread_mutex_lock(&lock);
	if (memhead){
		if (wmemtail) {
			wmemtail->next = memhead;
#ifdef PREPEND_LISTDATA
			memhead->prev = wmemtail;
#endif
			memhead = wmemhead;
		}
	}
	else {
		memhead = wmemhead;
		memtail = wmemtail;
	}
	wmemhead = wmemtail = NULL;
	pthread_mutex_unlock(&lock);
}
#endif

void sendAndRecv(mqd_t mq, int cmd, LIST *resp, int listSize, int initVal)
{
	msg_resp msgresp;

	switch (cmd) {
		case HEAPWALK_INCREMENT:
		case HEAPWALK_FULL:
#ifdef OPTIMIZE_MQ_TRANSFER
		case HEAPWALK_MMAP_ENTRIES:
#endif
			{
			mqd_t mqsend;
			msg_cmd msgcmd;
			char mq_name[64];

			memset(resp, initVal, listSize*sizeof(LIST));
			msgcmd.pid = getpid();	
			sprintf(mq_name, "/mq_wrapper_%d", msgcmd.pid);
			mqsend = mq_open(mq_name, O_WRONLY);
			if(mqsend < 0) {
				dbg(PRINT_ERROR, "Error, cannot open the queue: %s, error: %s.\n", mq_name, strerror(errno));
				return;
			}
			msgcmd.cmd = cmd;
			dbg(PRINT_MSGQ, "%s: sending cmd %d on mq %s\n", __FUNCTION__, msgcmd.cmd, mq_name);
			if (-1 == mq_send(mqsend, (const char *)&msgcmd, sizeof(msg_cmd), 0)){
				dbg(PRINT_ERROR, "msgsnd failed, %s\n", strerror(errno));
				mq_close(mqsend);
				return;
			}
			mq_close(mqsend);
#ifndef OPTIMIZE_MQ_TRANSFER
			unsigned int prio;
			int msgsize = sizeof(msg_resp);
			int listIndex = 0;
			while (0 != msgsize){
				dbg(PRINT_MSGQ, "Message receive %s\n", __FUNCTION__);
				msgsize = mq_receive(mq, (char*)&msgresp, sizeof(msg_resp), &prio);
				if (msgsize && (-1 == msgresp.seq)){
					resp[listIndex].ptr = NULL;
					dbg(PRINT_MSGQ, "End of Message\n");
					break;
				}
				if (listSize > listIndex) {
					if (!strcmp(msgresp.msg, "No new allocations") ||
							!strcmp(msgresp.msg, "Already walked:") ||
							!strcmp(msgresp.msg, "New allocations:")) {
						dbg(PRINT_MSGQ, "Info messages\n");
					}else {
						sscanf(msgresp.msg, "0x%p %d", &resp[listIndex].ptr, &resp[listIndex].size);
						listIndex++;
					}
				}
				else {
					dbg(PRINT_MUST, "Increase resp size..\n");
					dbg(PRINT_MUST, "%d [%d: %d: %s]\n",listIndex, msgsize, msgresp.seq, msgresp.msg);
					mq_close(mqsend);
					return;
				}
				dbg(PRINT_MSGQ, "%d [%d: %d: %s]\n",listIndex, msgsize, msgresp.seq, msgresp.msg);
			}
#else
			sleep(1);
			storeHeapwalk(mq, msgcmd.cmd, msgcmd.pid, 1);
			int xferIndex = 0;
			processHeapwalk(msgcmd.cmd, msgcmd.pid, 0, 1, resp, &xferIndex, NULL);
#endif
			}
			break;

		case HEAPWALK_MARKALL:
#ifdef MAINTAIN_SINGLE_LIST
			heapwalkMarkall();
#else
			heapwalkMarkalllocal();
#endif
			break;

		case HEAPWALK_RESET_MARKED:
#ifdef MAINTAIN_SINGLE_LIST
			heapwalkReset();
#else
			heapwalkResetLocal();
#endif
			break;

		case HEAPWALK_BASE:
			{
				struct   timespec tm;
				unsigned int prio;
				clock_gettime(CLOCK_REALTIME, &tm);
				tm.tv_sec += 3;
				errno = 0;
				(void)mq_timedreceive(mq, (char*)&msgresp, sizeof(msg_resp), &prio, &tm);
				if (ETIMEDOUT == errno) {
					dbg(PRINT_FATAL, "Error, mq_receive: %s.[%d]\n", strerror(errno),errno);
					resp[0].ptr = (int*)0xff;
				}
				else {
					dbg(PRINT_FATAL, "Error, mq_receive: %s.[%d]\n", strerror(errno),errno);
				}
			}
			break;


		default:
			dbg(PRINT_INFO, "Invalid cmd %d\n", cmd);
			break;

	}
}

void runAllocationTests(mqd_t mq)
{
	resetList();
	LIST resp[8];
	int passed=0, failed=0;
#ifdef PREPEND_LISTDATA
	int listSize = sizeof(LIST);
#else
	int listSize = 0;
#endif
	int testnum = 1;

	char *z = malloc(27);
	dbg(PRINT_MUST, "\n**********************************\n%s: %d\n**********************************\n", __FUNCTION__, getpid());
	memset(z,0,27);
	strcpy(z, "abcdefghijklmnopqrstuvwxyz");
	dbg(PRINT_ERROR, "%d. [%d] Show %p,%d,%s\n", testnum++,__LINE__, z, 26, z);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if ((z == (char*)resp[0].ptr) && (27 == resp[0].size) && (!strcmp(z,(char*)resp[0].ptr))) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\t%d: Fail %p,%d,%s\n", __LINE__, resp[0].ptr, resp[0].size, (char*)resp[0].ptr);
		failed++;
	}

#ifdef OPTIMIZE_MQ_TRANSFER
	// In sendAndRecv, fopen might use heap, therefore get the current ptr
	char *z1 = &gInitialAlloc[gInitIndex] + listSize;
#else
	char *z1 = z + 32 + listSize;
#endif
	
	z = realloc(z, 72);
	memset(&z[25],0,45);
	strcpy(&z[25], "1234567890");
	dbg(PRINT_ERROR, "%d. [%d] Show %p,%d,%s\n", testnum++,__LINE__, (0 < gMemInitialized)?z:z1, 72, z);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if (((0 < gMemInitialized)? 1 : (z1 == (char*)resp[0].ptr)) && (72 == resp[0].size) && (!strcmp(z,(char*)resp[0].ptr))) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\t%d: Fail %p,%d,%s\n", __LINE__, resp[0].ptr, resp[0].size, (char*)resp[0].ptr);
		failed++;
	}

#ifdef OPTIMIZE_MQ_TRANSFER
	z1 =  &gInitialAlloc[gInitIndex] + listSize;
#else
	z1 = z + 72 + listSize;
#endif
	free(z);
	z = realloc(NULL, 83);
	memset(z,0,83);
	strcpy(z, "abcdefghijklmnopqrstuvwxyz1234567890abcdefghijklmnopqrstuvwxyz");
	dbg(PRINT_ERROR, "%d. [%d] Show %p,%d,%s\n", testnum++,__LINE__, (0 < gMemInitialized)?z:z1, 83, z);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if (((0 < gMemInitialized)?1:(z1 == (char*)resp[0].ptr)) && (83 == resp[0].size) && (!strcmp(z,(char*)resp[0].ptr))) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\t%d: Fail %p,%d,%s\n", __LINE__, resp[0].ptr, resp[0].size, (char*)resp[0].ptr);
		failed++;
	}

#ifdef OPTIMIZE_MQ_TRANSFER
	z1 =  &gInitialAlloc[gInitIndex] + listSize;
#else
	z1 = z + 88 + listSize;
#endif
	free(z);
	z = calloc(1, 32);
	memset(z,0,32);
	strcpy(z, "abcdefghijklmnopqrstuvwxyz01234");
	dbg(PRINT_ERROR, "%d. [%d] Show %p,%d,%s\n", testnum++,__LINE__, (0 < gMemInitialized)?z:z1, 32, z);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if (((0 < gMemInitialized)?1:(z1 == (char*)resp[0].ptr)) && (32 == resp[0].size) && (!strcmp(z,(char*)resp[0].ptr))) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		// This will fail, since we don't display nmem blocks in heapwalk!! we need to change the approach
		dbg(PRINT_ERROR, "\t%d: Fail %p,%d,%s\n", __LINE__, resp[0].ptr, resp[0].size, (char*)resp[0].ptr);
		failed++;
	}

#ifdef OPTIMIZE_MQ_TRANSFER
	z1 =  &gInitialAlloc[gInitIndex] + listSize;
#else
	z1 = z + 32 + listSize;
#endif
	z = realloc(z, 40);
	memset(&z[31],0,8);
	strcpy(&z[31], "567890a");
	dbg(PRINT_ERROR, "%d. [%d] Show %p,%d,%s\n", testnum++,__LINE__, (0 < gMemInitialized)?z:z1, 40, z);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if (((0 < gMemInitialized)?1:(z1 == (char*)resp[0].ptr)) && (40 == resp[0].size) && (!strcmp(z,(char*)resp[0].ptr))) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\t%d: Fail %p,%d,%s\n", __LINE__, resp[0].ptr, resp[0].size, (char*)resp[0].ptr);
		failed++;
	}

#ifdef OPTIMIZE_MQ_TRANSFER
	z1 =  &gInitialAlloc[gInitIndex] + listSize;
#else
	z1 = z + 40 + listSize;
#endif
	free(z);
	z = calloc(2, 16);
	memset(z,0,32);
	strcpy(z, "abcdefghijklmnopqrstuvwxyz01234");
	dbg(PRINT_ERROR, "%d. [%d] Show %p,%d,%s\n", testnum++,__LINE__, (0 < gMemInitialized)?z1:z, 32, z);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if (((0 < gMemInitialized)?1:(z1 == (char*)resp[0].ptr)) && (32 == resp[0].size) && (!strcmp(z,(char*)resp[0].ptr))) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\t%d: Fail %p,%d,%s\n", __LINE__, resp[0].ptr, resp[0].size, (char*)resp[0].ptr);
		failed++;
	}

#ifdef OPTIMIZE_MQ_TRANSFER
	z1 =  &gInitialAlloc[gInitIndex] + listSize;
#else
	z1 = z + 32 + listSize;
#endif
	z = realloc(z, 40);
	memset(&z[31],0,8);
	strcpy(&z[31], "567890a");
	dbg(PRINT_ERROR, "%d. [%d] Show %p,%d,%s\n", testnum++,__LINE__, (0 < gMemInitialized)?z:z1, 40, z);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if (((0 < gMemInitialized)?1:(z1 == (char*)resp[0].ptr)) && (40 == resp[0].size) && (!strcmp(z,(char*)resp[0].ptr))) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\t%d: Fail %p,%d,%s\n", __LINE__, resp[0].ptr, resp[0].size, (char*)resp[0].ptr);
		failed++;
	}
	z = realloc(z, 0);
#ifdef OPTIMIZE_MQ_TRANSFER
	z =  &gInitialAlloc[gInitIndex] + listSize;
#else
	z = z1 + 40 + listSize;
#endif
	z1 = realloc(NULL, 40);
	strcpy(z1, "abcdefghijklmnopqrstuvwxyz1234567890");
	dbg(PRINT_ERROR, "%d. [%d] Show %p,%d,%s\n", testnum++,__LINE__, (0 < gMemInitialized)?z1:z, 40, z1);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if (((0 < gMemInitialized)?(z1 == (char*)resp[0].ptr):(z == (char*)resp[0].ptr)) && (40 == resp[0].size) && (!strcmp(z1,(char*)resp[0].ptr))) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\t%d: Fail %p,%d,%s\n", __LINE__, resp[0].ptr, resp[0].size, (char*)resp[0].ptr);
		failed++;
	}
	z = memalign(64, 40);
	free(z1);
#if 0 //def PREPEND_LISTDATA
	unsigned int alignment = 64;
	unsigned int size = 40;
	unsigned int newSize;
        if (listSize < alignment) {
                newSize = size + alignment;
        } else {
                newSize = size + listSize + (listSize % alignment);
        }
#endif
	
	//z1 = z1 + 40 + listSize;
	strcpy(z, "abcdefghijklmnopqrstuvwxyz1234567890");
	dbg(PRINT_ERROR, "%d. [%d] Show %p,%d,%s,mod(z,64)=0\n", testnum++,__LINE__, z, 40, z);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if ((z == (char*)resp[0].ptr) && (40 == resp[0].size) && (!strcmp(z,(char*)resp[0].ptr)) && !((unsigned long)z%64)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\t%d: Fail %p,%d,%s,%lu\n", __LINE__, resp[0].ptr, resp[0].size, (char*)resp[0].ptr, ((unsigned long)z%64));
		failed++;
	}
	z1 = memalign(32, 64);
	free(z);
	//z = z1 + 64 + listSize;
	strcpy(z1, "abcdefghijklmnopqrstuvwxyz123456abcdefghijklmnopqrstuvwxyz12345");
	dbg(PRINT_ERROR, "%d. [%d] Show %p,%d,%s,mod(z,32)=0\n", testnum++,__LINE__, z1, 64, z1);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if ((z1 == (char*)resp[0].ptr) && (64 == resp[0].size) && (!strcmp(z1,(char*)resp[0].ptr)) && !((unsigned long)z1%32)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\t%d: Fail %p,%d,%s,%lu\n", __LINE__, resp[0].ptr, resp[0].size, (char*)resp[0].ptr, ((unsigned long)z1%32));
		failed++;
	}
	free(z1);
	z = memalign(128, 27);
	strcpy(z, "abcdefghijklmnopqrstuvwxyz");
	dbg(PRINT_ERROR, "%d. [%d] Show %p,%d,%s,mod(z,128)=0\n", testnum++,__LINE__, z, 27, z);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if ((z == (char*)resp[0].ptr) && (27 == resp[0].size) && (!strcmp(z,(char*)resp[0].ptr)) && !((unsigned long)z%128)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\t%d: Fail %p,%d,%s,%lu\n", __LINE__, resp[0].ptr, resp[0].size, (char*)resp[0].ptr, ((unsigned long)z%128));
		failed++;
	}
	z1 = memalign(4096, 64);
	strcpy(z1, "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz0123456789");
	dbg(PRINT_ERROR, "%d. [%d] Show %p,%d,%s,mod(z,4096)=0\n", testnum++,__LINE__, z1, 64, z1);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if ((z1 == (char*)resp[0].ptr) && (64 == resp[0].size) && (!strcmp(z1,(char*)resp[0].ptr)) && !((unsigned long)z1%4096)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\t%d: Fail %p,%d,%s,%lu\n", __LINE__, resp[0].ptr, resp[0].size, (char*)resp[0].ptr, ((unsigned long)z1%4096));
		failed++;
	}

	dbg(PRINT_MUST, "Total test cases: %d [Pass %d Fail %d]\n", passed+failed, passed, failed);

	FILE *fp = fopen("/tmp/memleakutil_selftest.txt", "a");
	if (NULL != fp) {
		fprintf(fp, "%d:\t\t%s: Pass %d Fail %d\n", 
				getpid(), __FUNCTION__, passed, failed);
		fclose(fp);
	}
	sleep (3);
}

void runListTests(mqd_t mq)
{
	resetList();
	LIST resp[8];
	int *x[10];
	int passed=0, failed=0;
	int testnum = 1;

	dbg(PRINT_MUST, "\n**********************************\n%s: %d\n**********************************\n", __FUNCTION__, getpid());
	x[0] = malloc(8);

#ifdef MAINTAIN_SINGLE_LIST
	dbg(PRINT_ERROR, "\n%d. [%d] Show ptr[%p] = hpfmemhead->ptr[%p] = hpwmemhead->ptr[%p] = hpfmemtail->ptr[%p], [NULL] hpfmemhead->next[%p] hpfmemtail->next[%p]\n", 
			testnum++,__LINE__, x[0], (hpfmemhead)?hpfmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->ptr:NULL, 
			(hpwmemhead)?hpwmemhead->ptr:NULL, (hpfmemhead)?hpfmemhead->next:NULL, (hpfmemtail)?hpfmemtail->next:NULL);
	if (hpfmemhead && hpwmemhead && hpfmemtail && (x[0] == (int*)hpfmemhead->ptr) && (x[0] == (int*)hpwmemhead->ptr) && 
			(x[0] == (int*)hpfmemtail->ptr) && !hpfmemhead->next && !hpwmemhead->next && !hpfmemtail->next) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p,%p,%p\n", (hpfmemhead)?hpfmemhead->ptr:NULL, (hpwmemhead)?hpwmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->ptr:NULL, 
				(hpfmemhead)?hpfmemhead->next:NULL, (hpwmemhead)?hpwmemhead->next:NULL, (hpfmemtail)?hpfmemtail->next:NULL);
		failed++;
	}

	dbg(PRINT_ERROR, "%d. [%d] Show %p,%p\n", testnum++,__LINE__, x[0], NULL);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if ((x[0] == (int*)resp[0].ptr) && (NULL == (int*)resp[1].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p\n", resp[0].ptr, resp[1].ptr);
		failed++;
	}	

	
	dbg(PRINT_ERROR, "\n%d. [%d] Show ptr[%p] = hpfmemhead->ptr[%p] = hpfmemtail->ptr[%p], [NULL] hpwmemhead->ptr[%p] hpfmemhead->next[%p] hpfmemtail->next[%p]\n", 
			testnum++,__LINE__, x[0], (hpfmemhead)?hpfmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->ptr:NULL, 
			(hpwmemhead)?hpwmemhead->ptr:NULL, (hpfmemhead)?hpfmemhead->next:NULL, (hpfmemtail)?hpfmemtail->next:NULL);
	if (hpfmemhead && !hpwmemhead && hpfmemtail && (x[0] == (int*)hpfmemhead->ptr) && 
			(x[0] == (int*)hpfmemtail->ptr) && !hpfmemhead->next && !hpfmemtail->next) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p,%p\n", (hpfmemhead)?hpfmemhead->ptr:NULL, hpwmemhead, (hpfmemtail)?hpfmemtail->ptr:NULL, 
				(hpfmemhead)?hpfmemhead->next:NULL, (hpfmemtail)?hpfmemtail->next:NULL);
		failed++;
	}

	dbg(PRINT_ERROR, "\n%d. [%d] Show [null]\n", testnum++,__LINE__);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if (NULL == (int*)resp[0].ptr) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p\n", resp[0].ptr);
		failed++;
	}

	dbg(PRINT_ERROR, "\n%d. [%d] Show %p\n", testnum++,__LINE__, x[0]);
	sendAndRecv(mq, HEAPWALK_FULL, resp, 8, 0);
	dbg(PRINT_INFO, "%p\n",resp[0].ptr);
	if ((x[0] == (int*)resp[0].ptr) && (NULL == (int*)resp[1].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p\n", resp[0].ptr, resp[1].ptr);
		failed++;
	}
	
	x[1] = malloc(16);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show x[0][%p] = hpfmemhead->ptr[%p] x[1][%p] = hpfmemtail->ptr[%p] = hpwmemhead->ptr[%p] \
			NULL = hpfmemtail->next[%p] NULL = hpwmemhead->next[%p]\n", 
			testnum++,__LINE__, x[0], (hpfmemhead)?hpfmemhead->ptr:NULL, x[1], (hpfmemtail)?hpfmemtail->ptr:NULL, (hpwmemhead)?hpwmemhead->ptr:NULL, 
			(hpfmemtail)?hpfmemtail->next:NULL, (hpwmemhead)?hpwmemhead->next:NULL);
	if (hpfmemhead && hpfmemtail && hpwmemhead && (x[0] == (int*)hpfmemhead->ptr) && (x[1] == (int*)hpfmemtail->ptr) && (x[1] == (int*)hpwmemhead->ptr) &&
		       	!hpfmemtail->next && !hpwmemhead->next) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p,%p\n", (hpfmemhead)?hpfmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->ptr:NULL, 
				(hpwmemhead)?hpwmemhead->next:NULL, (hpfmemtail)?hpfmemtail->next:NULL, (hpwmemhead)?hpwmemhead->next:NULL);
		failed++;
	}

	x[2] = malloc(24);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show x[0][%p] = hpfmemhead->ptr[%p] x[2][%p] = hpfmemtail->ptr[%p] x[1][%p] = hpwmemhead->ptr[%p] \
			NULL = hpfmemtail->next[%p] NULL != hpwmemhead->next[%p]\n", 
			testnum++,__LINE__, x[0], (hpfmemhead)?hpfmemhead->ptr:NULL, x[2], (hpfmemtail)?hpfmemtail->ptr:NULL, x[1], (hpwmemhead)?hpwmemhead->ptr:NULL, 
			(hpfmemtail)?hpfmemtail->next:NULL, (hpwmemhead)?hpwmemhead->next:NULL);
	if (hpfmemhead && hpfmemtail && hpwmemhead && (x[0] == (int*)hpfmemhead->ptr) && (x[2] == (int*)hpfmemtail->ptr) && (x[1] == (int*)hpwmemhead->ptr) &&
		       	!hpfmemtail->next && hpwmemhead->next) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p,%p\n", (hpfmemhead)?hpfmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->ptr:NULL, 
				(hpwmemhead)?hpwmemhead->next:NULL, (hpfmemtail)?hpfmemtail->next:NULL, (hpwmemhead)?hpwmemhead->next:NULL);
		failed++;
	}

	x[3] = malloc(32);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show x[0][%p] = hpfmemhead->ptr[%p] x[3][%p] = hpfmemtail->ptr[%p] x[1][%p] = hpwmemhead->ptr[%p] \
			NULL = hpfmemtail->next[%p] NULL != hpwmemhead->next[%p]\n", 
			testnum++,__LINE__, x[0], (hpfmemhead)?hpfmemhead->ptr:NULL, x[3], (hpfmemtail)?hpfmemtail->ptr:NULL, x[1], (hpwmemhead)?hpwmemhead->ptr:NULL, 
			(hpfmemtail)?hpfmemtail->next:NULL, (hpwmemhead)?hpwmemhead->next:NULL);
	if (hpfmemhead && hpfmemtail && hpwmemhead && (x[0] == (int*)hpfmemhead->ptr) && (x[3] == (int*)hpfmemtail->ptr) && (x[1] == (int*)hpwmemhead->ptr) &&
		       	!hpfmemtail->next && hpwmemhead->next) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p,%p\n", (hpfmemhead)?hpfmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->ptr:NULL, 
				(hpwmemhead)?hpwmemhead->next:NULL, (hpfmemtail)?hpfmemtail->next:NULL, (hpwmemhead)?hpwmemhead->next:NULL);
		failed++;
	}

	free(x[2]);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show x[0][%p] = hpfmemhead->ptr[%p] x[3][%p] = hpfmemtail->ptr[%p] x[1][%p] = hpwmemhead->ptr[%p] \
			NULL = hpfmemtail->next[%p] x[3][%p] = hpwmemhead->next[%p]\n", 
			testnum++,__LINE__, x[0], (hpfmemhead)?hpfmemhead->ptr:NULL, x[3], (hpfmemtail)?hpfmemtail->ptr:NULL, x[1], (hpwmemhead)?hpwmemhead->ptr:NULL, 
			(hpfmemtail)?hpfmemtail->next:NULL, x[3], (hpwmemhead && hpwmemhead->next)?hpwmemhead->next->ptr:NULL);
	if (hpfmemhead && hpfmemtail && hpwmemhead && hpwmemhead->next && (x[0] == (int*)hpfmemhead->ptr) && (x[3] == (int*)hpfmemtail->ptr) && (x[1] == (int*)hpwmemhead->ptr) &&
		       	!hpfmemtail->next && (x[3] == hpwmemhead->next->ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p,%p\n", (hpfmemhead)?hpfmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->ptr:NULL, 
				(hpwmemhead)?hpwmemhead->next:NULL, (hpfmemtail)?hpfmemtail->next:NULL, (hpwmemhead && hpwmemhead->next)?hpwmemhead->next->ptr:NULL);
		failed++;
	}

	free(x[1]);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show x[0][%p] = hpfmemhead->ptr[%p] x[3][%p] = hpfmemtail->ptr[%p] x[3][%p] = hpwmemhead->ptr[%p] \
			NULL = hpfmemtail->next[%p] NULL = hpwmemhead->next[%p]\n", 
			testnum++,__LINE__, x[0], (hpfmemhead)?hpfmemhead->ptr:NULL, x[3], (hpfmemtail)?hpfmemtail->ptr:NULL, x[3], (hpwmemhead)?hpwmemhead->ptr:NULL, 
			(hpfmemtail)?hpfmemtail->next:NULL, (hpwmemhead)?hpwmemhead->next:NULL);
	if (hpfmemhead && hpfmemtail && hpwmemhead && (x[0] == (int*)hpfmemhead->ptr) && (x[3] == (int*)hpfmemtail->ptr) && (x[3] == (int*)hpwmemhead->ptr) &&
		       	!hpfmemtail->next && !hpwmemhead->next) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p,%p,%p\n", (hpfmemhead)?hpfmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->ptr:NULL, 
				(hpwmemhead)?hpwmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->next:NULL, hpwmemhead, (hpwmemhead)?hpwmemhead->next:NULL);
		failed++;
	}

	free(x[3]);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show ptr[%p] = hpfmemhead->ptr[%p] hpfmemtail->ptr[%p], [NULL] hpfmemhead->next[%p] hpfmemtail->next[%p] hpwmemhead[%p]\n", 
			testnum++,__LINE__, x[0], (hpfmemhead)?hpfmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->ptr:NULL, 
			(hpfmemhead)?hpfmemhead->next:NULL, (hpfmemtail)?hpfmemtail->next:NULL, hpwmemhead);
	if (hpfmemhead && hpfmemtail && !hpwmemhead && (x[0] == (int*)hpfmemhead->ptr) && (x[0] == (int*)hpfmemtail->ptr) && !hpfmemhead->next && !hpfmemtail->next) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p,%p\n", (hpfmemhead)?hpfmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->ptr:NULL, 
				(hpfmemhead)?hpfmemhead->next:NULL, (hpfmemtail)?hpfmemtail->next:NULL, hpwmemhead);
		failed++;
	}

	free(x[0]);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show NULL = hpfmemhead hpfmemtail, hpwmemhead\n", testnum++,__LINE__);
	if (!hpfmemhead && !hpfmemtail && !hpwmemhead) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p\n", hpfmemhead, hpfmemtail, hpwmemhead); 
		failed++;
	}

	x[0] = malloc(8);

	dbg(PRINT_ERROR, "\n%d. [%d] Show ptr[%p] = hpfmemhead->ptr hpfmemtail->ptr, hpwmemhead NULL = hpfmemhead->next hpfmemtail->next\n", 
			testnum++,__LINE__, x[0]); 
	if (hpfmemhead && hpfmemtail && hpwmemhead && (x[0] == (int*)hpfmemhead->ptr) && (x[0] == (int*)hpfmemtail->ptr) && 
			(x[0] == (int*)hpwmemhead->ptr) && !hpfmemhead->next && !hpfmemtail->next) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p,%p\n", (hpfmemhead)?hpfmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->ptr:NULL, 
				(hpwmemhead)?hpwmemhead->ptr:NULL, (hpfmemhead)?hpfmemhead->next:NULL, (hpfmemtail)?hpfmemtail->next:NULL);
		failed++;
	}

	x[1] = malloc(16);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show x[0][%p] = hpfmemhead->ptr x[1][%p] = hpfmemtail->ptr, NULL = hpwmemhead\n", 
			testnum++,__LINE__, x[0], x[1]); 
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if (hpfmemhead && hpfmemtail && !hpwmemhead && (x[0] == (int*)hpfmemhead->ptr) && (x[1] == (int*)hpfmemtail->ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p\n", (hpfmemhead)?hpfmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->ptr:NULL, hpwmemhead); 
		failed++;
	}

	x[2] = malloc(24);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show x[0][%p] = hpfmemhead->ptr x[2][%p] = hpfmemtail->ptr, NULL = hpwmemhead\n", 
			testnum++,__LINE__, x[0], x[2]); 
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if (hpfmemhead && hpfmemtail && !hpwmemhead && (x[0] == (int*)hpfmemhead->ptr) && (x[2] == (int*)hpfmemtail->ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p\n", (hpfmemhead)?hpfmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->ptr:NULL, hpwmemhead); 
		failed++;
	}

	free(x[1]);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show x[0][%p] = hpfmemhead->ptr x[2][%p] = hpfmemtail->ptr, NULL = hpwmemhead\n", 
			testnum++,__LINE__, x[0], x[2]); 
	if (hpfmemhead && hpfmemtail && !hpwmemhead && (x[0] == (int*)hpfmemhead->ptr) && (x[2] == (int*)hpfmemtail->ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p\n", (hpfmemhead)?hpfmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->ptr:NULL, hpwmemhead); 
		failed++;
	}

	free(x[2]);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show ptr[%p] = hpfmemhead->ptr hpfmemtail->ptr NULL = hpwmemhead NULL\n", 
			testnum++,__LINE__, x[0]); 
	if (hpfmemhead && hpfmemtail && !hpwmemhead && (x[0] == (int*)hpfmemhead->ptr) && (x[0] == (int*)hpfmemtail->ptr) && 
			 !hpfmemhead->next && !hpfmemtail->next) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p,%p\n", (hpfmemhead)?hpfmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->ptr:NULL, 
				hpwmemhead, (hpfmemhead)?hpfmemhead->next:NULL, (hpfmemtail)?hpfmemtail->next:NULL);
		failed++;
	}

	free(x[0]);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show NULL = hpfmemhead hpfmemtail, hpwmemhead\n", testnum++,__LINE__);
	if (!hpfmemhead && !hpfmemtail && !hpwmemhead) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p\n", hpfmemhead, hpfmemtail, hpwmemhead); 
		failed++;
	}

	x[0] = malloc(8);
	sendAndRecv(mq, HEAPWALK_MARKALL, resp, 8, 0);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show ptr[%p] = hpfmemhead->ptr hpfmemtail->ptr NULL = hpwmemhead NULL\n", 
			testnum++,__LINE__, x[0]); 
	if (hpfmemhead && hpfmemtail && !hpwmemhead && (x[0] == (int*)hpfmemhead->ptr) && (x[0] == (int*)hpfmemtail->ptr) && 
			 !hpfmemhead->next && !hpfmemtail->next) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p,%p\n", (hpfmemhead)?hpfmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->ptr:NULL, 
				hpwmemhead, (hpfmemhead)?hpfmemhead->next:NULL, (hpfmemtail)?hpfmemtail->next:NULL);
		failed++;
	}

	sendAndRecv(mq, HEAPWALK_RESET_MARKED, resp, 8, 0);
	dbg(PRINT_ERROR, "\n%d. [%d] Show ptr[%p] = hpfmemhead->ptr hpfmemtail->ptr, hpwmemhead NULL = hpfmemhead->next hpfmemtail->next\n", 
			testnum++,__LINE__, x[0]); 
	if (hpfmemhead && hpfmemtail && hpwmemhead && (x[0] == (int*)hpfmemhead->ptr) && (x[0] == (int*)hpfmemtail->ptr) && 
			(x[0] == (int*)hpwmemhead->ptr) && !hpfmemhead->next && !hpfmemtail->next) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p,%p\n", (hpfmemhead)?hpfmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->ptr:NULL, 
				(hpwmemhead)?hpwmemhead->ptr:NULL, (hpfmemhead)?hpfmemhead->next:NULL, (hpfmemtail)?hpfmemtail->next:NULL);
		failed++;
	}

	sendAndRecv(mq, HEAPWALK_MARKALL, resp, 8, 0);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show ptr[%p] = hpfmemhead->ptr hpfmemtail->ptr NULL = hpwmemhead NULL\n", 
			testnum++,__LINE__, x[0]); 
	if (hpfmemhead && hpfmemtail && !hpwmemhead && (x[0] == (int*)hpfmemhead->ptr) && (x[0] == (int*)hpfmemtail->ptr) && 
			 !hpfmemhead->next && !hpfmemtail->next) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p,%p\n", (hpfmemhead)?hpfmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->ptr:NULL, 
				hpwmemhead, (hpfmemhead)?hpfmemhead->next:NULL, (hpfmemtail)?hpfmemtail->next:NULL);
		failed++;
	}

	x[1] = malloc(16);
	sendAndRecv(mq, HEAPWALK_RESET_MARKED, resp, 8, 0);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show x[0][%p] = hpfmemhead->ptr hpwmemhead x[1][%p] = hpfmemtail->ptr\n", 
			testnum++,__LINE__, x[0], x[1]); 
	if (hpfmemhead && hpfmemtail && hpwmemhead && (x[0] == (int*)hpfmemhead->ptr) && (x[0] == (int*)hpwmemhead->ptr) && 
			(x[1] == (int*)hpfmemtail->ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p\n", (hpfmemhead)?hpfmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->ptr:NULL, 
				(hpwmemhead)?hpwmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->ptr:NULL);
		failed++;
	}

	x[2] = malloc(24);
	sendAndRecv(mq, HEAPWALK_RESET_MARKED, resp, 8, 0);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show x[0][%p] = hpfmemhead->ptr hpwmemhead x[2][%p] = hpfmemtail->ptr\n", 
			testnum++,__LINE__, x[0], x[2]); 
	if (hpfmemhead && hpfmemtail && hpwmemhead && (x[0] == (int*)hpfmemhead->ptr) && (x[0] == (int*)hpwmemhead->ptr) && 
			(x[2] == (int*)hpfmemtail->ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p\n", (hpfmemhead)?hpfmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->ptr:NULL, 
				(hpwmemhead)?hpwmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->ptr:NULL);
		failed++;
	}

	free(x[0]);

	dbg(PRINT_ERROR, "\n%d. [%d] Show x[1][%p] = hpfmemhead->ptr hpwmemhead->ptr x[2][%p] = hpfmemtail->ptr\n", 
			testnum++,__LINE__, x[1], x[2]); 
	if (hpfmemhead && hpfmemtail && hpwmemhead && (x[1] == (int*)hpfmemhead->ptr) && (x[1] == (int*)hpwmemhead->ptr) && 
			(x[2] == (int*)hpfmemtail->ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p\n", (hpfmemhead)?hpfmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->ptr:NULL, 
				(hpwmemhead)?hpwmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->ptr:NULL);
		failed++;
	}

	sendAndRecv(mq, HEAPWALK_MARKALL, resp, 8, 0);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show x[1][%p] = hpfmemhead->ptr x[2][%p] = hpfmemtail->ptr NULL = hpwmemhead\n", 
			testnum++,__LINE__, x[1], x[2]); 
	if (hpfmemhead && hpfmemtail && !hpwmemhead && (x[1] == (int*)hpfmemhead->ptr) && (x[2] == (int*)hpfmemtail->ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p\n", (hpfmemhead)?hpfmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->ptr:NULL, 
				(hpfmemtail)?hpfmemtail->ptr:NULL, hpwmemhead);
		failed++;
	}

	sendAndRecv(mq, HEAPWALK_RESET_MARKED, resp, 8, 0);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show x[1][%p] = hpfmemhead->ptr hpwmemhead->ptr x[2][%p] = hpfmemtail->ptr\n", 
			testnum++,__LINE__, x[1], x[2]); 
	if (hpfmemhead && hpfmemtail && hpwmemhead && (x[1] == (int*)hpfmemhead->ptr) && (x[1] == (int*)hpwmemhead->ptr) && 
			(x[2] == (int*)hpfmemtail->ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p\n", (hpfmemhead)?hpfmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->ptr:NULL, 
				(hpwmemhead)?hpwmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->ptr:NULL);
		failed++;
	}

	free(x[1]);

	dbg(PRINT_ERROR, "\n%d. [%d] Show x[2][%p] = hpfmemhead->ptr hpwmemhead->ptr hpfmemtail->ptr\n", testnum++,__LINE__, x[2]);
	if (hpfmemhead && hpfmemtail && hpwmemhead && (x[2] == (int*)hpfmemhead->ptr) && (x[2] == (int*)hpwmemhead->ptr) && 
			(x[2] == (int*)hpfmemtail->ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p\n", (hpfmemhead)?hpfmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->ptr:NULL, 
				(hpwmemhead)?hpwmemhead->ptr:NULL, (hpfmemtail)?hpfmemtail->ptr:NULL);
		failed++;
	}
	
	free(x[2]);

	dbg(PRINT_ERROR, "\n%d. [%d] Show NULL = hpfmemhead hpfmemtail, hpwmemhead\n", testnum++,__LINE__);
	if (!hpfmemhead && !hpfmemtail && !hpwmemhead) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p\n", hpfmemhead, hpfmemtail, hpwmemhead); 
		failed++;
	}

	sendAndRecv(mq, HEAPWALK_MARKALL, resp, 8, 0);
	dbg(PRINT_ERROR, "\n%d. [%d] Show NULL = hpfmemhead hpfmemtail, hpwmemhead\n", testnum++,__LINE__);
	if (!hpfmemhead && !hpfmemtail && !hpwmemhead) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p\n", hpfmemhead, hpfmemtail, hpwmemhead); 
		failed++;
	}

	sendAndRecv(mq, HEAPWALK_RESET_MARKED, resp, 8, 0);
	dbg(PRINT_ERROR, "\n%d. [%d] Show NULL = hpfmemhead hpfmemtail, hpwmemhead\n", testnum++,__LINE__);
	if (!hpfmemhead && !hpfmemtail && !hpwmemhead) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p\n", hpfmemhead, hpfmemtail, hpwmemhead); 
		failed++;
	}
#else	
	dbg(PRINT_ERROR, "\n%d. [%d] Show ptr[%p] = memhead->ptr[%p] memtail->ptr[%p], [NULL] memhead->next[%p] memtail->next[%p]\n", 
			testnum++,__LINE__, x[0], (memhead)?memhead->ptr:NULL, (memtail)?memtail->ptr:NULL, (memhead)?memhead->next:NULL, (memtail)?memtail->next:NULL);
	if (memhead && memtail && (x[0] == (int*)memhead->ptr) && (x[0] == (int*)memtail->ptr) && !memhead->next&& !memtail->next) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p\n", (memhead)?memhead->ptr:NULL, (memtail)?memtail->ptr:NULL, (memhead)?memhead->next:NULL, (memtail)?memtail->next:NULL);
		failed++;
	}

	dbg(PRINT_ERROR, "%d. [%d] Show %p,%p\n", testnum++,__LINE__, x[0], NULL);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if ((x[0] == (int*)resp[0].ptr) && (NULL == (int*)resp[1].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p\n", resp[0].ptr, resp[1].ptr);
		failed++;
	}	

	
	dbg(PRINT_ERROR, "\n%d. [%d] Show ptr[%p] = wmemhead->ptr[%p] wmemtail->ptr[%p]\n   [NULL] wmemhead->next[%p] wmemtail->next[%p], memhead[%p]=memtail[%p]=NULL\n", 
			testnum++,__LINE__, x[0], (wmemhead)?wmemhead->ptr:NULL, (wmemtail)?wmemtail->ptr:NULL, (wmemhead)?wmemhead->next:NULL, (wmemtail)?wmemtail->next:NULL, memhead, memtail);
	if (wmemhead && wmemtail && (x[0] == (int*)wmemhead->ptr) && (x[0] == (int*)wmemtail->ptr) && !wmemhead->next && !wmemtail->next && !memhead && !memtail) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p,%p,%p\n", (wmemhead)?wmemhead->ptr:NULL, (wmemtail)?wmemtail->ptr:NULL, (wmemhead)?wmemhead->next:NULL, (wmemtail)?wmemtail->next:NULL, memhead, memtail);
		failed++;
	}

	dbg(PRINT_ERROR, "\n%d. [%d] Show [null]\n", testnum++,__LINE__);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if (NULL == (int*)resp[0].ptr) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p\n", resp[0].ptr);
		failed++;
	}

	dbg(PRINT_ERROR, "\n%d. [%d] Show %p\n", testnum++,__LINE__, x[0]);
	sendAndRecv(mq, HEAPWALK_FULL, resp, 8, 0);
	dbg(PRINT_INFO, "%p\n",resp[0].ptr);
	if ((x[0] == (int*)resp[0].ptr) && (NULL == (int*)resp[1].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p\n", resp[0].ptr, resp[1].ptr);
		failed++;
	}
	
	x[1] = malloc(16);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show ptr[%p] = wmemhead->ptr[%p] wmemtail->ptr[%p], [NULL] wmemhead->next[%p] wmemtail->next[%p]\n \
			ptr[%p] = memhead->ptr[%p] memtail->ptr[%p],[NULL] memhead->next[%p], (memtail)?memtail->next:NULL[%p]\n", 
			testnum++,__LINE__, x[0], (wmemhead)?wmemhead->ptr:NULL, (wmemtail)?wmemtail->ptr:NULL, (wmemhead)?wmemhead->next:NULL, (wmemtail)?wmemtail->next:NULL, \
			x[1], (memhead)?memhead->ptr:NULL, (memtail)?memtail->ptr:NULL, (memhead)?memhead->next:NULL, (memtail)?memtail->next:NULL);
	if (memhead && memtail && wmemhead && wmemtail && (x[0] == (int*)wmemhead->ptr) && (x[0] == (int*)wmemtail->ptr) && !wmemhead->next && !wmemtail->next && \
			(x[1] == (int*)memhead->ptr) && (x[1] == (int*)memtail->ptr) && !memhead->next&& !memtail->next) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p,%p,%p,%p,%p\n", (wmemhead)?wmemhead->ptr:NULL, (wmemtail)?wmemtail->ptr:NULL, (wmemhead)?wmemhead->next:NULL, (wmemtail)?wmemtail->next:NULL, (memhead)?memhead->ptr:NULL, (memtail)?memtail->ptr:NULL, (memhead)?memhead->next:NULL, (memtail)?memtail->next:NULL);
		failed++;
	}

	x[2] = malloc(24);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show ptr[%p] = wmemhead->ptr[%p] wmemtail->ptr[%p], [NULL] wmemhead->next[%p] wmemtail->next[%p]\n \
			ptr[%p] = memhead->ptr[%p] ptr[%p]=memhead->next->ptr[%p]=memtail->ptr[%p], [NULL]=memtail->next[%p]\n", 
			testnum++,__LINE__, x[0], (wmemhead)?wmemhead->ptr:NULL, (wmemtail)?wmemtail->ptr:NULL, (wmemhead)?wmemhead->next:NULL, (wmemtail)?wmemtail->next:NULL, \
			x[1], (memhead)?memhead->ptr:NULL, x[2], (memhead && memhead->next)?memhead->next->ptr:NULL, (memtail)?memtail->ptr:NULL, (memtail)?memtail->next:NULL);
	if (memhead && memtail && wmemhead && wmemtail && (x[0] == (int*)wmemhead->ptr) && (x[0] == (int*)wmemtail->ptr) && !wmemhead->next && !wmemtail->next && \
			(x[1] == (int*)memhead->ptr) && (x[2] == (int*)memhead->next->ptr) && (x[2] == (int*)memtail->ptr) && !memtail->next) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p,%p,%p,%p\n", (wmemhead)?wmemhead->ptr:NULL, (wmemtail)?wmemtail->ptr:NULL, (wmemhead)?wmemhead->next:NULL, (wmemtail)?wmemtail->next:NULL, (memhead && memhead->next)?memhead->next->ptr:NULL, (memtail)?memtail->ptr:NULL, (memtail)?memtail->next:NULL);
		failed++;
	}

	x[3] = malloc(32);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show ptr[%p] = wmemhead->ptr[%p] wmemtail->ptr[%p], [NULL] wmemhead->next[%p] wmemtail->next[%p]\n \
			ptr[%p] = memhead->ptr[%p] ptr[%p]=memhead->next->ptr[%p] ptr[%p] = memtail->ptr[%p], [NULL]=memtail->next[%p]\n", 
			testnum++,__LINE__, x[0], (wmemhead)?wmemhead->ptr:NULL, (wmemtail)?wmemtail->ptr:NULL, (wmemhead)?wmemhead->next:NULL, (wmemtail)?wmemtail->next:NULL, \
			x[1], (memhead)?memhead->ptr:NULL, x[2], (memhead && memhead->next)?memhead->next->ptr:NULL, x[3], (memtail)?memtail->ptr:NULL, (memtail)?memtail->next:NULL);
	if (memhead && memtail && wmemhead && wmemtail && (x[0] == (int*)wmemhead->ptr) && (x[0] == (int*)wmemtail->ptr) && !wmemhead->next && !wmemtail->next && \
			(x[1] == (int*)memhead->ptr) && (x[2] == (int*)memhead->next->ptr) && (x[3] == (int*)memtail->ptr) && !memtail->next) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p,%p,%p,%p\n", (wmemhead)?wmemhead->ptr:NULL, (wmemtail)?wmemtail->ptr:NULL, (wmemhead)?wmemhead->next:NULL, (wmemtail)?wmemtail->next:NULL, (memhead && memhead->next)?memhead->next->ptr:NULL, (memtail)?memtail->ptr:NULL, (memtail)?memtail->next:NULL);
		failed++;
	}

	free(x[2]);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show ptr[%p] = wmemhead->ptr[%p] wmemtail->ptr[%p], [NULL] wmemhead->next[%p] wmemtail->next[%p]\n \
			ptr[%p] = memhead->ptr[%p] ptr[%p]=memhead->next->ptr[%p] ptr[%p]=memtail->ptr[%p], [NULL]=memtail->next[%p]\n", 
			testnum++,__LINE__, x[0], (wmemhead)?wmemhead->ptr:NULL, (wmemtail)?wmemtail->ptr:NULL, (wmemhead)?wmemhead->next:NULL, (wmemtail)?wmemtail->next:NULL, \
			x[1], (memhead)?memhead->ptr:NULL, x[3], (memhead && memhead->next)?memhead->next->ptr:NULL, x[3], (memtail)?memtail->ptr:NULL, (memtail)?memtail->next:NULL);
	if (memhead && memtail && wmemhead && wmemtail && (x[0] == (int*)wmemhead->ptr) && (x[0] == (int*)wmemtail->ptr) && !wmemhead->next && !wmemtail->next && \
			(x[1] == (int*)memhead->ptr) && (x[3] == (int*)memhead->next->ptr) && (x[3] == (int*)memtail->ptr) && !memtail->next) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p,%p,%p,%p\n", (wmemhead)?wmemhead->ptr:NULL, (wmemtail)?wmemtail->ptr:NULL, (wmemhead)?wmemhead->next:NULL, (wmemtail)?wmemtail->next:NULL, (memhead && memhead->next)?memhead->next->ptr:NULL, (memtail)?memtail->ptr:NULL, (memtail)?memtail->next:NULL);
		failed++;
	}

	free(x[3]);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show ptr[%p] = wmemhead->ptr[%p] wmemtail->ptr[%p], [NULL] wmemhead->next[%p] wmemtail->next[%p]\n \
			ptr[%p] = memhead->ptr[%p] memtail->ptr[%p],[NULL] memhead->next[%p], (memtail)?memtail->next:NULL[%p]\n", 
			testnum++,__LINE__, x[0], (wmemhead)?wmemhead->ptr:NULL, (wmemtail)?wmemtail->ptr:NULL, (wmemhead)?wmemhead->next:NULL, (wmemtail)?wmemtail->next:NULL, \
			x[1], (memhead)?memhead->ptr:NULL, (memtail)?memtail->ptr:NULL, (memhead)?memhead->next:NULL, (memtail)?memtail->next:NULL);
	if (memhead && memtail && wmemhead && wmemtail && (x[0] == (int*)wmemhead->ptr) && (x[0] == (int*)wmemtail->ptr) && !wmemhead->next && !wmemtail->next && \
			(x[1] == (int*)memhead->ptr) && (x[1] == (int*)memtail->ptr) && !memhead->next&& !memtail->next) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p,%p,%p,%p,%p\n", (wmemhead)?wmemhead->ptr:NULL, (wmemtail)?wmemtail->ptr:NULL, (wmemhead)?wmemhead->next:NULL, (wmemtail)?wmemtail->next:NULL, (memhead)?memhead->ptr:NULL, (memtail)?memtail->ptr:NULL, (memhead)?memhead->next:NULL, (memtail)?memtail->next:NULL);
		failed++;
	}

	free(x[1]);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show ptr[%p] = wmemhead->ptr[%p] wmemtail->ptr[%p], [NULL] wmemhead->next[%p] wmemtail->next[%p]\n \
			[NULL] memhead, memtail\n", 
			testnum++,__LINE__, x[0], (wmemhead)?wmemhead->ptr:NULL, (wmemtail)?wmemtail->ptr:NULL, (wmemhead)?wmemhead->next:NULL, (wmemtail)?wmemtail->next:NULL);
	if (wmemhead && wmemtail && (x[0] == (int*)wmemhead->ptr) && (x[0] == (int*)wmemtail->ptr) && !wmemhead->next && !wmemtail->next && \
			!memhead && !memtail) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	} else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p,%p,%p\n", (wmemhead)?wmemhead->ptr:NULL, (wmemtail)?wmemtail->ptr:NULL, (wmemhead)?wmemhead->next:NULL, (wmemtail)?wmemtail->next:NULL, memhead, memtail);
		failed++;
	}

	x[1] = malloc(16);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p = wmemhead->ptr %p = wmemtail->ptr, [NULL] wmemtail->next\n", testnum++,__LINE__, x[0], x[1]);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if (wmemhead && wmemtail && (x[1] == (int*)resp[0].ptr) && (NULL == (int*)resp[1].ptr) && (x[0] == (int*)wmemhead->ptr) && (x[1] == (int*)wmemtail->ptr) && !wmemtail->next) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p,%p\n", resp[0].ptr, resp[1].ptr, (wmemhead)?wmemhead->ptr:NULL, (wmemtail)?wmemtail->ptr:NULL, (wmemtail)?wmemtail->next:NULL);
		failed++;
	}

	x[2] = malloc(24);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p = wmemtail->ptr, [NULL] wmemtail->next\n", testnum++,__LINE__, x[2]);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if (wmemtail && (x[2] == (int*)resp[0].ptr) && (NULL == (int*)resp[1].ptr) && (x[2] == (int*)wmemtail->ptr) && !wmemtail->next) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p\n", resp[0].ptr, resp[1].ptr, (wmemtail)?wmemtail->ptr:NULL, (wmemtail)?wmemtail->next:NULL);
		failed++;
	}

	free(x[1]);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p = wmemtail->ptr, [NULL] wmemtail->next\n", testnum++,__LINE__, x[2]);
	if (wmemtail && (x[2] == (int*)wmemtail->ptr) && !wmemtail->next) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p\n", (wmemtail)?wmemtail->ptr:NULL, (wmemtail)?wmemtail->next:NULL);
		failed++;
	}

	free(x[2]);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p = wmemtail->ptr, [NULL] wmemtail->next\n", testnum++,__LINE__, x[0]);
	if (wmemtail && (x[0] == (int*)wmemtail->ptr) && !wmemtail->next) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p\n", wmemtail?wmemtail->ptr:NULL, wmemtail?wmemtail->next:NULL);
		failed++;
	}

	free(x[0]);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show empty, wmemhead,wmemtail = NULL\n", testnum++,__LINE__);
	sendAndRecv(mq, HEAPWALK_FULL, resp, 8, 0);
	if (NULL == (int*)resp[0].ptr && !wmemhead && !wmemtail) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p\n", resp[0].ptr, wmemhead, wmemtail);
		failed++;
	}

	x[0] = malloc(8);
	sendAndRecv(mq, HEAPWALK_MARKALL, resp, 8, 0);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p = wmemhead->ptr, (wmemtail)?wmemtail->ptr:NULL\n", testnum++,__LINE__, x[0]);
	if (wmemhead && wmemtail && (x[0] == (int*)wmemhead->ptr) && (x[0] == (int*)wmemtail->ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p\n", wmemhead?wmemhead->ptr:NULL, wmemtail?wmemtail->ptr:NULL);
		failed++;
	}

	sendAndRecv(mq, HEAPWALK_RESET_MARKED, resp, 8, 0);
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p = memhead->ptr, (memtail)?memtail->ptr:NULL\n", testnum++,__LINE__, x[0]);
	if (memhead && memtail && (x[0] == (int*)memhead->ptr) && (x[0] == (int*)memtail->ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p\n", (memhead)?memhead->ptr:NULL, (memtail)?memtail->ptr:NULL);
		failed++;
	}

	sendAndRecv(mq, HEAPWALK_MARKALL, resp, 8, 0);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p = wmemhead->ptr, (wmemtail)?wmemtail->ptr:NULL\n", testnum++,__LINE__, x[0]);
	if (wmemhead && wmemtail && (x[0] == (int*)wmemhead->ptr) && (x[0] == (int*)wmemtail->ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p\n", wmemhead?wmemhead->ptr:NULL, wmemtail?wmemtail->ptr:NULL);
		failed++;
	}

	x[1] = malloc(16);
	sendAndRecv(mq, HEAPWALK_RESET_MARKED, resp, 8, 0);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p = memhead->ptr, %p = memtail->ptr\n", testnum++,__LINE__, x[0], x[1]);
	if (memhead && memtail && (x[0] == (int*)memhead->ptr) && (x[1] == (int*)memtail->ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p\n", (memhead)?memhead->ptr:NULL, (memtail)?memtail->ptr:NULL);
		failed++;
	}

	sendAndRecv(mq, HEAPWALK_MARKALL, resp, 8, 0);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p = wmemhead->ptr, %p = wmemtail->ptr\n", testnum++,__LINE__, x[0], x[1]);
	if (wmemhead && wmemtail && (x[0] == (int*)wmemhead->ptr) && (x[1] == (int*)wmemtail->ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p\n", (wmemhead)?wmemhead->ptr:NULL, (wmemtail)?wmemtail->ptr:NULL);
		failed++;
	}

	x[2] = malloc(24);
	sendAndRecv(mq, HEAPWALK_RESET_MARKED, resp, 8, 0);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p = memhead->ptr, %p = memtail->ptr\n", testnum++,__LINE__, x[0], x[2]);
	if (memhead && memtail && (x[0] == (int*)memhead->ptr) && (x[2] == (int*)memtail->ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p\n", (memhead)?memhead->ptr:NULL, (memtail)?memtail->ptr:NULL);
		failed++;
	}

	sendAndRecv(mq, HEAPWALK_MARKALL, resp, 8, 0);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p = wmemhead->ptr, %p = wmemtail->ptr\n", testnum++,__LINE__, x[0], x[2]);
	if (wmemhead && wmemtail && (x[0] == (int*)wmemhead->ptr) && (x[2] == (int*)wmemtail->ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p\n", (wmemhead)?wmemhead->ptr:NULL, (wmemtail)?wmemtail->ptr:NULL);
		failed++;
	}

	free(x[1]);
	sendAndRecv(mq, HEAPWALK_MARKALL, resp, 8, 0);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p = wmemhead->ptr, %p = wmemtail->ptr\n", testnum++,__LINE__, x[0], x[2]);
	if (wmemhead && wmemtail && (x[0] == (int*)wmemhead->ptr) && (x[2] == (int*)wmemtail->ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p\n", (wmemhead)?wmemhead->ptr:NULL, (wmemtail)?wmemtail->ptr:NULL);
		failed++;
	}

	sendAndRecv(mq, HEAPWALK_RESET_MARKED, resp, 8, 0);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p = memhead->ptr, %p = memtail->ptr\n", testnum++,__LINE__, x[0], x[2]);
	if (memhead && memtail && (x[0] == (int*)memhead->ptr) && (x[2] == (int*)memtail->ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p\n", (memhead)?memhead->ptr:NULL, (memtail)?memtail->ptr:NULL);
		failed++;
	}

	free(x[2]);
	sendAndRecv(mq, HEAPWALK_MARKALL, resp, 8, 0);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p = wmemhead->ptr = wmemtail->ptr\n", testnum++,__LINE__, x[0]);
	if (wmemhead && wmemtail && (x[0] == (int*)wmemhead->ptr) && (x[0] == (int*)wmemtail->ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p\n", (wmemhead)?wmemhead->ptr:NULL, (wmemtail)?wmemtail->ptr:NULL);
		failed++;
	}

	sendAndRecv(mq, HEAPWALK_RESET_MARKED, resp, 8, 0);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p = memhead->ptr = memtail->ptr\n", testnum++,__LINE__, x[0]);
	if (memhead && memtail && (x[0] == (int*)memhead->ptr) && (x[0] == (int*)memtail->ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p\n", (memhead)?memhead->ptr:NULL, (memtail)?memtail->ptr:NULL);
		failed++;
	}

	free(x[0]);
	sendAndRecv(mq, HEAPWALK_MARKALL, resp, 8, 0);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show NULL wmemhead, wmemtail, memhead, memtail\n", testnum++,__LINE__);
	if (!wmemhead && !wmemtail && !memhead && !memtail) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p\n", wmemhead, wmemtail, memhead, memtail);
		failed++;
	}

	sendAndRecv(mq, HEAPWALK_RESET_MARKED, resp, 8, 0);
	
	dbg(PRINT_ERROR, "\n%d. [%d] Show NULL wmemhead, wmemtail, memhead, memtail\n", testnum++,__LINE__);
	if (!wmemhead && !wmemtail && !memhead && !memtail) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p\n", wmemhead, wmemtail, memhead, memtail);
		failed++;
	}
#endif
	x[0] = malloc(8);
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p\n", testnum++,__LINE__, x[0]);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	dbg(PRINT_INFO, "%p\n",resp[0].ptr);
	if ((x[0] == (int*)resp[0].ptr) && (NULL == (int*)resp[1].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p\n", resp[0].ptr, resp[1].ptr);
		failed++;
	}

	x[1] = malloc(8);
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p,%p\n", testnum++,__LINE__, x[0],x[1]);
	sendAndRecv(mq, HEAPWALK_FULL, resp, 8, 0);
	if ((x[0] == (int*)resp[0].ptr) && (x[1] == (int*)resp[1].ptr) && (NULL == (int*)resp[2].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p\n", resp[0].ptr, resp[1].ptr, resp[2].ptr);
		failed++;
	}

	x[2] = malloc(8);
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p,%p,%p\n", testnum++,__LINE__, x[0],x[1],x[2]);
	sendAndRecv(mq, HEAPWALK_FULL, resp, 8, 0);
	if ((x[0] == (int*)resp[0].ptr) && (x[1] == (int*)resp[1].ptr) && (x[2] == (int*)resp[2].ptr) && (NULL == (int*)resp[3].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail\n");
		failed++;
	}

	x[3] = malloc(8);
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p,%p,%p,%p\n", testnum++,__LINE__, x[0],x[1],x[2],x[3]);
	sendAndRecv(mq, HEAPWALK_FULL, resp, 8, 0);
	if ((x[0] == (int*)resp[0].ptr) && (x[1] == (int*)resp[1].ptr) && 
			(x[2] == (int*)resp[2].ptr) && (x[3] == (int*)resp[3].ptr) && (NULL == (int*)resp[4].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail\n");
		failed++;
	}

	dbg(PRINT_ERROR, "\n%d. [%d] Show %p,%p,%p\n", testnum++,__LINE__, x[0],x[1],x[3]);
	free(x[2]);
	sendAndRecv(mq, HEAPWALK_FULL, resp, 8, 0);
	if ((x[0] == (int*)resp[0].ptr) && (x[1] == (int*)resp[1].ptr) && (x[3] == (int*)resp[2].ptr) && (NULL == (int*)resp[3].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail\n");
		failed++;
	}

	dbg(PRINT_ERROR, "\n%d. [%d] Show %p,%p\n", testnum++,__LINE__, x[0],x[1]);
	free(x[3]);
	sendAndRecv(mq, HEAPWALK_FULL, resp, 8, 0);
	if ((x[0] == (int*)resp[0].ptr) && (x[1] == (int*)resp[1].ptr) && (NULL == (int*)resp[2].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail\n");
		failed++;
	}

	dbg(PRINT_ERROR, "\n%d. [%d] Show %p\n", testnum++,__LINE__, x[1]);
	free(x[0]);
	sendAndRecv(mq, HEAPWALK_FULL, resp, 8, 0);
	if ((x[1] == (int*)resp[0].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail\n");
		failed++;
	}

	free(x[1]);
	dbg(PRINT_ERROR, "\n%d. [%d] Show [null]\n", testnum++,__LINE__);
	sendAndRecv(mq, HEAPWALK_FULL, resp, 8, 1);
	if ((NULL == (int*)resp[0].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail\n");
		failed++;
	}

	x[0] = malloc(8);
	x[1] = malloc(8);
	x[2] = malloc(8);
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p,%p,%p\n", testnum++,__LINE__, x[0],x[1],x[2]);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if ((x[0] == (int*)resp[0].ptr) && (x[1] == (int*)resp[1].ptr) && (x[2] == (int*)resp[2].ptr) && (NULL == (int*)resp[3].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail\n");
		failed++;
	}

	
	free(x[0]);
	free(x[1]);
	free(x[2]);
	x[0] = malloc(8);
	x[1] = malloc(8);
	x[2] = malloc(8);
	free(x[1]);
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p,%p\n", testnum++,__LINE__, x[0],x[2]);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if ((x[0] == (int*)resp[0].ptr) && (x[2] == (int*)resp[1].ptr) && (NULL == (int*)resp[2].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail\n");
		failed++;
	}

	x[3] = malloc(8);
	x[4] = malloc(8);
	x[5] = malloc(8);
	free(x[3]);
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p,%p\n", testnum++,__LINE__, x[4],x[5]);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if ((x[4] == (int*)resp[0].ptr) && (x[5] == (int*)resp[1].ptr) && (NULL == (int*)resp[2].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail\n");
		failed++;
	}

	dbg(PRINT_ERROR, "\n%d. [%d] Show %p,%p,%p,%p\n", testnum++,__LINE__, x[0],x[2],x[4],x[5]);
	sendAndRecv(mq, HEAPWALK_FULL, resp, 8, 0);
	if ((x[0] == (int*)resp[0].ptr) && (x[2] == (int*)resp[1].ptr) && (x[4] == (int*)resp[2].ptr) && (x[5] == (int*)resp[3].ptr) && (NULL == (int*)resp[4].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail\n");
		failed++;
	}

	x[6] = malloc(8);
	x[7] = malloc(8);
	x[8] = malloc(8);
	*x[8] = 2388;
	free(x[8]);
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p,%p\n", testnum++,__LINE__, x[6],x[7]);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if ((x[6] == (int*)resp[0].ptr) && (x[7] == (int*)resp[1].ptr) && (NULL == (int*)resp[4].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail\n");
		failed++;
	}

	dbg(PRINT_ERROR, "\n%d. [%d] Show %p,%p,%p,%p,%p,%p\n", testnum++,__LINE__, x[0],x[2],x[4],x[5],x[6],x[7]);
	sendAndRecv(mq, HEAPWALK_FULL, resp, 8, 0);
	if ((x[0] == (int*)resp[0].ptr) && (x[2] == (int*)resp[1].ptr) && (x[4] == (int*)resp[2].ptr) && (x[5] == (int*)resp[3].ptr) && 
		(x[6] == (int*)resp[4].ptr) && (x[7] == (int*)resp[5].ptr) && (NULL == (int*)resp[6].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail\n");
		failed++;
	}

	free(x[5]);
	x[8] = malloc(8);
	x[9] = malloc(8);
	*x[8] = 2388;
	free(x[8]);
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p,%p,%p,%p,%p,%p\n", testnum++,__LINE__, x[0],x[2],x[4],x[6],x[7],x[9]);
	sendAndRecv(mq, HEAPWALK_FULL, resp, 8, 0);
	if ((x[0] == (int*)resp[0].ptr) && (x[2] == (int*)resp[1].ptr) && (x[4] == (int*)resp[2].ptr) && (x[6] == (int*)resp[3].ptr) && 
		(x[7] == (int*)resp[4].ptr) && (x[9] == (int*)resp[5].ptr) && (NULL == (int*)resp[6].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail\n");
		failed++;
	}

	free(x[9]);
	x[8] = malloc(8);
	x[9] = malloc(8);
	*x[9] = 2388;
	free(x[9]);
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p\n", testnum++,__LINE__, x[8]);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if ((x[8] == (int*)resp[0].ptr) && (NULL == (int*)resp[1].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail\n");
		failed++;
	}

	dbg(PRINT_ERROR, "\n%d. [%d] Show %p,%p,%p,%p,%p,%p\n", testnum++,__LINE__, x[0],x[2],x[4],x[6],x[7],x[8]);
	sendAndRecv(mq, HEAPWALK_FULL, resp, 8, 0);
	if ((x[0] == (int*)resp[0].ptr) && (x[2] == (int*)resp[1].ptr) && (x[4] == (int*)resp[2].ptr) && (x[6] == (int*)resp[3].ptr) && 
		(x[7] == (int*)resp[4].ptr) && (x[8] == (int*)resp[5].ptr) && (NULL == (int*)resp[6].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail\n");
		failed++;
	}

	dbg(PRINT_ERROR, "\n%d. [%d] Show [null]\n", testnum++,__LINE__);
	free(x[0]);free(x[2]);free(x[4]);free(x[6]);free(x[7]);free(x[8]);
	x[0] = malloc(8);
	*x[0] = 2388;
	free(x[0]);
	sendAndRecv(mq, HEAPWALK_FULL, resp, 8, 1);
	if ((NULL == (int*)resp[0].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail\n");
		failed++;
	}

	dbg(PRINT_ERROR, "\n%d. [%d] Show [null]\n", testnum++,__LINE__);
	x[0] = malloc(8);
	x[1] = malloc(8);
	*x[0] = 2388;
	dbg(PRINT_INFO, "*x[0] %d\n", *x[0]);
	*x[1] = 2388;
	dbg(PRINT_INFO, "*x[1] %d\n", *x[1]);
	free(x[0]);
	free(x[1]);
	sendAndRecv(mq, HEAPWALK_FULL, resp, 8, 1);
	if ((NULL == (int*)resp[0].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail\n");
		failed++;
	}


	x[0] = calloc(1,8);
	x[1] = calloc(1,10);
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p,%p\n", testnum++,__LINE__, x[0], x[1]);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if ((x[0] == (int*)resp[0].ptr) && (x[1] == (int*)resp[1].ptr) && (NULL == (int*)resp[2].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p\n", resp[0].ptr, resp[1].ptr, resp[2].ptr);
		failed++;
	}

	x[2] = calloc(1,8);
	x[3] = calloc(1,8);
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p,%p,%p,%p\n", testnum++,__LINE__, x[0], x[1], x[2], x[3]);
	sendAndRecv(mq, HEAPWALK_FULL, resp, 8, 0);
	if ((x[0] == (int*)resp[0].ptr) && (x[1] == (int*)resp[1].ptr) && 
	    (x[2] == (int*)resp[2].ptr) && (x[3] == (int*)resp[3].ptr) && (NULL == (int*)resp[4].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p,%p\n", resp[0].ptr, resp[1].ptr, resp[2].ptr, resp[3].ptr, resp[4].ptr);
		failed++;
	}

	x[4] = malloc(8);
	strcpy((char*)x[1], "12345678");;
	x[1] = realloc(x[1], 17);
	strcat((char*)x[1], "87654321");;
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p,%p,%p,%p,%p,[%s]\n", testnum++,__LINE__, x[0], x[2], x[3], x[4], x[1], "1234567887654321");
	sendAndRecv(mq, HEAPWALK_FULL, resp, 8, 0);
	if ((x[0] == (int*)resp[0].ptr) && (x[2] == (int*)resp[1].ptr) && 
	    (x[3] == (int*)resp[2].ptr) && (x[4] == (int*)resp[3].ptr) && 
	    (x[1] == (int*)resp[4].ptr) && (!strcmp((char*)x[1], "1234567887654321")) && (NULL == (int*)resp[5].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p,%p,[%s],%p\n", 
		    resp[0].ptr, resp[1].ptr, resp[2].ptr, resp[3].ptr, 
		    resp[4].ptr, (char*)x[1], resp[5].ptr);
		failed++;
	}

	dbg(PRINT_ERROR, "\n%d. [%d] Show [null]\n", testnum++,__LINE__);
	x[5] = malloc(8);
	sendAndRecv(mq, HEAPWALK_MARKALL, resp, 8, 0);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if ((NULL == (int*)resp[0].ptr)) { 
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p\n", resp[0].ptr);
		failed++;
	}

	dbg(PRINT_ERROR, "\n%d. [%d] Show %p,%p,%p,%p,%p,%p\n", testnum++,__LINE__, x[0], x[2], x[3], x[4], x[1], x[5]);
	sendAndRecv(mq, HEAPWALK_FULL, resp, 8, 0);
	if ((x[0] == (int*)resp[0].ptr) && (x[2] == (int*)resp[1].ptr) && 
	    (x[3] == (int*)resp[2].ptr) && (x[4] == (int*)resp[3].ptr) && 
	    (x[1] == (int*)resp[4].ptr) && (x[5] == (int*)resp[5].ptr) && (NULL == (int*)resp[6].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p,%p,%p,%p\n", 
		    resp[0].ptr, resp[1].ptr, resp[2].ptr, resp[3].ptr, 
		    resp[4].ptr, resp[5].ptr, resp[6].ptr);
		failed++;
	}

	x[6] = malloc(8);
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p,%p,%p,%p,%p,%p,%p\n", testnum++,__LINE__, x[0], x[2], x[3], x[4], x[1], x[5], x[6]);
	sendAndRecv(mq, HEAPWALK_RESET_MARKED, resp, 8, 0);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if ((x[0] == (int*)resp[0].ptr) && (x[2] == (int*)resp[1].ptr) && 
	    (x[3] == (int*)resp[2].ptr) && (x[4] == (int*)resp[3].ptr) && 
	    (x[1] == (int*)resp[4].ptr) && (x[5] == (int*)resp[5].ptr) && 
	    (x[6] == (int*)resp[6].ptr) && (NULL == (int*)resp[7].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p,%p,%p,%p,%p\n", 
		    resp[0].ptr, resp[1].ptr, resp[2].ptr, resp[3].ptr, 
		    resp[4].ptr, resp[5].ptr, resp[6].ptr, resp[7].ptr);
		failed++;
	}

	free(x[0]);free(x[2]);free(x[3]);free(x[4]);free(x[1]);free(x[5]);free(x[6]);
	dbg(PRINT_ERROR, "\n%d. [%d] Show [null]\n", testnum++,__LINE__);
	sendAndRecv(mq, HEAPWALK_RESET_MARKED, resp, 8, 0);
	sendAndRecv(mq, HEAPWALK_MARKALL, resp, 8, 0);
	sendAndRecv(mq, HEAPWALK_FULL, resp, 8, 0);
	if ((NULL == (int*)resp[0].ptr)) { 
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p\n", resp[0].ptr);
		failed++;
	}

	x[0] = calloc(1,8);
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p\n", testnum++,__LINE__, x[0]);
	sendAndRecv(mq, HEAPWALK_RESET_MARKED, resp, 8, 0);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if ((x[0] == (int*)resp[0].ptr) && (NULL == (int*)resp[1].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p\n", resp[0].ptr, resp[1].ptr);
		failed++;
	}

	dbg(PRINT_ERROR, "\n%d. [%d] Show [null]\n", testnum++,__LINE__);
	sendAndRecv(mq, HEAPWALK_MARKALL, resp, 8, 0);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if (NULL == (int*)resp[0].ptr) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p\n", resp[0].ptr, resp[1].ptr);
		failed++;
	}

	dbg(PRINT_ERROR, "\n%d. [%d] Show %p\n", testnum++,__LINE__, x[0]);
	sendAndRecv(mq, HEAPWALK_MARKALL, resp, 8, 0);
	sendAndRecv(mq, HEAPWALK_FULL, resp, 8, 0);
	if ((x[0] == (int*)resp[0].ptr) && (NULL == (int*)resp[1].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p\n", resp[0].ptr, resp[1].ptr);
		failed++;
	}

	x[1] = calloc(1,8);
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p,%p\n", testnum++,__LINE__, x[0], x[1]);
	sendAndRecv(mq, HEAPWALK_RESET_MARKED, resp, 8, 0);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if ((x[0] == (int*)resp[0].ptr) && (x[1] == (int*)resp[1].ptr) && (NULL == (int*)resp[2].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p\n", resp[0].ptr, resp[1].ptr, resp[2].ptr);
		failed++;
	}

	x[2] = calloc(1,8);
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p,%p,%p\n", testnum++,__LINE__, x[0], x[1], x[2]);
	sendAndRecv(mq, HEAPWALK_RESET_MARKED, resp, 8, 0);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if ((x[0] == (int*)resp[0].ptr) && (x[1] == (int*)resp[1].ptr) && (x[2] == (int*)resp[2].ptr) && (NULL == (int*)resp[3].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p\n", resp[0].ptr, resp[1].ptr, resp[2].ptr, resp[3].ptr);
		failed++;
	}

	x[3] = calloc(1,8);
	dbg(PRINT_ERROR, "\n%d. [%d] Show %p,%p,%p,%p\n", testnum++,__LINE__, x[0], x[1], x[2], x[3]);
	sendAndRecv(mq, HEAPWALK_RESET_MARKED, resp, 8, 0);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if ((x[0] == (int*)resp[0].ptr) && (x[1] == (int*)resp[1].ptr) && (x[2] == (int*)resp[2].ptr) && (x[3] == (int*)resp[3].ptr) && (NULL == (int*)resp[4].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p,%p\n", resp[0].ptr, resp[1].ptr, resp[2].ptr, resp[3].ptr, resp[4].ptr);
		failed++;
	}

	x[4] = calloc(1,8);
	dbg(PRINT_ERROR, "\n%d. [%d] Show [null]\n", testnum++,__LINE__);
	sendAndRecv(mq, HEAPWALK_MARKALL, resp, 8, 0);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if (NULL == (int*)resp[0].ptr) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p\n", resp[0].ptr, resp[1].ptr);
		failed++;
	}

	dbg(PRINT_ERROR, "\n%d. [%d] Show %p,%p,%p,%p,%p\n", testnum++,__LINE__, x[0], x[1], x[2], x[3], x[4]);
	sendAndRecv(mq, HEAPWALK_RESET_MARKED, resp, 8, 0);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if ((x[0] == (int*)resp[0].ptr) && (x[1] == (int*)resp[1].ptr) && (x[2] == (int*)resp[2].ptr) && (x[3] == (int*)resp[3].ptr) && (x[4] == (int*)resp[4].ptr) && (NULL == (int*)resp[5].ptr)) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p,%p,%p,%p,%p\n", resp[0].ptr, resp[1].ptr, resp[2].ptr, resp[3].ptr, resp[4].ptr, resp[5].ptr);
		failed++;
	}

	dbg(PRINT_ERROR, "\n%d. [%d] Show [null]\n", testnum++,__LINE__);
	sendAndRecv(mq, HEAPWALK_MARKALL, resp, 8, 0);
	sendAndRecv(mq, HEAPWALK_INCREMENT, resp, 8, 0);
	if (NULL == (int*)resp[0].ptr) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p,%p\n", resp[0].ptr, resp[1].ptr);
		failed++;
	}

	dbg(PRINT_ERROR, "\n%d. [%d] Show [timeout]\n", testnum++,__LINE__);
	sendAndRecv(mq, HEAPWALK_BASE, resp, 8, 0);
	if ((int*)0xff == resp[0].ptr) {
		dbg(PRINT_ERROR, "\tPass\n");
		passed++;
	}
	else {
		dbg(PRINT_ERROR, "\tFail %p\n", resp[0].ptr);
		failed++;
	}

	dbg(PRINT_MUST, "Total test cases: %d [Pass %d Fail %d]\n", passed+failed, passed, failed);
	FILE *fp = fopen("/tmp/memleakutil_selftest.txt", "a");
	if (NULL != fp) {
		fprintf(fp, "%d:\t\t%s:       Pass %d Fail %d\n", 
				getpid(), __FUNCTION__, passed, failed);
		fclose(fp);
	}
	sleep (3);
}

void selftest()
{
	mqd_t mqrecv;

	/* semaphore for making child wait, otherwise which mq_util queue will not be synchronised by parent and child */
	/* No need for named semaphore, as fork'd child knows the semaphoe */
	sem_t *selftest_sem = mmap(NULL, sizeof(*selftest_sem), PROT_READ |PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS, -1, 0);
	if (selftest_sem == MAP_FAILED) {
		dbg(PRINT_MUST, "mmap failed, %s\n", strerror(errno));
		exit(1);
	}
	// start with locked
	sem_init(selftest_sem, 1, 0);

	dbg(PRINT_MUST, "%s: Testing me..\n", __FUNCTION__);

	mqrecv = createMq();

	pid_t childPid = 0;
	dispStatus();
	dbg(PRINT_MUST, "Run runListTests before libc load (1/0)\n");
	int c;
	//c = 1;
	scanf("%d", &c);
	if (1 == c) {
		dbg(PRINT_ERROR, "Running tests for %d\n", getpid());
		runListTests(mqrecv);
		runAllocationTests(mqrecv);
	}

	dbg(PRINT_INFO, "Before interpreting..\n");
	load_libc_functions();

	dbg(PRINT_MUST, "Run runListTests after libc load(1/0)\n");
	scanf("%d", &c);
	//c = 1;
	if (1 == c) {
		dbg(PRINT_ERROR, "Running tests for %d\n", getpid());
		runListTests(mqrecv);
		runAllocationTests(mqrecv);
	}

	
	/* No need for named semaphore, since we are not sharing across spawned processes.
	sem_unlink("/memleak_selftest");
	selftest_sem = sem_open("/memleak_selftest", O_CREAT|O_EXCL, 0644, 1);
	if (SEM_FAILED == selftest_sem)
		dbg(PRINT_ERROR, "Error sem_open, %s\n", strerror(errno));
	*/

	// lock the list mutex and see if fork gets locked...TODO....TODO...
	if((childPid = fork()) < 0) {
		dbg(PRINT_FATAL, "%s: fork() failed\n", __FUNCTION__);
	}
	else
	{
		if (0 == childPid){
			dbg(PRINT_MUST, "Inside child...%d\n", gettid());

			gMemInitialized = 2;
			//dispStatus();
			int semval;
			sem_getvalue(selftest_sem, &semval);
			dbg(PRINT_SEM, "%s: semval [%d], going to sem_wait\n", __FUNCTION__, semval);
			sem_wait(selftest_sem);
			dbg(PRINT_MUST, "Running tests for %d\n", getpid());
			runListTests(mqrecv);
			dbg(PRINT_SEM, "%s: going to sem_post\n", __FUNCTION__);
			sem_post(selftest_sem);

			pid_t childChildPid = 0;
			if((childChildPid = fork()) < 0) {
				dbg(PRINT_FATAL, "%s: fork() failed\n", __FUNCTION__);
			}
			else
			{
				if (0 == childChildPid){
					dbg(PRINT_MUST, "Inside childChild...%d\n", gettid());
					gMemInitialized = 3;
					dispStatus();
					int semval;
					sem_getvalue(selftest_sem, &semval);
					dbg(PRINT_SEM, "%s: semval [%d], going to sem_wait\n", __FUNCTION__, semval);
					sem_wait(selftest_sem);
					dbg(PRINT_MUST, "Running tests for %d\n", getpid());
					runListTests(mqrecv);
					runAllocationTests(mqrecv);
					dbg(PRINT_SEM, "%s: going to sem_post\n", __FUNCTION__);
					sem_post(selftest_sem);
				}
				else
				{
					dbg(PRINT_MUST, "Inside child after fork...%d\n", gettid());
					dispStatus();
					{
						int semval;
						dbg(PRINT_MUST, "Running tests for %d\n", getpid());
						sem_getvalue(selftest_sem, &semval);
						dbg(PRINT_SEM, "%s: semval [%d], going to sem_wait\n", __FUNCTION__, semval);
						sem_wait(selftest_sem);
						runListTests(mqrecv);
						runAllocationTests(mqrecv);
						dbg(PRINT_SEM, "%s: going to sem_post\n", __FUNCTION__);
						sem_post(selftest_sem);
						sem_getvalue(selftest_sem, &semval);
						dbg(PRINT_MUST, "%s: semval %d\n", __FUNCTION__, semval);
					}
				}
			}
		}
		else
		{
			dbg(PRINT_MUST, "Inside parent after fork...%d\n", gettid());
			dispStatus();
			{
				int semval;
				dbg(PRINT_MUST, "Running tests for %d\n", getpid());
				runListTests(mqrecv);
				runAllocationTests(mqrecv);
				dbg(PRINT_SEM, "%s: going to sem_post\n", __FUNCTION__);
				sem_post(selftest_sem);
				sem_getvalue(selftest_sem, &semval);
				dbg(PRINT_MUST, "%s: semval %d\n", __FUNCTION__, semval);
			}
		}
	}
	FILE *fp = fopen("/tmp/memleakutil_selftest.txt", "r");
	if (NULL != fp) {
		char str[256];
		while (fread(str, 256, 1, fp)) {
			PRINT("%s", str);
		}
		fclose(fp);
	}

	dbg(PRINT_MUST, "%s: sleep 120 secs before pausing..%d\n", __FUNCTION__, getpid());
	sleep(120);
	pause();
	sem_destroy(selftest_sem);
	//sem_close(selftest_sem);
	//sem_unlink("/memleak_selftest");

	mq_close(mqrecv);
	mq_unlink("/mq_util");
	dbg(PRINT_MUST, "%s: Pause..%d\n", __FUNCTION__, getpid());
	exit(0);
}
#endif
