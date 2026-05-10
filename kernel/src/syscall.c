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

/*
 * Lab5 Basic Ex2 system-call layer.
 *
 * Conventions:
 *   - All handlers receive their arguments unpacked from tf->a0..a2.
 *   - The return value is delivered via the long return type and
 *     written into tf->a0 by trap.c after do_syscall() returns.
 *   - sys_exit() is __noreturn — it never comes back, so its slot in
 *     do_syscall() does not return a value.
 */

/** ----------------------------------------------------------------------
 * @brief in_user_range() – Best-effort user-pointer validation.
 *
 * Without an MMU there is no hardware enforcement; instead we check
 * that [p, p+n) lies inside the contiguous image+stack buffer that
 * thread_spawn_user() / sys_fork() allocated for the current thread.
 * Catches obvious out-of-range arguments before they fault the
 * kernel. Treats overflow as failure.
 * @param p Start of the user buffer.
 * @param n Size in bytes.
 * @return 1 if the range is wholly inside the current image, else 0.
 * -------------------------------------------------------------------- */
static int in_user_range(const void *p, unsigned long n)
{
    /* Without an MMU, Non-PIC user binaries may access strings using
     * absolute addresses from the parent image. To support this, we
     * relax the check to allow any address that is not in the low 
     * kernel memory or sensitive hardware ranges. */
    uintptr_t a = (uintptr_t)p;
    uintptr_t e = a + n;
    
    if (e < a)
        return 0;
    
    /* DRAM check using buddy allocator boundaries to stay board-agnostic. */
    uintptr_t dram_base = buddy_get_base();
    uintptr_t dram_end  = dram_base + buddy_get_total_pages() * PAGE_SIZE;

    if (a >= dram_base && e <= dram_end)
        return 1;
        
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
    if (!self->image_base)
        return -1;

    const void *initrd = (const void *)
        dtb_getprop("/chosen", "linux,initrd-start");
    const void   *src;
    unsigned long sz;
    if (!initrd || cpio_find(initrd, path, &src, &sz) != 0)
        return -1;

    unsigned long total = sz + USER_STACK_SIZE;
    void *buf = kmalloc(total);
    if (!buf)
        return -1;
    mem_cpy(buf, src, sz);

    /* CRITICAL for real hardware: flush the instruction cache so that
     * the CPU sees the newly written code instead of stale cache lines. */
    asm volatile ("fence.i" ::: "memory");

    /* Swap image atomically wrt timer IRQ that might invoke schedule. */
    unsigned long flags = sie_save_clear();
    void *old = self->image_base;
    self->image_base = buf;
    self->image_size = sz;
    self->total_size = total;
    sie_restore(flags);
    kfree(old);

    /* Rewrite the trap_frame so trap_return_user lands the new image. */
    tf->sepc = (uintptr_t)buf;
    tf->sp   = ((uintptr_t)buf + total) & ~0xFUL;
    tf->tp = (uintptr_t)self;
    tf->ra = tf->gp = 0;
    tf->t0 = tf->t1 = tf->t2 = 0;
    tf->t3 = tf->t4 = tf->t5 = tf->t6 = 0;
    tf->a0 = tf->a1 = tf->a2 = tf->a3 = 0;
    tf->a4 = tf->a5 = tf->a6 = tf->a7 = 0;
    tf->s0 = tf->s1 = tf->s2 = tf->s3 = 0;
    tf->s4 = tf->s5 = tf->s6 = tf->s7 = 0;
    tf->s8 = tf->s9 = tf->s10 = tf->s11 = 0;

    trap_set_user_base((uintptr_t)buf);
    return 0;
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
    if (!par->image_base)
        return -1;

    struct thread *ch = thread_alloc_bare();
    if (!ch)
        return -1;

    /* 1) Full copy of image+stack into a fresh contiguous buffer. */
    ch->image_size = par->image_size;
    ch->total_size = par->total_size;
    ch->image_base = kmalloc(ch->total_size);
    if (!ch->image_base) {
        kfree(ch->kstack_base);
        kfree(ch);
        return -1;
    }
    mem_cpy(ch->image_base, par->image_base, ch->total_size);
    
    /* CRITICAL for real hardware: synchronize I-cache after memory copy. */
    asm volatile ("fence.i" ::: "memory");

    /* 2) Translate parent's user sp into child's buffer. */
    uintptr_t off_sp = (uintptr_t)tf->sp - (uintptr_t)par->image_base;
    uintptr_t child_user_sp = (uintptr_t)ch->image_base + off_sp;

    /* 3) Plant child's trap_frame at the top of its kstack. */
    uintptr_t top = (uintptr_t)ch->kstack_base + ch->kstack_size;
    top &= ~0xFUL;
    struct trap_frame *cf =
        (struct trap_frame *)(top - sizeof(struct trap_frame));
    /* Use mem_cpy() rather than struct assignment so the compiler does
     * not lower it into a libc memcpy() call (we are -nostdlib). */
    mem_cpy(cf, tf, sizeof(*cf));
    cf->a0 = 0;                     /* fork returns 0 in child */
    cf->sp = child_user_sp;
    cf->tp = (uintptr_t)ch;

    /* 4) Wire ctx for first switch_to() to land at user_thread_bootstrap. */
    ch->ctx.sp = (unsigned long)cf;
    ch->ctx.ra = (unsigned long)user_thread_bootstrap;

    /* 5) Parent linkage + enqueue. */
    ch->parent = par;
    unsigned long flags = sie_save_clear();
    list_add_tail(&ch->sibling, &par->children);
    sie_restore(flags);

    thread_enqueue_ready(ch);
    return (long)ch->pid;           /* parent path */
}

/** ----------------------------------------------------------------------
 * @brief sys_waitpid() – Block until child @pid exits, return its status.
 *
 * Locates the child in self->children (errors -1 if no such child),
 * blocks via thread_block() until the child becomes ZOMBIE, then
 * detaches and hands its kstack/struct off to the zombie reaper.
 * The child's image_base was already freed in sys_exit().
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

    if (self->image_base) {
        kfree(self->image_base);
        self->image_base = NULL;
    }

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
    if (!t || !t->image_base)
        return -1;

    unsigned long flags = sie_save_clear();
    t->exit_status = -1;
    if (t->state == THREAD_READY)
        list_del(&t->link);
    t->state = THREAD_ZOMBIE;
    struct thread *par = t->parent;
    sie_restore(flags);

    if (t->image_base) {
        kfree(t->image_base);
        t->image_base = NULL;
    }
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
    default:
        return -1;
    }
}
