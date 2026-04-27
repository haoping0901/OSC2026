#ifndef __RISCV_H__
#define __RISCV_H__

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
