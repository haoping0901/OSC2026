# Trace Result - 2026-05-06

## 1. 把 main() 當下執行中的 boot context 認養成第一條 thread 

```c 174:174:kernel/src/main.c
sched_init();
```

### 1.  

```c 233:233:kernel/src/sched.c
void sched_init(void)
```

### 2. 從 0 開始給 tid。bootstrap thread 拿到 tid=0，下一個 idle thread 拿 1。 

```c 235:235:kernel/src/sched.c
g_bootstrap.tid         = g_next_tid++;
```

### 3. bootstrap thread 不會經過 thread_entry_trampoline（它已經在執行了，不是「第一次被 dispatch」），所以 entry 永遠不會被讀。設 NULL 是防呆。 

```c 237:237:kernel/src/sched.c
g_bootstrap.entry       = NULL;
```

### 4. %%Tip%% 設定 thread pointer 為目前正在執行的 bootstrap thread 。 

```c 245:245:kernel/src/sched.c
asm volatile ("mv tp, %0" :: "r"(&g_bootstrap));
```

### 5. %%Tip%% 會配置一個新的 thread 結構（含 stack、context 等），並把它加入 scheduler 的 run queue。 

idle_thread_body 是這個 thread 要執行的函式。

```c 247:247:kernel/src/sched.c
thread_create(idle_thread_body);
```

#### 1. thread_create(idle_thread_body); 

```c 63:63:kernel/src/sched.c
struct thread *thread_create(void (*fn)(void))
```

#### 2. 初始化 thread 控制區塊。 

```c 75:87:kernel/src/sched.c
t->tid         = g_next_tid++;
t->state       = THREAD_READY;
t->entry       = fn;
t->kstack_base = stack;
t->kstack_size = KSTACK_SIZE;
INIT_LIST_HEAD(&t->link);

/* Zero all callee-saved slots; the trampoline does not read any of
 * them, but a deterministic initial state simplifies debugging if
 * the first switch ever lands somewhere unexpected. */
for (unsigned i = 0;
     i < sizeof(t->ctx) / sizeof(unsigned long); i++)
    ((unsigned long *)&t->ctx)[i] = 0;
```

#### 3. %%Tip%% sp 設成 stack buffer 的頂端，並對齊 16 bytes（RISC-V psABI 規定）。 

ra（return address）設成 thread_entry_trampoline：因為 switch_to 結束時是用 ret 指令返回，ret 會跳到 ra。所以新 thread 第一次「醒來」時，自然就會跳進 trampoline，再由 trampoline 呼叫 fn。

```c 89:95:kernel/src/sched.c
/* Stack grows down. Round the top to a 16-byte boundary as
 * required by the RISC-V psABI before storing it in ctx.sp. */
unsigned long top = (unsigned long)stack + KSTACK_SIZE;
top &= ~0xFUL;

t->ctx.ra = (unsigned long)thread_entry_trampoline;
t->ctx.sp = top;
```

##### 1.  

```c 44:44:kernel/src/sched.c
static void thread_entry_trampoline(void)
```

##### 2. %%Tip%% thread 第一次被排程執行時，銜接 context switch 機制和使用者提供的 thread entry functio 

```c 48:48:kernel/src/sched.c
thread_exit();
```

##### 3. thread 執行完任務後，標記自己為 zombie thread，使後續清理的 thread 來 free 自己。 

```c 161:161:kernel/src/sched.c
void thread_exit(void)
```

#### 4. 加入 run queue（critical section 保護） 

```c 97:99:kernel/src/sched.c
unsigned long flags = sie_save_clear();
list_add_tail(&t->link, &g_runq);
sie_restore(flags);
```

### 6. %%Tip%%  

```c 31:31:kernel/src/sched.c
static void idle_thread_body(void)
```

#### 1.  

```c 217:217:kernel/src/sched.c
kill_zombies();
```

##### 1.  

```c 185:185:kernel/src/sched.c
static void kill_zombies(void)
```

##### 2. 在 critical section 取出 zombie thread。 

```c 188:196:kernel/src/sched.c
unsigned long flags = sie_save_clear();
if (list_empty(&g_zombies)) {
    sie_restore(flags);
    return;
}
struct thread *z = list_entry(g_zombies.next,
                              struct thread, link);
list_del(&z->link);
sie_restore(flags);
```

##### 3. Free zombie thread stack 跟 zombie thread。 

```c 198:201:kernel/src/sched.c
if (z->kstack_base)
    kfree(z->kstack_base);
if (z != &g_bootstrap)
    kfree(z);
```

#### 2.  

```c 218:218:kernel/src/sched.c
schedule();
```

##### 1.  

```c 121:121:kernel/src/sched.c
void schedule(void)
```

##### 2. get_current() 透過 tp 暫存器取得目前 thread。然後只有 state 仍是 RUNNING 才放回 run queue。 

```c 125:130:kernel/src/sched.c
struct thread *prev = get_current();

if (prev->state == THREAD_RUNNING) {
    prev->state = THREAD_READY;
    list_add_tail(&prev->link, &g_runq);
}
```

##### 3. 處理 run queue 為空的 corner case。 

如果 run queue 空了，沒人可以接手，只能讓 prev 繼續跑。

```c 132:138:kernel/src/sched.c
if (list_empty(&g_runq)) {
    /* Nobody else is ready — keep prev on the CPU. Restore the
     * RUNNING state we may have just cleared. */
    prev->state = THREAD_RUNNING;
    sie_restore(flags);
    return;
}
```

##### 4. 挑出下一個 thread 並更新 state 

```c 140:142:kernel/src/sched.c
struct thread *next = list_entry(g_runq.next, struct thread, link);
list_del(&next->link);
next->state = THREAD_RUNNING;
```

##### 5. %%Tip%% 切換 thread。 

```c 146:147:kernel/src/sched.c
if (prev != next)
    switch_to(prev, next);
```

###### 1. switch_to(prev, next); 

```plaintext 37:37:kernel/src/switch.S
switch_to:
```

###### 2. 儲存上一個 thread 自己的 s0-s11，ra，sp 暫存器狀態。 

```plaintext 40:53:kernel/src/switch.S
sd  ra,    0(a0)
sd  sp,    8(a0)
sd  s0,   16(a0)
sd  s1,   24(a0)
sd  s2,   32(a0)
sd  s3,   40(a0)
sd  s4,   48(a0)
sd  s5,   56(a0)
sd  s6,   64(a0)
sd  s7,   72(a0)
sd  s8,   80(a0)
sd  s9,   88(a0)
sd  s10,  96(a0)
sd  s11, 104(a0)
```

###### 3. 載入 next 上次離開時的 ra 

```plaintext 56:56:kernel/src/switch.S
ld  ra,    0(a1)
```

###### 4. 切換 stack！這之後我們站在 next 的 stack 上 

```plaintext 57:57:kernel/src/switch.S
ld  sp,    8(a1)
```

###### 5. 替換存放目前正在跑的 thread struct 位址的暫存器為要 switch to 的 thread struct。 

```plaintext 73:73:kernel/src/switch.S
mv  tp, a1
```

###### 6. 跳到 ra 

```plaintext 74:74:kernel/src/switch.S
ret
```

