#include "mm.h"
#include "riscv.h"
#include "buddy.h"   /* PAGE_SIZE */
#include "types.h"
#include "sched.h"   /* struct thread, vma_list / mmap_top */
#include "kmalloc.h" /* kmalloc / kfree for struct vma */

/*
 * Early Sv39 page-table setup for the higher-half kernel.
 *
 * setup_vm() builds two linear mappings with 2 MiB PMD-level leaves and
 * enables paging:
 *   - identity map     VA == PA            (temporary boot scaffold)
 *   - higher-half map  VA == PA + offset   (permanent kernel window)
 * drop_identity_map() later tears down the identity half once execution has
 * relocated to the higher-half VAs.
 *
 * Both functions run before the buddy/kmalloc allocators exist, so the page
 * tables are statically reserved here rather than dynamically allocated.
 */

/*
 * Number of 1 GiB regions the linear map covers, starting at PA 0.
 *
 * QEMU virt RAM is [0x8000_0000, 0x2_8000_0000) (8 GiB), so the highest PA
 * we must reach is 0x2_8000_0000 = 10 GiB. Covering [0, 10 GiB) also spans
 * every MMIO aperture below the RAM base (UART/PLIC/CLINT/PCIe). Each PMD
 * table covers exactly 1 GiB, so we need LINEAR_MAP_GIB PMD tables.
 */
#define LINEAR_MAP_GIB   10

/* PGD index for a virtual address (VPN[2]). */
#define PGD_INDEX(va)   (((unsigned long)(va) >> PGD_SHIFT) & (PTRS_PER_TABLE - 1))

/*
 * Platform RAM map. leaf_prot() consults this to decide RAM (RWX) vs MMIO
 * (RW, no-X) per 2 MiB leaf. A single "PA < RAM_BASE" cutoff is wrong on
 * boards whose RAM is non-contiguous or whose MMIO sits ABOVE the RAM base:
 *
 *   QEMU virt : RAM [0x8000_0000, +8 GiB), MMIO (UART/PLIC/...) below it.
 *   Orange Pi RV2 (SpacemiT K1): RAM is split as
 *       [0x0000_0000, 0x8000_0000)            (2 GiB)
 *       [0x1_0000_0000, 0x1_8000_0000)        (2 GiB)
 *     and the UART lives at 0xD401_7000 — ABOVE 0x8000_0000 and inside the
 *     [2 GiB, 4 GiB) hole. The old cutoff mis-mapped the UART as executable
 *     RAM and the kernel image (loaded at 0x0020_0000) as non-executable
 *     device memory, so the first higher-half fetch faulted silently.
 *
 * Anything not covered by a RAM region below is treated as device memory.
 */
struct ram_region {
    unsigned long start;
    unsigned long end;      /* exclusive */
};

#ifdef QEMU
static const struct ram_region ram_regions[] = {
    { 0x80000000UL, 0x280000000UL },   /* virt RAM: 8 GiB from 0x8000_0000 */
};
#else
static const struct ram_region ram_regions[] = {
    { 0x000000000UL, 0x080000000UL },  /* opi RAM bank 0: [0, 2 GiB)       */
    { 0x100000000UL, 0x180000000UL },  /* opi RAM bank 1: [4 GiB, 6 GiB)   */
};
#endif /* QEMU */

#define NR_RAM_REGIONS  (sizeof(ram_regions) / sizeof(ram_regions[0]))

/*
 * Page tables live in .data (4 KiB aligned), NOT .bss.
 *
 * The boot order in start.S is: setup_vm() (fills + activates these tables)
 * BEFORE clear_bss. If the tables lived in .bss, clear_bss would zero the
 * entries setup_vm() just wrote and paging would fault on the next fetch.
 * .data is loaded with its initial (zero) contents from the image, so the
 * tables are already clean at boot and survive the later BSS clear.
 */
static unsigned long pgd[PTRS_PER_TABLE]
    __attribute__((section(".data"), aligned(PAGE_SIZE)));
static unsigned long pmd[LINEAR_MAP_GIB][PTRS_PER_TABLE]
    __attribute__((section(".data"), aligned(PAGE_SIZE)));

/** ----------------------------------------------------------------------
 * @brief pa_is_ram() – Test whether a PA falls in a platform RAM region.
 *
 * Walks the ram_regions[] table for the active platform. A 2 MiB leaf is
 * classified by its base PA; RAM regions are region-aligned for the boards
 * we target, so the base alone is sufficient.
 * @param pa Physical base address of the 2 MiB leaf.
 * @return 1 if pa lies within a RAM region, 0 otherwise (device memory).
 * -------------------------------------------------------------------- */
static inline int pa_is_ram(unsigned long pa)
{
    for (unsigned int i = 0; i < NR_RAM_REGIONS; i++) {
        if (pa >= ram_regions[i].start && pa < ram_regions[i].end)
            return 1;
    }
    return 0;
}

/** ----------------------------------------------------------------------
 * @brief leaf_prot() – Pick PTE flags for a linear-map leaf by PA.
 *
 * RAM gets full kernel RWX; everything else (MMIO apertures and unbacked
 * holes) gets RW without X so device memory is never executable, matching
 * the lab's W^X intent for device regions.
 * @param pa Physical base address of the 2 MiB leaf.
 * @return PROT_KERNEL for RAM, PROT_DEVICE otherwise.
 * -------------------------------------------------------------------- */
static inline unsigned long leaf_prot(unsigned long pa)
{
    return pa_is_ram(pa) ? PROT_KERNEL : PROT_DEVICE;
}

/** ----------------------------------------------------------------------
 * @brief map_linear_range() – Populate one PMD table with 2 MiB leaves.
 *
 * Fills @table with 512 leaf PTEs covering the 1 GiB region starting at
 * @gib_base, each a 2 MiB identity leaf (PPN = PA >> 12). The same table is
 * referenced by both the identity and the higher-half PGD entry, so a
 * single physical PMD table serves VA==PA and VA==PA+offset simultaneously.
 * @param table   PMD table (512 entries) to fill.
 * @param gib_base Physical base address of the 1 GiB region (PGD-aligned).
 * -------------------------------------------------------------------- */
static void map_linear_range(unsigned long *table, unsigned long gib_base)
{
    for (unsigned long i = 0; i < PTRS_PER_TABLE; i++) {
        unsigned long pa = gib_base + i * PMD_SIZE;
        table[i] = MAKE_PTE(pa, leaf_prot(pa));
    }
}

/** ----------------------------------------------------------------------
 * @brief setup_vm() – Build the linear page tables and enable Sv39 paging.
 *
 * For each of the LINEAR_MAP_GIB 1 GiB regions starting at PA 0, fills one
 * PMD table with 2 MiB leaves and links it into BOTH the identity PGD slot
 * (index i) and the higher-half PGD slot (index PGD_INDEX(offset) + i). The
 * two PGD entries point at the SAME PMD table, so identity and higher-half
 * resolve to identical PAs and dropping identity later only needs to clear
 * the low PGD entries.
 *
 * After the tables are built it writes satp (MODE=Sv39, PPN = pgd PA) and
 * issues sfence.vma to flush stale TLB state. Paging is live on return; the
 * caller (start.S) is still fetching via the identity map at this point.
 * -------------------------------------------------------------------- */
void setup_vm(void)
{
    unsigned long hh_pgd_base = PGD_INDEX(KERNEL_VA_OFFSET);

    for (unsigned long g = 0; g < LINEAR_MAP_GIB; g++) {
        unsigned long gib_base = (unsigned long)g << PGD_SHIFT;

        map_linear_range(pmd[g], gib_base);

        /* Both halves reference the one PMD table for this 1 GiB region. */
        unsigned long pmd_pte = MAKE_PTE((unsigned long)pmd[g], PTE_V);
        pgd[g]               = pmd_pte;   /* identity half    */
        pgd[hh_pgd_base + g] = pmd_pte;   /* higher-half half */
    }

    /* Activate Sv39 with pgd as the root, then flush the TLB. */
    unsigned long satp = MAKE_SATP((unsigned long)pgd);
    asm volatile (
        "csrw satp, %0\n"
        "sfence.vma zero, zero\n"
        :: "r"(satp) : "memory");
}

/** ----------------------------------------------------------------------
 * @brief drop_identity_map() – Remove the temporary identity mapping.
 *
 * Zeroes the low (identity) PGD entries so only the higher-half window
 * remains, then sfence.vma to flush the now-stale identity translations.
 * Safe to call only after execution has relocated to higher-half VAs
 * (done by the high jump in start.S). The PMD tables themselves are left
 * intact because the higher-half PGD entries still reference them.
 * -------------------------------------------------------------------- */
void drop_identity_map(void)
{
    for (unsigned long g = 0; g < LINEAR_MAP_GIB; g++)
        pgd[g] = 0;

    asm volatile ("sfence.vma zero, zero" ::: "memory");
}

/* ---------- Per-process address spaces --------------------------------- */

/** ----------------------------------------------------------------------
 * @brief kernel_pgd() – Expose the static kernel root page table.
 *
 * The kernel PGD lives in .data (built by setup_vm()); pgd_alloc()
 * clones its high half into every user PGD, and the scheduler installs
 * it when dispatching a kernel-only thread.
 * @return Kernel VA of the kernel root page table.
 * -------------------------------------------------------------------- */
unsigned long *kernel_pgd(void)
{
    return pgd;
}

/** ----------------------------------------------------------------------
 * @brief zero_page() – Clear one 4 KiB page word by word.
 *
 * Freestanding replacement for memset(0) on a freshly buddy_alloc'd
 * page-table page. @p must be PAGE_SIZE aligned and PAGE_SIZE long.
 * @param p Page to clear.
 * -------------------------------------------------------------------- */
static void zero_page(void *p)
{
    unsigned long *w = p;
    for (unsigned long i = 0; i < PAGE_SIZE / sizeof(unsigned long); i++)
        w[i] = 0;
}

/** ----------------------------------------------------------------------
 * @brief pgd_alloc() – Allocate a user root page table.
 *
 * Carves one zeroed 4 KiB page from the buddy allocator and copies the
 * kernel high-half PGD entries (KERNEL_PGD_HALF..PTRS_PER_TABLE-1) into
 * it. Those entries reference the SAME kernel PMD tables as the kernel
 * PGD, so kernel code/stack/linear-map resolve identically in the new
 * address space — the invariant that makes a satp switch safe.
 * @return Kernel VA of the new PGD, or NULL on OOM.
 * -------------------------------------------------------------------- */
unsigned long *pgd_alloc(void)
{
    unsigned long *p = buddy_alloc(PAGE_SIZE);
    if (!p)
        return NULL;
    zero_page(p);

    unsigned long *kpgd = kernel_pgd();
    for (int i = KERNEL_PGD_HALF; i < PTRS_PER_TABLE; i++)
        p[i] = kpgd[i];        /* share kernel high-half PMD tables */

    return p;
}

/** ----------------------------------------------------------------------
 * @brief walk_or_create() – Resolve one level, allocating a table.
 *
 * If @table[idx] already holds a valid non-leaf PTE, returns the kernel
 * VA of the next-level table it points at. Otherwise allocates and
 * zeroes a new table, links it as a pointer PTE (V only, R=W=X=0), and
 * returns it. Tables live in the linear map, so PA<->VA uses
 * phys_to_virt()/virt_to_phys().
 * @param table Current-level table (kernel VA).
 * @param idx   Entry index within @table.
 * @return Kernel VA of the next-level table, or NULL on OOM.
 * -------------------------------------------------------------------- */
static unsigned long *walk_or_create(unsigned long *table, unsigned long idx)
{
    unsigned long pte = table[idx];
    if (pte & PTE_V)
        return phys_to_virt(PTE_TO_PA(pte));

    unsigned long *next = buddy_alloc(PAGE_SIZE);
    if (!next)
        return NULL;
    zero_page(next);
    table[idx] = MAKE_PTE(virt_to_phys(next), PTE_V);
    return next;
}

/** ----------------------------------------------------------------------
 * @brief map_pages() – Install 4 KiB leaf mappings, building tables.
 *
 * Iterates over [va, va+size) one page at a time, walking PGD->PMD->PTE
 * (allocating intermediate tables on demand) and writing each leaf PTE
 * with PPN = pa>>12 and flags @prot. Inputs must be PAGE_SIZE aligned.
 * On OOM the partial mapping is left in place; the caller tears the
 * whole address space down via uvm_destroy().
 * @param pgd  Root page table (kernel VA).
 * @param va   Virtual base (PAGE_SIZE aligned).
 * @param size Byte length (PAGE_SIZE aligned).
 * @param pa   Physical base (PAGE_SIZE aligned).
 * @param prot Leaf PTE flag bits (must include PTE_V).
 * @return 0 on success, -1 on OOM.
 * -------------------------------------------------------------------- */
int map_pages(unsigned long *pgd, unsigned long va, unsigned long size,
              unsigned long pa, unsigned long prot)
{
    for (unsigned long off = 0; off < size; off += PAGE_SIZE) {
        unsigned long v = va + off;

        unsigned long *pmd = walk_or_create(pgd, PT_INDEX(v, 2));
        if (!pmd)
            return -1;
        unsigned long *pte = walk_or_create(pmd, PT_INDEX(v, 1));
        if (!pte)
            return -1;

        pte[PT_INDEX(v, 0)] = MAKE_PTE(pa + off, prot);
    }
    return 0;
}

/** ----------------------------------------------------------------------
 * @brief uvm_destroy() – Free the user-half intermediate page tables.
 *
 * Walks the low half (PGD indices 0..KERNEL_PGD_HALF-1): for each valid
 * PGD entry frees its PMD table after freeing every valid PMD entry's
 * PTE table. Leaf user frames are owned and freed by the thread's
 * image/stack bookkeeping, so they are deliberately not freed here. The
 * shared kernel high half (>= KERNEL_PGD_HALF) is never touched.
 * @param pgd Root page table whose user half is to be freed.
 * -------------------------------------------------------------------- */
void uvm_destroy(unsigned long *pgd)
{
    for (int i = 0; i < KERNEL_PGD_HALF; i++) {
        if (!(pgd[i] & PTE_V))
            continue;
        unsigned long *pmd = phys_to_virt(PTE_TO_PA(pgd[i]));

        for (int j = 0; j < PTRS_PER_TABLE; j++) {
            if (!(pmd[j] & PTE_V))
                continue;
            unsigned long *pte = phys_to_virt(PTE_TO_PA(pmd[j]));
            buddy_free(pte);
        }
        buddy_free(pmd);
        pgd[i] = 0;
    }
}

/** ----------------------------------------------------------------------
 * @brief pgd_free() – Destroy a user address space and its root table.
 *
 * Frees the user-half tables via uvm_destroy() then returns the PGD
 * page to the buddy allocator. Caller must ensure satp no longer points
 * at @pgd (see the reap-point reclamation in sched.c).
 * @param pgd Root page table to free.
 * -------------------------------------------------------------------- */
void pgd_free(unsigned long *pgd)
{
    uvm_destroy(pgd);
    buddy_free(pgd);
}

/** ----------------------------------------------------------------------
 * @brief mm_set_satp() – Switch the active address space.
 *
 * Computes the PGD physical address, writes satp (MODE=Sv39) and issues
 * sfence.vma to drop stale TLB entries. The kernel high half is
 * identical across all PGDs, so the currently executing kernel code and
 * stack remain valid across the switch.
 * @param pgd_va Kernel VA of the PGD to install.
 * -------------------------------------------------------------------- */
void mm_set_satp(unsigned long *pgd_va)
{
    unsigned long pa = virt_to_phys(pgd_va);
    asm volatile (
        "csrw satp, %0\n"
        "sfence.vma zero, zero\n"
        :: "r"(MAKE_SATP(pa)) : "memory");
}

/* ---------- mmap (anonymous memory) ------------------------------------ */

/** ----------------------------------------------------------------------
 * @brief mmap_round_up_page() – Round @n up to a PAGE_SIZE multiple.
 *
 * Local copy (sched.c has its own static round_up_page); kept private so
 * the two translation units stay independent.
 * @param n Byte count.
 * @return n rounded up to the next page boundary (n==0 -> 0).
 * -------------------------------------------------------------------- */
static inline unsigned long mmap_round_up_page(unsigned long n)
{
    return (n + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

/** ----------------------------------------------------------------------
 * @brief vma_alloc() – Allocate and populate a struct vma.
 *
 * kmalloc's a node (small object → chunk pool) and fills every field.
 * @prot is already a leaf PTE flag set (incl. PTE_U); it is stored
 * verbatim so teardown and fork copy need no re-conversion.
 * @param va      User virtual base (PAGE_SIZE aligned).
 * @param len     Byte length (PAGE_SIZE aligned).
 * @param prot    Leaf PTE flag bits.
 * @param kva     Kernel VA of the backing buddy block.
 * @param is_mmap 1 for mmap()'d regions, 0 for image/stack/sigpage.
 * @return New vma, or NULL on OOM.
 * -------------------------------------------------------------------- */
struct vma *vma_alloc(unsigned long va, unsigned long len,
                      unsigned long prot, void *kva, unsigned char is_mmap)
{
    struct vma *v = kmalloc(sizeof(*v));
    if (!v)
        return NULL;
    INIT_LIST_HEAD(&v->link);
    v->va      = va;
    v->len     = len;
    v->prot    = prot;
    v->kva     = kva;
    v->is_mmap = is_mmap;
    return v;
}

/** ----------------------------------------------------------------------
 * @brief vma_insert_sorted() – Link @vma into @t->vma_list (va-ascending).
 *
 * Linear scan for the first existing VMA whose va exceeds @vma->va and
 * inserts before it, preserving ascending order so gap searches and
 * overlap checks are single-pass. The list is short (a few regions), so
 * O(n) insertion is fine.
 * @param t   Owning thread.
 * @param vma Node to link (its va field decides position).
 * -------------------------------------------------------------------- */
void vma_insert_sorted(struct thread *t, struct vma *vma)
{
    struct list_head *it;
    list_for_each(it, &t->vma_list) {
        struct vma *cur = list_entry(it, struct vma, link);
        if (cur->va > vma->va)
            break;
    }
    /* Insert before `it` (== list head when appending at the tail). */
    list_add_tail(&vma->link, it);
}

/** ----------------------------------------------------------------------
 * @brief vma_overlaps() – Test whether [base, base+len) hits any VMA.
 *
 * Half-open interval intersection over the whole vma_list. Used both by
 * the hint path (reject an overlapping hint) and the top-down search
 * (skip occupied gaps).
 * @param t    Thread whose vma_list to scan.
 * @param base Candidate base VA.
 * @param len  Candidate byte length.
 * @return 1 if any VMA overlaps, 0 if the range is free.
 * -------------------------------------------------------------------- */
static int vma_overlaps(struct thread *t, unsigned long base,
                        unsigned long len)
{
    struct list_head *it;
    list_for_each(it, &t->vma_list) {
        struct vma *v = list_entry(it, struct vma, link);
        if (base < v->va + v->len && v->va < base + len)
            return 1;
    }
    return 0;
}

/** ----------------------------------------------------------------------
 * @brief mmap_find_va() – Choose a base VA for a new mapping.
 *
 * If @hint is non-zero, page-aligned and free, it is honoured. Otherwise
 * the kernel self-selects top-down from t->mmap_top, stepping a page at a
 * time until a @len-sized hole with no VMA overlap is found, then drops
 * mmap_top one guard page BELOW the chosen base so the next self-selected
 * mapping leaves an unmapped page between regions. The guard page makes a
 * read/write just past a region's end fault (the lab's out-of-bounds test
 * relies on this), and likewise keeps the topmost mmap region from abutting
 * the signal page (mmap_top is initialised one guard page below SIGPAGE_VA;
 * see MMAP_CURSOR_INIT). USER_CODE_VA (0) is the low bound; running below
 * it means the address space is exhausted.
 * @param t    Calling thread.
 * @param hint User-supplied address hint (0 if none).
 * @param len  Page-rounded length to place.
 * @return Chosen base VA, or 0 if no space.
 * -------------------------------------------------------------------- */
static unsigned long mmap_find_va(struct thread *t, unsigned long hint,
                                  unsigned long len)
{
    if (hint != 0 && (hint & (PAGE_SIZE - 1)) == 0 &&
        !vma_overlaps(t, hint, len))
        return hint;

    /*
     * Top-down search: the highest candidate base is mmap_top - len. Walk
     * downward a page at a time until a len-sized hole with no VMA overlap
     * is found. mmap_top is page-aligned, so every candidate is too.
     */
    if (t->mmap_top < len)
        return 0;
    for (unsigned long base = t->mmap_top - len;
         base >= PAGE_SIZE; base -= PAGE_SIZE) {
        if (!vma_overlaps(t, base, len)) {
            /* Reserve one guard page below this region for the next pick. */
            t->mmap_top = base - PAGE_SIZE;
            return base;
        }
    }
    return 0;
}

/** ----------------------------------------------------------------------
 * @brief mmap_prot_to_pte() – Convert user PROT_* bits to leaf PTE flags.
 *
 * Always sets PTE_V | PTE_U | PTE_A | PTE_D (A/D pre-set, matching the
 * PROT_USER_* convention of not doing A/D-driven paging). RWX bits follow
 * the request; PROT_WRITE implies PTE_R because Sv39 reserves W=1,R=0.
 * PROT_NONE yields V|U with no RWX — present but inaccessible, so a touch
 * page-faults (the intended "reserved" semantics).
 * @param prot User PROT_* bit set.
 * @return Leaf PTE flag set.
 * -------------------------------------------------------------------- */
static unsigned long mmap_prot_to_pte(int prot)
{
    unsigned long pte = PTE_V | PTE_U | PTE_A | PTE_D;
    if (prot & PROT_READ)
        pte |= PTE_R;
    if (prot & PROT_WRITE)
        pte |= PTE_W | PTE_R;       /* W must accompany R under Sv39 */
    if (prot & PROT_EXEC)
        pte |= PTE_X;
    return pte;
}

/** ----------------------------------------------------------------------
 * @brief zero_block() – Clear @bytes of a buddy block word by word.
 *
 * Anonymous pages must read as zero so a fresh mapping never leaks the
 * previous owner's data. @p is a linear-map kernel VA; @bytes is a
 * PAGE_SIZE multiple.
 * @param p     Block base (kernel VA).
 * @param bytes Length to clear.
 * -------------------------------------------------------------------- */
static void zero_block(void *p, unsigned long bytes)
{
    unsigned long *w = p;
    for (unsigned long i = 0; i < bytes / sizeof(unsigned long); i++)
        w[i] = 0;
}

/** ----------------------------------------------------------------------
 * @brief do_mmap() – Anonymous, eager mmap() core.
 *
 * Validates length, picks a base (hint or top-down), allocates a zeroed
 * buddy block, maps 4 KiB leaves carrying the converted prot into t->pgd,
 * and records a VMA. On any failure after allocation the partial state is
 * rolled back. A final sfence.vma makes the new leaves visible to U-mode
 * before the syscall returns. Demand paging is intentionally not done:
 * the frames are present immediately.
 * @param t      Calling thread (valid t->pgd required).
 * @param addr   Placement hint.
 * @param length Requested length (rounded up to a page).
 * @param prot   User PROT_* bits.
 * @param flags  User MAP_* bits (MAP_ANONYMOUS required).
 * @return User base VA, or MAP_FAILED on bad args / OOM.
 * -------------------------------------------------------------------- */
void *do_mmap(struct thread *t, void *addr, unsigned long length,
              int prot, int flags)
{
    if (length == 0 || !t->pgd)
        return MAP_FAILED;
    if (!(flags & MAP_ANONYMOUS))   /* only anonymous mappings supported */
        return MAP_FAILED;

    unsigned long len = mmap_round_up_page(length);

    unsigned long base = mmap_find_va(t, (unsigned long)addr, len);
    if (base == 0)
        return MAP_FAILED;

    unsigned long pte_prot = mmap_prot_to_pte(prot);

    void *kva = buddy_alloc(len);
    if (!kva)
        return MAP_FAILED;
    zero_block(kva, len);

    if (map_pages(t->pgd, base, len, virt_to_phys(kva), pte_prot) != 0) {
        buddy_free(kva);
        return MAP_FAILED;
    }

    struct vma *vma = vma_alloc(base, len, pte_prot, kva, 1);
    if (!vma) {
        /* Leaves were installed but unwinding them needs a teardown that
         * uvm_destroy() will do at exit; freeing the frame avoids a leak
         * now while the (orphaned) leaves resolve to nothing referenced. */
        buddy_free(kva);
        return MAP_FAILED;
    }
    vma_insert_sorted(t, vma);

    /* New leaves are live in t->pgd; flush so U-mode sees them at once. */
    asm volatile ("sfence.vma zero, zero" ::: "memory");
    return (void *)base;
}

/** ----------------------------------------------------------------------
 * @brief vma_free_list() – Free every VMA backing block + node on a list.
 *
 * Drains @head from the front: detaches each node, returns its buddy
 * block, and frees the node itself. Leaves @head empty. Shared by
 * vma_unmap_all() (the thread's live list) and sys_exec() (a detached
 * old list). Must run only when satp no longer points at the owning
 * address space.
 * @param head List head of VMAs to release.
 * -------------------------------------------------------------------- */
void vma_free_list(struct list_head *head)
{
    while (!list_empty(head)) {
        struct vma *v = list_entry(head->next, struct vma, link);
        list_del(&v->link);
        if (v->kva)
            buddy_free(v->kva);
        kfree(v);
    }
}

/** ----------------------------------------------------------------------
 * @brief vma_unmap_all() – Free every VMA backing block + node of @t.
 *
 * Thin wrapper that drains t->vma_list via vma_free_list(). Covers
 * image/stack/sigpage and all mmap regions uniformly, so
 * thread_free_user_vm() no longer issues the old per-region buddy_free
 * calls (which would double-free). Page tables and the PGD are reclaimed
 * separately by pgd_free().
 * @param t Thread whose VMAs are to be released.
 * -------------------------------------------------------------------- */
void vma_unmap_all(struct thread *t)
{
    vma_free_list(&t->vma_list);
}

/** ----------------------------------------------------------------------
 * @brief vma_detach_all() – Move @t's VMA list onto @dst, empty @t.
 *
 * Re-points the circular list so every node currently on t->vma_list now
 * hangs off @dst, then re-initialises t->vma_list to empty. No nodes are
 * freed. @dst must be an empty, initialised list head. Used by sys_exec()
 * to set the old address space's VMAs aside before building the new one.
 * @param t   Thread whose list is moved out.
 * @param dst Destination head (empty on entry).
 * -------------------------------------------------------------------- */
void vma_detach_all(struct thread *t, struct list_head *dst)
{
    struct list_head *src = &t->vma_list;
    if (list_empty(src)) {
        INIT_LIST_HEAD(dst);
        return;
    }
    /* Stitch the src ring (minus its head node) onto dst. */
    dst->next       = src->next;
    dst->prev       = src->prev;
    dst->next->prev = dst;
    dst->prev->next = dst;
    INIT_LIST_HEAD(src);
}

/** ----------------------------------------------------------------------
 * @brief vma_reattach() – Move @src's VMAs back onto an empty t->vma_list.
 *
 * Inverse of vma_detach_all(); the sys_exec() failure path uses it to put
 * the old region list back after the new image build failed (and left
 * t->vma_list empty). No nodes are freed. @t->vma_list must be empty.
 * @param t   Thread to restore the list onto.
 * @param src Saved list head holding the detached VMAs.
 * -------------------------------------------------------------------- */
void vma_reattach(struct thread *t, struct list_head *src)
{
    struct list_head *dst = &t->vma_list;
    if (list_empty(src)) {
        INIT_LIST_HEAD(dst);
        return;
    }
    dst->next       = src->next;
    dst->prev       = src->prev;
    dst->next->prev = dst;
    dst->prev->next = dst;
    INIT_LIST_HEAD(src);
}
