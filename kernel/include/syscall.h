#ifndef __SYSCALL_H__
#define __SYSCALL_H__

#include "trap.h"

/*
 * RISC-V Linux-style syscall ABI used by Lab5 Basic Ex2.
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

#endif /* __SYSCALL_H__ */
