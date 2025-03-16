#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>           /* For O_* constants */
#include <sys/stat.h>        /* For mode constants */
#include <mqueue.h>
#include <time.h>
#include <sys/types.h>

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
STATIC LIST *memhead, *memtail;
STATIC LIST *wmemhead, *wmemtail;
#endif

#ifdef ENABLE_STATISTICS
unsigned long totalHeapSize;
unsigned long totalOverhead;
#endif

#ifndef PREPEND_LISTDATA
#ifdef SELF_TEST
#define G_INITIAL_LIST_ALLOC_SIZE 1024*sizeof(LIST)
#else
#define G_INITIAL_LIST_ALLOC_SIZE 100*sizeof(LIST)
#endif
//#ifdef SELF_TEST
//char __attribute__ ((aligned (sizeof(void*)))) gListInitialAllocP[G_INITIAL_LIST_ALLOC_SIZE];
//#endif
char *gListInitialAlloc;
unsigned int gListInitIndex;
#define G_INITIAL_LIST_ALLOC_SIZE_ERROR "Increase G_INITIAL_LIST_ALLOC_SIZE\n"
#endif

#ifdef SELF_TEST
#define G_INITIAL_ALLOC_SIZE 4*1024*1024 // 4MB
#else
#define G_INITIAL_ALLOC_SIZE 2*1024*1024 // 2MB ??
#endif
//#ifdef SELF_TEST
//char __attribute__ ((aligned (4096))) gInitialAllocP[G_INITIAL_ALLOC_SIZE];
//#endif
char *gInitialAlloc;
unsigned int gInitIndex;
#define G_INITIAL_ALLOC_SIZE_ERROR "Increase G_INITIAL_ALLOC_SIZE\n"

#if !defined(DISABLE_DEBUG) && defined(DEBUG_RUNTIME)
int debug_level = 0;
void dbg(int level, const char *fmt, ...)
{
	va_list args;
	//char str[256];
	if ((level < debug_level) || (PRINT_MUST == level)) {
		va_start(args, fmt);
		//vsprintf(str, fmt, args);
		//printf("%d: %s", getpid(), str);
		// Below may have issue, like both prints might be intersected by some other thread's print!!
		if (PRINT_WALK != level) {
			printf("%d: ", getpid());
		}
		vprintf(fmt, args);
        	va_end(args);
	}
}
#endif

#include <sys/mman.h>
void mapInitialMemory() 
{
#ifndef PREPEND_LISTDATA
	if (NULL == gListInitialAlloc) {
		gListInitialAlloc = mmap(NULL, G_INITIAL_LIST_ALLOC_SIZE, PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
		if (gListInitialAlloc == MAP_FAILED) {
			fwrite("gListInitialAlloc mmap failed\n", strlen("gListInitialAlloc mmap failed\n"), 1, stderr);			
			exit(2);
		}
	}
#endif
	if (NULL == gInitialAlloc) {
		gInitialAlloc = mmap(NULL, G_INITIAL_ALLOC_SIZE, PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
		if (gInitialAlloc == MAP_FAILED) {
			fwrite("gInitialAlloc mmap failed\n", strlen("gInitialAlloc mmap failed\n"), 1, stderr);			
			exit(2);
		}
		// During self test, expect the entries to be present inside the same mmap.
#ifdef SELF_TEST
		memset(gInitialAlloc, 0, G_INITIAL_ALLOC_SIZE);
#endif
	}
}
static void * thread_start(void *arg)
{
	dbg(PRINT_INFO, "%s: Starting thread\n", __FUNCTION__);
#ifndef SELF_TEST
	dbg(PRINT_INFO, "%s: Starting thread: version %s\n", __FUNCTION__, versionString);
#endif
        mqd_t mq, mqsend;
        msg_cmd msgcmd;
	char mq_name[64];
	unsigned int prio;

        struct mq_attr mqattr = ((struct mq_attr){0, 3, sizeof(msg_cmd), 0, {0}});
	sprintf(mq_name, "/mq_wrapper_%d", getpid());
	// Create with read/write, so that memleakutil can open it for writing
	mq = mq_open(mq_name, O_CREAT | O_RDONLY, QUEUE_PERMISSION, &mqattr);
        if(0 > mq) {
                dbg(PRINT_MUST, "Error, cannot open/create the queue: %s.error %s\n", mq_name, strerror(errno));
		dbg(PRINT_MSGQ, "Trying to open without O_CREAT flag..\n");
		mq = mq_open(mq_name, O_RDONLY, QUEUE_PERMISSION, &mqattr);
		if (0 > mq) {
                	dbg(PRINT_MUST, "Error, cannot open the queue: %s.error %s\n", mq_name, strerror(errno));
			exit(1);
		}
        }

	/* Need to purge for new invocations alone. If fork'd, then it affects during selftest!! */
	if (-1 == gMemInitialized) 
	{
		// purge initial msg if any. Useful during process restarts
		struct   timespec tm;
		msgcmd.cmd = -1;
		do {
			if (-1 != msgcmd.cmd) {
				dbg(PRINT_MSGQ, "Purging cmd %d by thread %d\n", msgcmd.cmd, gettid());
			}
			clock_gettime(CLOCK_REALTIME, &tm);
			tm.tv_sec += 1;
		}while(0 <= mq_timedreceive(mq, (char*)&msgcmd, sizeof(msg_cmd), &prio,&tm));
		if (ETIMEDOUT != errno) {
			dbg(PRINT_ERROR, "Error, mq_receive: %s.\n", strerror(errno));
		}
	}
	else if (1 == gMemInitialized) { // Since it's a fork'd process, then let's mark all previous allocations as marked.
	       //heapwalkMarkall();
	}

        while (1){
		int msgsize = mq_receive(mq, (char*)&msgcmd, sizeof(msg_cmd), &prio);
		if (msgsize >= 0){
			dbg(PRINT_MSGQ, "Received %d: cmd %d\n",msgsize, msgcmd.cmd);
#if 0
			switch (msgcmd.cmd) {
				case 1:
				case 2:
				case 3:
					mqsend = mq_open("/mq_util", O_WRONLY);
					if(mqsend < 0) {
						dbg(PRINT_ERROR, "Error, cannot open the queue: %s.\n", strerror(errno));
					} else {
					}
			}
#endif
			if (HEAPWALK_INCREMENT == msgcmd.cmd){
				mqsend = mq_open("/mq_util", O_WRONLY);
				if(mqsend < 0) {
					dbg(PRINT_ERROR, "Error, cannot open the queue: %s.\n", 
							strerror(errno));
				}else{
					dbg(PRINT_MSGQ, "%s: sending on mq %d\n", __FUNCTION__, mqsend);
#ifdef OPTIMIZE_MQ_TRANSFER
					heapwalk(mqsend, 0);
#else
        				msg_resp msgresp;
					heapwalk(mqsend);
					msgresp.seq = -1;
#if defined(PREPEND_LISTDATA) && defined(ENABLE_STATISTICS)
					snprintf(msgresp.msg, MQ_MSG_SIZE, "TotalHeapSize %lu Bytes + Tool Overhead %lu", totalHeapSize, totalOverhead); 
#endif
					mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);
#endif
					dbg(PRINT_MSGQ, "%s: sent on mq %d\n", __FUNCTION__, mqsend);
					mq_close(mqsend);
				}
			}
			else if (HEAPWALK_FULL == msgcmd.cmd){
				mqsend = mq_open("/mq_util", O_WRONLY);
				if(mqsend < 0) {
					dbg(PRINT_ERROR, "Error, cannot open the queue: %s.\n", 
							strerror(errno));
				}else{
					dbg(PRINT_MSGQ, "%s: sending on mq %d\n", __FUNCTION__, mqsend);
#ifdef OPTIMIZE_MQ_TRANSFER
					heapwalk(mqsend, 1);
#else
        				msg_resp msgresp;
					heapwalk_full(mqsend);
					msgresp.seq = -1;
#if defined(PREPEND_LISTDATA) && defined(ENABLE_STATISTICS)
					snprintf(msgresp.msg, MQ_MSG_SIZE, "TotalHeapSize %lu Bytes + Tool Overhead %lu", totalHeapSize, totalOverhead); 
#endif
					mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);
#endif
					dbg(PRINT_MSGQ, "%s: sent on mq %d\n", __FUNCTION__, mqsend);
					mq_close(mqsend);
				}
			}
			else if (HEAPWALK_MMAP_ENTRIES == msgcmd.cmd) {
				mqsend = mq_open("/mq_util", O_WRONLY);
				if(mqsend < 0) {
					dbg(PRINT_ERROR, "Error, cannot open the queue: %s.\n",	strerror(errno));
				}else{
					dbg(PRINT_MSGQ, "%s: sending on mq %d\n", __FUNCTION__, mqsend);
#ifdef OPTIMIZE_MQ_TRANSFER
					heapwalk(mqsend, 1);
#else
					dbg(PRINT_MUST, "HEAPWALK_MMAP_ENTRIES supported only with OPTIMIZE_MQ_TRANSFER\n");
#endif
				}

			}
			else if (HEAPWALK_MARKALL == msgcmd.cmd){
				dbg(PRINT_MSGQ, "Calling heapwalkMarkall(). cmd %d\n", msgcmd.cmd);
				heapwalkMarkall();
			}
			else if (HEAPWALK_RESET_MARKED == msgcmd.cmd){
				dbg(PRINT_MSGQ, "Calling heapwalkReset(). cmd %d\n", msgcmd.cmd);
				heapwalkReset();
			}
			else
				dbg(PRINT_ERROR, "Invalid cmd 0x%x received\n", msgcmd.cmd);
		}else {
			dbg(PRINT_ERROR, "Error, mq_receive: %s.\n", 
					strerror(errno));
			sleep(1);
		}
	}
	mq_close(mq);
	mq_unlink(mq_name);
}

typedef void *(*calloc_type)(size_t, size_t);
calloc_type libc_calloc_fnptr = NULL;

typedef void *(*malloc_type)(size_t);
malloc_type libc_malloc_fnptr = NULL;

typedef void (*free_type)(void *);
free_type libc_free_fnptr = NULL;

typedef void *(*realloc_type)(void *, size_t);
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

__attribute__((constructor)) void heapwalk_thread_start()
{
	pthread_attr_t attr;
	pthread_t ptd;
	pthread_mutexattr_t mutexattr;
	pthread_mutexattr_init(&mutexattr);
	pthread_mutexattr_settype(&mutexattr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&lock, &mutexattr);
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 8*1024);

	pthread_create(&ptd, &attr, &thread_start, NULL);
	//pthread_join(ptd, NULL);
	//pthread_mutex_destroy(&lock);
	pthread_attr_destroy(&attr);
}

// Watch out for forked processes. In that case, create a thread in child process for serving heapwalk.
// register_atfork is similar to pthread_atfork. 
extern int __register_atfork (void (*__prepare) (void),
                              void (*__parent) (void),
                              void (*__child) (void),
                              void *dso_handle);
extern void* __dso_handle __attribute__ ((__weak__));
static void run_in_child_context(void) {
        dbg(PRINT_ERROR, "%s: pid %d\n", __FUNCTION__, getpid());
	heapwalk_thread_start();
}

#ifndef SELF_TEST
__attribute__((constructor)) 
#endif
void load_libc_functions()
{
	fwrite("load_libc_functions+\n", strlen("load_libc_functions+\n"), 1, stderr);			

	// Need exclusion? Don't think so it's important, since redoing doesn't affect
	if (-1 == gMemInitialized){
		gMemInitialized = 0;
		//fwrite("dlsym\n", strlen("dlsym\n"), 1, stderr);
#ifdef __GLIBC__
		libc_calloc_fnptr = dlsym(RTLD_NEXT, "__libc_calloc");
		// We can error out here if above isn't successful, but let's all at once and say what other reads have failed
		libc_malloc_fnptr = dlsym(RTLD_NEXT, "__libc_malloc");
		libc_free_fnptr = dlsym(RTLD_NEXT, "__libc_free");
		libc_realloc_fnptr = dlsym(RTLD_NEXT, "__libc_realloc");
#if defined(__USE_XOPEN2K)
		libc_posix_memalign_fnptr = dlsym(RTLD_NEXT, "__libc_posix_memalign");;
#endif
#if defined(__USE_ISOC11)
		libc_aligned_alloc_fnptr = dlsym(RTLD_NEXT, "__libc_aligned_alloc");;
#endif
#if defined(USE_DEPRECATED_MEMALIGN)	
		libc_memalign_fnptr = dlsym(RTLD_NEXT, "__libc_memalign");
#endif
		fwrite("dlsym'd\n", strlen("dlsym'd\n"), 1, stderr);			

		if ( (NULL == libc_malloc_fnptr) || (NULL == libc_calloc_fnptr) || (NULL == libc_free_fnptr) || (NULL == libc_realloc_fnptr) 
#if defined(USE_DEPRECATED_MEMALIGN)
			|| (NULL == libc_memalign_fnptr) 
#endif
#if defined(__USE_XOPEN2K)
			|| (NULL == libc_posix_memalign_fnptr)
#endif
#if defined(__USE_ISOC11)
			|| (NULL == libc_aligned_alloc_fnptr)
#endif
		) {
			fwrite("Couldn't load symbols from libc", strlen("Couldn't load symbols from libc"), 1, stderr);			
#if defined(USE_DEPRECATED_MEMALIGN)
			if (NULL == libc_memalign_fnptr) {
				fwrite(": memalign not loaded\n", strlen(": memalign not loaded\n"), 1, stderr);
			}
#endif
#if defined(__USE_XOPEN2K)
			if (NULL == libc_posix_memalign_fnptr) {
				fwrite(": memalign not loaded\n", strlen(": posix_memalign not loaded\n"), 1, stderr);
			}
#endif
#if defined(__USE_ISOC11)
			if (NULL == libc_aligned_alloc_fnptr) {
				fwrite(": memalign not loaded\n", strlen(": aligned_alloc not loaded\n"), 1, stderr);
			}
#endif
			abort();
		}else {
			if (__register_atfork(NULL, NULL, run_in_child_context, __dso_handle) != 0) {
				fwrite("__register_atfork", strlen("__register_atfork"), 1, stderr);			
				dbg(PRINT_ERROR, "%s: Error __register_atfork\n", __FUNCTION__);
				abort();
			}
			gMemInitialized = 1;
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
		if (libcLibrary) {
			libc_calloc_fnptr = dlsym(libcLibrary, "calloc");
			libc_malloc_fnptr = dlsym(libcLibrary, "malloc");
			libc_free_fnptr = dlsym(libcLibrary, "free");
			libc_realloc_fnptr = dlsym(libcLibrary, "realloc");
			libc_memalign_fnptr = dlsym(libcLibrary, "memalign");
			dlclose(libcLibrary);
		}
		else {
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
	dbg(PRINT_MUST, "%s: Enter from pid:tid %d:%d\n", __FUNCTION__, getpid(),gettid());
	dbg(PRINT_MUST, "%s:\ngMemInitialized %d\n", __FUNCTION__, gMemInitialized);
	if (0 < gMemInitialized) {
               dbg(PRINT_MUST, "Loaded symbols from libc, malloc:calloc:free:realloc [%p][%p][%p][%p]\n",
                       libc_malloc_fnptr, libc_calloc_fnptr, libc_free_fnptr, libc_realloc_fnptr);
#if defined(USE_DEPRECATED_MEMALIGN)
               if (NULL != libc_memalign_fnptr) {
                       dbg(PRINT_MUST, "%s: Loaded symbols from libc, memalign [%p]\n", __FUNCTION__, libc_memalign_fnptr);
               }
#endif
#if defined(__USE_XOPEN2K)
               if (NULL != libc_posix_memalign_fnptr) {
                       dbg(PRINT_MUST, "%s: Loaded symbols from libc, posix_memalign [%p]\n", __FUNCTION__, libc_posix_memalign_fnptr);
               }
#endif
#if defined(__USE_ISOC11)                              
               if (NULL != libc_aligned_alloc_fnptr) {
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
        while (tmp) {
		dbg(PRINT_MUST, "Ptr: %p size: %u ra: %p tid: %ld time: %ld\n", tmp->ptr, tmp->size, tmp->ra,(long) tmp->tid,(long) tmp->seconds );
		tmp = tmp->next;
	}
	dbg(PRINT_MUST, "New Allocations:\n");
	tmp = memhead;
        while (tmp) {
		dbg(PRINT_MUST, "Ptr: %p size: %u ra: %p tid: %ld time: %ld\n", 
				tmp->ptr, tmp->size, tmp->ra,(long) tmp->tid,(long) tmp->seconds);
                tmp = tmp->next;
        }
#else
	LIST *tmp = hpfmemhead;
	dbg(PRINT_MUST, "Ptr\tsize\tra\ttid\ttime\n");
	while (tmp) {
		if (tmp == hpwmemhead) {
			dbg(PRINT_MUST, "New Allocations:\n");
		}
		dbg(PRINT_MUST, "%p\t%u\t%p\t%ld\t%ld\n", tmp->ptr, tmp->size, tmp->ra,(long) tmp->tid,(long) tmp->seconds );
                tmp = tmp->next;
	}
#endif
	dbg(PRINT_MUST, "\n");
	pthread_mutex_unlock(&lock);
}
#endif

LIST* getItem(void *ptr)
{
	LIST *ret = NULL;
	pthread_mutex_lock(&lock);
#ifndef PREPEND_LISTDATA
#ifndef MAINTAIN_SINGLE_LIST
        LIST *tmp = memhead;
        while (tmp) {
                if (ptr == tmp->ptr){
			pthread_mutex_unlock(&lock);
                        return tmp;
		}
                tmp = tmp->next;
        }
	tmp = wmemhead;
        while (tmp) {
                if (ptr == tmp->ptr){
                        ret = tmp;
			break;
		}
                tmp = tmp->next;
        }
#else // else of #ifndef MAINTAIN_SINGLE_LIST
	LIST *tmp = hpfmemhead;
	while (tmp) {
		if (ptr == tmp->ptr) {
			pthread_mutex_unlock(&lock);
			return tmp;
		}
		tmp = tmp->next;
	}
#endif // #ifndef MAINTAIN_SINGLE_LIST
#else // else of #ifndef PREPEND_LISTDATA
	ret = (LIST*) ((char*) ptr - sizeof(LIST));
//#if defined(PREPEND_LISTDATA)
	// TODO ... Add unlikely attribute
	if (0xBEAD0000 != (ret->flags & 0xFFFF0000)) {
		ret = NULL;
	}
//#endif
#endif
	pthread_mutex_unlock(&lock);
        return ret;
}

__attribute__ ((weak)) void heapwalkMarkall()
{
	pthread_mutex_lock(&lock);
#ifdef MAINTAIN_SINGLE_LIST
	hpwmemhead = NULL;
#else
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
#endif
	pthread_mutex_unlock(&lock);
}

void heapwalkReset()
{
	// protect
	pthread_mutex_lock(&lock);

#ifdef MAINTAIN_SINGLE_LIST
	hpwmemhead = hpfmemhead;
#else
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
	/*LIST *tmp = wmemhead;
	LIST *tail = wmemhead;
        while (tmp) {
		tail = tmp;
                tmp = tmp->next;
        }
	if (tail){
		tail->next = memhead;
		memhead = wmemhead;
		wmemhead = NULL;
	}*/
#endif
	pthread_mutex_unlock(&lock);
}
#ifdef OPTIMIZE_MQ_TRANSFER
// It's a duplication of heapwalk(), but this way neat and easier to handle OPTIMIZE_MQ_TRANSFER
void heapwalk(mqd_t mqsend, bool walkAll)
{
	msg_resp msgresp;
	LIST *tmp;

	dbg(PRINT_NOISE, "%s: Enter\n", __FUNCTION__);	
	msgresp.numItemOrInfo = HEAPWALK_EMPTY;
	pthread_mutex_lock(&lock);
	if (walkAll) {
#ifdef MAINTAIN_SINGLE_LIST
		tmp = hpfmemhead;
#else
		tmp = wmemhead;
#endif
		if (NULL != tmp){
			while (tmp) {
#ifdef MAINTAIN_SINGLE_LIST
				if (tmp == hpwmemhead) {
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
				if (MAX_MSG_XFER <= msgresp.numItemOrInfo) {
					if (tmp->next) {
						msgresp.numItemOrInfo |= HEAPWALK_ITEM_CONTN;
					}
					else { // Incidentally last item...set the end of list
						msgresp.numItemOrInfo |= HEAPWALK_ENDOF_LIST;
#if defined(PREPEND_LISTDATA) && defined(ENABLE_STATISTICS)
						msgresp.totalHeapSize = totalHeapSize;
						msgresp.totalOverhead = totalOverhead;
#endif
					}
					mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);
					msgresp.numItemOrInfo = 0;
				}
				tmp = tmp->next;
			}
			if (msgresp.numItemOrInfo) {
				msgresp.numItemOrInfo |= HEAPWALK_ENDOF_LIST;
#if defined(PREPEND_LISTDATA) && defined(ENABLE_STATISTICS)
				msgresp.totalHeapSize = totalHeapSize;
				msgresp.totalOverhead = totalOverhead;
#endif
				// TODO: for now send the full size. sync receiver to accept for lesser size
				mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);
			}
		}
		else {
			// msgresp.numItemOrInfo = HEAPWALK_EMPTY; Initialized already
			mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);
		}
	}

	msgresp.numItemOrInfo = HEAPWALK_EMPTY;
#ifdef MAINTAIN_SINGLE_LIST
	tmp = hpwmemhead;
#else
	tmp = memhead;
#endif
	if (NULL != tmp){
        	while (tmp) {
			//memcpy(((char*)msgresp.xfer + msgresp_xfer_index), tmp, sizeof(LISTxfer));
#ifdef PREPEND_LISTDATA
			msgresp.xfer[msgresp.numItemOrInfo].flags = tmp->flags;
#endif
			msgresp.xfer[msgresp.numItemOrInfo].ptr = tmp->ptr;
			msgresp.xfer[msgresp.numItemOrInfo].size = tmp->size;
			msgresp.xfer[msgresp.numItemOrInfo].ra = tmp->ra;
			msgresp.xfer[msgresp.numItemOrInfo].tid = tmp->tid;
			msgresp.xfer[msgresp.numItemOrInfo].seconds = tmp->seconds;
			msgresp.numItemOrInfo++;
			if (MAX_MSG_XFER <= msgresp.numItemOrInfo) {
				if (tmp->next) {
						msgresp.numItemOrInfo |= HEAPWALK_ITEM_CONTN;
				}
				else { // Incidentally end of list
					msgresp.numItemOrInfo |= HEAPWALK_ENDOF_LIST;
#if defined(PREPEND_LISTDATA) && defined(ENABLE_STATISTICS)
					msgresp.totalHeapSize = totalHeapSize;
					msgresp.totalOverhead = totalOverhead;
#endif
				}
				mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);
				msgresp.numItemOrInfo = 0;
			}
			//prev = tmp;
                	tmp = tmp->next;
        	}
		if (msgresp.numItemOrInfo) {
			msgresp.numItemOrInfo |= HEAPWALK_ENDOF_LIST;
#if defined(PREPEND_LISTDATA) && defined(ENABLE_STATISTICS)
			msgresp.totalHeapSize = totalHeapSize;
			msgresp.totalOverhead = totalOverhead;
#endif
			// TODO: for now send the full size. sync receiver to accept for lesser size
			mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);
		}

#ifdef MAINTAIN_SINGLE_LIST
		hpwmemhead = NULL;
#else
		//if (prev)
		{
			if (wmemtail){
				wmemtail->next = memhead;
				//wmemtail = prev;
#ifdef PREPEND_LISTDATA
				if (memhead) {
					memhead->prev = wmemtail;
				}
#endif
				if (memtail) {
					wmemtail = memtail;
				}
			}
			else {
				wmemhead = memhead;
				wmemtail = memtail;
			}

			memhead = memtail = NULL;
		}
#endif
	}
	else {
		dbg(PRINT_INFO, "No new allocations\n");
#if defined(PREPEND_LISTDATA) && defined(ENABLE_STATISTICS)
		msgresp.totalHeapSize = totalHeapSize;
		msgresp.totalOverhead = totalOverhead;
#endif
		//msgresp.numItemOrInfo = HEAPWALK_EMPTY;
		mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);
	}
	pthread_mutex_unlock(&lock);
	dbg(PRINT_NOISE, "%s: Exit\n", __FUNCTION__);
}
#else // else of #ifdef OPTIMIZE_MQ_TRANSFER
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
	if (NULL != tmp){
		//LIST *prev = NULL;
        	while (tmp) {
#ifdef PREPEND_LISTDATA
			snprintf(msgresp.msg, MQ_MSG_SIZE, "%p %u %p %u %ld%s", 
					tmp->ptr, tmp->size, tmp->ra, tmp->tid, tmp->seconds, (tmp->flags & 0x1)? " - R" : "");
#else
			//snprintf(msgresp.msg, MQ_MSG_SIZE, "Ptr: %p size: %u ra: %p tid: %ld time: %ld", 
			snprintf(msgresp.msg, MQ_MSG_SIZE, "%p %u %p %u %ld", 
					tmp->ptr, tmp->size, tmp->ra, tmp->tid, tmp->seconds);
#endif
			mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);
			msgresp.seq++;

			//prev = tmp;
                	tmp = tmp->next;
        	}

		// Mark the walked allocations
#ifndef MAINTAIN_SINGLE_LIST
		//if (prev)
		{
			if (wmemtail){
				wmemtail->next = memhead;
				//wmemtail = prev;
#ifdef PREPEND_LISTDATA
				if (memhead) {
					memhead->prev = wmemtail;
				}
#endif
				if (memtail) {
					wmemtail = memtail;
				}
			}
			else {
				wmemhead = memhead;
				wmemtail = memtail;
			}

			//prev->next = wmemhead;
			//wmemhead = memhead;
			memhead = memtail = NULL;
		}
#else // else of #ifndef MAINTAIN_SINGLE_LIST
		hpwmemhead = NULL;
#endif
	}else {
		dbg(PRINT_INFO, "No new allocations\n");
		snprintf(msgresp.msg, MQ_MSG_SIZE, "No new allocations");
		mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);
	}
	dbg(PRINT_NOISE, "%s: Exit\n", __FUNCTION__);
	pthread_mutex_unlock(&lock);
}
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
	if (NULL != tmp){
        	while (tmp) {
#ifdef MAINTAIN_SINGLE_LIST
			if (tmp == hpwmemhead) {
				sprintf(msgresp.msg, "New allocations:");
				mq_send(mqsend, (const char *)&msgresp, sizeof(msg_resp), 0);
				msgresp.seq = 0;
			}
#endif
			msgresp.seq++;
#ifdef PREPEND_LISTDATA
			snprintf(msgresp.msg, MQ_MSG_SIZE, "%p %u %p %u %ld%s", 
					tmp->ptr, tmp->size, tmp->ra, tmp->tid, tmp->seconds, (tmp->flags & 0x1)? " - R" : "");
#else
			snprintf(msgresp.msg, MQ_MSG_SIZE, "%p %u %p %u %ld", tmp->ptr, tmp->size, tmp->ra, tmp->tid, tmp->seconds );
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

// TODO, not completed for prependITem to list and prepend list data combination
#if !defined(MAINTAIN_SINGLE_LIST) && !defined(PREPEND_LISTDATA) // Available only for normal list allocation type
void prependItemToList(void *item, unsigned int size, void *ra)
{
        LIST *tmp;
	//pthread_mutex_lock(&lock);
	if (0 < gMemInitialized){
		tmp = (LIST*)libc_malloc(sizeof(LIST));
	}else {
		pthread_mutex_lock(&lock);
		tmp = (LIST*)&gListInitialAlloc[gListInitIndex];
		// Since LIST is not packed, it'll be aligned.
		gListInitIndex += (sizeof(LIST)); //  + sizeof(LIST)%4);
		// Unlock mutex here. If there is race condition, it'll only affect the order
		pthread_mutex_unlock(&lock);
		if (G_INITIAL_LIST_ALLOC_SIZE <= gListInitIndex) {
			// Using printf or any function that will allocate memory inside or any variable number of arg fns
			// Might get into loop and wait indefinetly on futex
			// OR might trigger recursion and end up stack full
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

	//dbg(PRINT_INFO, "%s: Prepend item %p\n", __FUNCTION__, item);
	pthread_mutex_lock(&lock);
	tmp->next = memhead;
	if (NULL == memtail)
		memtail = tmp;
        memhead = tmp;
	pthread_mutex_unlock(&lock);
}
#endif

#ifndef PREPEND_LISTDATA
void appendItemToList(void *item, unsigned int size, void *ra)
#else
void appendItemToList(void *item, unsigned int size, unsigned int flags, void *ra)
#endif
{
        LIST *listPtr;

#ifndef PREPEND_LISTDATA
	if (0 < gMemInitialized){
		listPtr = (LIST*)libc_malloc(sizeof(LIST));
	}else {
		pthread_mutex_lock(&lock);
		listPtr = (LIST*)&gListInitialAlloc[gListInitIndex];
		gListInitIndex += (sizeof(LIST)); // + sizeof(LIST)%4);
		// Unlock mutex here. If there is race condition, it'll only affect the order
		pthread_mutex_unlock(&lock);
		if (G_INITIAL_LIST_ALLOC_SIZE <= gListInitIndex) {
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
	listPtr = (LIST*)((char*)item - sizeof(LIST));
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
	if (hpfmemtail) {
		hpfmemtail->next = listPtr;
#ifdef PREPEND_LISTDATA
		listPtr->prev = hpfmemtail;
#endif
		hpfmemtail = listPtr;
		if (NULL == hpwmemhead) {
			hpwmemhead = listPtr;
		}
	}
	else { // head should also be null
		hpfmemhead = hpfmemtail = hpwmemhead = listPtr;
	}
#else
	if (memtail) {
		memtail->next = listPtr;
#ifdef PREPEND_LISTDATA
		listPtr->prev = memtail;
#endif
		memtail = listPtr;
	}
	else { // head should also be null
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
	if (2 > flags) {
		totalOverhead += sizeof(LIST);
	}
#endif
	pthread_mutex_unlock(&lock);
}

#ifdef PREPEND_LISTDATA
unsigned int setAlignment(unsigned int alignment)
{
        for (unsigned int i=1;i <= 16; i++) {
                alignment = alignment >> 1;
                if ( !(alignment) ) {
                        return i;
                }
        }
        return 0;
}

/****
 * if(alignment>sizeof LIST) then 
 * 	Ptr = Alloc'd Address + alignment; 
 * else if (modulus(sizeof(LIST), alignment) then 
 * 	Ptr = Alloc'd Addr + sizeof(LIST) + modulus(sizeof(LIST), alignment); 
 * else 
 * 	Ptr = Alloc'd Addr + sizeof(LIST)
 ****/
void *deleteItemFromList(void *item)
{
	void *ptr;
	LIST *tmp = (LIST*)((char*)item - sizeof(LIST));
	if (0xBEAD0000 == (tmp->flags & 0xFFFF0000)) {
#ifdef ENABLE_STATISTICS
		unsigned int overhead;
#endif
		unsigned int flags = tmp->flags & 0xFFFF;
		tmp->flags = 0xDEAD0000;
		if (2 > flags) { // 0 --> malloc/calloc 1 --> realloc
			ptr = (void*)tmp;
#ifdef ENABLE_STATISTICS
			overhead = sizeof(LIST);
#endif
		} else {
			unsigned int alignment = 1 << (flags-1);
			if (alignment > sizeof(LIST)) {
				ptr = (char*)item - alignment;
#ifdef ENABLE_STATISTICS
				overhead = alignment;
#endif
			} else if ((sizeof(LIST) % alignment)) {
				ptr = (char*)item - sizeof(LIST) - (sizeof(LIST) % alignment);
#ifdef ENABLE_STATISTICS
				overhead = sizeof(LIST) + (sizeof(LIST) % alignment);
#endif
			} else {
				ptr = (char*)item - sizeof(LIST);
#ifdef ENABLE_STATISTICS
				overhead = sizeof(LIST);
#endif
			}
		}
		pthread_mutex_lock(&lock);
#ifndef MAINTAIN_SINGLE_LIST
		if (tmp->prev) {
			tmp->prev->next = tmp->next;
			if (tmp->next) {
				tmp->next->prev = tmp->prev;
			}else {
				// this is the tail. Therefore update memtail or wmemtail
				if (memtail == tmp) {
					memtail = tmp->prev;
					if (memtail) {
						memtail->next = NULL;
					}
				} else if (wmemtail == tmp) {
					wmemtail = tmp->prev;
					if (wmemtail) {
						wmemtail->next = NULL;
					}
				}
				else {
					ptr = NULL;
					fwrite("deleteItemFromList:(w)memtail not matches\n", strlen("deleteItemFromList:(w)memtail not matches\n"), 1, stderr);
				}
			}
		}
		else {
			if (memhead == tmp) {
				if (memhead->next) {
					memhead = memhead->next;
					memhead->prev = NULL;
				} else { // Only one item
					memhead = memtail = NULL;
				}
			} else if (wmemhead == tmp) {
				if (wmemhead->next) {
					wmemhead = wmemhead->next;
					wmemhead->prev = NULL;
				} else { // Only one item
					wmemhead = wmemtail = NULL;
				}
			} else {
				ptr = NULL;
				fwrite("deleteItemFromList:(w)memhead not matches\n", strlen("deleteItemFromList:(w)memhead not matches\n"), 1, stderr);
			}
		}
#else
		if (tmp->prev) { // This is not a head, so check for tail
			tmp->prev->next = tmp->next;
			if (tmp->next) { // This is not a tail, but still check for walked head
				tmp->next->prev = tmp->prev;
				if (hpwmemhead == tmp) {
					hpwmemhead = tmp->next;
				}
			}else {
				// this is the tail. Therefore update tail
				if (hpfmemtail == tmp) {
					hpfmemtail = tmp->prev;
					if (hpfmemtail) {
						hpfmemtail->next = NULL;
					} else { // List empty..
						hpfmemhead = NULL;
					}
				}
				else {
					ptr = NULL;
					fwrite("deleteItemFromList:(w)memtail not matches\n", strlen("deleteItemFromList:(w)memtail not matches\n"), 1, stderr);
				}
				if (hpwmemhead == tmp) {
					hpwmemhead = NULL;
				}
			}
		}
		else { // This is the head
			if (hpfmemhead == tmp) {
				if (hpfmemhead->next) { // More than 1 item
					hpfmemhead = hpfmemhead->next;
					hpfmemhead->prev = NULL;
				} else { // Only one item
					hpfmemhead = hpfmemtail = NULL;
				}
			} else {
				ptr = NULL;
				fwrite("deleteItemFromList:(w)memhead not matches\n", strlen("deleteItemFromList:(w)memhead not matches\n"), 1, stderr);
			}
			if (hpwmemhead == tmp) {
				hpwmemhead = hpwmemhead->next;
			}
		}
#endif	
#ifdef ENABLE_STATISTICS
		totalHeapSize -= tmp->size; // Consider failed pointer size??
		totalOverhead -= overhead;
#endif
		pthread_mutex_unlock(&lock);

	}
	else { // Either corrupted or not the start of the pointer allocated and returned
		fwrite("deleteItemFromList: Invalid ptr\n", strlen("deleteItemFromList: Invalid ptr\n"), 1, stderr);
		// TODO, search through the list and see where this pointer falls...TODO ... JAGA
		ptr = NULL;
	}
	return ptr;
}
#else // else of #ifdef PREPEND_LISTDATA
int deleteItemFromList(LIST **list, LIST **tail, void *item)
{
	int ret = 0;
	dbg(PRINT_INFO, "deleteItemFromList: checking %p for item %p\n", list, item);
	pthread_mutex_lock(&lock);
	if (NULL != *list){
		LIST *tmp = *list;

		if (item != tmp->ptr){
			LIST *prev;
			while (NULL != tmp && item != tmp->ptr){
				prev = tmp;
				tmp = tmp->next;
			}
			if (NULL != tmp){
				dbg(PRINT_INFO, "Removing %p, prev %p tmp %p\n", item, prev, tmp);
				prev->next = tmp->next;
				if (NULL == tmp->next) { // Last item, therefore adjust tail 
					*tail = prev;
				}
#ifdef MAINTAIN_SINGLE_LIST
				if (tmp == hpwmemhead) {
					hpwmemhead = tmp->next;
				}
#endif
				if ((gListInitialAlloc > (char*)tmp) || 
					((char*)(gListInitialAlloc+G_INITIAL_LIST_ALLOC_SIZE) < (char*)tmp)){
					libc_free(tmp);
                                }
			}
			else {
				ret = 1;
			}
		}else {
		// First item itself, need to adjust the head/whead
#ifdef MAINTAIN_SINGLE_LIST
			if (hpwmemhead == *list) {
				hpwmemhead = hpwmemhead->next;
			}
#endif
			*list = (*list)->next;
			if (NULL == *list) { // Only one item, therefore adjust tail
				*tail = NULL;
#ifdef MAINTAIN_SINGLE_LIST
				dbg(PRINT_INFO, "%s: Only one item..[%p][%p][%p]\n", __FUNCTION__, hpfmemhead, hpwmemhead, hpfmemtail);
				hpwmemhead = NULL;
#else
				dbg(PRINT_INFO, "%s: Only one item..[%p][%p][%p][%p]\n", __FUNCTION__, wmemhead, wmemtail, memhead, memtail);
#endif
			}
			if ((gListInitialAlloc > (char*)tmp) || 
					((char*)(gListInitialAlloc+G_INITIAL_LIST_ALLOC_SIZE) < (char*)tmp))
				libc_free(tmp);
		}
	}
	else {
		dbg(PRINT_INFO, "%s: List is null\n", __FUNCTION__);
		ret = 1;
	}
	pthread_mutex_unlock(&lock);
	return ret;
}
#endif

__attribute__ ((visibility("default"))) void *malloc(size_t __size)
{
	// track me
	void *p = NULL;
#ifdef PREPEND_LISTDATA
	__size += sizeof(LIST);
#endif
  
	if (0 < gMemInitialized) {
		p = libc_malloc_fnptr(__size);
	}else {
		mapInitialMemory();
#ifndef SELF_TEST
		if (-1 == gMemInitialized) {
			load_libc_functions(); // Try once, but give the current request from static buffer
		}
#endif
		pthread_mutex_lock(&lock);
		p = (void*)&gInitialAlloc[gInitIndex];
		// TODO: Optimize??
		if (__size%sizeof(void*)) {
			gInitIndex += (__size + (sizeof(void*) - __size%sizeof(void*)));
		} else {
			gInitIndex += (__size);
		}		
		pthread_mutex_unlock(&lock);
		if (G_INITIAL_ALLOC_SIZE <= gInitIndex) {
			fwrite(G_INITIAL_ALLOC_SIZE_ERROR, sizeof(G_INITIAL_ALLOC_SIZE_ERROR), 1, stderr);
			abort();
		}
	}
	// We are not checking for NULL. But should be fine, bcos we can track that as well TODO TODO really...
#ifdef PREPEND_LISTDATA
	appendItemToList((char*)p + sizeof(LIST), __size - sizeof(LIST), 0, __builtin_return_address(0));
	return (void*)((char*)p + sizeof(LIST));
#else
	//prependItemToList(p, __size, 0, __builtin_return_address(0));
	appendItemToList(p, __size, __builtin_return_address(0));
	return p;
#endif
}
__attribute__ ((visibility("default"))) void *calloc(size_t __nmemb, size_t __size)
{
        // track me
	void *p = NULL;
#ifdef PREPEND_LISTDATA
	// Doesn't matter, whether 2 * 5 is allocated or 1 * 10
	__size = (__nmemb * __size) + sizeof(LIST);
	__nmemb = 1;
#endif
	
	if (0 < gMemInitialized) {
		p = libc_calloc_fnptr(__nmemb, __size);
	}else {
		mapInitialMemory();
#ifndef SELF_TEST
		if (-1 == gMemInitialized) {
			load_libc_functions(); // Try once, but give the current request from static buffer
		}
#endif
		pthread_mutex_lock(&lock);
		p = &gInitialAlloc[gInitIndex];
		// TODO: Optimize??
		if ((__size * __nmemb) % sizeof(void*)) {
			gInitIndex += (__size*__nmemb + (sizeof(void*) - (__size*__nmemb)%sizeof(void*)));
		} else {
			gInitIndex += (__size*__nmemb);
		}
		pthread_mutex_unlock(&lock);
		
		if (G_INITIAL_ALLOC_SIZE <= gInitIndex) {
			fwrite(G_INITIAL_ALLOC_SIZE_ERROR, sizeof(G_INITIAL_ALLOC_SIZE_ERROR), 1, stderr);
			abort();
		}
		memset(&gInitialAlloc[gInitIndex], 0, __nmemb*__size);
	}
#ifdef PREPEND_LISTDATA
	appendItemToList((char*)p + sizeof(LIST), (__size - sizeof(LIST)), 0, __builtin_return_address(0));
	return (void*)((char*)p + sizeof(LIST));
#else	
	//prependItemToList(p, __size*__nmemb, __nmemb, __builtin_return_address(0));
	appendItemToList(p, __size*__nmemb, __builtin_return_address(0));
	return p;
#endif
}

__attribute__ ((visibility("default"))) void *realloc(void *curPtr, size_t newSize)
{
	void *np;
        
	// curPtr can be null, OR pointer allocated earlier via malloc or calloc
	LIST *item = (curPtr)? getItem(curPtr):NULL;
	unsigned int size = 0;

	if (NULL != curPtr) {
#ifdef PREPEND_LISTDATA
		// This increase is for realloc to copy the entire previously allocated buffer into newly allocated pointer
		//size += sizeof(LIST);
		size = sizeof(LIST);
		if (NULL == (item = deleteItemFromList(curPtr)))
#elif MAINTAIN_SINGLE_LIST
		if (deleteItemFromList(&hpfmemhead, &hpfmemtail, curPtr) && (0 < gMemInitialized))
#else
		if (deleteItemFromList(&wmemhead, &wmemtail, curPtr) && deleteItemFromList(&memhead, &memtail, curPtr) && (0 < gMemInitialized))
#endif
		{
			dbg(PRINT_ERROR, "%s: Delete failed for %p, probably bug in list? corrupt?\n",
					__FUNCTION__, curPtr);
		}
		else {
			size += item->size;
		}
	} 
	//else {
	//	item = NULL;
	//}

	if (!newSize && curPtr) {
		if ((0 < gMemInitialized) && ((gInitialAlloc > (char*)item) || ((char*)(gInitialAlloc+G_INITIAL_ALLOC_SIZE) < (char*)item))) {
#ifdef PREPEND_LISTDATA
			libc_free_fnptr((void*)item);
#else
			libc_free_fnptr(curPtr);
#endif
		}
		return NULL;
	}
#ifdef PREPEND_LISTDATA
	newSize += sizeof(LIST);
	curPtr = (void*)item;
#endif

	// TODO....JAGA to be moved to general heap if initialized
	// see if curPtr allocated from gInitialAlloc..
	if ((0 < gMemInitialized) && ((NULL == curPtr) || (gInitialAlloc > (char*)curPtr) || 
			((char*)(gInitialAlloc+G_INITIAL_ALLOC_SIZE) < (char*)curPtr))) {
		// During the previous allocation, since the start of the buffer was used for LIST, after reallocation, realloc is going to copy the whole
		// to the new buffer. Remember, we are going to give the newly allocated pointer + LIST size to the application.
		// Therefore there is no need to adjust the data before giving to realloc.
		np = libc_realloc_fnptr(curPtr, newSize);
	}else {	
		mapInitialMemory();
#ifndef SELF_TEST
		if (-1 == gMemInitialized) {
			load_libc_functions(); // Try once, but give the current request from static buffer
		}
#endif
		pthread_mutex_lock(&lock);
		np = (void*)&gInitialAlloc[gInitIndex];
		if (newSize % sizeof(void*)) {
			gInitIndex += (newSize + sizeof(void*) - (newSize % sizeof(void*)));
		}else {
			gInitIndex += newSize;
		}
		pthread_mutex_unlock(&lock);
		if (G_INITIAL_ALLOC_SIZE <= gInitIndex) {
			fwrite(G_INITIAL_ALLOC_SIZE_ERROR, sizeof(G_INITIAL_ALLOC_SIZE_ERROR), 1, stderr);
			abort();
		}
		if (NULL != curPtr) {
			memcpy(np, curPtr, size); // FIXME: see if size needs to be checked..
		}
	}
#ifdef PREPEND_LISTDATA
	appendItemToList((char*)np + sizeof(LIST), newSize - sizeof(LIST), 1, __builtin_return_address(0));
	return (void*)((char*)np + sizeof(LIST));
#else
	//prependItemToList(np, totalsize, nmem, __builtin_return_address(0));
	appendItemToList(np, newSize, __builtin_return_address(0));
	return np;
#endif
}

__attribute__ ((visibility("default"))) void free(void* ptr)
{
#ifdef PREPEND_LISTDATA
	if (ptr) {
		if (NULL == (ptr = deleteItemFromList(ptr))) {
			dbg(PRINT_ERROR, "%s: List Delete failed for %p list bug? corrupt pointer?\n",__FUNCTION__, ptr);
		}
		else if ((0 < gMemInitialized) && ((gInitialAlloc > (char*)ptr) || ((char*)(gInitialAlloc+G_INITIAL_ALLOC_SIZE) < (char*)ptr))) {
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
		dbg(PRINT_ERROR, "%s: List Delete failed for %p list bug? corrupt pointer?\n",__FUNCTION__, ptr);
	}
	if ((ptr != NULL) && ((gInitialAlloc > (char*)ptr) || ((char*)(gInitialAlloc+G_INITIAL_ALLOC_SIZE) < (char*)ptr))) {
		libc_free_fnptr(ptr);
	}
#endif
}

#if defined(USE_DEPRECATED_MEMALIGN) || defined(__USE_ISOC11) || defined(__USE_XOPEN2K)
void *common_memalign(int type, size_t alignment, size_t size)
{
	// track me
	void *p = NULL;
#ifdef PREPEND_LISTDATA
	// 2 challenges. 
	// First, memalign'd address returns needs to hold LIST pointer as well, which is 32/64 bytes in 32-bit/64-bit compilers
	// Placing the LIST pointer at the beginning of the memalign'd address doesn't guarantee the asked alignment
	// Therefore to calculate the newSize, used the following formula
	//           if(alignment > sizeof(LIST)) then 
	//               newSize = size + alignment; 
	//           else
	//               newSize = size + sizeof(LIST) + modulus(sizeof(LIST),alignment);
	//
	// Second, when this pointer comes for free'ng, need to get the item address in the list.
	// Free'ng pointer - sizeof LIST is used
	//          if (alignment > sizeof(LIST)) then 
	//              Ptr = Alloc'd Address + alignment; 
	//          else if (modulus(sizeof(LIST), alignment) then 
	//              Ptr = Alloc'd Addr + sizeof(LIST) + modulus(sizeof(LIST), alignment); 
	//          else 
	//              Ptr = Alloc'd Addr + sizeof(LIST)
	//
	size_t newSize;
	if (sizeof(LIST) < alignment) {
		// TODO, will anyone ask for MAX size_t ??
		newSize = size + alignment;
	} else {
		newSize = size + sizeof(LIST) + (sizeof(LIST) % alignment);
	}
#else
	size_t newSize = size;
#endif	
	if (0 < gMemInitialized) {
#if defined(USE_DEPRECATED_MEMALIGN)
		if (1 == type) {
			p = libc_memalign_fnptr(alignment, newSize);
		} 
#endif
#if defined(__USE_ISOC11)
		if (2 == type) {
               		p = libc_aligned_alloc_fnptr(alignment, newSize);
		}
#endif
#if defined(__USE_XOPEN2K)
		if (3 == type) {
			if (0 != libc_posix_memalign_fnptr(&p, alignment, newSize)) {
				p = NULL;
			}
		}
#endif
	}else {
		mapInitialMemory();
#ifndef SELF_TEST
		if (-1 == gMemInitialized) {
			load_libc_functions(); // Try once, but give the current request from static buffer
		}
#endif
		pthread_mutex_lock(&lock);
		// If gInitIndex == alignment, then below will unnecessarily add alignment. But that is rare
		gInitIndex = gInitIndex + alignment - (gInitIndex % alignment);
		p = &gInitialAlloc[gInitIndex];
		// TODO: check if alignment is power of 2 as well as multiple of sizeof(void*)??. %%NOT IMPORTANT%%
		if (newSize % sizeof(void*)) {
			gInitIndex += (newSize + (sizeof(void*) - newSize%sizeof(void*)));
		} else {
			gInitIndex += (newSize);
		}
		pthread_mutex_unlock(&lock);
		if (G_INITIAL_ALLOC_SIZE <= gInitIndex) {
			fwrite(G_INITIAL_ALLOC_SIZE_ERROR, sizeof(G_INITIAL_ALLOC_SIZE_ERROR), 1, stderr);
			abort();
		}
	}
#ifdef PREPEND_LISTDATA
	char *ptr;
	if (p) {
		ptr = (char*)p;
		if (alignment > sizeof(LIST)) {
			ptr = ptr + alignment;
		} else if ((sizeof(LIST) % alignment)) {
			ptr = ptr + sizeof(LIST) + (sizeof(LIST) % alignment);
		} else {
			ptr = ptr + sizeof(LIST);
		}
	} else {
		ptr = NULL;
	}
#ifdef ENABLE_STATISTICS
	pthread_mutex_lock(&lock);
	totalOverhead += (newSize - size);
	pthread_mutex_unlock(&lock);
#endif
	appendItemToList(ptr, size, setAlignment(alignment), __builtin_return_address(0));
	return ptr;
#else
	//prependItemToList(p, size, 0, __builtin_return_address(0));
	appendItemToList(p, size, __builtin_return_address(0));
	return p;
#endif
}
#endif

#if defined(USE_DEPRECATED_MEMALIGN)
__attribute__ ((visibility("default"))) void *memalign(size_t alignment, size_t size)
{
	return common_memalign(1, alignment, size);
}
#endif

#if defined(__USE_ISOC11)
__attribute__ ((visibility("default"))) void *aligned_alloc (size_t __alignment, size_t __size)
{
	return common_memalign(2, __alignment, __size);
}
#endif

#if defined(__USE_XOPEN2K)
__attribute__ ((visibility("default"))) int posix_memalign (void **__memptr, size_t __alignment, size_t __size)
{
	*__memptr = common_memalign(3, __alignment, __size);
}
#endif



/* If want to bypass the tracking, use below methods directly */
void *libc_malloc(size_t size)
{
	return libc_malloc_fnptr(size);
}
void *libc_calloc(size_t nmemb, size_t size)
{
	return libc_calloc_fnptr(nmemb, size);
}
void *libc_realloc(void *ptr, size_t size)
{
	return libc_realloc_fnptr(ptr, size);
}
void libc_free(void *ptr)
{
	libc_free_fnptr(ptr);
}
#if defined(USE_DEPRECATED_MEMALIGN)
void *libc_memalign(size_t alignment, size_t size)
{
        return libc_memalign_fnptr(alignment, size);
}
#endif
#if defined(__USE_XOPEN2K)
int libc_posix_memalign(void **memptr, size_t alignment, size_t size)
{
       return libc_posix_memalign_fnptr(memptr, alignment, size);
}
#endif
#if defined(__USE_ISOC11)
void *libc_aligned_alloc(size_t alignment, size_t size)
{
       return libc_aligned_alloc_fnptr(alignment, size);
}
#endif

