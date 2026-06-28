#ifndef __MM_H__
#define __MM_H__

#ifndef __ASSEMBLER__
#include "list.h"
#endif

/*
 * Higher-half linear-map offset.
 *
 * After setup_vm() the whole accessible physical address space is mapped
 * linearly at this fixed offset, so every physical address PA has a
 * canonical kernel virtual address VA = PA + KERNEL_VA_OFFSET. The kernel
 * is linked and runs entirely in this higher-half window.
 *
 * This value MUST stay in sync with:
 *   - the linker VMA base in linker.ld (KERNEL_BASE)
 *   - PAGE_OFFSET used by start.S for the high jump
 */
#define KERNEL_VA_OFFSET   0xffffffc000000000UL

/*
 * First PGD slot belonging to the kernel higher half. KERNEL_VA_OFFSET
 * (0xffff_ffc0_..) has VPN[2] = 256, so PGD indices 256..511 are the
 * kernel half. pgd_alloc() copies these slots into every user PGD so
 * the kernel code/stack/linear-map stay addressable after a satp switch.
 */
#define KERNEL_PGD_HALF    256

#ifndef __ASSEMBLER__

/**
 * phys_to_virt() - map a physical address to its linear kernel VA.
 *
 * Valid only for addresses covered by the linear map established in
 * setup_vm() (all RAM banks plus the MMIO apertures within [0, LINEAR_MAP)).
 */
static inline void *phys_to_virt(unsigned long pa)
{
    return (void *)(pa + KERNEL_VA_OFFSET);
}

/**
 * virt_to_phys() - map a linear kernel VA back to its physical address.
 *
 * Inverse of phys_to_virt(); valid only for higher-half linear-map VAs.
 */
static inline unsigned long virt_to_phys(void *va)
{
    return (unsigned long)va - KERNEL_VA_OFFSET;
}

/* ---------- Boot-time linear map --------------------------------------- */

void setup_vm(void);
void drop_identity_map(void);

/* ---------- Per-process address spaces --------------------------------- */

/**
 * kernel_pgd() - kernel VA of the boot/kernel root page table.
 *
 * Returned to pgd_alloc() (to clone the kernel high half) and to the
 * scheduler (to install when running a kernel-only thread).
 */
unsigned long *kernel_pgd(void);

/**
 * pgd_alloc() - allocate a fresh root page table for a user process.
 *
 * Returns a zeroed 4 KiB-aligned PGD whose high half (indices
 * KERNEL_PGD_HALF..511) is copied from the kernel PGD, so the kernel
 * stays mapped in the new address space. NULL on OOM.
 */
unsigned long *pgd_alloc(void);

/**
 * map_pages() - map [va, va+size) -> [pa, pa+size) at 4 KiB granularity.
 *
 * Walks @pgd, allocating intermediate PMD/PTE tables on demand, and
 * installs leaf PTEs carrying @prot. va/pa/size must be PAGE_SIZE
 * aligned and @prot must include PTE_V. Returns 0 on success, -1 on OOM.
 */
int map_pages(unsigned long *pgd, unsigned long va, unsigned long size,
              unsigned long pa, unsigned long prot);

/**
 * uvm_destroy() - free the user (low) half page tables of @pgd.
 *
 * Walks PGD indices 0..KERNEL_PGD_HALF-1 and frees every intermediate
 * PMD/PTE table. Leaf user frames are NOT freed here (they are owned by
 * the thread's image_base/stack bookkeeping). The shared kernel high
 * half is never touched.
 */
void uvm_destroy(unsigned long *pgd);

/**
 * pgd_free() - tear down a user address space and free its root table.
 *
 * Calls uvm_destroy(@pgd) then frees the PGD page itself. The caller
 * must guarantee satp no longer points at @pgd.
 */
void pgd_free(unsigned long *pgd);

/**
 * mm_set_satp() - install @pgd_va as the active address space.
 *
 * Writes satp (MODE=Sv39, PPN = PA of @pgd_va) and flushes the TLB.
 */
void mm_set_satp(unsigned long *pgd_va);

/* ---------- mmap (anonymous memory) ------------------------------------ */

/*
 * mmap prot bits (user ABI, lab spec). These are USER-facing values, NOT
 * PTE flags; mmap_prot_to_pte() converts them to Sv39 leaf flags. Do not
 * confuse with the kernel-side PROT_USER_* sets in riscv.h.
 */
#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

/* mmap flags (user ABI, lab spec). Only MAP_ANONYMOUS is honoured; this
 * implementation is eager so MAP_POPULATE is implied (no demand paging). */
#define MAP_ANONYMOUS  0x20
#define MAP_POPULATE   0x8000

/* Returned by do_mmap() on failure (POSIX MAP_FAILED). */
#define MAP_FAILED   ((void *)-1)

/*
 * Initial value of thread->mmap_top: one guard page below SIGPAGE_VA so the
 * topmost self-selected mmap region never abuts the signal page (an out-of-
 * bounds access just past a region must fault, not silently hit a neighbour).
 * Self-selected mmap regions grow downward from here. Requires riscv.h for
 * SIGPAGE_VA; both headers are included together by every TU that uses this.
 */
#define MMAP_CURSOR_INIT  (SIGPAGE_VA - PAGE_SIZE)

/*
 * One contiguous user virtual-memory area. Every user region (loaded
 * image, stack, signal page, and each mmap result) is described by one
 * vma linked into thread->vma_list, ordered by ascending va. The backing
 * frames are a single buddy block at kva; map_pages() installs true 4 KiB
 * leaves over [va, va+len) carrying prot.
 */
struct vma {
    struct list_head link;      /* node in thread->vma_list (va-ascending) */
    unsigned long    va;        /* user virtual base (PAGE_SIZE aligned)    */
    unsigned long    len;       /* byte length (PAGE_SIZE aligned)          */
    unsigned long    prot;      /* leaf PTE flag bits already incl. PTE_U   */
    void            *kva;       /* kernel VA of the buddy block backing it  */
    unsigned char    is_mmap;   /* 1 for mmap()'d regions, 0 for img/stk/sig */
};

struct thread;   /* forward decl: defined in sched.h */

/**
 * vma_alloc() - allocate and populate a struct vma.
 *
 * kmalloc's a vma node and fills every field; the caller links it into a
 * thread via vma_insert_sorted(). @prot must already be leaf PTE flags
 * (incl. PTE_U). Returns NULL on OOM.
 */
struct vma *vma_alloc(unsigned long va, unsigned long len,
                      unsigned long prot, void *kva, unsigned char is_mmap);

/**
 * vma_insert_sorted() - link @vma into @t->vma_list keeping va ascending.
 *
 * O(n) linear scan; the list is short (a handful of regions). Caller owns
 * IRQ discipline — current callers run with the address space quiescent
 * (setup / syscall context on the owning thread).
 */
void vma_insert_sorted(struct thread *t, struct vma *vma);

/**
 * vma_unmap_all() - free every VMA frame block and node of @t.
 *
 * Drains @t->vma_list, buddy_free()s each backing block and kfree()s each
 * node. Page tables / PGD are freed separately by pgd_free(). Must run
 * only when satp no longer points at @t->pgd.
 */
void vma_unmap_all(struct thread *t);

/**
 * vma_detach_all() - move @t's VMA list onto @dst, leaving @t empty.
 *
 * Splices the whole vma_list onto a caller-owned, already-initialised
 * list head and re-inits @t->vma_list. Used by sys_exec() to set the old
 * regions aside before building the new address space, so the new and old
 * VMAs never share one list. @dst must be empty on entry.
 */
void vma_detach_all(struct thread *t, struct list_head *dst);

/**
 * vma_reattach() - move VMAs from @src back onto an empty @t->vma_list.
 *
 * Inverse of vma_detach_all(): used on the sys_exec() failure path to
 * restore the old region list after the new image build failed (which
 * leaves t->vma_list empty). @t->vma_list must be empty on entry.
 */
void vma_reattach(struct thread *t, struct list_head *src);

/**
 * vma_free_list() - free every VMA backing block + node on @head.
 *
 * Like vma_unmap_all() but over an arbitrary list head (e.g. one filled
 * by vma_detach_all()). Leaves @head empty. Must run only when satp no
 * longer points at the address space those VMAs belonged to.
 */
void vma_free_list(struct list_head *head);

/**
 * do_mmap() - core of the mmap() syscall (anonymous, eager).
 *
 * Picks a base VA (hint or top-down), allocates and zeroes a backing
 * buddy block, installs 4 KiB leaves carrying the converted prot into
 * @t->pgd, and records a VMA. Returns the user base VA, or MAP_FAILED on
 * bad args / OOM.
 * @param t      Target (calling) thread; must have a valid t->pgd.
 * @param addr   Placement hint (may be 0 / unaligned / overlapping).
 * @param length Requested byte length (rounded up to a page).
 * @param prot   User PROT_* bits.
 * @param flags  User MAP_* bits (MAP_ANONYMOUS required).
 * @return User base VA on success, MAP_FAILED on failure.
 */
void *do_mmap(struct thread *t, void *addr, unsigned long length,
              int prot, int flags);

#endif /* __ASSEMBLER__ */

#endif /* __MM_H__ */
