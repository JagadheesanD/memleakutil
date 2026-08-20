/*
 * Copyright [2025] [Jagadheesan.D@gmail.com]
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
//#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>	  /* For O_* constants */
#include <sys/stat.h> /* For mode constants */
#include <mqueue.h>
#include <time.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <limits.h> /* For ULONG_MAX */
#include <dirent.h>

#include "memfns_wrap.h"

#ifdef SELF_TEST
extern void selftest();
extern void spawntestrunthread();
#endif

static const char versionString[] = "" MEMWRAP_MAJOR_VERSION "." MEMWRAP_MINOR_VERSION "";
char *rwPath;
char *fileSuffix;
int pid;
int tid;
int cmd;
int interactive;

#define OFFLINE_STORE 1
#define OFFLINE_PROCESS 2
int offlineAnalysis;


char storedTime[32];
typedef struct threadstat
{
	int tid;
	unsigned long long allocationSize;
	// unsigned long cputime;
	struct threadstat *next;
} threadStat;
threadStat *threadStatHead;
int baseCmd; /* Holds for which cmd to display total heap stats */

MMAP_info *mmapAnon, *mmapAnonTail;
MMAP_info *mmapAll, *mmapAllTail;
LIST_pagemap *pagemapHead, *pagemapTail;
// Main stack
unsigned long baseStack, baseStackSwap;
// pthread stack
unsigned long pthreadStack_tstack, pthreadStackSwap_tstack;
// exited pthread stack
unsigned long pthreadStack_etstack, pthreadStackSwap_etstack;
// stack not mapped
unsigned long stackInactive;
unsigned long totalrsspages;
unsigned long totalswappages;

long PAGE_SIZE = 4096; // Will be setting with sysconf(_SC_PAGE_SIZE) in main()

/**
 * @brief Creates a message queue for receiving messages.
 *
 * This function creates a message queue named /mq_util for receiving messages.
 * It handles the maximum message size and reports any errors encountered during creation.
 *
 * @return The message queue descriptor.
 */
mqd_t createMq()
{
	mqd_t mqrecv;
	struct mq_attr mqattr = ((struct mq_attr){0, QUEUE_MAXMSG, sizeof(msg_resp), 0, {0}});

	char procread_max[32];
	FILE *fp = fopen("/proc/sys/fs/mqueue/msgsize_max", "r");
	if (fp)
	{
		if (fgets(procread_max, sizeof(procread_max) - 1, fp) != NULL)
		{
			if (sizeof(msg_resp) > atoi(procread_max)) {
#ifdef OPTIMIZE_MQ_TRANSFER 
				dbg(PRINT_FATAL, "\nReduce msg_resp size (currently %lu) by reducing MAX_MSG_XFER\n", sizeof(msg_resp));
#else
				dbg(PRINT_FATAL, "\nReduce msg_resp size (currently %lu) by reducing MQ_MSG_SIZE\n", sizeof(msg_resp));
#endif
				fclose(fp);
				exit(1);
			}
		}
		fclose(fp);
	}
	mqrecv = mq_open("/mq_util", O_CREAT | O_RDONLY, QUEUE_PERMISSION, &mqattr);
	/* For testing!! */
	/*if (-1 < mqrecv) {
		PRINT("%s: Simulating EINVAL(%d), [%s] for mq_open\n", __FUNCTION__, EINVAL, strerror(EINVAL));
		mq_close(mqrecv);
		mqrecv = -1;
		errno = EINVAL;
	}*/
	if (0 > mqrecv)
	{
		/* EINVAL - With O_CREAT and setting mqattr->mq_maxmsg, mqattr->mq_msqsize with
		 * 	    QUEUE_MAXMSG (256) and sizeof(msg_resp) (4824 - when OPTIMIZE_MQ_TRANSFER is defined)
		 *          this error is reported. In that case, we try opening with /proc/sys/fs/mqueue/msg_max 
		 *          value present. If still reporting error, then try with /proc/sys/fs/mqueue/msg_default.
		 *
		 * EMFILE - In some platforms, even for the above error, this errno is reported. 
		 */

		if ((EINVAL == errno) || (EMFILE == errno))
		{
			dbg(PRINT_MUST, "Error, cannot open the queue /mq_util [%s]...Trying with msg_max size\n", strerror(errno));
			fp = fopen("/proc/sys/fs/mqueue/msg_max", "r");
			if (fp)
			{
				if (fgets(procread_max, sizeof(procread_max) - 1, fp) != NULL)
				{
					mqattr.mq_maxmsg = atoi(procread_max);
					dbg(PRINT_MUST, "/proc/sys/fs/mqueue/msg_max limit is %ld\n", mqattr.mq_maxmsg);
					mqrecv = mq_open("/mq_util", O_CREAT | O_RDONLY, QUEUE_PERMISSION, &mqattr);

					/* For testing!! */
					/*if (-1 < mqrecv) {
						PRINT("%s: Simulating EINVAL(%d), [%s] for mq_open\n", __FUNCTION__, EINVAL, strerror(EINVAL));
						mq_close(mqrecv);
						mqrecv = -1;
						errno = EINVAL;
					}*/
					if (0 > mqrecv) {
						if ((EINVAL == errno) || (EMFILE == errno))
						{
							dbg(PRINT_MUST, "Error, cannot open the queue /mq_util [%s]...Trying with msg_default size\n", strerror(errno));
							fclose(fp);
							fp = fopen("/proc/sys/fs/mqueue/msg_default", "r");
							if (fp)
							{
								if (fgets(procread_max, sizeof(procread_max) - 1, fp) != NULL)
								{
									mqattr.mq_maxmsg = atoi(procread_max);
									dbg(PRINT_MUST, "/proc/sys/fs/mqueue/msg_default limit is %ld\n", mqattr.mq_maxmsg);
									mqrecv = mq_open("/mq_util", O_CREAT | O_RDONLY, QUEUE_PERMISSION, &mqattr);
								}
							}
						}

					}
				}
				if (fp) {
					fclose(fp);
				}
			}
		}
	}

	if (0 > mqrecv)
	{
		dbg(PRINT_FATAL, "Error, cannot open the queue: /mq_util [%s].\n", strerror(errno));
		exit(1);
	}

	/* purge initial msg if any. Useful during process restarts */
	struct timespec tm;
	unsigned int prio;
	int purgedMsgs = -1;
	msg_resp msgresp;
	do
	{
		purgedMsgs++;
		clock_gettime(CLOCK_REALTIME, &tm);
		tm.tv_sec += 1;
	} while (0 <= mq_timedreceive(mqrecv, (char *)&msgresp, sizeof(msg_resp), &prio, &tm));

	if (ETIMEDOUT != errno)
	{
		dbg(PRINT_FATAL, "/mq_util Purge Error, mq_receive: [%s] errno [%d]\n", strerror(errno), errno);
	}

	if (purgedMsgs)
	{
		dbg(PRINT_MSGQ, "mq_util purged %d msgs..\n", purgedMsgs);
	}
	return mqrecv;
}

#ifdef OPTIMIZE_MQ_TRANSFER
/**
 * @brief Stores heapwalk data to a file.
 *
 * This function receives heapwalk data from the message queue and stores it to a file for analysis.
 *
 * @param mqrecv The message queue descriptor to receive messages from.
 * @param cmd The command indicating the type of heapwalk operation.
 * @param pid The process ID of the target process.
 * @param isSelfTest Flag indicating whether this is a self-test operation.
 */
int storeHeapwalk(mqd_t mqrecv, int cmd, int pid, bool isSelfTest)
{
	msg_resp msgresp;
#ifdef SELF_TEST1
	unsigned index = 0;
	int stcmd = cmd;
#endif
	unsigned int prio;
	struct timespec tm;
	int msgsize = sizeof(msg_resp);
	char heapwalkFile[32];
	dbg(PRINT_INFO, "%s+\n", __FUNCTION__);
#ifndef SELF_TEST1
	sprintf(heapwalkFile, "%s/hp_%d%s.dat", rwPath, pid, fileSuffix?fileSuffix:"");
	FILE *fpHWalk = fopen(heapwalkFile, "wb");
	if (NULL == fpHWalk)
	{
		dbg(PRINT_MUST, "%s open error, %s\n", heapwalkFile, strerror(errno));
		return 1;
	}
	FILE *fpCurrent = fpHWalk;
	FILE *fpHWFull;
	if ((HEAPWALK_FULL == cmd) || (HEAPWALK_MMAP_ENTRIES == cmd))
	{
		sprintf(heapwalkFile, "%s/hpf_%d%s.dat", rwPath, pid, fileSuffix?fileSuffix:"");
		fpHWFull = fopen(heapwalkFile, "wb");
		if (NULL == fpHWFull)
		{
			dbg(PRINT_MUST, "%s open error, %s\n", heapwalkFile, strerror(errno));
			fclose(fpHWalk);
			return 1;
		}
		fpCurrent = fpHWFull;
	}
	else
	{
		fpHWFull = NULL;
	}
#endif
	while (0 != msgsize)
	{
		clock_gettime(CLOCK_REALTIME, &tm);
		tm.tv_sec += 10;
		msgsize = mq_timedreceive(mqrecv, (char *)&msgresp, sizeof(msg_resp), &prio, &tm);
		if (-1 == msgsize) {
			if (ETIMEDOUT == errno) {
				dbg(PRINT_MUST, "%s:%d: Giving up..waited for 10 secs\n", __FUNCTION__, __LINE__);
			}else {
				dbg(PRINT_MUST, "%s:%d: mq_timedreceive failed [%s]\n", __FUNCTION__, __LINE__, strerror(errno));
			}
#ifndef SELF_TEST1
			if (fpHWFull == fpCurrent) {
				fclose(fpHWFull);
			}
			fclose(fpHWalk);
#endif
			return 1;
		}
		else if (msgsize)
		{
			unsigned int info = msgresp.numItemOrInfo & 0x30000000;
			if (info)
			{
#ifndef SELF_TEST1
				if (!fwrite((void *)&msgresp, sizeof(msg_resp), 1, fpCurrent))
				{
					dbg(PRINT_MUST, "%s: Error storing %s\n", __FUNCTION__, strerror(errno));
				}
#else
				memcpy(&stmsgresp[index++], &msgresp, sizeof(msg_resp));
#endif
				if (HEAPWALK_ITEM_CONTN == info)
				{
					continue;
				}
			}
#ifndef SELF_TEST1
			if (fpHWFull == fpCurrent)
			{
				if (!isSelfTest)
				{
					dbg(PRINT_MUST, "Done already walked\n");
				}
				fclose(fpHWFull);
				fpCurrent = fpHWalk;
			}
			else
			{
				if (!isSelfTest)
				{
					dbg(PRINT_MUST, "Done heapwalk\n");
				}
				fclose(fpHWalk);
				break;
			}
#else
			if (HEAPWALK_INCREMENT != stcmd) { 
					//|| (HEAPWALK_MMAP_ENTRIES == cmd))
				sprintf(heapwalkFile, "%s/hpf_%d%s.dat", rwPath, pid, fileSuffix?fileSuffix:"");
				FILE *fpHWFull = fopen(heapwalkFile, "wb");
				if (NULL == fpHWFull) {
					dbg(PRINT_MUST, "%s open error, %s\n", heapwalkFile, strerror(errno));
					return 1;
				}
				if (!fwrite((void *)&msgresp, sizeof(msg_resp), 1, fpHWFull))
				{
					dbg(PRINT_MUST, "%s: Error storing %s\n", __FUNCTION__, strerror(errno));
				}
				fclose(fpHWFull);
				memset(&msgresp, 0, sizeof(msgresp));
				index = 0;
				stcmd = HEAPWALK_INCREMENT;
			}	
			else {
				sprintf(heapwalkFile, "%s/hp_%d%s.dat", rwPath, pid, fileSuffix?fileSuffix:"");
				FILE *fpHWFull = fopen(heapwalkFile, "wb");
				if (NULL == fpHWFull) {
					dbg(PRINT_MUST, "%s open error, %s\n", heapwalkFile, strerror(errno));
					return 1;
				}
				if (!fwrite((void *)&msgresp, sizeof(msg_resp), 1, fpHWFull))
				{
					dbg(PRINT_MUST, "%s: Error storing %s\n", __FUNCTION__, strerror(errno));
				}
				fclose(fpHWFull);
				break;
			}			
#endif
		}
	}
	return 0;
}

int getStackIntercepts(mqd_t mqrecv, unsigned pid)
{
	msg_resp msgresp;
	unsigned int prio;
	struct timespec tm;
	int msgsize = sizeof(msg_resp);
	char heapwalkFile[128];
	sprintf(heapwalkFile, "%s/hps_%d%s.dat", rwPath, pid, fileSuffix?fileSuffix:"");
	FILE *fpHWalk = fopen(heapwalkFile, "wb");
	if (NULL == fpHWalk)
	{
		dbg(PRINT_MUST, "%s open error, %s\n", heapwalkFile, strerror(errno));
		return 1;
	}
	while (0 != msgsize)
	{
		clock_gettime(CLOCK_REALTIME, &tm);
		tm.tv_sec += 10;
		msgsize = mq_timedreceive(mqrecv, (char *)&msgresp, sizeof(msg_resp), &prio, &tm);
		if (-1 == msgsize) {
			if (ETIMEDOUT == errno) {
				dbg(PRINT_MUST, "%s:%d: Giving up..waited for 10 secs\n", __FUNCTION__, __LINE__);
			}else {
				dbg(PRINT_MUST, "%s:%d: mq_timedreceive failed [%s]\n", __FUNCTION__, __LINE__, strerror(errno));
			}
			fclose(fpHWalk);
			return 1;
		}
		else if (msgsize)
		{
			unsigned int info = msgresp.numItemOrInfo & 0x30000000;
			if (info)
			{
				if (!fwrite((void *)&msgresp, sizeof(msg_resp), 1, fpHWalk))
				{
					dbg(PRINT_MUST, "%s: Error storing %s\n", __FUNCTION__, strerror(errno));
				}
				dbg(PRINT_INFO, "pthread intercept received (%u) entries\n", msgresp.numItemOrInfo & 0xfffffff);
				if (HEAPWALK_ITEM_CONTN == info)
				{
					continue;
				}
			}
			fclose(fpHWalk);
			break;
		}
	}
	return 0;
}

void mappthreadStack(unsigned pid)
{
	msg_resp msgresp;
	int msgsize = sizeof(msg_resp);
	char tmp[128]; 
	FILE *fpIntercept;
	unsigned totalMsgs = 0;

	sprintf(tmp, "%s/hps_%d%s.dat", rwPath, pid, fileSuffix?fileSuffix:"");
	fpIntercept = fopen(tmp, "rb");

	if (fpIntercept)
	{
		stackInactive = 0;
		unsigned msgIndex;
		do
		{
			msgsize = fread(&msgresp, 1, sizeof(msgresp), fpIntercept);
			if (msgsize) {
				if (msgresp.numItemOrInfo)
				{
					msgIndex = 0;
					int msgCount = msgresp.numItemOrInfo & 0xFFFFFFF;
					totalMsgs += msgCount;
					dbg(PRINT_INFO, "Total pthread intercept stack count %u\n", totalMsgs);
					while (msgIndex < msgCount) {
						// Iterate through anon entry list to update entries belonging to stack
						// Unoptimized, but for now okay
						MMAP_info *tmpanon = mmapAnon;
						//unsigned found = 0;
						unsigned rssvalue = 0;
						unsigned swapvalue = 0;
						while (tmpanon) {
							void *ptr = msgresp.xfer[msgIndex].stack_addr_bottom;
							unsigned size = msgresp.xfer[msgIndex].size;
							unsigned long stack_addr_bottom = (unsigned long)ptr; //msgresp.xfer[msgIndex].stack_addr_bottom;
							if ((stack_addr_bottom == tmpanon->startAddress) && ((stack_addr_bottom+size) == tmpanon->endAddress)) {
								if (!strcmp(tmpanon->entryName, "[anon]")) {
									strcpy(tmpanon->entryName, (msgresp.xfer[msgIndex].pthread_id)? "[tstack]" : "[etstack]");
								}
								else if (strlen(tmpanon->entryName) < (256 - 9)) {
									strcat(tmpanon->entryName, (msgresp.xfer[msgIndex].pthread_id)? "/[tstack]" : "/[etstack]");
								}
								if (msgresp.xfer[msgIndex].pthread_id) {
									pthreadStack_tstack += tmpanon->rss;
									pthreadStackSwap_tstack += tmpanon->swapPss;
								}
								else {
									pthreadStack_etstack += tmpanon->rss;
									pthreadStackSwap_etstack += tmpanon->swapPss;
								}
								dbg(PRINT_INFO, "Found stack %lx size %lu in [anon], now %s, %lx:%lx with rss %u\n", 
									stack_addr_bottom, msgresp.xfer[msgIndex].size, tmpanon->entryName, tmpanon->startAddress, tmpanon->endAddress, tmpanon->rss);
								//found = 1;
								rssvalue = tmpanon->rss;
								swapvalue = tmpanon->swapPss;
								break;
							} else if ((stack_addr_bottom >= tmpanon->startAddress) && ((stack_addr_bottom+size) <= tmpanon->endAddress)) {
								if (strlen(tmpanon->entryName) < (256 - 9)) {
									strcat(tmpanon->entryName, (msgresp.xfer[msgIndex].pthread_id)? "/[tstack]" : "/[etstack]");
								}
								dbg(PRINT_INFO, "Found stack %lx size %lu in %s, %lx:%lx with rss %u\n", 
									stack_addr_bottom, msgresp.xfer[msgIndex].size, tmpanon->entryName, tmpanon->startAddress, tmpanon->endAddress, tmpanon->rss);

								LIST_pagemap *pagetmp = pagemapHead;
								unsigned printonce = 1;
								// TODO optimize
								// Found stack 7fd4892b2000 in anon's, 7fd4892b2000:7fd489ab2000

								while (pagetmp) {
									if (pagetmp->pageaddress > ptr) { // No point in going past
										dbg(PRINT_ERROR, "Breaking, ptr %p < pagemap %p\n", ptr, pagetmp->pageaddress);
										break;
									}
									if (pagetmp->pageaddress == ptr)  {
										unsigned sizeinpage = (pagetmp->pageaddress + PAGE_SIZE) - ptr;
										if (pagetmp->pagestat) {
											dbg(PRINT_INFO, "Found pagemap for stackaddr %p, pageaddress %p, size %u, sizeinpage %u\n",
													ptr, pagetmp->pageaddress, size, sizeinpage);
											if (PAGE_ACCOUNTED == pagetmp->pagestat) {
												dbg(PRINT_ERROR, "Page accounted !!, shouldn't happen in a stack space..stackaddr %p, pageaddress %p\n",
														ptr, pagetmp->pageaddress);
											}
											if ((unsigned)sizeinpage >= size) {
												if (PAGE_PRESENT & pagetmp->pagestat) {
												       rssvalue += size;
												}
												else if (PAGE_SWAPPED & pagetmp->pagestat) {
													swapvalue += size;
												}
												pagetmp->pagestat = PAGE_ACCOUNTED;
												break;
											}else {
												ptr += sizeinpage;
												size = size - sizeinpage;
												if (PAGE_PRESENT & pagetmp->pagestat) {
													rssvalue += sizeinpage;
												}
												else if (PAGE_SWAPPED & pagetmp->pagestat) {
													swapvalue += sizeinpage;
												}
												pagetmp->pagestat = PAGE_ACCOUNTED;
											}
										}
										else {
											if (printonce) {
												printonce = 0;
												dbg(PRINT_INFO, "Page %p no rss, see next page\n", pagetmp->pageaddress);
											}
											ptr += sizeinpage;
											size = size - sizeinpage;
										}
									}
									pagetmp = pagetmp->next;
								}
								dbg(PRINT_INFO, "Stack %p size %u rss %u swap %u\n", ptr, size, rssvalue, swapvalue);
								if (msgresp.xfer[msgIndex].pthread_id) {
									pthreadStack_tstack += (rssvalue * 4);
									pthreadStackSwap_tstack += (swapvalue * 4);
								}
								else {
									pthreadStack_etstack += (rssvalue * 4);
									pthreadStackSwap_etstack += (swapvalue * 4);
								}
	
								//found = 1;
								break;
							}
							tmpanon = tmpanon->next;
						} // while (tmpanon)
						//if (!found) {
						if (!tmpanon) {
							// Then this thread is no longer active...
							if (msgresp.xfer[msgIndex].pthread_id) {
								dbg(PRINT_ERROR, "Active thread not having anon entry!!!\n");
							}
							stackInactive += msgresp.xfer[msgIndex].size;
						}
						else {
							// Right now not used..
							// ?? tmpanon->pthreadinfo.pthread_id = msgresp.xfer[msgIndex].pthread_id;
							tmpanon->pthreadinfo.size = msgresp.xfer[msgIndex].size;
							tmpanon->pthreadinfo.stack_rss = rssvalue;
							tmpanon->pthreadinfo.stack_swap = swapvalue;
							tmpanon->pthreadinfo.start_routine = msgresp.xfer[msgIndex].start_routine;
							// ?? tmpanon->pthreadinfo.time = msgresp.xfer[msgIndex].seconds;
						}
						msgIndex++;
					} // while (msgIndex < msgCount) 
				}
			}
		} while (msgsize);
		fclose(fpIntercept);
	}
	else
	{
		dbg(PRINT_MUST, "%s: %s open error, %s\n", __FUNCTION__, tmp, strerror(errno));
	}
}

void freePagemapDataStruct()
{
	LIST_pagemap *tmp = pagemapHead;
	while (tmp) {
		pagemapHead = tmp->next;
		free((void*)tmp);
		tmp = pagemapHead;
	}
}

void addPagemapToDataStruct(unsigned long addr, PAGEMAPSTAT stat)
{
	LIST_pagemap *tmp = (LIST_pagemap*)malloc(sizeof(LIST_pagemap));

	if (tmp) {
		tmp->pageaddress = (void*)addr;
		tmp->pagestat = stat;
		tmp->next = NULL;
		if (pagemapHead) {
			pagemapTail->next = tmp;
			pagemapTail = tmp;
		}
		else {
			pagemapHead = pagemapTail = tmp;
		}
	}
	else {
		dbg(PRINT_ERROR, "%s: Failed to add\n", __FUNCTION__);
	}
}

void storeAnonHeapStackPagemap(int pid)
{
	char buf[4096];
	sprintf(buf, "/proc/%d/pagemap", pid);
	FILE *src = fopen(buf, "rb");
	if (src) {
		sprintf(buf, "%s/hpp_%d%s.txt", rwPath, pid, fileSuffix?fileSuffix:"");
		FILE *dest = fopen(buf, "w");
		if (dest) {
			MMAP_info *mmapAnonTmp = mmapAnon;
			unsigned long long pageinfo;
			while (mmapAnonTmp)
			{
				unsigned long addr = mmapAnonTmp->startAddress;
				while (addr < mmapAnonTmp->endAddress)
				{
					// usually page size of 4096, and page info is 8 bytes. so jump to the entry
					if (0 != fseek(src, (addr / PAGE_SIZE) * 8, SEEK_SET)) {
						PRINT("Failed to fseek 0x%lx, %ld, %s\n", addr, PAGE_SIZE, strerror(errno));
						continue;
					}
					if(fread(&pageinfo, 8, 1, src)) {
					// we are interested only if page present or swapped. PFN might be 0, if CAP_SYS_ADMIN capability is not present.
					sprintf(buf, "0x%lx %u\n", addr, (unsigned)(pageinfo>>60));
					fwrite(buf, 1, strlen(buf), dest);
					addPagemapToDataStruct(addr, 
							(pageinfo & 0xC000000000000000)? ((pageinfo & 0x8000000000000000)? PAGE_PRESENT : PAGE_SWAPPED) : PAGE_NOT_PRESENT);
					addr += PAGE_SIZE;
					}
				}
				mmapAnonTmp = mmapAnonTmp->next;
			}
			fclose(dest);
		}
		else {
			PRINT("%s: %s fopen failed %s\n", __FUNCTION__, buf, strerror(errno));
		}
		fclose(src);
	}
	else {
		PRINT("%s: %s fopen failed %s\n", __FUNCTION__, buf, strerror(errno));
	}
}

int readStoredPagemap(int pid)
{
	char buf[256];
	sprintf(buf, "%s/hpp_%d%s.txt", rwPath, pid, fileSuffix?fileSuffix:"");
	FILE *fp = fopen(buf, "r");
	if (fp) {
		unsigned pageinfo;
		unsigned long addr;
		while (fgets(buf, 256, fp)) {
			if (2 <= sscanf(buf, "0x%lx %u\n", &addr, &pageinfo)) {
				addPagemapToDataStruct(addr, 
						(pageinfo & 0xC)? ((pageinfo & 0x8)? PAGE_PRESENT : PAGE_SWAPPED) : PAGE_NOT_PRESENT);
			}
		}
		fclose(fp);
	}
	else {
		PRINT("%s: %s fopen failed %s\n", __FUNCTION__, buf, strerror(errno));
	}
	return (pagemapHead)? 0:1;
}

/**
 * @brief Adds an anonymous memory entry.
 *
 * This function adds an anonymous memory entry to the linked list of memory entries.
 *
 * @param addMe The memory entry to be added.
 */
void addMMapEntry(MMAP_info addMe, MMAP_info **addTo, MMAP_info **addToTail)
{
	MMAP_info *tmpAdd = (MMAP_info *)malloc(sizeof(MMAP_info));
	if (tmpAdd)
	{
		memcpy(tmpAdd, &addMe, sizeof(MMAP_info));
		if (!strcmp(tmpAdd->entryName, "[stack]")) {
			baseStack = tmpAdd->rss;
			baseStackSwap = tmpAdd->swapPss;
		}
	}
	else
	{
		dbg(PRINT_MUST, "%s: Alloc error %s\n", __FUNCTION__, strerror(errno));
		exit(0);
	}
	if (*addTo)
	{
		//if (mmapAnon != mmapAnonTail)
		if (*addTo != *addToTail)
		{
			(*addToTail)->prev->next = tmpAdd;
		}
		// else
		{
			(*addToTail)->next = tmpAdd;
			*addToTail = tmpAdd;
			tmpAdd->prev = *addToTail;
		}
	}
	else
	{
		*addTo = *addToTail = tmpAdd;
	}
}

/**
 * @brief Frees mmapAnon list entries.
 *
 * This function frees all anon memory entry from the linked list of mmapAnon.
 *
 * @param void.
 */
void freeMMapList()
{
	MMAP_info *tmprem;
	while (mmapAnon) {
		tmprem = mmapAnon;
		mmapAnon = mmapAnon->next;
		free(tmprem);
		/* care to set prev for tmprem?? */
		//if (mmapAnon) {
		//	mmapAnon->prev = NULL;
		//}
	}
	mmapAnon = mmapAnonTail = NULL;
	while (mmapAll) {
		tmprem = mmapAll;
		mmapAll = mmapAll->next;
		free(tmprem);
	}
	mmapAll = mmapAllTail = NULL;
	baseStack = baseStackSwap = pthreadStack_tstack = pthreadStackSwap_tstack = pthreadStack_etstack = pthreadStackSwap_etstack = 0;
}

/**
 * @brief Adds a thread statistics entry.
 *
 * This function adds a new thread statistics entry or updates the allocation size for an existing entry.
 *
 * @param tid The thread ID.
 * @param size The size of the allocation to be added.
 */
void addThreadStatEntry(int tid, unsigned long size)
{
	threadStat *tmp = threadStatHead, *prev = NULL;
	/* Check if thread already present */
	while (tmp)
	{
		if (tmp->tid == tid)
		{
			tmp->allocationSize += size;
			return;
		}
		prev = tmp;
		tmp = tmp->next;
	}
	tmp = (threadStat *)malloc(sizeof(threadStat));
	if (tmp)
	{
		tmp->tid = tid;
		tmp->allocationSize = size;
		tmp->next = NULL;
	}
	else
	{
		dbg(PRINT_MUST, "%s: Alloc error %s\n", __FUNCTION__, strerror(errno));
		exit(0);
	}

	if (prev)
	{
		prev->next = tmp;
	}
	else
	{
		threadStatHead = tmp;
	}
}

/**
 * @brief Prints thread statistics.
 *
 * This function prints the total allocation size for each thread.
 */
void printAndFreeThreadStat()
{
	threadStat *threadstat = threadStatHead;
	PRINT("\nThreadwise Allocation total in bytes:\nTid:\t");
	while (threadstat)
	{
		PRINT("%d\t", threadstat->tid);
		threadstat = threadstat->next;
	}
	threadstat = threadStatHead;
	PRINT("\nSize:\t");
	while (threadstat)
	{
		PRINT("%llu\t", threadstat->allocationSize);

		threadstat = threadstat->next;
		free(threadStatHead);
		threadStatHead = threadstat;
	}
	PRINT("\n");
}

void freeThreadStat()
{
	threadStat *threadstat = threadStatHead;
	while (threadstat)
	{
		threadstat = threadstat->next;
		free(threadStatHead);
		threadStatHead = threadstat;
	}
}

unsigned isASLREnabled;
unsigned getASLRStatus()
{
	char tmp[32];
	tmp[0] = 0;
	FILE *fp = fopen("/proc/sys/kernel/randomize_va_space", "r");
	if (fp) {
		if(!fread(tmp, 1, 32, fp)) tmp[0] = '\0';
		fclose(fp);
	}
	printf("\n%s: ASLR %d\n", __FUNCTION__, atoi(tmp));
	return atoi(tmp);
}

/* During offline analysis, read from rwPath, and populate anon */
/* pass createMap = 0 to not create any map, MMAP_ANON(1) to create only anon/stack/heap map, MMAP_ALL(2) to create all map */
int readStoredSmaps(unsigned pid, unsigned createMap)
{
	char mmapTmpArray[1024]; /* Used to read entries from /proc/pid/smaps...big enough to hold large entries */
	FILE *fpMmap;
	
	sprintf(mmapTmpArray, "%s/smaps_%d%s.txt", rwPath, pid, fileSuffix?fileSuffix:"");
	fpMmap = fopen(mmapTmpArray, "r");

	if (NULL != fpMmap) {
		dbg(PRINT_INFO, "smaps file available, %s\n", mmapTmpArray);
		fgets(mmapTmpArray, 1024, fpMmap);
		// TODO check return status
		sscanf(mmapTmpArray, "ASLR: %u Time: %*s\n", &isASLREnabled);
		MMAP_info tmp = {0};
		while (fgets(mmapTmpArray, 1024, fpMmap)) {
			// Checking for reading 6 entries, since anon entries will be unnamed, thus reading only 6
			if (6 <= sscanf(mmapTmpArray, "%lx-%lx %u %u %u %s %s", &tmp.startAddress, &tmp.endAddress, &tmp.size, &tmp.rss, &tmp.swapPss, tmp.perm, tmp.entryName)) {
				dbg(PRINT_NOISE, "Read %lx-%lx %u %u %u %s %s from %s", tmp.startAddress, tmp.endAddress, tmp.size, tmp.rss, tmp.swapPss, tmp.perm, tmp.entryName, mmapTmpArray);
				if (MMAP_ALL == createMap) {
					addMMapEntry(tmp, &mmapAll, &mmapAllTail);
				}
				else if (MMAP_ANON == createMap && (('\0' == tmp.entryName[0]) || (strstr(tmp.entryName, "[anon]")) || (strstr(tmp.entryName, "[heap]")) || (strstr(tmp.entryName, "[stack]")))) {
					dbg(PRINT_INFO, "Adding %lx-%lx %u %u %u %s %s\n", tmp.startAddress, tmp.endAddress, tmp.size, tmp.rss, tmp.swapPss, tmp.perm, tmp.entryName);
					if ('\0' == tmp.entryName[0]) { // May not be needed, but still
						strcpy(tmp.entryName, "[anon]");
					}
					addMMapEntry(tmp, &mmapAnon, &mmapAnonTail);
				}
				memset(&tmp, 0, sizeof(MMAP_info));
			}
		}
		fclose(fpMmap);
	}
	else {
		PRINT("%s: Open failed, errno %d [%s]\n", mmapTmpArray, errno, strerror(errno));
	}
	return ((MMAP_ANON == createMap && mmapAnon) || (MMAP_ALL == createMap && mmapAll))? 0:1;
}
/*
 * During normal menu, read from /proc, store and populate anon when cmd is 3 -- !analyze 
 * During normal cmd 1 or 2, read from /proc and store
 */
int readAndStoreSmaps(unsigned pid, bool createAnon)
{
	char mmapTmpArray[1024]; /* Used to read entries from /proc/pid/smaps...big enough to hold large entries */
	FILE *fpMmap;
	/* For validation only */
	//sprintf(mmapTmpArray, "cat /proc/%d/smaps > %s/test_smaps_%d.txt", pid, rwPath, pid);
	//system(mmapTmpArray);
	sprintf(mmapTmpArray, "%s/smaps_%d%s.txt", rwPath, pid, fileSuffix?fileSuffix:"");
	fpMmap = fopen(mmapTmpArray, "w");

	if (NULL != fpMmap) {
		static unsigned skipToEntry = 0, skipToSize = 0, skipToRss = 0, skipToSwapPss = 0, skipToRollover = 0; 
		unsigned skippedToLearn = 0;

		time_t timenow = time(NULL);
		struct tm *tmNow = localtime(&timenow);
		if (0 == strftime(mmapTmpArray, sizeof(mmapTmpArray), "YYYY_MM_DD HH_MM_SS %Y_%m_%d %H_%M_%S", tmNow)) {
			// Shouldn't fail unless mmapTmpArray is not big enough to hold
			sprintf(mmapTmpArray, "Date_in_epoch: %lu secs", timenow); // see if this is warned in 32 bit systems..
		}
		isASLREnabled = getASLRStatus();
		fprintf(fpMmap, "ASLR: %u Time: %s\n", isASLREnabled, mmapTmpArray);
		sprintf(mmapTmpArray, "/proc/%u/smaps", pid);
		
		FILE *smap = fopen(mmapTmpArray, "r");
		if (smap) {
			//sprintf(mmapTmpArray, "cat /proc/%u/smaps > /tmp/smaps_tst.txt", pid);
			//system(mmapTmpArray);
			unsigned offset = 0;
			unsigned lines_To_skip = skipToEntry;
			unsigned skipped = 1; // Tracks current skips
			unsigned expect_entry = 1, expect_size = 0, expect_rss = 0, expect_swap_pss = 0;

			MMAP_info tmp = {0};
			while (fgets(mmapTmpArray, 1024, smap)) {
				if (skipToRollover) { // Learnt the format
					if (++skipped > lines_To_skip) {
						if (expect_entry) {
							/* aaaad3ff0000-aaaad4139000 r-xp 00000000 b3:02 2368                       /usr/bin/bash */
							// Checking for 3 entries below, since an entry can be unnamed
							if (4 <= sscanf(mmapTmpArray, "%lx-%lx %s %x %*s %*u %s", &tmp.startAddress, &tmp.endAddress, tmp.perm, &offset, tmp.entryName)) {
								tmp.startAddress -= offset;
								dbg(PRINT_NOISE,"Read Entry %s %lx-%lx %s", mmapTmpArray, tmp.startAddress, tmp.endAddress, tmp.entryName);
								/*if (!createAnon || (('\0' == tmp.entryName[0]) && (strstr(tmp.entryName, "heap")) && (strstr(tmp.entryName, "stack")))) {
									dbg(PRINT_NOISE, "Can be skipped: %lu-%lu %s %s\n", tmp.startAddress, tmp.endAddress, tmp.perm, tmp.entryName);
									lines_To_skip = skipToSize + skipToRss + skipToSwapPss + skipToRollover;
								}
								else*/ {
									lines_To_skip = skipToSize;
									expect_size = 1;
									expect_entry = 0;
								}
								skipped = 1;
							}
							else {
								dbg(PRINT_ERROR,"Error Reading Entry from %s", mmapTmpArray);
							}
						} 
						else if (expect_size) {
							if (sscanf(mmapTmpArray, "Size: %u kB", &tmp.size)) {
								dbg(PRINT_NOISE,"Read Entry %s %u", mmapTmpArray, tmp.size);
								lines_To_skip = skipToRss;
								skipped = 1;
								expect_rss = 1;
								expect_size = 0;
							}
							else {
								dbg(PRINT_ERROR,"Error Reading Size from %s", mmapTmpArray);
							}
						} 
						else if (expect_rss) {
							if (sscanf(mmapTmpArray, "Rss: %u kB", &tmp.rss)) {
								dbg(PRINT_NOISE,"Read Entry %s %u", mmapTmpArray, tmp.rss);
								if (!tmp.rss) {
									lines_To_skip = skipToSwapPss + skipToRollover;
									expect_entry = 1;
									if (createAnon && (('\0' == tmp.entryName[0]) || (strstr(tmp.entryName, "[heap]")) || (strstr(tmp.entryName, "[stack]")))) {
										if ('\0' == tmp.entryName[0]) {
											strcpy(tmp.entryName, "[anon]");
											addMMapEntry(tmp, &mmapAnon, &mmapAnonTail);
										}
									}
									fprintf(fpMmap, "%lx-%lx %u %u 0 %s %s\n", tmp.startAddress, tmp.endAddress, tmp.size, tmp.rss, tmp.perm, tmp.entryName);
									memset(&tmp, 0, sizeof(MMAP_info));
								}
								else {
									lines_To_skip = skipToSwapPss;
									expect_swap_pss = 1;
								}
								skipped = 1;
								expect_rss = 0;
							}
							else {
								dbg(PRINT_ERROR,"Error Reading Rss from %s", mmapTmpArray);
							}
						}
						else if (expect_swap_pss) {
							if (sscanf(mmapTmpArray, "SwapPss: %u kB", &tmp.swapPss)) {
								if (createAnon && (('\0' == tmp.entryName[0]) || (strstr(tmp.entryName, "[heap]")) || (strstr(tmp.entryName, "[stack]")))) {
									if ('\0' == tmp.entryName[0]) {
										strcpy(tmp.entryName, "[anon]");
									}
									addMMapEntry(tmp, &mmapAnon, &mmapAnonTail);
								}
								lines_To_skip = skipToRollover;
								skipped = 1;
								expect_entry = 1;
								expect_swap_pss = 0;
								fprintf(fpMmap, "%lx-%lx %u %u %u %s %s\n", tmp.startAddress, tmp.endAddress, tmp.size, tmp.rss, tmp.swapPss, tmp.perm, tmp.entryName);
								memset(&tmp, 0, sizeof(MMAP_info));
							}
							else {
								dbg(PRINT_ERROR,"Error Reading Swap Pss from %s", mmapTmpArray);
							}
						}
						else {
							PRINT("%s:%d: Shouldn't get here..read line %s\n", __FUNCTION__, __LINE__, mmapTmpArray);
						}
					}
					else {
						dbg(PRINT_NOISE, "Skipping, skipped vs lines_To_skip %u:%u, line %s", skipped, lines_To_skip, mmapTmpArray);
					}
				}
				else { // Learn here
					if (!skipToEntry) {
						if (4 <= sscanf(mmapTmpArray, "%lx-%lx %s %x %*s %*u %s", &tmp.startAddress, &tmp.endAddress, tmp.perm, &offset, tmp.entryName)) {
							tmp.startAddress -= offset;
							tmp.size = tmp.endAddress - tmp.startAddress;
							skipToEntry = skippedToLearn + 1;
							skippedToLearn = 0;
							dbg(PRINT_NOISE,"Read Entry %lx-%lx %s %s after skipping %u lines - %s", 
									tmp.startAddress, tmp.endAddress, tmp.entryName, tmp.perm, skipToEntry, mmapTmpArray);
						}
						else {
							skippedToLearn++;
							dbg(PRINT_INFO, "skippedForEntry: %u, %s\n", skippedToLearn, mmapTmpArray);
						}
					}
					else if (!skipToSize) {
						if (sscanf(mmapTmpArray, "Size: %u kB", &tmp.size)) {
							skipToSize = skippedToLearn + 1;
							skippedToLearn = 0;
							dbg(PRINT_NOISE,"Read Size %u after skipping %u lines - %s", tmp.size, skipToSize, mmapTmpArray);
						}
						else {
							skippedToLearn++;
							dbg(PRINT_INFO, "skippedForSize: %u, %s\n", skippedToLearn, mmapTmpArray);
						}
					}
					else if (!skipToRss) {
						if (sscanf(mmapTmpArray, "Rss: %u kB", &tmp.rss)) {
							skipToRss = skippedToLearn + 1;
							skippedToLearn = 0;
							dbg(PRINT_NOISE,"Read Rss %u after skipping %u - %s", tmp.rss, skipToRss, mmapTmpArray);
						}
						else {
							skippedToLearn++;
							dbg(PRINT_INFO, "skippedForRss: %u, %s\n", skippedToLearn, mmapTmpArray);
						}
					}
					else if (!skipToSwapPss) {
						if (sscanf(mmapTmpArray, "SwapPss: %u kB", &tmp.swapPss)) {
							if (createAnon && ((strstr(tmp.entryName, "[heap]")) || ('\0' == tmp.entryName[0]) || (strstr(tmp.entryName, "[stack]")))) {
								if ('\0' == tmp.entryName[0]) { /* May not happen here.. */
									strcpy(tmp.entryName, "[anon]");
								}
								addMMapEntry(tmp, &mmapAnon, &mmapAnonTail);
							}
							skipToSwapPss = skippedToLearn + 1;
							skippedToLearn = 0;
							dbg(PRINT_NOISE,"Read Swap Pss %u after skipping %u - %s", tmp.swapPss, skipToSwapPss, mmapTmpArray);
							fprintf(fpMmap, "%lx-%lx %u %u %u %s %s\n", tmp.startAddress, tmp.endAddress, tmp.size, tmp.rss, tmp.swapPss, tmp.perm, tmp.entryName);
							memset(&tmp, 0, sizeof(MMAP_info));
						}
						else {
							skippedToLearn++;
							dbg(PRINT_INFO, "skippedForSwapPss: %u, %s\n", skippedToLearn, mmapTmpArray);
						}
					}
					else if (!skipToRollover) {
                                                if (4 <= sscanf(mmapTmpArray, "%lx-%lx %s %x %*s %*u %s", &tmp.startAddress, &tmp.endAddress, tmp.perm, &offset, tmp.entryName)) {
							tmp.startAddress -= offset;
                                                        tmp.size = tmp.endAddress - tmp.startAddress;
                                                        skipToRollover = skippedToLearn + 1;
							skippedToLearn = 0;
                                                        dbg(PRINT_NOISE, "Read Rollover %lx-%lx %s %s after skipping %u - %s", 
									tmp.startAddress, tmp.endAddress, tmp.entryName, tmp.perm, skipToRollover, mmapTmpArray);
							expect_size = 1;
							expect_entry = 0;
                                                }
                                                else {
                                                        skippedToLearn++;
							dbg(PRINT_INFO, "skippedForRollover: %u, %s\n", skippedToLearn, mmapTmpArray);
                                                }
                                        }
					else {
						PRINT("%s:%d: Shouldn't get here..read line %s\n", __FUNCTION__, __LINE__, mmapTmpArray);
					}
				}
			}
			fclose(smap);
		}
		else {
			PRINT("%s: Open failed, errno %d [%s]\n", mmapTmpArray, errno, strerror(errno));
			return 1;
		}
		fclose(fpMmap);
	}
	else {
		PRINT("%s: Open failed, errno %d [%s]\n", mmapTmpArray, errno, strerror(errno));
		return 1;
	}
	return 0;
}

struct listgrp;
typedef struct listgrp
{
	//unsigned int flags; /* First 2 bytes are magic number (for LSB/MSB), next 2 are real flag */
	//void *ptr; // Needed??
	unsigned int size;
	//unsigned int rss;
	void *ra;
	pid_t tid;
	time_t *seconds;
	unsigned numEntries;
	float stdev;
	struct listgrp *next;
} LISTgrp;

LISTgrp *grpdEntries, *grpdEntriesTail;
void addToGroup(unsigned flags, unsigned size, void *ra, time_t seconds)
{
	LISTgrp *listPtr = NULL;

	// See if we have an allocation before for the same ra/size
	if (grpdEntries) {
		listPtr = grpdEntries;
		while (listPtr) {
			if (listPtr->ra == ra && listPtr->size == size) {
				// Add to it
				break;
			}
			listPtr = listPtr->next;
		}
	}
	if (NULL == listPtr) {
		listPtr = (LISTgrp *)malloc(sizeof(LISTgrp));
		//listPtr->flags = 0xBEAD0000 | flags;
		listPtr->size = size;
		listPtr->ra = ra;
		listPtr->next = NULL;
		listPtr->seconds = NULL;
		listPtr->numEntries = 0;
		if (grpdEntriesTail)
		{
			grpdEntriesTail->next = listPtr;
			grpdEntriesTail = listPtr;
		}
		else
		{ // head should also be null
			grpdEntries = grpdEntriesTail = listPtr;
		}
	}
	listPtr->seconds = (time_t*)realloc(listPtr->seconds, (listPtr->numEntries + 1) * sizeof(time_t));
	listPtr->seconds[listPtr->numEntries++] = seconds;
}

#include <math.h>
void pruneAndUpdateGrouped()
{
	LISTgrp *listPtr = grpdEntries, *listPtrP, *listPtr_prev = NULL;
	while (listPtr) {
		unsigned i = 0;
		if (1 == listPtr->numEntries) {
			if (listPtr_prev) {
				listPtr_prev->next = listPtr->next;
			}
			else { //if (grpdEntries == listPtr) {
				grpdEntries = listPtr->next;
			}
	
			// Maynot maintain tail hereafterwards, as we are not going to insert anything new from hereon	
			// But still for this loop, let us have it. In the next loop where we sort, we are skipping it.	
			if (grpdEntriesTail == listPtr) {
				grpdEntriesTail = listPtr_prev;
				grpdEntriesTail->next = NULL;
			}
			listPtrP = listPtr;
			listPtr = listPtr->next;
			free(listPtrP->seconds);
			free(listPtrP);
			continue;
		}
		else {
			double sum = 0.0, average = 0.0, variance = 0.0;
			for (i=0; i<listPtr->numEntries; i++) {
				// To prevent overflow, in case
				sum += (listPtr->seconds[i] - listPtr->seconds[0] + 1);
			}
			average = sum / listPtr->numEntries;
			for (i=0; i<listPtr->numEntries; i++) {
				variance += pow((listPtr->seconds[i] - listPtr->seconds[0] + 1) - average, 2);
			}
			listPtr->stdev = sqrt(variance / listPtr->numEntries);
		}
		listPtr_prev = listPtr;
		listPtr = listPtr->next;
	}
	listPtr = grpdEntries;
	LISTgrp *target, *dest, *target_prev = NULL, *dest_prev = NULL;
	LISTgrp *listPtrP_prev = NULL;
	while (listPtr) {
		target = listPtr;
		dest = NULL;
		listPtrP = listPtr->next;
		listPtrP_prev = listPtr;
		while (listPtrP) {
			if (target->stdev < listPtrP->stdev) {
				if ((NULL == dest) || (dest->stdev < listPtrP->stdev)) {
					dest = listPtrP;
					dest_prev = listPtrP_prev;
				}
			}
			listPtrP_prev = listPtrP;
			listPtrP = listPtrP->next;
		}
		if (dest) {
			/*
			 * Boundary conditions:
			 * 1. - target - - dest -
			 * 2. target - dest - - -
			 * 3. - - target - - dest
			 * 4. - - target dest - -
			 * 5. target dest - - - -
			 * 6. - - - - target dest
			 */
			if (target_prev) {
				target_prev->next = dest;
			}
			else {
				grpdEntries = dest;
			}
			listPtrP = target->next;
			target->next = dest->next;
			if (target != dest_prev) {
				dest_prev->next = target;
				dest->next = listPtrP;
			}
			else {
				dest->next = target;
			}
			/*listPtrP = target->next;
			if (target != dest_prev)
				dest_prev->next = target;

			target->next = dest->next;
			if (target_prev)
				target_prev->next = dest;
			else 
				grpdEntries = dest;

			if (target != dest_prev)
				dest->next = listPtrP;
			*/
		}
		target_prev = listPtr;
		listPtr = listPtr->next;
	}
}

void *getOffsetMapped(void *ra, char **entry, MMAP_info *mmapanon)
{
	MMAP_info *tmpprn = mmapanon;
	
	while (tmpprn) {
		if ((tmpprn->startAddress <= (unsigned long)ra) && (tmpprn->endAddress >= (unsigned long)ra)) {
			*entry = tmpprn->entryName;
			if (isASLREnabled || strstr(tmpprn->entryName, ".so")) {
				dbg(PRINT_INFO, "\n%s: start %lx ra %p end %lx offset %p\n", 
						__FUNCTION__, tmpprn->startAddress, ra, tmpprn->endAddress, (void*) (ra - (void*)tmpprn->startAddress));
				return (void*) (ra - (void*)tmpprn->startAddress);
			}
			return ra;
		}
		tmpprn = tmpprn->next;
	}
	return NULL;
}

void displayGrouped()
{
	pruneAndUpdateGrouped();
	LISTgrp *listPtr = grpdEntries, *listPtrP;
	char *binaryMapped;
	readStoredSmaps(pid, MMAP_ALL);
	unsigned count = 1;
	PRINT("\nSorted in the order of probability of leak\n  SNo  RA                       Size            ProbableLeakScore");
	while (listPtr) {
		void *offsetRA = getOffsetMapped(listPtr->ra, &binaryMapped, mmapAll);
		PRINT("\n%5u  %018lx  %9u            (%f)\n       (%p - %s)\n       (Allocated %u times at):", 
				count++, (long unsigned int)listPtr->ra, listPtr->size, listPtr->stdev, offsetRA, 
				(offsetRA)?binaryMapped:"Not Found", listPtr->numEntries);

		unsigned i = 0;
		while (i < listPtr->numEntries) {
			/* Not worrying about thread safe, since I'm the sole user */
			struct tm *tmNow = localtime(&(listPtr->seconds[i++]));
			char timef[32]; // = {'\0'};
			if (0 != strftime(timef, sizeof(timef), "%Y_%m_%d_%H_%M_%S", tmNow)) {
			/* may not fail..but still */
				timef[19] = '\0';
			}
			else {
				sprintf(timef, "%lu", listPtr->seconds[i - 1]);
			}
			PRINT("%s%s ", (1==i)?"":", ",timef);
		}
		listPtrP = listPtr;
		listPtr = listPtr->next;
		free(listPtrP->seconds);
		free(listPtrP);
	}
	PRINT("\n\n");
	grpdEntries = grpdEntriesTail = NULL;
	if (interactive) sleep(2);
}

/**
 * @brief Processes heapwalk data.
 *
 * This function processes heapwalk data and prints the results, including allocations and memory mapping.
 *
 * @param cmd The command indicating the type of heapwalk operation.
 * @param pid The process ID of the target process.
 * @param tid The thread ID to be used.
 * @param isSelfTest Flag indicating whether this is a self-test operation.
 * @param resp Pointer to the response list.
 * @param listIndex Pointer to the list index.
 * @param mmapIn Pointer to the input memory mapping.
 */
void processHeapwalk(int cmd, int pid, int tid, bool isSelfTest, LIST *resp, int *listIndex, MMAP_info *mmapIn, bool analyze)
{
	msg_resp msgresp;
	int msgsize = sizeof(msg_resp);
	char heapwalkFile[32];

	dbg(PRINT_NOISE, "%s: cmd %d pid %d tid %d analyze %d\n", __FUNCTION__, cmd, pid, tid, analyze);
	if (HEAPWALK_MMAP_ENTRIES == cmd)
	{
                if (NULL == mmapAnon) {
                        /* Looks like pmap entries couldn't be processed!! */
                        PRINT("%s: heap/anon entries couldn't be read from map\n", __FUNCTION__);
			return; // No point in continuing
                }
                else {
                        MMAP_info *tmpprn = mmapAnon;
                        while (tmpprn)
                        {
                                // PRINT("%s: %lx %u %u %s\n", tmpprn->entryName, tmpprn->startAddress, tmpprn->size, tmpprn->rss, tmpprn->perm);
                                //tmpprn->endAddress = tmpprn->startAddress + (tmpprn->size * 1024);
                                /* Fill it's heatmap start/end */
                                unsigned long long increment = (tmpprn->size * 1024) / MAX_HEAT_MAP;
                                unsigned long long startAddress = tmpprn->startAddress;
                                for (int i = 0; i < MAX_HEAT_MAP; i++)
                                {
                                        tmpprn->heatmap[i].startAddress = startAddress;
                                        startAddress += increment;
                                        tmpprn->heatmap[i].endAddress = startAddress;
                                        // PRINT("0x%lx:0x%lx %llu\n", tmpprn->heatmap[i].startAddress, tmpprn->heatmap[i].endAddress, increment);
                                }
                                if (tmpprn->endAddress != tmpprn->heatmap[MAX_HEAT_MAP - 1].endAddress)
                                {
                                        dbg(PRINT_ERROR, "Something went wrong in heatmap distribution??0x%lx:0x%lx\n",
                                                tmpprn->endAddress, tmpprn->heatmap[MAX_HEAT_MAP - 1].endAddress);
                                }
                                tmpprn = tmpprn->next;
                        }
                }
                mmapIn = mmapAnon;
		/* Recursive call to complete HeapWalkAll processing */
		processHeapwalk(HEAPWALK_FULL, pid, tid, isSelfTest, resp, listIndex, mmapIn, analyze);


		if (!offlineAnalysis || OFFLINE_PROCESS == offlineAnalysis) {	
		/* Print results */
		MMAP_info *tmpprn = mmapAnon;
		if ('\0' != storedTime[0])
		{
			PRINT("\tGenerated Time: %s\n", storedTime);
		}
		unsigned long anonRSSTotal = 0, heapTotal = 0; 

		PRINT("\n\tmmapStart-mmapEnd                      Name(Perm)    Size-Kb     RSS-Kb    Heap'd(bytes)   Heap'd(%s)vsRSS\n\n", "%");
		
		while (tmpprn)
		{
			if (tmpprn->rss)
			{
				float percent = ((float)tmpprn->heapEntries / (float)(tmpprn->rss * 1024)) * 100;
				if (tmpprn->heapEntries)
				{
					PRINT("\t%016lx-%016lx %9s(%s) %10u %10u %10llu(%llu)             %.2f\n",
						  tmpprn->startAddress, tmpprn->endAddress, tmpprn->entryName, tmpprn->perm, tmpprn->size, tmpprn->rss,
						  tmpprn->heapEntries/1024, tmpprn->heapEntries, percent);
					PRINT("\tHeap Allocations in bytes over %u divisions\n\t", MAX_HEAT_MAP);
					for (int i = 0; i < MAX_HEAT_MAP; i++)
					{
						PRINT("[0x%04llx]", tmpprn->heatmap[i].heapEntries);
					}
					PRINT("\n");
				}
				else
				{
					PRINT("\t%016lx-%016lx %9s(%s) %10u %10u\n",
						  tmpprn->startAddress, tmpprn->endAddress, tmpprn->entryName, tmpprn->perm, tmpprn->size, tmpprn->rss);
				}
							//tmpanon->pthreadinfo.size = msgresp.xfer[msgIndex].size;
                                                        //tmpanon->pthreadinfo.stack_rss = rssvalue;
                                                        //tmpanon->pthreadinfo.stack_swap = swapvalue;
                                                        //tmpanon->pthreadinfo.start_routine = msgresp.xfer[msgIndex].start_routine;
				
				if (tmpprn->pthreadinfo.size) {
					char *binaryMapped = "";
					if (NULL == mmapAll) {
						readStoredSmaps(pid, MMAP_ALL);
					}
					void *offsetRA = getOffsetMapped(tmpprn->pthreadinfo.start_routine, &binaryMapped, mmapAll);
					
					PRINT("\tThread: (%p: %s), Size: %lu Rss: %lu Swap: %lu\n\n", (offsetRA)?offsetRA:tmpprn->pthreadinfo.start_routine, binaryMapped, 
							tmpprn->pthreadinfo.size, tmpprn->pthreadinfo.stack_rss, tmpprn->pthreadinfo.stack_swap);
				}
				else {
					PRINT("\n");
				}
			}
			else
			{
				if (tmpprn->heapEntries)
				{
					PRINT("\t%016lx-%016lx %9s(%s) %10u %10u %10llu ????\n",
						  tmpprn->startAddress, tmpprn->endAddress, tmpprn->entryName, tmpprn->perm, tmpprn->size, tmpprn->rss,
						  tmpprn->heapEntries);
				}
				else if (tmpprn->startAddress)
				{
					PRINT("\t%016lx-%016lx %9s(%s) %10u %10u\n",
						  tmpprn->startAddress, tmpprn->endAddress, tmpprn->entryName, tmpprn->perm, tmpprn->size, tmpprn->rss);
				}
			}
			if (!strstr(tmpprn->entryName, "stack")) {
				anonRSSTotal += tmpprn->rss;
			}
			heapTotal += tmpprn->heapEntries;
			tmpprn = tmpprn->next;
		}

	        PRINT("\nTOTAL HEAP-----------------------------: %lu KB by Physical pages, %lu KB by heap entries size, Swap %lu KB\n", totalrsspages*4, heapTotal/1024, totalswappages*4);
		if (baseStack || baseStackSwap) {
			PRINT("TOTAL STACK----------------------------: %lu KB, Swap %lu KB\n", baseStack, baseStackSwap);
		}
		if (pthreadStack_tstack || pthreadStackSwap_tstack) {
			PRINT("TOTAL STACK of pthreads----------------: %lu KB, Swap %lu KB\n", pthreadStack_tstack, pthreadStackSwap_tstack);
		}
		if (pthreadStack_etstack || pthreadStackSwap_etstack) {
			PRINT("TOTAL STACK of exited pthreads---------: %lu KB, Swap %lu KB\n", pthreadStack_etstack, pthreadStackSwap_etstack);
		}
		        PRINT("Heap+stack utilization against mmap'd--: %f %s\n", 
				anonRSSTotal?(((double)(totalrsspages*4) + baseStack + pthreadStack_tstack + pthreadStack_etstack) / (double)anonRSSTotal)*100:0, "%");
		if (stackInactive) {
			PRINT("TOTAL STACK not mapped-----------------: %lu Bytes\n", stackInactive);
		}

		PRINT("\n");
		}
	}
	else
	{
		sprintf(heapwalkFile, "%s/hp%s_%d%s.dat", rwPath, (HEAPWALK_FULL == cmd)?"f":"", pid, fileSuffix?fileSuffix:"");

		unsigned totalMsgs = 0;
		FILE *fpHWalk = fopen(heapwalkFile, "rb");
		if (NULL == fpHWalk)
		{
			dbg(PRINT_MUST, "%s open error, %s\n", heapwalkFile, strerror(errno));
		}
		else
		{
			/* to be tested and added later
			if (analyze) {
				// check version
				hp_walk_header hpwHdr = {MEMWRAP_MSG_RESP_VERSION,0};

				if (fread(&hpwHdr, 1, sizeof(hpwHdr), fpHWalk)) {
					if (MEMWRAP_MSG_RESP_VERSION != hpwHdr.version) {
						PRINT("Version mismatch, heapwalk file %u vs uty %u\n", hpwHdr.version, MEMWRAP_MSG_RESP_VERSION);
					}
					PRINT("%s: Total entries: %lu\n", heapwalkFile, hpwHdr.totalEntries);
				}
			}
			*/
			unsigned msgSeq = 0;
			unsigned msgIndex;
			unsigned long long threadAllocationOnly = 0;
			do
			{
				msgsize = fread(&msgresp, 1, sizeof(msgresp), fpHWalk);
				if (msgsize)
				{
					if (msgresp.numItemOrInfo)
					{
						msgIndex = 0;
						if (!msgSeq)
						{
							if (!isSelfTest && (NULL == mmapIn))
							{
								dbg(PRINT_WALK, "\n%s\n", (HEAPWALK_FULL == cmd) ? "Already walked:" : "New Allocations:");
								//"%5u %18p %9lu %9u(%4s) %18p %8u %s%s\n"
								dbg(PRINT_WALK, "  SNo  Pointer                  Size            Usage           RA         ThreadID  AllocationTime\n");
							}
						}
						int msgCount = msgresp.numItemOrInfo & 0xFFFFFFF;
						totalMsgs += msgCount;
						while (msgIndex < msgCount)
						{
							if (isSelfTest)
							{
								resp[*listIndex].ptr = msgresp.xfer[msgIndex].ptr;
								resp[*listIndex].size = msgresp.xfer[msgIndex].size;
								*listIndex = *listIndex + 1;
							}
							else
							{
									unsigned rssvalue = 0;
									unsigned swapvalue = 0;
									// This might be the good place to determine if the allocation has associated physical page
									LIST_pagemap *pagetmp = pagemapHead;
									void *ptr = msgresp.xfer[msgIndex].ptr;
									//void *ptrPagemap = msgresp.xfer[msgIndex].ptr & 0xFFFFFFFFFFFFF000;  // sysconf(_SC_PAGE_SIZE) = 4096
									void *ptrPagemap = (void*)((unsigned long)msgresp.xfer[msgIndex].ptr & ~(PAGE_SIZE - 1));
									unsigned size = msgresp.xfer[msgIndex].size;
									// TODO optimize
									while (pagetmp) {
										if (ptrPagemap == pagetmp->pageaddress) {
											unsigned sizeinpage = (pagetmp->pageaddress + PAGE_SIZE) - ptr;
											dbg(PRINT_INFO, "Found pagemap for %p (%p), pageaddress %p, size %u, sizeinpage %u\n",
													ptr, ptrPagemap, pagetmp->pageaddress, size, sizeinpage);
											if (pagetmp->pagestat) {
												if (PAGE_ACCOUNTED != pagetmp->pagestat) {
													(PAGE_PRESENT & pagetmp->pagestat)? totalrsspages++ : totalswappages++;
													pagetmp->pagestat = PAGE_ACCOUNTED;
												}
												else {
													dbg(PRINT_NOISE, "Page %p accounted already\n", pagetmp->pageaddress);
												}
											}
											if ((unsigned)sizeinpage >= size) {
												if (PAGE_PRESENT & pagetmp->pagestat) {
												       rssvalue += size;
												}
												else if (PAGE_SWAPPED & pagetmp->pagestat) {
													swapvalue += size;
												}
												break;
											}else {
												ptr += sizeinpage;
												size = size - sizeinpage;
												if (PAGE_PRESENT & pagetmp->pagestat) {
													rssvalue += sizeinpage;
												}
												else if (PAGE_SWAPPED & pagetmp->pagestat) {
													swapvalue += sizeinpage;
												}
												pagetmp = pagetmp->next;
												ptrPagemap += PAGE_SIZE;
												// Account this page for total
												continue;
											}
										}
										else if (ptrPagemap < pagetmp->pageaddress) {
											dbg(PRINT_ERROR, "Breaking, ptr %p < pagemap %p\n", ptr, pagetmp->pageaddress);
											pagetmp = NULL;
											break;
										}
										pagetmp = pagetmp->next;
									}
									if (NULL == pagetmp) {
										dbg(PRINT_ERROR, "******** ptr %p not found in pagemap\n", ptr);
									}


									if ((NULL == mmapIn) && ((tid) ? tid == msgresp.xfer[msgIndex].tid : 1))
									{
										/* Not worrying about thread safe, since I'm the sole user */
										struct tm *tmNow = localtime(&msgresp.xfer[msgIndex].seconds);
										char timef[32] = {'\0'};
										if (0 == strftime(timef, sizeof(timef), "%Y_%m_%d_%H_%M_%S", tmNow)) {
											/* may not fail..but still */
											sprintf(timef, "%lu", msgresp.xfer[msgIndex].seconds);
										}

#ifdef PREPEND_LISTDATA
										PRINT("%5u  %018lx  %9lu  %9u(%4s)  %18p  %8u  %s%s\n", 
												++msgSeq, (unsigned long)msgresp.xfer[msgIndex].ptr, msgresp.xfer[msgIndex].size, 
												rssvalue|swapvalue, (rssvalue)?"RSS":(swapvalue)?"Swap":"Nil", msgresp.xfer[msgIndex].ra,
											  	msgresp.xfer[msgIndex].tid, timef,
											  	(0 == (msgresp.xfer[msgIndex].flags & 0xFF02)) ? 
											  	"" : (msgresp.xfer[msgIndex].flags & FLAGS_BIT1_REALLOC) ? (" -Realloc") : (" -Memalign"));
#else
										PRINT("%u %p %u %p %u %s\n", ++msgSeq, msgresp.xfer[msgIndex].ptr, msgresp.xfer[msgIndex].size, 
												msgresp.xfer[msgIndex].ra, msgresp.xfer[msgIndex].tid, timef);
#endif
										threadAllocationOnly += msgresp.xfer[msgIndex].size;
										addThreadStatEntry(msgresp.xfer[msgIndex].tid, msgresp.xfer[msgIndex].size);
									}
									// Reuse size filed for filling in heat map for anon entries
									// ******* If vm size as well as RSS needs to be shown in mmap analysis, then use another variable to store rss+swap
									msgresp.xfer[msgIndex].size = rssvalue + swapvalue;
									if (HEAPWALK_LEAKCHECK == baseCmd) {
										addToGroup(msgresp.xfer[msgIndex].flags, msgresp.xfer[msgIndex].size, msgresp.xfer[msgIndex].ra, msgresp.xfer[msgIndex].seconds);
									}

									// TODO: Add here for grouping, update mmap entry, consider memalign'd overhead using flags
									MMAP_info *tmpprn = mmapIn;
									while (tmpprn)
									{
										// PRINT("%s: %lx %u %u %s\n", tmpprn->entryName, tmpprn->startAddress, tmpprn->size, tmpprn->rss, tmpprn->perm);
										if ((tmpprn->startAddress <= (unsigned long)msgresp.xfer[msgIndex].ptr) &&
											((tmpprn->endAddress) >= (unsigned long)msgresp.xfer[msgIndex].ptr))
										{
											/* Get entry size including the book keeping!! */ // TODO Alignment ??
											unsigned size = msgresp.xfer[msgIndex].size + sizeof(LIST);
											tmpprn->heapEntries += size;
											// TODO optimize..
											int i;
											for (i = 0; i < MAX_HEAT_MAP; i++)
											{
												if ((unsigned long)msgresp.xfer[msgIndex].ptr <= tmpprn->heatmap[i].endAddress)
												{
													if ((unsigned long)((char *)msgresp.xfer[msgIndex].ptr + size) <=
														tmpprn->heatmap[i].endAddress)
													{
														tmpprn->heatmap[i].heapEntries += size;
													}
													else
													{
														unsigned long long partial = (unsigned long)tmpprn->heatmap[i].endAddress -
																					 (unsigned long)msgresp.xfer[msgIndex].ptr;
														tmpprn->heatmap[i].heapEntries += partial;
														tmpprn->heatmap[i + 1].heapEntries += ((unsigned long long)size - partial);
													}
													break;
												}
											}
											if (MAX_HEAT_MAP == i)
											{
												dbg(PRINT_ERROR, "Revisit heatmap start/end!!\n");
											}
											break;
										}
										tmpprn = tmpprn->next;
									}
									if (mmapIn && (NULL == tmpprn))
									{
										dbg(PRINT_MUST, "Error, entry unmapped? 0x%p:%lu\n", msgresp.xfer[msgIndex].ptr, msgresp.xfer[msgIndex].size);
									}
							}
							msgIndex++;
						} // while (msgIndex < msgCount)
					} // if (msgresp.numItemOrInfo)
				}
				else
				{
					if (isSelfTest)
					{
						resp[*listIndex].ptr = NULL;
						resp[*listIndex].size = 0;
					}
				}
			} while (msgsize);

			if (!isSelfTest && totalMsgs && (NULL == mmapIn))
			{
				if (tid && threadAllocationOnly)
				{
					PRINT("HeapSize for walked thread(%d): %llu Bytes\n\n", tid, threadAllocationOnly);
				}
				printAndFreeThreadStat();

				if ((HEAPWALK_INCREMENT == cmd)) {
					PRINT("\nTotalHeapSize                 :%6lu KB (%lu bytes)\n(excludes tool overhead)\n",
							msgresp.totalHeapSize/1024, msgresp.totalHeapSize);
					PRINT("RSS                           :%6lu KB (%lu bytes)\n",
							totalrsspages*(PAGE_SIZE/1024), totalrsspages*PAGE_SIZE);
					PRINT("Swap                          :%6lu KB (%lu bytes)\n",
							totalswappages*(PAGE_SIZE/1024), totalswappages*PAGE_SIZE);
					PRINT("PeakTotalHeapSize             :%6lu KB (%lu bytes)\n", msgresp.heapPeakSize/1024, msgresp.heapPeakSize);
					PRINT("  at %sTool Overhead                 :%6lu KB (%lu bytes)\n\n",
							ctime(&msgresp.heapPeakedAt), msgresp.totalOverhead/1024, msgresp.totalOverhead);
				}
				else {
					PRINT("\n");
				}
				if (interactive) sleep(2);
				// dbg(PRINT_MUST, "Received Msgs %u sequence %u\n", totalMsgs, msgSeq);
			}
			else
			{
				if (!isSelfTest && (NULL == mmapIn))
				{
					dbg(PRINT_MUST, "%s\n", (HEAPWALK_FULL == cmd) ? "Already walked: None" : "No New Allocations");
					if (HEAPWALK_INCREMENT == cmd) {
						PRINT("\n");
						if (interactive) sleep(2);
					}
				}
			}
			fclose(fpHWalk);
		}

		if (HEAPWALK_FULL == cmd)
		{
			if (!totalMsgs) {
				baseCmd = HEAPWALK_INCREMENT;
			}

			processHeapwalk(HEAPWALK_INCREMENT, pid, tid, isSelfTest, resp, listIndex, mmapIn, analyze);

			if (HEAPWALK_LEAKCHECK == baseCmd) {
				displayGrouped();
			}
		}

	}
}
#endif

void performOfflineAnalysis(int pid) //, char *fileSuffix)
{
	if (!readStoredSmaps(pid, MMAP_ANON)) {
		if (readStoredPagemap(pid)) {
			dbg(PRINT_MUST, "Error reading Pagemap...physical size may not be available\n");
			sleep(3);	
		}
		mappthreadStack(pid);
		totalrsspages = totalswappages = 0;
		baseCmd = cmd|HEAPWALK_BASE;
		if (HEAPWALK_LEAKCHECK == baseCmd) {
			processHeapwalk(HEAPWALK_FULL | HEAPWALK_BASE, pid, tid, 0, NULL, NULL, NULL, 1);
		}
		else {
			processHeapwalk(cmd | HEAPWALK_BASE, pid, tid, 0, NULL, NULL, NULL, 1);
		}

		freeMMapList();
		freePagemapDataStruct();
		pid = tid = cmd = -1;
		interactive = 0;
	}
}

void printHelp(char *argv)
{
	PRINT("\nUsage:\n\tmemleakutil - Args for live debugging\n");
	PRINT("\t       -p,  --pid	<pid>\n");
	PRINT("\t       [-t,  --tid <threadid>, default 0 (all threads), optional parameter]\n");
	PRINT("\t       -c,  --cmd <1/2/3/4/5>\n");
	PRINT("\t                   1 - Heapwalk New entries that were not walked earlier\n");
	PRINT("\t                   2 - Heapwalk all\n");
	PRINT("\t                   3 - Group Probable leaks (experimental)\n");
	PRINT("\t                   4 - mmap details of anon, stack, heap, thread stack if available\n");
	PRINT("\t                   5 - Mark all entries of heap as walked\n");
	PRINT("\t                   6 - Mark all entries of heap as unwalked\n");
	PRINT("\t                   7 - Call malloc stats\n");

	PRINT("\n\tmemleakutil - Args to save data for Offline analysis\n");
	PRINT("\t       -o, --offline\n");
	PRINT("\t       -p, --pid <pid>\n");
	PRINT("\t       [-d, --dir <dir>, default /tmp, optional parameter]\n");
	PRINT("\t       [-s, --suffix <suffix>, example first/2nd/entry/..]\n");

	PRINT("\n\tmemleakutil - Args to analyze saved Offline reports\n");
	PRINT("\t       -a, --analyze <pid>\n");
	PRINT("\t       [-d, --dir <dir>, default /tmp, optional parameter]\n");
	PRINT("\t       [-s, --suffix <suffix>, example first/2nd/entry/..]\n");

	PRINT("\n\tmemleakutil -- Interactive mode\n");
	PRINT("\t       -i, --interactive\n");
	PRINT("\t       [-d, --dir <dir>, default /tmp, optional parameter]\n");

	PRINT("\n\tmemleakutil -- Selftest, available when compiled with SELF_TEST flag\n");
	PRINT("\t       --selftest\n");

	PRINT("\n\tmemleakutil -- Test run in interactive mode by giving pid as 0, available when compiled SELF_TEST flag\n");
	PRINT("\t       --testrun\n");
        exit(1);
}

void printHelpE(int argc, char *argv[])
{
	PRINT("Error in argument, usage:\n");
	printHelp(argv[0]);
}

void processArgs(int argc, char *argv[])
{
	for (int i=1; i < argc; i++) {
		if (!strcmp(argv[i], "--output") || !strcmp(argv[i], "-o")) {
			offlineAnalysis = OFFLINE_STORE;
			continue;
		}
		if (!strcmp(argv[i], "--interactive") || !strcmp(argv[i], "-i")) {
			interactive = 1;
			offlineAnalysis = pid = tid = cmd = 0;
			break;
		}
		if (!strcmp(argv[i], "--pid") || !strcmp(argv[i], "-p")) {
			if (i+1 < argc) {
				i++;
				pid = atoi(argv[i]);
				continue;
			}
			printHelpE(argc, argv);
		}
		if (!strcmp(argv[i], "--tid") || !strcmp(argv[i], "-t")) {
			if (i+1 < argc) {
				i++;
				tid = atoi(argv[i]);
				continue;
			}
			printHelpE(argc, argv);
		}
		if (!strcmp(argv[i], "--cmd") || !strcmp(argv[i], "-c")) {
			if (i+1 < argc) {
				i++;
				cmd = atoi(argv[i]);
				continue;
			}
			printHelpE(argc, argv);
		}
		if (!strcmp(argv[i], "--dir") || !strcmp(argv[i], "-d")) {
			if (i+1 < argc) {
				i++;
				DIR *outputDir = opendir(argv[i]);
				if (outputDir) {
					closedir(outputDir);
					rwPath = argv[i];
					continue;
				}
				PRINT("Dir %s access error %d [%s]\n", argv[i], errno, strerror(errno));
			}
			printHelpE(argc, argv);
		}
		if (!strcmp(argv[i], "--analyze") || !strcmp(argv[i], "-a")) {
			if (i+1 < argc) {
				i++;
				offlineAnalysis = OFFLINE_PROCESS;
				pid = atoi(argv[i]);
				continue;
			}
			printHelpE(argc, argv);
		}
		if (!strcmp(argv[i], "--suffix") || !strcmp(argv[i], "-s")) {
			if (i+1 < argc) {
				i++;
				fileSuffix = malloc(strlen(argv[i]) + 2);
				snprintf(fileSuffix, strlen(argv[i]) + 2, "_%s", argv[i]);
				dbg(PRINT_INFO, "Offline analysis suffix %s\n", &fileSuffix[1]);
				continue;
			}
			printHelpE(argc, argv);
		}
		if (!strcmp(argv[i], "--selftest"))
		{
#ifdef SELF_TEST
			FILE *fp = fopen("/tmp/memleakutil_selftest.txt", "w");
			if (NULL != fp) {
                		fclose(fp);
        		}
			rwPath = "/tmp/";
			selftest();
#else
			dbg(PRINT_MUST, "ERROR...Build with SELF_TEST compiler directive to run selftest\n");
#endif
			exit(1);;
		}
		if (!strcmp(argv[i], "--testrun"))
		{
		
#ifdef SELF_TEST
			rwPath = "/tmp/";
			interactive = 1;
			spawntestrunthread();
#else
			dbg(PRINT_MUST, "Build with SELF_TEST compiler directive to do testrun\n");
#endif
			return;
		}
		if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) 
		{
			printHelp(argv[0]);
		}
	}
	if (interactive) {
		if (offlineAnalysis || pid || cmd || tid) {
			printHelpE(argc, argv);
		}
	}
	else {
		if (!offlineAnalysis) {
			if (!pid) {
				printHelpE(argc, argv);
			}
			else if (!cmd) {
				printHelpE(argc, argv);
			}
		}
		else {
			if (!pid) {
				printHelpE(argc, argv);
			}
			if (OFFLINE_STORE == offlineAnalysis && cmd) {
				PRINT("Ignoring cmd for offline report save...\n");
				cmd = 3;
			}
		}
	}
}

int main(int argc, char *argv[])
{
	/* mqrecv for mq_util, mqsend for sending to mq_wrapper_<pid> */
	mqd_t mqrecv, mqsend = -1;
	msg_cmd msgcmd;
	PRINT("memleakutil %s\n", versionString);
#ifdef OPTIMIZE_MQ_TRANSFER_FOR_CMD
	char cOPTIMIZE_MQ_TRANSFER_FOR_CMD = 'Y';
#else
	char cOPTIMIZE_MQ_TRANSFER_FOR_CMD = 'N';
#endif

#ifdef PREPEND_LISTDATA_FOR_CMD
	char cPREPEND_LISTDATA_FOR_CMD = 'Y';
#else
	char cPREPEND_LISTDATA_FOR_CMD = 'N';
#endif

#ifdef MAINTAIN_SINGLE_LIST_FOR_CMD
	char cMAINTAIN_SINGLE_LIST_FOR_CMD = 'Y';
#else
	char cMAINTAIN_SINGLE_LIST_FOR_CMD = 'N';
#endif

	PRINT("Build Options:\nMEMWRAP_COMMANDS_VERSION=%d\nOPTIMIZE_MQ_TRANSFER_FOR_CMD=%c\nPREPEND_LISTDATA_FOR_CMD=%c\nMAINTAIN_SINGLE_LIST_FOR_CMD=%c\n",
		   MEMWRAP_COMMANDS_VERSION, cOPTIMIZE_MQ_TRANSFER_FOR_CMD, cPREPEND_LISTDATA_FOR_CMD, cMAINTAIN_SINGLE_LIST_FOR_CMD);
	PRINT("Minimum Overhead for each allocation %lu bytes\n\n", sizeof(LIST));
	PRINT("msg_resp size %lu bytes\n", sizeof(msg_resp));
#if defined(DEBUG_RUNTIME)
	char *dbg_level = getenv("DEBUG_ENV_LEVEL");
	if (dbg_level)
	{
		sscanf(dbg_level, "%d", &debug_level);
	}
	PRINT("DEBUG_RUNTIME enabled. Debug level %d\n", debug_level);
#endif
	dbg(PRINT_MUST, "PRINT_MUST (=%d) will be printed\n", PRINT_MUST);
	dbg(PRINT_WALK, "PRINT_WALK (=%d) will be printed\n", PRINT_WALK);
	dbg(PRINT_FATAL, "PRINT_FATAL (=%d)  will be printed\n", PRINT_FATAL);
	dbg(PRINT_ERROR, "PRINT_ERROR (=%d) will be printed\n", PRINT_ERROR);
	dbg(PRINT_SEM, "PRINT_SEM (=%d) will be printed\n", PRINT_SEM);
	dbg(PRINT_MSGQ, "PRINT_MSGQ (=%d) will be printed\n", PRINT_MSGQ);
	dbg(PRINT_LIST, "PRINT_LIST (=%d) will be printed\n", PRINT_LIST);
	dbg(PRINT_INFO, "PRINT_INFO (=%d) will be printed\n", PRINT_INFO);
	dbg(PRINT_NOISE, "PRINT_NOISE (=%d) will be printed\n", PRINT_NOISE);
	
	PAGE_SIZE = sysconf(_SC_PAGE_SIZE);
	if (0 >= PAGE_SIZE) {
		PRINT("sysconf(_SC_PAGE_SIZE) returned [%s], setting to 4096\n", strerror(errno));
		PAGE_SIZE = 4096;
	}
	processArgs(argc, argv);

	if (NULL == rwPath) {
		rwPath = "/tmp/";
	}
	if (OFFLINE_PROCESS == offlineAnalysis) {
		performOfflineAnalysis(pid);
		exit(0);
	}
	//sleep(300);
#if defined(STANDALONE_TESTRUN)
	pause();
#endif
	mqrecv = createMq();

	while (1)
	{
		char mq_name[64];
		if (!interactive) {
			msgcmd.pid = pid;
			msgcmd.cmd = cmd;
		}
		else {
			msgcmd.pid = -1;
			PRINT("\nEnter Process PID to send to %s: ", "(-1 to exit)");
			if (!scanf("%d", &msgcmd.pid)) continue; 
		}
		if (-1 == msgcmd.pid)
			break;
		if (0 == msgcmd.pid)
			msgcmd.pid = getpid();

		sprintf(mq_name, "/mq_wrapper_%d", msgcmd.pid);
		mqsend = mq_open(mq_name, O_WRONLY);
		if (mqsend < 0)
		{
			dbg(PRINT_MUST, "Error, cannot open the queue: %s, error: %s.\n", mq_name, strerror(errno));
			if (interactive) 
				continue;
			else
				break;
		}
		while (0 < mqsend) {
			if (interactive) {
				PRINT("1. Heapwalk New allocations\n   %s\n", "-Shows newly allocated and not free'd entries after previous Heapwalk");
				PRINT("2. Heapwalk all allocations\n   %s\n", "-Shows all entries");
				PRINT("3. Grouped Probable Leaks\n   %s\n", "-Groups the allocations and sorts probable leaking entries in descending order(experimental)");
				PRINT("4. Map heap vs mmap entries\n  %s\n", "-Prints anon distribution of entries and % mapping of heap. Available with OPTIMIZE_MQ_TRANSFER");
				PRINT("5. Mark all allocations as walked\n   %s\n", "-Doesn't show any entries, but marks all as walked");
				PRINT("6. Unmark walked allocations\n   %s\n", "-Doesn't show any entries, but subsequent walk shows all entries");
				PRINT("7. Call malloc_stats\n   %s\n", "-Calls malloc_stats API that prints the details in stderr");
				PRINT("0. Return\n   %s\n", "-Return to explore different Process");
				PRINT("Enter cmd to send: ");
				if(!scanf("%d", &msgcmd.cmd)) continue;
			}
			msgcmd.cmd |= HEAPWALK_BASE;
#ifdef OPTIMIZE_MQ_TRANSFER
			baseCmd = 0;
#endif

			switch (msgcmd.cmd)
			{
			case HEAPWALK_MMAP_ENTRIES:
#ifndef OPTIMIZE_MQ_TRANSFER
				PRINT("Cmd supported only with OPTIMIZE_MQ_TRANSFER, continuing..\n");
				break;
#else
				baseCmd = HEAPWALK_BASE;
#endif
				// break intentionally left
			case HEAPWALK_INCREMENT:
#ifdef OPTIMIZE_MQ_TRANSFER
				baseCmd = HEAPWALK_INCREMENT;
#endif
				// break intentionally left
			case HEAPWALK_LEAKCHECK:
				if (!baseCmd) {
					baseCmd = HEAPWALK_LEAKCHECK;
					msgcmd.cmd = HEAPWALK_FULL;
					pid = msgcmd.pid;
				}
			case HEAPWALK_FULL:
			{
#ifdef OPTIMIZE_MQ_TRANSFER
				if (!baseCmd) {
					baseCmd = HEAPWALK_FULL;
				}
#endif
				int threadid = 0;
				if (interactive && HEAPWALK_MMAP_ENTRIES != msgcmd.cmd && HEAPWALK_LEAKCHECK != baseCmd)
				{
					// TODO Optimize to get only entries for this thread
					PRINT("Enter threadid (0 for all):");
					if(!scanf("%d", &threadid)) continue;
					if (threadid) {
						PRINT("Walking only for thread %d\n", threadid);
					}
				}
				else {
					threadid = tid;
				}
				dbg(PRINT_MSGQ, "%s: sending cmd %d on mq %s\n", __FUNCTION__, msgcmd.cmd, mq_name);
				if (-1 != mq_send(mqsend, (const char *)&msgcmd, sizeof(msg_cmd), 0))
				{
#ifdef OPTIMIZE_MQ_TRANSFER
					if (!storeHeapwalk(mqrecv, msgcmd.cmd, msgcmd.pid, 0)) {
						if (HEAPWALK_MMAP_ENTRIES == msgcmd.cmd) {
							getStackIntercepts(mqrecv, msgcmd.pid);
						}
						readAndStoreSmaps(msgcmd.pid, 1);
						storeAnonHeapStackPagemap(msgcmd.pid);
						mappthreadStack(msgcmd.pid);
						totalrsspages = totalswappages = 0;
						processHeapwalk(msgcmd.cmd, msgcmd.pid, threadid, 0, NULL, NULL, NULL, 0);
						freeMMapList();
						freePagemapDataStruct();
					} else {
						dbg(PRINT_ERROR, "storeHeapwalk failed\n");
					}
#else
					msg_resp msgresp;
					unsigned int prio;
					struct timespec tm;
					int msgsize = sizeof(msg_resp);
					while (0 != msgsize)
					{
						clock_gettime(CLOCK_REALTIME, &tm);
						tm.tv_sec += 10;
						msgsize = mq_timedreceive(mqrecv, (char *)&msgresp, sizeof(msg_resp), &prio, &tm);
						if (-1 == msgsize) {
							if (ETIMEDOUT == errno) {
								dbg(PRINT_MUST, "%s:%d: Giving up..waited for 10 secs\n", __FUNCTION__, __LINE__);
							}else {
								dbg(PRINT_MUST, "%s:%d: mq_timedreceive failed [%s]\n", __FUNCTION__, __LINE__, strerror(errno));
							}
							break;
						}
						if (0 < msgsize && (-1 == msgresp.seq))
						{
							dbg(PRINT_MUST, "End of List\n");
#if defined(PREPEND_LISTDATA) && defined(ENABLE_STATISTICS)
							dbg(PRINT_MUST, "%s\n", msgresp.msg);
#endif
							break;
						}
						if (!strcmp(msgresp.msg, "No new allocations") ||
							!strcmp(msgresp.msg, "Already walked:") ||
							!strcmp(msgresp.msg, "New allocations:"))
						{
							dbg(PRINT_WALK, "%s\n", msgresp.msg);
							continue;
						}
						else if (1 == msgresp.seq)
						{
							dbg(PRINT_WALK, "Pointer Size RA ThreadID AllocationTime\n");
						}
						dbg(PRINT_WALK, "%d) %s\n", msgresp.seq, msgresp.msg);
					}
#endif
				}
				else
				{
					dbg(PRINT_ERROR, "msgsnd failed, %s\n", strerror(errno));
				}
			}
			break;

			case HEAPWALK_MARKALL:
				dbg(PRINT_MSGQ, "%s: sending cmd %d on mq %s\n", __FUNCTION__, msgcmd.cmd, mq_name);
				if (-1 == mq_send(mqsend, (const char *)&msgcmd, sizeof(msg_cmd), 0))
				{
					dbg(PRINT_ERROR, "msgsnd failed, %s\n", strerror(errno));
				}
				else
				{
					dbg(PRINT_MUST, "Marked. heapwalk will list new allocations from now on\n");
				}
				if (interactive) sleep(3);
				break;

			case HEAPWALK_RESET_MARKED:
				dbg(PRINT_MSGQ, "%s: sending cmd %d on mq %s\n", __FUNCTION__, msgcmd.cmd, mq_name);
				if (-1 == mq_send(mqsend, (const char *)&msgcmd, sizeof(msg_cmd), 0))
				{
					dbg(PRINT_ERROR, "msgsnd failed, %s\n", strerror(errno));
				}
				else
				{
					dbg(PRINT_MUST, "Reset done. heapwalk will list all allocations\n");
				}
				if (interactive) sleep(3);
				break;

			case HEAPWALK_MALLOC_STATS:
				dbg(PRINT_MSGQ, "%s: sending cmd %d on mq %s\n", __FUNCTION__, msgcmd.cmd, mq_name);
				if (-1 == mq_send(mqsend, (const char *)&msgcmd, sizeof(msg_cmd), 0))
				{
					dbg(PRINT_ERROR, "msgsnd failed, %s\n", strerror(errno));
				}
				else
				{
					dbg(PRINT_MUST, "malloc_stats requested. By default malloc_stats prints in stderr\n");
				}
				if (interactive) sleep(3);
				break;

			case HEAPWALK_BASE:
				mq_close(mqsend);
				mqsend = -1;
				break;

			default:
				dbg(PRINT_ERROR, "Invalid cmd 0x%x...continuing\n", msgcmd.cmd);
				PRINT("This Utility Built with:\nMEMWRAP_COMMANDS_VERSION=%d\nOPTIMIZE_MQ_TRANSFER_FOR_CMD=%c\nPREPEND_LISTDATA_FOR_CMD=%c\nMAINTAIN_SINGLE_LIST_FOR_CMD=%c\n\n",
					   MEMWRAP_COMMANDS_VERSION, cOPTIMIZE_MQ_TRANSFER_FOR_CMD, cPREPEND_LISTDATA_FOR_CMD, cMAINTAIN_SINGLE_LIST_FOR_CMD);
				if (interactive) sleep(5);
				break;
			} // switch (msgcmd.cmd)
			if (!interactive) break;
		} // while (0 < mqsend)
		if (!interactive) break;
	} // while (1)
	if (-1 != mqsend) {
		mq_close(mqsend);
	}
	mq_close(mqrecv);
	mq_unlink("/mq_util");
	exit(0);
}
