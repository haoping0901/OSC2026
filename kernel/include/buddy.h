#ifndef __BUDDY_H__
#define __BUDDY_H__

#include "types.h"

/* ---------- Constants --------------------------------------------------- */

#define PAGE_SIZE   4096UL
#define MAX_ORDER   16          /* 2^16 pages = 256 MiB per block */

/* Frame-array sentinel values */
#define FRAME_FREE_PART  (-1)   /* free but belongs to a larger block */
#define FRAME_RESERVED   (-2)   /* reserved; must never be allocated */

/*
 * Static frame-array capacity.
 * Sized for the OrangePi RV2 first memory region (2 GiB).
 * 2 GiB / 4 KiB = 524288 entries × 4 bytes = 2 MiB in BSS — acceptable.
 */
#define MAX_PAGES   (2UL * 1024UL * 1024UL * 1024UL / PAGE_SIZE)   /* 524288 */

/* ---------- Public API -------------------------------------------------- */

/**
 * buddy_init - record the managed memory region and mark all pages FREE_PART.
 * Does NOT build free lists yet; call buddy_build_free_lists() after all
 * buddy_reserve() calls.
 *
 * @base: physical start address (page-aligned)
 * @size: byte length of the region
 */
void buddy_init(uintptr_t base, uintptr_t size);

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
