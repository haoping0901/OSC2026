#ifndef __SCHED_H__
#define __SCHED_H__

#include "list.h"
#include "types.h"

/*
 * Cooperative kernel-thread scheduler (Lab5 Basic Ex1).
 *
 * Each thread owns a private kernel stack and a context block holding
 * callee-saved registers plus ra/sp. switch_to() in switch.S persists
 * the prev thread's context and reloads the next thread's, then sets
 * tp = next so get_current() observes the switch.
 *
 * Scheduling is round-robin and strictly cooperative: a thread relays
 * the CPU only by calling schedule() or thread_exit(). Timer-driven
 * preemption is left for a later exercise.
 */

typedef enum {
    THREAD_READY = 0,
    THREAD_RUNNING,
    THREAD_ZOMBIE,
} thread_state_t;

/*
 * Saved register slots restored by switch_to(). Field order MUST stay
 * in sync with the offsets hard-coded in switch.S (0, 8, 16, ..., 104).
 * Caller-saved regs are not stored here because switch_to() is invoked
 * from C and the compiler has already spilled anything live across it.
 */
struct thread_ctx {
    unsigned long ra;
    unsigned long sp;
    unsigned long s0;
    unsigned long s1;
    unsigned long s2;
    unsigned long s3;
    unsigned long s4;
    unsigned long s5;
    unsigned long s6;
    unsigned long s7;
    unsigned long s8;
    unsigned long s9;
    unsigned long s10;
    unsigned long s11;
};

/*
 * The ctx member MUST be first: switch.S addresses it as 0(a0)/0(a1)
 * without an explicit offset. Do not reorder.
 */
struct thread {
    struct thread_ctx ctx;
    struct list_head  link;
    int               tid;
    thread_state_t    state;
    void            (*entry)(void);
    void             *kstack_base;
    unsigned long     kstack_size;
};

void  sched_init(void);
struct thread *thread_create(void (*fn)(void));
void  schedule(void);
void  thread_exit(void) __attribute__((noreturn));

/*
 * Read tp and reinterpret as the current thread pointer. Inlined here
 * because callers are tiny and a real call would clobber more regs
 * than the load itself.
 */
static inline struct thread *get_current(void)
{
    struct thread *t;
    asm volatile ("mv %0, tp" : "=r"(t));
    return t;
}

/* Implemented in switch.S. */
void switch_to(struct thread *prev, struct thread *next);

#endif /* __SCHED_H__ */
