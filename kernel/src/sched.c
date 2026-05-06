#include "sched.h"
#include "list.h"
#include "kmalloc.h"
#include "riscv.h"
#include "types.h"

/*
 * Per-thread kernel stack size. 8 KiB exceeds MAX_CHUNK_SIZE so the
 * allocation falls through kmalloc() into buddy_alloc(), giving us a
 * 2-page (8 KiB) region whose base address can later be returned to
 * the buddy allocator via kfree().
 */
#define KSTACK_SIZE  (8 * 1024)

/* FIFO of READY threads. Round-robin order is preserved by always
 * inserting at the tail and dispatching from the head. */
static struct list_head g_runq    = LIST_HEAD_INIT(g_runq);

/* Threads in THREAD_ZOMBIE state, awaiting reaping by kill_zombies()
 * from the idle thread. A thread cannot free its own stack while still
 * executing on it, so this hand-off is mandatory. */
static struct list_head g_zombies = LIST_HEAD_INIT(g_zombies);

static int g_next_tid = 0;

/* Bootstrap thread: represents whatever was running on the boot stack
 * when sched_init() was called. Statically allocated because we cannot
 * yet free a kmalloc'd struct that holds the very stack we are on. */
static struct thread g_bootstrap;

static void idle_thread_body(void);

/** ----------------------------------------------------------------------
 * @brief thread_entry_trampoline() – First-dispatch landing pad.
 *
 * Every freshly created thread "returns" here from its first
 * switch_to(): thread_create() planted this routine's address in
 * ctx.ra. We resolve the user-supplied entry function via
 * get_current()->entry (the tp register has just been set to point at
 * the new thread), invoke it, and on natural return fall through to
 * thread_exit() so a thread body that simply returns will not run off
 * the end of its stack.
 * -------------------------------------------------------------------- */
static void thread_entry_trampoline(void)
{
    struct thread *self = get_current();
    self->entry();
    thread_exit();
}

/** ----------------------------------------------------------------------
 * @brief thread_create() – Allocate a thread and enqueue it as READY.
 *
 * Allocates a thread struct plus a fresh kernel stack, primes the
 * context so the first switch_to() lands in thread_entry_trampoline()
 * with sp pointing at the (16-byte aligned) top of the new stack, and
 * appends the thread to the run queue. Critical-section bracketing
 * around the list_add_tail uses sie_save_clear/sie_restore so future
 * preemption from IRQ context will not observe a half-linked list.
 * @param fn Function the trampoline will invoke once the thread runs.
 * @return   Pointer to the new thread, or NULL on allocation failure.
 * -------------------------------------------------------------------- */
struct thread *thread_create(void (*fn)(void))
{
    struct thread *t = kmalloc(sizeof(*t));
    if (!t)
        return NULL;

    void *stack = kmalloc(KSTACK_SIZE);
    if (!stack) {
        kfree(t);
        return NULL;
    }

    t->tid         = g_next_tid++;
    t->state       = THREAD_READY;
    t->entry       = fn;
    t->kstack_base = stack;
    t->kstack_size = KSTACK_SIZE;
    INIT_LIST_HEAD(&t->link);

    /* Zero all callee-saved slots; the trampoline does not read any of
     * them, but a deterministic initial state simplifies debugging if
     * the first switch ever lands somewhere unexpected. */
    for (unsigned i = 0;
         i < sizeof(t->ctx) / sizeof(unsigned long); i++)
        ((unsigned long *)&t->ctx)[i] = 0;

    /* Stack grows down. Round the top to a 16-byte boundary as
     * required by the RISC-V psABI before storing it in ctx.sp. */
    unsigned long top = (unsigned long)stack + KSTACK_SIZE;
    top &= ~0xFUL;

    t->ctx.ra = (unsigned long)thread_entry_trampoline;
    t->ctx.sp = top;

    unsigned long flags = sie_save_clear();
    list_add_tail(&t->link, &g_runq);
    sie_restore(flags);

    return t;
}

/** ----------------------------------------------------------------------
 * @brief schedule() – Round-robin yield to the next runnable thread.
 *
 * If the caller is still RUNNING (i.e. yielded voluntarily rather than
 * exiting), demote it to READY and append to the run queue tail so the
 * other threads cycle through before it runs again. If the queue ends
 * up empty, the caller simply continues — this happens when the idle
 * thread is alone on the system. Otherwise pop the head, mark it
 * RUNNING, and hand control to switch_to(); the latter publishes
 * tp = next so get_current() observes the switch immediately.
 *
 * The list mutation runs with sstatus.SIE cleared so that a future
 * preempting IRQ cannot see a half-linked queue. switch_to() itself is
 * called outside the critical section because the new thread expects
 * to inherit a normal (interruptible) state, and the act of switching
 * stacks safely carries the SIE bit forward via sstatus.
 * -------------------------------------------------------------------- */
void schedule(void)
{
    unsigned long flags = sie_save_clear();

    struct thread *prev = get_current();

    if (prev->state == THREAD_RUNNING) {
        prev->state = THREAD_READY;
        list_add_tail(&prev->link, &g_runq);
    }

    if (list_empty(&g_runq)) {
        /* Nobody else is ready — keep prev on the CPU. Restore the
         * RUNNING state we may have just cleared. */
        prev->state = THREAD_RUNNING;
        sie_restore(flags);
        return;
    }

    struct thread *next = list_entry(g_runq.next, struct thread, link);
    list_del(&next->link);
    next->state = THREAD_RUNNING;

    sie_restore(flags);

    if (prev != next)
        switch_to(prev, next);
}

/** ----------------------------------------------------------------------
 * @brief thread_exit() – Mark the current thread zombie and yield.
 *
 * Flips state to THREAD_ZOMBIE and links onto g_zombies so the idle
 * thread's kill_zombies() pass can free both the kernel stack and the
 * thread struct. We then call schedule(), which observes the non-
 * RUNNING state and therefore does NOT re-enqueue us; control passes
 * to the next runnable thread and never comes back. The trailing
 * for(;;) is unreachable but satisfies the noreturn contract should
 * a future bug let schedule() return.
 * -------------------------------------------------------------------- */
void thread_exit(void)
{
    unsigned long flags = sie_save_clear();
    struct thread *self = get_current();
    self->state = THREAD_ZOMBIE;
    list_add_tail(&self->link, &g_zombies);
    sie_restore(flags);

    schedule();

    for (;;)
        ;
}

/** ----------------------------------------------------------------------
 * @brief kill_zombies() – Reap exited threads from the idle context.
 *
 * Walks g_zombies, detaches each node under SIE=0 (so a future IRQ
 * cannot race the list mutation), then frees the kernel stack and the
 * thread struct outside the critical section. The bootstrap thread
 * uses kstack_base = NULL as a sentinel to opt out of stack freeing
 * (it sits on the boot stack carved out by the linker, not on a
 * kmalloc'd region).
 * -------------------------------------------------------------------- */
static void kill_zombies(void)
{
    for (;;) {
        unsigned long flags = sie_save_clear();
        if (list_empty(&g_zombies)) {
            sie_restore(flags);
            return;
        }
        struct thread *z = list_entry(g_zombies.next,
                                      struct thread, link);
        list_del(&z->link);
        sie_restore(flags);

        if (z->kstack_base)
            kfree(z->kstack_base);
        if (z != &g_bootstrap)
            kfree(z);
    }
}

/** ----------------------------------------------------------------------
 * @brief idle_thread_body() – Background reaper and yield loop.
 *
 * Permanently runnable thread that recycles zombies and yields. By
 * keeping the run queue non-empty even when no real work exists, we
 * sidestep the corner case of schedule() finding nothing to dispatch.
 * The loop never returns; the trampoline's fall-through to
 * thread_exit() is a defensive backstop only.
 * -------------------------------------------------------------------- */
static void idle_thread_body(void)
{
    for (;;) {
        kill_zombies();
        schedule();
    }
}

/** ----------------------------------------------------------------------
 * @brief sched_init() – Adopt the boot context and spawn the idle thread.
 *
 * The boot path arrives here running on _stack_top with no thread
 * struct backing it. We retro-fit g_bootstrap as that thread: tp is
 * set so subsequent get_current() calls work, state is RUNNING because
 * we are literally executing on it, and kstack_base is left NULL so
 * kill_zombies() will never attempt to kfree the linker-provided
 * boot stack. Finally thread_create(idle_thread_body) installs an
 * always-runnable thread so the run queue is never empty.
 * -------------------------------------------------------------------- */
void sched_init(void)
{
    g_bootstrap.tid         = g_next_tid++;
    g_bootstrap.state       = THREAD_RUNNING;
    g_bootstrap.entry       = NULL;
    g_bootstrap.kstack_base = NULL;
    g_bootstrap.kstack_size = 0;
    INIT_LIST_HEAD(&g_bootstrap.link);
    for (unsigned i = 0;
         i < sizeof(g_bootstrap.ctx) / sizeof(unsigned long); i++)
        ((unsigned long *)&g_bootstrap.ctx)[i] = 0;

    asm volatile ("mv tp, %0" :: "r"(&g_bootstrap));

    thread_create(idle_thread_body);
}
