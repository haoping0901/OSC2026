#ifndef __TRAP_H__
#define __TRAP_H__

#ifndef __ASSEMBLER__
#include "types.h"
#endif

/* ---------- trap_frame field offsets (used by trap_entry.S) ------------- */
#define TF_RA        0
#define TF_SP        8
#define TF_GP        16
#define TF_TP        24
#define TF_T0        32
#define TF_T1        40
#define TF_T2        48
#define TF_S0        56
#define TF_S1        64
#define TF_A0        72
#define TF_A1        80
#define TF_A2        88
#define TF_A3        96
#define TF_A4        104
#define TF_A5        112
#define TF_A6        120
#define TF_A7        128
#define TF_S2        136
#define TF_S3        144
#define TF_S4        152
#define TF_S5        160
#define TF_S6        168
#define TF_S7        176
#define TF_S8        184
#define TF_S9        192
#define TF_S10       200
#define TF_S11       208
#define TF_T3        216
#define TF_T4        224
#define TF_T5        232
#define TF_T6        240
#define TF_SEPC      248
#define TF_SSTATUS   256
#define TF_SCAUSE    264
#define TF_STVAL     272
#define TF_SIZE      280

#ifndef __ASSEMBLER__

/*
 * Layout mirrors the TF_* offsets above.  Order matters: trap_entry.S
 * relies on these offsets when saving/restoring general-purpose registers.
 */
struct trap_frame {
    uintptr_t ra;          /* x1  */
    uintptr_t sp;          /* x2  */
    uintptr_t gp;          /* x3  */
    uintptr_t tp;          /* x4  */
    uintptr_t t0;          /* x5  */
    uintptr_t t1;          /* x6  */
    uintptr_t t2;          /* x7  */
    uintptr_t s0;          /* x8  */
    uintptr_t s1;          /* x9  */
    uintptr_t a0;          /* x10 */
    uintptr_t a1;          /* x11 */
    uintptr_t a2;          /* x12 */
    uintptr_t a3;          /* x13 */
    uintptr_t a4;          /* x14 */
    uintptr_t a5;          /* x15 */
    uintptr_t a6;          /* x16 */
    uintptr_t a7;          /* x17 */
    uintptr_t s2;          /* x18 */
    uintptr_t s3;          /* x19 */
    uintptr_t s4;          /* x20 */
    uintptr_t s5;          /* x21 */
    uintptr_t s6;          /* x22 */
    uintptr_t s7;          /* x23 */
    uintptr_t s8;          /* x24 */
    uintptr_t s9;          /* x25 */
    uintptr_t s10;         /* x26 */
    uintptr_t s11;         /* x27 */
    uintptr_t t3;          /* x28 */
    uintptr_t t4;          /* x29 */
    uintptr_t t5;          /* x30 */
    uintptr_t t6;          /* x31 */
    uintptr_t sepc;
    uintptr_t sstatus;
    uintptr_t scause;
    uintptr_t stval;
};

/** ----------------------------------------------------------------------
 * @brief trap_init() – Install the S-mode trap vector.
 *
 * Writes trap_entry into stvec (direct mode) and clears sscratch so that
 * a trap taken while still in S-mode can be distinguished from a trap
 * taken from U-mode via the sscratch==0 test inside trap_entry.
 * -------------------------------------------------------------------- */
void trap_init(void);

/** ----------------------------------------------------------------------
 * @brief trap_handler() – C-level dispatcher for S-mode traps.
 *
 * Invoked by trap_entry with a pointer to the saved trap frame. Handles
 * ECALL_FROM_U by advancing sepc past the ecall instruction; every other
 * non-interrupt cause currently halts the kernel.
 * @param[in,out] tf Pointer to the saved trap frame on the kernel stack.
 * -------------------------------------------------------------------- */
void trap_handler(struct trap_frame *tf);

/** ----------------------------------------------------------------------
 * @brief trap_set_user_base() – Record the current user program base.
 *
 * Stores the absolute address at which the most recently launched user
 * program was loaded. trap_handler() subtracts this value from sepc so
 * that the diagnostic output shows a program-relative offset instead of
 * a kmalloc() allocation address.
 * @param base Absolute load address of the running user program.
 * -------------------------------------------------------------------- */
void trap_set_user_base(uintptr_t base);

/** ----------------------------------------------------------------------
 * @brief trap_set_uart_irq() – Register the UART0 IRQ id.
 *
 * Tells the trap dispatcher which PLIC IRQ id should be routed to the
 * UART interrupt handler. Call this after DTB parsing and before
 * enabling sie.SEIE.
 * @param irq PLIC IRQ source id for UART0.
 * -------------------------------------------------------------------- */
void trap_set_uart_irq(unsigned int irq);

/** ----------------------------------------------------------------------
 * @brief enter_user_mode() – Drop from S-mode into U-mode via sret.
 *
 * Stashes the current kernel sp into sscratch, programs sepc with the
 * user entry point, clears sstatus.SPP, sets sstatus.SPIE, switches sp
 * to the user stack, then executes sret. Does not return.
 * @param entry   Absolute address of the first user instruction.
 * @param user_sp 16-byte aligned user stack pointer.
 * -------------------------------------------------------------------- */
__attribute__((noreturn))
void enter_user_mode(uintptr_t entry, uintptr_t user_sp);

#endif /* !__ASSEMBLER__ */

#endif /* __TRAP_H__ */
