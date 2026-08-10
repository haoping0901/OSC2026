#include "buddy.h"
#include "list.h"
#include "uart.h"
#include "utils.h"
#include "types.h"
#include "mm.h"
#include "riscv.h"   /* sie_save_clear/sie_restore for refcount RMW */

/*
 * Compile-time switch for the noisy per-page trace lines prefixed with
 * "[+]" / "[-]" / "[*]". Leave undefined to silence them (default); define
 * to re-enable the verbose pool build / merge traces.
 */
/* #define BUDDY_VERBOSE_LOG */

/* ===== Frame Array ======================================================
 *
 *   g_frame_array[i] >= 0             : head of a free block of order val
 *   g_frame_array[i] == FRAME_FREE_PART (-1) : free, part of a larger block
 *   g_frame_array[i] == FRAME_RESERVED  (-2) : reserved, never allocatable
 *   g_frame_array[i] == ALLOC_TAG(idx, order): allocated
 *
 * For free block heads we embed a struct list_head at the start of the
 * page's memory (the page is unused, so this is safe).
 * ==================================================================== */

/* bit 30 set to 1 signifies an allocated frame tag */
#define ALLOC_TAG(idx, order)    ( (int)( (1 << 30) | ((idx) << 8) | (order) ) )
#define GET_ALLOC_ORDER(val)     ( (val) & 0xFF )
#define GET_ALLOC_IDX(val)       ( ((val) >> 8) & 0x3FFFFF )

/* Physical memory region managed by this allocator (set at runtime). */
static uintptr_t      g_buddy_base;
static uintptr_t      g_buddy_end;
static unsigned long  g_total_pages;

static int *g_frame_array;           /* dynamically allocated by startup allocator */

/*
 * Per-frame reference counts, parallel to g_frame_array and indexed by the
 * HEAD frame of an allocated block. buddy_alloc() sets the head's count
 * to 1; buddy_ref_inc() adds a sharer (copy-on-write); buddy_free() is
 * dec-and-test — the block only returns to the free lists when the last
 * reference is dropped. Non-shared allocations (page tables, kstacks,
 * kmalloc pools) stay at 1 for their whole lifetime, so their free path
 * behaves exactly as before.
 */
static unsigned int *g_ref_array;      /* dynamically allocated by startup allocator */

/* One free-list head per order (0 .. MAX_ORDER). */
static struct list_head free_list[MAX_ORDER + 1];

/* ---- Helpers ----------------------------------------------------------- */

/** Convert a frame index to the physical address it represents. */
static inline uintptr_t idx_to_addr(unsigned long idx)
{
    return g_buddy_base + idx * PAGE_SIZE;
}

/** Convert a physical address back to a frame index. */
static inline unsigned long addr_to_idx(uintptr_t addr)
{
    return (addr - g_buddy_base) / PAGE_SIZE;
}

/*
 * PA <-> pointer bridge (higher-half paging).
 *
 * The buddy allocator keeps ALL of its bookkeeping in physical addresses
 * (idx_to_addr/addr_to_idx, g_buddy_base/end, reserves), preserving each
 * frame's PA identity. Once paging is on, however, a PA cannot be
 * dereferenced directly — only its linear-map VA can. These two helpers
 * are the only places buddy.c converts: a PA becomes a usable pointer when
 * we touch in-page memory or hand a block to a caller, and a caller's
 * pointer becomes a PA again on the way back in.
 */
static inline void *frame_pa_to_ptr(uintptr_t pa)
{
    return phys_to_virt(pa);
}

static inline uintptr_t frame_ptr_to_pa(void *p)
{
    return virt_to_phys(p);
}

/**
 * Get the list_head pointer that lives at the start of a free page.
 * Only valid when the page is free and is a block head.
 *
 * The free-list node is stored inside the page itself, so it must be
 * accessed through the page's linear-map VA, not its raw PA.
 */
static inline struct list_head *frame_list_head(unsigned long idx)
{
    return (struct list_head *)frame_pa_to_ptr(idx_to_addr(idx));
}

/** Recover the frame index from a list_head that lives in a free page. */
static inline unsigned long list_head_to_idx(struct list_head *lh)
{
    return addr_to_idx(frame_ptr_to_pa(lh));
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
#ifdef BUDDY_VERBOSE_LOG
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
#else
    (void)idx;
    (void)order;
#endif
}

static void log_remove(unsigned long idx, int order)
{
#ifdef BUDDY_VERBOSE_LOG
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
#else
    (void)idx;
    (void)order;
#endif
}

static void log_buddy_found(unsigned long buddy_idx, unsigned long page_idx,
                            int order)
{
#ifdef BUDDY_VERBOSE_LOG
    uart_puts("[*] Buddy found! buddy idx: ");
    print_dec_ulong(buddy_idx);
    uart_puts(" for page ");
    print_dec_ulong(page_idx);
    uart_puts(" with order ");
    print_dec_ulong((unsigned long)order);
    uart_puts("\n");
#else
    (void)buddy_idx;
    (void)page_idx;
    (void)order;
#endif
}

/* ---- Internal: add / remove block from free list ----------------------- */

/** Put free block starting at @idx of given @order onto its free list. */
static void block_push(unsigned long idx, int order)
{
    g_frame_array[idx] = order;
    /* Mark remaining pages in this block as FRAME_FREE_PART */
    unsigned long count = 1UL << order;
    for (unsigned long i = 1; i < count; i++)
        g_frame_array[idx + i] = FRAME_FREE_PART;

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

/**
 * ---------------------------------------------------------------------------
 * @brief Initialize the buddy allocator over a physical memory region.
 *
 * Records the managed region bounds and page count, binds the caller-provided
 * frame-state and reference-count arrays, and resets all per-frame state. Every
 * frame is marked FRAME_FREE_PART with a zero reference count; the free lists
 * themselves are left empty here and populated later, once all buddy_reserve()
 * calls have carved out the unavailable ranges.
 *
 * @param      base            Physical base address of the managed region.
 * @param      size            Size of the managed region in bytes.
 * @param[out] ext_frame_array Caller-provided per-frame state array.
 * @param[out] ext_ref_array   Caller-provided per-frame reference-count
 *                             array.
 * @param      frame_count     Number of frames covered by the arrays.
 * ---------------------------------------------------------------------------
 */
void buddy_init(uintptr_t base, uintptr_t size,
                int *ext_frame_array, unsigned int *ext_ref_array,
                unsigned long frame_count)
{
    g_buddy_base  = base;
    g_buddy_end   = base + size;
    g_total_pages = frame_count;
    g_frame_array = ext_frame_array;
    g_ref_array   = ext_ref_array;

    /* Initialize all free list heads. */
    for (int i = 0; i <= MAX_ORDER; i++)
        INIT_LIST_HEAD(&free_list[i]);

    /* Mark every frame as free-part; free lists will be built later,
     * after all buddy_reserve() calls are done. */
    for (unsigned long i = 0; i < g_total_pages; i++) {
        g_frame_array[i] = FRAME_FREE_PART;
        g_ref_array[i]   = 0;
    }

    uart_puts("[Buddy] Init: base=0x");
    print_hex_ulong(base);
    uart_puts(" pages=");
    print_dec_ulong(g_total_pages);
    uart_puts("\n");
}    /* buddy_init */

void buddy_reserve(uintptr_t start, uintptr_t end)
{
    /* Round start down and end up to page boundaries. */
    start = start & ~(PAGE_SIZE - 1UL);
    end   = (end + PAGE_SIZE - 1UL) & ~(PAGE_SIZE - 1UL);

    /* Clamp to the managed region. */
    if (start < g_buddy_base)
        start = g_buddy_base;
    if (end   > g_buddy_end)
        end = g_buddy_end;
    if (start >= end)
        return;

    unsigned long first = addr_to_idx(start);
    unsigned long last  = addr_to_idx(end);   /* exclusive */

    for (unsigned long i = first; i < last; i++)
        g_frame_array[i] = FRAME_RESERVED;

    uart_puts("[Reserve] 0x");
    print_hex_ulong(start);
    uart_puts(" - 0x");
    print_hex_ulong(end);
    uart_puts("\n");
}

uintptr_t buddy_get_base(void)
{
    return g_buddy_base;
}

unsigned long buddy_get_total_pages(void)
{
    return g_total_pages;
}

void buddy_build_free_lists(void)
{
    unsigned long idx = 0;
    while (idx < g_total_pages) {
        /* Skip reserved pages one by one. */
        if (g_frame_array[idx] == FRAME_RESERVED) {
            idx++;
            continue;
        }

        /*
         * Find the largest order block that:
         *   1) is aligned         (idx % (1 << order) == 0)
         *   2) fits within range  (idx + (1 << order) <= g_total_pages)
         *   3) contains no RESERVED pages
         *   4) <= MAX_ORDER
         */
        int order = MAX_ORDER;
        while (order > 0) {
            unsigned long block = 1UL << order;

            /* Alignment and range check. */
            if ((idx & (block - 1)) != 0 || idx + block > g_total_pages) {
                order--;
                continue;
            }

            /* Ensure no reserved page inside this block. */
            int clean = 1;
            for (unsigned long k = 0; k < block; k++) {
                if (g_frame_array[idx + k] == FRAME_RESERVED) {
                    clean = 0;
                    break;
                }
            }
            if (clean)
                break;
            order--;
        }

        block_push(idx, order);
        idx += (1UL << order);
    }

    uart_puts("[Buddy] Free lists built: ");
    print_dec_ulong(g_total_pages);
    uart_puts(" pages (");
    print_dec_ulong(g_total_pages * PAGE_SIZE / 1024 / 1024);
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
        g_frame_array[idx + i] = ALLOC_TAG(idx, target_order);

    /* The caller is the sole owner until buddy_ref_inc() adds sharers. */
    g_ref_array[idx] = 1;

    /* Return a dereferenceable VA; internal bookkeeping stays in PA. */
    return frame_pa_to_ptr(idx_to_addr(idx));
}

void buddy_free(void *ptr)
{
    if (!ptr)
        return;

    /* Caller hands back the VA we returned from buddy_alloc(); convert it
     * to a PA to resume PA-based bookkeeping. */
    uintptr_t addr = frame_ptr_to_pa(ptr);
    if (addr < g_buddy_base || addr >= g_buddy_end)
        return;

    unsigned long idx = addr_to_idx(addr);

    /* Guard against freeing a reserved page. */
    if (g_frame_array[idx] == FRAME_RESERVED)
        return;

    /*
     * Dec-and-test on the per-frame reference count. The RMW is bracketed
     * against S-mode interrupts: fault handlers run with SIE on, so two
     * sharers' inc/dec could otherwise interleave through preemption.
     * While other sharers remain the block must stay allocated — only the
     * last reference actually releases it into the free lists below.
     */
    unsigned long flags = sie_save_clear();
    if (g_ref_array[idx] == 0) {
        sie_restore(flags);
        uart_puts("[Buddy] WARN: free of zero-ref block idx=");
        print_dec_ulong(idx);
        uart_puts("\n");
        return;
    }
    g_ref_array[idx]--;
    if (g_ref_array[idx] > 0) {
        sie_restore(flags);
        return;
    }
    sie_restore(flags);

    /* Extract the order of the originally allocated block from the tag */
    int order = GET_ALLOC_ORDER(g_frame_array[idx]);

    /* Mark pages as free. */
    for (unsigned long j = 0; j < (1UL << order); j++)
        g_frame_array[idx + j] = FRAME_FREE_PART;

    /* Coalesce with buddy iteratively. */
    unsigned long cur_idx = idx;
    int cur_order = order;

    while (cur_order < MAX_ORDER) {
        unsigned long buddy_idx = cur_idx ^ (1UL << cur_order);

        /* Buddy must be within range. */
        if (buddy_idx >= g_total_pages)
            break;

        /* Buddy must not be reserved. */
        if (g_frame_array[buddy_idx] == FRAME_RESERVED)
            break;

        /* Buddy must be a free block head of the same order. */
        if (g_frame_array[buddy_idx] != cur_order)
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
}

/** ----------------------------------------------------------------------
 * @brief buddy_ref_inc() – Take one extra reference on a block.
 *
 * Registers a new co-owner of the block whose head is at @ptr (the VA
 * previously returned by buddy_alloc()); a matching buddy_free() from
 * that owner later drops the reference. The increment is bracketed
 * against S-mode interrupts for the same reason as the dec-and-test in
 * buddy_free(): sharers may race through preemption.
 * @param[in] ptr Linear-map VA of the block head.
 * -------------------------------------------------------------------- */
void buddy_ref_inc(void *ptr)
{
    if (!ptr)
        return;

    uintptr_t addr = frame_ptr_to_pa(ptr);
    if (addr < g_buddy_base || addr >= g_buddy_end)
        return;

    unsigned long idx = addr_to_idx(addr);

    unsigned long flags = sie_save_clear();
    g_ref_array[idx]++;
    sie_restore(flags);
}

/** ----------------------------------------------------------------------
 * @brief buddy_ref_read() – Read the reference count of a block.
 *
 * Single-word read, no locking: on a single hart a snapshot is enough
 * for the copy-on-write "sole owner?" decision — a count of 1 cannot
 * concurrently grow, because only the sole owner itself could fork.
 * @param[in] ptr Linear-map VA of the block head.
 * @return Current reference count, or 0 if @ptr is out of range.
 * -------------------------------------------------------------------- */
unsigned int buddy_ref_read(void *ptr)
{
    if (!ptr)
        return 0;

    uintptr_t addr = frame_ptr_to_pa(ptr);
    if (addr < g_buddy_base || addr >= g_buddy_end)
        return 0;

    return g_ref_array[addr_to_idx(addr)];
}

/* ===== Startup Allocator ==================================================
 *
 * Bump-style allocator used exclusively before buddy_init() is called.
 * Its sole purpose is to allocate metadata arrays (g_frame_array,
 * page_pool_idx) from the first usable gap in the physical memory region,
 * skipping all reserved ranges registered via buddy_startup_reserve().
 *
 * Each successful allocation is appended to g_sa_reserves so that future
 * allocations automatically skip already-used memory.  After buddy_init(),
 * the caller must invoke buddy_startup_replay_reserves() to propagate all
 * tracked regions (pre-registered reserves + metadata allocations) into
 * the buddy system in one pass.
 * ======================================================================== */

static struct {
    uintptr_t start;
    uintptr_t end;
} g_sa_reserves[BUDDY_STARTUP_MAX_RESERVES];
static int       g_sa_reserve_count;

static uintptr_t g_sa_mem_base;
static uintptr_t g_sa_mem_end;


/** Insertion-sort g_sa_reserves[] by start address (count is tiny). */
static void sa_sort_reserves(void)
{
    for (int i = 1; i < g_sa_reserve_count; i++) {
        uintptr_t ks = g_sa_reserves[i].start;
        uintptr_t ke = g_sa_reserves[i].end;
        int j = i - 1;
        while (j >= 0 && g_sa_reserves[j].start > ks) {
            g_sa_reserves[j + 1] = g_sa_reserves[j];
            j--;
        }
        g_sa_reserves[j + 1].start = ks;
        g_sa_reserves[j + 1].end   = ke;
    }
}

/** ----------------------------------------------------------------------
 * @brief sa_find_first_free() – find the first 4 KiB-aligned free gap.
 *
 * Scans the managed memory region from g_sa_mem_base, advancing the
 * cursor past every reserved region until a contiguous free window of
 * at least @size bytes is found.
 *
 * @param size minimum byte length required (already page-aligned)
 * @return start address of the gap, or 0 if none found
 * -------------------------------------------------------------------- */
static uintptr_t sa_find_first_free(unsigned long size)
{
    /* Always scan from the beginning of memory.  Previously allocated
     * regions are recorded in g_sa_reserves, so they are naturally skipped
     * along with the pre-registered reserved regions. */
    uintptr_t cursor = g_sa_mem_base;

    for (int i = 0; i <= g_sa_reserve_count; ++i) {
        /* Upper bound of the current free window. */
        uintptr_t window_end = (i < g_sa_reserve_count)
                               ? g_sa_reserves[i].start
                               : g_sa_mem_end;

        /* Align cursor up to PAGE_SIZE. */
        cursor = (cursor + PAGE_SIZE - 1UL) & ~(PAGE_SIZE - 1UL);

        if (cursor + size <= window_end)
            return cursor;

        /* Advance past the next reserved region. */
        if (i < g_sa_reserve_count)
            cursor = g_sa_reserves[i].end;
    }
    return 0; /* no suitable gap found */
}

void buddy_startup_init(uintptr_t mem_base, uintptr_t mem_size)
{
    g_sa_mem_base      = mem_base;
    g_sa_mem_end       = mem_base + mem_size;
    g_sa_reserve_count = 0;

    uart_puts("[Startup] Init: base=0x");
    print_hex_ulong(mem_base);
    uart_puts(" size=0x");
    print_hex_ulong(mem_size);
    uart_puts("\n");
}

void buddy_startup_reserve(uintptr_t start, uintptr_t end)
{
    if (g_sa_reserve_count >= BUDDY_STARTUP_MAX_RESERVES) {
        uart_puts("[Startup] ERROR: too many reserves\n");
        return;
    }

    /* Align to 4 KiB: round start down, round end up. */
    start &= ~(PAGE_SIZE - 1UL);
    end    = (end + PAGE_SIZE - 1UL) & ~(PAGE_SIZE - 1UL);

    /* Clamp to managed region. */
    if (start < g_sa_mem_base)
        start = g_sa_mem_base;
    if (end > g_sa_mem_end)
        end = g_sa_mem_end;
    if (start >= end)
        return;

    g_sa_reserves[g_sa_reserve_count].start = start;
    g_sa_reserves[g_sa_reserve_count].end   = end;
    g_sa_reserve_count++;

#ifdef BUDDY_VERBOSE_LOG
    uart_puts("[Startup] Reserve 0x");
    print_hex_ulong(start);
    uart_puts(" - 0x");
    print_hex_ulong(end);
    uart_puts("\n");
#endif
}

void *buddy_startup_alloc(unsigned long size)
{
    /* Round size up to a PAGE_SIZE multiple. */
    size = (size + PAGE_SIZE - 1UL) & ~(PAGE_SIZE - 1UL);

    /* Re-sort every time: newly appended allocation records may be out of
     * order relative to the pre-registered reserves. */
    sa_sort_reserves();

    /* Scan from mem_base every time.  Previously allocated regions have
     * been appended to g_sa_reserves, so sa_find_first_free() skips them
     * automatically and can therefore use gaps that appear before an
     * earlier allocation. */
    uintptr_t ret = sa_find_first_free(size);
    if (ret == 0) {
        uart_puts("[Startup] ERROR: no free region found\n");
        return NULL;
    }

    /* Record this allocation as a reserved region so future calls skip it. */
    g_sa_reserves[g_sa_reserve_count].start = ret;
    g_sa_reserves[g_sa_reserve_count].end   = ret + size;
    g_sa_reserve_count++;

    uart_puts("[Startup] Alloc 0x");
    print_hex_ulong(ret);
    uart_puts(" size=0x");
    print_hex_ulong(size);
    uart_puts("\n");

    /* ret is a PA; hand the caller a dereferenceable linear-map VA so the
     * metadata arrays (g_frame_array, page_pool_idx) can be indexed directly. */
    return frame_pa_to_ptr(ret);
}

void buddy_startup_replay_reserves(void)
{
    uart_puts("[Startup] Replaying ");
    print_dec_ulong((unsigned long)g_sa_reserve_count);
    uart_puts(" reserve(s) into buddy\n");

    for (int i = 0; i < g_sa_reserve_count; i++)
        buddy_reserve(g_sa_reserves[i].start, g_sa_reserves[i].end);
}

