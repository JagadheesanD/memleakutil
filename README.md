# What do you get?
## 1. ProcessWise/Threadwise Full Heap Walk
  - Pointer, Size, Function address that called (RA), Thread ID, Allocation Time, Whether pointer re-alloc'd
  - Total Heap size, Tool overhead
## 2. Incremental Heap Walk
  - Display entries allocated (and not free'd) after previous walk
## 3. MMAP % mapping with heap and it's distribution
  - mmapStart-mmapEnd        Size(Kb)    RSS(Kb)    Heap'd(Bytes)    Heap'd(%)vsRSS
  - Anon Heatmap of total size over 32 (configurable via MAX_HEAT_MAP) divisions
## 4. Mark all Heap entries as Walked
  - Doesn't display, rather marks entries as walked
## 5. Mark all Heap entries as un-walked
  - Next walk displays all entries

# About
Module consists of 2 parts, (1) memleakutil executable (2) Library to be linked to target process.

User interactive memleakutil executable, communicates with libmemfnswrap.so which is linked/LD_PRELOAD'd with target process that is debugged. Gives option for heapwalk (threadwise or whole process), heapwalk full, Mapping of anon vs heap entries.

Memfnswrap, is a simple interceptor for Glibc's malloc, calloc, realloc, memalign and free calls. libmemfnswrap.so can either be linked with the executable during linking time or LD_PRELOAD'd to start intercepting and collect information like pointer, size, thread id, time, function address that called malloc/calloc/realloc/memalign, flag indicating whether re-alloc'd. The information is maintained in a linked list whose nodes are allocated alongside the requested pointer address (with PREPEND_LISTDATA_FOR_CMD enabled). Basically the size needed for maintaining the information is added to the size of requested allocation, also considering the alignment requirement. Thus the information is placed at the beginning and after considering the alignment, the following address is returned to the caller. This not only reduces the extra call for allocating the information structure, but also useful during free/realloc for locating the node by use of hashing. 

When a pointer is freed, the node containing the details is also freed up. Therefore at any point of time, a heapwalk will give us the allocations that are currently in use.

# How to build
configure.ac/Makefile.am provides instruction for autotool to create Makefile.in during cross/straight compilation. For quick compilation, use make -f Makefile.raw
## Compiler Options available:
   - MAINTAIN_SINGLE_LIST_FOR_CMD - Maintains single list for walked entries and un-walked entries. This option is default for efficiency.
   - PREPEND_LISTDATA_FOR_CMD     - Manipulates pointer size requested for maintaining the data. If there is any heap corruption, then this will take a hit. In that case, disable this for debugging. This option is default, as it's efficient handling free/realloc for locating the data maintained using hashing, rather than navigating the list. If this size increase meddles with fastbin/tcache size, then disable for effective debugging. 
   - OPTIMIZE_MQ_TRANSFER_FOR_CMD - Effective bulk transfer during heapwalk, rather than holding the process for long. This option is default
   - MEMWRAP_COMMANDS_VERSION     - Commands version. Changes when there is option change. Above 3 definitions affects the command value, so that memleakutil/libmemfnswrap.so compatibility is determined. Below is the way commands are constructed
     ActualCmd = (MEMWRAP_COMMANDS_VERSION << 24 | OPTIMIZE_MQ_TRANSFER_FOR_CMD << 23 | PREPEND_LISTDATA_FOR_CMD << 22 | MAINTAIN_SINGLE_LIST_FOR_CMD << 21 | Cmd)

# How to use
memleakutil executable shall be used to perform heapwalk on the processes that run with libmemfnswrap.so. It gets the pid of the process and tries to open posix message queue named mq_wrapper_pid, created by thread running under process context, waiting for read. During heapwalk, the tool will print the total size of the heap, as well as the overhead by the tool. Please note, the size is only a virtual size, and the tool doesn't calculate the actual physical size that was utilized.

The tool will mark the walked allocations and subsequent heapwalk command will not list the walked allocations. It will only print the new allocations done after previous walk. This way, the probable leaked allocations can be easily determined by doing an initial walk, then introducing the leak, and then doing a walk. This smaller list shall be analyzed for finding out the leak.

If subsequent heap walk shows total heap size increase, which is not proportional to process's RSS increase, then to determine if mmap anon holds sparse allocations, use mmap % mapping option. This could happen when fastbin or thread cache holds-up the allocations, as well as a result of mmap coalesing. In such scenarios, use glibc tunables to regulate use of fastbins or thread cache.

# Resolving RA address
Get the process's maps and subtract the initial offset of the map entry from the RA (Note: If ASLR is enabled, do this for all. If not, only for dynamic libraries) and using addr2line -f -e <file with symbols> 0x<processed RA>

# Selftest
make selftest or compile with SELF_TEST compiler option to add selftest code, and run "./memleakutil selftest". In Yocto environment, ptest distro ensures creation of ptest package, which can be used for doing selftest.

# TODO/Improvements
## Provision to store data for offline analysis
## Get configured number of backtrace addresses instead of the current RA alone
## Once above is available, have a logic to list out probable leak
## Have pause / resume functionality
## Figure out physical usage of the allocations
## More enhancement possible
