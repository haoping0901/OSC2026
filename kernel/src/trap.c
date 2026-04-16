#include "trap.h"
#include "riscv.h"
#include "uart.h"
#include "utils.h"
#include "types.h"

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

    /* Ex1 does not enable any S-mode interrupts; ignore if one slips in. */
    if (cause & SCAUSE_INTR_BIT) {
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

    if (cause == EXC_ECALL_U) {
        /* Skip the ecall instruction (always 4 bytes in RV64I). */
        tf->sepc += 4;
        return;
    }

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
