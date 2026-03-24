#ifndef __KMALLOC_H__
#define __KMALLOC_H__

#include "types.h"

/* ---------- Pool configuration ------------------------------------------ */

/*
 * Chunk pool sizes (bytes).  Requests are rounded up to the nearest pool size.
 * Requests larger than MAX_CHUNK_SIZE fall through directly to buddy_alloc.
 */
#define NUM_POOLS       8
#define MAX_CHUNK_SIZE  2048

/* ---------- Public API -------------------------------------------------- */

/**
 * Initialize the dynamic memory allocator.
 * Must be called once at boot, after buddy_init().
 */
void kmalloc_init(void);

/**
 * Allocate at least @size bytes of memory.
 *   - size <= MAX_CHUNK_SIZE  → served from chunk pools
 *   - size >  MAX_CHUNK_SIZE  → delegated to buddy_alloc
 * Returns NULL on failure.
 */
void *kmalloc(unsigned long size);

/**
 * Free memory previously returned by kmalloc().
 */
void kfree(void *ptr);

#endif /* __KMALLOC_H__ */
