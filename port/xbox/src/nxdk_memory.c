/*
NXDK_MEMORY.C

Xbox contiguous memory (XPhysicalAlloc) and page protection, from the
Xbox's own kernel: the other ports emulate the window at 0x80000000 that
it maps physical memory at (port/linux/src/xbox_memory.c), which is real
here.

The kernel gives contiguous memory only from the low 64 MB, even on a
128 MB console, and ordinary virtual memory takes the low half's free pages
before the upper half's (port/xbox/README.md, the probe). So the caches the
GPU reads are contiguous and allocated first (cache/physical_memory_map.c),
then a pool of contiguous memory for later, and the game state, which is
larger on the native builds than on the Xbox (halo_port_capacity.h), is
virtual memory at a fixed address instead of at physical 0x1A00000
(xbox_game_state_allocate), as the sound cache is virtual memory.
*/

#include <xboxkrnl/xboxkrnl.h>
#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "nxdk_platform.h"

#define PAGE_BYTES 0x1000UL
#define CONTIGUOUS_BASE 0x80000000UL
#define CONTIGUOUS_SIZE 0x08000000UL
#define ANY_PHYSICAL_ADDRESS 0xffffffffUL

/* (port/linux/src/platform.h's names for these, which this unit, built
with nxdk's headers instead of the Xbox SDK's, does not include) */
void *platform_contiguous_alloc(unsigned long size, unsigned long alignment,
	unsigned long physical_address, DWORD protect);
void platform_contiguous_free(void *address);
BOOL platform_is_contiguous(const void *address);

BOOL platform_is_contiguous(const void *address)
{
	return (unsigned long)address >= CONTIGUOUS_BASE &&
		(unsigned long)address < CONTIGUOUS_BASE + CONTIGUOUS_SIZE;
}

/* Contiguous memory set aside before the game state is allocated
(xbox_contiguous_reserve): a pool, its pages handed out when the kernel has
no contiguous memory left. The game state and the heap are virtual memory,
which the kernel gives the low 64 MB's free pages first, and only the low
64 MB is had as contiguous (the program and the caches fill most of it):
with nothing set aside, 3.4 MB was left once the game state was allocated,
then less as the heap grew, too little for the menus' larger pictures. Set
aside, the pool's pages are the upper half's instead, and its pages stay
its own: given back to the kernel, the heap would take them. */
#define POOL_BLOCK_BYTES (64UL * 1024)
#define POOL_MAXIMUM_BYTES (12UL * 1024 * 1024)
/* (the pool's blocks lie in the low 64 MB: its span, in pages) */
#define POOL_MAXIMUM_SPAN (64UL * 1024 * 1024 / PAGE_BYTES)

static struct
{
	unsigned long base;
	unsigned long span;
	/* each page: whether free, and at an allocation's first page, its
	length in pages */
	unsigned char free[POOL_MAXIMUM_SPAN];
	unsigned short length[POOL_MAXIMUM_SPAN];
	unsigned long pages, used_pages;
	RTL_CRITICAL_SECTION lock;
} pool;

void xbox_contiguous_reserve(void)
{
	void *blocks[POOL_MAXIMUM_BYTES / POOL_BLOCK_BYTES];
	unsigned long count = 0, index, low = 0xffffffffUL, high = 0;

	RtlInitializeCriticalSection(&pool.lock);
#ifdef HALO_XBOX_64MB
	/* (a 64 MB console's memory is all the low 64 MB: virtual memory takes
	nothing the kernel could not give as contiguous, and the pool would only
	take what is left) */
	return;
#endif
	while (count < POOL_MAXIMUM_BYTES / POOL_BLOCK_BYTES)
	{
		void *block = MmAllocateContiguousMemoryEx(POOL_BLOCK_BYTES, 0, 0xFFFFFFFF, 0, PAGE_READWRITE);

		if (!block)
			break;
		blocks[count++] = block;
		if ((unsigned long)block < low)
			low = (unsigned long)block;
		if ((unsigned long)block + POOL_BLOCK_BYTES > high)
			high = (unsigned long)block + POOL_BLOCK_BYTES;
	}
	if (!count)
		return;
	pool.base = low;
	pool.span = (high - low) / PAGE_BYTES;
	for (index = 0; index < count; index++)
		memset(pool.free + ((unsigned long)blocks[index] - low) / PAGE_BYTES, 1, POOL_BLOCK_BYTES / PAGE_BYTES);
	pool.pages = count * (POOL_BLOCK_BYTES / PAGE_BYTES);
}

static void *pool_alloc(unsigned long pages, unsigned long alignment, DWORD protect)
{
	unsigned long first, run = 0;
	void *result = NULL;

	RtlEnterCriticalSection(&pool.lock);
	/* the first free run long enough, from an aligned page */
	for (first = 0; first + pages <= pool.span; first++)
	{
		if (!pool.free[first] || ((pool.base + first * PAGE_BYTES) & (alignment - 1)))
			continue;
		for (run = 0; run < pages && pool.free[first + run]; run++)
			;
		if (run == pages)
			break;
		first += run;
	}
	if (first + pages <= pool.span && run == pages)
	{
		memset(pool.free + first, 0, pages);
		pool.length[first] = (unsigned short)pages;
		pool.used_pages += pages;
		result = (void *)(pool.base + first * PAGE_BYTES);
	}
	RtlLeaveCriticalSection(&pool.lock);
	if (result && protect != PAGE_READWRITE)
		MmSetAddressProtect(result, pages * PAGE_BYTES, protect);
	return result;
}

/* whether the pool's (and so given back to it) */
static BOOL pool_free(void *address)
{
	unsigned long first = ((unsigned long)address - pool.base) / PAGE_BYTES;

	if (!pool.span || (unsigned long)address < pool.base || first >= pool.span)
		return FALSE;
	RtlEnterCriticalSection(&pool.lock);
	if (pool.length[first])
	{
		MmSetAddressProtect(address, pool.length[first] * PAGE_BYTES, PAGE_READWRITE);
		memset(pool.free + first, 1, pool.length[first]);
		pool.used_pages -= pool.length[first];
		pool.length[first] = 0;
	}
	RtlLeaveCriticalSection(&pool.lock);
	return TRUE;
}

void *platform_contiguous_alloc(unsigned long size, unsigned long alignment,
	unsigned long physical_address, DWORD protect)
{
	void *result;

	size = (size + PAGE_BYTES - 1) & ~(PAGE_BYTES - 1);
	if (alignment < PAGE_BYTES)
		alignment = PAGE_BYTES;
	if (physical_address != ANY_PHYSICAL_ADDRESS)
	{
		/* (a placed block is had exactly where asked or not at all) */
		return MmAllocateContiguousMemoryEx(size, physical_address, physical_address + size - 1, alignment,
			protect);
	}
	result = MmAllocateContiguousMemoryEx(size, 0, 0xFFFFFFFF, alignment, protect);
	if (!result && pool.pages)
	{
		static BOOL logged;

		result = pool_alloc(size / PAGE_BYTES, alignment, protect);
		if (!logged || !result)
		{
			platform_log("memory: contiguous memory from the pool set aside (%lu KB %s, %lu of its %lu KB in use)",
				size / 1024, result ? "had" : "not had", pool.used_pages * (PAGE_BYTES / 1024),
				pool.pages * (PAGE_BYTES / 1024));
			logged = TRUE;
		}
	}
	return result;
}

void platform_contiguous_free(void *address)
{
	if (platform_is_contiguous(address) && !pool_free(address))
		MmFreeContiguousMemory(address);
}

/* what is free in the pool, in KB */
unsigned long xbox_contiguous_pool_free_kb(void)
{
	return (pool.pages - pool.used_pages) * (PAGE_BYTES / 1024);
}

/* the largest contiguous block to be had, in KB (for reports of its running
out: the game's heap takes the low half's free pages too) */
unsigned long xbox_contiguous_largest_kb(void)
{
	unsigned long low = 0, high = 64 * 1024 * 1024 / PAGE_BYTES;

	/* (pages: had at low, not at high) */
	while (high - low > 1)
	{
		unsigned long middle = (low + high) / 2;
		void *block = MmAllocateContiguousMemoryEx(middle * PAGE_BYTES, 0, 0xFFFFFFFF, 0, PAGE_READWRITE);

		if (block)
		{
			MmFreeContiguousMemory(block);
			low = middle;
		}
		else
			high = middle;
	}
	return low * PAGE_BYTES / 1024;
}

/* the memory left, to the log */
void xbox_memory_report(const char *when)
{
	MM_STATISTICS statistics;

	statistics.Length = sizeof(statistics);
	if (NT_SUCCESS(MmQueryStatistics(&statistics)))
		platform_log("memory: %s: %lu KB free, the largest contiguous block %lu KB, %lu KB free in the pool set aside", when,
			(unsigned long)statistics.AvailablePages * (PAGE_BYTES / 1024), xbox_contiguous_largest_kb(),
			(pool.pages - pool.used_pages) * (PAGE_BYTES / 1024));
}

/* ---------- the game state, and other virtual memory */

/* memory only the processor reads, anywhere (the sound cache: the sounds
are mixed in software); XPhysicalFree frees it */
void *xbox_virtual_allocate(unsigned long size)
{
	PVOID base = NULL;
	SIZE_T region_size = size;
	NTSTATUS status = NtAllocateVirtualMemory(&base, 0, &region_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

	if (!NT_SUCCESS(status))
	{
		platform_log("cannot have %lu KB of virtual memory (%08lx)", size / 1024, (unsigned long)status);
		return NULL;
	}
	return base;
}

/* the game state with every page backed at once (when it cannot be backed
as it is written: below) */
static void *game_state_allocate_committed(unsigned long address, unsigned long size)
{
	PVOID base = (PVOID)address;
	SIZE_T region_size = size;
	NTSTATUS status = NtAllocateVirtualMemory(&base, 0, &region_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

	if (!NT_SUCCESS(status))
	{
		platform_log("the game state: cannot have %lu KB of virtual memory at %08lx (%08lx)",
			size / 1024, address, (unsigned long)status);
		return NULL;
	}
	return base;
}

/* ---------- Halo Custom Edition's tag cache

A Custom Edition map's tags are linked to run at 0x40440000, 23 MB of them
at most (port/linux/game/cache_file_formats.h), where the Xbox maps' run at
0x803A6000, 22 MB. Only one map is loaded at a time, and there are not 23 MB
to spare, so the window is the Xbox tag cache's own pages mapped a second
time, and 1 MB more for its last megabyte. Every program runs in the
processor's most privileged mode here, so the window is made by writing the
page tables (the kernel's, mapped at 0xC0000000 as Windows NT's are), in an
address range reserved from the kernel, which never commits, frees or looks
into it: the range covers the page tables' 4 MB steps the window touches, so
no other allocation shares their page tables. */
#define PAGE_TABLES_BASE 0xC0000000UL
#define PAGE_DIRECTORY_BASE 0xC0300000UL
#define PAGE_TABLE_SPAN 0x400000UL
#define PAGE_PRESENT 0x001UL
#define PAGE_WRITABLE 0x002UL
#define PAGE_ACCESSED 0x020UL
#define PAGE_DIRTY 0x040UL
#define PAGE_LARGE 0x080UL
#define PAGE_FRAME 0xFFFFF000UL
/* (port/linux/game/cache_file_formats.h, port/linux/game/custom_edition_cache.h) */
#define CUSTOM_EDITION_TAG_CACHE_ADDRESS 0x40440000UL
#define CUSTOM_EDITION_TAG_CACHE_BYTES 0x01700000UL
#define CUSTOM_EDITION_MAP_DIRECTORY "D:\\custom_maps"

/* (port/linux/src/port_config.c) */
int config_boolean(const char *name);

static void *custom_edition_tag_cache = NULL;
/* the bytes of it the GPU reads at the Xbox tag cache's address on (all of
it, once made) */
static unsigned long custom_edition_tag_cache_gpu_bytes = 0;

static volatile unsigned long *page_directory_entry(unsigned long address)
{
	return (volatile unsigned long *)(PAGE_DIRECTORY_BASE + (address / PAGE_TABLE_SPAN) * 4);
}

static volatile unsigned long *page_table_entry(unsigned long address)
{
	return (volatile unsigned long *)(PAGE_TABLES_BASE + (address / PAGE_BYTES) * 4);
}

/* whether the page tables are where they are looked for: a page of the
program's own, which the kernel mapped, is found through them at its
physical address */
static BOOL page_tables_found(void)
{
	static unsigned long probe = 1;
	unsigned long address = (unsigned long)&probe;
	unsigned long directory = *page_directory_entry(address);
	unsigned long physical = (unsigned long)MmGetPhysicalAddress(&probe);

	if (!(directory & PAGE_PRESENT))
		return FALSE;
	if (directory & PAGE_LARGE)
		return (directory & ~(PAGE_TABLE_SPAN - 1)) == (physical & ~(PAGE_TABLE_SPAN - 1));
	return (*page_table_entry(address) & PAGE_FRAME) == (physical & PAGE_FRAME);
}

void xbox_custom_edition_tag_cache_map(void *tag_cache, unsigned long tag_cache_bytes)
{
	unsigned long first = CUSTOM_EDITION_TAG_CACHE_ADDRESS & ~(PAGE_TABLE_SPAN - 1);
	unsigned long end = (CUSTOM_EDITION_TAG_CACHE_ADDRESS + CUSTOM_EDITION_TAG_CACHE_BYTES + PAGE_TABLE_SPAN - 1) &
		~(PAGE_TABLE_SPAN - 1);
	unsigned long extra_bytes = CUSTOM_EDITION_TAG_CACHE_BYTES - tag_cache_bytes;
	unsigned long page_tables = (end - first) / PAGE_TABLE_SPAN;
	unsigned char *tables, *extra;
	unsigned long address, index;
	PVOID base = (PVOID)first;
	SIZE_T region_size = end - first;
	DWORD attributes;

	if (!config_boolean("game.custom_edition"))
		return;
#ifdef HALO_XBOX_64MB
	/* (the 64 MB console's texture cache lies in the tag cache, where a
	Custom Edition map's tags would be: halo_port_capacity.h) */
	platform_log("custom edition: not on a 64 MB console");
	return;
#endif
	/* (the 1 MB more is had only when there are Custom Edition maps) */
	attributes = GetFileAttributesA(CUSTOM_EDITION_MAP_DIRECTORY);
	if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
		return;
	if (tag_cache_bytes > CUSTOM_EDITION_TAG_CACHE_BYTES || !page_tables_found())
	{
		platform_log("custom edition: the page tables are not where they were looked for: Custom Edition maps cannot run");
		return;
	}
	if (!NT_SUCCESS(NtAllocateVirtualMemory(&base, 0, &region_size, MEM_RESERVE, PAGE_READWRITE)) ||
		(unsigned long)base != first)
	{
		platform_log("custom edition: cannot reserve %08lx-%08lx: Custom Edition maps cannot run", first, end);
		return;
	}
	/* (the last megabyte where the tag cache's pages end, so that the whole
	window is one run of physical memory, which the GPU reads at the tag
	cache's address on: structure BSPs load to the top, and their vertices
	are compressed where they lie, custom_edition_geometry.c) */
	{
		unsigned long tag_cache_end = (unsigned long)MmGetPhysicalAddress((unsigned char *)tag_cache + tag_cache_bytes - 1) + 1;

		extra = extra_bytes ?
			MmAllocateContiguousMemoryEx(extra_bytes, tag_cache_end, tag_cache_end + extra_bytes - 1, PAGE_BYTES, PAGE_READWRITE) :
			NULL;
		custom_edition_tag_cache_gpu_bytes = extra ? CUSTOM_EDITION_TAG_CACHE_BYTES : tag_cache_bytes;
	}
	tables = MmAllocateContiguousMemoryEx(page_tables * PAGE_BYTES, 0, 0xFFFFFFFF, PAGE_BYTES, PAGE_READWRITE);
	if (!tables || (extra_bytes && !extra))
	{
		platform_log("custom edition: no memory for the tag cache's last %lu KB after its pages: Custom Edition maps cannot run",
			extra_bytes / 1024);
		if (tables)
			MmFreeContiguousMemory(tables);
		if (extra)
			MmFreeContiguousMemory(extra);
		custom_edition_tag_cache_gpu_bytes = 0;
		return;
	}
	memset(tables, 0, page_tables * PAGE_BYTES);
	if (extra)
		memset(extra, 0, extra_bytes);
	for (index = 0; index < page_tables; index++)
	{
		*page_directory_entry(first + index * PAGE_TABLE_SPAN) =
			((unsigned long)MmGetPhysicalAddress(tables + index * PAGE_BYTES) & PAGE_FRAME) |
			PAGE_PRESENT | PAGE_WRITABLE | PAGE_ACCESSED;
	}
	__asm__ __volatile__("movl %%cr3, %%eax\n\tmovl %%eax, %%cr3" ::: "eax", "memory");
	for (address = CUSTOM_EDITION_TAG_CACHE_ADDRESS;
		address < CUSTOM_EDITION_TAG_CACHE_ADDRESS + CUSTOM_EDITION_TAG_CACHE_BYTES;
		address += PAGE_BYTES)
	{
		unsigned long offset = address - CUSTOM_EDITION_TAG_CACHE_ADDRESS;
		void *page = offset < tag_cache_bytes ? (unsigned char *)tag_cache + offset : extra + (offset - tag_cache_bytes);

		*page_table_entry(address) = ((unsigned long)MmGetPhysicalAddress(page) & PAGE_FRAME) |
			PAGE_PRESENT | PAGE_WRITABLE | PAGE_ACCESSED | PAGE_DIRTY;
	}
	__asm__ __volatile__("movl %%cr3, %%eax\n\tmovl %%eax, %%cr3" ::: "eax", "memory");
	/* (what is written to one is read from the other) */
	{
		volatile unsigned long *window = (volatile unsigned long *)CUSTOM_EDITION_TAG_CACHE_ADDRESS;
		volatile unsigned long *own = (volatile unsigned long *)tag_cache;
		unsigned long kept = *own;
		BOOL same;

		*window = 0x43454D50UL;
		same = *own == 0x43454D50UL;
		*own = kept;
		if (!same)
		{
			platform_log("custom edition: the tag cache window does not show the tag cache: Custom Edition maps cannot run");
			return;
		}
	}
	custom_edition_tag_cache = (void *)CUSTOM_EDITION_TAG_CACHE_ADDRESS;
	platform_log("custom edition: the tag cache at %08lx is the Xbox tag cache's pages and the %lu KB after them",
		CUSTOM_EDITION_TAG_CACHE_ADDRESS, extra_bytes / 1024);
}

unsigned long xbox_custom_edition_tag_cache_gpu_bytes(void)
{
	return custom_edition_tag_cache_gpu_bytes;
}

void *halo_custom_edition_tag_cache(void)
{
	return custom_edition_tag_cache;
}

/* ---------- memory backed as it is written

The game state's pools are sized for the largest games the native builds
play, and every machine in a game must have the same sizes
(halo_port_capacity.h), but a game uses a small part of them: in a
sixteen-player Blood Gulch, 0.9 MB of 17.7 MB held anything. The platform
layer's largest static arrays are sized the same way (HALO_PORT_SPARSE). So
these ranges are sparse: each of their pages is at first the one page of
zeros, read only, and a page is given memory of its own when it is first
written. Every program here runs in the processor's most privileged mode,
and the page fault is looked at here before the kernel's handler has it.
The faults take pages from a stock refilled between frames (the kernel's
allocator is called from the fault only when the stock is out). Pages that
hold only zeros again, as each map's pools are emptied, go back to the page
of zeros between frames (xbox_game_state_frame).

A device writing to a sparse page that was never written would write the
page of zeros itself, which every unwritten page reads: files are read into
sparse ranges through a buffer of their own (xbox_sparse_read_through), and
no static array a device writes to is made sparse. */
#define GAME_STATE_ON_DEMAND 1
#define GAME_STATE_MAXIMUM_PAGES (0x02000000UL / PAGE_BYTES)
#define SPARSE_MAXIMUM_RANGES 128
/* the heap's large blocks (xbox_sparse_allocate), in an address range of
their own */
#define SPARSE_HEAP_BASE 0x44000000UL
#define SPARSE_HEAP_BYTES 0x04000000UL
#ifdef HALO_XBOX_64MB
/* (a fault with the stock out asks the kernel itself: the 64 MB console
keeps less idle) */
#define STOCK_TARGET_PAGES 64
#else
#define STOCK_TARGET_PAGES 256
#endif
#define STOCK_MAXIMUM_PAGES 1024
/* memory running short: pages held back for the faults the kernel has no
page for, which carry the game on until the frame ends and the game is
ended (xbox_memory_short, main.c); and the free memory under which the
game is ended between frames, before any allocation fails */
#define RESERVE_PAGES 128
#define LOW_WATER_PAGES 256
#define CR0_WRITE_PROTECT 0x00010000UL
#define PAGE_FAULT_VECTOR 14

struct sparse_range
{
	unsigned long base, end;
	/* each page's own memory (from MmAllocateSystemMemory), or NULL */
	void **pages;
};

static struct
{
	/* an unwritten page's page table entry: the page of zeros, read only
	(or, without the processor's write protection, not present) */
	unsigned long zero_entry;
	/* the game state's, then the static arrays' */
	struct sparse_range ranges[SPARSE_MAXIMUM_RANGES];
	unsigned long range_count;
	/* the static arrays named before the game state is made
	(xbox_sparse_register: constructors) */
	struct
	{
		void *address;
		unsigned long size;
	} statics[64];
	unsigned long static_count;
	void *stock[STOCK_MAXIMUM_PAGES];
	unsigned long stock_count;
	void *reserve[RESERVE_PAGES];
	unsigned long reserve_count;
	/* (xbox_memory_short: the reserve in use, or free memory under the low
	water mark, as the last frame ended) */
	int short_of_memory;
	unsigned long backed, most_backed, faults, faults_unstocked, fault_races, reclaimed;
} demand;

static void *game_state_pages[GAME_STATE_MAXIMUM_PAGES];

static BOOL sparse_heap_start(void);

/* (the kernel's page fault handler: the entry below goes on to it) */
unsigned long xbox_page_fault_original;
void xbox_page_fault_entry(void);
int xbox_sparse_fault(unsigned long address, unsigned long error);

/* the page fault: the processor has pushed the error code, then the
fault's flags, code segment and address to return to */
__asm__(
	".text\n"
	".globl _xbox_page_fault_entry\n"
	"_xbox_page_fault_entry:\n"
	"\tpushal\n"
	"\tcld\n"
	"\tmovl 32(%esp), %ecx\n"
	"\tmovl %cr2, %eax\n"
	"\tpushl %ecx\n"
	"\tpushl %eax\n"
	"\tcall _xbox_sparse_fault\n"
	"\taddl $8, %esp\n"
	"\ttestl %eax, %eax\n"
	"\tpopal\n"
	"\tjz 1f\n"
	"\taddl $4, %esp\n"
	"\tiretl\n"
	"1:\n"
	"\tjmp *_xbox_page_fault_original\n");

static unsigned long interrupts_disable(void)
{
	unsigned long flags;

	__asm__ __volatile__("pushfl\n\tpopl %0\n\tcli" : "=r"(flags) :: "memory");
	return flags;
}

static void interrupts_restore(unsigned long flags)
{
	__asm__ __volatile__("pushl %0\n\tpopfl" :: "r"(flags) : "memory", "cc");
}

static void page_invalidate(unsigned long address)
{
	__asm__ __volatile__("invlpg (%0)" :: "r"(address) : "memory");
}

/* (no SSE in the fault: the faulting code's registers are not saved) */
static void page_zero(void *page)
{
	unsigned long count = PAGE_BYTES / 4;

	__asm__ __volatile__("rep stosl" : "+D"(page), "+c"(count) : "a"(0) : "memory");
}

/* a page of the kernel's for a sparse range, and back. The kernel moves
pages it has given out, fixing up the mappings it knows of, to make room
for a contiguous allocation (on a 64 MB console, often): the entries made
here it does not know of, and would go on mapping the old page, someone
else's by then. So the pages are locked where they are while they are the
ranges' */
static void *page_take(void)
{
	void *memory = MmAllocateSystemMemory(PAGE_BYTES, PAGE_READWRITE);

	while (memory)
	{
		ULONG_PTR physical = (ULONG_PTR)MmGetPhysicalAddress(memory);

		MmLockUnlockPhysicalPage(physical, FALSE);
		/* (moved before it was locked: the lock is on the old page) */
		if ((ULONG_PTR)MmGetPhysicalAddress(memory) == physical)
			break;
		MmLockUnlockPhysicalPage(physical, TRUE);
	}
	return memory;
}

static void page_give(void *memory)
{
	MmLockUnlockPhysicalPage((ULONG_PTR)MmGetPhysicalAddress(memory), TRUE);
	MmFreeSystemMemory(memory, PAGE_BYTES);
}

static BOOL page_is_zero(const void *page)
{
	const unsigned long *words = (const unsigned long *)page;
	unsigned long index;

	for (index = 0; index < PAGE_BYTES / 4; index++)
	{
		if (words[index])
			return FALSE;
	}
	return TRUE;
}

static struct sparse_range *sparse_range_find(unsigned long address)
{
	unsigned long index;

	for (index = 0; index < demand.range_count; index++)
	{
		if (address >= demand.ranges[index].base && address < demand.ranges[index].end)
			return &demand.ranges[index];
	}
	return NULL;
}

__attribute__((used)) int xbox_sparse_fault(unsigned long address, unsigned long error)
{
	unsigned long page = address & ~(PAGE_BYTES - 1);
	struct sparse_range *range = sparse_range_find(address);
	unsigned long index;
	void *memory;

	(void)error;
	if (!range)
		return 0;
	index = (page - range->base) / PAGE_BYTES;
	if (range->pages[index])
	{
		/* (backed already: the processor still had the old entry) */
		page_invalidate(page);
		return 1;
	}
	if (demand.stock_count)
	{
		memory = demand.stock[--demand.stock_count];
	}
	else
	{
		memory = page_take();
		demand.faults_unstocked++;
		if (!memory && demand.reserve_count)
			memory = demand.reserve[--demand.reserve_count];
		if (!memory)
			return 0;
		/* (the kernel's allocator can let another thread run, which may have
		freed a block, moving the ranges, or faulted on this page meanwhile:
		its memory is kept) */
		range = sparse_range_find(address);
		if (!range || range->pages[(page - range->base) / PAGE_BYTES])
		{
			if (demand.stock_count < STOCK_MAXIMUM_PAGES)
				demand.stock[demand.stock_count++] = memory;
			demand.fault_races++;
			page_invalidate(page);
			return range != NULL;
		}
		index = (page - range->base) / PAGE_BYTES;
	}
	page_zero(memory);
	range->pages[index] = memory;
	*page_table_entry(page) = ((unsigned long)MmGetPhysicalAddress(memory) & PAGE_FRAME) |
		PAGE_PRESENT | PAGE_WRITABLE | PAGE_ACCESSED | PAGE_DIRTY;
	page_invalidate(page);
	demand.faults++;
	if (++demand.backed > demand.most_backed)
		demand.most_backed = demand.backed;
	return 1;
}

static void stock_refill(void)
{
	/* (the reserve first) */
	while (demand.reserve_count < RESERVE_PAGES)
	{
		void *memory = page_take();
		unsigned long flags;

		if (!memory)
			break;
		flags = interrupts_disable();
		demand.reserve[demand.reserve_count++] = memory;
		interrupts_restore(flags);
	}
	while (demand.stock_count < STOCK_TARGET_PAGES)
	{
		void *memory = page_take();
		unsigned long flags;

		if (!memory)
			break;
		flags = interrupts_disable();
		demand.stock[demand.stock_count++] = memory;
		interrupts_restore(flags);
	}
}

/* pages holding only zeros back to the page of zeros, and the stock kept
at its size */
static void sparse_reclaim(void)
{
	unsigned long range_index, index;

	for (range_index = 0; range_index < demand.range_count; range_index++)
	{
		struct sparse_range *range = &demand.ranges[range_index];

		for (index = 0; index < (range->end - range->base) / PAGE_BYTES; index++)
		{
			void *memory = range->pages[index], *release = NULL;
			unsigned long flags;

			if (!memory || !page_is_zero(memory))
				continue;
			/* (looked at again with nothing else running, then taken away) */
			flags = interrupts_disable();
			if (page_is_zero(memory))
			{
				unsigned long page = range->base + index * PAGE_BYTES;

				*page_table_entry(page) = demand.zero_entry;
				page_invalidate(page);
				range->pages[index] = NULL;
				demand.backed--;
				demand.reclaimed++;
				if (demand.stock_count < STOCK_MAXIMUM_PAGES)
					demand.stock[demand.stock_count++] = memory;
				else
					release = memory;
			}
			interrupts_restore(flags);
			if (release)
				page_give(release);
		}
	}
	while (demand.stock_count > STOCK_TARGET_PAGES)
	{
		unsigned long flags = interrupts_disable();
		void *memory = demand.stock_count > STOCK_TARGET_PAGES ? demand.stock[--demand.stock_count] : NULL;

		interrupts_restore(flags);
		if (memory)
			page_give(memory);
	}
}

/* page tables for the 4 MB steps of [first, end) the kernel has none for */
static BOOL page_tables_make(unsigned long first, unsigned long end)
{
	unsigned long address;

	/* (a page each: little contiguous memory is left once the caches are had) */
	for (address = first; address < end; address += PAGE_TABLE_SPAN)
	{
		unsigned char *table = MmAllocateContiguousMemoryEx(PAGE_BYTES, 0, 0xFFFFFFFF, PAGE_BYTES, PAGE_READWRITE);

		if (!table)
		{
			platform_log("memory: no page for the page table of %08lx", address);
			return FALSE;
		}
		memset(table, 0, PAGE_BYTES);
		*page_directory_entry(address) = ((unsigned long)MmGetPhysicalAddress(table) & PAGE_FRAME) |
			PAGE_PRESENT | PAGE_WRITABLE | PAGE_ACCESSED;
	}
	__asm__ __volatile__("movl %%cr3, %%eax\n\tmovl %%eax, %%cr3" ::: "eax", "memory");
	return TRUE;
}

/* the page fault's gate in the interrupt descriptor table, written with
the write protection off (the table may be in a read-only page) */
static void page_fault_hook(void)
{
	unsigned char descriptor[8];
	unsigned short *gate;
	unsigned long cr0, flags;

	__asm__ __volatile__("sidt %0" : "=m"(*(unsigned char (*)[6])(descriptor + 2)));
	gate = (unsigned short *)(*(unsigned long *)(descriptor + 4) + PAGE_FAULT_VECTOR * 8);
	flags = interrupts_disable();
	__asm__ __volatile__("movl %%cr0, %0" : "=r"(cr0));
	xbox_page_fault_original = gate[0] | ((unsigned long)gate[3] << 16);
	__asm__ __volatile__("movl %0, %%cr0" :: "r"(cr0 & ~CR0_WRITE_PROTECT) : "memory");
	gate[0] = (unsigned short)((unsigned long)xbox_page_fault_entry & 0xFFFF);
	gate[3] = (unsigned short)((unsigned long)xbox_page_fault_entry >> 16);
	__asm__ __volatile__("movl %0, %%cr0" :: "r"(cr0) : "memory");
	interrupts_restore(flags);
}

/* a static array of the platform layer's (HALO_PORT_SPARSE, from a
constructor: before main) */
void xbox_sparse_register(void *address, unsigned long size)
{
	if (demand.static_count < sizeof(demand.statics) / sizeof(demand.statics[0]))
	{
		demand.statics[demand.static_count].address = address;
		demand.statics[demand.static_count].size = size;
		demand.static_count++;
	}
}

/* the static arrays' whole pages that hold only zeros given back to the
kernel, and made sparse */
static void sparse_statics_start(void)
{
	unsigned long index, given = 0, kept = 0;

	for (index = 0; index < demand.static_count && demand.range_count < SPARSE_MAXIMUM_RANGES; index++)
	{
		unsigned long first = ((unsigned long)demand.statics[index].address + PAGE_BYTES - 1) & ~(PAGE_BYTES - 1);
		unsigned long end = ((unsigned long)demand.statics[index].address + demand.statics[index].size) &
			~(PAGE_BYTES - 1);
		struct sparse_range *range;
		unsigned long page;

		if (end <= first)
			continue;
		range = &demand.ranges[demand.range_count];
		range->pages = calloc((end - first) / PAGE_BYTES, sizeof(void *));
		if (!range->pages)
			continue;
		range->base = first;
		range->end = end;
		/* (pages not given back stay the program's: never faulted, never
		taken back) */
		for (page = first; page < end; page += PAGE_BYTES)
		{
			PVOID base = (PVOID)page;
			SIZE_T region_size = PAGE_BYTES;
			unsigned long flags;

			if (!page_is_zero((void *)page))
			{
				kept++;
				continue;
			}
			flags = interrupts_disable();
			if (page_is_zero((void *)page) &&
				NT_SUCCESS(NtFreeVirtualMemory(&base, &region_size, MEM_DECOMMIT)))
			{
				*page_table_entry(page) = demand.zero_entry;
				page_invalidate(page);
				given++;
			}
			else
			{
				kept++;
			}
			interrupts_restore(flags);
		}
		demand.range_count++;
	}
	platform_log("memory: %lu static arrays sparse, %lu KB given back, %lu KB kept", demand.static_count, given * 4,
		kept * 4);
}

static BOOL game_state_on_demand_start(unsigned long address, unsigned long size)
{
	unsigned long first = address & ~(PAGE_TABLE_SPAN - 1);
	unsigned long end = (address + size + PAGE_TABLE_SPAN - 1) & ~(PAGE_TABLE_SPAN - 1);
	unsigned long index, cr0;
	PVOID base = (PVOID)first;
	SIZE_T region_size = end - first;

	if (size > GAME_STATE_MAXIMUM_PAGES * PAGE_BYTES || (address | size) & (PAGE_BYTES - 1) || !page_tables_found())
		return FALSE;
	if (!NT_SUCCESS(NtAllocateVirtualMemory(&base, 0, &region_size, MEM_RESERVE, PAGE_READWRITE)) ||
		(unsigned long)base != first)
	{
		platform_log("the game state: cannot reserve %08lx-%08lx", first, end);
		return FALSE;
	}
	__asm__ __volatile__("movl %%cr0, %0" : "=r"(cr0));
	if (cr0 & CR0_WRITE_PROTECT)
	{
		void *zero = page_take();

		if (!zero)
			return FALSE;
		page_zero(zero);
		demand.zero_entry = ((unsigned long)MmGetPhysicalAddress(zero) & PAGE_FRAME) | PAGE_PRESENT | PAGE_ACCESSED;
	}
	if (!page_tables_make(first, end))
		return FALSE;
	for (index = 0; index < size / PAGE_BYTES; index++)
		*page_table_entry(address + index * PAGE_BYTES) = demand.zero_entry;
	__asm__ __volatile__("movl %%cr3, %%eax\n\tmovl %%eax, %%cr3" ::: "eax", "memory");
	demand.ranges[0].base = address;
	demand.ranges[0].end = address + size;
	demand.ranges[0].pages = game_state_pages;
	demand.range_count = 1;
	stock_refill();
	page_fault_hook();
	platform_log("the game state: %lu KB at %08lx, backed as it is written (%s); the kernel's page fault handler at %08lx",
		size / 1024, address, demand.zero_entry ? "unwritten pages read the page of zeros" : "unwritten pages not present",
		xbox_page_fault_original);
	/* (the static arrays only with the page of zeros: a page not present
	would fault on every read) */
	if (demand.zero_entry)
	{
		sparse_statics_start();
		sparse_heap_start();
	}
	return TRUE;
}

void *xbox_game_state_allocate(unsigned long address, unsigned long size)
{
	if (GAME_STATE_ON_DEMAND && game_state_on_demand_start(address, size))
		return (void *)address;
	return game_state_allocate_committed(address, size);
}

/* ---------- the heap's large blocks

Blocks of the game's heap of 256 KB or more (GlobalAlloc: the game's
debug_malloc) are sparse too: the AI's and the renderer's debug records, a
few pools sized for the worst case (xbox_kernel.c). Each is a range of its
own in [SPARSE_HEAP_BASE, +SPARSE_HEAP_BYTES), which has its own page
tables and is reserved from the kernel. */
static BOOL sparse_heap_ready;
/* (taken while the ranges are added, removed or walked between frames; the
fault reads them with interrupts off) */
static RTL_CRITICAL_SECTION sparse_heap_lock;

static BOOL sparse_heap_start(void)
{
	PVOID base = (PVOID)SPARSE_HEAP_BASE;
	SIZE_T region_size = SPARSE_HEAP_BYTES;

	if (!demand.zero_entry)
		return FALSE;
	if (!NT_SUCCESS(NtAllocateVirtualMemory(&base, 0, &region_size, MEM_RESERVE, PAGE_READWRITE)) ||
		(unsigned long)base != SPARSE_HEAP_BASE)
	{
		platform_log("memory: cannot reserve %08lx-%08lx for the heap's large blocks", SPARSE_HEAP_BASE,
			SPARSE_HEAP_BASE + SPARSE_HEAP_BYTES);
		return FALSE;
	}
	if (!page_tables_make(SPARSE_HEAP_BASE, SPARSE_HEAP_BASE + SPARSE_HEAP_BYTES))
		return FALSE;
	platform_log("memory: blocks of the heap of 256 KB or more sparse, at %08lx", SPARSE_HEAP_BASE);
	RtlInitializeCriticalSection(&sparse_heap_lock);
	sparse_heap_ready = TRUE;
	return TRUE;
}

void *xbox_sparse_allocate(unsigned long size)
{
	unsigned long pages = (size + PAGE_BYTES - 1) / PAGE_BYTES, address = SPARSE_HEAP_BASE, page, flags;
	void **backing;
	struct sparse_range *range;

	if (!sparse_heap_ready || demand.range_count >= SPARSE_MAXIMUM_RANGES)
		return NULL;
	backing = calloc(pages, sizeof(void *));
	if (!backing)
		return NULL;
	RtlEnterCriticalSection(&sparse_heap_lock);
	/* the lowest gap between the blocks there are (with a page between
	blocks, so an overrun faults) */
	flags = interrupts_disable();
	for (;;)
	{
		unsigned long index, next = address;

		for (index = 1; index < demand.range_count; index++)
		{
			range = &demand.ranges[index];
			if (range->base >= SPARSE_HEAP_BASE && range->base < address + (pages + 1) * PAGE_BYTES &&
				range->end + PAGE_BYTES > address)
				next = range->end + PAGE_BYTES > next ? range->end + PAGE_BYTES : next;
		}
		if (next == address)
			break;
		address = next;
	}
	if (address + pages * PAGE_BYTES > SPARSE_HEAP_BASE + SPARSE_HEAP_BYTES)
	{
		interrupts_restore(flags);
		RtlLeaveCriticalSection(&sparse_heap_lock);
		free(backing);
		return NULL;
	}
	range = &demand.ranges[demand.range_count];
	range->base = address;
	range->end = address + pages * PAGE_BYTES;
	range->pages = backing;
	for (page = range->base; page < range->end; page += PAGE_BYTES)
	{
		*page_table_entry(page) = demand.zero_entry;
		page_invalidate(page);
	}
	demand.range_count++;
	interrupts_restore(flags);
	RtlLeaveCriticalSection(&sparse_heap_lock);
	return (void *)address;
}

/* the size of a block of xbox_sparse_allocate's, or 0 */
unsigned long xbox_sparse_size(const void *address)
{
	struct sparse_range *range = sparse_heap_ready ? sparse_range_find((unsigned long)address) : NULL;

	return range && range->base == (unsigned long)address && range->base >= SPARSE_HEAP_BASE ?
		range->end - range->base : 0;
}

/* [address, address+size) made zeros: in a sparse range, its whole pages
given back to the page of zeros instead of written (HALO_PORT_SPARSE_ZERO) */
void xbox_sparse_zero(void *address, unsigned long size)
{
	unsigned long start = (unsigned long)address, end = start + size, page;
	unsigned long first = (start + PAGE_BYTES - 1) & ~(PAGE_BYTES - 1), last = end & ~(PAGE_BYTES - 1);
	/* (by its first whole page: a static array's range starts there) */
	struct sparse_range *range = last > first ? sparse_range_find(first) : NULL;

	if (!range || last > range->end)
	{
		memset(address, 0, size);
		return;
	}
	memset(address, 0, first - start);
	memset((void *)last, 0, end - last);
	for (page = first; page < last; page += PAGE_BYTES)
	{
		unsigned long index = (page - range->base) / PAGE_BYTES, flags;
		void *memory, *release = NULL;

		if (!range->pages[index])
			continue;
		flags = interrupts_disable();
		memory = range->pages[index];
		if (memory)
		{
			*page_table_entry(page) = demand.zero_entry;
			page_invalidate(page);
			range->pages[index] = NULL;
			demand.backed--;
			demand.reclaimed++;
			if (demand.stock_count < STOCK_MAXIMUM_PAGES)
				demand.stock[demand.stock_count++] = memory;
			else
				release = memory;
		}
		interrupts_restore(flags);
		if (release)
			page_give(release);
	}
}

/* whether an address lies in a block of xbox_sparse_allocate's */
int xbox_sparse_contains(const void *address)
{
	return sparse_heap_ready && (unsigned long)address >= SPARSE_HEAP_BASE &&
		(unsigned long)address < SPARSE_HEAP_BASE + SPARSE_HEAP_BYTES;
}

void xbox_sparse_free(void *address)
{
	struct sparse_range *range;
	unsigned long index, page, pages, flags;
	void **backing;

	if (!xbox_sparse_size(address))
		return;
	RtlEnterCriticalSection(&sparse_heap_lock);
	flags = interrupts_disable();
	range = sparse_range_find((unsigned long)address);
	backing = range->pages;
	for (page = range->base; page < range->end; page += PAGE_BYTES)
	{
		void *memory = backing[(page - range->base) / PAGE_BYTES];

		*page_table_entry(page) = 0;
		page_invalidate(page);
		if (memory)
		{
			demand.backed--;
			if (demand.stock_count < STOCK_MAXIMUM_PAGES)
			{
				demand.stock[demand.stock_count++] = memory;
				backing[(page - range->base) / PAGE_BYTES] = NULL;
			}
		}
	}
	pages = (range->end - range->base) / PAGE_BYTES;
	index = range - demand.ranges;
	memmove(range, range + 1, (demand.range_count - index - 1) * sizeof(*range));
	demand.range_count--;
	interrupts_restore(flags);
	RtlLeaveCriticalSection(&sparse_heap_lock);
	/* (the pages the stock had no room for) */
	for (index = 0; index < pages; index++)
	{
		if (backing[index])
			page_give(backing[index]);
	}
	free(backing);
}

static BOOL game_state_on_demand_contains(const void *address)
{
	return demand.range_count && (unsigned long)address >= demand.ranges[0].base &&
		(unsigned long)address < demand.ranges[0].end;
}

/* whether [address, address+size) meets a sparse range */
int xbox_sparse_overlaps(const void *address, unsigned long size)
{
	unsigned long index;

	for (index = 0; index < demand.range_count; index++)
	{
		if ((unsigned long)address < demand.ranges[index].end &&
			(unsigned long)address + size > demand.ranges[index].base)
			return TRUE;
	}
	return FALSE;
}

/* a file's bytes read into a sparse range: through a buffer of their own,
and only the pages not all zeros (or already backed) written */
long xbox_sparse_read_through(void *destination, unsigned long size,
	long (*read)(void *context, void *buffer, unsigned long size), void *context)
{
	enum { CHUNK = 64 * 1024 };
	unsigned char *buffer = malloc(CHUNK);
	unsigned long done = 0;

	if (!buffer)
		return -1;
	while (done < size)
	{
		unsigned long want = size - done < CHUNK ? size - done : CHUNK;
		long got = read(context, buffer, want);
		unsigned long at = 0;

		if (got < 0)
		{
			free(buffer);
			return done ? (long)done : -1;
		}
		while (at < (unsigned long)got)
		{
			unsigned char *target = (unsigned char *)destination + done + at;
			unsigned long run = PAGE_BYTES - ((unsigned long)target & (PAGE_BYTES - 1));
			struct sparse_range *range = sparse_range_find((unsigned long)target);

			if (run > (unsigned long)got - at)
				run = (unsigned long)got - at;
			/* (a whole page of zeros over a page never written: left as it is) */
			if (!(run == PAGE_BYTES && range && !range->pages[((unsigned long)target - range->base) / PAGE_BYTES] &&
				page_is_zero(buffer + at)))
				memcpy(target, buffer + at, run);
			at += run;
		}
		done += (unsigned long)got;
		if ((unsigned long)got < want)
			break;
	}
	free(buffer);
	return (long)done;
}

/* between frames: the stock refilled, and now and then the zeroed pages
taken back and the use logged */
void xbox_game_state_frame(unsigned long frame)
{
	MM_STATISTICS statistics;
	int short_of_memory;

	if (!demand.range_count)
		return;
	stock_refill();
	statistics.Length = sizeof(statistics);
	statistics.AvailablePages = 0;
	short_of_memory = demand.reserve_count < RESERVE_PAGES ||
		(NT_SUCCESS(MmQueryStatistics(&statistics)) && statistics.AvailablePages < LOW_WATER_PAGES);
	if (short_of_memory && !demand.short_of_memory)
		platform_log("memory: short (%lu KB free, %lu KB of the reserve left)",
			(unsigned long)statistics.AvailablePages * (PAGE_BYTES / 1024), demand.reserve_count * (PAGE_BYTES / 1024));
	demand.short_of_memory = short_of_memory;
	if (frame % 300 == 0)
	{
		if (sparse_heap_ready)
			RtlEnterCriticalSection(&sparse_heap_lock);
		sparse_reclaim();
		if (sparse_heap_ready)
			RtlLeaveCriticalSection(&sparse_heap_lock);
		platform_log("sparse memory: %lu KB backed, at most %lu KB; %lu faults, %lu without stock (%lu raced); %lu KB taken back",
			demand.backed * 4, demand.most_backed * 4, demand.faults, demand.faults_unstocked, demand.fault_races,
			demand.reclaimed * 4);
	}
}

/* whether memory ran short as the last frame ended (port/xbox/include/
xbox_memory.h; main.c ends the game) */
int xbox_memory_short(void)
{
	return demand.short_of_memory;
}

/* ---------- XAPI */

LPVOID WINAPI XPhysicalAlloc(SIZE_T size, ULONG_PTR physical_address, ULONG_PTR alignment, DWORD protect)
{
	/* as port/linux/src/xbox_memory.c: a highest-acceptable address inside
	physical memory places the block there (the tag cache) */
	void *result = platform_contiguous_alloc(size, alignment,
		physical_address < CONTIGUOUS_SIZE ? physical_address : ANY_PHYSICAL_ADDRESS, protect);

	if (!result)
	{
		platform_log("XPhysicalAlloc: cannot allocate %lu KB (physical address 0x%08lx)",
			(unsigned long)size / 1024, (unsigned long)physical_address);
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
	}
	return result;
}

VOID WINAPI XPhysicalFree(LPVOID address)
{
	/* (the game state backed as it is written is kept to the end) */
	if (game_state_on_demand_contains(address))
		return;
	if (platform_is_contiguous(address))
	{
		platform_contiguous_free(address);
	}
	else if (address)
	{
		/* the game state and the sound cache (xbox_game_state_allocate, xbox_virtual_allocate) */
		PVOID base = address;
		SIZE_T region_size = 0;

		NtFreeVirtualMemory(&base, &region_size, MEM_RELEASE);
	}
}

/* (VirtualProtect under the name the game and the platform layer call it
by: halo_xbox_prefix.h) */
BOOL WINAPI halo_xbox_VirtualProtect(LPVOID address, SIZE_T size, DWORD new_protect, PDWORD old_protect)
{
	/* (the game state backed as it is written keeps its own page tables:
	its pages stay as they are) */
	if (game_state_on_demand_contains(address))
	{
		if (old_protect)
			*old_protect = PAGE_READWRITE;
		return TRUE;
	}
	if (old_protect)
		*old_protect = MmQueryAddressProtect(address);
	if (platform_is_contiguous(address))
	{
		MmSetAddressProtect(address, size, new_protect);
	}
	else
	{
		PVOID base = address;
		SIZE_T region_size = size;
		ULONG previous;

		if (!NT_SUCCESS(NtProtectVirtualMemory(&base, &region_size, new_protect, &previous)))
		{
			SetLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
	}
	return TRUE;
}

VOID WINAPI XPhysicalProtect(LPVOID address, SIZE_T size, DWORD new_protect)
{
	halo_xbox_VirtualProtect(address, size, new_protect, NULL);
}

DWORD WINAPI XQueryMemoryProtect(LPVOID address)
{
	if (game_state_on_demand_contains(address))
		return PAGE_READWRITE;
	return MmQueryAddressProtect(address);
}
