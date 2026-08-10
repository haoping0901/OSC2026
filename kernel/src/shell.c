#include "uart.h"
#include "sbi.h"
#include "utils.h"
#include "dtb.h"
#include "cpio.h"
#include "buddy.h"
#include "kmalloc.h"
#include "trap.h"
#include "timer.h"
#include "task.h"
#include "sched.h"
#include "syscall.h"
#include "riscv.h"
#include "list.h"
#include "types.h"
#include "signal.h"
#include "mm.h"
#include "vfs.h"

#define SHELL_BUF_SIZE 128

#define BOOT_MAGIC 0x544F4F42UL /* "BOOT" */
#ifdef QEMU
#define KERNEL_LOAD_ADDR 0x82000000UL
#else
#define KERNEL_LOAD_ADDR 0x20000000UL
#endif // QEMU

static void shell_print_help(void)
{
    uart_puts("  help  - show all commands.\n");
    uart_puts("  hello - print Hello world.\n");
    uart_puts("  info  - print system info.\n");
    uart_puts("  load  - receive kernel over UART and boot.\n");
    uart_puts("  ls    - list files in the initial ramdisk.\n");
    uart_puts("  cat   - print content of a file in the initial ramdisk.\n");
    uart_puts("  exec  - load a user program from initrd and run it in U-mode.\n");
    uart_puts("  test  - run memory allocator test.\n");
    uart_puts("  vfstest - run VFS / tmpfs test.\n");
    uart_puts("  setTimeout <sec> <msg> - print msg after sec seconds.\n");
    uart_puts("  taskdemo - enqueue 3 tasks out of priority order.\n");
    uart_puts("  tasknest - nested priority dispatch demo (start -> inner -> end).\n");
    uart_puts("  threadtest - spawn 3 cooperative threads (Lab5 Basic Ex1).\n");
    uart_puts("  stop <pid> - forcibly terminate a user process (Lab5 Basic Ex2).\n");
    uart_puts("  kill <pid> <signum> - post a POSIX signal to a user process.\n");
}

/* ---------- threadtest (cooperative threads) --------------------------- */

/** ----------------------------------------------------------------------
 * @brief demo_thread_body() – Worker body for the threadtest demo.
 *
 * Loops five times: prints its own tid + iteration, busy-waits for a
 * visible interval, then yields via schedule(). Returning naturally
 * from this function falls through the trampoline into thread_exit(),
 * which marks the thread zombie so the idle thread eventually reaps
 * it. Output from three concurrent invocations should interleave
 * round-robin, confirming the context switch works.
 * -------------------------------------------------------------------- */
static void demo_thread_body(void)
{
    int id = get_current()->tid;
    for (int i = 0; i < 5; i++) {
        uart_puts("Thread id: ");
        print_dec_ulong((unsigned long)id);
        uart_puts(" iter ");
        print_dec_ulong((unsigned long)i);
        uart_puts("\n");
        for (volatile int j = 0; j < 1000000; j++)
            ;
        schedule();
    }
}

/* ---------- taskdemo / tasknest (Advanced Ex2: bottom-half tasks) ------- */

/** ----------------------------------------------------------------------
 * @brief demo_task_short() – Print a single tagged line.
 *
 * Bottom-half callback used by the taskdemo command. The integer
 * priority is smuggled through the void* arg via uintptr_t cast so
 * the demo does not need to allocate per-task context.
 * @param[in] arg Priority value (cast through uintptr_t) stamped on output.
 * -------------------------------------------------------------------- */
static void demo_task_short(void *arg)
{
    int p = (int)(uintptr_t)arg;
    uart_puts("[task pri=");
    print_dec_ulong((unsigned long)p);
    uart_puts("] hello\n");
}

/** ----------------------------------------------------------------------
 * @brief demo_task_slow() – Synchronous nested-priority dispatch demo.
 *
 * Runs at priority 9. Mid-flight it enqueues a higher-priority (5)
 * task and calls task_run_pending() itself; the nested dispatcher
 * sees 5 < 9 and runs the inner task before returning, so the inner
 * "[task pri=...] hello" line lands between this task's start / end
 * markers. This is the same g_running_priority gate that implements
 * IRQ-driven preemption — synthesised synchronously here so the demo
 * is reproducible without external input. arg is unused.
 * -------------------------------------------------------------------- */
static void demo_task_slow(void *arg)
{
    (void)arg;
    uart_puts("[slow pri=9] start\n");
    add_task(demo_task_short, (void *)(uintptr_t)123, 5);
    task_run_pending();
    uart_puts("[slow pri=9] end\n");
}

/* ---------- setTimeout (Advanced Ex1: timer multiplexing) --------------- */

#define SETTO_MSG_MAX 96

struct setto_ctx {
    uint64_t reg_tick;
    uint64_t exp_tick;
    char     msg[SETTO_MSG_MAX];
};

/** ----------------------------------------------------------------------
 * @brief setto_cb() – One-shot timer callback for the shell setTimeout.
 *
 * Runs in timer IRQ context. Prints the registered / scheduled /
 * actual-fire times (each converted from ticks to whole seconds via
 * timer_get_timebase_freq()) alongside the stashed message, then
 * frees the context. Safe to block briefly on UART because uart_putc
 * already masks SIE while it touches its ring.
 * @param[in] arg Pointer to a kmalloc'd struct setto_ctx (transferred in).
 * -------------------------------------------------------------------- */
static void setto_cb(void *arg)
{
    struct setto_ctx *c = arg;
    uint64_t hz  = timer_get_timebase_freq();
    uint64_t now = timer_read_ticks();

    uart_puts("\n[setTimeout] registered=");
    print_dec_ulong((unsigned long)(c->reg_tick / hz));
    uart_puts("s scheduled=");
    print_dec_ulong((unsigned long)(c->exp_tick / hz));
    uart_puts("s fired=");
    print_dec_ulong((unsigned long)(now / hz));
    uart_puts("s : ");
    uart_puts(c->msg);
    uart_puts("\n");

    kfree(c);
}

/** ----------------------------------------------------------------------
 * @brief shell_set_timeout() – Parse and queue a setTimeout command.
 *
 * Grammar: "setTimeout <positive_int_seconds> <message...>".  The
 * message runs to end-of-line; shell_handle_command() already NUL-
 * terminates at '\n'. The message is copied into a kmalloc'd context
 * because the readline buffer is reused on the next keystroke.
 * @param[in] args Command tail (the characters after "setTimeout ").
 * -------------------------------------------------------------------- */
static void shell_set_timeout(const char *args)
{
    const char *p = args;
    while (*p == ' ')
        p++;

    int sec = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
        sec = sec * 10 + (*p - '0');
        p++;
        digits++;
    }
    if (digits == 0 || sec <= 0 || *p != ' ') {
        uart_puts("usage: setTimeout <seconds> <message>\n");
        return;
    }
    while (*p == ' ')
        p++;
    if (*p == '\0') {
        uart_puts("usage: setTimeout <seconds> <message>\n");
        return;
    }

    struct setto_ctx *c = kmalloc(sizeof(*c));
    if (!c) {
        uart_puts("setTimeout: out of memory\n");
        return;
    }

    c->reg_tick = timer_read_ticks();
    c->exp_tick = c->reg_tick +
                  (uint64_t)sec * timer_get_timebase_freq();

    unsigned int i = 0;
    while (p[i] != '\0' && i < SETTO_MSG_MAX - 1) {
        c->msg[i] = p[i];
        i++;
    }
    c->msg[i] = '\0';

    add_timer(setto_cb, c, sec);
}

/** ----------------------------------------------------------------------
 * @brief run_user_program() – Spawn a user process from an initrd file.
 *
 * Hands the program off to thread_spawn_user(), which builds a kernel
 * thread together with its trap_frame. The shell yields once so the
 * new thread starts running; it later exits on its own and the idle
 * thread reaps its kernel stack. Multiple `exec` invocations therefore
 * coexist as concurrent user processes.
 * @param[in] name Filename inside the initial ramdisk.
 * -------------------------------------------------------------------- */
static void run_user_program(const char *name)
{
    struct thread *t = thread_spawn_user(name);
    if (!t) {
        uart_puts("exec: cannot start ");
        uart_puts(name);
        uart_puts("\n");
        return;
    }
    /* Wait until the process has fully exited. sys_exit() wakes the
     * parent (shell) explicitly before yielding. */
    while (t->state != THREAD_ZOMBIE)
        thread_block();

    /* Reap the zombie so it doesn't linger in memory. */
    unsigned long flags = sie_save_clear();
    list_del(&t->sibling);
    sie_restore(flags);
    sched_zombify(t);
}

/** ----------------------------------------------------------------------
 * @brief shell_stop_pid() – Debug command: forcibly terminate a pid.
 *
 * Wraps sys_stop()'s logic for use from kernel context (without going
 * through ecall). Useful for verifying §4.6 of the plan.
 * @param[in] args Command tail (decimal pid).
 * -------------------------------------------------------------------- */
static void shell_stop_pid(const char *args)
{
    const char *p = args;
    while (*p == ' ')
        p++;
    int pid = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
        pid = pid * 10 + (*p - '0');
        p++;
        digits++;
    }
    if (digits == 0) {
        uart_puts("usage: stop <pid>\n");
        return;
    }

    struct thread *t = find_thread_by_pid(pid);
    if (!t || !t->pgd) {
        uart_puts("stop: pid not found\n");
        return;
    }

    unsigned long flags = sie_save_clear();
    t->exit_status = -1;
    if (t->state == THREAD_READY)
        list_del(&t->link);
    t->state = THREAD_ZOMBIE;
    struct thread *par = t->parent;
    sie_restore(flags);

    /* User VM reclaimed by the reaper once satp has switched away. */
    if (par)
        thread_wakeup(par);
    uart_puts("[stop] pid=");
    print_dec_ulong((unsigned long)pid);
    uart_puts(" terminated\n");
}

/** ----------------------------------------------------------------------
 * @brief shell_kill_pid() – Debug command: post a signal from the shell.
 *
 * Parses "kill <pid> <signum>" and calls signal_post() directly on the
 * target. Bypasses sys_kill() because the shell runs in S-mode and
 * cannot issue ecalls to itself; the effect on the target is identical.
 * @param[in] args Command tail after "kill ".
 * -------------------------------------------------------------------- */
static void shell_kill_pid(const char *args)
{
    const char *p = args;
    int pid = 0;
    int digits = 0;
    int signum = 0;

    while (*p == ' ')
        p++;
    while (*p >= '0' && *p <= '9') {
        pid = pid * 10 + (*p - '0');
        p++;
        digits++;
    }
    if (digits == 0 || *p != ' ') {
        uart_puts("usage: kill <pid> <signum>\n");
        return;
    }

    while (*p == ' ')
        p++;
    digits = 0;
    while (*p >= '0' && *p <= '9') {
        signum = signum * 10 + (*p - '0');
        p++;
        digits++;
    }
    if (digits == 0 || signum <= 0 || signum >= NSIG) {
        uart_puts("usage: kill <pid> <signum>\n");
        return;
    }

    struct thread *t = find_thread_by_pid(pid);
    if (!t || !t->pgd) {
        uart_puts("kill: pid not found\n");
        return;
    }

    signal_post(t, signum);
    uart_puts("[kill] posted signum=");
    print_dec_ulong((unsigned long)signum);
    uart_puts(" to pid=");
    print_dec_ulong((unsigned long)pid);
    uart_puts("\n");
}

/* ---------- VFS / tmpfs test case -------------------------------------- */

/** ----------------------------------------------------------------------
 * @brief vfs_report() – Print one PASS/FAIL line for a check.
 *
 * Keeps the test body free of repeated formatting so each check reads
 * as a single assertion.
 * @param[in] name Description of the check.
 * @param     ok   Non-zero when the check succeeded.
 * -------------------------------------------------------------------- */
static void vfs_report(const char *name, int ok)
{
    uart_puts(ok ? "[ OK ] " : "[FAIL] ");
    uart_puts(name);
    uart_puts("\n");
}

/** ----------------------------------------------------------------------
 * @brief test_vfs() – Exercise the tmpfs round trip and its limits.
 *
 * Covers the create/write/close/reopen/read path plus the boundaries a
 * tmpfs must enforce: name length, entries per directory, file size,
 * duplicate creation, and reading an empty file.
 * -------------------------------------------------------------------- */
static void test_vfs(void)
{
    struct file *f = NULL;
    char buf[64];
    const char *msg = "Hello VFS from tmpfs!";
    int msg_len = 21;

    uart_puts("=== VFS / tmpfs test ===\n");

    /* Create, write, then close. */
    vfs_report("open /test.txt with O_CREAT",
               vfs_open("/test.txt", O_CREAT, &f) == 0 && f != NULL);
    if (!f)
        return;

    int written = vfs_write(f, msg, (size_t)msg_len);
    vfs_report("write returns the full length", written == msg_len);
    vfs_report("close", vfs_close(f) == 0);

    /* Reopen without O_CREAT: the vnode must have outlived the handle. */
    f = NULL;
    vfs_report("reopen /test.txt without O_CREAT",
               vfs_open("/test.txt", 0, &f) == 0 && f != NULL);
    if (!f)
        return;

    for (int i = 0; i < (int)sizeof(buf); i++)
        buf[i] = '\0';
    int got = vfs_read(f, buf, sizeof(buf) - 1);
    vfs_report("read back the same length", got == msg_len);
    vfs_report("read back the same bytes", str_eq(buf, msg));
    uart_puts("       content: ");
    uart_puts(buf);
    uart_puts("\n");

    /* A second read starts at EOF because f_pos advanced. */
    vfs_report("read at EOF returns 0", vfs_read(f, buf, sizeof(buf)) == 0);
    vfs_close(f);

    /* Missing file without O_CREAT must fail and must not be created. */
    f = NULL;
    vfs_report("open missing file without O_CREAT fails",
               vfs_open("/nonexist.txt", 0, &f) != 0);

    /* Boundary: a name longer than the 15-character limit. */
    f = NULL;
    vfs_report("reject name longer than 15 chars",
               vfs_open("/0123456789abcdef", O_CREAT, &f) != 0);

    /* Boundary: an empty file reads as EOF straight away. */
    f = NULL;
    if (vfs_open("/empty.txt", O_CREAT, &f) == 0 && f) {
        vfs_report("read empty file returns 0",
                   vfs_read(f, buf, sizeof(buf)) == 0);
        vfs_close(f);
    } else {
        vfs_report("read empty file returns 0", 0);
    }

    /* Boundary: writing past the 4096-byte cap is truncated, not
     * refused, so the return value reports the short write. */
    f = NULL;
    if (vfs_open("/big.txt", O_CREAT, &f) == 0 && f) {
        static char big[5000];
        for (int i = 0; i < (int)sizeof(big); i++)
            big[i] = 'A';
        int n = vfs_write(f, big, sizeof(big));
        vfs_report("write beyond 4096 is truncated to 4096", n == 4096);
        vfs_report("further write at the cap returns 0",
                   vfs_write(f, big, 16) == 0);
        vfs_close(f);
    } else {
        vfs_report("write beyond 4096 is truncated to 4096", 0);
    }

    /* The two root-level directories the multi-level checks need are
     * created here, before the entry cap is reached: once the root is
     * full every mkdir would fail for lack of a free slot rather than
     * for the reason actually under test. */
    vfs_report("mkdir /dir1", vfs_mkdir("/dir1") == 0);
    vfs_report("mkdir /mnt", vfs_mkdir("/mnt") == 0);

    /* Boundary: the root directory holds at most 16 entries. Three are
     * already used (test.txt, empty.txt, big.txt) and the two
     * directories just above take two more, so exactly 11 further
     * entries must succeed before creation starts failing. */
    char name[8] = "/e00";
    int created = 0;
    for (int i = 0; i < 20; i++) {
        name[2] = (char)('0' + i / 10);
        name[3] = (char)('0' + i % 10);
        f = NULL;
        if (vfs_open(name, O_CREAT, &f) != 0)
            break;
        created++;
        vfs_close(f);
    }
    uart_puts("       entries created before the cap: ");
    print_dec_ulong((unsigned long)created);
    uart_puts("\n");
    vfs_report("directory stops accepting entries at 16", created == 11);

    /* A path that names no entry inside a directory is rejected. */
    f = NULL;
    vfs_report("reject the root path itself",
               vfs_open("/", O_CREAT, &f) != 0);

    /* ---- Multi-level VFS ---------------------------------------------
     * /dir1 and /mnt already exist: they had to be created before the
     * root filled up. Everything below either nests inside them or is
     * expected to fail, so the full root does not distort the results. */
    uart_puts("--- multi-level ---\n");

    /* Creating a directory whose name is taken must not succeed. */
    vfs_report("mkdir /dir1 twice fails", vfs_mkdir("/dir1") != 0);

    /* A directory nested inside a directory. */
    vfs_report("mkdir /dir1/dir2", vfs_mkdir("/dir1/dir2") == 0);

    /* An intermediate component that does not exist stops the walk. */
    vfs_report("mkdir under a missing parent fails",
               vfs_mkdir("/nodir/x") != 0);

    /* A file two levels down: create, write, close, reopen, read back. */
    const char *deep = "deep file";
    int deep_len = 9;
    f = NULL;
    if (vfs_open("/dir1/dir2/f.txt", O_CREAT, &f) == 0 && f) {
        vfs_report("write into /dir1/dir2/f.txt",
                   vfs_write(f, deep, (size_t)deep_len) == deep_len);
        vfs_close(f);

        f = NULL;
        for (int i = 0; i < (int)sizeof(buf); i++)
            buf[i] = '\0';
        if (vfs_open("/dir1/dir2/f.txt", 0, &f) == 0 && f) {
            vfs_report("read back /dir1/dir2/f.txt",
                       vfs_read(f, buf, sizeof(buf) - 1) == deep_len &&
                       str_eq(buf, deep));
            vfs_close(f);
        } else {
            vfs_report("read back /dir1/dir2/f.txt", 0);
        }
    } else {
        vfs_report("write into /dir1/dir2/f.txt", 0);
        vfs_report("read back /dir1/dir2/f.txt", 0);
    }

    /* vfs_lookup() resolves a full path; "/" names the root itself. */
    struct vnode *node = NULL;
    vfs_report("lookup /dir1/dir2", vfs_lookup("/dir1/dir2", &node) == 0);
    node = NULL;
    vfs_report("lookup a missing entry fails",
               vfs_lookup("/dir1/none", &node) != 0);
    node = NULL;
    vfs_report("lookup / returns the root",
               vfs_lookup("/", &node) == 0 && node == g_rootfs->root);

    /* Mounting: a fresh tmpfs onto the existing /mnt directory. The
     * file created first is what the mount must shadow. */
    f = NULL;
    vfs_report("create /mnt/shadow.txt before mounting",
               vfs_open("/mnt/shadow.txt", O_CREAT, &f) == 0 && f != NULL);
    if (f)
        vfs_close(f);

    vfs_report("mount an unknown fs fails",
               vfs_mount("/mnt", "nosuchfs") != 0);
    vfs_report("mount onto a missing path fails",
               vfs_mount("/notexist", "tmpfs") != 0);

    /* A regular file cannot be covered: nothing could ever be looked up
     * inside the result. */
    vfs_report("mount onto a regular file fails",
               vfs_mount("/test.txt", "tmpfs") != 0);

    vfs_report("mount tmpfs at /mnt", vfs_mount("/mnt", "tmpfs") == 0);
    vfs_report("mount at /mnt twice fails",
               vfs_mount("/mnt", "tmpfs") != 0);

    /* Crossing the mount point: /mnt now resolves to the new mount's
     * root, so the vnode belongs to a different mount than the rootfs. */
    node = NULL;
    vfs_report("lookup /mnt crosses into the mounted fs",
               vfs_lookup("/mnt", &node) == 0 && node != NULL &&
               node->mount != g_rootfs);

    /* The entry created before the mount is hidden by it. */
    f = NULL;
    vfs_report("the mount shadows /mnt/shadow.txt",
               vfs_open("/mnt/shadow.txt", 0, &f) != 0);

    /* A file created below the mount point lands in the mounted fs. */
    f = NULL;
    if (vfs_open("/mnt/inner.txt", O_CREAT, &f) == 0 && f) {
        vfs_report("write into the mounted fs",
                   vfs_write(f, "inner", 5) == 5);
        vfs_close(f);

        node = NULL;
        vfs_report("/mnt/inner.txt belongs to the mounted fs",
                   vfs_lookup("/mnt/inner.txt", &node) == 0 &&
                   node != NULL && node->mount != g_rootfs);
    } else {
        vfs_report("write into the mounted fs", 0);
        vfs_report("/mnt/inner.txt belongs to the mounted fs", 0);
    }

    uart_puts("=== VFS / tmpfs test done ===\n");
}    /* test_vfs */

/* ---------- Memory allocator test case --------------------------------- */

static void test_alloc_1(void)
{
    uart_puts("Testing memory allocation...\n");

    /* Page-level allocations (> MAX_CHUNK_SIZE → buddy) */
    uart_puts("Allocating 4000 bytes...\n");
    char *ptr1 = (char *)kmalloc(4000);
    uart_puts("Allocating 8000 bytes...\n");
    char *ptr2 = (char *)kmalloc(8000);
    uart_puts("Allocating another 4000 bytes...\n");
    char *ptr3 = (char *)kmalloc(4000);
    uart_puts("Allocating 4000 bytes again...\n");
    char *ptr4 = (char *)kmalloc(4000);

    uart_puts("Freeing the 4000-byte block...\n");
    kfree(ptr1);
    uart_puts("Freeing the 8000-byte block...\n");
    kfree(ptr2);
    uart_puts("Freeing the last two 4000-byte blocks...\n");
    kfree(ptr3);
    uart_puts("Freeing the last 4000-byte block...\n");
    kfree(ptr4);

    /* Chunk-level allocations */
    uart_puts("Testing dynamic allocator...\n");
    uart_puts("Allocating chunks of sizes 16, 32, 64, 128, 16, 32 bytes...\n");
    char *kmem_ptr1 = (char *)kmalloc(16);
    char *kmem_ptr2 = (char *)kmalloc(32);
    char *kmem_ptr3 = (char *)kmalloc(64);
    char *kmem_ptr4 = (char *)kmalloc(128);
    char *kmem_ptr5 = (char *)kmalloc(16);
    char *kmem_ptr6 = (char *)kmalloc(32);

    uart_puts("Freeing the 6 allocated chunks...\n");
    kfree(kmem_ptr1);
    kfree(kmem_ptr2);
    kfree(kmem_ptr3);
    kfree(kmem_ptr4);
    kfree(kmem_ptr5);
    kfree(kmem_ptr6);

    /* Test allocate new page if the cache is not enough */
    uart_puts("Testing chunk pool refill...\n");
    void *kmem_ptr[102];
    for (int i = 0; i < 100; i++) {
        kmem_ptr[i] = (char *)kmalloc(128);
    }
    for (int i = 0; i < 100; i++) {
        kfree(kmem_ptr[i]);
    }

    /* Test exceeding the maximum size */
    uart_puts("Testing allocation exceeding maximum size...\n");
    char *kmem_ptr7 = (char *)kmalloc(buddy_get_total_pages() * PAGE_SIZE + 1);
    if (kmem_ptr7 == NULL) {
        uart_puts("Allocation failed as expected for size > MAX_ALLOC_SIZE\n");
    } else {
        uart_puts("Unexpected allocation success for size > MAX_ALLOC_SIZE\n");
        kfree(kmem_ptr7);
    }

    uart_puts("=== Memory allocation test done ===\n");
}

static void shell_load_kernel(void)
{
    unsigned int magic;
    unsigned int size;
    volatile unsigned char *dst;
    unsigned int i;

    uart_puts("Waiting for kernel over UART...\n");

    /* Read 8-byte header: magic (LE) + size (LE) */
    magic = (unsigned int)uart_getc() | ((unsigned int)uart_getc() << 8) |
            ((unsigned int)uart_getc() << 16) |
            ((unsigned int)uart_getc() << 24);
    size = (unsigned int)uart_getc() | ((unsigned int)uart_getc() << 8) |
           ((unsigned int)uart_getc() << 16) |
           ((unsigned int)uart_getc() << 24);

    if (magic != BOOT_MAGIC) {
        uart_puts("Invalid header (bad magic).\n");
        return;
    }

    dst = (volatile unsigned char *)KERNEL_LOAD_ADDR;
    for (i = 0; i < size; ++i)
        dst[i] = uart_getc_raw();

    uart_puts("Loaded ");
    print_dec_ulong((unsigned long)size);
    uart_puts(" bytes, jumping to 0x");
    print_hex_ulong((unsigned long)dst);
    uart_puts(" ...\n");

    /* Jump to loaded kernel; do not return. */
    ((void (*)(unsigned long, void *))dst)(0, dtb_get_addr());
}

static void shell_print_info(void)
{
    struct sbiret ret;

    ret = sbi_get_spec_version();
    if (ret.error == 0) {
        uart_puts("OpenSBI spec version: 0x");
        print_hex_ulong((unsigned long)ret.value);
        uart_puts("\n");
    } else {
        uart_puts("OpenSBI spec version: error ");
        print_dec_ulong((unsigned long)ret.error);
        uart_puts("\n");
    }

    ret = sbi_get_impl_id();
    if (ret.error == 0) {
        uart_puts("Implementation ID: 0x");
        print_hex_ulong((unsigned long)ret.value);
        uart_puts("\n");
    } else {
        uart_puts("Implementation ID: error ");
        print_dec_ulong((unsigned long)ret.error);
        uart_puts("\n");
    }

    ret = sbi_get_impl_version();
    if (ret.error == 0) {
        uart_puts("Implementation version: 0x");
        print_hex_ulong((unsigned long)ret.value);
        uart_puts("\n");
    } else {
        uart_puts("Implementation version: error ");
        print_dec_ulong((unsigned long)ret.error);
        uart_puts("\n");
    }
}

static void shell_handle_command(const char *cmd)
{
    if (str_eq(cmd, "help") != 0) {
        shell_print_help();
    } else if (str_eq(cmd, "hello") != 0) {
        uart_puts("Hello world.\n");
    } else if (str_eq(cmd, "info") != 0) {
        shell_print_info();
    } else if (str_eq(cmd, "load") != 0) {
        shell_load_kernel();
    } else if (str_eq(cmd, "test") != 0) {
        test_alloc_1();
    } else if (str_eq(cmd, "vfstest") != 0) {
        test_vfs();
    } else if (str_eq(cmd, "ls") != 0) {
        /* initrd-start is a PA from the DTB; under paging we must deref
         * via its higher-half VA, mirroring how main.c handles every
         * other DTB-derived PA. */
        uintptr_t initrd_pa = dtb_getprop("/chosen", "linux,initrd-start");
        cpio_ls(initrd_pa ? phys_to_virt(initrd_pa) : 0);
    } else if (str_startswith(cmd, "cat ")) {
        const char *filename = cmd + 4;
        /* skip leading spaces */
        while (*filename == ' ') {
            filename++;
        }
        if (*filename == '\0') {
            uart_puts("usage: cat <filename>\n");
        } else {
            uintptr_t initrd_pa =
                dtb_getprop("/chosen", "linux,initrd-start");
            cpio_cat(initrd_pa ? phys_to_virt(initrd_pa) : 0, filename);
        }
    } else if (str_eq(cmd, "cat") != 0) {
        uart_puts("usage: cat <filename>\n");
    } else if (str_startswith(cmd, "exec ")) {
        const char *name = cmd + 5;
        while (*name == ' ') {
            name++;
        }
        if (*name == '\0') {
            uart_puts("usage: exec <filename>\n");
        } else {
            run_user_program(name);
        }
    } else if (str_eq(cmd, "exec") != 0) {
        uart_puts("usage: exec <filename>\n");
    } else if (str_startswith(cmd, "setTimeout ")) {
        shell_set_timeout(cmd + 11);
    } else if (str_eq(cmd, "setTimeout") != 0) {
        uart_puts("usage: setTimeout <seconds> <message>\n");
    } else if (str_eq(cmd, "taskdemo") != 0) {
        /* Enqueue three tasks in scrambled priority order. Expected
         * stdout is 1, 3, 5 — confirming the queue is priority-sorted
         * and equal priorities preserve FIFO. The shell is in thread
         * context (SIE = 1) so add_task is safe; we then call
         * task_run_pending() once to drain immediately rather than
         * waiting for the next IRQ. */
        add_task(demo_task_short, (void *)(uintptr_t)3, 3);
        add_task(demo_task_short, (void *)(uintptr_t)1, 1);
        add_task(demo_task_short, (void *)(uintptr_t)5, 5);
        task_run_pending();
    } else if (str_eq(cmd, "tasknest") != 0) {
        /* Schedule a priority-9 task that itself enqueues a pri=5
         * task and yields via task_run_pending(). Output order
         * (start -> inner pri=5 -> end) confirms the strict-< gate
         * in g_running_priority lets the inner task preempt before
         * the outer one finishes. */
        add_task(demo_task_slow, NULL, 9);
        task_run_pending();
    } else if (str_eq(cmd, "threadtest") != 0) {
        /* Spawn three worker threads and yield from the shell several
         * times so they all get a turn. After every worker calls
         * thread_exit() the run queue narrows back to just the
         * bootstrap and idle threads, at which point schedule() lands
         * us back here and the loop exits. The idle thread reaps the
         * zombies on its own cycle. */
        thread_create(demo_thread_body);
        thread_create(demo_thread_body);
        thread_create(demo_thread_body);
        for (int i = 0; i < 20; i++)
            schedule();
        uart_puts("[threadtest] back in shell\n");
    } else if (str_startswith(cmd, "stop ")) {
        shell_stop_pid(cmd + 5);
    } else if (str_eq(cmd, "stop") != 0) {
        uart_puts("usage: stop <pid>\n");
    } else if (str_startswith(cmd, "kill ")) {
        shell_kill_pid(cmd + 5);
    } else if (str_eq(cmd, "kill") != 0) {
        uart_puts("usage: kill <pid> <signum>\n");
    } else if (*cmd != '\0') {
        uart_puts("Unknown command: ");
        uart_puts(cmd);
        uart_puts("\n");
    }
}

void shell(void)
{
    char buf[SHELL_BUF_SIZE];
    int len = 0;

    uart_puts("opi-rv2> ");

    while (1) {
        int c = uart_getc();

        if (c < 0) {
            continue;
        }

        if (c == '\n') {
            uart_putc('\n');
            buf[len] = '\0';
            shell_handle_command(buf);
            len = 0;
            uart_puts("opi-rv2> ");
        } else if (c == '\b' || c == 127) {
            if (len > 0) {
                len--;
                uart_puts("\b \b");
            }
        } else {
            if (len < SHELL_BUF_SIZE - 1) {
                buf[len++] = (char)c;
                uart_putc((char)c);
            }
        }
    }
}