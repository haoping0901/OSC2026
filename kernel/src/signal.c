#include "signal.h"
#include "sched.h"
#include "trap.h"
#include "syscall.h"
#include "kmalloc.h"
#include "riscv.h"
#include "uart.h"
#include "utils.h"
#include "list.h"
#include "types.h"
#include "buddy.h"

/*
 * Lab5 Advanced Exercise — POSIX signal core.
 *
 * Dispatch model (no nesting): one user-mode handler may be active per
 * thread at a time. signal_check_and_dispatch() snapshots the entire
 * pre-signal trap_frame into thread->sig.saved, kmalloc()s a per-call
 * signal stack, plants a 2-instruction "li a7, 11; ecall" trampoline
 * at the bottom of that stack, then rewrites the live trap_frame so
 * the impending sret lands in U-mode at the user handler with ra
 * pointing at the trampoline. When the user handler returns, control
 * falls into the trampoline, which raises SYS_SIGRETURN; that syscall
 * restores the saved frame, releases the sigstack, and clears the
 * in_handler gate.
 *
 * The implementation lives outside syscall.c because most of it is
 * pure signal logic (post / dispatch / return / default-terminate /
 * trampoline planting) shared by the syscall layer and the trap-
 * return path in trap.c.
 */

/* RV64I machine encoding of the trampoline planted at sigstack[0]:
 *   addi a7, zero, 11   ; SYS_SIGRETURN
 *   ecall
 */
#define TRAMPOLINE_INST0    0x00b00893u   /* addi a7, zero, 11 */
#define TRAMPOLINE_INST1    0x00000073u   /* ecall             */
#define TRAMPOLINE_BYTES    8u

/** ----------------------------------------------------------------------
 * @brief lowest_bit_index() – Return the position of the LSB set in @x.
 *
 * Avoids __builtin_ctzl(), which on rv64gc without Zbb expands to a
 * libgcc call (__ctzdi2) that we do not link. The freestanding kernel
 * has no libgcc, so we open-code the search instead. Caller must
 * guarantee @x != 0.
 * @param x Non-zero bitmap.
 * @return Bit index in [0, 63] of the lowest set bit.
 * -------------------------------------------------------------------- */
static int lowest_bit_index(unsigned long x)
{
    int idx = 0;
    while ((x & 1UL) == 0) {
        x >>= 1;
        idx++;
    }
    return idx;
}

/** ----------------------------------------------------------------------
 * @brief signal_state_init() – Reset @s to a quiescent default state.
 *
 * Word-zeroes the whole block; pending bitmap, handler table, saved
 * trap_frame and sigstack pointer are all cleared. Called from
 * thread_alloc_bare() before the thread is observable.
 * @param s State block to clear.
 * -------------------------------------------------------------------- */
void signal_state_init(struct signal_state *s)
{
    unsigned long *w = (unsigned long *)s;
    for (unsigned long i = 0; i < sizeof(*s) / sizeof(unsigned long); i++)
        w[i] = 0;
}

/** ----------------------------------------------------------------------
 * @brief signal_post() – Atomically set @signum's pending bit on @t.
 *
 * Bracketed by sie_save_clear/restore so a timer IRQ that interrupts
 * the read-modify-write of the pending bitmap cannot leave it in a
 * torn state. The caller is responsible for argument validation.
 * @param t      Target thread.
 * @param signum Signal number in [1, NSIG).
 * -------------------------------------------------------------------- */
void signal_post(struct thread *t, int signum)
{
    if (!t || signum <= 0 || signum >= NSIG)
        return;

    unsigned long flags = sie_save_clear();
    t->sig.pending |= (1UL << signum);
    sie_restore(flags);
}

/** ----------------------------------------------------------------------
 * @brief signal_release() – Clear the in_handler gate of @t.
 *
 * Used by the process-exit / re-image paths (sys_exit / sys_stop /
 * sys_exec / default terminate) so a thread dying or re-imaging
 * mid-handler leaves a clean signal state. The signal page itself is
 * owned by the process VM (t->sigpage_base) and reclaimed by the reaper,
 * so nothing is freed here.
 * @param t Thread to release.
 * -------------------------------------------------------------------- */
void signal_release(struct thread *t)
{
    if (!t)
        return;
    t->sig.in_handler = 0;
}

/** ----------------------------------------------------------------------
 * @brief signal_default_terminate() – Apply the default "terminate" rule.
 *
 * Mirrors sys_stop()'s remote-pid body: detach @t from the runq if it
 * was READY, mark it ZOMBIE with exit_status = -1, clear the in_handler
 * gate, and wake the parent so a pending waitpid resumes. The user
 * address space and the thread struct/kstack are reclaimed later by the
 * reaper, after satp has switched away from @t.
 * @param t Thread to terminate (may be the current thread).
 * -------------------------------------------------------------------- */
void signal_default_terminate(struct thread *t)
{
    if (!t)
        return;

    unsigned long flags = sie_save_clear();
    t->exit_status = -1;
    if (t->state == THREAD_READY)
        list_del(&t->link);
    t->state = THREAD_ZOMBIE;
    struct thread *par = t->parent;
    sie_restore(flags);

    /* The user address space (image/stack frames + page tables) is
     * reclaimed by the reaper, after satp has switched away from this
     * thread's PGD; freeing it here would risk tearing down the live
     * satp root. Only the kernel-side sigstack is released now. */
    signal_release(t);

    if (par)
        thread_wakeup(par);
}

/** ----------------------------------------------------------------------
 * @brief plant_sigreturn_trampoline() – Write SYS_SIGRETURN stub to ustk.
 *
 * Stores the 8-byte "li a7, 11 ; ecall" sequence at @stack_base so the
 * user handler's ret falls into a 2-instruction shim that crosses back
 * into the kernel via syscall #11. fence.i is issued so the CPU sees
 * the freshly written code on first execution (matters on real silicon
 * with split I/D caches; harmless under QEMU).
 * @param stack_base Low address of the sigstack buffer.
 * -------------------------------------------------------------------- */
static void plant_sigreturn_trampoline(void *stack_base)
{
    unsigned int *w = (unsigned int *)stack_base;
    w[0] = TRAMPOLINE_INST0;
    w[1] = TRAMPOLINE_INST1;
    asm volatile ("fence.i" ::: "memory");
}

/** ----------------------------------------------------------------------
 * @brief signal_check_and_dispatch() – Try to deliver one pending signal.
 *
 * Called from trap_handler() right before sret to U-mode. Skips if a
 * handler is already running (no nesting), or if the pending bitmap is
 * empty. Otherwise picks the lowest-numbered pending signal, clears
 * its bit, and routes by handler kind:
 *   - SIG_IGN       : drop;
 *   - SIG_DFL/NULL  : terminate via signal_default_terminate(); the
 *                     caller in trap.c notices state == ZOMBIE and
 *                     calls schedule() so this thread never sret's;
 *   - user pointer  : snapshot @tf into self->sig.saved, plant the
 *                     trampoline at the signal page base, and rewrite @tf
 *                     so the impending sret lands at the handler with sp
 *                     on the signal page (USER VA) and ra pointing at the
 *                     trampoline's USER VA.
 *
 * tf->a0 is set to the signum so handlers declared `void(int)` see it;
 * `void()` handlers simply ignore the extra argument.
 * @param tf Trap frame about to be restored by trap_return_user.
 * -------------------------------------------------------------------- */
void signal_check_and_dispatch(struct trap_frame *tf)
{
    struct thread *self = get_current();
    if (!self || !self->image_base)
        return;
    if (self->sig.in_handler || self->sig.pending == 0)
        return;

    /* Pick the lowest-numbered pending signal. */
    unsigned long flags = sie_save_clear();
    if (self->sig.pending == 0) {
        sie_restore(flags);
        return;
    }
    int signum = lowest_bit_index(self->sig.pending);
    self->sig.pending &= ~(1UL << signum);
    sie_restore(flags);

    void (*h)(void) = self->sig.handlers[signum];

    if (h == SIG_IGN)
        return;

    if (h == SIG_DFL || h == NULL) {
        signal_default_terminate(self);
        return;
    }

    /* A user process must own a signal page to receive a handler. (Kernel
     * threads were already filtered out by image_base above; this also
     * guards a never-set-up image.) */
    if (!self->sigpage_base)
        return;

    /* User handler path: snapshot full context for sigreturn. mem_cpy
     * avoids the compiler lowering struct assignment into a libc
     * memcpy call (kernel is -nostdlib / no libgcc). */
    mem_cpy(&self->sig.saved, tf, sizeof(*tf));

    /* Plant the trampoline through the page's kernel-VA backing while we
     * are still in S-mode; the handler will reach it at SIGPAGE_VA in U. */
    plant_sigreturn_trampoline(self->sigpage_base);
    self->sig.in_handler = 1;

    /* The handler runs in U-mode, so sp and ra must be the USER VAs of the
     * signal page (PTE_U), not its kernel-VA backing. Stack grows down
     * from the page top; the trampoline sits at the page base (SIGPAGE_VA)
     * and stays reachable via ra. */
    uintptr_t top = SIGPAGE_VA + PAGE_SIZE;
    top &= ~0xFUL;

    tf->sepc = (uintptr_t)h;
    tf->sp   = top;
    tf->a0   = (uintptr_t)signum;
    tf->ra   = SIGPAGE_VA;              /* sigreturn trampoline base */
}

/** ----------------------------------------------------------------------
 * @brief signal_return() – Body of the SYS_SIGRETURN syscall.
 *
 * Restores the pre-signal trap_frame snapshot and clears the in_handler
 * gate so the next pending signal can be delivered on the following
 * trap-return cycle. The signal page is reused across calls (no per-call
 * allocation), so nothing is freed here. Returns the
 * original syscall return value (saved.a0) so that the do_syscall()
 * caller's `tf->a0 = do_syscall(tf)` write preserves whatever a0 was
 * before the signal interrupted U-mode.
 *
 * If no handler was active (user faked a sigreturn) returns -1 and
 * leaves the trap_frame untouched.
 * @param tf Live trap frame currently representing the trampoline ecall.
 * @return saved a0 on success, -1 on misuse.
 * -------------------------------------------------------------------- */
long signal_return(struct trap_frame *tf)
{
    struct thread *self = get_current();
    if (!self->sig.in_handler)
        return -1;

    long ret_a0 = (long)self->sig.saved.a0;

    mem_cpy(tf, &self->sig.saved, sizeof(*tf));

    signal_release(self);

    uart_puts("[sigreturn] handler finished, context restored\n");

    return ret_a0;
}
