#include "syscall.h"
#include "sched.h"
#include "trap.h"
#include "uart.h"
#include "cpio.h"
#include "dtb.h"
#include "kmalloc.h"
#include "list.h"
#include "buddy.h"
#include "riscv.h"
#include "utils.h"
#include "types.h"
#include "video.h"
#include "timer.h"
#include "signal.h"
#include "mm.h"
#include "vfs.h"
#include "errno.h"

/*
 * System-call layer.
 *
 * Conventions:
 *   - All handlers receive their arguments unpacked from tf->a0..a2.
 *   - The return value is delivered via the long return type and
 *     written into tf->a0 by trap.c after do_syscall() returns.
 *   - sys_exit() is __noreturn — it never comes back, so its slot in
 *     do_syscall() does not return a value.
 */

/** ----------------------------------------------------------------------
 * @brief in_user_range() – User-pointer validation against the VMA list.
 *
 * A valid user buffer must lie wholly inside one mapped region. Since
 * every region (image, stack, signal page, each mmap result) is a VMA on
 * cur->vma_list, the check is a single scan for a VMA whose [va, va+len)
 * contains [a, e). This rejects stray kernel/MMIO pointers before the
 * kernel dereferences them. A pointer inside a VMA may still be un-
 * populated under demand paging; the S-mode access then page-faults and
 * do_page_fault() pages it in transparently. Treats overflow as failure.
 *
 * Note: this validates mapping presence only, not prot — a PROT_NONE mmap
 * region passes here but a real access still faults in U-mode. Per-prot
 * checking is a deliberate non-goal.
 * @param[in] p Start of the user buffer.
 * @param     n Size in bytes.
 * @return 1 if the range is wholly inside a mapped user region, else 0.
 * -------------------------------------------------------------------- */
static int in_user_range(const void *p, unsigned long n)
{
    struct thread *cur = get_current();
    uintptr_t a = (uintptr_t)p;
    uintptr_t e = a + n;

    if (e < a)                          /* overflow */
        return 0;
    if (!cur->pgd)                      /* not a user process */
        return 0;

    struct list_head *it;
    list_for_each(it, &cur->vma_list) {
        struct vma *v = list_entry(it, struct vma, link);
        if (a >= v->va && e <= v->va + v->len)
            return 1;
    }
    return 0;
}

/** ----------------------------------------------------------------------
 * @brief copy_path_from_user() – Copy a NUL-terminated user path in.
 *
 * A path arrives as a bare pointer with no length, so its extent cannot
 * be validated up front the way a sized buffer can. Each byte is
 * therefore range-checked immediately before it is read, and the copy
 * stops at the buffer limit: the kernel never dereferences past what
 * the caller actually has mapped, and an unterminated string cannot
 * drive an unbounded scan.
 * @param[in]  upath User pointer to the path.
 * @param[out] kbuf  Kernel buffer receiving the NUL-terminated copy.
 * @param      sz    Size of @kbuf in bytes, NUL included.
 * @return 0 on success, -EFAULT when @upath leaves mapped memory,
 *         -ENAMETOOLONG when the path does not fit @kbuf.
 * -------------------------------------------------------------------- */
static int copy_path_from_user(const char *upath, char *kbuf, size_t sz)
{
    if (!upath || !kbuf || sz == 0)
        return -EFAULT;

    for (size_t i = 0; i < sz; i++) {
        if (!in_user_range(upath + i, 1))
            return -EFAULT;
        kbuf[i] = upath[i];
        if (kbuf[i] == '\0')
            return 0;
    }
    return -ENAMETOOLONG;
}

/** ----------------------------------------------------------------------
 * @brief fd_alloc() – Bind @file to the lowest free descriptor of @t.
 *
 * Scanning from 0 hands back the smallest available number, which is
 * both the POSIX rule and what makes a descriptor reusable as soon as
 * it is closed.
 * @param[in,out] t    Task whose table gains the entry.
 * @param[in]     file Open file description to install.
 * @return The new descriptor, or -EMFILE when the table is full.
 * -------------------------------------------------------------------- */
static int fd_alloc(struct thread *t, struct file *file)
{
    for (int fd = 0; fd < VFS_MAX_FD; fd++) {
        if (!t->fd_table[fd]) {
            t->fd_table[fd] = file;
            return fd;
        }
    }
    return -EMFILE;
}

/** ----------------------------------------------------------------------
 * @brief fd_get() – Translate a user-supplied descriptor to its handle.
 *
 * The only place a descriptor from user space is trusted, so every
 * syscall taking an fd goes through here: an out-of-range number and a
 * number naming a closed slot both fail rather than indexing the table.
 * @param[in] t  Task owning the descriptor.
 * @param     fd Descriptor as passed by user space.
 * @return The open file description, or NULL when @fd names none.
 * -------------------------------------------------------------------- */
static struct file *fd_get(struct thread *t, int fd)
{
    if (fd < 0 || fd >= VFS_MAX_FD)
        return NULL;
    return t->fd_table[fd];
}

/** ----------------------------------------------------------------------
 * @brief fd_table_close_all() – Release every descriptor held by @t.
 *
 * Called on process exit. Each slot is cleared before vfs_close() drops
 * its reference, so a handle shared with a forked relative is released
 * exactly once from this side.
 * @param[in,out] t Task being torn down.
 * -------------------------------------------------------------------- */
static void fd_table_close_all(struct thread *t)
{
    for (int fd = 0; fd < VFS_MAX_FD; fd++) {
        struct file *f = t->fd_table[fd];
        if (!f)
            continue;
        t->fd_table[fd] = NULL;
        vfs_close(f);
    }
}

/** ----------------------------------------------------------------------
 * @brief sys_getpid() – Return the current process id.
 * @return Current thread's pid (== tid).
 * -------------------------------------------------------------------- */
static long sys_getpid(void)
{
    return (long)get_current()->pid;
}

/** ----------------------------------------------------------------------
 * @brief sys_uart_read() – Block until ≥1 byte then drain best-effort.
 *
 * Spec semantics: blocking read. We yield the CPU via schedule() while
 * the UART ring is empty, return as soon as the first byte arrives,
 * and opportunistically drain up to @count more bytes if they are
 * already buffered. Validates @buf against the current image.
 * @param[out] buf   User destination.
 * @param      count Maximum bytes to read.
 * @return Bytes actually written into @buf, or -1 on bad arguments.
 * -------------------------------------------------------------------- */
static long sys_uart_read(char *buf, long count)
{
    if (count <= 0 || !in_user_range(buf, (unsigned long)count))
        return -1;

    long i = 0;
    while (i < count) {
        int c = uart_getc();
        if (c < 0) {
            if (i > 0)
                break;          /* non-blocking after the first byte */
            schedule();         /* yield while waiting */
            continue;
        }
        buf[i++] = (char)c;
    }
    return i;
}

/** ----------------------------------------------------------------------
 * @brief sys_uart_write() – Push @count bytes from @buf to the UART.
 *
 * Uses the existing uart_putc() path which routes through the IRQ-
 * driven TX ring (or the polling fallback before PLIC is online).
 * Validates @buf against the current image.
 * @param[in] buf   User source bytes.
 * @param     count Number of bytes to write.
 * @return @count on success, or -1 on bad arguments.
 * -------------------------------------------------------------------- */
static long sys_uart_write(const char *buf, long count)
{
    if (count < 0 || !in_user_range(buf, (unsigned long)count))
        return -1;
    for (long i = 0; i < count; i++)
        uart_putc((unsigned char)buf[i]);
    return count;
}

/** ----------------------------------------------------------------------
 * @brief sys_exec() – Replace the current image with @path from initrd.
 *
 * Looks the file up via cpio, builds a fresh demand-paged address space
 * (metadata VMAs + eager sigpage only), and rewrites the in-place
 * trap_frame so the impending sret jumps into the new program at offset
 * 0 with a clean register file — the first instruction fetch then
 * demand-faults the first image page in. Does not create a new thread;
 * the caller's pid stays the same.
 * @param[in]     path Filename inside the initial ramdisk.
 * @param[in,out] tf   Trap frame on the kernel stack (will be rewritten).
 * @return 0 on success, -1 on lookup / OOM failure.
 * -------------------------------------------------------------------- */
static long sys_exec(const char *path, struct trap_frame *tf)
{
    struct thread *self = get_current();
    if (!self->pgd)
        return -1;

    /* initrd-start is a PA; under paging deref it through its VA. */
    uintptr_t initrd_pa = dtb_getprop("/chosen", "linux,initrd-start");
    const void *initrd = initrd_pa ? phys_to_virt(initrd_pa) : 0;
    const void   *src;
    unsigned long sz;
    if (!initrd || cpio_find(initrd, path, &src, &sz) != 0)
        return -1;

    /*
     * Build a brand-new address space rather than mutating the live one.
     * This keeps the running satp/PGD valid until the switch below, and
     * lets us bail out cleanly on OOM.
     */
    unsigned long *new_pgd = pgd_alloc();
    if (!new_pgd)
        return -1;

    /* Detach the old VMA list aside so uvm_setup_image() can build the
     * new image's VMAs on a fresh, empty list. The old frames stay owned
     * by old_pgd's page tables. */
    unsigned long *old_pgd = self->pgd;
    void *old_sig = self->sigpage_base;
    struct list_head old_vmas;
    INIT_LIST_HEAD(&old_vmas);
    vma_detach_all(self, &old_vmas);

    self->pgd = new_pgd;
    self->sigpage_base = NULL;
    if (uvm_setup_image(self, src, sz) != 0) {
        /* uvm_setup_image rolls back all-or-nothing: on failure it leaves
         * self->vma_list empty. Restore the old bookkeeping and move the
         * detached old VMA list back onto self. */
        self->pgd = old_pgd;
        self->sigpage_base = old_sig;
        vma_reattach(self, &old_vmas);
        pgd_free(new_pgd);
        return -1;
    }

    /* Switch to the new address space, then reclaim the old one: node
     * metadata via the detached list, frames + tables via pgd_free()
     * (satp no longer points at old_pgd). */
    mm_set_satp(new_pgd);
    vma_free_list(&old_vmas);
    pgd_free(old_pgd);

    /* POSIX: exec resets handlers to default and drops pending. The
     * in_handler gate is cleared defensively in case a previous handler
     * had exec()'d (cannot legitimately happen via sigreturn, but cheap). */
    for (int i = 0; i < NSIG; i++)
        self->sig.handlers[i] = SIG_DFL;
    self->sig.pending = 0;
    signal_release(self);

    /* Rewrite the trap_frame so trap_return_user lands the new image at
     * the fixed user VAs. */
    tf->sepc = USER_CODE_VA;
    tf->sp   = USER_STACK_TOP & ~0xFUL;
    tf->tp = (uintptr_t)self;
    tf->ra = tf->gp = 0;
    tf->t0 = tf->t1 = tf->t2 = 0;
    tf->t3 = tf->t4 = tf->t5 = tf->t6 = 0;
    tf->a0 = tf->a1 = tf->a2 = tf->a3 = 0;
    tf->a4 = tf->a5 = tf->a6 = tf->a7 = 0;
    tf->s0 = tf->s1 = tf->s2 = tf->s3 = 0;
    tf->s4 = tf->s5 = tf->s6 = tf->s7 = 0;
    tf->s8 = tf->s9 = tf->s10 = tf->s11 = 0;

    return 0;
}

/** ----------------------------------------------------------------------
 * @brief sys_fork() – Duplicate the calling process.
 *
 * Allocates a new thread and clones the parent's address space with
 * uvm_clone_vma(): every VMA's metadata is inherited (including the
 * image's initrd file backing) and the pages the parent actually
 * populated are shared copy-on-write — both sides' PTEs reference the
 * same frame, downgraded to read-only, and the first store from either
 * side breaks the share into a private copy. Only the sigpage is
 * eagerly copied (the kernel writes it through a linear-map alias that
 * bypasses PTE protection). Untouched pages demand-fault in the child
 * later, so fork cost scales with page-table size, not the image. A
 * trap_frame mirroring the parent's (except a0 = 0) is planted on the
 * child's kernel stack; the first switch_to() enters
 * user_thread_bootstrap, jumps to trap_return_user, and sret's into U.
 *
 * sepc was already advanced past the ecall by trap.c BEFORE
 * do_syscall() ran, so the child resumes at the instruction after
 * the fork-call ecall, just like the parent will.
 * @param[in] tf Parent's trap frame.
 * @return Child pid in the parent path, 0 in the child path, -1 on
 *         allocation failure.
 * -------------------------------------------------------------------- */
static long sys_fork(struct trap_frame *tf)
{
    extern void user_thread_bootstrap(void);

    struct thread *par = get_current();
    if (!par->pgd)
        return -1;

    struct thread *ch = thread_alloc_bare();
    if (!ch)
        return -1;

    /* 1) Child address space: private PGD (kernel high half shared). */
    ch->pgd = pgd_alloc();
    if (!ch->pgd)
        goto fail_thread;

    /* 2) Clone VMA metadata + copy only the parent's populated pages. */
    if (uvm_clone_vma(ch, par) != 0)
        goto fail_vm;

    /* 2b) The parent's sigpage is eager, hence present, hence copied by
     *     the clone above; re-derive the child's kernel-VA alias from its
     *     own page table. */
    unsigned long *spte = pt_lookup(ch->pgd, SIGPAGE_VA);
    if (!spte || !(*spte & PTE_V))
        goto fail_vm;
    ch->sigpage_base = phys_to_virt(PTE_TO_PA(*spte));

    /* Inherit the parent's placement cursor so the child's future mmaps
     * land below the regions it inherited, not on top. */
    ch->mmap_top = par->mmap_top;

    /* 3) Plant child's trap_frame at the top of its kstack. The child
     *    keeps the parent's user sp VERBATIM: parent and child share an
     *    identical user VA layout, so that VA points into the child's own
     *    stack frames within its own address space — no translation. */
    uintptr_t top = (uintptr_t)ch->kstack_base + ch->kstack_size;
    top &= ~0xFUL;
    struct trap_frame *cf =
        (struct trap_frame *)(top - sizeof(struct trap_frame));
    /* Use mem_cpy() rather than struct assignment so the compiler does
     * not lower it into a libc memcpy() call (we are -nostdlib). */
    mem_cpy(cf, tf, sizeof(*cf));
    cf->a0 = 0;                     /* fork returns 0 in child */
    cf->tp = (uintptr_t)ch;

    /* 4) Wire ctx for first switch_to() to land at user_thread_bootstrap. */
    ch->ctx.sp = (unsigned long)cf;
    ch->ctx.ra = (unsigned long)user_thread_bootstrap;

    /* 5) Inherit signal handler table from parent; pending/in_handler
     *    do NOT cross the fork boundary (POSIX: child starts with an
     *    empty pending set, not running any handler). The child's signal
     *    page is its own (cloned above), independent of the parent's. */
    mem_cpy(&ch->sig.handlers, &par->sig.handlers,
            sizeof(par->sig.handlers));
    ch->sig.pending       = 0;
    ch->sig.in_handler    = 0;

    /* 5b) Inherit the file system context. The cwd is a shared vnode
     *     (both tasks may sit in the same directory; each may chdir away
     *     independently). Descriptors are inherited as POSIX specifies:
     *     the child gets the same open file descriptions, NOT copies, so
     *     the two sides share one f_pos per descriptor. Each inherited
     *     slot takes its own reference, which is what lets both tasks
     *     close independently without the first release freeing a handle
     *     the other still holds.
     *
     *     thread_alloc_bare() already copied the creator's cwd, but the
     *     creator is the forking parent only because fork runs in its
     *     context; assigning explicitly keeps that from being load
     *     bearing. */
    ch->cwd = par->cwd;
    for (int fd = 0; fd < VFS_MAX_FD; fd++) {
        struct file *f = par->fd_table[fd];
        if (!f)
            continue;
        vfs_file_get(f);
        ch->fd_table[fd] = f;
    }

    /* 6) Parent linkage + enqueue. */
    ch->parent = par;
    unsigned long flags = sie_save_clear();
    list_add_tail(&ch->sibling, &par->children);
    sie_restore(flags);

    thread_enqueue_ready(ch);
    return (long)ch->pid;           /* parent path */

fail_vm:
    /* Single-owner rollback: nodes via the VMA list, frames + tables via
     * pgd_free() — no per-block goto chain. */
    vma_free_list(&ch->vma_list);
    pgd_free(ch->pgd);
    ch->pgd = NULL;
fail_thread:
    kfree(ch->kstack_base);
    kfree(ch);
    return -1;
}

/** ----------------------------------------------------------------------
 * @brief sys_waitpid() – Block until child @pid exits, return its status.
 *
 * Locates the child in self->children (errors -1 if no such child),
 * blocks via thread_block() until the child becomes ZOMBIE, then
 * detaches and hands its kstack/struct off to the zombie reaper, which
 * also reclaims the child's user address space (frames + page tables).
 * @param pid Process id of an existing child.
 * @return Child's exit_status, or -1 if @pid is not a child.
 * -------------------------------------------------------------------- */
static long sys_waitpid(long pid)
{
    struct thread *self = get_current();
    struct thread *child = NULL;
    struct list_head *it;

    unsigned long flags = sie_save_clear();
    list_for_each(it, &self->children) {
        struct thread *c = list_entry(it, struct thread, sibling);
        if (c->pid == (int)pid) {
            child = c;
            break;
        }
    }
    sie_restore(flags);
    if (!child)
        return -1;

    /* Block until the child has fully exited. sys_exit() wakes us
     * directly via parent pointer, so a single re-check after
     * thread_block() returns is enough — but we loop defensively in
     * case of spurious wakeups (e.g. a future signal mechanism). */
    while (child->state != THREAD_ZOMBIE)
        thread_block();

    int st = child->exit_status;

    flags = sie_save_clear();
    list_del(&child->sibling);
    sie_restore(flags);
    sched_zombify(child);

    return (long)st;
}

/** ----------------------------------------------------------------------
 * @brief do_exit() – Terminate the current process (never returns).
 *
 * Reparents any surviving children onto g_bootstrap (so a future
 * waitpid by the shell can reap them), marks self ZOMBIE, wakes the
 * parent if it was blocked in waitpid, and yields. The kernel stack is
 * reaped later by sys_waitpid()'s sched_zombify() call (or by the
 * stop() path). Exported (see syscall.h) because the page-fault
 * handler's segfault kill needs the exact same teardown as a voluntary
 * exit — satp still points at self->pgd in both cases, so VM reclaim
 * must equally wait for the reaper.
 * @param status Exit status returned to the parent's waitpid.
 * -------------------------------------------------------------------- */
void do_exit(long status)
{
    struct thread *self = get_current();

    /*
     * Release the open files before the thread stops being scheduled.
     * This runs with interrupts on and in the dying task's own context,
     * which the reaper cannot offer: it frees the kstack from the idle
     * thread, where get_current() is no longer this task.
     */
    fd_table_close_all(self);

    unsigned long flags = sie_save_clear();
    self->exit_status = (int)status;

    while (!list_empty(&self->children)) {
        struct thread *c = list_entry(self->children.next,
                                      struct thread, sibling);
        list_del(&c->sibling);
        c->parent = &g_bootstrap;
        list_add_tail(&c->sibling, &g_bootstrap.children);
    }

    self->state = THREAD_ZOMBIE;
    struct thread *par = self->parent;
    sie_restore(flags);

    /*
     * Do NOT free the user address space here: satp still points at
     * self->pgd. The reaper (kill_zombies) frees the user frames, the
     * page tables and the PGD after schedule() has switched satp away.
     * signal_release() only clears the in_handler gate so a thread
     * dying mid-handler leaves a clean signal state.
     */
    signal_release(self);

    if (par)
        thread_wakeup(par);

    schedule();
    for (;;)
        ;
}

/** ----------------------------------------------------------------------
 * @brief sys_exit() – SYS_EXIT entry point; thin do_exit() wrapper.
 * @param status Exit status returned to the parent's waitpid.
 * -------------------------------------------------------------------- */
static void sys_exit(long status)
{
    do_exit(status);
}

/** ----------------------------------------------------------------------
 * @brief sys_stop() – Forcibly terminate process @pid.
 *
 * If @pid is the caller, behaves like exit(-1). Otherwise locates the
 * thread via find_thread_by_pid(): if READY, removes from the runq;
 * marks ZOMBIE; frees the user image; wakes the parent if blocked.
 * The victim is left attached to its parent's children list so a
 * future waitpid can collect its status (= -1).
 * @param pid Process id to stop.
 * @return 0 on success, -1 if @pid is not found or not a user proc.
 * -------------------------------------------------------------------- */
static long sys_stop(long pid)
{
    if ((int)pid == get_current()->pid) {
        sys_exit(-1);                 /* noreturn */
    }

    struct thread *t = find_thread_by_pid((int)pid);
    if (!t || !t->pgd)
        return -1;

    unsigned long flags = sie_save_clear();
    t->exit_status = -1;
    if (t->state == THREAD_READY)
        list_del(&t->link);
    t->state = THREAD_ZOMBIE;
    struct thread *par = t->parent;
    sie_restore(flags);

    /* The victim's user VM is reclaimed by the reaper; freeing it here
     * is unsafe (it may still be the live satp of a preempted run). Its
     * open files are not part of the address space, so they are released
     * here — the victim never runs again and so never reaches do_exit()
     * to release them itself. */
    fd_table_close_all(t);
    signal_release(t);
    if (par)
        thread_wakeup(par);
    return 0;
}

/** ----------------------------------------------------------------------
 * @brief sys_display() – Center-blit a user BMP buffer to framebuffer.
 *
 * Validates the @bmp pointer and dimensions: width/height must fit
 * inside the physical framebuffer so the centered placement does not
 * walk off the FB, and [bmp, bmp + w*h*4) must lie inside the caller's
 * image buffer. Computes the byte length as (uint64_t)w*h*4 to avoid a
 * 32-bit overflow when w*h is close to the FB size.
 * @param[in] bmp User-space pixel array (XRGB8888).
 * @param     w   Image width in pixels.
 * @param     h   Image height in pixels.
 * @return 0 on success, -1 on argument validation failure.
 * -------------------------------------------------------------------- */
static long sys_display(unsigned int *bmp, unsigned int w, unsigned int h)
{
    if (w == 0 || h == 0 || w > FB_WIDTH || h > FB_HEIGHT)
        return -1;
    uint64_t bytes = (uint64_t)w * (uint64_t)h * 4ULL;
    if (!in_user_range(bmp, (unsigned long)bytes))
        return -1;
    video_bmp_display(bmp, (int)w, (int)h);
    return 0;
}

/* Wakeup token planted on the sleeping thread's kernel stack. The
 * timer callback runs from the bottom-half task queue and only needs
 * the thread pointer to wake the sleeper. */
struct usleep_token {
    struct thread *t;
};

/** ----------------------------------------------------------------------
 * @brief usleep_cb() – Timer expiry callback for sys_usleep().
 *
 * Wakes the sleeping thread that registered the timer. Safe to run
 * from the bottom-half task queue because thread_wakeup() is itself
 * IRQ-safe (sie_save_clear inside).
 * @param[in] arg Pointer to the sleeper's stack-allocated usleep_token.
 * -------------------------------------------------------------------- */
static void usleep_cb(void *arg)
{
    struct usleep_token *s = (struct usleep_token *)arg;
    thread_wakeup(s->t);
}

/** ----------------------------------------------------------------------
 * @brief sys_usleep() – Block the caller for @usec microseconds.
 *
 * Implements userspace usleep() by registering a microsecond-resolution
 * timer that targets the current thread, then thread_block()ing. The
 * token lives on the caller's kernel stack: the kstack is not freed
 * while the thread is BLOCKED, so it stays valid until the callback
 * runs and we resume past thread_block().
 * @param usec Microseconds to sleep. 0 returns immediately.
 * @return 0 on success.
 * -------------------------------------------------------------------- */
static long sys_usleep(unsigned int usec)
{
    if (usec == 0)
        return 0;
    struct usleep_token tok = { .t = get_current() };
    add_timer_us(usleep_cb, &tok, (uint64_t)usec);
    thread_block();
    return 0;
}

/** ----------------------------------------------------------------------
 * @brief sys_signal() – Register a user-mode handler for @signum.
 *
 * Replaces self->sig.handlers[signum] and returns the previous slot
 * value so userspace can chain handlers POSIX-style. SIG_DFL (NULL)
 * and SIG_IGN are accepted as @handler. Validates @signum against
 * the [1, NSIG) range; returns -1 otherwise.
 * @param signum  Signal number to install for.
 * @param handler User function pointer, or SIG_DFL / SIG_IGN.
 * @return Previous handler pointer cast to long, or -1 on bad signum.
 * -------------------------------------------------------------------- */
static long sys_signal(int signum, void (*handler)(void))
{
    if (signum <= 0 || signum >= NSIG)
        return -1;
    struct thread *self = get_current();
    void (*prev)(void) = self->sig.handlers[signum];
    self->sig.handlers[signum] = handler;
    return (long)(uintptr_t)prev;
}

/** ----------------------------------------------------------------------
 * @brief sys_kill() – Post @signum to process @pid.
 *
 * Looks up the target via find_thread_by_pid(); refuses if the pid is
 * not a live user process. Posting is asynchronous — delivery happens
 * the next time the target is about to return to U-mode through
 * signal_check_and_dispatch().
 * @param pid    Target process id.
 * @param signum Signal number in [1, NSIG).
 * @return 0 on success, -1 if @pid does not exist or @signum is bad.
 * -------------------------------------------------------------------- */
static long sys_kill(int pid, int signum)
{
    if (signum <= 0 || signum >= NSIG)
        return -1;
    struct thread *t = find_thread_by_pid(pid);
    if (!t || !t->pgd)
        return -1;
    signal_post(t, signum);
    return 0;
}

/** ----------------------------------------------------------------------
 * @brief sys_sigreturn() – SYS_SIGRETURN entry point.
 *
 * Thin wrapper around signal_return() that lives in syscall.c so
 * do_syscall()'s switch can reach it. The heavy lifting (restoring
 * the saved trap_frame and freeing the sigstack) is in signal.c.
 * @param[in,out] tf Live trap frame at trampoline ecall time.
 * @return Original syscall return value to land in tf->a0.
 * -------------------------------------------------------------------- */
static long sys_sigreturn(struct trap_frame *tf)
{
    return signal_return(tf);
}

/** ----------------------------------------------------------------------
 * @brief sys_mmap() – Map an anonymous memory region into the caller.
 *
 * Thin wrapper that forwards to do_mmap() on the current thread. The
 * return value (a user base VA, or MAP_FAILED == (void*)-1 on error) is
 * delivered through tf->a0 by trap.c. Only anonymous, eager mappings are
 * supported; fd/offset are absent from the ABI.
 * @param[in] addr   Placement hint (NULL → kernel chooses).
 * @param     length Requested byte length (page-rounded by do_mmap).
 * @param     prot   User PROT_* bits.
 * @param     flags  User MAP_* bits (MAP_ANONYMOUS required).
 * @return User base VA on success, MAP_FAILED on failure, both as long.
 * -------------------------------------------------------------------- */
static long sys_mmap(void *addr, unsigned long length, int prot, int flags)
{
    return (long)do_mmap(get_current(), addr, length, prot, flags);
}

/** ----------------------------------------------------------------------
 * @brief sys_open() – Open @pathname and return a descriptor for it.
 *
 * Resolves @pathname against the caller's cwd when it is relative, then
 * publishes the handle in the caller's table. A table that is already
 * full is detected only after the open succeeded, so the handle is
 * closed again rather than leaked.
 * @param[in] pathname User pointer to the path to open.
 * @param     flags    O_CREAT to create the file when it is missing.
 * @return A non-negative descriptor, or a negated errno on failure.
 * -------------------------------------------------------------------- */
static long sys_open(const char *pathname, int flags)
{
    struct thread *self = get_current();
    char path[VFS_MAX_PATHNAME + 1];

    int ret = copy_path_from_user(pathname, path, sizeof(path));
    if (ret != 0)
        return ret;

    struct file *file = NULL;
    ret = vfs_open_at(self->cwd, path, flags, &file);
    if (ret != 0)
        return ret;

    int fd = fd_alloc(self, file);
    if (fd < 0) {
        vfs_close(file);
        return fd;
    }
    return fd;
}

/** ----------------------------------------------------------------------
 * @brief sys_close() – Release descriptor @fd.
 *
 * Clears the slot first so the descriptor is reusable even if the file
 * system reports a failure while releasing the handle.
 * @param fd Descriptor to close.
 * @return 0 on success, -EBADF when @fd names no open file.
 * -------------------------------------------------------------------- */
static long sys_close(int fd)
{
    struct thread *self = get_current();
    struct file *file = fd_get(self, fd);

    if (!file)
        return -EBADF;

    self->fd_table[fd] = NULL;
    return vfs_close(file);
}

/** ----------------------------------------------------------------------
 * @brief sys_read() – Read up to @count bytes from @fd into @buf.
 * @param     fd    Descriptor to read from.
 * @param[out] buf  User destination buffer.
 * @param     count Maximum number of bytes to read.
 * @return Bytes read (0 at EOF), or a negated errno on failure.
 * -------------------------------------------------------------------- */
static long sys_read(int fd, void *buf, unsigned long count)
{
    struct thread *self = get_current();
    struct file *file = fd_get(self, fd);

    if (!file)
        return -EBADF;
    if (count == 0)
        return 0;
    if (!in_user_range(buf, count))
        return -EFAULT;

    return vfs_read(file, buf, (size_t)count);
}

/** ----------------------------------------------------------------------
 * @brief sys_write() – Write up to @count bytes from @buf to @fd.
 * @param    fd    Descriptor to write to.
 * @param[in] buf  User source buffer.
 * @param    count Number of bytes to write.
 * @return Bytes written, or a negated errno on failure.
 * -------------------------------------------------------------------- */
static long sys_write(int fd, const void *buf, unsigned long count)
{
    struct thread *self = get_current();
    struct file *file = fd_get(self, fd);

    if (!file)
        return -EBADF;
    if (count == 0)
        return 0;
    if (!in_user_range(buf, count))
        return -EFAULT;

    return vfs_write(file, buf, (size_t)count);
}

/** ----------------------------------------------------------------------
 * @brief sys_mkdir() – Create the directory named by @pathname.
 *
 * @mode is accepted for signature compatibility and ignored: this
 * kernel has no access control for it to describe.
 * @param[in] pathname User pointer to the path of the new directory.
 * @param     mode     Permission bits; ignored.
 * @return 0 on success, or a negated errno on failure.
 * -------------------------------------------------------------------- */
static long sys_mkdir(const char *pathname, unsigned int mode)
{
    (void)mode;

    struct thread *self = get_current();
    char path[VFS_MAX_PATHNAME + 1];

    int ret = copy_path_from_user(pathname, path, sizeof(path));
    if (ret != 0)
        return ret;

    return vfs_mkdir_at(self->cwd, path);
}

/** ----------------------------------------------------------------------
 * @brief sys_mount() – Mount @filesystem onto the directory @target.
 *
 * @src, @flags and @data are accepted for signature compatibility and
 * ignored: the only file system here is memory backed, so it has no
 * device to name and no options to parse.
 * @param[in] src        Source device; ignored.
 * @param[in] target     User pointer to the mount-point path.
 * @param[in] filesystem User pointer to the registered fs name.
 * @param     flags      Mount flags; ignored.
 * @param[in] data       File-system private options; ignored.
 * @return 0 on success, or a negated errno on failure.
 * -------------------------------------------------------------------- */
static long sys_mount(const char *src, const char *target,
                      const char *filesystem, unsigned long flags,
                      const void *data)
{
    (void)src;
    (void)flags;
    (void)data;

    struct thread *self = get_current();
    char target_path[VFS_MAX_PATHNAME + 1];
    char fs_name[VFS_MAX_PATHNAME + 1];

    int ret = copy_path_from_user(target, target_path, sizeof(target_path));
    if (ret != 0)
        return ret;

    ret = copy_path_from_user(filesystem, fs_name, sizeof(fs_name));
    if (ret != 0)
        return ret;

    return vfs_mount_at(self->cwd, target_path, fs_name);
}

/** ----------------------------------------------------------------------
 * @brief sys_chdir() – Move the caller's working directory to @path.
 *
 * Only the calling task is affected: cwd lives in struct thread, so a
 * child that chdir()s never drags its parent along.
 * @param[in] path User pointer to the path of the new directory.
 * @return 0 on success, or a negated errno on failure.
 * -------------------------------------------------------------------- */
static long sys_chdir(const char *path)
{
    struct thread *self = get_current();
    char kpath[VFS_MAX_PATHNAME + 1];

    int ret = copy_path_from_user(path, kpath, sizeof(kpath));
    if (ret != 0)
        return ret;

    struct vnode *dir = NULL;
    ret = vfs_chdir(self->cwd, kpath, &dir);
    if (ret != 0)
        return ret;

    self->cwd = dir;
    return 0;
}

/** ----------------------------------------------------------------------
 * @brief do_syscall() – Decode tf->a7 and dispatch to the handler.
 *
 * Unknown syscall numbers return -ENOSYS so user space can detect them.
 * @param[in,out] tf Trap frame on the kernel stack.
 * @return Value to be written into tf->a0 by trap.c.
 * -------------------------------------------------------------------- */
long do_syscall(struct trap_frame *tf)
{
    switch (tf->a7) {
    case SYS_GETPID:
        return sys_getpid();
    case SYS_UART_READ:
        return sys_uart_read((char *)tf->a0, (long)tf->a1);
    case SYS_UART_WRITE:
        return sys_uart_write((const char *)tf->a0, (long)tf->a1);
    case SYS_EXEC:
        return sys_exec((const char *)tf->a0, tf);
    case SYS_FORK:
        return sys_fork(tf);
    case SYS_WAITPID:
        return sys_waitpid((long)tf->a0);
    case SYS_EXIT:
        sys_exit((long)tf->a0);       /* noreturn */
    case SYS_STOP:
        return sys_stop((long)tf->a0);
    case SYS_DISPLAY:
        return sys_display((unsigned int *)tf->a0,
                           (unsigned int)tf->a1,
                           (unsigned int)tf->a2);
    case SYS_USLEEP:
        return sys_usleep((unsigned int)tf->a0);
    case SYS_SIGNAL:
        return sys_signal((int)tf->a0, (void (*)(void))tf->a1);
    case SYS_SIGRETURN:
        return sys_sigreturn(tf);
    case SYS_KILL:
        return sys_kill((int)tf->a0, (int)tf->a1);
    case SYS_MMAP:
        return sys_mmap((void *)tf->a0, (unsigned long)tf->a1,
                        (int)tf->a2, (int)tf->a3);
    case SYS_OPEN:
        return sys_open((const char *)tf->a0, (int)tf->a1);
    case SYS_CLOSE:
        return sys_close((int)tf->a0);
    case SYS_READ:
        return sys_read((int)tf->a0, (void *)tf->a1,
                        (unsigned long)tf->a2);
    case SYS_WRITE:
        return sys_write((int)tf->a0, (const void *)tf->a1,
                         (unsigned long)tf->a2);
    case SYS_MKDIR:
        return sys_mkdir((const char *)tf->a0, (unsigned int)tf->a1);
    case SYS_MOUNT:
        return sys_mount((const char *)tf->a0, (const char *)tf->a1,
                         (const char *)tf->a2, (unsigned long)tf->a3,
                         (const void *)tf->a4);
    case SYS_CHDIR:
        return sys_chdir((const char *)tf->a0);
    default:
        return -ENOSYS;
    }
}
