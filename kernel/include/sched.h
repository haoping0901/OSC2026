#ifndef __SCHED_H__
#define __SCHED_H__

#include "list.h"
#include "types.h"
#include "signal.h"

/*
 * Cooperative kernel-thread scheduler extended with user-process
 * abstractions.
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

    /* ---------------- Per-process address space ----------------------- */

    /*
     * Root page table (kernel VA) of this process's private Sv39 address
     * space. Its high half is shared with the kernel PGD; its low half
     * maps the user image at USER_CODE_VA and the user stack below
     * USER_STACK_TOP. NULL for kernel-only threads, which run on the
     * kernel PGD (kernel_pgd()).
     */
    unsigned long    *pgd;

    /*
     * Backing physical frames for the user image and stack, each held by
     * its contiguous-block kernel VA (buddy_alloc'd):
     *   image_base       = kernel VA of the image frame block; the user
     *                      sees it at USER_CODE_VA. Used for memcpy on
     *                      load/fork and for a single buddy_free on exit.
     *   image_size       = bytes of program copied in.
     *   image_pages      = bytes mapped for the image (page-rounded).
     *   user_stack_base  = kernel VA of the stack frame block; the user
     *                      sees its top at USER_STACK_TOP.
     *   user_stack_size  = bytes mapped for the user stack.
     * All NULL/0 for kernel-only threads.
     */
    void             *image_base;
    unsigned long     image_size;
    unsigned long     image_pages;
    void             *user_stack_base;
    unsigned long     user_stack_size;

    /*
     * Kernel VA backing the per-process signal page mapped at SIGPAGE_VA
     * (PROT_USER_RWX). Holds the sigreturn trampoline (page base) plus the
     * U-mode handler stack. buddy_alloc'd in uvm_setup_image()/sys_fork(),
     * reclaimed by thread_free_user_vm() at reap time. NULL for kernel-only
     * threads.
     */
    void             *sigpage_base;

    /* Per-thread POSIX signal state. Untouched for kernel-only threads
     * (their pending bitmap stays 0 so signal_check_and_dispatch is a
     * no-op even if it is ever reached on a non-user path). */
    struct signal_state sig;

    /* ---------------- mmap regions ------------------------------------ */

    /*
     * All user virtual-memory areas of this process (image, stack, signal
     * page, and every mmap()'d region), kept va-ascending. Replaces the
     * ad-hoc image_base/user_stack_base bookkeeping for *enumeration* and
     * teardown; those fields are retained for fast image memcpy on fork.
     */
    struct list_head vma_list;

    /*
     * Top-down cursor for addr==NULL mmap placement. Initialised below the
     * stack region; each anonymous mapping is carved downward from here so
     * mmap regions never collide with the fixed image (low) / stack (high).
     */
    unsigned long mmap_top;
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

/*
 * Build @t's user address space for a freshly loaded image: allocate
 * image + stack frame blocks, copy @sz bytes of program from @src into
 * the image block, and map both at the fixed user VAs (image at
 * USER_CODE_VA, stack below USER_STACK_TOP) in @t->pgd. @t->pgd must
 * already be a valid user PGD (pgd_alloc()). On success fills
 * image_base/image_size/image_pages/user_stack_base/user_stack_size and
 * returns 0; on OOM frees whatever it allocated and returns -1.
 * Shared by thread_spawn_user() and sys_exec().
 */
int uvm_setup_image(struct thread *t, const void *src, unsigned long sz);

/*
 * Reclaim @t's user image/stack frames and page tables immediately.
 * Used by sys_exec() to drop the OLD address space AFTER satp has been
 * switched to the new one. Do NOT call on a thread whose PGD is the
 * live satp root.
 */
void thread_free_user_vm(struct thread *t);

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
