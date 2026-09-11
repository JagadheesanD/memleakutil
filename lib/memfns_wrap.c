/*
 * Copyright [2025] [Jagadheesan.D@gmail.com]
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>	  /* For O_* constants */
#include <sys/stat.h> /* For mode constants */
#include <mqueue.h>
#include <time.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <malloc.h>
#include "memfns_wrap.h"

#ifndef SELF_TEST
static const char versionString[] = "" MEMWRAP_MAJOR_VERSION "." MEMWRAP_MINOR_VERSION "";
#endif

STATIC pthread_mutex_t lock;

#ifndef SELF_TEST
#define STATIC static
#else
#define STATIC
#endif

STATIC int gMemInitialized = -1;

#ifdef MAINTAIN_SINGLE_LIST
STATIC LIST *hpfmemhead, *hpfmemtail, *hpwmemhead;
#else
STATIC LIST *memhead, *memtail, *wmemhead, *wmemtail;
#endif

#ifdef ENABLE_STATISTICS
unsigned long totalHeapSize, totalOverhead;
unsigned long heapPeakSize;
time_t heapPeakedAt;
#endif

#ifndef PREPEND_LISTDATA
#ifdef SELF_TEST
#define G_INITIAL_LIST_ALLOC_SIZE 1024 * sizeof(LIST)
#else
#define G_INITIAL_LIST_ALLOC_SIZE 100 * sizeof(LIST)
#endif

char *gListInitialAlloc;
unsigned int gListInitIndex;
#define G_INITIAL_LIST_ALLOC_SIZE_ERROR "Increase G_INITIAL_LIST_ALLOC_SIZE\n"
/*
#ifdef SELF_TEST
char __attribute__ ((aligned (sizeof(void*)))) gListInitialAllocP[G_INITIAL_LIST_ALLOC_SIZE];
#endif
*/
#endif

#ifdef SELF_TEST
#define G_INITIAL_ALLOC_SIZE 4 * 1024 * 1024 /* Allocate 4MB for self-test */
#else
#define G_INITIAL_ALLOC_SIZE 2 * 1024 * 1024 /* Allocate 2MB for self-test */
#endif
/*
#ifdef SELF_TEST
char __attribute__ ((aligned (4096))) gInitialAllocP[G_INITIAL_ALLOC_SIZE];
#endif
*/
char *gInitialAlloc;
unsigned int gInitIndex;
#define G_INITIAL_ALLOC_SIZE_ERROR "Increase G_INITIAL_ALLOC_SIZE\n"

// This function pointer will be dlsym'd from libpthreadintercept.so
typedef void (*sendPthreadIntercept_type)(mqd_t);
sendPthreadIntercept_type sendPthreadIntercept_fnptr = NULL;
void sndPthreadIntercepts(mqd_t);

#if !defined(DISABLE_DEBUG) && defined(DEBUG_RUNTIME)
int debug_level = 0;
void dbg(int level, const char *fmt, ...)
{
	va_list args;
	// char str[256];
	if ((level < debug_level) || (PRINT_MUST == level))
	{
		va_start(args, fmt);
		// vsprintf(str, fmt, args);
		// printf("%d: %s", getpid(), str);
		if (PRINT_WALK != level) /* Both prints may be intersected by an another thread's print!! */
		{
			printf("%d: ", getpid());
		}
		vprintf(fmt, args);
		va_end(args);
	}
}
#endif

#if defined(INTERCEPT_MMAP)
typedef void *(*mmap_type) (void *, size_t, int, int, int, off_t);
//typedef void *(*mmap64_type) (void *, size_t, int, int, int, off64_t);
typedef int (*munmap_type) (void *, size_t);
//typedef void *(*mremap_type) (void *, size_t, size_t, int, ...);

LIST_mmap_wrap *mmap_wrap_head;

mmap_type mmap_fnptr = NULL;
//mmap64 might be static function in most standard libraries like glibc
//mmap64_type mmap64_fnptr = NULL;
munmap_type munmap_fnptr = NULL;
//mremap might be static function in some standard library, like glibc
//mremap_type mremap_fnptr = NULL;

void mmap_wrapper()
{
	static int initialized = 0;
	if (!initialized) {
		mmap_fnptr = dlsym(RTLD_NEXT, "__mmap");
		//mmap64_fnptr = dlsym(RTLD_NEXT, "__mmap64"); // Not a weak/global function
		munmap_fnptr = dlsym(RTLD_NEXT, "__munmap");
		//mremap_fnptr = dlsym(RTLD_NEXT, "__mremap");
				
		if ((NULL != mmap_fnptr) && (NULL != munmap_fnptr)) // (NULL != mmap64_fnptr) && (NULL != mremap_fnptr) &&
		{
			initialized = 1;
		}
		else {
			fwrite("mmap_wrapper, failed\n", sizeof("mmap_wrapper, failed\n"), 1, stderr);
			exit(1);
		}
	}
}
void addToMMAPlist(void *start, size_t len, void *ra)
{
	LIST_mmap_wrap *tmp = mmap_wrap_head, *prev = NULL;

	// Find a deleted (start_addr is null)  entry for re-using it.
	// At the end of this loop, prev will point to tail if there 
	// is no free slot, so that newly allocated can be added at the end
	while(tmp) {
		if (!tmp->start_addr) {
			prev = NULL;
			break;
		}
		prev = tmp;
		tmp = tmp->next;
	}
	if (!tmp) {
		pthread_mutex_lock(&lock);
		tmp = (LIST_mmap_wrap*)&gInitialAlloc[gInitIndex];
		gInitIndex += sizeof(LIST_mmap_wrap);
		pthread_mutex_unlock(&lock);
		if (G_INITIAL_ALLOC_SIZE <= gInitIndex)
		{
			fwrite(G_INITIAL_ALLOC_SIZE_ERROR, sizeof(G_INITIAL_ALLOC_SIZE_ERROR), 1, stderr);
			abort();
		}
		tmp->next = NULL;
	}
	tmp->start_addr = start;
	tmp->end_addr = start + len;
	tmp->ra = ra;
	tmp->time = time(0);

	if (prev) {
		prev->next = tmp;
	}
	else {
		if (NULL == mmap_wrap_head) {
			mmap_wrap_head = tmp;
		}
	}
	//tmp = mmap_wrap_head;
	//while (tmp) { PRINT("\n%p-%p", tmp->start_addr, tmp->end_addr);tmp = tmp->next;}
	//dbg(PRINT_MUST, "\n");
}

void remFromMMAPlist(void *start, size_t len)
{
	LIST_mmap_wrap *tmp = mmap_wrap_head;

	while(tmp) {
		if (start == tmp->start_addr) {
			tmp->start_addr = NULL;
			break;
		}
		tmp = tmp->next;
	}
	if (!tmp) {
		dbg(PRINT_ERROR, "munmap list rem failed 0x%p, size %ld list head 0x%p\n", start, len, mmap_wrap_head);
	}
	//tmp = mmap_wrap_head;
	//while (tmp) { PRINT("\n%p-%p", tmp->start_addr, tmp->end_addr);tmp = tmp->next;}
	//dbg(PRINT_MUST, "\n");
}

__attribute__((visibility("default"))) void *mmap(void *start, size_t len, int prot, int flags, int fd, off_t offset)
{
	void *ra = __builtin_return_address(0);
	//fwrite("intercepting mmap\n", sizeof("intercepting mmap\n"), 1, stderr);
	if (NULL == mmap_fnptr) { // Not efficient, as this is not always true
		mmap_wrapper();
		if (NULL == mmap_fnptr) {
			fwrite("Couldn't dlsym __mmap\n", sizeof("Couldn't dlsym __mmap\n"), 1, stderr);
			exit(1);
		}
	}
	void *mapped = mmap_fnptr(start, len, prot, flags, fd, offset);
	addToMMAPlist(mapped, len, ra);
	return mapped;
}
__attribute__((visibility("default"))) int munmap(void *start, size_t len)
{
	//fwrite("intercepting munmap\n", sizeof("intercepting munmap\n"), 1, stderr);
	remFromMMAPlist(start, len); // assuming munmap to be successful
	return munmap_fnptr(start, len);
}
#if 0
__attribute__((visibility("default"))) void *mremap(void *oldaddr, int tmp,  size_t old_size, size_t new_size, int flags, ...)
{
	fwrite("intercepting mremap\n", sizeof("intercepting mremap\n"), 1, stderr);
	va_list args;
	va_start(args, flags);
	void *newaddr = mremap_fnptr(oldaddr, old_size, new_size, flags, args);
	va_end(args);
	return newaddr;
}
__attribute__((visibility("default"))) void *mmap64(void *start, size_t len, int prot, int flags, int fd, off64_t offset)
{
	fwrite("intercepting mmap64\n", sizeof("intercepting mmap64\n"), 1, stderr);
	if (NULL == mmap64_fnptr)
		mmap_wrapper();
	return mmap64_fnptr(start, len, prot, flags, fd, offset);
}
#endif
#ifndef PREPEND_LISTDATA
char gListInitialAlloc_buffer[G_INITIAL_LIST_ALLOC_SIZE];
#endif

char gInitialAlloc_buffer[G_INITIAL_ALLOC_SIZE];

void sndMmapIntercepts(mqd_t mqsend)
{
	msg_resp msgresp;
	// Plan is to write this as header for checking compatibility during offline analysis
	hp_walk_header hpwHdr = {MEMWRAP_MSG_RESP_VERSION,0};

	LIST_mmap_wrap *tmp = mmap_wrap_head;

	msgresp.numItemOrInfo = HEAPWALK_EMPTY;
	if (NULL == tmp) {
		mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);
	}
	else {
		while (tmp)
		{
			msgresp.xfer[msgresp.numItemOrInfo].stack_addr_bottom = tmp->start_addr;
			msgresp.xfer[msgresp.numItemOrInfo].start_routine = tmp->ra;
			msgresp.xfer[msgresp.numItemOrInfo].size = tmp->end_addr - tmp->start_addr;
			msgresp.xfer[msgresp.numItemOrInfo++].seconds = tmp->time;
			hpwHdr.totalEntries++;

			// Check if we've reached max size to transfer
			if (MAX_MSG_XFER <= msgresp.numItemOrInfo) {
				if (tmp->next) {
					msgresp.numItemOrInfo |= HEAPWALK_ITEM_CONTN;
				}
				else {
					msgresp.numItemOrInfo |= HEAPWALK_ENDOF_LIST;
				}
				mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp),0);
				msgresp.numItemOrInfo = HEAPWALK_EMPTY;
			}
			tmp = tmp->next;
		}
		if (msgresp.numItemOrInfo) { 
			msgresp.numItemOrInfo |= HEAPWALK_ENDOF_LIST;
			mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);
		}
	}
}
#endif

/**
 * @brief Initializes the initial memory mapping.
 *
 * This function maps the initial memory for list allocation and the main allocation.
 * It uses mmap to create the mappings and handles errors accordingly.
 */
void mapInitialMemory()
{
#ifndef PREPEND_LISTDATA
#if !defined(INTERCEPT_MMAP)
	if (NULL == gListInitialAlloc)
	{
		gListInitialAlloc = mmap(NULL, G_INITIAL_LIST_ALLOC_SIZE, PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (gListInitialAlloc == MAP_FAILED)
		{
			fwrite("gListInitialAlloc mmap failed\n", strlen("gListInitialAlloc mmap failed\n"), 1, stderr);
			exit(2);
		}
	}
#else
	if (NULL == gListInitialAlloc) {
		gListInitialAlloc = gListInitialAlloc_buffer;
	}
#endif // #if !defined(INTERCEPT_MMAP)
#endif // #ifndef PREPEND_LISTDATA
#if !defined(INTERCEPT_MMAP)
	if (NULL == gInitialAlloc)
	{
		gInitialAlloc = mmap(NULL, G_INITIAL_ALLOC_SIZE, PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (gInitialAlloc == MAP_FAILED)
		{
			fwrite("gInitialAlloc mmap failed\n", strlen("gInitialAlloc mmap failed\n"), 1, stderr);
			exit(2);
		}
#ifdef SELF_TEST /* During self test, expect the entries to be present inside the same mmap. */
		memset(gInitialAlloc, 0, G_INITIAL_ALLOC_SIZE);
#endif
	}
#else
	if (NULL == gInitialAlloc) {
		gInitialAlloc = gInitialAlloc_buffer;
		mmap_wrapper();
		/*gInitialAlloc = mmap(gInitialAlloc, G_INITIAL_ALLOC_SIZE, PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (gInitialAlloc == MAP_FAILED)
		{
			fwrite("gInitialAlloc mmap failed\n", strlen("gInitialAlloc mmap failed\n"), 1, stderr);
			exit(2);
		}*/
	}
#endif
}

// Making it global to close & unlink during exit
mqd_t mq;
char mq_name[64];
__attribute__((destructor)) void library_exit()
{
        mq_close(mq);
        mq_unlink(mq_name);
}

/**
 * @brief Thread start function.
 *
 * This function handles message queue operations and manages memory initialization in a concurrent environment.
 *
 * @param arg A generic argument passed to the thread function.
 * @return Always returns NULL.
 */
static void *thread_start(void *arg)
{
	dbg(PRINT_INFO, "%s: Starting thread\n", __FUNCTION__);
#ifndef SELF_TEST
	dbg(PRINT_INFO, "%s: Starting thread: version %s\n", __FUNCTION__, versionString);
#endif
	mqd_t mqsend;
	msg_cmd msgcmd;
	unsigned int prio;

	struct mq_attr mqattr = ((struct mq_attr){0, 3, sizeof(msg_cmd), 0, {0}});
	sprintf(mq_name, "/mq_wrapper_%d", getpid());
	/* Create with read/write */
	mq = mq_open(mq_name, O_CREAT | O_RDONLY, QUEUE_PERMISSION, &mqattr);
	if (0 > mq)
	{
		dbg(PRINT_MUST, "Error, cannot open/create the queue: %s.error %s\n", mq_name, strerror(errno));
		dbg(PRINT_MSGQ, "Trying to open without O_CREAT flag..\n");
		mq = mq_open(mq_name, O_RDONLY, QUEUE_PERMISSION, &mqattr);
		if (0 > mq)
		{
			dbg(PRINT_MUST, "Error, cannot open the queue: %s.error %s\n", mq_name, strerror(errno));
			exit(1);
		}
	}

	/* Purge for new invocations, when fork'd, Selftest might be interrupted!! */
	if (-1 == gMemInitialized)
	{
		/* purge initial msg if any, for any process restarts */
		struct timespec tm;
		msgcmd.cmd = -1;
		do
		{
			if (-1 != msgcmd.cmd)
			{
				dbg(PRINT_MSGQ, "Purging cmd %d by thread %d\n", msgcmd.cmd, gettid());
			}
			clock_gettime(CLOCK_REALTIME, &tm);
			tm.tv_sec += 1;
			errno = 0;
		} while (0 <= mq_timedreceive(mq, (char *)&msgcmd, sizeof(msg_cmd), &prio, &tm));
		if (ETIMEDOUT != errno)
		{
			dbg(PRINT_ERROR, "Error, mq_receive: %s.\n", strerror(errno));
		}
	}

	while (1)
	{
		int msgsize = mq_receive(mq, (char *)&msgcmd, sizeof(msg_cmd), &prio);
		if (msgsize >= 0)
		{
			dbg(PRINT_MSGQ, "Received cmd %d, size %d\n", msgcmd.cmd, msgsize);

			switch (msgcmd.cmd)
			{
				case HEAPWALK_INCREMENT:
				case HEAPWALK_FULL:
				case HEAPWALK_LEAKCHECK:
#ifdef OPTIMIZE_MQ_TRANSFER
				case HEAPWALK_MMAP_ENTRIES:
#endif
					mqsend = mq_open("/mq_util", O_WRONLY);
					if (0 <= mqsend) {
						dbg(PRINT_MSGQ, "%s: sending on mq %d\n", __FUNCTION__, mqsend);
						dbg(PRINT_ERROR, "%s: msgcmd 0x%X sending on mq %d\n", __FUNCTION__, msgcmd.cmd, mqsend);
#ifdef OPTIMIZE_MQ_TRANSFER
						heapwalk(mqsend, ~HEAPWALK_INCREMENT&msgcmd.cmd, NULL);
						if (HEAPWALK_MMAP_ENTRIES == msgcmd.cmd) {
							//dbg(PRINT_MSGQ, "%s: sending on mq %d\n", __FUNCTION__, mqsend);
							dbg(PRINT_ERROR, "%s: cmd: 0x%x sending pthread intercepts on mq %d\n", __FUNCTION__, msgcmd.cmd, mqsend);
							sndPthreadIntercepts(mqsend);
#if defined(INTERCEPT_MMAP)
							sndMmapIntercepts(mqsend);
#endif
						}
#else
						msg_resp msgresp;
						if (HEAPWALK_INCREMENT == msgcmd.cmd)
							heapwalk(mqsend);
						else
							heapwalk_full(mqsend);
						msgresp.seq = -1;
	#if defined(PREPEND_LISTDATA) && defined(ENABLE_STATISTICS)
						snprintf(msgresp.msg, MQ_MSG_SIZE, "TotalHeapSize %lu Bytes + Tool Overhead %lu. Peak %lu at %u", 
								totalHeapSize, totalOverhead, heapPeakSize, heapPeakedAt);
	#endif
						mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);
#endif
						dbg(PRINT_MSGQ, "%s: sent on mq %d\n", __FUNCTION__, mqsend);
						mq_close(mqsend);
					}
					else {
						dbg(PRINT_ERROR, "Error, cannot open mq_util queue: %s.\n", strerror(errno));
					}
					break;
				case HEAPWALK_PTHREAD_INTERCEPT: // Not used, atleast in version 4
					mqsend = mq_open("/mq_util", O_WRONLY);
					if (0 <= mqsend) {
						dbg(PRINT_MSGQ, "%s: sending on mq %d\n", __FUNCTION__, mqsend);
						sndPthreadIntercepts(mqsend);
						mq_close(mqsend);
					}
					else {
						dbg(PRINT_ERROR, "Error, cannot open mq_util queue: %s.\n", strerror(errno));
					}
					break;
				case HEAPWALK_MARKALL:
					dbg(PRINT_MSGQ, "Calling heapwalkMarkall(). cmd %d\n", msgcmd.cmd);
					heapwalkMarkall();
					break;
				case HEAPWALK_RESET_MARKED:
					dbg(PRINT_MSGQ, "Calling heapwalkReset(). cmd %d\n", msgcmd.cmd);
					heapwalkReset();
					break;
				case HEAPWALK_MALLOC_STATS:
					dbg(PRINT_MSGQ, "Calling malloc_stats(). cmd %d\n", msgcmd.cmd);
					malloc_stats();
					PRINT("\n");
					break;
				default:
					dbg(PRINT_ERROR, "Invalid cmd 0x%x received\n", msgcmd.cmd);
					break;
			}
		}
		else
		{
			/** Careful, this error msg going to stdout might affect child process's called via popen()!! **/
			//dbg(PRINT_INFO, "Error, mq_receive from %s [%s]\n", mq_name, strerror(errno));
			char tmp[96];
			fwrite(tmp, snprintf(tmp, 95, "Error, mq_receive from %s [%s]\n", mq_name, strerror(errno)), 1, stderr);
			sleep(1);
		}
	}
	mq_close(mq);
	mq_unlink(mq_name);
}

typedef void *(*calloc_type)(size_t, size_t);
typedef void *(*malloc_type)(size_t);
typedef void (*free_type)(void *);
typedef void *(*realloc_type)(void *, size_t);

calloc_type libc_calloc_fnptr = NULL;
malloc_type libc_malloc_fnptr = NULL;
free_type libc_free_fnptr = NULL;
realloc_type libc_realloc_fnptr = NULL;

#if defined(__USE_XOPEN2K)
typedef int (*posix_memalign_type)(void **, size_t, size_t);
posix_memalign_type libc_posix_memalign_fnptr = NULL;
#endif

#if defined(__USE_ISOC11)
typedef void *(*aligned_alloc_type)(size_t, size_t);
aligned_alloc_type libc_aligned_alloc_fnptr = NULL;
#endif

#if defined(USE_DEPRECATED_MEMALIGN)
typedef void *(*memalign_type)(size_t, size_t);
memalign_type libc_memalign_fnptr = NULL;
#endif

/**
 * @brief Start the heapwalk thread.
 *
 * This function initializes mutex attributes and creates a new thread for heapwalk operations.
 */
__attribute__((constructor)) void heapwalk_thread_start()
{
	pthread_attr_t attr;
	pthread_t ptd;
	pthread_mutexattr_t mutexattr;

	pthread_mutexattr_init(&mutexattr);
	pthread_mutexattr_settype(&mutexattr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&lock, &mutexattr);
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 8 * 1024);

	pthread_create(&ptd, &attr, &thread_start, NULL);
	// pthread_join(ptd, NULL);
	pthread_attr_destroy(&attr);
}

/* create a thread in child process for heapwalk. */
/* register_atfork is similar to pthread_atfork. */
extern int __register_atfork(void (*__prepare)(void), void (*__parent)(void), void (*__child)(void), void *dso_handle);
extern void *__dso_handle __attribute__((__weak__));

/**
 * @brief Runs in the child process context.
 *
 * This function starts the heapwalk thread when running in the context of a forked child process.
 */
static void run_in_child_context(void)
{
	/** Careful, this info msg going to stdout, when enabled, might affect child process's called via popen()!! **/
	/*dbg(PRINT_INFO, "%s: pid %d\n", __FUNCTION__, getpid());*/
	heapwalk_thread_start();
}

#ifndef SELF_TEST2
__attribute__((constructor))
#endif
/**
 * @brief Loads libc memory allocation functions.
 *
 * This function dynamically loads memory allocation functions from libc using dlsym.
 */
void load_libc_functions()
{
	//fwrite("load_libc_functions+\n", strlen("load_libc_functions+\n"), 1, stderr);

	/* Does not need an execution, No effect */
	if (-1 == gMemInitialized)
	{
		gMemInitialized = 0;
		/* Load Memory allocation functions from libc */
#ifdef __GLIBC__
		libc_calloc_fnptr = dlsym(RTLD_NEXT, "__libc_calloc"); /* Throw error when all reads fail */
		libc_malloc_fnptr = dlsym(RTLD_NEXT, "__libc_malloc");
		libc_free_fnptr = dlsym(RTLD_NEXT, "__libc_free");
		libc_realloc_fnptr = dlsym(RTLD_NEXT, "__libc_realloc");
#if defined(__USE_XOPEN2K)
		libc_posix_memalign_fnptr = dlsym(RTLD_NEXT, "__libc_posix_memalign");
#endif

#if defined(__USE_ISOC11)
		libc_aligned_alloc_fnptr = dlsym(RTLD_NEXT, "__libc_aligned_alloc");
#endif
#if defined(USE_DEPRECATED_MEMALIGN)
		libc_memalign_fnptr = dlsym(RTLD_NEXT, "__libc_memalign");
#endif
		fwrite("dlsym'd\n", strlen("dlsym'd\n"), 1, stderr);

		if ((NULL == libc_malloc_fnptr) || (NULL == libc_calloc_fnptr) || (NULL == libc_free_fnptr) || (NULL == libc_realloc_fnptr)
#if defined(USE_DEPRECATED_MEMALIGN)
			|| (NULL == libc_memalign_fnptr)
#endif

#if defined(__USE_XOPEN2K)
			|| (NULL == libc_posix_memalign_fnptr)
#endif

#if defined(__USE_ISOC11)
			|| (NULL == libc_aligned_alloc_fnptr)
#endif
		)
		{
			fwrite("Couldn't load symbols from libc", strlen("Couldn't load symbols from libc"), 1, stderr);
#if defined(USE_DEPRECATED_MEMALIGN)
			if (NULL == libc_memalign_fnptr)
			{
				fwrite(": memalign not loaded\n", strlen(": memalign not loaded\n"), 1, stderr);
			}
#endif

#if defined(__USE_XOPEN2K)
			if (NULL == libc_posix_memalign_fnptr)
			{
				fwrite(": memalign not loaded\n", strlen(": posix_memalign not loaded\n"), 1, stderr);
			}
#endif

#if defined(__USE_ISOC11)
			if (NULL == libc_aligned_alloc_fnptr)
			{
				fwrite(": memalign not loaded\n", strlen(": aligned_alloc not loaded\n"), 1, stderr);
			}
#endif
			abort();
		}
		else
		{
			if (__register_atfork(NULL, NULL, run_in_child_context, __dso_handle) != 0)
			{
				fwrite("Error in __register_atfork", strlen("Error in __register_atfork"), 1, stderr);
				abort();
			}
			gMemInitialized = 1;
			/** Careful, these info msg going to stdout, when enabled, might affect child process's called via popen()!! **/
			dbg(PRINT_INFO, "%s: Loaded symbols from libc, malloc:calloc:free:realloc [%p][%p][%p][%p]\n",
				__FUNCTION__, libc_malloc_fnptr, libc_calloc_fnptr, libc_free_fnptr, libc_realloc_fnptr);
#if defined(USE_DEPRECATED_MEMALIGN)
			dbg(PRINT_INFO, "%s: Loaded symbols from libc, memalign [%p]\n", __FUNCTION__, libc_memalign_fnptr);
#endif

#if defined(__USE_XOPEN2K)
			dbg(PRINT_INFO, "%s: Loaded symbols from libc, posix_memalign [%p]\n", __FUNCTION__, libc_posix_memalign_fnptr);
#endif

#if defined(__USE_ISOC11)
			dbg(PRINT_INFO, "%s: Loaded symbols from libc, aligned_alloc [%p]\n", __FUNCTION__, libc_aligned_alloc_fnptr);
#endif
		}
#else
		void *libcLibrary = dlopen("libc.so", RTLD_LAZY | RTLD_NOLOAD);
		if (libcLibrary)
		{
			libc_calloc_fnptr = dlsym(libcLibrary, "calloc");
			libc_malloc_fnptr = dlsym(libcLibrary, "malloc");
			libc_free_fnptr = dlsym(libcLibrary, "free");
			libc_realloc_fnptr = dlsym(libcLibrary, "realloc");
			libc_memalign_fnptr = dlsym(libcLibrary, "memalign");
			dlclose(libcLibrary);
		}
		else
		{
			fwrite("dlopen failed..try without RTLD_NOLOAD\n", strlen("dlopen failed..try without RTLD_NOLOAD\n"), 1, stderr);
		}
#endif
	}
}

void libc_free(void *ptr);
void *libc_malloc(size_t size);

#ifdef SELF_TEST
void resetList() // for testing the list, since a default entry get in and test case couldn't be written
{
#ifdef MAINTAIN_SINGLE_LIST
	hpfmemhead = hpfmemtail = hpwmemhead = NULL;
#else
	memhead = memtail = wmemhead = wmemtail = NULL;
#endif
}

void dispStatus()
{
	dbg(PRINT_MUST, "%s: Enter from pid:tid %d:%d\n", __FUNCTION__, getpid(), gettid());
	dbg(PRINT_MUST, "%s:\ngMemInitialized %d\n", __FUNCTION__, gMemInitialized);
	if (0 < gMemInitialized)
	{
		dbg(PRINT_MUST, "Loaded symbols from libc, malloc:calloc:free:realloc [%p][%p][%p][%p]\n",
			libc_malloc_fnptr, libc_calloc_fnptr, libc_free_fnptr, libc_realloc_fnptr);
#if defined(USE_DEPRECATED_MEMALIGN)
		if (NULL != libc_memalign_fnptr)
		{
			dbg(PRINT_MUST, "%s: Loaded symbols from libc, memalign [%p]\n", __FUNCTION__, libc_memalign_fnptr);
		}
#endif

#if defined(__USE_XOPEN2K)
		if (NULL != libc_posix_memalign_fnptr)
		{
			dbg(PRINT_MUST, "%s: Loaded symbols from libc, posix_memalign [%p]\n", __FUNCTION__, libc_posix_memalign_fnptr);
		}
#endif

#if defined(__USE_ISOC11)
		if (NULL != libc_aligned_alloc_fnptr)
		{
			dbg(PRINT_MUST, "%s: Loaded symbols from libc, aligned_alloc [%p]\n", __FUNCTION__, libc_aligned_alloc_fnptr);
		}
#endif
	}
#ifndef PREPEND_LISTDATA
	dbg(PRINT_MUST, "gInitialAlloc %p, gInitIndex %d\ngListInitialAlloc %p, gListInitIndex %d\n", gInitialAlloc, gInitIndex, gListInitialAlloc, gListInitIndex);
#else
	dbg(PRINT_MUST, "gInitialAlloc %p, gInitIndex %d\n", gInitialAlloc, gInitIndex);
#endif
	pthread_mutex_lock(&lock);
	dbg(PRINT_MUST, "Already walked:\n");
#ifndef MAINTAIN_SINGLE_LIST
	LIST *tmp = wmemhead;
	while (tmp)
	{
		dbg(PRINT_MUST, "Ptr: %p size: %u ra: %p tid: %ld time: %ld\n", tmp->ptr, tmp->size, tmp->ra, (long)tmp->tid, (long)tmp->seconds);
		tmp = tmp->next;
	}
	dbg(PRINT_MUST, "New Allocations:\n");
	tmp = memhead;
	while (tmp)
	{
		dbg(PRINT_MUST, "Ptr: %p size: %u ra: %p tid: %ld time: %ld\n",
			tmp->ptr, tmp->size, tmp->ra, (long)tmp->tid, (long)tmp->seconds);
		tmp = tmp->next;
	}
#else
	LIST *tmp = hpfmemhead;
	dbg(PRINT_MUST, "Ptr\tsize\tra\ttid\ttime\n");
	while (tmp)
	{
		if (tmp == hpwmemhead)
		{
			dbg(PRINT_MUST, "New Allocations:\n");
		}
		dbg(PRINT_MUST, "%p\t%u\t%p\t%ld\t%ld\n", tmp->ptr, tmp->size, tmp->ra, (long)tmp->tid, (long)tmp->seconds);
		tmp = tmp->next;
	}
#endif
	dbg(PRINT_MUST, "\n");
	pthread_mutex_unlock(&lock);
}
#endif

/**
 * @brief Retrieves the item from the list for a given pointer.
 *
 * @param ptr The pointer for which the item needs to be retrieved.
 * @return The item corresponding to the given pointer, or NULL if not found.
 */
LIST *getItem(void *ptr)
{
	LIST *ret = NULL;
	pthread_mutex_lock(&lock);
#ifndef PREPEND_LISTDATA
    #ifndef MAINTAIN_SINGLE_LIST
	LIST *tmp = memhead;
	while (tmp)
	{
		if (ptr == tmp->ptr)
		{
			pthread_mutex_unlock(&lock);
			return tmp;
		}
		tmp = tmp->next;
	}
	tmp = wmemhead;
	while (tmp)
	{
		if (ptr == tmp->ptr)
		{
			ret = tmp;
			break;
		}
		tmp = tmp->next;
	}
    #else  /* else of #ifndef MAINTAIN_SINGLE_LIST */
	LIST *tmp = hpfmemhead;
	while (tmp)
	{
		if (ptr == tmp->ptr)
		{
			pthread_mutex_unlock(&lock);
			return tmp;
		}
		tmp = tmp->next;
	}
    #endif /* End of #ifndef MAINTAIN_SINGLE_LIST */
#else  /* else of #ifndef PREPEND_LISTDATA */
	ret = (LIST *)((char *)ptr - sizeof(LIST));
	// TODO: Add unlikely attribute
	if (0xBEAD0000 != (ret->flags & 0xFFFF0000))
	{
		ret = NULL;
	}
#endif
	pthread_mutex_unlock(&lock);
	return ret;
}

/**
 * @brief Marks all the memory allocations as tracked.
 *
 * This function moves all new allocations to the list of tracked allocations.
 */
__attribute__((weak)) void heapwalkMarkall()
{
	pthread_mutex_lock(&lock);
#ifdef MAINTAIN_SINGLE_LIST
	hpwmemhead = NULL;
#else
	if (wmemtail)
	{
		wmemtail->next = memhead;
		if (memhead)
		{
#ifdef PREPEND_LISTDATA
			memhead->prev = wmemtail;
#endif
			wmemtail = memtail;
		}
	}
	else
	{
		wmemhead = memhead;
		wmemtail = memtail;
	}

	memhead = memtail = NULL;
#endif
	pthread_mutex_unlock(&lock);
}

/**
 * @brief Resets the tracked memory allocations.
 *
 * This function moves all tracked allocations back to the list of new allocations.
 */
void heapwalkReset()
{
	/* Protect */
	pthread_mutex_lock(&lock);

#ifdef MAINTAIN_SINGLE_LIST
	hpwmemhead = hpfmemhead;
#else
	if (memhead)
	{
		if (wmemtail)
		{
			wmemtail->next = memhead;
#ifdef PREPEND_LISTDATA
			memhead->prev = wmemtail;
#endif
			memhead = wmemhead;
		}
	}
	else
	{
		memhead = wmemhead;
		memtail = wmemtail;
	}
	wmemhead = wmemtail = NULL;
	/* LIST *tmp = wmemhead;
	LIST *tail = wmemhead;
		while (tmp) {
		tail = tmp;
				tmp = tmp->next;
		}
	if (tail){
		tail->next = memhead;
		memhead = wmemhead;
		wmemhead = NULL;
	} */
#endif
	pthread_mutex_unlock(&lock);
}

#ifdef OPTIMIZE_MQ_TRANSFER
/**
 * @brief Walks the heap and transfers memory information to the message queue.
 *
 * This function transfers information about heap allocations to the provided message queue descriptor.
 *
 * @param mqsend The message queue descriptor to which memory information will be sent.
 * @param walkAll A flag indicating whether to walk all allocations or only new ones.
 */
void heapwalk(mqd_t mqsend, bool walkAll, char *fname)
{
	msg_resp msgresp;
	hp_walk_header hpwHdr = {MEMWRAP_MSG_RESP_VERSION,0};
	LIST *tmp;

	dbg(PRINT_INFO, "%s+: hpfmemhead [%p] hpwmemhead [%p] hpfmemtail [%p]\n", __FUNCTION__, hpfmemhead, hpwmemhead, hpfmemtail);

	msgresp.numItemOrInfo = HEAPWALK_EMPTY;
	/* to be tested and added
	FILE *fp = fopen(fname, "wb");
	if (NULL != fp) {
		fseek(fp, 0, SEEK_SET);
		if (!fwrite((void *)&hpwHdr, sizeof(hpwHdr), 1, fp)) {
			dbg(PRINT_ERROR, "%s:%d: store error [%s]\n", __FUNCTION__, __LINE__, strerror(errno));
		}
		fclose(fp);
	}
	dbg(PRINT_ERROR, "1Total entries: %lu, version: %u\n", hpwHdr.totalEntries, hpwHdr.version);
	*/
	pthread_mutex_lock(&lock);
	if (walkAll)
	{
#ifdef MAINTAIN_SINGLE_LIST
		tmp = hpfmemhead;
#else
		tmp = wmemhead;
#endif
		if (NULL != tmp)
		{
			while (tmp)
			{
#ifdef MAINTAIN_SINGLE_LIST
				if (tmp == hpwmemhead)
				{
					dbg(PRINT_NOISE, "%s: Breaking for processing new allocation\n", __FUNCTION__);
					/* In case where there was no previous walk, then send full walk as empty and
					 * then proceed with to be walked send */
					if (hpwmemhead == hpfmemhead) {
						msgresp.numItemOrInfo = HEAPWALK_EMPTY;
						if (NULL == fname) {
							mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);
						}
					}
					break;
				}
#endif

#ifdef PREPEND_LISTDATA
				msgresp.xfer[msgresp.numItemOrInfo].flags = tmp->flags;
#endif
				msgresp.xfer[msgresp.numItemOrInfo].ptr = tmp->ptr;
				msgresp.xfer[msgresp.numItemOrInfo].size = tmp->size;
				msgresp.xfer[msgresp.numItemOrInfo].ra = tmp->ra;
				msgresp.xfer[msgresp.numItemOrInfo].tid = tmp->tid;
				msgresp.xfer[msgresp.numItemOrInfo].seconds = tmp->seconds;
				msgresp.numItemOrInfo++;
				hpwHdr.totalEntries++;
				dbg(PRINT_INFO, "%s: Processed %d walked items for sending\n", __FUNCTION__, msgresp.numItemOrInfo);
				if (MAX_MSG_XFER <= msgresp.numItemOrInfo)
				{
					if (tmp->next)
					{
						dbg(PRINT_INFO, "%s: sending %d walked items..more to go\n", __FUNCTION__, msgresp.numItemOrInfo);
						msgresp.numItemOrInfo |= HEAPWALK_ITEM_CONTN;
					}
					else
					{ /* Set the end of list */
						dbg(PRINT_INFO, "%s: sending %d walked items..finished\n", __FUNCTION__, msgresp.numItemOrInfo);
						msgresp.numItemOrInfo |= HEAPWALK_ENDOF_LIST;
					}
#if defined(PREPEND_LISTDATA) && defined(ENABLE_STATISTICS)
					msgresp.totalHeapSize = totalHeapSize;
					msgresp.totalOverhead = totalOverhead;
					msgresp.heapPeakSize = heapPeakSize;
					msgresp.heapPeakedAt = heapPeakedAt;
#endif
					if (NULL == fname) {
						mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);
					}
					else {
						/* Reason we open everytime and write is to avoid showing an allocation
						 * during fopen and fwrite 
						 */
						FILE *fp = fopen(fname, "ab");
						if (NULL != fp) {
							if (!fwrite((void *)&msgresp, sizeof(msg_resp), 1, fp)) {
								dbg(PRINT_ERROR, "%s:%d: store error [%s]\n", __FUNCTION__, __LINE__, strerror(errno));
							}
							fclose(fp);
						}
					}
					msgresp.numItemOrInfo = HEAPWALK_EMPTY;
				}
				tmp = tmp->next;
			}
			if (msgresp.numItemOrInfo)
			{
				dbg(PRINT_INFO, "%s: sending %d walked items..finishing outside loop\n", __FUNCTION__, msgresp.numItemOrInfo);
				msgresp.numItemOrInfo |= HEAPWALK_ENDOF_LIST;
#if defined(PREPEND_LISTDATA) && defined(ENABLE_STATISTICS)
				msgresp.totalHeapSize = totalHeapSize;
				msgresp.totalOverhead = totalOverhead;
				msgresp.heapPeakSize = heapPeakSize;
				msgresp.heapPeakedAt = heapPeakedAt;
#endif
				// TODO: for now send the full size. sync receiver to accept for lesser size
				/* Send the message with the information collected */
				if (NULL == fname) {
					mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);
				}
				else {
					FILE *fp = fopen(fname, "ab");
					if (NULL != fp) {
						if (!fwrite((void *)&msgresp, sizeof(msg_resp), 1, fp)) {
							dbg(PRINT_ERROR, "%s:%d: store error [%s]\n", __FUNCTION__, __LINE__, strerror(errno));
						}
						fclose(fp);
					}
				}
			}
		}
		else
		{
			// msgresp.numItemOrInfo = HEAPWALK_EMPTY; Initialized already
			if (NULL == fname) {
				mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);
			}
		}
		if (fname) {
			pthread_mutex_unlock(&lock);
			/* To be tested and added later
			if (hpwHdr.totalEntries) {
				fp = fopen(fname, "ab");
				if (NULL != fp) {
					fseek(fp, 0, SEEK_SET);
					if (!fwrite((void *)&hpwHdr, sizeof(hpwHdr), 1, fp)) {
						dbg(PRINT_ERROR, "%s:%d: store error [%s]\n", __FUNCTION__, __LINE__, strerror(errno));
					}
					fclose(fp);
				}
			}
			*/
			dbg(PRINT_INFO, "2Total entries: %lu, version: %u\n", hpwHdr.totalEntries, hpwHdr.version);
			return;
		}
	} // if (walkAll)

	msgresp.numItemOrInfo = HEAPWALK_EMPTY;
#ifdef MAINTAIN_SINGLE_LIST
	tmp = hpwmemhead;
#else
	tmp = memhead;
#endif
	if (NULL != tmp)
	{
		while (tmp)
		{
			// memcpy(((char*)msgresp.xfer + msgresp_xfer_index), tmp, sizeof(LISTxfer));
#ifdef PREPEND_LISTDATA
			msgresp.xfer[msgresp.numItemOrInfo].flags = tmp->flags;
#endif
			msgresp.xfer[msgresp.numItemOrInfo].ptr = tmp->ptr;
			msgresp.xfer[msgresp.numItemOrInfo].size = tmp->size;
			msgresp.xfer[msgresp.numItemOrInfo].ra = tmp->ra;
			msgresp.xfer[msgresp.numItemOrInfo].tid = tmp->tid;
			msgresp.xfer[msgresp.numItemOrInfo].seconds = tmp->seconds;
			msgresp.numItemOrInfo++;
			hpwHdr.totalEntries++;
			dbg(PRINT_INFO, "%s: Processed %d unwalked items for sending\n", __FUNCTION__, msgresp.numItemOrInfo);
			if (MAX_MSG_XFER <= msgresp.numItemOrInfo)
			{
				dbg(PRINT_INFO, "%s: Going to send %d unwalked items\n", __FUNCTION__, msgresp.numItemOrInfo);
				if (tmp->next)
				{
					msgresp.numItemOrInfo |= HEAPWALK_ITEM_CONTN;
				}
				else
				{ // Set End of list
					msgresp.numItemOrInfo |= HEAPWALK_ENDOF_LIST;
				}
#if defined(PREPEND_LISTDATA) && defined(ENABLE_STATISTICS)
				msgresp.totalHeapSize = totalHeapSize;
				msgresp.totalOverhead = totalOverhead;
				msgresp.heapPeakSize = heapPeakSize;
				msgresp.heapPeakedAt = heapPeakedAt;
#endif
				dbg(PRINT_INFO, "%s: Sending %d unwalked items\n", __FUNCTION__, (msgresp.numItemOrInfo&(~(0x20000000))));
				if (NULL == fname) {
					mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);
				}
				else {
					FILE *fp = fopen(fname, "ab");
					if (NULL != fp) {
						if (!fwrite((void *)&msgresp, sizeof(msg_resp), 1, fp)) {
							dbg(PRINT_ERROR, "%s:%d: store error [%s]\n", __FUNCTION__, __LINE__, strerror(errno));
						}
						fclose(fp);
					}
				}
				msgresp.numItemOrInfo = 0;
			}
			// prev = tmp;
			tmp = tmp->next;
		}
		if (msgresp.numItemOrInfo)
		{
			dbg(PRINT_NOISE, "%s: Sending final set of %d unwalked items\n", __FUNCTION__, msgresp.numItemOrInfo);
			msgresp.numItemOrInfo |= HEAPWALK_ENDOF_LIST;
#if defined(PREPEND_LISTDATA) && defined(ENABLE_STATISTICS)
			msgresp.totalHeapSize = totalHeapSize;
			msgresp.totalOverhead = totalOverhead;
			msgresp.heapPeakSize = heapPeakSize;
			msgresp.heapPeakedAt = heapPeakedAt;
#endif
			// TODO: for now send the full size. sync receiver to accept for lesser size
			if (NULL == fname) {
				mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);
			}
			else {
				FILE *fp = fopen(fname, "ab");
				if (NULL != fp) {
					if (!fwrite((void *)&msgresp, sizeof(msg_resp), 1, fp)) {
						dbg(PRINT_ERROR, "%s:%d: store error [%s]\n", __FUNCTION__, __LINE__, strerror(errno));
					}
					fclose(fp);
				}
			}
		}

#ifdef MAINTAIN_SINGLE_LIST
		hpwmemhead = NULL;
#else
		// if (prev)
		{
			if (wmemtail)
			{
				wmemtail->next = memhead;
				// wmemtail = prev;
#ifdef PREPEND_LISTDATA
				if (memhead)
				{
					memhead->prev = wmemtail;
				}
#endif
				if (memtail)
				{
					wmemtail = memtail;
				}
			}
			else
			{
				wmemhead = memhead;
				wmemtail = memtail;
			}

			memhead = memtail = NULL;
		}
#endif
	}
	else
	{
		dbg(PRINT_INFO, "No new allocations\n");
#if defined(PREPEND_LISTDATA) && defined(ENABLE_STATISTICS)
		msgresp.totalHeapSize = totalHeapSize;
		msgresp.totalOverhead = totalOverhead;
		msgresp.heapPeakSize = heapPeakSize;
		msgresp.heapPeakedAt = heapPeakedAt;
#endif
		// msgresp.numItemOrInfo = HEAPWALK_EMPTY;
		if (NULL == fname) {
			mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);
		}
		else {
			FILE *fp = fopen(fname, "ab");
			if (NULL != fp) {
				if (!fwrite((void *)&msgresp, sizeof(msg_resp), 1, fp)) {
					dbg(PRINT_ERROR, "%s:%d: store error [%s]\n", __FUNCTION__, __LINE__, strerror(errno));
				}
				fclose(fp);
			}
		}
	}
	pthread_mutex_unlock(&lock);
	/*
	if (hpwHdr.totalEntries) {
		fp = fopen(fname, "ab");
		if (NULL != fp) {
			fseek(fp, 0, SEEK_SET);
			if (!fwrite((void *)&hpwHdr, sizeof(hpwHdr), 1, fp)) {
				dbg(PRINT_ERROR, "%s:%d: store error [%s]\n", __FUNCTION__, __LINE__, strerror(errno));
			}
			fclose(fp);
		}
	}
	*/
	dbg(PRINT_INFO, "3Total entries: %lu, version: %u\n", hpwHdr.totalEntries, hpwHdr.version);
	dbg(PRINT_NOISE, "%s: Exit\n", __FUNCTION__);
	dbg(PRINT_INFO, "%s-: hpfmemhead [%p] hpwmemhead [%p] hpfmemtail [%p]\n", __FUNCTION__, hpfmemhead, hpwmemhead, hpfmemtail);
}

void sndPthreadIntercepts(mqd_t mqsend)
{
	int once = 1;
	if (once && (NULL == sendPthreadIntercept_fnptr)) {
		dbg(PRINT_INFO, "Looking for sendPthreadIntercept\n");
	       	sendPthreadIntercept_fnptr = dlsym(RTLD_DEFAULT, "sendPthreadIntercept"); /* Throw error when all reads fail */
		once--;
	}
	if (sendPthreadIntercept_fnptr) {
		sendPthreadIntercept_fnptr(mqsend);
	}
	else {
		dbg(PRINT_ERROR, "sendPthreadIntercept not found\n");
		msg_resp msgresp;
		msgresp.numItemOrInfo = HEAPWALK_EMPTY;
		mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);
	}
}

int storeSmaps(char *suffix)
{
	char mmapTmpArray[1024]; /* Used to read entries from /proc/pid/smaps...big enough to hold large entries */
	FILE *fpMmap;
	if (suffix) {
		sprintf(mmapTmpArray, "/tmp/smaps_%d_%s.txt", getpid(), suffix);
	}
	else {
		sprintf(mmapTmpArray, "/tmp/smaps_%d.txt", getpid());
	}
	fpMmap = fopen(mmapTmpArray, "w");

	if (NULL != fpMmap) {
		static unsigned skipToEntry = 0, skipToSize = 0, skipToRss = 0, skipToRollover = 0; 
		unsigned skippedToLearn = 0;

		time_t timenow = time(NULL);
		//struct tm *tmNow = localtime(&timenow); /* This allocates 15/17/20 bytes randomly creaing confusion during heapwalk. so avoid by using epoch
		//if (0 == strftime(mmapTmpArray, sizeof(mmapTmpArray), "YYYY_MM_DD HH_MM_SS %Y_%m_%d %H_%M_%S", tmNow)) {
			sprintf(mmapTmpArray, "Date in epoch: %lu secs", timenow); // see if this is warned in 32 bit systems..
		//}
		fprintf(fpMmap, "%s\n", mmapTmpArray);
		sprintf(mmapTmpArray, "/proc/%u/smaps", getpid());
		FILE *smap = fopen(mmapTmpArray, "r");
		if (smap) {
			unsigned lines_To_skip = skipToEntry;
			unsigned skipped = 1; // Tracks current skips
			unsigned expect_entry = 1, expect_size = 0, expect_rss = 0;

			MMAP_info tmp = {0};
#if defined(PROCESS_PAGEMAP_IN_LIB)
			sprintf(mmapTmpArray, "/proc/%d/pagemap", getpid());
			FILE *pagemap = fopen(mmapTmpArray, "rb");
			sprintf(mmapTmpArray, "/tmp/hpp_lib_%d.txt", getpid());
			FILE *pagemapFp = fopen(mmapTmpArray, "w");
			if (NULL == pagemapFp) {
				PRINT("%s: Open failed, errno %d [%s]\n", mmapTmpArray, errno, strerror(errno));
				fclose(smap);
				fclose(fpMmap);
				return 1;
			}
			unsigned long startAddr = 0, endAddr = 0;
#endif
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
								lines_To_skip = skipToRollover;
								skipped = 1;
								expect_entry = 1;
								expect_rss = 0;
								fprintf(fpMmap, "%lx-%lx %u %u %s %s\n", tmp.startAddress, tmp.endAddress, tmp.size, tmp.rss, tmp.perm, tmp.entryName);
#if defined(PROCESS_PAGEMAP_IN_LIB)
								if ('\0' == tmp.entryName[0] || strstr(tmp.entryName, "stack") || strstr(tmp.entryName, "heap")) {
									startAddr = tmp.startAddress;endAddr = tmp.endAddress;
								}
#endif
								memset(&tmp, 0, sizeof(MMAP_info));
							}
							else {
								dbg(PRINT_ERROR,"Error Reading Rss from %s", mmapTmpArray);
							}
						} 
					}
					else {
						dbg(PRINT_NOISE, "Skipping, skipped vs lines_To_skip %u:%u, line %s", skipped, lines_To_skip, mmapTmpArray);
					}
				}
				else { // Learn here
					if (!skipToEntry) {
						if (3 <= sscanf(mmapTmpArray, "%lx-%lx %s %*x %*s %*u %s", &tmp.startAddress, &tmp.endAddress, tmp.perm, tmp.entryName)) {
							tmp.size = tmp.endAddress - tmp.startAddress;
							skipToEntry = skippedToLearn + 1;
							skippedToLearn = 0;
						}
						else {
							skippedToLearn++;
						}
					}
					else if (!skipToSize) {
						if (sscanf(mmapTmpArray, "Size: %u kB", &tmp.size)) {
							skipToSize = skippedToLearn + 1;
							skippedToLearn = 0;
						}
						else {
							skippedToLearn++;
						}
					}
					else if (!skipToRss) {
						if (sscanf(mmapTmpArray, "Rss: %u kB", &tmp.rss)) {
							skipToRss = skippedToLearn + 1;
							skippedToLearn = 0;
							fprintf(fpMmap, "%lx-%lx %u %u %s %s\n", tmp.startAddress, tmp.endAddress, tmp.size, tmp.rss, tmp.perm, tmp.entryName);
#if defined(PROCESS_PAGEMAP_IN_LIB)
							if ('\0' == tmp.entryName[0] || strstr(tmp.entryName, "stack") || strstr(tmp.entryName, "heap")) {
								startAddr = tmp.startAddress;endAddr = tmp.endAddress;
							}
#endif
							memset(&tmp, 0, sizeof(MMAP_info));
						}
						else {
							skippedToLearn++;
						}
					}
					else if (!skipToRollover) {
                                                if (3 <= sscanf(mmapTmpArray, "%lx-%lx %s %*x %*s %*u %s", &tmp.startAddress, &tmp.endAddress, tmp.perm, tmp.entryName)) {
                                                        tmp.size = tmp.endAddress - tmp.startAddress;
                                                        skipToRollover = skippedToLearn + 1;
							skippedToLearn = 0;
							expect_size = 1;
							expect_entry = 0;
                                                }
                                                else {
                                                        skippedToLearn++;
                                                }
                                        }
					else {
						PRINT("%s:%d: Shouldn't get here..read line %s\n", __FUNCTION__, __LINE__, mmapTmpArray);
					}
				}
#if defined(PROCESS_PAGEMAP_IN_LIB)
				if (startAddr) {
					unsigned long long pageinfo;
					while (startAddr < endAddr) {
						fseek(pagemap, (startAddr / sysconf(_SC_PAGE_SIZE)) * 8, SEEK_SET);
						fread(&pageinfo, 8, 1, pagemap);
						sprintf(mmapTmpArray, "0x%lx 0x%llx\n", startAddr, pageinfo);
						fwrite(mmapTmpArray, 1, strlen(mmapTmpArray), pagemapFp);
						startAddr += sysconf(_SC_PAGE_SIZE);
					}
					startAddr = 0;
				}
#endif
			}
			fclose(smap);
#if defined(PROCESS_PAGEMAP_IN_LIB)
			fclose(pagemapFp);
			fclose(pagemap);
#endif
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
/* Stores either full or incremental walk into /tmp/hp(f)_pid_[suffix].data */
int saveHeapwalk(char *suffix)
{
	char *heapwalkFile;

	if (NULL == suffix || '\0' == suffix[0]) {
		heapwalkFile = alloca(32);
		sprintf(heapwalkFile, "/tmp/hpf_%d.dat", getpid());
	}
	else {
		heapwalkFile = alloca(strlen(suffix) + 32);
		sprintf(heapwalkFile, "/tmp/hpf_%d_%s.dat", getpid(), suffix);
	}
	FILE *fpHWalk = fopen(heapwalkFile, "wb");
	if (NULL == fpHWalk)
	{
		dbg(PRINT_MUST, "%s open error, %s\n", heapwalkFile, strerror(errno));
		return 1;
	}
	fclose(fpHWalk);
	heapwalk(-1, 1, heapwalkFile);

	if (NULL == suffix || '\0' == suffix[0]) {
		sprintf(heapwalkFile, "/tmp/hp_%d.dat", getpid());
	}
	else {
		sprintf(heapwalkFile, "/tmp/hp_%d_%s.dat", getpid(), suffix);
	}
	fpHWalk = fopen(heapwalkFile, "wb");
	if (NULL == fpHWalk)
	{
		dbg(PRINT_MUST, "%s open error, %s\n", heapwalkFile, strerror(errno));
		return 1;
	}
	fclose(fpHWalk);
	heapwalk(-1, 0, heapwalkFile);
	storeSmaps(suffix);
	return 0;
}
void saveHeapWalkAtExit()
{
	dbg(PRINT_MUST, "Calling to save heapwalk\n");
	saveHeapwalk("atexit");
}

void registerAtExit(void)
{
	//PRINT("ATEXIT_MAX = %ld\n", sysconf(_SC_ATEXIT_MAX));
	if (atexit(saveHeapWalkAtExit)) {
		PRINT("%s: Error atexit()\n", __FUNCTION__);
	}
	else
		PRINT("%s: Registered atexit()\n", __FUNCTION__);
	return;
}
#else /* else of #ifdef OPTIMIZE_MQ_TRANSFER */

/**
 * @brief Walks the heap and transfers memory information to the message queue.
 *
 * This function transfers information about heap allocations to the provided message queue descriptor.
 *
 * @param mqsend The message queue descriptor to which memory information will be sent.
 */
void heapwalk(mqd_t mqsend)
{
	msg_resp msgresp;
	msgresp.seq = 1;

	dbg(PRINT_NOISE, "%s: Enter\n", __FUNCTION__);
	pthread_mutex_lock(&lock);
#ifdef MAINTAIN_SINGLE_LIST
	LIST *tmp = hpwmemhead;
#else
	LIST *tmp = memhead;
#endif
	if (NULL != tmp)
	{
		// LIST *prev = NULL;
		while (tmp)
		{
#ifdef PREPEND_LISTDATA
			snprintf(msgresp.msg, MQ_MSG_SIZE, "%p %u %p %u %ld%s", tmp->ptr, tmp->size, tmp->ra, tmp->tid, tmp->seconds, (tmp->flags & FLAGS_BIT1_REALLOC) ? " - R" : "");
#else
			// snprintf(msgresp.msg, MQ_MSG_SIZE, "Ptr: %p size: %u ra: %p tid: %ld time: %ld",
			snprintf(msgresp.msg, MQ_MSG_SIZE, "%p %u %p %u %ld", tmp->ptr, tmp->size, tmp->ra, tmp->tid, tmp->seconds);
#endif
			mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);
			msgresp.seq++;
			// prev = tmp;
			tmp = tmp->next;
		}

		/* Mark as walked allocation */
#ifndef MAINTAIN_SINGLE_LIST
		// if (prev)
		{
			if (wmemtail)
			{
				wmemtail->next = memhead;
				// wmemtail = prev;
#ifdef PREPEND_LISTDATA
				if (memhead)
				{
					memhead->prev = wmemtail;
				}
#endif
				if (memtail)
				{
					wmemtail = memtail;
				}
			}
			else
			{
				wmemhead = memhead;
				wmemtail = memtail;
			}

			// prev->next = wmemhead;
			// wmemhead = memhead;
			memhead = memtail = NULL;
		}
#else /* else of #ifndef MAINTAIN_SINGLE_LIST */
		hpwmemhead = NULL;
#endif
	}
	else
	{
		dbg(PRINT_INFO, "No new allocations\n");
		snprintf(msgresp.msg, MQ_MSG_SIZE, "No new allocations");
		mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);
	}
	dbg(PRINT_NOISE, "%s: Exit\n", __FUNCTION__);
	pthread_mutex_unlock(&lock);
}

/**
 * @brief Walks the full heap and transfers memory information to the message queue.
 *
 * This function transfers information about all heap allocations to the provided message queue descriptor.
 *
 * @param mqsend The message queue descriptor to which full memory information will be sent.
 */
void heapwalk_full(mqd_t mqsend)
{
	msg_resp msgresp;
	msgresp.seq = 0;
	pthread_mutex_lock(&lock);
	sprintf(msgresp.msg, "Already walked:");
	mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);

#ifdef MAINTAIN_SINGLE_LIST
	LIST *tmp = hpfmemhead;
#else
	LIST *tmp = memhead;
#endif
	if (NULL != tmp)
	{
		while (tmp)
		{
#ifdef MAINTAIN_SINGLE_LIST
			if (tmp == hpwmemhead)
			{
				sprintf(msgresp.msg, "New allocations:");
				mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);
				msgresp.seq = 0;
			}
#endif
			msgresp.seq++;
#ifdef PREPEND_LISTDATA
			snprintf(msgresp.msg, MQ_MSG_SIZE, "%p %u %p %u %ld%s", tmp->ptr, tmp->size, tmp->ra, tmp->tid, tmp->seconds, (tmp->flags & FLAGS_BIT1_REALLOC) ? " - R" : "");
#else
			snprintf(msgresp.msg, MQ_MSG_SIZE, "%p %u %p %u %ld", tmp->ptr, tmp->size, tmp->ra, tmp->tid, tmp->seconds);
#endif
			mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);
			tmp = tmp->next;
		}
	}
#ifndef MAINTAIN_SINGLE_LIST
	sprintf(msgresp.msg, "New allocations:");
	mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);
	heapwalk(mqsend);
#else
	hpwmemhead = NULL;
#endif
	pthread_mutex_unlock(&lock);
}

#endif

// TODO: not completed for prependITem to list and prepend list data combination
#if !defined(MAINTAIN_SINGLE_LIST) && !defined(PREPEND_LISTDATA) // Available only for normal list allocation type
/**
 * @brief Prepends an item to the list of allocations.
 *
 * This function adds a new allocation item to the start of the allocation list.
 *
 * @param item The allocated memory pointer.
 * @param size The size of the allocated memory.
 * @param ra The return address where the allocation was made.
 */
void prependItemToList(void *item, unsigned int size, void *ra)
{
	LIST *tmp;
	// pthread_mutex_lock(&lock);
	if (0 < gMemInitialized)
	{
		tmp = (LIST *)libc_malloc(sizeof(LIST));
	}
	else
	{
		pthread_mutex_lock(&lock);
		tmp = (LIST *)&gListInitialAlloc[gListInitIndex]; /* Aligned since not packed */
		gListInitIndex += (sizeof(LIST)); //  + sizeof(LIST)%4);
		pthread_mutex_unlock(&lock); /* Unlock mutex, Race condition if any will only affect the order */
		if (G_INITIAL_LIST_ALLOC_SIZE <= gListInitIndex)
		{
			/* Using printf or any function that will allocate memory inside or any variable number of arg fns
			Might get into loop and wait indefinetly on futex
			OR might trigger recursion and end up stack full
			*/
			// dbg(PRINT_MUST, "%s:%s:!!!!!! INCREASE gListInitialAlloc[], size now is %ld\n", __FILE__, __FUNCTION__, (long int)G_INITIAL_LIST_ALLOC_SIZE);
			fwrite(G_INITIAL_LIST_ALLOC_SIZE_ERROR, sizeof(G_INITIAL_LIST_ALLOC_SIZE_ERROR), 1, stderr);
			exit(1);
		}
	}
	tmp->ptr = item;
	tmp->size = size;
	tmp->ra = ra;
	tmp->tid = gettid();
	tmp->seconds = time(NULL);

	// dbg(PRINT_INFO, "%s: Prepend item %p\n", __FUNCTION__, item);
	pthread_mutex_lock(&lock);
	tmp->next = memhead;
	if (NULL == memtail)
		memtail = tmp;
	memhead = tmp;
	pthread_mutex_unlock(&lock);
}
#endif

#ifdef ENABLE_STATISTICS
/**
 * @brief Get the alignment value from the stored flag.
 *
 * This function extracts the original alignment value.
 *
 * @param alignment The calculated alignment value.
 * @return The original alignment value. Note: Will not work for invalid alignment like 0
 */
size_t __attribute__((visibility("internal"))) getAlignment(unsigned alignmentInOnesPos)
{
	size_t alignment = 1;
	for (unsigned int i = 1; i < alignmentInOnesPos; i++)
	{
		alignment <<= 1;
	}
	return alignment;
}

unsigned __attribute__((visibility("internal"))) isPowerOfTwo(size_t alignment) 
{
	unsigned noOfOnes = 0;
        while (alignment) {
		if (alignment & 0x1) {
			if (!noOfOnes) {
				noOfOnes++;
			}
			else {
				return 0;
			}
		}
                alignment = alignment >> 1;
        }
        return (noOfOnes)? 1 : 0;
}
#endif

#ifndef PREPEND_LISTDATA
/**
 * @brief Appends an item to the list of allocations.
 *
 * This function adds a new allocation item to the end of the allocation list.
 *
 * @param item The allocated memory pointer.
 * @param size The size of the allocated memory.
 * @param ra The return address where the allocation was made.
 */
void appendItemToList(void *item, unsigned int size, void *ra)
#else
/**
 * @brief Appends an item to the list of allocations (with flags).
 *
 * This function adds a new allocation item to the end of the allocation list and sets the flags.
 *
 * @param item The allocated memory pointer.
 * @param size The size of the allocated memory.
 * @param flags The flags indicating allocation type and alignment.
 * @param ra The return address where the allocation was made.
 */
void appendItemToList(void *item, unsigned int size, unsigned int flags, void *ra)
#endif
{
	LIST *listPtr;

#ifndef PREPEND_LISTDATA
	if (0 < gMemInitialized)
	{
		listPtr = (LIST *)libc_malloc(sizeof(LIST));
	}
	else
	{
		pthread_mutex_lock(&lock);
		listPtr = (LIST *)&gListInitialAlloc[gListInitIndex];
		gListInitIndex += (sizeof(LIST)); // + sizeof(LIST)%4);
		// Unlock mutex here. If there is race condition, it'll only affect the order
		pthread_mutex_unlock(&lock);
		if (G_INITIAL_LIST_ALLOC_SIZE <= gListInitIndex)
		{
			// Using printf or any function that will allocate memory inside or any variable number of arg fns
			// Might get into loop and wait indefinetly on futex
			// OR might trigger recursion and end up stack full
			// dbg(PRINT_MUST, "%s:%s:!!!!!! INCREASE gListInitialAlloc[], size now is %ld\n", __FILE__, __FUNCTION__, (long)G_INITIAL_LIST_ALLOC_SIZE);
			fwrite(G_INITIAL_LIST_ALLOC_SIZE_ERROR, sizeof(G_INITIAL_LIST_ALLOC_SIZE_ERROR), 1, stderr);
			exit(1);
		}
	}
	listPtr->ptr = item;
#else
	listPtr = (LIST *)((char *)item - sizeof(LIST));
	listPtr->ptr = item;
	listPtr->flags = 0xBEAD0000 | flags;
	listPtr->prev = NULL;
#endif
	listPtr->size = size;
	listPtr->ra = ra;
	listPtr->tid = gettid();
	listPtr->seconds = time(NULL);
	listPtr->next = NULL;

	pthread_mutex_lock(&lock);
#ifdef MAINTAIN_SINGLE_LIST
	if (hpfmemtail)
	{
		hpfmemtail->next = listPtr;
#ifdef PREPEND_LISTDATA
		listPtr->prev = hpfmemtail;
#endif
		hpfmemtail = listPtr;
		if (NULL == hpwmemhead)
		{
			hpwmemhead = listPtr;
		}
	}
	else
	{ // head should also be null
		hpfmemhead = hpfmemtail = hpwmemhead = listPtr;
	}
#else
	if (memtail)
	{
		memtail->next = listPtr;
#ifdef PREPEND_LISTDATA
		listPtr->prev = memtail;
#endif
		memtail = listPtr;
	}
	else
	{ // head should also be null
		memhead = memtail = listPtr;
	}

	/*if (NULL == memhead){
			memhead = listPtr;
	}else {
			LIST *trav = memhead->next;
			LIST *prev = memhead;
			while (trav){
					prev = trav;
					trav = trav->next;
			}
			prev->next = listPtr;
	}*/
#endif

#ifdef ENABLE_STATISTICS
	totalHeapSize += size;
	if (FLAGS_MEMALIGN > flags)
	{
		totalOverhead += sizeof(LIST);
	}
	/*** Taken care in common_aligned()
	else { // memaligned entry
		unsigned int alignment = getAlignment((flags >> 8));
		if (alignment > sizeof(LIST)) {
			totalOverhead += alignment;
		}
		else if ((sizeof(LIST) % alignment)) {
			totalOverhead += (sizeof(LIST) + (sizeof(LIST) % alignment));
		}
		else {
			totalOverhead += sizeof(LIST);
		}
	}
	***/
	if (heapPeakSize <= totalHeapSize) {
		heapPeakSize = totalHeapSize;
		heapPeakedAt = listPtr->seconds;
	}
#endif
	pthread_mutex_unlock(&lock);
}

#ifdef PREPEND_LISTDATA
/**
 * @brief Sets the alignment value for a given alignment.
 *
 * This function calculates One's position or n in 2^n.
 *
 * @param alignment The alignment value. 
 *        Should be a power of 2 (as well as multiple/greater than sizeof(void*)) 
 * @return The calculated alignment value.
 */
unsigned int __attribute__((visibility("internal"))) setAlignment(size_t alignment)
{
	unsigned alignmentInOnesPos = 0;
        while (alignment) {
                alignmentInOnesPos++;
                alignment = alignment >> 1;
        }
        return alignmentInOnesPos;
}

/****
 * if(alignment>sizeof LIST) then
 * 	Ptr = Alloc'd Address + alignment;
 * else if (modulus(sizeof(LIST), alignment) then
 * 	Ptr = Alloc'd Addr + sizeof(LIST) + modulus(sizeof(LIST), alignment);
 * else
 * 	Ptr = Alloc'd Addr + sizeof(LIST)
 ****/

/**
 * @brief Deletes an item from the list of allocations.
 *
 * This function removes an item from the list of allocations and adjusts pointers appropriately.
 *
 * @param item The item to be removed.
 * @return The adjusted pointer.
 */
void *deleteItemFromList(void *item)
{
	void *ptr;
	LIST *tmp = (LIST *)((char *)item - sizeof(LIST));

	if (0xBEAD0000 == (tmp->flags & 0xFFFF0000))
	{
#ifdef ENABLE_STATISTICS
		unsigned int overhead;
#endif
		unsigned int flags = tmp->flags & 0xFFFF;
		tmp->flags = 0xDEAD0000;
		if (FLAGS_MEMALIGN > flags)
		{ // Bit0 = 0 --> allocated from glibc heap 1 --> allocated from static buffer Bit1 = 0 --> malloc/calloc 1 --> realloc
			ptr = (void *)tmp;
#ifdef ENABLE_STATISTICS
			overhead = sizeof(LIST);
#endif
		}
		else
		{
			unsigned int alignment = getAlignment((flags >> 8));
			if (alignment > sizeof(LIST))
			{
				ptr = (char *)item - alignment;
#ifdef ENABLE_STATISTICS
				overhead = alignment;
#endif
			}
			else if ((sizeof(LIST) % alignment))
			{
				ptr = (char *)item - sizeof(LIST) - (sizeof(LIST) % alignment);
#ifdef ENABLE_STATISTICS
				overhead = sizeof(LIST) + (sizeof(LIST) % alignment);
#endif
			}
			else
			{
				ptr = (char *)item - sizeof(LIST);
#ifdef ENABLE_STATISTICS
				overhead = sizeof(LIST);
#endif
			}
		}

		if (flags & FLAGS_BIT0_STATIC_BUFF_ALLOCATED) { // Comes from static buffer, so no need for glibc free
			ptr = NULL;
		}
		pthread_mutex_lock(&lock);
#ifndef MAINTAIN_SINGLE_LIST
		if (tmp->prev)
		{
			tmp->prev->next = tmp->next;
			if (tmp->next)
			{
				tmp->next->prev = tmp->prev;
			}
			else
			{
				// this is the tail. Therefore update memtail or wmemtail
				if (memtail == tmp)
				{
					memtail = tmp->prev;
					if (memtail)
					{
						memtail->next = NULL;
					}
				}
				else if (wmemtail == tmp)
				{
					wmemtail = tmp->prev;
					if (wmemtail)
					{
						wmemtail->next = NULL;
					}
				}
				else
				{
					ptr = NULL;
					fwrite("deleteItemFromList:(w)memtail not matches\n", strlen("deleteItemFromList:(w)memtail not matches\n"), 1, stderr);
				}
			}
		}
		else
		{
			if (memhead == tmp)
			{
				if (memhead->next)
				{
					memhead = memhead->next;
					memhead->prev = NULL;
				}
				else
				{ // Only one item
					memhead = memtail = NULL;
				}
			}
			else if (wmemhead == tmp)
			{
				if (wmemhead->next)
				{
					wmemhead = wmemhead->next;
					wmemhead->prev = NULL;
				}
				else
				{ // Only one item
					wmemhead = wmemtail = NULL;
				}
			}
			else
			{
				ptr = NULL;
				fwrite("deleteItemFromList:(w)memhead not matches\n", strlen("deleteItemFromList:(w)memhead not matches\n"), 1, stderr);
			}
		}
#else /* else of ifndef MAINTAIN_SINGLE_LIST */
		if (tmp->prev)
		{ // This is not a head, so check for tail
			tmp->prev->next = tmp->next;
			if (tmp->next)
			{ // This is not a tail, but still check for walked head
				tmp->next->prev = tmp->prev;
				if (hpwmemhead == tmp)
				{
					hpwmemhead = tmp->next;
				}
			}
			else
			{
				// this is the tail. Therefore update tail
				if (hpfmemtail == tmp)
				{
					hpfmemtail = tmp->prev;
					if (hpfmemtail)
					{
						hpfmemtail->next = NULL;
					}
					else
					{ // List empty..
						hpfmemhead = NULL;
					}
				}
				else
				{
					ptr = NULL;
					fwrite("deleteItemFromList:(w)memtail not matches\n", strlen("deleteItemFromList:(w)memtail not matches\n"), 1, stderr);
				}
				if (hpwmemhead == tmp)
				{
					hpwmemhead = NULL;
				}
			}
		}
		else
		{ // This is the head
			if (hpfmemhead == tmp)
			{
				if (hpfmemhead->next)
				{ // More than 1 item
					hpfmemhead = hpfmemhead->next;
					hpfmemhead->prev = NULL;
				}
				else
				{ // Only one item
					hpfmemhead = hpfmemtail = NULL;
				}
			}
			else
			{
				ptr = NULL;
				fwrite("deleteItemFromList:(w)memhead not matches\n", strlen("deleteItemFromList:(w)memhead not matches\n"), 1, stderr);
			}
			if (hpwmemhead == tmp)
			{
				hpwmemhead = hpwmemhead->next;
			}
		}
#endif /* End of ifndef MAINTAIN_SINGLE_LIST */

#ifdef ENABLE_STATISTICS
		totalHeapSize -= tmp->size; // Consider failed pointer size??
		totalOverhead -= overhead;
#endif
		pthread_mutex_unlock(&lock);
	}
	else
	{ /* Allocate & return start of the pointer no matter if its corrupted or not */
		fwrite("deleteItemFromList: Invalid ptr\n", strlen("deleteItemFromList: Invalid ptr\n"), 1, stderr);
		// TODO: search through the list and see where this pointer falls
		ptr = NULL;
	}
	return ptr;
}
#else /* else of #ifdef PREPEND_LISTDATA */

/**
 * @brief Deletes an item from the list of allocations.
 *
 * This function removes an item from a doubly-linked list of allocations and adjusts pointers accordingly.
 *
 * @param list The head of the list from which the item should be removed.
 * @param tail The tail of the list from which the item should be removed.
 * @param item The item to be removed.
 * @return 0 if successful; 1 if the item is not found.
 */
int deleteItemFromList(LIST **list, LIST **tail, void *item)
{
	int ret = 0;
	dbg(PRINT_INFO, "deleteItemFromList: checking %p for item %p\n", list, item);
	pthread_mutex_lock(&lock);
	if (NULL != *list)
	{
		LIST *tmp = *list;

		if (item != tmp->ptr)
		{
			LIST *prev;
			while (NULL != tmp && item != tmp->ptr)
			{
				prev = tmp;
				tmp = tmp->next;
			}
			if (NULL != tmp)
			{
				dbg(PRINT_INFO, "Removing %p, prev %p tmp %p\n", item, prev, tmp);
				prev->next = tmp->next;
				if (NULL == tmp->next)
				{ // Last item, therefore adjust tail
					*tail = prev;
				}
#ifdef MAINTAIN_SINGLE_LIST
				if (tmp == hpwmemhead)
				{
					hpwmemhead = tmp->next;
				}
#endif
				if ((gListInitialAlloc > (char *)tmp) ||
					((char *)(gListInitialAlloc + G_INITIAL_LIST_ALLOC_SIZE) < (char *)tmp))
				{
					libc_free(tmp);
				}
			}
			else
			{
				ret = 1;
			}
		}
		else
		{
			/* Adjust the head/whead (first item) */
#ifdef MAINTAIN_SINGLE_LIST
			if (hpwmemhead == *list)
			{
				hpwmemhead = hpwmemhead->next;
			}
#endif
			*list = (*list)->next;
			if (NULL == *list)
			{ /* Adjust tail (only one item) */
				*tail = NULL;
#ifdef MAINTAIN_SINGLE_LIST
				dbg(PRINT_INFO, "%s: Only one item..[%p][%p][%p]\n", __FUNCTION__, hpfmemhead, hpwmemhead, hpfmemtail);
				hpwmemhead = NULL;
#else
				dbg(PRINT_INFO, "%s: Only one item..[%p][%p][%p][%p]\n", __FUNCTION__, wmemhead, wmemtail, memhead, memtail);
#endif
			}
			if ((gListInitialAlloc > (char *)tmp) ||
				((char *)(gListInitialAlloc + G_INITIAL_LIST_ALLOC_SIZE) < (char *)tmp))
				libc_free(tmp);
		}
	}
	else
	{
		dbg(PRINT_INFO, "%s: List is null\n", __FUNCTION__);
		ret = 1;
	}
	pthread_mutex_unlock(&lock);
	return ret;
}
#endif

/**
 * @brief Allocates memory with tracking.
 *
 * This function allocates memory of the given size and tracks the allocation.
 *
 * @param __size The size of memory to be allocated.
 * @return The allocated memory pointer.
 */
__attribute__((visibility("default"))) void *malloc(size_t __size)
{
	/* Track this */
	void *p = NULL;
#ifdef PREPEND_LISTDATA
	__size += sizeof(LIST);
#endif
	void *ra = __builtin_return_address(0);
	if (0 < gMemInitialized)
	{
		p = libc_malloc_fnptr(__size);
	}
	else
	{
		mapInitialMemory();
#ifndef SELF_TEST
		if (-1 == gMemInitialized)
		{
			load_libc_functions(); /* Request to static buffer, Try once */
		}
#endif
		pthread_mutex_lock(&lock);
		p = (void *)&gInitialAlloc[gInitIndex];
		// TODO: Optimize
		if (__size % sizeof(void *))
		{
			gInitIndex += (__size + (sizeof(void *) - __size % sizeof(void *)));
		}
		else
		{
			gInitIndex += (__size);
		}
		pthread_mutex_unlock(&lock);
		if (G_INITIAL_ALLOC_SIZE <= gInitIndex)
		{
			fwrite(G_INITIAL_ALLOC_SIZE_ERROR, sizeof(G_INITIAL_ALLOC_SIZE_ERROR), 1, stderr);
			abort();
		}
#ifdef PREPEND_LISTDATA // Though redundant, it saves runtime overhead
		appendItemToList((char *)p + sizeof(LIST), __size - sizeof(LIST), FLAGS_BIT0_STATIC_BUFF_ALLOCATED, ra);
		return (void *)((char *)p + sizeof(LIST));
#endif
	}
	// TODO: NULL check is not done
#ifdef PREPEND_LISTDATA
	appendItemToList((char *)p + sizeof(LIST), __size - sizeof(LIST), FLAGS_BIT0_GLIBC_ALLOCATED, ra);
	return (void *)((char *)p + sizeof(LIST));
#else
	// prependItemToList(p, __size, 0, ra);
	appendItemToList(p, __size, ra);
	return p;
#endif
}

/**
 * @brief Allocates and clears memory with tracking.
 *
 * This function allocates memory for an array of the given number of elements, each of the given size,
 * and initializes all bytes in the allocated memory to zero. The allocation is then tracked for debugging.
 *
 * @param __nmemb The number of elements to be allocated.
 * @param __size The size of each element.
 * @return The allocated and zero-initialized memory pointer.
 */
__attribute__((visibility("default"))) void *calloc(size_t __nmemb, size_t __size)
{
	// track me
	void *p = NULL;
	void *ra = __builtin_return_address(0);
#ifdef PREPEND_LISTDATA
	/* Adjust the LIST structure size, doesn't matter, whether 2 * 5 is allocated or 1 * 10 */
	__size = (__nmemb * __size) + sizeof(LIST);
	__nmemb = 1;
#endif

	if (0 < gMemInitialized)
	{
		p = libc_calloc_fnptr(__nmemb, __size);
	}
	else
	{
		mapInitialMemory();
#ifndef SELF_TEST
		if (-1 == gMemInitialized)
		{
			load_libc_functions(); /* Request to static buffer, Try once */
		}
#endif
		pthread_mutex_lock(&lock);
		p = &gInitialAlloc[gInitIndex];
		// TODO: Optimize
		if ((__size * __nmemb) % sizeof(void *))
		{
			gInitIndex += (__size * __nmemb + (sizeof(void *) - (__size * __nmemb) % sizeof(void *)));
		}
		else
		{
			gInitIndex += (__size * __nmemb);
		}
		pthread_mutex_unlock(&lock);

		if (G_INITIAL_ALLOC_SIZE <= gInitIndex)
		{
			fwrite(G_INITIAL_ALLOC_SIZE_ERROR, sizeof(G_INITIAL_ALLOC_SIZE_ERROR), 1, stderr);
			abort();
		}
		memset(&gInitialAlloc[gInitIndex], 0, __nmemb * __size);
#ifdef PREPEND_LISTDATA // Though redundant, it saves runtime overhead
		appendItemToList((char *)p + sizeof(LIST), (__size - sizeof(LIST)), FLAGS_BIT0_STATIC_BUFF_ALLOCATED, ra);
		return (void *)((char *)p + sizeof(LIST));
#endif
	}
#ifdef PREPEND_LISTDATA
    /* Append item to the list and return adjusted pointer */
	appendItemToList((char *)p + sizeof(LIST), (__size - sizeof(LIST)), FLAGS_BIT0_GLIBC_ALLOCATED, ra);
	return (void *)((char *)p + sizeof(LIST));
#else
	// prependItemToList(p, __size*__nmemb, __nmemb, ra);
	appendItemToList(p, __size * __nmemb, ra);
	return p;
#endif
}

/**
 * @brief Reallocates memory with tracking.
 *
 * This function reallocates memory to a new size and preserves the content up to the lesser of the old and new sizes.
 * If the pointer is NULL, it behaves like malloc. The reallocation is then tracked for debugging.
 *
 * @param curPtr The current memory pointer to be reallocated.
 * @param newSize The new size to allocate.
 * @return The reallocated memory pointer.
 */
__attribute__((visibility("default"))) void *realloc(void *curPtr, size_t newSize)
{
	void *np;
	void *ra = __builtin_return_address(0);

#ifdef PREPEND_LISTDATA
	LIST *item;
	unsigned int oldsize;

	if (NULL != curPtr)
	{
		item = getItem(curPtr);
	       	if (NULL != deleteItemFromList(curPtr)) { // This is more likely
			if (newSize) {
				curPtr = item;
				oldsize = 0;
			}
			else {
				return libc_realloc_fnptr((void *)item, newSize);
			}
		}
		else {
			if (newSize) {
				// The previous allocation came from static buffer. So note down the size to copy back the contents
				oldsize = item->size + sizeof(LIST);
				curPtr = NULL;
			}
			else {
				return NULL;
			}
		}
	}
	else {
		oldsize = 0;
		item = NULL; // Not needed to set, but to make compiler happy since it's used below
	}

	newSize += sizeof(LIST);
	if (0 < gMemInitialized) {
		np = libc_realloc_fnptr(curPtr, newSize);
		//if (item && (item->flags & 0x1)) 
		if (oldsize) {
			memcpy(np, item, oldsize);
		}
	}
	else {
		mapInitialMemory();
#ifndef SELF_TEST
		if (-1 == gMemInitialized)
		{
			load_libc_functions(); /* Request to static buffer, Try once */
		}
#endif
		pthread_mutex_lock(&lock);
		np = (void *)&gInitialAlloc[gInitIndex];
		if (newSize % sizeof(void *))
		{
			gInitIndex += (newSize + sizeof(void *) - (newSize % sizeof(void *)));
		}
		else
		{
			gInitIndex += newSize;
		}
		pthread_mutex_unlock(&lock);
		if (G_INITIAL_ALLOC_SIZE <= gInitIndex)
		{
			fwrite(G_INITIAL_ALLOC_SIZE_ERROR, sizeof(G_INITIAL_ALLOC_SIZE_ERROR), 1, stderr);
			abort();
		}
		//if (item && (item->flags & 0x1)) 
		if (oldsize) {
			memcpy(np, item, oldsize); // FIXME: see if size needs to be checked..
		}
		appendItemToList((char *)np + sizeof(LIST), newSize - sizeof(LIST), FLAGS_BIT0_STATIC_BUFF_ALLOCATED|FLAGS_BIT1_REALLOC, ra);
		return (void *)((char *)np + sizeof(LIST));
	}
	appendItemToList((char *)np + sizeof(LIST), newSize - sizeof(LIST), FLAGS_BIT0_GLIBC_ALLOCATED|FLAGS_BIT1_REALLOC, ra);
	return (void *)((char *)np + sizeof(LIST));
#else
	/* check curPtr, it can be null, or pointer allocated earlier via malloc or calloc */
	LIST *item = (curPtr) ? getItem(curPtr) : NULL;
	unsigned int size = 0;

	if (NULL != curPtr)
	{
#if MAINTAIN_SINGLE_LIST
		if (deleteItemFromList(&hpfmemhead, &hpfmemtail, curPtr) && (0 < gMemInitialized))
#else
		if (deleteItemFromList(&wmemhead, &wmemtail, curPtr) && deleteItemFromList(&memhead, &memtail, curPtr) && (0 < gMemInitialized))
#endif
		{
			dbg(PRINT_ERROR, "%s: Delete failed for %p, probably a. double free? b. bug in list? c. corrupt?\n",
				__FUNCTION__, curPtr);
		}
		else
		{
			size += item->size;
		}
	}

	if (!newSize && curPtr)
	{
		if ((0 < gMemInitialized) && ((gInitialAlloc > (char *)item) || ((char *)(gInitialAlloc + G_INITIAL_ALLOC_SIZE) < (char *)item)))
		{
			libc_free_fnptr(curPtr);
		}
		return NULL;
	}

	// TODO: To be moved to general heap if initialized, see if curPtr allocated from gInitialAlloc..
	if ((0 < gMemInitialized) && ((NULL == curPtr) || (gInitialAlloc > (char *)curPtr) ||
								  ((char *)(gInitialAlloc + G_INITIAL_ALLOC_SIZE) < (char *)curPtr)))
	{
		/* During the previous allocation, since the start of the buffer was used for LIST, after reallocation, realloc is going to copy the whole
		to the new buffer. Remember, we are going to give the newly allocated pointer + LIST size to the application.
		Therefore there is no need to adjust the data before giving to realloc. */
		np = libc_realloc_fnptr(curPtr, newSize);
	}
	else
	{
		mapInitialMemory();
#ifndef SELF_TEST
		if (-1 == gMemInitialized)
		{
			load_libc_functions(); /* Request to static buffer, Try once */
		}
#endif
		pthread_mutex_lock(&lock);
		np = (void *)&gInitialAlloc[gInitIndex];
		if (newSize % sizeof(void *))
		{
			gInitIndex += (newSize + sizeof(void *) - (newSize % sizeof(void *)));
		}
		else
		{
			gInitIndex += newSize;
		}
		pthread_mutex_unlock(&lock);
		if (G_INITIAL_ALLOC_SIZE <= gInitIndex)
		{
			fwrite(G_INITIAL_ALLOC_SIZE_ERROR, sizeof(G_INITIAL_ALLOC_SIZE_ERROR), 1, stderr);
			abort();
		}
		if (NULL != curPtr)
		{
			memcpy(np, curPtr, size); // FIXME: see if size needs to be checked..
		}
	}
	// prependItemToList(np, totalsize, nmem, ra);
	appendItemToList(np, newSize, ra);
	return np;
#endif
}

/**
 * @brief Frees memory with tracking.
 *
 * This function frees the memory allocated and tracked earlier. It adjusts the tracking information accordingly.
 *
 * @param ptr The memory pointer to be freed.
 */
__attribute__((visibility("default"))) void free(void *ptr)
{
#ifdef PREPEND_LISTDATA
	if (ptr)
	{
		if (NULL != (ptr = deleteItemFromList(ptr)))
		{
			libc_free_fnptr(ptr);
		}
	}
#else
#ifdef MAINTAIN_SINGLE_LIST
	if (deleteItemFromList(&hpfmemhead, &hpfmemtail, ptr) && (0 < gMemInitialized))
#else
	if (deleteItemFromList(&wmemhead, &wmemtail, ptr) && deleteItemFromList(&memhead, &memtail, ptr) && (0 < gMemInitialized))
#endif // #ifdef MAINTAIN_SINGLE_LIST
	{
		dbg(PRINT_ERROR, "%s: List Delete failed for %p, probably a. double free? b. list bug? c. corrupt pointer?\n", __FUNCTION__, ptr);
	}
	if ((ptr != NULL) && ((gInitialAlloc > (char *)ptr) || ((char *)(gInitialAlloc + G_INITIAL_ALLOC_SIZE) < (char *)ptr)))
	{
		libc_free_fnptr(ptr);
	}
#endif
}

#if defined(USE_DEPRECATED_MEMALIGN) || defined(__USE_ISOC11) || defined(__USE_XOPEN2K)
/**
 * @brief Common memory alignment function.
 *
 * This function handles different types of aligned memory allocations.
 *
 * @param type The type of alignment function to use (memalign, posix_memalign, or aligned_alloc).
 * @param alignment The alignment value.
 * @param size The size of memory to be allocated.
 * @return The aligned memory pointer.
 */
void *common_memalign(int type, size_t alignment, size_t size, void *ra)
{
	// track me
	void *p = NULL;
	unsigned flags = setAlignment(alignment) << 8;
#ifdef PREPEND_LISTDATA
	/* 2 challenges.
	First, memalign'd address returns needs to hold LIST pointer as well, which is 32/64 bytes in 32-bit/64-bit compilers
	Placing the LIST pointer at the beginning of the memalign'd address doesn't guarantee the asked alignment
	Therefore to calculate the newSize, used the following formula
	          if(alignment > sizeof(LIST)) then
	              newSize = size + alignment;
	          else
	              newSize = size + sizeof(LIST) + modulus(sizeof(LIST),alignment);
	Second, when this pointer comes for free'ng, need to get the item address in the list.
	Free'ng pointer - sizeof LIST is used
	         if (alignment > sizeof(LIST)) then
	             Ptr = Alloc'd Address + alignment;
	         else if (modulus(sizeof(LIST), alignment) then
	             Ptr = Alloc'd Addr + sizeof(LIST) + modulus(sizeof(LIST), alignment);
	         else
	             Ptr = Alloc'd Addr + sizeof(LIST)
	*/
	size_t newSize;
	if (sizeof(LIST) < alignment)
	{
		// TODO, will anyone ask for MAX size_t ??
		newSize = size + alignment;
	}
	else
	{
		newSize = size + sizeof(LIST) + (sizeof(LIST) % alignment);
	}
#else
	size_t newSize = size;
#endif
	if (0 < gMemInitialized)
	{
#if defined(USE_DEPRECATED_MEMALIGN)
		if (1 == type)
		{
			p = libc_memalign_fnptr(alignment, newSize);
		}
#endif
#if defined(__USE_ISOC11)
		if (2 == type)
		{
			p = libc_aligned_alloc_fnptr(alignment, newSize);
		}
#endif
#if defined(__USE_XOPEN2K)
		if (3 == type)
		{
			if (0 != libc_posix_memalign_fnptr(&p, alignment, newSize))
			{
				p = NULL;
			}
		}
#endif
	}
	else
	{
		mapInitialMemory();
#ifndef SELF_TEST
		if (-1 == gMemInitialized)
		{
			load_libc_functions(); /* Request to static buffer, Try once */
		}
#endif
		pthread_mutex_lock(&lock);
		/* If gInitIndex == alignment, then below will unnecessarily add alignment. But that is rare */
		gInitIndex = gInitIndex + alignment - (gInitIndex % alignment);
		p = &gInitialAlloc[gInitIndex];
		if (newSize % sizeof(void *))
		{
			gInitIndex += (newSize + (sizeof(void *) - newSize % sizeof(void *)));
		}
		else
		{
			gInitIndex += (newSize);
		}
		pthread_mutex_unlock(&lock);
		if (G_INITIAL_ALLOC_SIZE <= gInitIndex)
		{
			fwrite(G_INITIAL_ALLOC_SIZE_ERROR, sizeof(G_INITIAL_ALLOC_SIZE_ERROR), 1, stderr);
			abort();
		}

		flags |= FLAGS_BIT0_STATIC_BUFF_ALLOCATED;
	}
#ifdef PREPEND_LISTDATA
	char *ptr;
	if (p)
	{
		ptr = (char *)p;
		if (alignment > sizeof(LIST))
		{
			ptr = ptr + alignment;
		}
		else if ((sizeof(LIST) % alignment))
		{
			ptr = ptr + sizeof(LIST) + (sizeof(LIST) % alignment);
		}
		else
		{
			ptr = ptr + sizeof(LIST);
		}
#ifdef ENABLE_STATISTICS
		pthread_mutex_lock(&lock);
		totalOverhead += (newSize - size);
		pthread_mutex_unlock(&lock);
#endif
		appendItemToList(ptr, size, flags, ra);
	}
	else
	{
		ptr = NULL;
	}
	return ptr;
#else
	// prependItemToList(p, size, 0, ra);
	appendItemToList(p, size, ra);
	return p;
#endif
}
#endif

#if defined(USE_DEPRECATED_MEMALIGN)
/**
 * @brief Allocates aligned memory with tracking.
 *
 * This function allocates memory with the specified alignment and size, and tracks the allocation.
 *
 * @param alignment The alignment value.
 * @param size The size of memory to be allocated.
 * @return The aligned memory pointer.
 */
__attribute__((visibility("default"))) void *memalign(size_t alignment, size_t size)
{
	void *ra = __builtin_return_address(0);
	return isPowerOfTwo(alignment)? common_memalign(1, alignment, size, ra) : NULL;
}
#endif

#if defined(__USE_ISOC11)
/**
 * @brief Allocates aligned memory with tracking (ISO C11).
 *
 * This function allocates memory with the specified alignment and size, and tracks the allocation.
 *
 * @param __alignment The alignment value.
 * @param __size The size of memory to be allocated.
 * @return The aligned memory pointer.
 */
__attribute__((visibility("default"))) void *aligned_alloc(size_t __alignment, size_t __size)
{
	void *ra = __builtin_return_address(0);
	return (!isPowerOfTwo(alignment) && !(__size%__alignment)) ? common_memalign(2, __alignment, __size, ra) : NULL;
}
#endif

#if defined(__USE_XOPEN2K)
/**
 * @brief Allocates aligned memory with tracking (POSIX).
 *
 * This function allocates memory with the specified alignment and size, and tracks the allocation.
 *
 * @param __memptr Pointer to the memory that will be allocated.
 * @param __alignment The alignment value.
 * @param __size The size of memory to be allocated.
 * @return 0 if successful; error code otherwise.
 */
__attribute__((visibility("default"))) int posix_memalign(void **__memptr, size_t __alignment, size_t __size)
{
	void *ra = __builtin_return_address(0);
	if ((sizeof(void*) <= alignment) && isPowerOfTwo(alignment)) {
		*__memptr = common_memalign(3, __alignment, __size, ra);
	}
	return (*__memptr)? 0 : EINVAL;
	 
}
#endif

/* Bypass tracking APIs */
/**
 * @brief Allocates memory using libc malloc.
 *
 * This function uses the libc malloc function to allocate memory.
 *
 * @param size The size of memory to be allocated.
 * @return The allocated memory pointer.
 */
void *libc_malloc(size_t size)
{
	return libc_malloc_fnptr(size);
}

/**
 * @brief Allocates and clears memory using libc calloc.
 *
 * This function uses the libc calloc function to allocate and clear memory.
 *
 * @param nmemb The number of elements to be allocated.
 * @param size The size of each element.
 * @return The allocated and cleared memory pointer.
 */
void *libc_calloc(size_t nmemb, size_t size)
{
	return libc_calloc_fnptr(nmemb, size);
}

/**
 * @brief Reallocates memory using libc realloc.
 *
 * This function uses the libc realloc function to reallocate memory.
 *
 * @param ptr The current memory pointer to be reallocated.
 * @param size The new size to allocate.
 * @return The reallocated memory pointer.
 */
void *libc_realloc(void *ptr, size_t size)
{
	return libc_realloc_fnptr(ptr, size);
}

/**
 * @brief Frees memory using libc free.
 *
 * This function uses the libc free function to free the allocated memory.
 *
 * @param ptr The memory pointer to be freed.
 */
void libc_free(void *ptr)
{
	libc_free_fnptr(ptr);
}

#if defined(USE_DEPRECATED_MEMALIGN)
/**
 * @brief Allocates aligned memory using libc memalign.
 *
 * This function uses the libc memalign function to allocate aligned memory.
 *
 * @param alignment The alignment value.
 * @param size The size of memory to be allocated.
 * @return The aligned memory pointer.
 */
void *libc_memalign(size_t alignment, size_t size)
{
	return libc_memalign_fnptr(alignment, size);
}
#endif

#if defined(__USE_XOPEN2K)
/**
 * @brief Allocates aligned memory using libc posix_memalign.
 *
 * This function uses the libc posix_memalign function to allocate aligned memory.
 *
 * @param memptr Pointer to the memory that will be allocated.
 * @param alignment The alignment value.
 * @param size The size of memory to be allocated.
 * @return 0 if successful; error code otherwise.
 */
int libc_posix_memalign(void **memptr, size_t alignment, size_t size)
{
	return libc_posix_memalign_fnptr(memptr, alignment, size);
}
#endif

#if defined(__USE_ISOC11)
/**
 * @brief Allocates aligned memory using libc aligned_alloc.
 *
 * This function uses the libc aligned_alloc function to allocate aligned memory.
 *
 * @param alignment The alignment value.
 * @param size The size of memory to be allocated.
 * @return The aligned memory pointer.
 */
void *libc_aligned_alloc(size_t alignment, size_t size)
{
	return libc_aligned_alloc_fnptr(alignment, size);
}
#endif
