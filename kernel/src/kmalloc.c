#include "kmalloc.h"
#include "buddy.h"
#include "uart.h"
#include "utils.h"

/* ===== Pool definitions =================================================
 *
 * Each pool manages chunks of one fixed size.  Free chunks are kept on a
 * singly-linked list embedded inside the chunk memory itself (the chunk is
 * unused while on the free list, so this is safe and costs zero extra space).
 *
 * When a pool runs out of free chunks, it requests a fresh 4 KB page from
 * the buddy allocator and slices it into new chunks.
 *
 * For kfree(), we need to know which pool a chunk belongs to.  We maintain
 * a small global array `page_pool_idx[]` indexed by page frame number:
 *   -1  → page was allocated directly via buddy (large allocation)
 *   0‥7 → index into `pools[]`
 * ==================================================================== */

/* Pool sizes in ascending order. */
static const unsigned long pool_sizes[NUM_POOLS] = {
    16, 32, 64, 128, 256, 512, 1024, 2048
};

/* Singly-linked free-chunk node (embedded in the free chunk's memory). */
typedef struct chunk_node {
    struct chunk_node *next;
} chunk_node_t;

/* Per-pool metadata. */
typedef struct {
    unsigned long  chunk_size;   /* fixed size for this pool             */
    chunk_node_t  *free_list;    /* head of singly-linked free list      */
} pool_t;

static pool_t pools[NUM_POOLS];

/*
 * Page-to-pool mapping.
 * Index = (page_phys_addr - buddy_get_base()) / PAGE_SIZE
 * Value = pool index (0..NUM_POOLS-1) or -1 for large / buddy-only pages.
 * Sized for the maximum managed region (MAX_PAGES).
 */
static signed char page_pool_idx[MAX_PAGES];

/* ---- Helpers ----------------------------------------------------------- */

/** Round @size up to the nearest pool and return its index, or -1. */
static int find_pool(unsigned long size)
{
    for (int i = 0; i < NUM_POOLS; i++) {
        if (size <= pool_sizes[i])
            return i;
    }
    return -1;  /* too large for any pool */
}

/** Convert a physical address to a page-frame index (relative to buddy base). */
static inline unsigned long addr_to_page_idx(uintptr_t addr)
{
    return (addr - buddy_get_base()) / PAGE_SIZE;
}

/* ---- Logging ----------------------------------------------------------- */

static void log_chunk_alloc(uintptr_t addr, unsigned long chunk_size)
{
    uart_puts("[Chunk] Allocate 0x");
    print_hex_ulong(addr);
    uart_puts(" at chunk size ");
    print_dec_ulong(chunk_size);
    uart_puts("\n");
}

static void log_chunk_free(uintptr_t addr, unsigned long chunk_size)
{
    uart_puts("[Chunk] Free 0x");
    print_hex_ulong(addr);
    uart_puts(" at chunk size ");
    print_dec_ulong(chunk_size);
    uart_puts("\n");
}

/* ===== Public API ======================================================= */

void kmalloc_init(void)
{
    /* Initialize pool metadata. */
    for (int i = 0; i < NUM_POOLS; i++) {
        pools[i].chunk_size = pool_sizes[i];
        pools[i].free_list  = NULL;
    }

    /* Mark all managed pages as "not owned by any pool" initially. */
    unsigned long total = buddy_get_total_pages();
    for (unsigned long i = 0; i < total; i++)
        page_pool_idx[i] = -1;

    uart_puts("[kmalloc] Initialized: ");
    print_dec_ulong((unsigned long)NUM_POOLS);
    uart_puts(" chunk pools (16 .. 2048)\n");
}

void *kmalloc(unsigned long size)
{
    if (size == 0)
        return NULL;

    /* ---- Large allocation: delegate to buddy ---- */
    int pidx = find_pool(size);
    if (pidx < 0) {
        void *ptr = buddy_alloc(size);
        if (ptr) {
            /* Mark all pages covered by this allocation as -1 (large). */
            unsigned long pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
            unsigned long base  = addr_to_page_idx((uintptr_t)ptr);
            for (unsigned long i = 0; i < pages; i++)
                page_pool_idx[base + i] = -1;
        }
        return ptr;  /* buddy_alloc already logs */
    }

    /* ---- Small allocation: use chunk pool ---- */
    pool_t *pool = &pools[pidx];

    /* If the free list is empty, request a new page and slice it. */
    if (pool->free_list == NULL) {
        void *page = buddy_alloc(PAGE_SIZE);
        if (!page)
            return NULL;

        unsigned long pg_idx = addr_to_page_idx((uintptr_t)page);
        page_pool_idx[pg_idx] = (signed char)pidx;

        /* Slice the page into chunks and push onto the free list. */
        unsigned long chunk_sz = pool->chunk_size;
        unsigned long num_chunks = PAGE_SIZE / chunk_sz;
        for (unsigned long i = 0; i < num_chunks; i++) {
            chunk_node_t *node =
                (chunk_node_t *)((uintptr_t)page + i * chunk_sz);
            node->next = pool->free_list;
            pool->free_list = node;
        }
    }

    /* Pop one chunk from the free list. */
    chunk_node_t *chunk = pool->free_list;
    pool->free_list = chunk->next;

    log_chunk_alloc((uintptr_t)chunk, pool->chunk_size);
    return (void *)chunk;
}

void kfree(void *ptr)
{
    if (!ptr)
        return;

    uintptr_t addr = (uintptr_t)ptr;

    /* Find the page this pointer lives in. */
    uintptr_t page_base = addr & ~(PAGE_SIZE - 1);
    unsigned long pg_idx = addr_to_page_idx(page_base);

    signed char pidx = page_pool_idx[pg_idx];

    if (pidx < 0) {
        /* Large allocation — delegate to buddy. */
        buddy_free(ptr);   /* buddy_free already logs */
        return;
    }

    /* Small allocation — return chunk to its pool's free list. */
    pool_t *pool = &pools[pidx];
    chunk_node_t *node = (chunk_node_t *)ptr;
    node->next = pool->free_list;
    pool->free_list = node;

    log_chunk_free(addr, pool->chunk_size);
}
