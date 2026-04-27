#ifndef __TASK_H__
#define __TASK_H__

#include "types.h"

/*
 * Bottom-half task queue (Lab4 Advanced Ex2).
 *
 * Device interrupt handlers are split into a top half (run in IRQ
 * context with sstatus.SIE = 0; minimal work, then add_task()) and a
 * bottom half (run by task_run_pending() at trap return with SIE = 1
 * so a higher-priority IRQ can preempt). Smaller priority value =
 * higher priority; equal priorities preserve FIFO order.
 */

typedef void (*task_callback_t)(void *arg);

/* Named priority slots used by device drivers. Keep in sync with
 * uart.c / timer.c: smaller is higher priority. Shell demos should
 * stay >= DEFAULT_TASK_PRIO so device IRQs can preempt them. */
#define UART_TASK_PRIO     1
#define TIMER_TASK_PRIO    2
#define DEFAULT_TASK_PRIO  5

/* Returns 0 on success, -1 if kmalloc failed. Callers that must not
 * lose the work (e.g. UART top half deferring plic_complete) check
 * this and fall back to a synchronous path. */
int add_task(task_callback_t cb, void *arg, int priority);

/* Drain ready tasks; called at trap return and (optionally) from
 * thread context after enqueuing. Re-entrant: a nested call from a
 * preempted-by-IRQ bottom half is the mechanism that implements
 * preemption — only tasks strictly higher-priority than the one
 * already running on this stack are dispatched. */
void task_run_pending(void);

#endif /* __TASK_H__ */
