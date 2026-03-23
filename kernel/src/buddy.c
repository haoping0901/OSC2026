#include "buddy.h"
#include "list.h"
#include "uart.h"
#include "utils.h"
#include "types.h"

/* ===== Frame Array ======================================================
 *
 *   frame_array[i] >= 0           : head of a free block of order val
 *   frame_array[i] == FRAME_FREE_PART (-1) : free, part of a larger block
 *   frame_array[i] == ALLOC_TAG(idx, order): allocated
 *
 * For free block heads we embed a struct list_head at the start of the
 * page's memory (the page is unused, so this is safe).
 * ==================================================================== */

/* bit 30 set to 1 signifies an allocated frame tag */
#define ALLOC_TAG(idx, order)    ( (int)( (1 << 30) | ((idx) << 8) | (order) ) )
#define GET_ALLOC_ORDER(val)     ( (val) & 0xFF )
#define GET_ALLOC_IDX(val)       ( ((val) >> 8) & 0x3FFFFF )

static int frame_array[TOTAL_PAGES];

/* One free-list head per order (0 .. MAX_ORDER). */
static struct list_head free_list[MAX_ORDER + 1];

/* ---- Helpers ----------------------------------------------------------- */

/** Convert a frame index to the physical address it represents. */
static inline uintptr_t idx_to_addr(unsigned long idx)
{
    return BUDDY_BASE + idx * PAGE_SIZE;
}

/** Convert a physical address back to a frame index. */
static inline unsigned long addr_to_idx(uintptr_t addr)
{
    return (addr - BUDDY_BASE) / PAGE_SIZE;
}

/**
 * Get the list_head pointer that lives at the start of a free page.
 * Only valid when the page is free and is a block head.
 */
static inline struct list_head *frame_list_head(unsigned long idx)
{
    return (struct list_head *)idx_to_addr(idx);
}

/** Recover the frame index from a list_head that lives in a free page. */
static inline unsigned long list_head_to_idx(struct list_head *lh)
{
    return addr_to_idx((uintptr_t)lh);
}

/**
 * Return the smallest order whose block size >= @pages.
 * block size at order k = 2^k pages.
 */
static int pages_to_order(unsigned long pages)
{
    int order = 0;
    unsigned long block = 1;
    while (block < pages) {
        block <<= 1;
        order++;
    }
    return order;
}

/* ---- Logging helpers --------------------------------------------------- */

static void log_add(unsigned long idx, int order)
{
    unsigned long count = 1UL << order;
    uart_puts("[+] Add page ");
    print_dec_ulong(idx);
    uart_puts(" to order ");
    print_dec_ulong((unsigned long)order);
    uart_puts(". Range: [");
    print_dec_ulong(idx);
    uart_puts(", ");
    print_dec_ulong(idx + count - 1);
    uart_puts("]\n");
}

static void log_remove(unsigned long idx, int order)
{
    unsigned long count = 1UL << order;
    uart_puts("[-] Remove page ");
    print_dec_ulong(idx);
    uart_puts(" from order ");
    print_dec_ulong((unsigned long)order);
    uart_puts(". Range: [");
    print_dec_ulong(idx);
    uart_puts(", ");
    print_dec_ulong(idx + count - 1);
    uart_puts("]\n");
}

static void log_buddy_found(unsigned long buddy_idx, unsigned long page_idx,
                            int order)
{
    uart_puts("[*] Buddy found! buddy idx: ");
    print_dec_ulong(buddy_idx);
    uart_puts(" for page ");
    print_dec_ulong(page_idx);
    uart_puts(" with order ");
    print_dec_ulong((unsigned long)order);
    uart_puts("\n");
}

static void log_alloc(uintptr_t addr, int order, unsigned long page_idx)
{
    uart_puts("[Page] Allocate 0x");
    print_hex_ulong(addr);
    uart_puts(" at order ");
    print_dec_ulong((unsigned long)order);
    uart_puts(", page ");
    print_dec_ulong(page_idx);
    uart_puts("\n");
}

static void log_free(uintptr_t addr, int order, unsigned long page_idx)
{
    uart_puts("[Page] Free 0x");
    print_hex_ulong(addr);
    uart_puts(" and add back to order ");
    print_dec_ulong((unsigned long)order);
    uart_puts(", page ");
    print_dec_ulong(page_idx);
    uart_puts("\n");
}

/* ---- Internal: add / remove block from free list ----------------------- */

/** Put free block starting at @idx of given @order onto its free list. */
static void block_push(unsigned long idx, int order)
{
    frame_array[idx] = order;
    /* Mark remaining pages in this block as FRAME_FREE_PART */
    unsigned long count = 1UL << order;
    for (unsigned long i = 1; i < count; i++)
        frame_array[idx + i] = FRAME_FREE_PART;

    struct list_head *node = frame_list_head(idx);
    INIT_LIST_HEAD(node);
    list_add(node, &free_list[order]);

    log_add(idx, order);
}

/** Remove free block starting at @idx from its free list. */
static void block_pop(unsigned long idx, int order)
{
    struct list_head *node = frame_list_head(idx);
    list_del(node);

    log_remove(idx, order);
}

/* ===== Public API ======================================================= */

void buddy_init(void)
{
    /* Initialise all free list heads. */
    for (int i = 0; i <= MAX_ORDER; i++)
        INIT_LIST_HEAD(&free_list[i]);

    /* All frames start as FRAME_FREE_PART (will be overwritten for heads). */
    for (unsigned long i = 0; i < TOTAL_PAGES; i++)
        frame_array[i] = FRAME_FREE_PART;

    /*
     * Build the initial free blocks.
     * Walk through the range and create the largest possible aligned blocks.
     */
    unsigned long idx = 0;
    while (idx < TOTAL_PAGES) {
        /* Find the largest order that:
         *   1) is aligned  (idx % (1 << order) == 0)
         *   2) fits        (idx + (1 << order) <= TOTAL_PAGES)
         *   3) <= MAX_ORDER
         */
        int order = MAX_ORDER;
        while (order > 0) {
            unsigned long block_pages = 1UL << order;
            if ((idx & (block_pages - 1)) == 0 && idx + block_pages <= TOTAL_PAGES)
                break;
            order--;
        }
        block_push(idx, order);
        idx += (1UL << order);
    }

    uart_puts("[Buddy] Initialised: ");
    print_dec_ulong(TOTAL_PAGES);
    uart_puts(" pages (");
    print_dec_ulong(TOTAL_PAGES * PAGE_SIZE / 1024 / 1024);
    uart_puts(" MiB) managed\n");
}

void *buddy_alloc(unsigned long size)
{
    if (size == 0)
        return NULL;

    /* Determine how many pages we need. */
    unsigned long pages_needed = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    int target_order = pages_to_order(pages_needed);

    if (target_order > MAX_ORDER)
        return NULL;   /* too large */

    /* Search upward for a free block. */
    int current_order = target_order;
    while (current_order <= MAX_ORDER && list_empty(&free_list[current_order]))
        current_order++;

    if (current_order > MAX_ORDER)
        return NULL;   /* out of memory */

    /* Take the first block from that list. */
    struct list_head *chosen = free_list[current_order].next;
    unsigned long idx = list_head_to_idx(chosen);
    block_pop(idx, current_order);

    /* Split: release the upper half repeatedly until we reach target_order. */
    while (current_order > target_order) {
        current_order--;
        unsigned long buddy_idx = idx + (1UL << current_order);
        block_push(buddy_idx, current_order);
    }

    /* Mark all pages of the allocated block. */
    unsigned long alloc_pages = 1UL << target_order;
    for (unsigned long i = 0; i < alloc_pages; i++)
        frame_array[idx + i] = ALLOC_TAG(idx, target_order);

    uintptr_t addr = idx_to_addr(idx);
    log_alloc(addr, target_order, idx);

    return (void *)addr;
}

void buddy_free(void *ptr)
{
    if (!ptr)
        return;

    uintptr_t addr = (uintptr_t)ptr;
    if (addr < BUDDY_BASE || addr >= BUDDY_END)
        return;

    unsigned long idx = addr_to_idx(addr);

    /* Extract the order of the originally allocated block from the tag */
    int order = GET_ALLOC_ORDER(frame_array[idx]);

    /* Mark pages as free. */
    for (unsigned long j = 0; j < (1UL << order); j++)
        frame_array[idx + j] = FRAME_FREE_PART;

    /* Coalesce with buddy iteratively. */
    unsigned long cur_idx = idx;
    int cur_order = order;

    while (cur_order < MAX_ORDER) {
        unsigned long buddy_idx = cur_idx ^ (1UL << cur_order);

        /* Buddy must be within range. */
        if (buddy_idx >= TOTAL_PAGES)
            break;

        /* Buddy must be a free block head of the same order. */
        if (frame_array[buddy_idx] != cur_order)
            break;

        log_buddy_found(buddy_idx, cur_idx, cur_order);

        /* Remove buddy from its free list. */
        block_pop(buddy_idx, cur_order);

        /* Merge: the merged block starts at the lower index. */
        if (buddy_idx < cur_idx)
            cur_idx = buddy_idx;

        cur_order++;
    }

    /* Place the (possibly merged) block on the appropriate free list. */
    block_push(cur_idx, cur_order);

    log_free(addr, cur_order, cur_idx);
}
