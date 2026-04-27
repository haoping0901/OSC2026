# Trace Result - 2026-04-27

## 1.  

```c 70:70:kernel/src/trap.c
timer_top_half();
```

### 1.  

```c 153:153:kernel/src/timer.c
void timer_top_half(void)
```

### 2. 不斷移除已過期的 timer，並將過期的 timer 的 bottom half 加到 task list 後，free timer node。 

```c 157:169:kernel/src/timer.c
while (!list_empty(&g_timer_queue)) {
	struct timer_node *t = list_entry(g_timer_queue.next,
	                                  struct timer_node, link);
	if (t->expire_tick > now)
		break;

	list_del(&t->link);
	if (add_task(t->cb, t->arg, TIMER_TASK_PRIO) != 0) {
		/* OOM fallback: keep semantics over preemption. */
		t->cb(t->arg);
	}
	kfree(t);
}
```

## 2.  

```c 77:77:kernel/src/trap.c
uart_top_half(irq);
```

### 1.  

```c 243:243:kernel/src/uart.c
void uart_top_half(unsigned int irq)
```

### 2. 讀 RX FIFO 

```c 245:248:kernel/src/uart.c
while (mmio_read(UART_BASE + UART_LSR) & LSR_DR) {
    unsigned char ch = mmio_read(UART_BASE + UART_RBR) & 0xFF;
    ring_push(&rx_buf, ch);
}
```

### 3. 放 TX FIFO 

```c 250:254:kernel/src/uart.c
while (!ring_empty(&tx_buf) &&
       (mmio_read(UART_BASE + UART_LSR) & LSR_TDRQ)) {
    int c = ring_pop(&tx_buf);
    mmio_write(UART_BASE + UART_THR, (unsigned char)c);
}
```

### 4. 沒有要傳資料出去時，關閉 TX 中斷。 

```c 256:260:kernel/src/uart.c
if (ring_empty(&tx_buf)) {
    unsigned int ier = mmio_read(UART_BASE + UART_IER);
    if (ier & IER_TIE)
        mmio_write(UART_BASE + UART_IER, ier & ~IER_TIE);
}
```

### 5. %%Tip%% 把 uart_bottom_half 排進 g_task_queue，priority = UART_TASK_PRIO (1)。 

```c 262:266:kernel/src/uart.c
if (add_task(uart_bottom_half, (void *)(uintptr_t)irq,
             UART_TASK_PRIO) != 0) {
    /* OOM: skip the BH and complete here so the line unmasks. */
    plic_complete(irq);
}
```

#### 1. add_task(uart_bottom_half, (void *)(uintptr_t)irq, UART_TASK_PRIO)。 

```c 44:44:kernel/src/task.c
int add_task(task_callback_t cb, void *arg, int priority)
```

##### 1. 初始化 task node。 

```c 46:52:kernel/src/task.c
struct task_node *n = kmalloc(sizeof(*n));
if (!n)
    return -1;

n->cb       = cb;
n->arg      = arg;
n->priority = priority;
```

##### 2. 確保 S-mode 中斷不會在新增 task 的時候進來，導致 list 設置有誤。 

```c 54:54:kernel/src/task.c
unsigned long flags = sie_save_clear();
```

##### 3. 找到第一個優先級比新增的 task 還大的 task。 

```c 57:61:kernel/src/task.c
for (p = g_task_queue.next; p != &g_task_queue; p = p->next) {
    struct task_node *t = list_entry(p, struct task_node, link);
    if (n->priority < t->priority)
        break;
}
```

##### 4. 將新 task 插入第一個優先級比它大的 task 前面。 

```c 62:65:kernel/src/task.c
n->link.prev = p->prev;
n->link.next = p;
p->prev->next = &n->link;
p->prev = &n->link;
```

##### 5. 恢復中斷狀態。 

```c 67:67:kernel/src/task.c
sie_restore(flags);
```

#### 2.  

```c 278:278:kernel/src/uart.c
void uart_bottom_half(void *arg)
```

##### 1. 在 bottom half 結尾做 complete，開啟 PLIC 特定 IRQ 的 mask。 

```c 281:281:kernel/src/uart.c
plic_complete(irq);
```

## 3.  

```c 83:83:kernel/src/trap.c
task_run_pending();
```

### 1.  

```c 86:86:kernel/src/task.c
void task_run_pending(void)
```

### 2. 關 SIE 

```c 89:89:kernel/src/task.c
unsigned long flags = sie_save_clear();
```

### 3. 取出 priority 比當前正在跑得 task 還小的 task。 

```c 91:99:kernel/src/task.c
struct task_node *t = NULL;
if (!list_empty(&g_task_queue)) {
    struct task_node *head =
        list_entry(g_task_queue.next, struct task_node, link);
    if (head->priority < g_running_priority) {
        list_del(&head->link);
        t = head;
    }
}
```

### 4. 紀錄接下來要被執行的 task 的 priority。 

```c 106:114:kernel/src/task.c
int prev_prio = g_running_priority;
g_running_priority = t->priority;

/* Bottom half body runs with interrupts ENABLED regardless of
 * the caller's prior state, so a higher-priority device can
 * preempt via the trap path. We don't sie_restore(flags) here
 * because BH semantics require SIE=1 unconditionally. */
asm volatile ("csrs sstatus, %0"
              :: "r"((unsigned long)SSTATUS_SIE));
```

### 5. 執行 bottom half handler。 

```c 116:116:kernel/src/task.c
t->cb(t->arg);
```

### 6. 於關閉中斷的區間中還原上次執行的 task 的優先級，與 free task node。 

```c 119:122:kernel/src/task.c
unsigned long f2 = sie_save_clear();
g_running_priority = prev_prio;
kfree(t);
sie_restore(f2);
```

