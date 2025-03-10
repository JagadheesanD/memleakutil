#ifndef MEMFNS_WRAP_H
#define MEMFNS_WRAP_H

#include <stddef.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>
#include <mqueue.h>

// Define/undefine as needed
#define USE_DEPRECATED_MEMALIGN
#undef __USE_XOPEN2K
#undef __USE_ISOC11

// Version control, used for setting versionString[]
#define MEMWRAP_MAJOR_VERSION 1
#define MEMWRAP_MINOR_VERSION 0

// Allocate extra for holding the data, so that we can avoid additional allocation
#define PREPEND_LISTDATA
// Doesn't hold the process during lengthy heapwalk
#define OPTIMIZE_MQ_TRANSFER
// Maintains single list for both walked and un-walked
#define MAINTAIN_SINGLE_LIST

#ifdef OPTIMIZE_MQ_TRANSFER
#define OPTIMIZE_MQ_TRANSFER_FOR_CMD 1
#else
#define OPTIMIZE_MQ_TRANSFER_FOR_CMD 0
#endif

// Print statistics
#ifdef PREPEND_LISTDATA
#define ENABLE_STATISTICS
#define PREPEND_LISTDATA_FOR_CMD 1
#else
#define PREPEND_LISTDATA_FOR_CMD 0
#endif

#ifdef MAINTAIN_SINGLE_LIST
#define MAINTAIN_SINGLE_LIST_FOR_CMD 1
#else
#define MAINTAIN_SINGLE_LIST_FOR_CMD 0
#endif

#ifndef SELF_TEST
#define STATIC static
#else
#define STATIC
#endif

struct list;
// Do not pack this structure. If so, then we need to make gListInitIndex aligned to void*
// Also, keep it in sync with LISTxfer structure
typedef struct list {
#if defined(PREPEND_LISTDATA)
	// First 2 bytes (taking care of LSB/MSB) contains magic number. 
	// Second 2 bytes real flag. For now using a bit for realloc'd entries
	unsigned int flags;
#endif
	void *ptr;
        unsigned int size;
        void *ra;
        pid_t tid;
        time_t seconds;
        struct list *next;
#ifdef PREPEND_LISTDATA
        struct list *prev;
#endif
}LIST;

#ifdef OPTIMIZE_MQ_TRANSFER
typedef struct list_xfer {
#ifdef PREPEND_LISTDATA
	// First 2 bytes (taking care of LSB/MSB) contains magic number. 
	// Second 2 bytes real flag. For now using a bit for realloc'd entries
	unsigned int flags;
#endif
	void *ptr;
        unsigned int size;
        void *ra;
        pid_t tid;
        time_t seconds;
}LISTxfer;

// Keep it as 2^x
#define MAX_HEAT_MAP 32
struct HEATMAP {
	unsigned long startAddress;
	unsigned long endAddress;
	unsigned long long heapEntries;
};

typedef struct mmap {
	unsigned long startAddress;
	unsigned long endAddress;
	// Holds the total size of heap entries falling into this mmap
	unsigned long long heapEntries;
	unsigned int size;
	unsigned int rss;
	unsigned int dirty;
	char perm[8];
	char entryName[64];
	struct HEATMAP heatmap[MAX_HEAT_MAP];
	struct mmap *prev;
	struct mmap *next;
}MMAP_anon;
#endif

#define MQ_MSG_SIZE 128
typedef struct mq_msg_cmd{
        int cmd;
        int pid;
}msg_cmd;

typedef enum {
	HEAPWALK_BASE = (MEMWRAP_MAJOR_VERSION<<24|MEMWRAP_MINOR_VERSION<<16|OPTIMIZE_MQ_TRANSFER_FOR_CMD<<15|PREPEND_LISTDATA_FOR_CMD<<14|MAINTAIN_SINGLE_LIST_FOR_CMD<<13),
	HEAPWALK_INCREMENT = (MEMWRAP_MAJOR_VERSION<<24|MEMWRAP_MINOR_VERSION<<16|OPTIMIZE_MQ_TRANSFER_FOR_CMD<<15|PREPEND_LISTDATA_FOR_CMD<<14|MAINTAIN_SINGLE_LIST_FOR_CMD<<13|1),
	HEAPWALK_FULL = (MEMWRAP_MAJOR_VERSION<<24|MEMWRAP_MINOR_VERSION<<16|OPTIMIZE_MQ_TRANSFER_FOR_CMD<<15|PREPEND_LISTDATA_FOR_CMD<<14|MAINTAIN_SINGLE_LIST_FOR_CMD<<13|2),
	HEAPWALK_MMAP_ENTRIES = (MEMWRAP_MAJOR_VERSION<<24|MEMWRAP_MINOR_VERSION<<16|OPTIMIZE_MQ_TRANSFER_FOR_CMD<<15|PREPEND_LISTDATA_FOR_CMD<<14|MAINTAIN_SINGLE_LIST_FOR_CMD<<13|3),
	HEAPWALK_MARKALL = (MEMWRAP_MAJOR_VERSION<<24|MEMWRAP_MINOR_VERSION<<16|OPTIMIZE_MQ_TRANSFER_FOR_CMD<<15|PREPEND_LISTDATA_FOR_CMD<<14|MAINTAIN_SINGLE_LIST_FOR_CMD<<13|4),
	HEAPWALK_RESET_MARKED = (MEMWRAP_MAJOR_VERSION<<24|MEMWRAP_MINOR_VERSION<<16|OPTIMIZE_MQ_TRANSFER_FOR_CMD<<15|PREPEND_LISTDATA_FOR_CMD<<14|MAINTAIN_SINGLE_LIST_FOR_CMD<<13|5),
	HEAPWALK_EXIT = (MEMWRAP_MAJOR_VERSION<<24|MEMWRAP_MINOR_VERSION<<16|OPTIMIZE_MQ_TRANSFER_FOR_CMD<<15|PREPEND_LISTDATA_FOR_CMD<<14|MAINTAIN_SINGLE_LIST_FOR_CMD<<13|6)
}mycmds;

typedef enum {
	HEAPWALK_EMPTY = 0x0,
	HEAPWALK_ITEM_CONTN = 0x10000000,
	HEAPWALK_ENDOF_LIST = 0x20000000
}heapwalkCtrl;

#define MAX_MSG_XFER 100
typedef struct mq_msg_recv{
#ifndef OPTIMIZE_MQ_TRANSFER
        int seq;
        char msg[MQ_MSG_SIZE];
#else
	unsigned int numItemOrInfo;
	unsigned long totalHeapSize;
	unsigned long totalOverhead;
	LISTxfer xfer[MAX_MSG_XFER];
#endif
}msg_resp;

#define QUEUE_PERMISSION ((int)(0666))
#define QUEUE_READ_PERMISSION ((int)(0444))
#define QUEUE_MAXMSG 256

void load_libc_functions();
#if defined(USE_DEPRECATED_MEMALIGN)
// Since it is deprecated, if someone is still using such that it links to memalign@GLIBC_2.17, then
// below provides declaration to interpret
void *memalign(size_t alignment, size_t size);
#endif

#ifdef OPTIMIZE_MQ_TRANSFER
void heapwalk(mqd_t mqsend, bool walkAll);
#else
void heapwalk(mqd_t mqsend);
void heapwalk_full(mqd_t mqsend);
#endif
void heapwalkMarkall();
void heapwalkReset();

#ifdef SELF_TEST
// For selftest functionality alone..
void resetList();
void dispStatus();
extern pthread_mutex_t lock;
extern int gMemInitialized;
#ifdef MAINTAIN_SINGLE_LIST
extern LIST *hpfmemhead, *hpfmemtail, *hpwmemhead;
#else
extern LIST *memhead, *memtail;
extern LIST *wmemhead, *wmemtail;
#endif
extern char *gInitialAlloc;
extern unsigned int gInitIndex;
#endif

#define PRINT printf
/********************************
 * DISABLE_DEBUG   --> Define to disable all debug statements
 * DEBUG_RUNTIME   --> Define to control debug print level during runtime. 
 *                     Update DEBUG_ENV_LEVEL env with new level and send sigusr1 signal
 *                     NOTE: Function with variable number of args will not detect format errors. 
 *                     Therefore, check format errors without this flag
 * !DEBUG_RUNTIME  --> Undefine to control debug level during compilation time. No runtime overhead 
 * PRINT_****      --> Use/define new levels
 *******************************/

#undef DISABLE_DEBUG
//#define DEBUG_RUNTIME

#if defined(DISABLE_DEBUG)
#define dbg(A,...) \
	((void)0);
#else //#if defined(DISABLE_DEBUG)


#define PRINT_MUST 23
#define PRINT_WALK -1
#define PRINT_FATAL 0
#define PRINT_ERROR 1
#define PRINT_SEM 2
#define PRINT_MSGQ 3
#define PRINT_LIST 4
#define PRINT_INFO 5
#define PRINT_NOISE 6

#include <stdarg.h>
#if !defined(DEBUG_RUNTIME)
// Default level
#define DEBUG_LEVEL 2
#define dbg(A,...) \
	if ((DEBUG_LEVEL > A) || (PRINT_MUST == A)) {\
		if (PRINT_WALK != A) {\
			printf("%d: ", getpid()); }\
		printf(__VA_ARGS__); \
	}else ((void)0);

#else
// Default level is 2
// set DEBUG_ENV_LEVEL in environment
extern int debug_level;
extern void dbg(int a, const char *b, ...);
#endif

#endif //#if defined(DISABLE_DEBUG)

#endif //MEMFNS_WRAP_H
