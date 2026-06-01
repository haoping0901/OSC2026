/*
 * switch.S – Cooperative context switch primitive.
 *
 *   void switch_to(struct thread *prev, struct thread *next);
 *
 *     a0 = prev (where to save the current context)
 *     a1 = next (whose context to restore)
 *
 * struct thread_ctx layout (must match include/sched.h):
 *
 *     offset  field
 *      0      ra
 *      8      sp
 *     16      s0
 *     24      s1
 *     32      s2
 *     40      s3
 *     48      s4
 *     56      s5
 *     64      s6
 *     72      s7
 *     80      s8
 *     88      s9
 *     96      s10
 *    104      s11
 *
 * Caller-saved registers (t0..t6, a0..a7 except the parameters
 * themselves) are intentionally NOT preserved: switch_to() is called
 * from C, so the compiler has already spilled anything live across
 * the call. We update tp = next as the final step before ret so that
 * get_current() observes the new thread on the very next instruction.
 */

.section .text
.globl switch_to
.align 2
switch_to:
    /* Persist prev->ctx. struct thread.ctx sits at offset 0, so a0
     * itself is the address of the ctx block — no extra add needed. */
    sd  ra,    0(a0)
    sd  sp,    8(a0)
    sd  s0,   16(a0)
    sd  s1,   24(a0)
    sd  s2,   32(a0)
    sd  s3,   40(a0)
    sd  s4,   48(a0)
    sd  s5,   56(a0)
    sd  s6,   64(a0)
    sd  s7,   72(a0)
    sd  s8,   80(a0)
    sd  s9,   88(a0)
    sd  s10,  96(a0)
    sd  s11, 104(a0)

    /* Restore next->ctx. */
    ld  ra,    0(a1)
    ld  sp,    8(a1)
    ld  s0,   16(a1)
    ld  s1,   24(a1)
    ld  s2,   32(a1)
    ld  s3,   40(a1)
    ld  s4,   48(a1)
    ld  s5,   56(a1)
    ld  s6,   64(a1)
    ld  s7,   72(a1)
    ld  s8,   80(a1)
    ld  s9,   88(a1)
    ld  s10,  96(a1)
    ld  s11, 104(a1)

    /* Publish "current thread = next" via the tp register. Anything
     * after this instruction belongs to the next thread. */
    mv  tp, a1
    ret
