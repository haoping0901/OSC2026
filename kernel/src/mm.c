#include "mm.h"
#include "riscv.h"
#include "buddy.h"   /* PAGE_SIZE */
#include "types.h"

/*
 * Early Sv39 page-table setup for the higher-half kernel (Lab6 Basic Ex1).
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
