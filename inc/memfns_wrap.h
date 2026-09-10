/*
 * Copyright [2025] [Jagadheesan.D@gmail.com]
 *
 * SPDX-License-Identifier: Apache-2.0
 */
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
#define MEMWRAP_MAJOR_VERSION "2"
#define MEMWRAP_MINOR_VERSION "0"

/* MEMWRAP_COMMANDS_VERSION is 8 bit unsigned - shouldn't be greater than 255 */
#define MEMWRAP_COMMANDS_VERSION 4

/* Below is part of heapwalk file header hp_walk_header, used to check compatibility. 
 * Increment in case msg_resp structure changes. It's 8 bit unsigned, max 255
 * */
#define MEMWRAP_MSG_RESP_VERSION 1

typedef struct hp_header {
	int version; /* for now, using only last 8 bits for for msg_resp version */
	unsigned long totalEntries;
} hp_walk_header;

/* Memory Management Options */
#define PREPEND_LISTDATA /* Allocate extra for holding the data to avoid additional allocation */
#define OPTIMIZE_MQ_TRANSFER /* Avoid holding the process during lengthy heapwalk */
#define MAINTAIN_SINGLE_LIST /* Maintain single list for both walked and unwalked */
#define INTERCEPT_MMAP /* intercepts mmap calls. Makes use of static buffer for initial allocations, rather than allocating via mmap */

/* Command Optimization Flags */
#if defined(OPTIMIZE_MQ_TRANSFER)
#define OPTIMIZE_MQ_TRANSFER_FOR_CMD 1
#else
#define OPTIMIZE_MQ_TRANSFER_FOR_CMD 0
#endif

#if defined(PREPEND_LISTDATA)
#define ENABLE_STATISTICS
#define PREPEND_LISTDATA_FOR_CMD 1
#else
#define PREPEND_LISTDATA_FOR_CMD 0
#endif

#if defined(MAINTAIN_SINGLE_LIST)
#define MAINTAIN_SINGLE_LIST_FOR_CMD 1
#else
#define MAINTAIN_SINGLE_LIST_FOR_CMD 0
#endif

#if defined(INTERCEPT_MMAP)
#define INTERCEPT_MMAP_FOR_CMD 1
#else
#define INTERCEPT_MMAP_FOR_CMD 0
#endif

//#define PROCESS_PAGEMAP_IN_LIB

/* Static for internal testing */
#ifndef SELF_TEST
#define STATIC static
#else
#define STATIC
#endif

enum {
	FLAGS_BIT0_GLIBC_ALLOCATED = 0,
	FLAGS_BIT0_STATIC_BUFF_ALLOCATED = 1,
	FLAGS_BIT1_MALLOC_CALLOC = 0,
	FLAGS_BIT1_REALLOC = 2,
	FLAGS_MEMALIGN // Let it get 1 more than last entry
};

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
	/*
	 * Bit 0 --> 0 if allocated via glibc malloc, 1 if allocated before malloc is intercepted using static buffer
	 * Bit 1 --> 0 malloc/calloc, 1 realloc
	 * Bit 2 --> 1 memalign. Not needed, as we store alignment in the first half
	 */
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
	union {
	void *ptr;
	void *stack_addr_bottom;
	};
	unsigned long size;
	union {
	void *ra;
	void *start_routine;
	};
	union {
	pid_t tid;
	pthread_t pthread_id;
	};
	time_t seconds;
} LISTxfer;

/* Define maximum heatmap size as power of 2 */
#define MAX_HEAT_MAP 8

struct HEATMAP
{
	unsigned long startAddress;
	unsigned long endAddress;
	unsigned long long heapEntries;
};

struct pthread_info
{
	pthread_t pthread_id;
	unsigned long size;
	unsigned long stack_rss;
	unsigned long stack_swap;
	void *start_routine;
	time_t time;
};

enum {
	MMAP_NONE,
	MMAP_ANON,
	MMAP_ALL
};

typedef struct mmap
{
	unsigned long startAddress;
	unsigned long endAddress;
	unsigned long long heapEntries; /* Total size of heap entries within this mmap */
	unsigned int size;
	unsigned int rss;
	unsigned int swapPss;
	char entryName[256];
	char perm[8];
	//union  After coalesing stack and heap maps might be combined.
	//{
		struct HEATMAP heatmap[MAX_HEAT_MAP];
		struct pthread_info pthreadinfo;
	//};
	struct mmap *prev;
	struct mmap *next;
} MMAP_info;
#endif

typedef struct pthread_list
{
        pthread_t pthread_id;
	void *stack_addr_bottom;
	unsigned long size;
	void *start_routine;
	time_t time;
        struct pthread_list *next;
} LIST_pthread;

/*
 * Bits 0-54  page frame number (PFN) if present
 * Bits 0-4   swap type if swapped
 * Bits 5-54  swap offset if swapped
 * Bit  55    pte is soft-dirty (see Documentation/vm/soft-dirty.txt)
 * Bit  56    page exclusively mapped (since 4.2)
 * Bits 57-60 zero
 * Bit  61    page is file-page or shared-anon (since 3.5)
 * Bit  62    page swapped
 * Bit  63    page present
 */
typedef enum {
	PAGE_NOT_PRESENT = 0, // Bit 63, 62 not set. Note: If PFN may be 0 if process doesn't have CAP_SYS_ADMIN capability, but fine, we donot need.
	PAGE_PRESENT = 1, // Bit 63 set
	PAGE_SWAPPED = 2, // Bit 62 set
	PAGE_ACCOUNTED = 3, // when above 2 bits are set, while identifying rss for allocations and accounting total heap rss size, this indicates this page was already taken into account
	PAGE_FILEPAGE_SHAREDANON, // Not used
	PAGE_DIRTY, // Not used
}PAGEMAPSTAT;

// TODO can be converted to binary search tree or red black tree for efficient traversing later on for determining rss value for each allocated entry
typedef struct pagemap_list
{
	void *pageaddress; // Let's store pageaddress directly instead of pfn
        //size_t pageindex; // vaddr / sysconf(_SC_PAGE_SIZE)
	PAGEMAPSTAT pagestat;
        struct pagemap_list *next;
} LIST_pagemap;

#if defined(INTERCEPT_MMAP)
struct list_mmap_wrap;

typedef struct list_mmap_wrap {
        void *start_addr;
        void *end_addr;
        void *ra;
        time_t time;
        struct list_mmap_wrap *next;
}LIST_mmap_wrap;
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
	HEAPWALK_INCREMENT = (HEAPWALK_BASE | 1),
	HEAPWALK_FULL = (HEAPWALK_BASE | 2),
	HEAPWALK_LEAKCHECK = (HEAPWALK_BASE | 3),
	HEAPWALK_MMAP_ENTRIES = (HEAPWALK_BASE | 4),
	HEAPWALK_MARKALL = (HEAPWALK_BASE | 5),
	HEAPWALK_RESET_MARKED = (HEAPWALK_BASE | 6),
	HEAPWALK_MALLOC_STATS = (HEAPWALK_BASE | 7),
	HEAPWALK_PTHREAD_INTERCEPT = (HEAPWALK_BASE | 8), // Internal cmd to get pthread create intercepts
	HEAPWALK_INTERCEPT_MMAP = (HEAPWALK_BASE | 9)
	//HEAPWALK_EXIT = (MEMWRAP_COMMANDS_VERSION << 24 | OPTIMIZE_MQ_TRANSFER_FOR_CMD << 23 | PREPEND_LISTDATA_FOR_CMD << 22 | MAINTAIN_SINGLE_LIST_FOR_CMD << 21 | 0)
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
	unsigned long heapPeakSize;
	time_t heapPeakedAt;
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
void heapwalk(mqd_t mqsend, bool walkAll, char *fname);
int saveHeapwalk(char *suffix);
void registerAtExit(void);
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
