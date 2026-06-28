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
 * contains [a, e). This both rejects stray kernel/MMIO pointers and keeps
 * the kernel from dereferencing an unmapped user VA (which would page-
 * fault in S-mode). Treats overflow as failure.
 *
 * Note: this validates mapping presence only, not prot — a PROT_NONE mmap
 * region passes here but a real access still faults in U-mode. Per-prot
 * checking is a deliberate non-goal.
 * @param p Start of the user buffer.
 * @param n Size in bytes.
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
 * @param buf   User destination.
 * @param count Maximum bytes to read.
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
 * @param buf   User source bytes.
 * @param count Number of bytes to write.
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
 * Looks the file up via cpio, allocates a fresh contiguous image+stack
 * buffer, frees the old one, and rewrites the in-place trap_frame so
 * the impending sret jumps into the new program at offset 0 with a
 * clean register file. Does not create a new thread; the caller's
 * pid stays the same.
 * @param path Filename inside the initial ramdisk.
 * @param tf   Trap frame on the kernel stack (will be rewritten).
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
     * Snapshot the old VM bookkeeping, install a fresh PGD on a scratch
     * thread view (we reuse self's fields only after success), then map
     * the new image. This keeps the running satp/PGD valid until the
     * switch below, and lets us bail out cleanly on OOM.
     */
    unsigned long *new_pgd = pgd_alloc();
    if (!new_pgd)
        return -1;

    /* Stash the old VM so we can free it after switching away from it.
     * The old image/stack/sigpage AND any mmap regions live on
     * self->vma_list; detach the whole list aside so uvm_setup_image()
     * can build the new image's VMAs on a fresh, empty list. */
    unsigned long *old_pgd = self->pgd;
    void *old_image = self->image_base;
    void *old_stack = self->user_stack_base;
    void *old_sig   = self->sigpage_base;
    struct list_head old_vmas;
    INIT_LIST_HEAD(&old_vmas);
    vma_detach_all(self, &old_vmas);

    /* Point self at the new PGD, then map the image into it (this also
     * installs a fresh signal page via uvm_setup_image). On failure
     * restore the old bookkeeping (including the detached VMA list) and
     * tear the new PGD down. */
    self->pgd = new_pgd;
    self->image_base = NULL;
    self->user_stack_base = NULL;
    self->sigpage_base = NULL;
    if (uvm_setup_image(self, src, sz) != 0) {
        /* uvm_setup_image rolls back all-or-nothing: on failure it leaves
         * self->vma_list empty and frees its own blocks. Restore the old
         * bookkeeping and move the detached old VMA list back onto self. */
        self->pgd = old_pgd;
        self->image_base = old_image;
        self->user_stack_base = old_stack;
        self->sigpage_base = old_sig;
        vma_reattach(self, &old_vmas);
        pgd_free(new_pgd);
        return -1;
    }

    /* Switch to the new address space, then reclaim the old one. The old
     * VMA list owns the old image/stack/sigpage/mmap frame blocks; free
     * them all via the detached list (satp no longer points at old_pgd). */
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
 * @brief fork_copy_vmas() – Build the child's VMA list during fork.
 *
 * Registers the three fixed regions (image/stack/sigpage, is_mmap=0) whose
 * frames @par already duplicated, then walks @par->vma_list and, for every
 * mmap VMA (is_mmap==1), allocates a private copy of the backing frames,
 * copies the parent's bytes in, maps them at the SAME user VA in ch->pgd,
 * and records a child VMA. POSIX fork semantics: anonymous mappings are
 * inherited as private copies.
 *
 * All-or-nothing: on any failure every node/block this function linked or
 * allocated is released and ch->vma_list is left empty, so the caller's
 * rollback (which still owns img/stk/sig directly) does not double-free.
 * @param par       Parent thread (source VMAs).
 * @param ch        Child thread (empty vma_list, valid ch->pgd).
 * @param img       Child image block (already duplicated + mapped).
 * @param img_bytes Image mapped length.
 * @param stk       Child stack block (already duplicated + mapped).
 * @param stk_bytes Stack mapped length.
 * @param sig       Child signal page (already allocated + mapped).
 * @return 0 on success, -1 on OOM (nothing left linked).
 * -------------------------------------------------------------------- */
static int fork_copy_vmas(struct thread *par, struct thread *ch,
                          void *img, unsigned long img_bytes,
                          void *stk, unsigned long stk_bytes, void *sig)
{
    /*
     * Allocate the three fixed-region nodes up front but do NOT link them
     * yet. They reference the caller-owned img/stk/sig blocks; if anything
     * below fails we kfree only the nodes, leaving those blocks for the
     * caller's rollback to free (no double free).
     */
    struct vma *vi = vma_alloc(USER_CODE_VA, img_bytes,
                               PROT_USER_RWX, img, 0);
    struct vma *vs = vma_alloc(USER_STACK_TOP - stk_bytes, stk_bytes,
                               PROT_USER_DATA, stk, 0);
    struct vma *vg = vma_alloc(SIGPAGE_VA, PAGE_SIZE,
                               PROT_USER_RWX, sig, 0);
    if (!vi || !vs || !vg)
        goto fail_nodes;

    /*
     * Copy each parent mmap region into a private child copy, accumulating
     * onto a local list. Keeping these separate from ch->vma_list until the
     * end means a mid-loop failure frees ONLY these copies (their own
     * blocks), never the fixed-region blocks the caller still owns.
     */
    struct list_head copies;
    INIT_LIST_HEAD(&copies);

    struct list_head *it;
    list_for_each(it, &par->vma_list) {
        struct vma *pv = list_entry(it, struct vma, link);
        if (!pv->is_mmap)
            continue;

        void *nkva = buddy_alloc(pv->len);
        if (!nkva)
            goto fail_copies;
        mem_cpy(nkva, pv->kva, pv->len);

        if (map_pages(ch->pgd, pv->va, pv->len,
                      virt_to_phys(nkva), pv->prot) != 0) {
            buddy_free(nkva);
            goto fail_copies;
        }
        struct vma *nv = vma_alloc(pv->va, pv->len, pv->prot, nkva, 1);
        if (!nv) {
            buddy_free(nkva);
            goto fail_copies;
        }
        list_add_tail(&nv->link, &copies);
    }

    /* All allocations succeeded: link everything into ch->vma_list. From
     * here the child owns the img/stk/sig blocks via vi/vs/vg. */
    vma_insert_sorted(ch, vi);
    vma_insert_sorted(ch, vs);
    vma_insert_sorted(ch, vg);
    while (!list_empty(&copies)) {
        struct vma *v = list_entry(copies.next, struct vma, link);
        list_del(&v->link);
        vma_insert_sorted(ch, v);
    }
    return 0;

fail_copies:
    vma_free_list(&copies);        /* free mmap copies' blocks + nodes */
fail_nodes:
    if (vi)
        kfree(vi);
    if (vs)
        kfree(vs);
    if (vg)
        kfree(vg);
    return -1;
}

/** ----------------------------------------------------------------------
 * @brief sys_fork() – Duplicate the calling process.
 *
 * Allocates a new thread, makes a full memcpy of the parent's
 * image+stack (chosen over text-sharing so non-PIC binaries with
 * writable .data/.bss work safely), translates the user sp into the
 * new buffer, and plants a trap_frame on the child's kernel stack
 * that mirrors the parent's except for a0 (=0 in the child) and sp
 * (=child_user_sp). The first switch_to() into the child enters
 * user_thread_bootstrap, jumps to trap_return_user, sret's into U.
 *
 * sepc was already advanced past the ecall by trap.c BEFORE
 * do_syscall() ran, so the child resumes at the instruction after
 * the fork-call ecall, just like the parent will.
 * @param tf Parent's trap frame.
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

    /* 2) Duplicate the image frames and map them at the SAME user VA.
     *    Separate physical frames + identical layout = isolation. */
    unsigned long img_bytes = par->image_pages;
    void *img = buddy_alloc(img_bytes);
    if (!img)
        goto fail_pgd;
    mem_cpy(img, par->image_base, img_bytes);
    asm volatile ("fence.i" ::: "memory");
    if (map_pages(ch->pgd, USER_CODE_VA, img_bytes,
                  virt_to_phys(img), PROT_USER_RWX) != 0)
        goto fail_img;

    /* 3) Duplicate the stack frames likewise. */
    unsigned long stk_bytes = par->user_stack_size;
    void *stk = buddy_alloc(stk_bytes);
    if (!stk)
        goto fail_img;
    mem_cpy(stk, par->user_stack_base, stk_bytes);
    if (map_pages(ch->pgd, USER_STACK_TOP - stk_bytes, stk_bytes,
                  virt_to_phys(stk), PROT_USER_DATA) != 0)
        goto fail_stk;

    /* 3b) Child's own signal page (PROT_USER_RWX). Contents need not be
     *     copied: the handler stack/trampoline are (re)established at
     *     dispatch time; we only need the backing frame mapped at the same
     *     SIGPAGE_VA so a signal can be delivered to the child in U-mode. */
    void *sig = buddy_alloc(PAGE_SIZE);
    if (!sig)
        goto fail_stk;
    if (map_pages(ch->pgd, SIGPAGE_VA, PAGE_SIZE,
                  virt_to_phys(sig), PROT_USER_RWX) != 0)
        goto fail_sig;

    ch->image_base      = img;
    ch->image_size      = par->image_size;
    ch->image_pages     = img_bytes;
    ch->user_stack_base = stk;
    ch->user_stack_size = stk_bytes;
    ch->sigpage_base    = sig;
    /* Inherit the parent's placement cursor so the child's future mmaps
     * land below the regions it inherited (copied below), not on top. */
    ch->mmap_top        = par->mmap_top;

    /*
     * 3c) Register the three fixed regions as the child's VMAs (is_mmap=0)
     *     and then copy every mmap VMA from the parent. After this point
     *     the child's frame blocks are owned by ch->vma_list, so failures
     *     unwind via vma_unmap_all(ch) (fail_vmas) — NOT the per-block
     *     buddy_free paths, which would double-free.
     */
    if (fork_copy_vmas(par, ch, img, img_bytes, stk, stk_bytes, sig) != 0)
        goto fail_sig;

    /* 4) Plant child's trap_frame at the top of its kstack. The child
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
     *    page is its own (mapped above), independent of the parent's. */
    mem_cpy(&ch->sig.handlers, &par->sig.handlers,
            sizeof(par->sig.handlers));
    ch->sig.pending       = 0;
    ch->sig.in_handler    = 0;

    /* 6) Parent linkage + enqueue. */
    ch->parent = par;
    unsigned long flags = sie_save_clear();
    list_add_tail(&ch->sibling, &par->children);
    sie_restore(flags);

    thread_enqueue_ready(ch);
    return (long)ch->pid;           /* parent path */

fail_sig:
    buddy_free(sig);
fail_stk:
    buddy_free(stk);
fail_img:
    buddy_free(img);
fail_pgd:
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
 * @brief sys_exit() – Terminate the current process.
 *
 * Reparents any surviving children onto g_bootstrap (so a future
 * waitpid by the shell can reap them; in fork_test scope this never
 * fires), frees the user image, marks self ZOMBIE, wakes the parent
 * if it was blocked in waitpid, and yields. The kernel stack is
 * reaped later by sys_waitpid()'s sched_zombify() call (or by the
 * stop() path).
 * @param status Exit status returned to the parent's waitpid.
 * -------------------------------------------------------------------- */
static void sys_exit(long status)
{
    struct thread *self = get_current();

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
     * self->pgd. The reaper (kill_zombies) frees the image/stack frames,
     * the signal page and the page tables after schedule() has switched
     * satp away. signal_release() only clears the in_handler gate so a
     * thread dying mid-handler leaves a clean signal state.
     */
    signal_release(self);

    if (par)
        thread_wakeup(par);

    schedule();
    for (;;)
        ;
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
     * is unsafe (it may still be the live satp of a preempted run). */
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
 * @param bmp User-space pixel array (XRGB8888).
 * @param w   Image width in pixels.
 * @param h   Image height in pixels.
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
 * @param arg Pointer to the sleeper's stack-allocated usleep_token.
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
 * @param tf Live trap frame at trampoline ecall time.
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
 * @param addr   Placement hint (NULL → kernel chooses).
 * @param length Requested byte length (page-rounded by do_mmap).
 * @param prot   User PROT_* bits.
 * @param flags  User MAP_* bits (MAP_ANONYMOUS required).
 * @return User base VA on success, MAP_FAILED on failure, both as long.
 * -------------------------------------------------------------------- */
static long sys_mmap(void *addr, unsigned long length, int prot, int flags)
{
    return (long)do_mmap(get_current(), addr, length, prot, flags);
}

/** ----------------------------------------------------------------------
 * @brief do_syscall() – Decode tf->a7 and dispatch to the handler.
 *
 * Unknown syscall numbers return -1 so user space can detect them.
 * @param tf Trap frame on the kernel stack.
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
    default:
        return -1;
    }
}
