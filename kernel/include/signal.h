#ifndef __SIGNAL_H__
#define __SIGNAL_H__

#include "trap.h"
#include "types.h"

struct thread;

/*
 * Lab5 Advanced Exercise — POSIX-style asynchronous signal delivery.
 *
 * Each user thread owns a struct signal_state that tracks:
 *   - pending signals as a 32-bit bitmap;
 *   - a per-signum handler table (NULL/SIG_DFL = terminate the target,
 *     SIG_IGN = silently drop, anything else = jump to user-mode handler);
 *   - an in_handler re-entrancy gate that disables nested dispatch;
 *   - a saved trap_frame snapshot used by sigreturn to restore the
 *     pre-signal user context;
 *   - the base of the per-invocation user stack (kmalloc'd) that the
 *     handler runs on, with a tiny sigreturn trampoline planted at its
 *     low address.
 *
 * Dispatch happens at the trap_handler() return-to-U-mode out-edges,
 * after schedule(), so newly-scheduled threads see pending signals on
 * their next sret. There is no real-time guarantee; targets receive
 * the signal when next they are about to enter U-mode.
 */

#define NSIG            32
#define SIG_DFL         ((void (*)(void))0)
#define SIG_IGN         ((void (*)(void))1)
#define SIGTERM         15
#define SIGSTACK_SIZE   (8 * 1024)

struct signal_state {
    unsigned long pending;
    void        (*handlers[NSIG])(void);
    int           in_handler;
    struct trap_frame saved;
    void         *sigstack_base;
};

/** ----------------------------------------------------------------------
 * @brief signal_state_init() – Reset a thread's signal state to defaults.
 *
 * Called from thread_alloc_bare() so every new thread starts with no
 * pending signals, no registered handlers, no active sigstack and the
 * in_handler gate cleared.
 * @param s State block to clear.
 * -------------------------------------------------------------------- */
void signal_state_init(struct signal_state *s);

/** ----------------------------------------------------------------------
 * @brief signal_post() – Mark @signum pending on the target thread.
 *
 * Sets the corresponding bit in @t->sig.pending under SIE-clear so the
 * read-modify-write cannot be torn by an interrupt. Does not wake the
 * target — delivery happens the next time the target is about to
 * return to U-mode.
 * @param t      Target thread (must be a user process).
 * @param signum Signal number in [1, NSIG).
 * -------------------------------------------------------------------- */
void signal_post(struct thread *t, int signum);

/** ----------------------------------------------------------------------
 * @brief signal_check_and_dispatch() – Deliver one pending signal.
 *
 * Invoked by trap_handler() right before sret to U-mode. If the
 * current thread has at least one pending signal and is not already
 * running a handler, this picks the lowest-numbered pending bit and:
 *   - SIG_IGN  → drops it and returns;
 *   - SIG_DFL  → terminates the thread (caller must then schedule());
 *   - user fn  → snapshots tf, allocates a sigstack, plants the
 *                sigreturn trampoline, and rewrites tf to land in U at
 *                the handler with ra pointing at the trampoline.
 * @param tf Trap frame about to be restored by trap_return_user.
 * -------------------------------------------------------------------- */
void signal_check_and_dispatch(struct trap_frame *tf);

/** ----------------------------------------------------------------------
 * @brief signal_return() – Restore pre-signal context (SYS_SIGRETURN).
 *
 * Body of the SYS_SIGRETURN syscall. Replaces @tf with the saved
 * snapshot taken at dispatch time, releases the sigstack, and clears
 * the in_handler gate. The returned long is whatever the do_syscall()
 * dispatcher will write back to tf->a0 — we return saved.a0 so the
 * original a0 (already in tf after the *tf = saved copy) survives.
 * @param tf Trap frame currently in U-handler context.
 * @return The original syscall return value, or -1 if no handler was
 *         active (i.e. user faked a sigreturn).
 * -------------------------------------------------------------------- */
long signal_return(struct trap_frame *tf);

/** ----------------------------------------------------------------------
 * @brief signal_default_terminate() – Default-handler "terminate" path.
 *
 * Equivalent to sys_stop()'s remote-pid branch: detaches the victim
 * from the runq if READY, marks it ZOMBIE, frees its image, wakes the
 * parent if blocked in waitpid. Used by signal_check_and_dispatch()
 * when the handler slot is SIG_DFL/NULL and also by the OOM fallback.
 * @param t Thread to terminate.
 * -------------------------------------------------------------------- */
void signal_default_terminate(struct thread *t);

/** ----------------------------------------------------------------------
 * @brief signal_release() – Free any sigstack still attached to @t.
 *
 * Invoked from sys_exit() / sys_stop() so the per-handler buffer is
 * not leaked when a thread dies mid-handler. Safe to call when no
 * sigstack is allocated.
 * @param t Thread whose signal state should be quiesced.
 * -------------------------------------------------------------------- */
void signal_release(struct thread *t);

#endif /* __SIGNAL_H__ */
