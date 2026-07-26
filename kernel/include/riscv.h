#ifndef __RISCV_H__
#define __RISCV_H__

/* ---------- Sv39 paging: satp / PTE ------------------------------------- */

/* satp MODE field: 8 selects Sv39 (three-level, 39-bit VA). */
#define SATP_SV39           (8UL << 60)

/* Build a satp value: MODE=Sv39, ASID=0, PPN = root page table PA >> 12. */
#define MAKE_SATP(pgd_pa)   (SATP_SV39 | ((unsigned long)(pgd_pa) >> 12))

/* PTE permission / status flag bits. */
#define PTE_V   (1UL << 0)   /* Valid                                     */
#define PTE_R   (1UL << 1)   /* Readable                                  */
#define PTE_W   (1UL << 2)   /* Writable                                  */
#define PTE_X   (1UL << 3)   /* Executable                                */
#define PTE_U   (1UL << 4)   /* User-accessible                           */
#define PTE_G   (1UL << 5)   /* Global mapping                            */
#define PTE_A   (1UL << 6)   /* Accessed                                  */
#define PTE_D   (1UL << 7)   /* Dirty                                     */

/*
 * Software-defined PTE bit (Sv39 RSW field, bits 8-9: ignored by the MMU).
 * PTE_COW marks a leaf whose frame is copy-on-write shared between address
 * spaces: PTE_W has been withheld even though the owning VMA allows writes,
 * so a store raises a page fault that the kernel resolves by breaking the
 * share (copy or in-place upgrade) instead of killing the process.
 */
#define PTE_COW (1UL << 8)   /* RSW: frame is CoW-shared, W withheld      */

/* Common leaf permission sets for the kernel linear map. */
#define PROT_KERNEL  (PTE_V | PTE_R | PTE_W | PTE_X | PTE_G | PTE_A | PTE_D)
#define PROT_DEVICE  (PTE_V | PTE_R | PTE_W | PTE_G | PTE_A | PTE_D)

/* ---------- User virtual address layout --------------------------------- */

/*
 * Fixed user VA layout mandated by the lab spec. Every user process sees
 * the same VAs (image at 0, stack growing down from USER_STACK_TOP);
 * isolation comes from each process owning a private set of physical
 * frames behind these identical VAs.
 *
 *   USER_CODE_VA   image (code/data/bss) loaded here — required because
 *                  osctest.bin is position-dependent and uses absolute
 *                  low addresses (e.g. 0x1e8480).
 *   USER_STACK_TOP exclusive top of the user stack (PGD index 1, fully
 *                  disjoint from the image's PGD index 0).
 */
#define USER_CODE_VA      0x0UL
#define USER_STACK_TOP    0x0000004000000000UL   /* 0x40_0000_0000 (256 GiB) */

/*
 * Per-process signal page. A POSIX signal handler runs in U-mode, so
 * the stack it executes on and the sigreturn trampoline it returns into
 * must be USER virtual addresses carrying PTE_U — a kmalloc'd kernel VA
 * is unreachable from U-mode under Sv39 and would fault on first use. We
 * map one dedicated PROT_USER_RWX page per process to hold both: the
 * trampoline at SIGPAGE_VA (page base) and the handler stack growing
 * down from the page top.
 *
 * VPN[2]=255, VPN[1]=0, VPN[0]=0: a PGD slot distinct from the image
 * (VPN[2]=0); it shares PGD slot 255 with the user stack but lives in a
 * disjoint PMD (0 vs 511), so their page tables never overlap. Stays
 * below USER_STACK_TOP.
 */
#define SIGPAGE_VA        0x0000003FC0000000UL

/*
 * User-page permission sets. All carry PTE_U; A/D are pre-set so a first
 * touch never faults (we do not implement A/D-driven paging).
 *
 * Raw binaries carry no ELF section info to separate text from data, so
 * the whole image is mapped PROT_USER_RWX. PROT_USER_CODE/DATA are kept
 * for a future ELF-aware loader that can enforce W^X.
 */
#define PROT_USER_CODE  (PTE_V | PTE_R | PTE_X | PTE_U | PTE_A | PTE_D)
#define PROT_USER_DATA  (PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D)
#define PROT_USER_RWX   (PTE_V | PTE_R | PTE_W | PTE_X | PTE_U | PTE_A | PTE_D)

/*
 * Sv39 page-table walk shifts. A 39-bit VA splits into three 9-bit VPN
 * fields plus a 12-bit page offset:
 *   VPN[2] = bits 38..30   (PGD index, 1 GiB stride)
 *   VPN[1] = bits 29..21   (PMD index, 2 MiB stride)
 *   VPN[0] = bits 20..12   (PTE index, 4 KiB stride)
 */
#define PGD_SHIFT   30
#define PMD_SHIFT   21
#define PTE_SHIFT   12
#define PTRS_PER_TABLE   512   /* entries per page table (4 KiB / 8 B)   */

/* Stride in bytes covered by one entry at each level. */
#define PGD_SIZE    (1UL << PGD_SHIFT)   /* 1 GiB                          */
#define PMD_SIZE    (1UL << PMD_SHIFT)   /* 2 MiB                          */

/*
 * Compose a PTE from a physical address and flag bits.
 * PPN occupies bits [53:10], i.e. (PA >> 12) << 10.
 */
#define MAKE_PTE(pa, flags) \
    ((((unsigned long)(pa) >> 12) << 10) | (unsigned long)(flags))

/* Extract the next-level table PA from a non-leaf PTE. */
#define PTE_TO_PA(pte)   ((((unsigned long)(pte) >> 10) << 12))

/* Flag bits of a PTE: V..D plus the RSW software bits (bits 0-9). */
#define PTE_FLAGS_MASK   ((1UL << 10) - 1)

/*
 * Per-level page-table index extraction for an Sv39 walk.
 *   level 2 -> PGD (VPN[2]), level 1 -> PMD (VPN[1]), level 0 -> PTE (VPN[0])
 * Each level is a 9-bit field; PTE_SHIFT (12) is the page offset width.
 */
#define PT_INDEX(va, level) \
    (((unsigned long)(va) >> (PTE_SHIFT + (level) * 9)) & (PTRS_PER_TABLE - 1))

/* ---------- sstatus bits ------------------------------------------------ */
#define SSTATUS_SIE   (1UL << 1)   /* S-mode interrupt enable            */
#define SSTATUS_SPIE  (1UL << 5)   /* previous SIE before trap           */
#define SSTATUS_SPP   (1UL << 8)   /* previous privilege: 0=U, 1=S       */
#define SSTATUS_SUM   (1UL << 18)  /* permit S-mode to access U pages    */

/* ---------- sie / sip bits ---------------------------------------------- */
#define SIE_SSIE  (1UL << 1)
#define SIE_STIE  (1UL << 5)
#define SIE_SEIE  (1UL << 9)

/* ---------- scause ------------------------------------------------------ */
#define SCAUSE_INTR_BIT   (1UL << 63)

/* Interrupt causes (low bits when SCAUSE_INTR_BIT is set) */
#define INTR_S_SOFT       1
#define INTR_S_TIMER      5
#define INTR_S_EXT        9

/* Exception causes */
#define EXC_INST_MISALIGN    0
#define EXC_INST_ACCESS      1
#define EXC_ILLEGAL_INST     2
#define EXC_BREAKPOINT       3
#define EXC_LOAD_MISALIGN    4
#define EXC_LOAD_ACCESS      5
#define EXC_STORE_MISALIGN   6
#define EXC_STORE_ACCESS     7
#define EXC_ECALL_U          8
#define EXC_ECALL_S          9
#define EXC_INST_PAGE_FAULT  12
#define EXC_LOAD_PAGE_FAULT  13
#define EXC_STORE_PAGE_FAULT 15

/* ---------- S-mode interrupt critical-section helpers ------------------ */

/** ----------------------------------------------------------------------
 * @brief sie_save_clear() – Atomically clear sstatus.SIE, return old bit.
 *
 * Uses csrrc so the read-modify-write is a single instruction and cannot
 * be split by a trap. The returned value is the previous SIE bit only
 * (masked), suitable to be fed back to sie_restore().
 * @return Previous sstatus.SIE bit (0 or SSTATUS_SIE).
 * -------------------------------------------------------------------- */
static inline unsigned long sie_save_clear(void)
{
    unsigned long prev;
    asm volatile ("csrrc %0, sstatus, %1"
                  : "=r"(prev) : "r"((unsigned long)SSTATUS_SIE));
    return prev & SSTATUS_SIE;
}

/** ----------------------------------------------------------------------
 * @brief sie_restore() – Re-assert sstatus.SIE iff it was previously set.
 *
 * Pair with sie_save_clear() to bracket a short critical section that
 * must not be pre-empted by an S-mode interrupt.
 * @param bit Value previously returned by sie_save_clear().
 * -------------------------------------------------------------------- */
static inline void sie_restore(unsigned long bit)
{
    if (bit)
        asm volatile ("csrs sstatus, %0" :: "r"(bit));
}

#endif /* __RISCV_H__ */
