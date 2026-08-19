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
#include "signal.h"
#include "mm.h"
#include "buddy.h"

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
 * @param[out] p     Region base.
 * @param      bytes Number of bytes to clear (multiple of word size).
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
    /* Inherit the creator's working directory; the fd table stays empty
     * (zeroed above) because only fork() duplicates open files. */
    t->cwd         = t->parent ? t->parent->cwd : NULL;
    INIT_LIST_HEAD(&t->link);
    INIT_LIST_HEAD(&t->children);
    INIT_LIST_HEAD(&t->sibling);
    INIT_LIST_HEAD(&t->vma_list);
    t->mmap_top = MMAP_CURSOR_INIT; /* reset per-image in uvm_setup_image() */
    signal_state_init(&t->sig);
    return t;
}

/** ----------------------------------------------------------------------
 * @brief thread_enqueue_ready() – Mark @t READY and append to the runq.
 *
 * Bracketed by sie_save_clear/restore so a preempting timer IRQ cannot
 * observe a half-linked queue.
 * @param[in] t Thread to enqueue.
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
 * @param[in] t Thread to wake.
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
 * stack and the thread struct (plus the user VM via
 * thread_free_user_vm()). Caller MUST already have:
 *   - detached @t from runq / parent->children;
 *   - set @t->state = THREAD_ZOMBIE.
 * @param[in] t Thread to schedule for reaping.
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

    if (prev != next) {
        /*
         * Install next's address space before switching kernel stacks.
         * A user process uses its private PGD; a kernel-only thread uses
         * the kernel PGD. The kernel high half is identical across all
         * PGDs, so the currently executing kernel code/stack stay valid
         * across the satp write. Skipping the write when the target PGD
         * already matches avoids a needless sfence.vma.
         */
        unsigned long *next_pgd = next->pgd ? next->pgd : kernel_pgd();
        unsigned long *prev_pgd = prev->pgd ? prev->pgd : kernel_pgd();
        if (next_pgd != prev_pgd)
            mm_set_satp(next_pgd);

        switch_to(prev, next);
    }
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
 * @brief thread_free_user_vm() – Release a process's user VM.
 *
 * Frees the VMA metadata nodes via vma_unmap_all(), then pgd_free()
 * reclaims every mapped user frame together with the page tables and
 * the PGD (single-owner model: the page table owns the frames).
 * sigpage_base is cleared afterward: it was only a cached alias of a
 * frame pgd_free() just released, so leaving it set would dangle.
 * Idempotent: a second call (or a never-spawned kernel thread with an
 * empty vma_list) is a no-op.
 *
 * MUST be called only from a context where satp no longer points at
 * @t->pgd — i.e. the reap point (kill_zombies), or sys_exec() AFTER it
 * has switched satp to the new PGD.
 * @param[in] t Thread whose address space is to be reclaimed.
 * -------------------------------------------------------------------- */
void thread_free_user_vm(struct thread *t)
{
    vma_unmap_all(t);
    t->sigpage_base = NULL;

    if (t->pgd) {
        pgd_free(t->pgd);
        t->pgd = NULL;
    }
}

/** ----------------------------------------------------------------------
 * @brief kill_zombies() – Reap exited threads from the idle context.
 *
 * Walks g_zombies, detaches each node under SIE=0 (so a future IRQ
 * cannot race the list mutation), then frees the user address space,
 * the kernel stack and the thread struct outside the critical section.
 * The user VM is freed HERE (not at sys_exit) so satp has long since
 * switched away from the dying PGD. The bootstrap thread uses
 * kstack_base = NULL as a sentinel to opt out of stack freeing (it sits
 * on the boot stack carved out by the linker, not on a kmalloc'd
 * region).
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

        thread_free_user_vm(z);
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
    INIT_LIST_HEAD(&g_bootstrap.vma_list);

    asm volatile ("mv tp, %0" :: "r"(&g_bootstrap));

    thread_create(idle_thread_body);
}

/* ---------- User-process spawn ------------------------------------------ */

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
 * @param[in] t       Target thread (uses kstack_base/kstack_size).
 * @param     entry   First instruction in U-mode.
 * @param     user_sp 16-byte aligned user stack pointer.
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
 * @brief round_up_page() – Round @n up to a PAGE_SIZE multiple.
 * @param n Byte count.
 * @return n rounded up to the next page boundary (n==0 -> 0).
 * -------------------------------------------------------------------- */
static inline unsigned long round_up_page(unsigned long n)
{
    return (n + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

/** ----------------------------------------------------------------------
 * @brief vma_register_fixed() – Record image/stack/sigpage VMA metadata.
 *
 * Allocates all three metadata nodes up front (the only failure point),
 * then links them into t->vma_list with is_mmap=0. The image VMA is
 * file-backed by the initrd bytes (@file_src/@file_len) so the fault
 * handler can page its contents in on demand; stack and sigpage are
 * anonymous. All-or-nothing: if any vma_alloc() fails, the already-
 * allocated nodes are kfree'd and the list is left untouched.
 * @param[in] t         Thread to register into (vma_list already INIT'd).
 * @param[in] file_src  Program bytes inside the initrd (kernel VA).
 * @param     file_len  Program length in bytes.
 * @param     img_bytes Image region length (page-rounded @file_len).
 * @return 0 on success, -1 on OOM (nothing linked).
 * -------------------------------------------------------------------- */
static int vma_register_fixed(struct thread *t, const void *file_src,
                              unsigned long file_len,
                              unsigned long img_bytes)
{
    struct vma *vi = vma_alloc(USER_CODE_VA, img_bytes,
                               PROT_USER_RWX, file_src, file_len, 0);
    struct vma *vs = vma_alloc(USER_STACK_TOP - USER_STACK_SIZE,
                               USER_STACK_SIZE, PROT_USER_DATA,
                               NULL, 0, 0);
    struct vma *vg = vma_alloc(SIGPAGE_VA, PAGE_SIZE,
                               PROT_USER_RWX, NULL, 0, 0);
    if (!vi || !vs || !vg) {
        if (vi)
            kfree(vi);
        if (vs)
            kfree(vs);
        if (vg)
            kfree(vg);
        return -1;
    }
    vma_insert_sorted(t, vi);
    vma_insert_sorted(t, vs);
    vma_insert_sorted(t, vg);
    return 0;
}

/** ----------------------------------------------------------------------
 * @brief uvm_setup_image() – Build a demand-paged user address space.
 *
 * Registers metadata-only VMAs for the image (file-backed by the initrd
 * bytes at @src — the initrd sits in reserved memory, so the reference
 * stays valid for the whole process lifetime), the stack and the signal
 * page. No image or stack frame is allocated: the first fetch/push
 * faults and do_page_fault() populates page by page.
 *
 * The signal page is the ONE eager region: signal dispatch writes the
 * sigreturn trampoline through t->sigpage_base (kernel VA), so its
 * frame must exist before any dispatch. Its ownership still lies with
 * the page table; sigpage_base is only a cached alias.
 * @param[in] t   Thread with a valid @t->pgd (from pgd_alloc()).
 * @param[in] src Program bytes inside the initrd (kernel VA).
 * @param     sz  Program length in bytes.
 * @return 0 on success, -1 on OOM.
 * -------------------------------------------------------------------- */
int uvm_setup_image(struct thread *t, const void *src, unsigned long sz)
{
    unsigned long img_bytes = round_up_page(sz);
    if (img_bytes == 0)                 /* an empty image is meaningless */
        return -1;

    /* Dedicated U-mode signal page: the handler stack + sigreturn
     * trampoline must live at a PTE_U user VA (a kernel VA would fault in
     * U-mode under Sv39). One page suffices because in_handler forbids
     * nested dispatch. Zeroed so the handler stack never leaks a previous
     * owner's data. */
    void *sig = buddy_alloc(PAGE_SIZE);
    if (!sig)
        return -1;
    zero_words(sig, PAGE_SIZE);

    if (map_pages(t->pgd, SIGPAGE_VA, PAGE_SIZE,
                  virt_to_phys(sig), PROT_USER_RWX) != 0) {
        /* map_pages() failed on an intermediate table, so the sig leaf
         * was never installed and the frame is still owned here. */
        uvm_destroy(t->pgd);
        buddy_free(sig);
        return -1;
    }

    if (vma_register_fixed(t, src, sz, img_bytes) != 0) {
        /* The sig leaf IS installed now, so uvm_destroy() frees the
         * frame along with the tables (single-owner model). */
        uvm_destroy(t->pgd);
        return -1;
    }

    t->sigpage_base = sig;

    /* mmap self-selection grows down from one guard page below the signal
     * page, so anonymous regions never collide with image (low) / stack
     * (high) and the topmost region does not abut the signal page. */
    t->mmap_top = MMAP_CURSOR_INIT;
    return 0;
}

/** ----------------------------------------------------------------------
 * @brief thread_spawn_user() – Boot a user process from initrd.
 *
 * Looks up @path in the cpio archive, builds a private Sv39 address
 * space (pgd_alloc + uvm_setup_image) with the image at USER_CODE_VA and
 * the stack below USER_STACK_TOP, then constructs a thread whose first
 * switch_to() lands at user_thread_bootstrap and sret's into U-mode at
 * VA 0. The new thread is linked as a child of the caller.
 * @param[in] path Filename inside the initial ramdisk.
 * @return New thread on success, NULL on lookup / OOM failure.
 * -------------------------------------------------------------------- */
struct thread *thread_spawn_user(const char *path)
{
    /* initrd-start is a PA; under paging deref it through its VA. */
    uintptr_t initrd_pa = dtb_getprop("/chosen", "linux,initrd-start");
    const void *initrd = initrd_pa ? phys_to_virt(initrd_pa) : 0;
    if (!initrd)
        return NULL;

    const void   *src;
    unsigned long sz;
    if (cpio_find(initrd, path, &src, &sz) != 0)
        return NULL;

    struct thread *t = thread_alloc_bare();
    if (!t)
        return NULL;

    t->pgd = pgd_alloc();
    if (!t->pgd)
        goto fail_thread;

    if (uvm_setup_image(t, src, sz) != 0)
        goto fail_pgd;

    struct trap_frame *tf =
        plant_initial_frame(t, USER_CODE_VA, USER_STACK_TOP & ~0xFUL);

    t->ctx.sp = (unsigned long)tf;
    t->ctx.ra = (unsigned long)user_thread_bootstrap;

    unsigned long flags = sie_save_clear();
    if (t->parent)
        list_add_tail(&t->sibling, &t->parent->children);
    sie_restore(flags);

    thread_enqueue_ready(t);
    return t;

fail_pgd:
    pgd_free(t->pgd);
    t->pgd = NULL;
fail_thread:
    kfree(t->kstack_base);
    kfree(t);
    return NULL;
}

/* ---------- Pid lookup -------------------------------------------------- */

/** ----------------------------------------------------------------------
 * @brief dfs_find_pid() – Recursive search of the process tree.
 *
 * Walks @root and its descendants looking for a thread whose pid
 * matches. Used by find_thread_by_pid(); kept private because the
 * recursion is bounded by the process tree depth (lab scope: tiny).
 * @param[in] root Subtree root to start from.
 * @param     pid  Process id to look for.
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

/* ---------- Preemption flag --------------------------------------------- */

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
