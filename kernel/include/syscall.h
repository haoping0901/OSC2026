#ifndef __SYSCALL_H__
#define __SYSCALL_H__

#include "trap.h"

/*
 * RISC-V Linux-style syscall ABI.
 *   a7        : syscall number
 *   a0..a2    : arguments (only the first three are used by these calls)
 *   a0 (out)  : return value, written back into the trap frame by trap.c
 */
#define SYS_GETPID      0
#define SYS_UART_READ   1
#define SYS_UART_WRITE  2
#define SYS_EXEC        3
#define SYS_FORK        4
#define SYS_WAITPID     5
#define SYS_EXIT        6
#define SYS_STOP        7
#define SYS_DISPLAY     8
#define SYS_USLEEP      9
#define SYS_SIGNAL      10
#define SYS_SIGRETURN   11
#define SYS_KILL        12
#define SYS_MMAP        13

/** ----------------------------------------------------------------------
 * @brief do_syscall() – Top-level dispatcher for U-mode ecalls.
 *
 * Invoked by trap_handler() once it has advanced sepc past the ecall
 * instruction. Reads the call number from tf->a7 and forwards the
 * arguments in tf->a0..a2 to the corresponding handler. The returned
 * value is written into tf->a0 by the caller.
 * @param tf Trap frame on the kernel stack.
 * @return Value the syscall returned to user space (placed in a0).
 * -------------------------------------------------------------------- */
long do_syscall(struct trap_frame *tf);

/** ----------------------------------------------------------------------
 * @brief do_exit() – Terminate the current process (never returns).
 *
 * Core of SYS_EXIT, exported so the page-fault handler can kill a
 * process on segmentation fault through the exact same teardown path
 * (reparent children, mark ZOMBIE, release signal state, wake parent,
 * schedule away). The user VM is reclaimed later by the reaper, once
 * satp has switched off the dying PGD.
 * @param status Exit status returned to the parent's waitpid.
 * -------------------------------------------------------------------- */
void do_exit(long status) __attribute__((noreturn));

#endif /* __SYSCALL_H__ */
