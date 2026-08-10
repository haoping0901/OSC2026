#include "task.h"
#include "list.h"
#include "kmalloc.h"
#include "riscv.h"
#include "types.h"

/* Sentinel: "no task currently running on this stack". Any priority
 * value (smaller is higher) is strictly less than this, so it lets
 * the very first dispatch through. */
#define TASK_PRIO_IDLE   0x7FFFFFFF

struct task_node {
    struct list_head link;
    task_callback_t  cb;
    void            *arg;
    int              priority;
};

/* Sorted ascending by priority — head holds the highest-priority
 * pending task. Equal priorities are kept FIFO because the insertion
 * walk only stops on a strictly-greater value. */
static struct list_head g_task_queue = LIST_HEAD_INIT(g_task_queue);

/* Priority of the task currently executing on this kernel stack.
 * task_run_pending() consults this to decide whether a queue head
 * should preempt; it is strictly the priority of the *deepest* task
 * frame, because each nested call saves and restores it. */
static int g_running_priority = TASK_PRIO_IDLE;

/** ----------------------------------------------------------------------
 * @brief add_task() – Enqueue a callback for bottom-half execution.
 *
 * Allocates a node, inserts it into the priority-sorted queue, and
 * returns. Re-entrant against nested IRQs: the queue mutation runs
 * with sstatus.SIE cleared so a preempting handler cannot observe a
 * half-linked list. Same-priority entries keep FIFO order because the
 * walk stops only when it sees a strictly lower priority (= larger
 * value).
 * @param     cb       Function to invoke in bottom-half context.
 * @param[in] arg      Opaque pointer handed verbatim to @cb.
 * @param     priority Smaller value = higher priority.
 * @return 0 on success, -1 on allocation failure.
 * -------------------------------------------------------------------- */
int add_task(task_callback_t cb, void *arg, int priority)
{
    struct task_node *n = kmalloc(sizeof(*n));
    if (!n)
        return -1;

    n->cb       = cb;
    n->arg      = arg;
    n->priority = priority;

    unsigned long flags = sie_save_clear();

    struct list_head *p;
    for (p = g_task_queue.next; p != &g_task_queue; p = p->next) {
        struct task_node *t = list_entry(p, struct task_node, link);
        if (n->priority < t->priority)
            break;
    }
    n->link.prev = p->prev;
    n->link.next = p;
    p->prev->next = &n->link;
    p->prev = &n->link;

    sie_restore(flags);
    return 0;
}

/** ----------------------------------------------------------------------
 * @brief task_run_pending() – Drain the queue with preemption enabled.
 *
 * Loops: pick the queue head iff its priority is strictly higher than
 * the task currently running on this stack (TASK_PRIO_IDLE on the
 * outermost call), record it as g_running_priority, enable
 * sstatus.SIE, run the callback, then disable SIE and restore the
 * previous priority.
 *
 * Preemption: while a callback runs with SIE = 1, a device IRQ can
 * fire, its top half adds a higher-priority task, the trap return
 * path calls task_run_pending() recursively. The strict "<" check
 * here filters everything except the new high-priority task, which
 * runs to completion before sret returns to the preempted callback.
 * -------------------------------------------------------------------- */
void task_run_pending(void)
{
    for (;;) {
        unsigned long flags = sie_save_clear();

        struct task_node *t = NULL;
        if (!list_empty(&g_task_queue)) {
            struct task_node *head =
                list_entry(g_task_queue.next, struct task_node, link);
            if (head->priority < g_running_priority) {
                list_del(&head->link);
                t = head;
            }
        }

        if (!t) {
            sie_restore(flags);
            return;
        }

        int prev_prio = g_running_priority;
        g_running_priority = t->priority;

        /* Bottom half body runs with interrupts ENABLED regardless of
         * the caller's prior state, so a higher-priority device can
         * preempt via the trap path. We don't sie_restore(flags) here
         * because BH semantics require SIE=1 unconditionally. */
        asm volatile ("csrs sstatus, %0"
                      :: "r"((unsigned long)SSTATUS_SIE));

        t->cb(t->arg);

        /* Close the critical section before mutating shared state. */
        unsigned long f2 = sie_save_clear();
        g_running_priority = prev_prio;
        kfree(t);
        sie_restore(f2);
    }
}
