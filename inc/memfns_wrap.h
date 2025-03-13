#ifndef MEMFNS_WRAP_H
#define MEMFNS_WRAP_H

/* Include libraries */
#include <stddef.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>
#include <mqueue.h>

/* Define/undefine as needed */
#define USE_DEPRECATED_MEMALIGN
#undef __USE_XOPEN2K
#undef __USE_ISOC11

/*
 * Version Constants: 
 * Used for setting versionString[]
 * MEMWRAP_COMMANDS_VERSION is used for tracking compatibility
 * between memleakutil and libmemfnswrap.so.
 * Increment only when there is a change in commands list.
 */
#define MEMWRAP_MAJOR_VERSION "1"
#define MEMWRAP_MINOR_VERSION "0"

/* MEMWRAP_COMMANDS_VERSION is 8 bit unsigned - shouldn't be greater than 255 */
#define MEMWRAP_COMMANDS_VERSION 1

/* Memory Management Options */
#define PREPEND_LISTDATA /* Allocate extra for holding the data to avoid additional allocation */
#define OPTIMIZE_MQ_TRANSFER /* Avoid holding the process during lengthy heapwalk */
#define MAINTAIN_SINGLE_LIST /* Maintain single list for both walked and unwalked */

/* Command Optimization Flags */
#ifdef OPTIMIZE_MQ_TRANSFER
#define OPTIMIZE_MQ_TRANSFER_FOR_CMD 1
#else
#define OPTIMIZE_MQ_TRANSFER_FOR_CMD 0
#endif

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

/* Static for internal testing */
#ifndef SELF_TEST
#define STATIC static
#else
#define STATIC
#endif

/* Data Structures */
struct list;

/* 
 * Define LIST structure - Ensure gListInitIndex is aligned to void* and sync with LISTxfer structure 
 * NOTE: Don't pack this structure
 */
typedef struct list
{
#if defined(PREPEND_LISTDATA)
	unsigned int flags; /* First 2 bytes are magic number (for LSB/MSB), next 2 are real flag */
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
} LIST;

#ifdef OPTIMIZE_MQ_TRANSFER
typedef struct list_xfer
{
#ifdef PREPEND_LISTDATA
	unsigned int flags; /* First 2 bytes are magic number (for LSB/MSB), next 2 are real flag */
#endif
	void *ptr;
	unsigned int size;
	void *ra;
	pid_t tid;
	time_t seconds;
} LISTxfer;

/* Define maximum heatmap size as power of 2 */
#define MAX_HEAT_MAP 32

struct HEATMAP
{
	unsigned long startAddress;
	unsigned long endAddress;
	unsigned long long heapEntries;
};

typedef struct mmap
{
	unsigned long startAddress;
	unsigned long endAddress;
	unsigned long long heapEntries; /* Total size of heap entries within this mmap */
	unsigned int size;
	unsigned int rss;
	unsigned int dirty;
	char perm[8];
	char entryName[64];
	struct HEATMAP heatmap[MAX_HEAT_MAP];
	struct mmap *prev;
	struct mmap *next;
} MMAP_anon;
#endif

/* Message Queue Configuration */
#define MQ_MSG_SIZE 128
typedef struct mq_msg_cmd
{
	int cmd;
	int pid;
} msg_cmd;

typedef enum
{
	HEAPWALK_BASE = (MEMWRAP_COMMANDS_VERSION << 24 | OPTIMIZE_MQ_TRANSFER_FOR_CMD << 23 | PREPEND_LISTDATA_FOR_CMD << 22 | MAINTAIN_SINGLE_LIST_FOR_CMD << 21),
	HEAPWALK_INCREMENT = (MEMWRAP_COMMANDS_VERSION << 24 | OPTIMIZE_MQ_TRANSFER_FOR_CMD << 23 | PREPEND_LISTDATA_FOR_CMD << 22 | MAINTAIN_SINGLE_LIST_FOR_CMD << 21 | 1),
	HEAPWALK_FULL = (MEMWRAP_COMMANDS_VERSION << 24 | OPTIMIZE_MQ_TRANSFER_FOR_CMD << 23 | PREPEND_LISTDATA_FOR_CMD << 22 | MAINTAIN_SINGLE_LIST_FOR_CMD << 21 | 2),
	HEAPWALK_MMAP_ENTRIES = (MEMWRAP_COMMANDS_VERSION << 24 | OPTIMIZE_MQ_TRANSFER_FOR_CMD << 23 | PREPEND_LISTDATA_FOR_CMD << 22 | MAINTAIN_SINGLE_LIST_FOR_CMD << 21 | 3),
	HEAPWALK_MARKALL = (MEMWRAP_COMMANDS_VERSION << 24 | OPTIMIZE_MQ_TRANSFER_FOR_CMD << 23 | PREPEND_LISTDATA_FOR_CMD << 22 | MAINTAIN_SINGLE_LIST_FOR_CMD << 21 | 4),
	HEAPWALK_RESET_MARKED = (MEMWRAP_COMMANDS_VERSION << 24 | OPTIMIZE_MQ_TRANSFER_FOR_CMD << 23 | PREPEND_LISTDATA_FOR_CMD << 22 | MAINTAIN_SINGLE_LIST_FOR_CMD << 21 | 5),
	HEAPWALK_EXIT = (MEMWRAP_COMMANDS_VERSION << 24 | OPTIMIZE_MQ_TRANSFER_FOR_CMD << 23 | PREPEND_LISTDATA_FOR_CMD << 22 | MAINTAIN_SINGLE_LIST_FOR_CMD << 21 | 6)
} mycmds;

typedef enum
{
	HEAPWALK_EMPTY = 0x0,
	HEAPWALK_ITEM_CONTN = 0x10000000,
	HEAPWALK_ENDOF_LIST = 0x20000000
} heapwalkCtrl;

#define MAX_MSG_XFER 100
typedef struct mq_msg_recv
{
#ifndef OPTIMIZE_MQ_TRANSFER
	int seq;
	char msg[MQ_MSG_SIZE];
#else
	unsigned int numItemOrInfo;
	unsigned long totalHeapSize;
	unsigned long totalOverhead;
	LISTxfer xfer[MAX_MSG_XFER];
#endif
} msg_resp;

#define QUEUE_PERMISSION ((int)(0666))
#define QUEUE_READ_PERMISSION ((int)(0444))
#define QUEUE_MAXMSG 256

/* Function Declarations */
void load_libc_functions();

#if defined(USE_DEPRECATED_MEMALIGN)
/* Deprecated 'memalign' declaration */
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
/* Self-test functionality */
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
 * Debugging Configuration
 * DISABLE_DEBUG   --> Define to disable all debug statements
 * DEBUG_RUNTIME   --> Define to control debug print level during runtime.
 *                     Update DEBUG_ENV_LEVEL env with new level and send sigusr1 signal
 *                     NOTE: Function with variable number of args will not detect format errors.
 *                     Therefore, check format errors without this flag
 * !DEBUG_RUNTIME  --> Undefine to control debug level during compilation time. No runtime overhead
 * PRINT_****      --> Use/define new levels
 ********************************/

#undef DISABLE_DEBUG
// #define DEBUG_RUNTIME

#if defined(DISABLE_DEBUG)
#define dbg(A, ...) ((void)0);
#else

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
#define dbg(A, ...)                         \
if ((DEBUG_LEVEL > A) || (PRINT_MUST == A)) \
{                                           \
	if (PRINT_WALK != A)                    \
	{                                       \
		printf("%d: ", getpid());           \
	}                                       \
	printf(__VA_ARGS__);                    \
}                                           \
else                                        \
((void)0);

#else
// Default level is 2
// set DEBUG_ENV_LEVEL in environment
extern int debug_level;
extern void dbg(int a, const char *b, ...);
#endif

#endif /* End of DISABLE_DEBUG */

#endif /* End of MEMFNS_WRAP_H */
