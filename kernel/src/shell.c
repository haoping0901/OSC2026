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
#include "types.h"

#define USER_STACK_SIZE  (16 * 1024)  /* 16 KiB user stack for prog.bin */

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
    uart_puts("  setTimeout <sec> <msg> - print msg after sec seconds.\n");
    uart_puts("  taskdemo - enqueue 3 tasks out of priority order.\n");
    uart_puts("  tasknest - nested priority dispatch demo (start -> inner -> end).\n");
    uart_puts("  threadtest - spawn 3 cooperative threads (Lab5 Basic Ex1).\n");
}

/* ---------- threadtest (Lab5 Basic Ex1: cooperative threads) ----------- */

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
 * @param arg Priority value (cast through uintptr_t) stamped on output.
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
 * @param arg Pointer to a kmalloc'd struct setto_ctx (transferred in).
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
 * @param args Command tail (the characters after "setTimeout ").
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
 * @brief run_user_program() – Load a file from initrd and drop into U-mode.
 *
 * Looks up @name inside the cpio archive, copies it into a fresh kmalloc
 * region sized for both the code and a 16 KiB user stack, then calls
 * enter_user_mode() which does not return. The shell regains control only
 * through a trap (printed by trap_handler) followed by sret back to the
 * program.
 * @param name Filename inside the initial ramdisk (e.g. "prog.bin").
 * -------------------------------------------------------------------- */
static void run_user_program(const char *name)
{
    const void *initrd = (const void *)dtb_getprop("/chosen",
                                                   "linux,initrd-start");
    if (!initrd) {
        uart_puts("exec: initrd not found\n");
        return;
    }

    const void *src = NULL;
    unsigned long src_size = 0;
    if (cpio_find(initrd, name, &src, &src_size) != 0) {
        uart_puts("exec: ");
        uart_puts(name);
        uart_puts(": not found\n");
        return;
    }

    unsigned long total = src_size + USER_STACK_SIZE;
    void *buf = kmalloc(total);
    if (!buf) {
        uart_puts("exec: out of memory\n");
        return;
    }
    mem_cpy(buf, src, src_size);

    uintptr_t entry   = (uintptr_t)buf;
    uintptr_t user_sp = ((uintptr_t)buf + total) & ~0xfUL;

    uart_puts("[exec] entry=0x");
    print_hex_ulong(entry);
    uart_puts(" sp=0x");
    print_hex_ulong(user_sp);
    uart_puts(" size=");
    print_dec_ulong(src_size);
    uart_puts("\n");

    trap_set_user_base(entry);
    enter_user_mode(entry, user_sp);
}

/* ---------- Lab 3 test case --------------------------------------------- */

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
    } else if (str_eq(cmd, "ls") != 0) {
        cpio_ls((void *)dtb_getprop("/chosen", "linux,initrd-start"));
    } else if (str_startswith(cmd, "cat ")) {
        const char *filename = cmd + 4;
        /* skip leading spaces */
        while (*filename == ' ') {
            filename++;
        }
        if (*filename == '\0') {
            uart_puts("usage: cat <filename>\n");
        } else {
            cpio_cat((void *)dtb_getprop("/chosen", "linux,initrd-start"), filename);
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