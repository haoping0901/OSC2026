#ifndef __BUDDY_H__
#define __BUDDY_H__

#include "types.h"

/* ---------- Constants --------------------------------------------------- */

#define PAGE_SIZE        4096UL
#define MAX_ORDER        16          /* 2^16 pages = 256 MiB */

/* Frame-array sentinel values */
#define FRAME_FREE_PART  (-1)        /* <F>: free but belongs to larger block */

/* Managed physical memory region (basic exercise, hardcoded).
 *
 * QEMU virt : RAM at 0x80000000; use 0x90000000–0xA0000000 (safe).
 * OrangePi  : use 0x10000000–0x20000000 as suggested by the lab spec.
 */
#ifdef QEMU
#define BUDDY_BASE       0x90000000UL
#define BUDDY_END        0xA0000000UL
#else
#define BUDDY_BASE       0x10000000UL
#define BUDDY_END        0x20000000UL
#endif
#define TOTAL_PAGES      ((BUDDY_END - BUDDY_BASE) / PAGE_SIZE)  /* 65536 */

/* ---------- Public API -------------------------------------------------- */

/**
 * Initialise the buddy allocator.
 * Must be called once at boot, after UART is ready (for log output).
 */
void buddy_init(void);

/**
 * Allocate contiguous page-aligned memory of at least @size bytes.
 * Returns NULL on failure.
 */
void *buddy_alloc(unsigned long size);

/**
 * Free memory previously returned by buddy_alloc().
 */
void buddy_free(void *ptr);

#endif /* __BUDDY_H__ */
