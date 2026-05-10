#ifndef __SCHED_H__
#define __SCHED_H__

#include "list.h"
#include "types.h"

/*
 * Cooperative kernel-thread scheduler (Lab5 Basic Ex1) extended with
 * user-process abstractions for Lab5 Basic Ex2.
 *
 * Each thread owns a private kernel stack and a context block holding
 * callee-saved registers plus ra/sp. switch_to() in switch.S persists
 * the prev thread's context and reloads the next thread's, then sets
 * tp = next so get_current() observes the switch.
 *
 * For Ex2 a thread is also the unit of process: pid == tid, the
 * parent/children/sibling links form the process tree, and
 * image_base/total_size describe the contiguous user-mode image+stack
 * buffer that thread_spawn_user() / sys_fork() allocate.
 */

typedef enum {
    THREAD_READY = 0,
    THREAD_RUNNING,
    THREAD_BLOCKED,    /* waitpid sleeping; not on runq */
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
    struct list_head  link;             /* node in runq / g_zombies */
    int               tid;
    int               pid;              /* alias of tid; named for syscalls */
    thread_state_t    state;
    void            (*entry)(void);     /* kernel-thread entry; NULL for user */
    void             *kstack_base;
    unsigned long     kstack_size;

    /* ------------- process fields (zero for kernel-only threads) ------- */
    struct thread    *parent;
    struct list_head  children;         /* head; nodes are children's sibling */
    struct list_head  sibling;          /* node in parent->children */
    int               exit_status;

    /*
     * User memory: a single contiguous kmalloc'd buffer. Layout:
     *   [image_base, image_base + image_size)         = code+data+bss
     *   [image_base + image_size, image_base+total_size) = user stack
     * total_size = image_size + USER_STACK_SIZE. NULL for kernel-only
     * threads (bootstrap, idle, and any future kernel worker).
     */
    void             *image_base;
    unsigned long     image_size;
    unsigned long     total_size;
};

/* User stack size attached after every user image. */
#define USER_STACK_SIZE   (16 * 1024)

void  sched_init(void);
struct thread *thread_create(void (*fn)(void));
void  schedule(void);
void  thread_exit(void) __attribute__((noreturn));

/*
 * Allocate a thread struct + kernel stack but skip trampoline planting
 * and runq insertion. Caller is responsible for filling ctx.{sp,ra}
 * (typically pointing at a trap_frame planted on the kstack) and
 * eventually calling thread_enqueue_ready().
 */
struct thread *thread_alloc_bare(void);

/* Mark @t READY and append to the run queue under SIE-clear. */
void thread_enqueue_ready(struct thread *t);

/* Current thread → BLOCKED, then schedule(). Returns when someone
 * later calls thread_wakeup() on it. */
void thread_block(void);

/* Move @t from BLOCKED → READY and re-enqueue, no-op otherwise. */
void thread_wakeup(struct thread *t);

/* Spawn the first user process from a file in initrd. Returns the new
 * thread or NULL on lookup / OOM failure. */
struct thread *thread_spawn_user(const char *path);

/* Park @t on the zombie list so the idle thread's reaper frees its
 * kstack and struct. Caller MUST have detached @t from any other list
 * (runq, parent->children) and set state = THREAD_ZOMBIE first. */
void sched_zombify(struct thread *t);

/* Walk the process tree starting from g_bootstrap and return the
 * thread whose pid matches, or NULL. O(N) — fine for lab scope. */
struct thread *find_thread_by_pid(int pid);

/* Bootstrap thread (= shell), exposed so sys_exit can reparent
 * orphaned children and shell can be queried as parent. */
extern struct thread g_bootstrap;

/* ---------- preemption flag -------------------------------------------- */

/*
 * Set by timer_top_half(); consumed by trap_handler() right before it
 * returns to U-mode. The actual schedule() runs in trap context (not
 * IRQ context) so we can switch kernel stacks safely.
 */
void  set_need_resched(void);
int   need_resched_clear(void);

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
