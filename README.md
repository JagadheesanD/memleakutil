# Memleakutil

## Table of Contents
[Contributing Guidelines](#contributing-guidelines)<br>
[Preface](#preface)<br>
[What does it do?](#what-does-it-do)<br>
[Overview](#overview)<br>
[Features and Benefits](#features-and-benefits)<br>
[Usecase](#usecase)<br>
[How to build?](#how-to-build)<br>
[How to use?](#how-to-use)<br>
[Resolving Return Address](#resolving-return-address)<br>
[Self Test](#self-test)<br>
[Future improvements](#future-improvements)<br>
[Versioning](#versioning)<br>
[CHANGELOG](#changelog)<br>
[Questions?](#questions)<br>

## **Contributing Guidelines**
1. [Fork](https://docs.github.com/en/github/getting-started-with-github/quickstart/fork-a-repo) the repository, commit your changes, build and test it a Linux/ Unix based environment.
2. Submit the changes as a [pull request](https://docs.github.com/en/github/collaborating-with-issues-and-pull-requests/proposing-changes-to-your-work-with-pull-requests/creating-a-pull-request-from-a-fork) against the latest develop branch.
3. Make sure the Workflows are running and all checks are passed and commits have Verified Signature
4. At least 1 Approving review is required by the reviewers with write access.

## **Preface**
**memeleakutil** is a debugging tool designed to intercept and track memory allocations within processes. It provides comprehensive insights into heap usage 
with the ability to walk through memory allocations, identify potential memory leaks, and analyze memory-mapped regions.

## **What does it do?**
### ProcessWise/Threadwise Full Heap Walk
  - Displays details such as Pointer, Size, Function address (Return Address), Thread ID, Allocation Time, and Reallocation status.
  - Shows Total Heap size and Tool overhead.
### Incremental Heap Walk
  - Displays entries that have been allocated and not freed since the last heap walk.
### MMAP % mapping with heap and it's distribution
  - Illustrates mapped address ranges in terms of Size(Kb), RSS(Kb), Heap'd(Bytes), and the heap’s percentage against RSS.
  - Provides a heatmap of the total size over 32 (configurable via MAX_HEAT_MAP) divisions.
### Mark all Heap entries as Walked
  - Marks entries as walked but doesn't display them.
### Mark all Heap entries as un-walked
  - Subsequent walks will display all entries.

## **Overview**
The tool consists of two main parts:
1. memleakutil Executable.
2. Memory Functions Interceptor Library (libmemfnswrap.so)

**memleakutil** communicates with **libmemfnswrap.so**, interposing itself in between the target process and the Glibc memory functions. It provides interactive commands to perform heap walks, marking memory allocations, and mapping memory regions for detailed analysis.

**libmemfnswrap.so** can be linked to the executable during the linking phase or be preloaded using **LD_PRELOAD** to begin intercepting and collecting information such as:
* Pointer addresses
* Allocation sizes
* Thread IDs
* Allocation times and more

This library maintains allocation details in a linked list, which can optionally use **PREPEND_LISTDATA_FOR_CMD** to manage these entries efficiently.

## **Features and Benefits**
1. **Single Linked List:** Provides an efficient way to maintain and track allocations.
2. **Incremental Walks:** Identify new allocations since the last tracked walk, making it easy to spot potential memory leaks.
3. **Heatmap Mapping:** Visualize memory usage within mapped memory regions, providing insights into how allocations are distributed.
4. **Command-Line Interface:** Interact with the tool in a user-friendly manner to perform heap walks, manage marks, and analyze memory usage.

## **How to build?**
The tool uses configure.ac and Makefile.am for creating Makefiles during cross/straight compilation. For a quick compilation, use:
```
make -f Makefile.raw
```
### Compiler Options available
- **MAINTAIN_SINGLE_LIST_FOR_CMD**: Maintains a single list for both walked and un-walked entries (default and efficient).
- **PREPEND_LISTDATA_FOR_CMD**: Adjusts requested pointer size to include metadata, aiding in efficient handling by reducing extra memory requests (default).
- **OPTIMIZE_MQ_TRANSFER_FOR_CMD**: Facilitates bulk transfer during heapwalks, reducing process hold times (default).
- **MEMWRAP_COMMANDS_VERSION**: Specifies command version for ensuring compatibility between memleakutil and libmemfnswrap.so.

## How to use?
**memleakutil** performs heap walks on processes running with libmemfnswrap.so attached. It interacts via a POSIX message queue */mq_wrapper_<pid>*, opened by a thread running within the target process. During a heap walk, the tool prints the total heap size and tool overhead.
### Steps
1. Start target process with libmemfnswrap.so attached.
```
LD_PRELOAD=/path/to/libmemfnswrap.so ./target_process
```
2. Run memleakutil and interact with it to perform heap walks.
```
./memleakutil
```
3. Follow on-screen instructions to view allocations, mark as walked, map heap vs mmap entries, etc.
**Example Commands:**
* Heapwalk New Allocations: Shows newly allocated and un-freed entries since the last walk.
* Heapwalk All Allocations: Displays all allocation entries.
* Map Heap vs Mmap Entries: Provides percentage mapping and distribution of anonymous memory mappings (requires OPTIMIZE_MQ_TRANSFER).
* Mark All Allocations as Walked: Marks allocations as walked but does not display them.
* Unmark Walked Allocations: Resets marks for walked allocations.

## Resolving Return Address
To resolve the RA address:
1. Get the process's memory maps (if ASLR is enabled, repeat for all entries; otherwise, do this only for dynamic libraries).
2. Subtract the initial offset of the map entry from the RA.
3. Use *addr2line* to convert the processed RA to a file and line number.
```
addr2line -f -e <file with symbols> 0x<processed RA>
```

## Self Test
Run self-tests using:
```
make selftest
```
Or compile with SELF_TEST to include self-test code and execute:
```
./memleakutil selftest
```
This command runs a series of tests to verify the tool’s functionality.

## Future Improvements
1. Offline Data Storage for Analysis
2. Capture Multiple Backtrace Addresses
3. Automated Leak Detection Logic
4. Pause/Resume Heap Walks
5. Determine Physical Usage of Allocations

### Versioning
Given a version number MAJOR.MINOR.PATCH, increment the:
- **MAJOR:** version when you make incompatible API changes that break backwards compatibility. This could be removing existing APIs, changes to API Signature or major changes to API behavior that breaks API contract, 
- **MINOR:** version when you add backward compatible new features like adding new APIs, adding new parameters to existing APIs,
- **PATCH:** version when you make backwards compatible bug fixes.

### CHANGELOG
The CHANGELOG file that contains all changes done so far. When version is updated, add a entry in the [CHANGELOG.md](./CHANGELOG.md) at the top with user friendly information on what was changed with the new version. Refer to [Changelog](https://github.com/olivierlacan/keep-a-changelog/blob/main/CHANGELOG.md) as an example and [Keep a Changelog](https://keepachangelog.com/en/1.0.0/) for more details.

Please Add entry in the CHANGELOG for each version change and indicate the type of change with these labels:
- **Added** for new features.
- **Changed** for changes in existing functionality.
- **Deprecated** for soon-to-be removed features.
- **Removed** for now removed features.
- **Fixed** for any bug fixes.
- **Security** in case of vulnerabilities.

### Questions?
Any questions or concerns? Please reach out to Jagadheesan Duraisamy