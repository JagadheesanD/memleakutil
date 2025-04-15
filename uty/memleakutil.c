
#define _GNU_SOURCE
#include <pthread.h>
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

#include "memfns_wrap.h"

#ifdef SELF_TEST
extern void selftest();
extern void spawntestrunthread();
#endif

static const char versionString[] = "" MEMWRAP_MAJOR_VERSION "." MEMWRAP_MINOR_VERSION "";

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

	mqrecv = mq_open("/mq_util", O_CREAT | O_RDONLY, QUEUE_PERMISSION, &mqattr);
	if (0 > mqrecv)
	{
		if (EINVAL == errno)
		{
			dbg(PRINT_MUST, "Error, cannot open the queue /mq_util [%s]...Trying with default mq_maxmsg size\n", strerror(errno));
			FILE *fp;
			fp = fopen("/proc/sys/fs/mqueue/msg_max", "r");
			if (fp)
			{
				char msq_max[32];
				if (fgets(msq_max, sizeof(msq_max) - 1, fp) != NULL)
				{
					mqattr.mq_maxmsg = atoi(msq_max);
					dbg(PRINT_MUST, "Default mq_maxmsg limit is %ld\n", mqattr.mq_maxmsg);
					mqrecv = mq_open("/mq_util", O_CREAT | O_RDONLY, QUEUE_PERMISSION, &mqattr);
				}
				fclose(fp);
			}
		}
	}

	if (0 > mqrecv)
	{
		dbg(PRINT_FATAL, "Error, cannot open the queue: /mq_util %s.\n", strerror(errno));
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
	unsigned int prio;
	struct timespec tm;
	int msgsize = sizeof(msg_resp);
	char heapwalkFile[32];
	sprintf(heapwalkFile, "/tmp/hp_%d.dat", pid);
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
		sprintf(heapwalkFile, "/tmp/hpf_%d.dat", pid);
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
			return 1;
		}
		else if (msgsize)
		{
			unsigned int info = msgresp.numItemOrInfo & 0x30000000;
			if (info)
			{
				if (!fwrite((void *)&msgresp, sizeof(msg_resp), 1, fpCurrent))
				{
					dbg(PRINT_MUST, "%s: Error storing %s\n", __FUNCTION__, strerror(errno));
				}
				if (HEAPWALK_ITEM_CONTN == info)
				{
					continue;
				}
			}
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
		}
	}
	return 0;
}

MMAP_anon *mmapAnon, *mmapAnonTail;

/**
 * @brief Adds an anonymous memory entry.
 *
 * This function adds an anonymous memory entry to the linked list of memory entries.
 *
 * @param addMe The memory entry to be added.
 */
void addAnonEntry(MMAP_anon addMe)
{
	MMAP_anon *tmpAdd = (MMAP_anon *)malloc(sizeof(MMAP_anon));
	if (tmpAdd)
	{
		memcpy(tmpAdd, &addMe, sizeof(MMAP_anon));
	}
	else
	{
		dbg(PRINT_MUST, "%s: Alloc error %s\n", __FUNCTION__, strerror(errno));
		exit(0);
	}
	if (mmapAnon)
	{
		if (mmapAnon != mmapAnonTail)
		{
			mmapAnonTail->prev->next = tmpAdd;
		}
		// else
		{
			mmapAnonTail->next = tmpAdd;
			mmapAnonTail = tmpAdd;
			tmpAdd->prev = mmapAnonTail;
		}
	}
	else
	{
		mmapAnon = mmapAnonTail = tmpAdd;
	}
}

/**
 * @brief Frees mmapAnon list entries.
 *
 * This function frees all anon memory entry from the linked list of mmapAnon.
 *
 * @param void.
 */
void removeAnonEntries()
{
	MMAP_anon *tmprem;
	while (mmapAnon) {
		tmprem = mmapAnon;
		mmapAnon = mmapAnon->next;
		free(tmprem);
		/* care to set prev for tmprem?? */
		if (mmapAnon) {
			mmapAnon->prev = NULL;
		}
	}
	mmapAnon = mmapAnonTail = NULL;
}

char storedTime[32];
typedef struct threadstat
{
	int tid;
	unsigned long long allocationSize;
	// unsigned long cputime;
	struct threadstat *next;
} threadStat;
threadStat *threadStatHead;
int prnThreadStatCmd;

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
void printThreadStat()
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
void processHeapwalk(int cmd, int pid, int tid, bool isSelfTest, LIST *resp, int *listIndex, MMAP_anon *mmapIn)
{
	msg_resp msgresp;
	int msgsize = sizeof(msg_resp);
	char heapwalkFile[32];

	if (HEAPWALK_MMAP_ENTRIES == cmd)
	{
		char mmapTmpArray[128];
		sprintf(mmapTmpArray, "date > /tmp/pmap_%d.txt; pmap -x %d >> /tmp/pmap_%d.txt", pid, pid, pid);
		system(mmapTmpArray); // Replace with popen.. TODO
		sprintf(mmapTmpArray, "/tmp/pmap_%d.txt", pid);
		FILE *fpMmap = fopen(mmapTmpArray, "r");
		if (NULL != fpMmap)
		{
			storedTime[0] = '\0';
			if (NULL == fgets(storedTime, 31, fpMmap))
			{
				dbg(PRINT_MUST, "Couldn't read storedtime\n");
			}
			while (fgets(mmapTmpArray, 127, fpMmap))
			{
				// Example: 0000aaaada1f6000      44      32      32 rw---   [ anon ]
				MMAP_anon tmp = {0};
				int sscanned = sscanf(mmapTmpArray, "%lx      %u      %u      %u %s   [ %s ]",
									  &tmp.startAddress, &tmp.size, &tmp.rss, &tmp.dirty, tmp.perm, tmp.entryName);
				if ((6 == sscanned) && ('\0' != tmp.entryName[0]) && (!strcmp(tmp.entryName, "anon")))
				{
					// PRINT("%s: %lx %u %u %s\n", tmp.entryName, tmp.startAddress, tmp.size, tmp.rss, tmp.perm);
					addAnonEntry(tmp);
				}
			}
			fclose(fpMmap);
			mmapIn = mmapAnon;
			MMAP_anon *tmpprn = mmapAnon;
			while (tmpprn)
			{
				// PRINT("%s: %lx %u %u %s\n", tmpprn->entryName, tmpprn->startAddress, tmpprn->size, tmpprn->rss, tmpprn->perm);
				tmpprn->endAddress = tmpprn->startAddress + (tmpprn->size * 1024);
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
			MMAP_anon tmp = {0, ULONG_MAX, 0, 0, 0, 0, "", "***", {{0}}, NULL, NULL};
			addAnonEntry(tmp);
		}
		/* Recursive call to complete HeapWalkAll processing */
		processHeapwalk(HEAPWALK_FULL, pid, tid, isSelfTest, resp, listIndex, mmapIn);
		
		/* Print results */
		MMAP_anon *tmpprn = mmapAnon;
		if ('\0' != storedTime[0])
		{
			PRINT("\tGenerated Time: %s\n", storedTime);
		}
		PRINT("\tmmapStart-mmapEnd\t\tSize(Kb)\tRSS(Kb)\tHeap'd(Bytes)\tHeap'd(%s)vsRSS\n", "%");
		while (tmpprn)
		{
			if (tmpprn->rss)
			{
				float percent = ((float)tmpprn->heapEntries / (float)(tmpprn->rss * 1024)) * 100;
				if (tmpprn->heapEntries)
				{
					PRINT("\t%lx-%lx\t%u\t\t%u\t%llu\t\t%.2f\n",
						  tmpprn->startAddress, tmpprn->endAddress, tmpprn->size, tmpprn->rss,
						  tmpprn->heapEntries, percent);
					PRINT("\tAllocations in bytes over %u divisions\n\t", MAX_HEAT_MAP);
					for (int i = 0; i < MAX_HEAT_MAP; i++)
					{
						PRINT("[0x%04llx]", tmpprn->heatmap[i].heapEntries);
					}
					PRINT("\n");
				}
				else
				{
					PRINT("\t%lx-%lx\t%u\t\t%u\n",
						  tmpprn->startAddress, tmpprn->endAddress, tmpprn->size, tmpprn->rss);
				}
			}
			else
			{
				if (tmpprn->heapEntries)
				{
					PRINT("\t%lx-%lx\t%u\t\t%u\t\t%llx\t????\n",
						  tmpprn->startAddress, tmpprn->endAddress, tmpprn->size, tmpprn->rss,
						  tmpprn->heapEntries);
				}
				else if (tmpprn->startAddress)
				{
					PRINT("\t%lx-%lx\t%u\t\t%u\n",
						  tmpprn->startAddress, tmpprn->endAddress, tmpprn->size, tmpprn->rss);
				}
			}
			tmpprn = tmpprn->next;
		}
		removeAnonEntries();
	}
	else
	{
		if (HEAPWALK_FULL == cmd)
		{
			sprintf(heapwalkFile, "/tmp/hpf_%d.dat", pid);
		}
		else
		{
			sprintf(heapwalkFile, "/tmp/hp_%d.dat", pid);
		}

		FILE *fpHWalk = fopen(heapwalkFile, "rb");
		if (NULL == fpHWalk)
		{
			dbg(PRINT_MUST, "%s open error, %s\n", heapwalkFile, strerror(errno));
		}
		else
		{
			unsigned msgSeq = 0;
			unsigned msgIndex;
			unsigned totalMsgs = 0;
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
								dbg(PRINT_WALK, "%s\n", (HEAPWALK_FULL == cmd) ? "Already walked:" : "New Allocations:");
								dbg(PRINT_WALK, "SNo Pointer Size RA ThreadID AllocationTime\n");
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
								if (NULL == mmapIn)
								{
									if ((tid) ? tid == msgresp.xfer[msgIndex].tid : 1)
									{
#ifdef PREPEND_LISTDATA
										PRINT("%u %p %u %p %u %ld%s\n", ++msgSeq, msgresp.xfer[msgIndex].ptr, msgresp.xfer[msgIndex].size, msgresp.xfer[msgIndex].ra,
											  msgresp.xfer[msgIndex].tid, msgresp.xfer[msgIndex].seconds,
											  (msgresp.xfer[msgIndex].flags & 0x1) ? " - R" : "");
#else
										PRINT("%u %p %u %p %u %ld\n", ++msgSeq, msgresp.xfer[msgIndex].ptr, msgresp.xfer[msgIndex].size, msgresp.xfer[msgIndex].ra,
											  msgresp.xfer[msgIndex].tid, msgresp.xfer[msgIndex].seconds);
#endif
										threadAllocationOnly += msgresp.xfer[msgIndex].size;
										addThreadStatEntry(msgresp.xfer[msgIndex].tid, msgresp.xfer[msgIndex].size);
									}
								}
								else
								{
									// TODO: Add here for grouping, update mmap entry, consider memalign'd overhead using flags
									MMAP_anon *tmpprn = mmapIn;
									while (tmpprn)
									{
										// PRINT("%s: %lx %u %u %s\n", tmpprn->entryName, tmpprn->startAddress, tmpprn->size, tmpprn->rss, tmpprn->perm);
										if ((tmpprn->startAddress < (unsigned long)msgresp.xfer[msgIndex].ptr) &&
											((tmpprn->endAddress) > (unsigned long)msgresp.xfer[msgIndex].ptr))
										{
											/* Get entry size including the book keeping!! */
											unsigned size = msgresp.xfer[msgIndex].size + sizeof(LIST);
											tmpprn->heapEntries += size;
											// TODO optimize..
											int i;
											for (i = 0; i < MAX_HEAT_MAP; i++)
											{
												if ((unsigned long)msgresp.xfer[msgIndex].ptr < tmpprn->heatmap[i].endAddress)
												{
													if ((unsigned long)((char *)msgresp.xfer[msgIndex].ptr + size) <
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
									if (NULL == tmpprn)
									{
										dbg(PRINT_MUST, "Error, entry unmapped? 0x%p:%u\n", msgresp.xfer[msgIndex].ptr, msgresp.xfer[msgIndex].size);
									}
								}
							}
							msgIndex++;
						}
					}
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
					PRINT("HeapSize for walked thread(%d): %llu Bytes\n", tid, threadAllocationOnly);
				}
				if (prnThreadStatCmd == cmd)
				{   
					printThreadStat();
					PRINT("TotalHeapSize: %lu\nTool Overhead: %lu\n\n", msgresp.totalHeapSize, msgresp.totalOverhead);
				}
				// dbg(PRINT_MUST, "Received Msgs %u sequence %u\n", totalMsgs, msgSeq);
			}
			else
			{
				if (!isSelfTest && (NULL == mmapIn))
				{
					dbg(PRINT_MUST, "%s\n", (HEAPWALK_FULL == cmd) ? "Already walked: None" : "No New Allocations");
				}
			}
			fclose(fpHWalk);
		}

		if (HEAPWALK_FULL == cmd)
		{
			processHeapwalk(HEAPWALK_INCREMENT, pid, tid, isSelfTest, resp, listIndex, mmapIn);
		}
	}
}
#endif

int main(int argc, char *argv[])
{
	/* mqrecv for mq_util, mqsend for sending to mq_wrapper_<pid> */
	mqd_t mqrecv, mqsend;
	msg_cmd msgcmd;

	printf("memleakutil %s\n", versionString);
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

	printf("Build Options:\nMEMWRAP_COMMANDS_VERSION=%d\nOPTIMIZE_MQ_TRANSFER_FOR_CMD=%c\nPREPEND_LISTDATA_FOR_CMD=%c\nMAINTAIN_SINGLE_LIST_FOR_CMD=%c\n\n",
		   MEMWRAP_COMMANDS_VERSION, cOPTIMIZE_MQ_TRANSFER_FOR_CMD, cPREPEND_LISTDATA_FOR_CMD, cMAINTAIN_SINGLE_LIST_FOR_CMD);
#if defined(DEBUG_RUNTIME)
	char *dbg_level = getenv("DEBUG_ENV_LEVEL");
	if (dbg_level)
	{
		sscanf(dbg_level, "%d", &debug_level);
	}
	printf("DEBUG_RUNTIME enabled. Debug level %d\n", debug_level);
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

	if (2 == argc)
	{
		if (!strcmp(argv[1], "selftest"))
		{
#ifdef SELF_TEST
			selftest();
#else
			dbg(PRINT_MUST, "Build with SELF_TEST compiler directive to run selftest\n");
#endif
		}
		if (!strcmp(argv[1], "testrun"))
		{
#ifdef SELF_TEST
			spawntestrunthread();
#else
			dbg(PRINT_MUST, "Build with SELF_TEST compiler directive to do testrun\n");
#endif
		}
	}

	mqrecv = createMq();

	while (1)
	{
		char mq_name[64];
		msgcmd.pid = -1;
		PRINT("\nEnter Process PID to send to %s: ", "(-1 to exit)");
		scanf("%d", &msgcmd.pid);
		if (-1 == msgcmd.pid)
			break;
		if (0 == msgcmd.pid)
			msgcmd.pid = getpid();

		sprintf(mq_name, "/mq_wrapper_%d", msgcmd.pid);
		mqsend = mq_open(mq_name, O_WRONLY);
		if (mqsend < 0)
		{
			dbg(PRINT_MUST, "Error, cannot open the queue: %s, error: %s.\n", mq_name, strerror(errno));
			continue;
		}
		while (0 < mqsend) {
			PRINT("1. Heapwalk New allocations\n   %s\n", "-Shows newly allocated and not free'd entries after previous Heapwalk");
			PRINT("2. Heapwalk all allocations\n   %s\n", "-Shows all entries");
			PRINT("3. Map heap vs mmap entries\n  %s\n", "-Prints anon distribution of entries and % mapping of heap. Available with OPTIMIZE_MQ_TRANSFER");
			PRINT("4. Mark all allocations as walked\n   %s\n", "-Doesn't show any entries, but marks all as walked");
			PRINT("5. Unmark walked allocations\n   %s\n", "-Doesn't show any entries, but subsequent walk shows all entries");
			PRINT("6. Return\n   %s\n", "-Return to explore different Process");
			PRINT("Enter cmd to send: ");
			scanf("%d", &msgcmd.cmd);

			msgcmd.cmd |= HEAPWALK_BASE;
			prnThreadStatCmd = 0;

			switch (msgcmd.cmd)
			{
			case HEAPWALK_MMAP_ENTRIES:
				prnThreadStatCmd = HEAPWALK_BASE;
#ifndef OPTIMIZE_MQ_TRANSFER
				PRINT("Cmd supported only with OPTIMIZE_MQ_TRANSFER, continuing..\n");
				break;
#endif
			case HEAPWALK_INCREMENT:
				prnThreadStatCmd = HEAPWALK_INCREMENT;
			case HEAPWALK_FULL:
			{
				if (!prnThreadStatCmd) {
					prnThreadStatCmd = HEAPWALK_FULL;
				}
				int threadid = 0;
				if (HEAPWALK_MMAP_ENTRIES != msgcmd.cmd)
				{
					PRINT("Enter threadid (0 for all):");
					scanf("%d", &threadid);
					if (threadid) {
						PRINT("Walking only for thread %d\n", threadid);
					}
				}
				dbg(PRINT_MSGQ, "%s: sending cmd %d on mq %s\n", __FUNCTION__, msgcmd.cmd, mq_name);
				if (-1 != mq_send(mqsend, (const char *)&msgcmd, sizeof(msg_cmd), 0))
				{
#ifdef OPTIMIZE_MQ_TRANSFER
					if (!storeHeapwalk(mqrecv, msgcmd.cmd, msgcmd.pid, 0)) {
						processHeapwalk(msgcmd.cmd, msgcmd.pid, threadid, 0, NULL, NULL, NULL);
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
				break;

			case HEAPWALK_EXIT:
				mq_close(mqsend);
				mqsend = -1;
				break;

			default:
				dbg(PRINT_ERROR, "Invalid cmd 0x%x...continuing\n", msgcmd.cmd);
				printf("This Utility Built with:\nMEMWRAP_COMMANDS_VERSION=%d\nOPTIMIZE_MQ_TRANSFER_FOR_CMD=%c\nPREPEND_LISTDATA_FOR_CMD=%c\nMAINTAIN_SINGLE_LIST_FOR_CMD=%c\n\n",
					   MEMWRAP_COMMANDS_VERSION, cOPTIMIZE_MQ_TRANSFER_FOR_CMD, cPREPEND_LISTDATA_FOR_CMD, cMAINTAIN_SINGLE_LIST_FOR_CMD);
				break;
			}
		}
	}
	if (-1 != mqsend) {
		mq_close(mqsend);
	}
	mq_close(mqrecv);
	mq_unlink("/mq_util");
	exit(0);
}
