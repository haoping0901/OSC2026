/*
 * trap_entry.S – S-mode trap vector and U-mode entry helper.
 *
 * The trap vector uses the sscratch CSR to distinguish the two trap
 * sources during Basic Ex1:
 *   - sscratch != 0 : trap was taken from U-mode; sscratch holds the
 *                     kernel sp that enter_user_mode() stashed. Swap
 *                     it with the current (user) sp to run on the
 *                     kernel stack.
 *   - sscratch == 0 : trap was taken while the kernel was already
 *                     running. Ex1 does not expect this path; the
 *                     fallback halts the kernel.
 */

#include "trap.h"

.section .text.trap, "ax"

.globl trap_entry
.align 2                            /* stvec direct mode: 4-byte aligned */
trap_entry:
    /* Swap sp with sscratch so sp points at the kernel stack. */
    csrrw sp, sscratch, sp
    beqz  sp, .Lfrom_kernel

    /* Reserve a trap_frame on the kernel stack and save GPRs. */
    addi  sp, sp, -TF_SIZE

    sd    x1,  TF_RA(sp)
    /* x2 (sp) is stashed below from the old sscratch value. */
    sd    x3,  TF_GP(sp)
    sd    x4,  TF_TP(sp)
    sd    x5,  TF_T0(sp)
    sd    x6,  TF_T1(sp)
    sd    x7,  TF_T2(sp)
    sd    x8,  TF_S0(sp)
    sd    x9,  TF_S1(sp)
    sd    x10, TF_A0(sp)
    sd    x11, TF_A1(sp)
    sd    x12, TF_A2(sp)
    sd    x13, TF_A3(sp)
    sd    x14, TF_A4(sp)
    sd    x15, TF_A5(sp)
    sd    x16, TF_A6(sp)
    sd    x17, TF_A7(sp)
    sd    x18, TF_S2(sp)
    sd    x19, TF_S3(sp)
    sd    x20, TF_S4(sp)
    sd    x21, TF_S5(sp)
    sd    x22, TF_S6(sp)
    sd    x23, TF_S7(sp)
    sd    x24, TF_S8(sp)
    sd    x25, TF_S9(sp)
    sd    x26, TF_S10(sp)
    sd    x27, TF_S11(sp)
    sd    x28, TF_T3(sp)
    sd    x29, TF_T4(sp)
    sd    x30, TF_T5(sp)
    sd    x31, TF_T6(sp)

    /* sscratch currently holds the user sp captured at entry. */
    csrr  t0, sscratch
    sd    t0, TF_SP(sp)
    /* Mark "currently in kernel" for any nested trap check later. */
    csrw  sscratch, zero

    /* Save the four supervisor CSRs that describe the trap. */
    csrr  t0, sepc
    csrr  t1, sstatus
    csrr  t2, scause
    csrr  t3, stval
    sd    t0, TF_SEPC(sp)
    sd    t1, TF_SSTATUS(sp)
    sd    t2, TF_SCAUSE(sp)
    sd    t3, TF_STVAL(sp)

    /* Dispatch to C. */
    mv    a0, sp
    call  trap_handler

    /* Restore sepc / sstatus (scause / stval are read-only on return). */
    ld    t0, TF_SEPC(sp)
    ld    t1, TF_SSTATUS(sp)
    csrw  sepc, t0
    csrw  sstatus, t1

    /* Restore general-purpose registers (x2/sp handled specially). */
    ld    x1,  TF_RA(sp)
    ld    x3,  TF_GP(sp)
    ld    x4,  TF_TP(sp)
    ld    x5,  TF_T0(sp)
    ld    x6,  TF_T1(sp)
    ld    x7,  TF_T2(sp)
    ld    x8,  TF_S0(sp)
    ld    x9,  TF_S1(sp)
    ld    x10, TF_A0(sp)
    ld    x11, TF_A1(sp)
    ld    x12, TF_A2(sp)
    ld    x13, TF_A3(sp)
    ld    x14, TF_A4(sp)
    ld    x15, TF_A5(sp)
    ld    x16, TF_A6(sp)
    ld    x17, TF_A7(sp)
    ld    x18, TF_S2(sp)
    ld    x19, TF_S3(sp)
    ld    x20, TF_S4(sp)
    ld    x21, TF_S5(sp)
    ld    x22, TF_S6(sp)
    ld    x23, TF_S7(sp)
    ld    x24, TF_S8(sp)
    ld    x25, TF_S9(sp)
    ld    x26, TF_S10(sp)
    ld    x27, TF_S11(sp)
    ld    x28, TF_T3(sp)
    ld    x29, TF_T4(sp)
    ld    x30, TF_T5(sp)
    ld    x31, TF_T6(sp)

    /* Load saved user sp into t0, then free the frame. */
    ld    t0, TF_SP(sp)
    addi  sp, sp, TF_SIZE
    /* Park the kernel sp in sscratch for the next U→S trap. */
    csrw  sscratch, sp
    mv    sp, t0
    sret

.Lfrom_kernel:
    /*
     * Trap occurred while the kernel was already running.
     * Not expected for Basic Ex1; halt loudly so the developer sees it.
     * sp is currently zero (swapped from sscratch); restore it from the
     * stashed value and fall into a C panic routine.
     */
    csrrw sp, sscratch, sp          /* restore sp, keep sscratch=0 */
    call  kernel_trap_panic
1:  wfi
    j     1b


/* ------------------------------------------------------------------ */
/* enter_user_mode(entry = a0, user_sp = a1)                          */
/* ------------------------------------------------------------------ */
.globl enter_user_mode
.align 2
enter_user_mode:
    /* Remember the current kernel sp for the eventual U→S trap. */
    csrw  sscratch, sp

    /* Program the return PC. */
    csrw  sepc, a0

    /* Clear SPP bit (sstatus[8]) so sret returns to U-mode. */
    li    t0, (1 << 8)
    csrc  sstatus, t0
    /* Set SPIE bit (sstatus[5]) so SIE becomes 1 after sret. */
    li    t0, (1 << 5)
    csrs  sstatus, t0

    /* Switch to the user stack and clear argument registers to avoid
     * leaking kernel state into U-mode. */
    mv    sp, a1
    li    a0, 0
    li    a1, 0
    li    a2, 0
    li    a3, 0
    li    a4, 0
    li    a5, 0
    li    a6, 0
    li    a7, 0
    li    t0, 0
    li    t1, 0
    li    t2, 0
    li    t3, 0
    li    t4, 0
    li    t5, 0
    li    t6, 0

    sret
