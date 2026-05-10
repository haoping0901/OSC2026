#include "sched.h"
#include "list.h"
#include "kmalloc.h"
#include "riscv.h"
#include "types.h"
#include "trap.h"
#include "cpio.h"
#include "dtb.h"
#include "uart.h"
#include "utils.h"

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
 * yet free a kmalloc'd struct that holds the very stack we are on.
 *
 * Exposed via sched.h (extern) so sys_exit() can reparent orphaned
 * children onto it (the bootstrap thread is also the shell, which is
 * the natural conceptual "init" of this kernel).
 */
struct thread g_bootstrap;

/* Volatile because timer ISR writes and trap_handler reads on the same
 * hart without explicit synchronization; single-hart, so a plain
 * volatile load/store is sufficient. */
static volatile int g_need_resched;

extern void user_thread_bootstrap(void);   /* trap_entry.S */

static void idle_thread_body(void);
static void zero_words(void *p, unsigned long bytes);
static struct thread *dfs_find_pid(struct thread *root, int pid);

/** ----------------------------------------------------------------------
 * @brief zero_words() – Word-granular zeroing helper.
 *
 * The kernel is built with -nostdlib so a tiny inline replacement for
 * memset(0) lives here. @bytes must be a multiple of sizeof(unsigned
 * long); all current callers pass a struct size that satisfies this.
 * @param p     Region base.
 * @param bytes Number of bytes to clear (multiple of word size).
 * -------------------------------------------------------------------- */
static void zero_words(void *p, unsigned long bytes)
{
    unsigned long *w = p;
    for (unsigned long i = 0; i < bytes / sizeof(unsigned long); i++)
        w[i] = 0;
}

/** ----------------------------------------------------------------------
 * @brief thread_entry_trampoline() – First-dispatch landing pad.
 *
 * Every freshly created kernel-mode thread "returns" here from its
 * first switch_to(): thread_create() planted this routine's address
 * in ctx.ra. We resolve the user-supplied entry function via
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
 * @brief thread_create() – Allocate a kernel thread and enqueue it.
 *
 * Used for kernel-mode workers (idle, threadtest, etc.). For user
 * processes use thread_spawn_user() / sys_fork() instead, which build
 * a trap_frame on the kstack and route through user_thread_bootstrap.
 * @param fn Function the trampoline will invoke once the thread runs.
 * @return   Pointer to the new thread, or NULL on allocation failure.
 * -------------------------------------------------------------------- */
struct thread *thread_create(void (*fn)(void))
{
    struct thread *t = thread_alloc_bare();
    if (!t)
        return NULL;

    t->entry = fn;

    /* Stack grows down. Round the top to a 16-byte boundary as
     * required by the RISC-V psABI before storing it in ctx.sp. */
    unsigned long top = (unsigned long)t->kstack_base + KSTACK_SIZE;
    top &= ~0xFUL;

    t->ctx.ra = (unsigned long)thread_entry_trampoline;
    t->ctx.sp = top;

    /* Link into parent's children list so the process tree walker
     * (find_thread_by_pid, sys_exit reparenting) sees kernel workers
     * too. For the very first call (creating idle from sched_init)
     * parent is g_bootstrap, which is correct. */
    unsigned long flags = sie_save_clear();
    if (t->parent)
        list_add_tail(&t->sibling, &t->parent->children);
    list_add_tail(&t->link, &g_runq);
    sie_restore(flags);

    return t;
}

/** ----------------------------------------------------------------------
 * @brief thread_alloc_bare() – Allocate a thread/kstack without wiring.
 *
 * Pairs with thread_spawn_user() / sys_fork() which build a trap_frame
 * on the new kstack manually and set ctx.{sp,ra} so the first
 * switch_to() lands at user_thread_bootstrap → trap_return_user.
 * The thread is NOT linked into any list yet; caller wires
 * sibling/children and calls thread_enqueue_ready() at the right time.
 * @return New thread, or NULL on allocation failure.
 * -------------------------------------------------------------------- */
struct thread *thread_alloc_bare(void)
{
    struct thread *t = kmalloc(sizeof(*t));
    if (!t)
        return NULL;
    void *stk = kmalloc(KSTACK_SIZE);
    if (!stk) {
        kfree(t);
        return NULL;
    }

    zero_words(t, sizeof(*t));
    t->tid         = g_next_tid++;
    t->pid         = t->tid;
    t->state       = THREAD_READY;
    t->kstack_base = stk;
    t->kstack_size = KSTACK_SIZE;
    t->parent      = get_current();
    INIT_LIST_HEAD(&t->link);
    INIT_LIST_HEAD(&t->children);
    INIT_LIST_HEAD(&t->sibling);
    return t;
}

/** ----------------------------------------------------------------------
 * @brief thread_enqueue_ready() – Mark @t READY and append to the runq.
 *
 * Bracketed by sie_save_clear/restore so a preempting timer IRQ cannot
 * observe a half-linked queue.
 * @param t Thread to enqueue.
 * -------------------------------------------------------------------- */
void thread_enqueue_ready(struct thread *t)
{
    unsigned long flags = sie_save_clear();
    t->state = THREAD_READY;
    list_add_tail(&t->link, &g_runq);
    sie_restore(flags);
}

/** ----------------------------------------------------------------------
 * @brief thread_block() – Park the current thread until woken.
 *
 * Sets state to BLOCKED and yields. schedule() only re-enqueues
 * threads whose state is still RUNNING, so a BLOCKED thread is left
 * out of the runq until thread_wakeup() inserts it back. Returns when
 * someone (typically sys_exit() of a child) wakes us via
 * thread_wakeup().
 * -------------------------------------------------------------------- */
void thread_block(void)
{
    unsigned long flags = sie_save_clear();
    get_current()->state = THREAD_BLOCKED;
    sie_restore(flags);
    schedule();
}

/** ----------------------------------------------------------------------
 * @brief thread_wakeup() – Re-enqueue @t if currently BLOCKED.
 *
 * Idempotent; a thread that is already READY/RUNNING is left alone,
 * and a ZOMBIE one is never resurrected.
 * @param t Thread to wake.
 * -------------------------------------------------------------------- */
void thread_wakeup(struct thread *t)
{
    unsigned long flags = sie_save_clear();
    if (t && t->state == THREAD_BLOCKED) {
        t->state = THREAD_READY;
        list_add_tail(&t->link, &g_runq);
    }
    sie_restore(flags);
}

/** ----------------------------------------------------------------------
 * @brief sched_zombify() – Move @t onto the zombie list for reaping.
 *
 * The idle thread's kill_zombies() pass will free both the kernel
 * stack and the thread struct. Caller MUST already have:
 *   - detached @t from runq / parent->children;
 *   - set @t->state = THREAD_ZOMBIE;
 *   - freed @t->image_base if any.
 * @param t Thread to schedule for reaping.
 * -------------------------------------------------------------------- */
void sched_zombify(struct thread *t)
{
    unsigned long flags = sie_save_clear();
    list_add_tail(&t->link, &g_zombies);
    sie_restore(flags);
}

/** ----------------------------------------------------------------------
 * @brief schedule() – Round-robin yield to the next runnable thread.
 *
 * If the caller is still RUNNING (i.e. yielded voluntarily rather than
 * exiting / blocking), demote it to READY and append to the run queue
 * tail so the other threads cycle through before it runs again.
 * BLOCKED and ZOMBIE callers are NOT re-enqueued; they will rejoin
 * the runq only via thread_wakeup() or never at all.
 *
 * If the queue is empty, the caller simply continues — this happens
 * when the idle thread is alone on the system.
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
         * RUNNING state we may have just cleared. BLOCKED prev would
         * deadlock here (no one to wake us), but the only legitimate
         * reason for an empty runq is "idle is the only thread", and
         * idle never blocks. */
        if (prev->state == THREAD_READY)
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
 * Used by kernel-mode threads (the trampoline's fall-through). User
 * processes go through sys_exit() instead, which also handles
 * children reparenting and parent wakeup. Both paths ultimately land
 * in sched_zombify() so kill_zombies() can reap the kernel stack.
 * -------------------------------------------------------------------- */
void thread_exit(void)
{
    unsigned long flags = sie_save_clear();
    struct thread *self = get_current();
    self->state = THREAD_ZOMBIE;
    /* Detach from any parent's children list — kernel workers are
     * leaf threads, so this is just hygiene. */
    if (self->parent)
        list_del(&self->sibling);
    sie_restore(flags);

    sched_zombify(self);
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
    zero_words(&g_bootstrap, sizeof(g_bootstrap));
    g_bootstrap.tid         = g_next_tid++;
    g_bootstrap.pid         = g_bootstrap.tid;
    g_bootstrap.state       = THREAD_RUNNING;
    g_bootstrap.entry       = NULL;
    g_bootstrap.kstack_base = NULL;
    g_bootstrap.kstack_size = 0;
    g_bootstrap.parent      = NULL;
    INIT_LIST_HEAD(&g_bootstrap.link);
    INIT_LIST_HEAD(&g_bootstrap.children);
    INIT_LIST_HEAD(&g_bootstrap.sibling);

    asm volatile ("mv tp, %0" :: "r"(&g_bootstrap));

    thread_create(idle_thread_body);
}

/* ---------- Ex2: user-process spawn ------------------------------------- */

/** ----------------------------------------------------------------------
 * @brief plant_initial_frame() – Build a trap_frame at the kstack top.
 *
 * Pre-populates the saved register/CSR slots that trap_return_user
 * will consume on the very first dispatch of a user thread:
 *   sepc    = entry point in U-mode
 *   sp      = user-mode stack pointer
 *   sstatus = SPIE only (SPP = 0 → return to U; SPIE = 1 → SIE on)
 * All GPRs are zeroed; the returned address is wired into ctx.sp by
 * the caller.
 * @param t        Target thread (uses kstack_base/kstack_size).
 * @param entry    First instruction in U-mode.
 * @param user_sp  16-byte aligned user stack pointer.
 * @return Pointer to the planted trap_frame on @t's kernel stack.
 * -------------------------------------------------------------------- */
static struct trap_frame *
plant_initial_frame(struct thread *t, uintptr_t entry, uintptr_t user_sp)
{
    uintptr_t top = (uintptr_t)t->kstack_base + t->kstack_size;
    top &= ~0xFUL;
    struct trap_frame *tf =
        (struct trap_frame *)(top - sizeof(struct trap_frame));

    zero_words(tf, sizeof(*tf));
    tf->sepc    = entry;
    tf->sp      = user_sp;
    tf->tp      = (uintptr_t)t;
    /*
     * Seed sstatus from the live CSR to preserve WARL fields (e.g.
     * UXL=2 on RV64). SPP=0 returns to U-mode; SPIE=1 re-enables
     * interrupts only after sret.
     *
     * SIE must be cleared: trap_return_user writes sstatus then
     * restores GPRs before sret. A pending SIE=1 would let an
     * interrupt fire mid-restore, clobbering sepc with a kernel PC
     * and causing a silent fault after sret. Real U->S traps avoid
     * this since hardware clears SIE on trap entry.
     */
    unsigned long ss;
    asm volatile ("csrr %0, sstatus" : "=r"(ss));
    ss &= ~(SSTATUS_SPP | SSTATUS_SIE);
    ss |=  SSTATUS_SPIE;
    tf->sstatus = ss;
    return tf;
}

/** ----------------------------------------------------------------------
 * @brief thread_spawn_user() – Boot a user process from initrd.
 *
 * Looks up @path in the cpio archive, allocates a contiguous user
 * image+stack buffer, copies the program in, then constructs a thread
 * whose first switch_to() lands at user_thread_bootstrap and sret's
 * into U-mode at the program's entry. The new thread is linked as a
 * child of the caller (typically the shell / g_bootstrap).
 * @param path Filename inside the initial ramdisk.
 * @return New thread on success, NULL on lookup / OOM failure.
 * -------------------------------------------------------------------- */
struct thread *thread_spawn_user(const char *path)
{
    const void *initrd = (const void *)
        dtb_getprop("/chosen", "linux,initrd-start");
    if (!initrd)
        return NULL;

    const void   *src;
    unsigned long sz;
    if (cpio_find(initrd, path, &src, &sz) != 0)
        return NULL;

    struct thread *t = thread_alloc_bare();
    if (!t)
        return NULL;

    t->image_size = sz;
    t->total_size = sz + USER_STACK_SIZE;
    t->image_base = kmalloc(t->total_size);
    if (!t->image_base) {
        kfree(t->kstack_base);
        kfree(t);
        return NULL;
    }
    mem_cpy(t->image_base, src, sz);

    asm volatile ("fence.i" ::: "memory");

    uintptr_t entry   = (uintptr_t)t->image_base;
    uintptr_t user_sp = ((uintptr_t)t->image_base + t->total_size)
                        & ~0xFUL;
    struct trap_frame *tf = plant_initial_frame(t, entry, user_sp);

    t->ctx.sp = (unsigned long)tf;
    t->ctx.ra = (unsigned long)user_thread_bootstrap;

    trap_set_user_base(entry);

    unsigned long flags = sie_save_clear();
    if (t->parent)
        list_add_tail(&t->sibling, &t->parent->children);
    sie_restore(flags);

    thread_enqueue_ready(t);
    return t;
}

/* ---------- Ex2: pid lookup -------------------------------------------- */

/** ----------------------------------------------------------------------
 * @brief dfs_find_pid() – Recursive search of the process tree.
 *
 * Walks @root and its descendants looking for a thread whose pid
 * matches. Used by find_thread_by_pid(); kept private because the
 * recursion is bounded by the process tree depth (lab scope: tiny).
 * @param root Subtree root to start from.
 * @param pid  Process id to look for.
 * @return Matching thread or NULL.
 * -------------------------------------------------------------------- */
static struct thread *dfs_find_pid(struct thread *root, int pid)
{
    if (!root)
        return NULL;
    if (root->pid == pid)
        return root;

    struct list_head *it;
    list_for_each(it, &root->children) {
        struct thread *c = list_entry(it, struct thread, sibling);
        struct thread *r = dfs_find_pid(c, pid);
        if (r)
            return r;
    }
    return NULL;
}

/** ----------------------------------------------------------------------
 * @brief find_thread_by_pid() – Locate a thread by pid.
 *
 * Walks the process tree rooted at g_bootstrap. Runs under SIE-clear
 * because sys_exit / fork mutate the children lists concurrently.
 * @param pid Process id to find.
 * @return Matching thread, or NULL if no such pid is alive.
 * -------------------------------------------------------------------- */
struct thread *find_thread_by_pid(int pid)
{
    unsigned long flags = sie_save_clear();
    struct thread *t = dfs_find_pid(&g_bootstrap, pid);
    sie_restore(flags);
    return t;
}

/* ---------- Ex2: preemption flag --------------------------------------- */

void set_need_resched(void)
{
    g_need_resched = 1;
}

int need_resched_clear(void)
{
    int v = g_need_resched;
    g_need_resched = 0;
    return v;
}
