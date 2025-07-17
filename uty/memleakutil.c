
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
#include <dirent.h>

#include "memfns_wrap.h"

#ifdef SELF_TEST
extern void selftest();
extern void spawntestrunthread();
#endif

static const char versionString[] = "" MEMWRAP_MAJOR_VERSION "." MEMWRAP_MINOR_VERSION "";
char *outPath;
char *offlinePidSuffix;

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
#ifndef SELF_TEST1
	sprintf(heapwalkFile, "%s/hp_%d.dat", outPath, pid);
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
		sprintf(heapwalkFile, "%s/hpf_%d.dat", outPath, pid);
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
					//TODO ...check Jaga, where is suffix
				sprintf(heapwalkFile, "%s/hpf_%d.dat", outPath, pid);
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
				sprintf(heapwalkFile, "%s/hp_%d.dat", outPath, pid);
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
int prnThreadStatCmd; /* Holds for which cmd to display total heap stats */

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

/* During offline analysis, read from outPath, and populate anon when cmd is 3 */
int readStoredSmaps(unsigned pid, bool createAnon)
{
	char mmapTmpArray[1024]; /* Used to read entries from /proc/pid/smaps...big enough to hold large entries */
	FILE *fpMmap;
	
	sprintf(mmapTmpArray, "%s/smaps_%d%s.txt", outPath, pid, offlinePidSuffix?offlinePidSuffix:"");
	fpMmap = fopen(mmapTmpArray, "r");

	if (NULL != fpMmap) {
		dbg(PRINT_MUST, "smaps file available, %s\n", mmapTmpArray);
		//time_t timenow = time(NULL);
		//struct tm *tmNow = localtime(&timenow);
		//if (0 == strftime(mmapTmpArray, sizeof(mmapTmpArray), "YYYY_MM_DD HH_MM_SS %Y_%m_%d %H_%M_%S", tmNow)) {
			// Shouldn't fail unless mmapTmpArray is not big enough to hold
		//	sprintf(mmapTmpArray, "Date in epoch: %lu secs", timenow); // see if this is warned in 32 bit systems..
		//}
		MMAP_anon tmp = {0};
		while (fgets(mmapTmpArray, 1024, fpMmap)) {

			if (5 <= sscanf(mmapTmpArray, "%lx-%lx %u %u %s %s", &tmp.startAddress, &tmp.endAddress, &tmp.size, &tmp.rss, tmp.perm, tmp.entryName)) {
				dbg(PRINT_INFO, "Read %lx-%lx %u %u %s %s from %s", tmp.startAddress, tmp.endAddress, tmp.size, tmp.rss, tmp.perm, tmp.entryName, mmapTmpArray);
				if (createAnon && (('\0' == tmp.entryName[0]) || (strstr(tmp.entryName, "heap")))) {
					if ('\0' == tmp.entryName[0]) {
						strcpy(tmp.entryName, "[anon]");
					}
					addAnonEntry(tmp);
				}
				memset(&tmp, 0, sizeof(MMAP_anon));
			}
		}
		fclose(fpMmap);
	}
	else {
		PRINT("%s: Open failed, errno %d [%s]\n", mmapTmpArray, errno, strerror(errno));
	}
	return 1;
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
	//sprintf(mmapTmpArray, "cat /proc/%d/smaps > %s/test_smaps_%d.txt", pid, outPath, pid);
	//system(mmapTmpArray);
	sprintf(mmapTmpArray, "%s/smaps_%d.txt", outPath, pid);
	fpMmap = fopen(mmapTmpArray, "w");

	if (NULL != fpMmap) {
		static unsigned skipToEntry = 0, skipToSize = 0, skipToRss = 0, skipToRollover = 0; 
		unsigned skippedToLearn = 0;

		time_t timenow = time(NULL);
		struct tm *tmNow = localtime(&timenow);
		if (0 == strftime(mmapTmpArray, sizeof(mmapTmpArray), "YYYY_MM_DD HH_MM_SS %Y_%m_%d %H_%M_%S", tmNow)) {
			// Shouldn't fail unless mmapTmpArray is not big enough to hold
			sprintf(mmapTmpArray, "Date_in_epoch: %lu secs", timenow); // see if this is warned in 32 bit systems..
		}
		fprintf(fpMmap, "%s\n", mmapTmpArray);
		sprintf(mmapTmpArray, "/proc/%u/smaps", pid);
		FILE *smap = fopen(mmapTmpArray, "r");
		if (smap) {
			unsigned lines_To_skip = skipToEntry;
			unsigned skipped = 1; // Tracks current skips
			unsigned expect_entry = 1, expect_size = 0, expect_rss = 0;

			MMAP_anon tmp = {0};
			while (fgets(mmapTmpArray, 1024, smap)) {
				if (skipToRollover) { // Learnt the format
					if (++skipped > lines_To_skip) {
						if (expect_entry) {
							/* aaaad3ff0000-aaaad4139000 r-xp 00000000 b3:02 2368                       /usr/bin/bash */
							if (3 <= sscanf(mmapTmpArray, "%lx-%lx %s %*x %*s %*u %s", &tmp.startAddress, &tmp.endAddress, tmp.perm, tmp.entryName)) {
								dbg(PRINT_NOISE,"Read Entry %s %lx-%lx %s", mmapTmpArray, tmp.startAddress, tmp.endAddress, tmp.entryName);
								lines_To_skip = skipToSize;
								skipped = 1;
								expect_size = 1;
								expect_entry = 0;
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
								if (createAnon && (('\0' == tmp.entryName[0]) || (strstr(tmp.entryName, "heap")))) {
									if ('\0' == tmp.entryName[0]) {
										strcpy(tmp.entryName, "[anon]");
									}
									addAnonEntry(tmp);
								}
								lines_To_skip = skipToRollover;
								skipped = 1;
								expect_entry = 1;
								expect_rss = 0;
								fprintf(fpMmap, "%lx-%lx %u %u %s %s\n", tmp.startAddress, tmp.endAddress, tmp.size, tmp.rss, tmp.perm, tmp.entryName);
								memset(&tmp, 0, sizeof(MMAP_anon));
							}
							else {
								dbg(PRINT_ERROR,"Error Reading Rss from %s", mmapTmpArray);
							}
						} 
					}
					else {
						dbg(PRINT_INFO, "Skipping, skipped vs lines_To_skip %u:%u, line %s", skipped, lines_To_skip, mmapTmpArray);
					}
				}
				else { // Learn here
					if (!skipToEntry) {
						if (3 <= sscanf(mmapTmpArray, "%lx-%lx %s %*x %*s %*u %s", &tmp.startAddress, &tmp.endAddress, tmp.perm, tmp.entryName)) {
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
							if (createAnon && ((strstr(tmp.entryName, "heap")) || ('\0' == tmp.entryName[0]))) {
								if ('\0' == tmp.entryName[0]) { /* May not happen here.. */
									strcpy(tmp.entryName, "[anon]");
								}
								addAnonEntry(tmp);
							}
							skipToRss = skippedToLearn + 1;
							skippedToLearn = 0;
							dbg(PRINT_NOISE,"Read Rss %u after skipping %u - %s", tmp.rss, skipToRss, mmapTmpArray);
							fprintf(fpMmap, "%lx-%lx %u %u %s %s\n", tmp.startAddress, tmp.endAddress, tmp.size, tmp.rss, tmp.perm, tmp.entryName);
							memset(&tmp, 0, sizeof(MMAP_anon));
						}
						else {
							skippedToLearn++;
							dbg(PRINT_INFO, "skippedForRss: %u, %s\n", skippedToLearn, mmapTmpArray);
						}
					}
					else if (!skipToRollover) {
                                                if (3 <= sscanf(mmapTmpArray, "%lx-%lx %s %*x %*s %*u %s", &tmp.startAddress, &tmp.endAddress, tmp.perm, tmp.entryName)) {
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
void processHeapwalk(int cmd, int pid, int tid, bool isSelfTest, LIST *resp, int *listIndex, MMAP_anon *mmapIn, bool analyze)
{
	msg_resp msgresp;
	int msgsize = sizeof(msg_resp);
	char heapwalkFile[32];

	if (HEAPWALK_MMAP_ENTRIES == cmd)
	{
		if (!isSelfTest) {
			if (!analyze) {
				readAndStoreSmaps(pid, 1);
			}
			else {
				readStoredSmaps(pid, 1);
			}
		}

                if (NULL == mmapAnon) {
                        /* Looks like pmap entries couldn't be processed!! */
                        PRINT("%s: heap/anon entries couldn't be read from map\n", __FUNCTION__);
			return; // No point in continuing
                }
                else {
                        MMAP_anon *tmpprn = mmapAnon;
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
		
		/* Print results */
		MMAP_anon *tmpprn = mmapAnon;
		if ('\0' != storedTime[0])
		{
			PRINT("\tGenerated Time: %s\n", storedTime);
		}
		unsigned long anonRSSTotal = 0, heapTotal = 0;

		PRINT("\tmmapStart-mmapEnd\t\tName(Perm)\tSize(Kb)\tRSS(Kb)\tHeap'd(VM Bytes)\tHeap'd(%s)vsRSS\n", "%");
		while (tmpprn)
		{
			if (tmpprn->rss)
			{
				float percent = ((float)tmpprn->heapEntries / (float)(tmpprn->rss * 1024)) * 100;
				if (tmpprn->heapEntries)
				{
					PRINT("\t%lx-%lx\t%s(%s)\t%u\t\t%u\t%llu\t\t%.2f\n",
						  tmpprn->startAddress, tmpprn->endAddress, tmpprn->entryName, tmpprn->perm, tmpprn->size, tmpprn->rss,
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
					PRINT("\t%lx-%lx\t%s(%s)\t%u\t\t%u\n",
						  tmpprn->startAddress, tmpprn->endAddress, tmpprn->entryName, tmpprn->perm, tmpprn->size, tmpprn->rss);
				}
			}
			else
			{
				if (tmpprn->heapEntries)
				{
					PRINT("\t%lx-%lx\t%s(%s)\t%u\t\t%u\t\t%llx\t????\n",
						  tmpprn->startAddress, tmpprn->endAddress, tmpprn->entryName, tmpprn->perm, tmpprn->size, tmpprn->rss,
						  tmpprn->heapEntries);
				}
				else if (tmpprn->startAddress)
				{
					PRINT("\t%lx-%lx\t%s(%s)\t%u\t\t%u\n",
						  tmpprn->startAddress, tmpprn->endAddress, tmpprn->entryName, tmpprn->perm, tmpprn->size, tmpprn->rss);
				}
			}
			anonRSSTotal += tmpprn->rss;
			heapTotal += tmpprn->heapEntries;
			tmpprn = tmpprn->next;
		}
		removeAnonEntries();
		PRINT("TOTAL HEAP (%lu KB) vs Anon percentage: %f\n\n", heapTotal/1024, anonRSSTotal?((double)heapTotal / ((double)anonRSSTotal * 1024))*100:0); 
	}
	else
	{
		if (HEAPWALK_FULL == cmd)
		{
			if (analyze) {
				sprintf(heapwalkFile, "%s/hpf_%d%s.dat", outPath, pid, offlinePidSuffix?offlinePidSuffix:"");
			}
			else {
				sprintf(heapwalkFile, "%s/hpf_%d.dat", outPath, pid);
			}
		}
		else
		{
			if (analyze) {
				sprintf(heapwalkFile, "%s/hp_%d%s.dat", outPath, pid, offlinePidSuffix?offlinePidSuffix:"");
			}
			else {
				sprintf(heapwalkFile, "%s/hp_%d.dat", outPath, pid);
			}
		}

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
										/* Not worrying about thread safe, since I'm the sole user */
										struct tm *tmNow = localtime(&msgresp.xfer[msgIndex].seconds);
										char timef[32] = {'\0'};
										if (0 == strftime(timef, sizeof(timef), "%Y_%m_%d_%H_%M_%S", tmNow)) {
											/* may not fail..but still */
											sprintf(timef, "%lu", msgresp.xfer[msgIndex].seconds);
										}

#ifdef PREPEND_LISTDATA
										PRINT("%u %p %u %p %u %s%s\n", ++msgSeq, msgresp.xfer[msgIndex].ptr, msgresp.xfer[msgIndex].size, msgresp.xfer[msgIndex].ra,
											  msgresp.xfer[msgIndex].tid, timef,
											  (0 == (msgresp.xfer[msgIndex].flags & 0xFF01)) ? "" : (msgresp.xfer[msgIndex].flags & 0x1) ? (" -Realloc") : (" -Memalign"));
#else
										PRINT("%u %p %u %p %u %s\n", ++msgSeq, msgresp.xfer[msgIndex].ptr, msgresp.xfer[msgIndex].size, msgresp.xfer[msgIndex].ra,
											  msgresp.xfer[msgIndex].tid, timef);
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
										if ((tmpprn->startAddress <= (unsigned long)msgresp.xfer[msgIndex].ptr) &&
											((tmpprn->endAddress) >= (unsigned long)msgresp.xfer[msgIndex].ptr))
										{
											/* Get entry size including the book keeping!! */
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
					PRINT("HeapSize for walked thread(%d): %llu Bytes\n\n", tid, threadAllocationOnly);
				}
				if ((prnThreadStatCmd == cmd)) // || analyze)
				{   
					printThreadStat();
					PRINT("\nTotalHeapSize(bytes)    : %lu\n(excludes tool overhead)\nPeakSize(bytes)         : %lu at %sTool Overhead(bytes)    : %lu\n\n", 
							msgresp.totalHeapSize, msgresp.heapPeakSize, ctime(&msgresp.heapPeakedAt),  msgresp.totalOverhead);
				}
				else {
					PRINT("\n");
				}
				sleep(2);
				// dbg(PRINT_MUST, "Received Msgs %u sequence %u\n", totalMsgs, msgSeq);
			}
			else
			{
				if (!isSelfTest && (NULL == mmapIn))
				{
					dbg(PRINT_MUST, "%s\n", (HEAPWALK_FULL == cmd) ? "Already walked: None" : "No New Allocations");
					if (HEAPWALK_INCREMENT == cmd) {
						PRINT("\n");
						sleep(2);
					}
				}
			}
			fclose(fpHWalk);
		}

		if (HEAPWALK_FULL == cmd)
		{
			if (!totalMsgs) {
				prnThreadStatCmd = HEAPWALK_INCREMENT;
			}
			processHeapwalk(HEAPWALK_INCREMENT, pid, tid, isSelfTest, resp, listIndex, mmapIn, analyze);
			//if (NULL == mmapIn) {
			/*if ((NULL == mmapIn) && analyze) {
				printThreadStat();
				PRINT("\nTotalHeapSize(bytes)    : %lu\n(excludes tool overhead)\nPeakSize(bytes)         : %lu at %sTool Overhead(bytes)    : %lu\n\n", 
						msgresp.totalHeapSize, msgresp.heapPeakSize, ctime(&msgresp.heapPeakedAt),  msgresp.totalOverhead);
			}*/
		}
		else {
			if (!analyze && !isSelfTest) {
				readAndStoreSmaps(pid, 0);
			}
			else { // For now no use case to load smaps from offline store
			       //readStoredSmaps(pid, 0);
			}
		}
	}
}
#endif

void performOfflineAnalysis(int pid) //, char *offlinePidSuffix)
{
	char tmpArray[256]; 
	FILE *fp;
	bool isFullWalkAvailable = 0, isWalkAvailable = 0;

	sprintf(tmpArray, "%s/hpf_%d%s.dat", outPath, pid, offlinePidSuffix?offlinePidSuffix:"");
	fp = fopen(tmpArray, "rb");
	if (NULL != fp) {
		isFullWalkAvailable = 1;
		fclose(fp);
	}
	else {
		dbg(PRINT_ERROR, "%s: couldn't open [%s]\n", tmpArray, strerror(errno));
	}

	sprintf(tmpArray, "%s/hp_%d%s.dat", outPath, pid, offlinePidSuffix?offlinePidSuffix:"");
	fp = fopen(tmpArray, "rb");
	if (NULL != fp) {
		isWalkAvailable = 1;
		fclose(fp);
	}else {
		dbg(PRINT_ERROR, "%s: couldn't open [%s]\n", tmpArray, strerror(errno));
	}

	sprintf(tmpArray, "%s/smaps_%d%s.txt", outPath, pid, offlinePidSuffix?offlinePidSuffix:"");
	fp = fopen(tmpArray, "r");
	if (NULL != fp) {
		if ((1 == isFullWalkAvailable) && (1 == isWalkAvailable)) {
			PRINT("\nsmaps and heapwalk available..processing distribution\n");
			prnThreadStatCmd = HEAPWALK_BASE;
			processHeapwalk(HEAPWALK_MMAP_ENTRIES, pid, 0, 0, NULL, NULL, NULL, 1);
		}
		fclose(fp);
	}
	else {
		dbg(PRINT_ERROR, "%s: couldn't open [%s]\n", tmpArray, strerror(errno));
	}

	if ((1 == isFullWalkAvailable) && (1 == isWalkAvailable)) {
		printf("\nProcessing heapwalk full..\n");
		prnThreadStatCmd = HEAPWALK_FULL;
		processHeapwalk(HEAPWALK_FULL, pid, 0, 0, NULL, NULL, NULL, 1);
	}
	else if (1 == isWalkAvailable) { // see if hp_%d is atleast available...
			printf("\nProcessing heapwalk ..\n");
			prnThreadStatCmd = HEAPWALK_INCREMENT;
			processHeapwalk(HEAPWALK_INCREMENT, pid, 0, 0, NULL, NULL, NULL, 1);
	}
	else {
		PRINT("Offline heapwalk files not available for analysis at %s suffix [%s] for %d\n", outPath, (offlinePidSuffix) ? &offlinePidSuffix[1] : "", pid);
	}
}

void printHelp(int argc, char *argv[])
{
        printf("Offline analysis# %s [--readdir/-r <dir>] [--analyze <pid>]  [--suffix <suffix, for ex, 2, entry, exit..forming pid_<suffix>]\n", argv[0]);
        printf("Interactive mode# %s [--output/-o <output directory, default /tmp>]\n", argv[0]);
        printf("Perform selftest# %s selftest\n", argv[0]);
        printf("Do testrun# %s testrun\n", argv[0]);
        exit(1);
}

void processArgs(int argc, char *argv[])
{
	int offlineAnalysisPid = 0;
	//char *offlinePidSuffix = NULL;

	for (int i=1; i < argc; i++) {
		if (!strcmp(argv[i], "--outdir") || !strcmp(argv[i], "-o")) {
			if (i < argc + 1) {
				i++;
				DIR *outputDir = opendir(argv[i]);
				if (outputDir) {
					closedir(outputDir);
					outPath = argv[i];
					continue;
				}
				printf("Dir %s access error %d [%s]\n", argv[i], errno, strerror(errno));
			}
			printHelp(argc, argv);
		}
		if (!strcmp(argv[i], "--readdir") || !strcmp(argv[i], "-r")) {
			if (i < argc + 1) {
				i++;
				DIR *readDir = opendir(argv[i]);
				if (readDir) {
					closedir(readDir);
					outPath = argv[i];
					continue;
				}
				printf("Dir %s access error %d [%s]\n", argv[i], errno, strerror(errno));
			}
			printHelp(argc, argv);
		}
		if (!strcmp(argv[i], "--analyze")) {
			if (i < argc + 1) {
				i++;
				offlineAnalysisPid = atoi(argv[i]);
				continue;
			}
			printHelp(argc, argv);
		}
		if (!strcmp(argv[i], "--suffix")) {
			if (i < argc + 1) {
				i++;
				//offlinePidSuffix = argv[i];
				offlinePidSuffix = malloc(strlen(argv[i]) + 2);
				snprintf(offlinePidSuffix, strlen(argv[i]) + 2, "_%s", argv[i]);
				dbg(PRINT_INFO, "Offline analysis suffix %s\n", &offlinePidSuffix[1]);
				continue;
			}
			printHelp(argc, argv);
		}
		if (!strcmp(argv[i], "selftest"))
		{
#ifdef SELF_TEST
			FILE *fp = fopen("/tmp/memleakutil_selftest.txt", "w");
			if (NULL != fp) {
                		fclose(fp);
        		}
			outPath = "/tmp/";
			selftest();
#else
			dbg(PRINT_MUST, "ERROR...Build with SELF_TEST compiler directive to run selftest\n");
#endif
			return;
		}
		if (!strcmp(argv[i], "testrun"))
		{
		
#ifdef SELF_TEST
			outPath = "/tmp/";
			spawntestrunthread();
#else
			dbg(PRINT_MUST, "Build with SELF_TEST compiler directive to do testrun\n");
#endif
			return;
		}
		//if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) 
		{
			printHelp(argc, argv);
		}
	}
	if (offlineAnalysisPid) {
		if (NULL == outPath) {
			outPath = "/tmp/";
		}
		performOfflineAnalysis(offlineAnalysisPid);
		exit(0);
	}
}

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

	printf("Build Options:\nMEMWRAP_COMMANDS_VERSION=%d\nOPTIMIZE_MQ_TRANSFER_FOR_CMD=%c\nPREPEND_LISTDATA_FOR_CMD=%c\nMAINTAIN_SINGLE_LIST_FOR_CMD=%c\n",
		   MEMWRAP_COMMANDS_VERSION, cOPTIMIZE_MQ_TRANSFER_FOR_CMD, cPREPEND_LISTDATA_FOR_CMD, cMAINTAIN_SINGLE_LIST_FOR_CMD);
	printf("Minimum Overhead for each allocation %lu bytes\n\n", sizeof(LIST));
	printf("msg_resp size %lu bytes\n", sizeof(msg_resp));
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

	processArgs(argc, argv);
	if (NULL == outPath) {
		outPath = "/tmp/";
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
			PRINT("6. Call malloc_stats\n   %s\n", "-Calls malloc_stats API that prints the details in stderr");
			PRINT("0. Return\n   %s\n", "-Return to explore different Process");
			PRINT("Enter cmd to send: ");
			scanf("%d", &msgcmd.cmd);

			msgcmd.cmd |= HEAPWALK_BASE;
#ifdef OPTIMIZE_MQ_TRANSFER
			prnThreadStatCmd = 0;
#endif

			switch (msgcmd.cmd)
			{
			case HEAPWALK_MMAP_ENTRIES:
#ifndef OPTIMIZE_MQ_TRANSFER
				PRINT("Cmd supported only with OPTIMIZE_MQ_TRANSFER, continuing..\n");
				break;
#else
				prnThreadStatCmd = HEAPWALK_BASE;
#endif
			case HEAPWALK_INCREMENT:
#ifdef OPTIMIZE_MQ_TRANSFER
				prnThreadStatCmd = HEAPWALK_INCREMENT;
#endif
			case HEAPWALK_FULL:
			{
#ifdef OPTIMIZE_MQ_TRANSFER
				if (!prnThreadStatCmd) {
					prnThreadStatCmd = HEAPWALK_FULL;
				}
#endif
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
						processHeapwalk(msgcmd.cmd, msgcmd.pid, threadid, 0, NULL, NULL, NULL, 0);
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
				sleep(3);
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
				sleep(3);
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
				sleep(3);
				break;

			case HEAPWALK_BASE:
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
