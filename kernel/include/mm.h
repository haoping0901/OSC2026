#ifndef __MM_H__
#define __MM_H__

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

/* ---------- Boot-time linear map (Lab6 Basic Ex1) ---------------------- */

void setup_vm(void);
void drop_identity_map(void);

/* ---------- Per-process address spaces (Lab6 Basic Ex2) --------------- */

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

#endif /* __ASSEMBLER__ */

#endif /* __MM_H__ */
