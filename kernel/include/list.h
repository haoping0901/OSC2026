#ifndef __LIST_H__
#define __LIST_H__

/*
 * Generic circular doubly-linked list, inspired by Linux kernel <linux/list.h>.
 *
 * Embed a `struct list_head` inside your data structure, then use
 * list_entry() to recover the outer struct from a list pointer.
 */

struct list_head {
    struct list_head *prev;
    struct list_head *next;
};

/* ---- Initialisation ---------------------------------------------------- */

#define LIST_HEAD_INIT(name) { &(name), &(name) }

static inline void INIT_LIST_HEAD(struct list_head *head)
{
    head->next = head;
    head->prev = head;
}

/* ---- Core helpers (internal) ------------------------------------------- */

static inline void __list_add(struct list_head *node,
                              struct list_head *prev,
                              struct list_head *next)
{
    next->prev = node;
    node->next = next;
    node->prev = prev;
    prev->next = node;
}

static inline void __list_del(struct list_head *prev,
                              struct list_head *next)
{
    next->prev = prev;
    prev->next = next;
}

/* ---- Public API -------------------------------------------------------- */

/** Insert @node immediately after @head (stack-like / LIFO). */
static inline void list_add(struct list_head *node, struct list_head *head)
{
    __list_add(node, head, head->next);
}

/** Insert @node immediately before @head (queue-like / FIFO). */
static inline void list_add_tail(struct list_head *node, struct list_head *head)
{
    __list_add(node, head->prev, head);
}

/** Remove @node from whatever list it belongs to. */
static inline void list_del(struct list_head *node)
{
    __list_del(node->prev, node->next);
    node->next = node;
    node->prev = node;
}

/** Return non-zero if the list is empty. */
static inline int list_empty(const struct list_head *head)
{
    return head->next == head;
}

/* ---- Container-of helpers ---------------------------------------------- */

/**
 * Obtain the address of the struct that contains this list_head member.
 *
 *   list_entry(ptr, type, member)
 *
 * @ptr    – pointer to the struct list_head inside the container
 * @type   – type of the container struct
 * @member – name of the list_head field inside the container
 */
#define offsetof(type, member) \
    ((unsigned long)&((type *)0)->member)

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#define list_entry(ptr, type, member) \
    container_of(ptr, type, member)

/**
 * Iterate over a list.
 *   list_for_each(pos, head)
 */
#define list_for_each(pos, head) \
    for (pos = (head)->next; pos != (head); pos = pos->next)

#endif /* __LIST_H__ */
