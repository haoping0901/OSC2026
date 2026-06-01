#include "trap.h"
#include "riscv.h"
#include "timer.h"
#include "uart.h"
#include "plic.h"
#include "task.h"
#include "utils.h"
#include "types.h"
#include "syscall.h"
#include "sched.h"
#include "signal.h"

/* UART0 IRQ id (from DTB). Set via trap_set_uart_irq() before SEIE. */
static unsigned int g_uart_irq;

void trap_set_uart_irq(unsigned int irq)
{
    g_uart_irq = irq;
}

_Static_assert(sizeof(struct trap_frame) == TF_SIZE,
               "struct trap_frame size must match TF_SIZE in trap.h");

/*
 * Absolute load address of the running user program. Updated by
 * run_user_program() right before enter_user_mode(); consumed by
 * trap_handler() so diagnostic output shows offsets relative to the
 * program image rather than raw kmalloc() pointers.
 */
static uintptr_t g_user_base = 0;

void trap_set_user_base(uintptr_t base)
{
    g_user_base = base;
}

/** ----------------------------------------------------------------------
 * @brief deliver_pending_signal() – Return-to-U gate for signal delivery.
 *
 * Called on every trap-handler exit path that will sret into U-mode.
 * Restricts dispatch to user processes (image_base != NULL) so kernel
 * threads, the bootstrap thread and the idle thread are skipped. If
 * the default handler kills the current thread, we re-enter
 * schedule() so the kernel never sret's into a dead user image; the
 * call never returns from the dead thread's perspective.
 * @param tf Trap frame about to be restored by trap_return_user.
 * -------------------------------------------------------------------- */
static void deliver_pending_signal(struct trap_frame *tf)
{
    struct thread *cur = get_current();
    if (!cur || !cur->image_base)
        return;

    signal_check_and_dispatch(tf);

    if (cur->state == THREAD_ZOMBIE)
        schedule();
}

/** ----------------------------------------------------------------------
 * @brief trap_init() – Install the S-mode trap vector.
 *
 * Writes trap_entry into stvec in direct mode (mode bits = 00) and
 * clears sscratch. A zero sscratch is the sentinel used by trap_entry
 * to detect "trap from kernel"; enter_user_mode() later writes the
 * kernel sp into sscratch so that a U→S trap finds a valid stack.
 * -------------------------------------------------------------------- */
void trap_init(void)
{
    extern char trap_entry[];

    uintptr_t vec = (uintptr_t)trap_entry;
    asm volatile ("csrw stvec, %0"    :: "r"(vec));
    asm volatile ("csrw sscratch, %0" :: "r"(0UL));
}

/** ----------------------------------------------------------------------
 * @brief trap_handler() – Top-level S-mode trap dispatcher.
 *
 * Prints the diagnostic CSR trio (scause / sepc / stval). For an
 * ecall taken from U-mode (scause = 8), advances sepc by 4 so that
 * the eventual sret resumes at the instruction after ecall.
 * @param tf Pointer to the trap frame saved by trap_entry.
 * -------------------------------------------------------------------- */
void trap_handler(struct trap_frame *tf)
{
    uintptr_t cause = tf->scause;

    /* Interrupt: top half only — bottom-half work is drained from
     * task_run_pending() below with sstatus.SIE = 1 so a higher-
     * priority device can preempt the running callback. */
    if (cause & SCAUSE_INTR_BIT) {
        uintptr_t code = cause & ~SCAUSE_INTR_BIT;

        if (code == INTR_S_TIMER) {
            timer_top_half();
        } else if (code == INTR_S_EXT) {
            unsigned int irq = plic_claim();
            if (irq == g_uart_irq && irq != 0) {
                /* UART defers plic_complete() to its bottom half so
                 * the source stays masked at the PLIC until the BH
                 * unmasks it (lab spec: "unmask at task completion"). */
                uart_top_half(irq);
            } else if (irq != 0) {
                plic_complete(irq);
            }
        }

        task_run_pending();

        /*
         * Preemption gate (Lab5 Basic Ex2): if a timer tick set the
         * need_resched flag and we are returning to U-mode, yield to
         * the next runnable thread. We do this here (trap context)
         * rather than inside the IRQ handler so that switch_to() can
         * safely change kernel stacks. SPP=0 means the trap was
         * taken from U-mode, which is the only case where we want to
         * preempt — preempting kernel threads would break the lab's
         * cooperative semantics for kernel-only paths.
         */
        if (need_resched_clear()
            && (tf->sstatus & SSTATUS_SPP) == 0)
            schedule();

        /* Signal dispatch only applies to U-mode-bound returns. SPP=0
         * means the trap was taken from U and we will sret back to U
         * after this. */
        if ((tf->sstatus & SSTATUS_SPP) == 0)
            deliver_pending_signal(tf);
        return;
    }

    if (cause == EXC_ECALL_U) {
        /* Skip the ecall instruction (always 4 bytes in RV64I) BEFORE
         * dispatching: sys_fork() copies the current sepc verbatim
         * into the child's trap_frame, so it must already point at
         * the instruction after the ecall. */
        tf->sepc += 4;

        /* Re-enable interrupts during syscall processing so that
         * blocking calls (like sys_uart_read) can still receive
         * UART interrupts. */
        asm volatile ("csrs sstatus, %0" :: "r"((unsigned long)SSTATUS_SIE));

        tf->a0 = (uintptr_t)do_syscall(tf);

        if (need_resched_clear())
            schedule();

        /* ECALL_U always sret's back to U-mode; SPP is 0 here. Try to
         * deliver one pending signal before resuming user code. */
        deliver_pending_signal(tf);
        return;
    }

    /*
     * Display sepc as an offset from the user program base so that
     * students can correlate it with the prog.bin disassembly without
     * worrying about where kmalloc() happened to place the image.
     */
    uintptr_t rel_sepc = tf->sepc - g_user_base;

    uart_puts("=== S-Mode trap ===\n");
    uart_puts("scause: ");
    print_dec_ulong(cause);
    uart_puts("\n");
    uart_puts("sepc: 0x");
    print_hex_u32((unsigned int)rel_sepc);
    uart_puts("\n");
    uart_puts("stval: ");
    print_dec_ulong(tf->stval);
    uart_puts("\n");

    uart_puts("[trap] unhandled exception, halting.\n");
    for (;;) {
        asm volatile ("wfi");
    }
}

/** ----------------------------------------------------------------------
 * @brief kernel_trap_panic() – Fatal handler for S→S traps in Ex1.
 *
 * Basic Ex1 does not expect any trap to be taken while the kernel is
 * already running. If one occurs, the assembly trampoline jumps here
 * so that the operator sees a clear message before the hart spins.
 * -------------------------------------------------------------------- */
void kernel_trap_panic(void)
{
    uintptr_t scause, sepc, stval;
    asm volatile ("csrr %0, scause" : "=r"(scause));
    asm volatile ("csrr %0, sepc"   : "=r"(sepc));
    asm volatile ("csrr %0, stval"  : "=r"(stval));

    uart_puts("[trap] kernel-mode trap! scause=0x");
    print_hex_ulong(scause);
    uart_puts(" sepc=0x");
    print_hex_ulong(sepc);
    uart_puts(" stval=0x");
    print_hex_ulong(stval);
    uart_puts("\n");
}
