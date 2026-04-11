#ifndef __BUDDY_H__
#define __BUDDY_H__

#include "types.h"

/* ---------- Constants --------------------------------------------------- */

#define PAGE_SIZE   4096UL
#define MAX_ORDER   16          /* 2^16 pages = 256 MiB per block */

/* Frame-array sentinel values */
#define FRAME_FREE_PART  (-1)   /* free but belongs to a larger block */
#define FRAME_RESERVED   (-2)   /* reserved; must never be allocated */

/* Maximum number of reserved regions the startup allocator can track. */
#define BUDDY_STARTUP_MAX_RESERVES  16

/* ---------- Startup Allocator API --------------------------------------- */

/**
 * buddy_startup_init() – initialise the bump-pointer startup allocator.
 *
 * Must be called before any buddy_startup_reserve() or buddy_startup_alloc()
 * calls, and before buddy_init().
 *
 * @mem_base: physical start address of the usable memory region
 * @mem_size: byte length of the usable memory region
 */
void buddy_startup_init(uintptr_t mem_base, uintptr_t mem_size);

/**
 * buddy_startup_reserve() – register a region that must not be allocated.
 *
 * Must be called after buddy_startup_init() and before buddy_startup_alloc().
 * Addresses are rounded to 4 KiB boundaries automatically.
 *
 * @start: physical start address of the reserved region
 * @end:   physical end address of the reserved region (exclusive)
 */
void buddy_startup_reserve(uintptr_t start, uintptr_t end);

/**
 * buddy_startup_alloc() – allocate a 4 KiB-aligned block of memory.
 *
 * Scans for the first free gap that is large enough, then bumps the
 * internal pointer forward.  Cannot be called after buddy_init().
 *
 * @size:   minimum byte length required (rounded up to a PAGE_SIZE multiple)
 * @return: pointer to the allocated region, or NULL on failure
 */
void *buddy_startup_alloc(unsigned long size);

/**
 * buddy_startup_replay_reserves() – replay all regions registered via
 * buddy_startup_reserve() (including allocations made by buddy_startup_alloc())
 * into the buddy system via buddy_reserve().
 *
 * Must be called after buddy_init() and before buddy_build_free_lists(),
 * replacing the manual buddy_reserve() calls for each individual region.
 */
void buddy_startup_replay_reserves(void);

/* ---------- Public API -------------------------------------------------- */

/**
 * buddy_init - record the managed memory region and mark all pages FREE_PART.
 * Does NOT build free lists yet; call buddy_build_free_lists() after all
 * buddy_reserve() calls.
 *
 * @base:        physical start address (page-aligned)
 * @size:        byte length of the region
 * @frame_array: pointer to an int array of @frame_count elements,
 *               allocated by buddy_startup_alloc()
 * @frame_count: number of page frames in the region (size / PAGE_SIZE)
 */
void buddy_init(uintptr_t base, uintptr_t size,
                int *frame_array, unsigned long frame_count);

/**
 * buddy_reserve - mark all 4 KiB pages overlapping [start, end) as RESERVED.
 * Must be called after buddy_init() and before buddy_build_free_lists().
 *
 * @start: physical start address (will be rounded down to page boundary)
 * @end:   physical end address   (will be rounded up   to page boundary)
 */
void buddy_reserve(uintptr_t start, uintptr_t end);

/**
 * buddy_build_free_lists - build the buddy free lists from all non-reserved
 * pages.  Must be called once after all buddy_reserve() calls.
 */
void buddy_build_free_lists(void);

/** Return the physical base address of the managed region. */
uintptr_t buddy_get_base(void);

/** Return the total number of pages in the managed region. */
unsigned long buddy_get_total_pages(void);

/**
 * buddy_alloc - allocate contiguous page-aligned memory of at least @size bytes.
 * Returns NULL on failure.
 */
void *buddy_alloc(unsigned long size);

/**
 * buddy_free - free memory previously returned by buddy_alloc().
 */
void buddy_free(void *ptr);

#endif /* __BUDDY_H__ */
