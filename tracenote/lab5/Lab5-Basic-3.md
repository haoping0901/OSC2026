# Trace Result - 2026-05-10

## 1. 把 main() 當下執行中的 boot context 認養成第一條 thread 

```c 184:184:kernel/src/main.c
sched_init();
```

### 1.  

```c 351:351:kernel/src/sched.c
void sched_init(void)
```

### 2. 從 0 開始給 tid。bootstrap thread 拿到 tid=0，下一個 idle thread 拿 1。 

```c 354:354:kernel/src/sched.c
g_bootstrap.tid         = g_next_tid++;
```

### 3. bootstrap thread 不會經過 thread_entry_trampoline（它已經在執行了，不是「第一次被 dispatch」），所以 entry 永遠不會被讀。設 NULL 是防呆。 

```c 357:357:kernel/src/sched.c
g_bootstrap.entry       = NULL;
```

### 4. 設定 thread pointer 為目前正在執行的 bootstrap thread 。 

```c 365:365:kernel/src/sched.c
asm volatile ("mv tp, %0" :: "r"(&g_bootstrap));
```

### 5. 會配置一個新的 thread 結構（TCB，含 stack、context 等），並把它加入 scheduler 的 run queue。 

idle_thread_body 是這個 thread 要執行的函式。

```c 367:367:kernel/src/sched.c
thread_create(idle_thread_body);
```

#### 1. thread_create(idle_thread_body); 

```c 95:95:kernel/src/sched.c
struct thread *thread_create(void (*fn)(void))
```

#### 2. 初始化 TCB。 

```c 97:97:kernel/src/sched.c
struct thread *t = thread_alloc_bare();
```

#### 3. %%Tip%% sp 設成 stack buffer 的頂端，並對齊 16 bytes（RISC-V psABI 規定）。 

ra（return address）設成 thread_entry_trampoline：因為 switch_to 結束時是用 ret 指令返回，ret 會跳到 ra。所以新 thread 第一次「醒來」時，自然就會跳進 trampoline，再由 trampoline 呼叫 fn。

```c 105:109:kernel/src/sched.c
/* Stack grows down. Round the top to a 16-byte boundary as
 * required by the RISC-V psABI before storing it in ctx.sp. */
unsigned long top = (unsigned long)stack + KSTACK_SIZE;
top &= ~0xFUL;

t->ctx.ra = (unsigned long)thread_entry_trampoline;
t->ctx.sp = top;
```

##### 1.  

```c 79:79:kernel/src/sched.c
static void thread_entry_trampoline(void)
```

##### 2. %%Tip%% thread 第一次被排程執行時，銜接 context switch 機制和使用者提供的 thread entry functio 

```c 83:83:kernel/src/sched.c
thread_exit();
```

##### 3. thread 執行完任務後，標記自己為 zombie thread，使後續清理的 thread 來 free 自己。 

```c 277:277:kernel/src/sched.c
void thread_exit(void)
```

##### 4. 把自己加到 zombie queue。 

```c 288:288:kernel/src/sched.c
sched_zombify(self);
```

##### 5.  

```c 217:217:kernel/src/sched.c
void sched_zombify(struct thread *t)
```

#### 4. 設定 parent 加入 run queue（critical section 保護） 

```c 115:119:kernel/src/sched.c
unsigned long flags = sie_save_clear();
if (t->parent)
    list_add_tail(&t->sibling, &t->parent->children);
list_add_tail(&t->link, &g_runq);
sie_restore(flags);
```

### 6. %%Tip%%  

```c 332:332:kernel/src/sched.c
static void idle_thread_body(void)
```

#### 1. 清理 zombie queue 

```c 335:335:kernel/src/sched.c
kill_zombies();
```

##### 1.  

```c 305:305:kernel/src/sched.c
static void kill_zombies(void)
```

##### 2. 在 critical section 取出 zombie thread。 

```c 308:316:kernel/src/sched.c
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

```c 318:321:kernel/src/sched.c
if (z->kstack_base)
    kfree(z->kstack_base);
if (z != &g_bootstrap)
    kfree(z);
```

#### 2.  

```c 187:187:kernel/src/sched.c
schedule();
```

##### 1.  

```c 236:236:kernel/src/sched.c
void schedule(void)
```

##### 2. get_current() 透過 tp 暫存器取得目前 thread。然後只有 state 仍是 RUNNING 才放回 run queue。 

```c 240:245:kernel/src/sched.c
struct thread *prev = get_current();

if (prev->state == THREAD_RUNNING) {
    prev->state = THREAD_READY;
    list_add_tail(&prev->link, &g_runq);
}
```

##### 3. 處理 run queue 為空的 corner case。 

如果 run queue 空了，沒人可以接手，只能讓 prev 繼續跑。

```c 253:257:kernel/src/sched.c
if (list_empty(&g_runq)) {
    /* Nobody else is ready — keep prev on the CPU. Restore the
     * RUNNING state we may have just cleared. */
    prev->state = THREAD_RUNNING;
    sie_restore(flags);
    return;
}
```

##### 4. 挑出下一個 thread 並更新 state 

```c 259:261:kernel/src/sched.c
struct thread *next = list_entry(g_runq.next, struct thread, link);
list_del(&next->link);
next->state = THREAD_RUNNING;
```

##### 5. %%Tip%% 切換 thread。 

```c 80:81:bear_OSC_2026/kernel/src/thread.c
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

## 2. 為 CPIO 中的 name file 產生一個 thread，並加到 run queue 中。 

```c 455:455:kernel/src/shell.c
run_user_program(name);
```

### 1.  

```c 210:210:kernel/src/shell.c
static void run_user_program(const char *name)
```

### 2. 產生一個執行給定 cpio file 的 thread，並加到 run queue。 

```c 212:212:kernel/src/shell.c
struct thread *t = thread_spawn_user(name);
```

#### 1.  

```c 429:429:kernel/src/sched.c
struct thread *thread_spawn_user(const char *path)
```

#### 2. 找 cpio 

```c 431:432:kernel/src/sched.c
const void *initrd = (const void *)
    dtb_getprop("/chosen", "linux,initrd-start");
```

#### 3. 找檔案 

```c 438:438:kernel/src/sched.c
if (cpio_find(initrd, path, &src, &sz) != 0)
```

#### 4. 配置且初始化 thread struct 與配置 kernel stack。 

```c 441:447:kernel/src/sched.c
struct thread *t = thread_alloc_bare();
if (!t)
    return NULL;

t->image_size = sz;
t->total_size = sz + USER_STACK_SIZE;
t->image_base = kmalloc(t->total_size);
```

##### 1.  

```c 134:134:kernel/src/sched.c
struct thread *thread_alloc_bare(void)
```

#### 5. 複製 image 到 thread struct。 

```c 453:453:kernel/src/sched.c
mem_cpy(t->image_base, src, sz);
```

#### 6. 紀錄 parent thread 

```c 151:151:kernel/src/sched.c
t->parent = get_current();
```

#### 7. %%Tip%% 在 kernel stack top 放 trap frame 

```c 460:460:kernel/src/sched.c
struct trap_frame *tf = plant_initial_frame(t, entry, user_sp);
```

##### 1. plant_initial_frame(t, entry, user_sp); 

```c 387:388:kernel/src/sched.c
static struct trap_frame *
plant_initial_frame(struct thread *t, uintptr_t entry, uintptr_t user_sp)
```

##### 2. 設定 sret 時，執行的程式 

```c 396:396:kernel/src/sched.c
tf->sepc    = entry;
```

##### 3. 紀錄 image stack 的位置。 

```c 397:397:kernel/src/sched.c
tf->sp      = user_sp;
```

##### 4. 設定 thread pointer 

```c 398:398:kernel/src/sched.c
tf->tp      = (uintptr_t)t;
```

#### 8. 紀錄自己 

```c 462:462:kernel/src/sched.c
t->ctx.sp = (unsigned long)tf;
```

#### 9. 設定下一次 schedule 到這個 thread 時，用來 return U-mode 的 trap_return_user function。 

```c 463:463:kernel/src/sched.c
t->ctx.ra = (unsigned long)user_thread_bootstrap;
```

##### 1.  

```plaintext 302:302:kernel/src/trap_entry.S
user_thread_bootstrap:
```

##### 2.  

```plaintext 304:304:kernel/src/trap_entry.S
j     trap_return_user
```

##### 3.  

```plaintext 99:99:kernel/src/trap_entry.S
trap_return_user:
```

#### 10.  

```c 465:465:kernel/src/sched.c
trap_set_user_base(entry);
```

#### 11. 將剛建立的 thread 新增到 parent thread 的 child thread 

```c 468:469:kernel/src/sched.c
if (t->parent)
    list_add_tail(&t->sibling, &t->parent->children);
```

#### 12. 將剛建立的 thread 新增到 run queue。 

```c 472:472:kernel/src/sched.c
thread_enqueue_ready(t);
```

### 3. 等待剛剛執行的 file 結束 

```c 221:222:kernel/src/shell.c
while (t->state != THREAD_ZOMBIE)
    thread_block();
```

#### 1.  

```c 182:182:kernel/src/sched.c
void thread_block(void)
```

#### 2. 執行 run queue 中的第一個 thread。 

```c 187:187:kernel/src/sched.c
schedule();
```

##### 1.  

```c 236:236:kernel/src/sched.c
void schedule(void)
```

##### 2. 跳去下一個 thread。跳轉的同時編譯器會自動幫忙儲存 caller-saved register，切換時只會儲存 callee-saved register。 

```c 266:266:kernel/src/sched.c
switch_to(prev, next);
```

###### 1. switch_to(prev, next); 

```plaintext 35:35:kernel/src/switch.S
.globl switch_to
```

### 4. 把結束的 cpio file thread 加到 zombie queue。 

```c 228:228:kernel/src/shell.c
sched_zombify(t);
```

## 3. 停止指定 pid 

```c 496:496:kernel/src/shell.c
shell_stop_pid(cmd + 5);
```

### 1.  

```c 238:238:kernel/src/shell.c
static void shell_stop_pid(const char *args)
```

### 2. 取出 PID 

```c 245:249:kernel/src/shell.c
while (*p >= '0' && *p <= '9') {
    pid = pid * 10 + (*p - '0');
    p++;
    digits++;
}
```

### 3. 找到 PID 對應的 TCB 

```c 255:255:kernel/src/shell.c
struct thread *t = find_thread_by_pid(pid);
```

### 4. 喚醒等待的 parent (把 parent 放到 run queue) 

```c 273:274:kernel/src/shell.c
if (par)
    thread_wakeup(par);
```

#### 1.  

```c 197:197:kernel/src/sched.c
void thread_wakeup(struct thread *t)
```

#### 2. 替換 block state 為 ready state，並把 thread 加到 run queue。 

```c 200:203:kernel/src/sched.c
if (t && t->state == THREAD_BLOCKED) {
    t->state = THREAD_READY;
    list_add_tail(&t->link, &g_runq);
}
```

## 4.  

```c 115:115:kernel/src/trap.c
tf->a0 = (uintptr_t)do_syscall(tf);
```

### 1.  

```c 453:453:kernel/src/syscall.c
long do_syscall(struct trap_frame *tf)
```

### 2. 回傳 TCB 紀錄的 PID 

```c 457:457:kernel/src/syscall.c
return sys_getpid();
```

### 3.  

```c 459:459:kernel/src/syscall.c
return sys_uart_read((char *)tf->a0, (long)tf->a1);
```

#### 1.  

```c 81:81:kernel/src/syscall.c
static long sys_uart_read(char *buf, long count)
```

#### 2. 驗證讀取數量與讀取位置合法。 

```c 83:84:kernel/src/syscall.c
if (count <= 0 || !in_user_range(buf, (unsigned long)count))
    return -1;
```

#### 3. 還沒讀過 data 時，先讓出執行權。反之，直接不讀回傳 data。 

```c 88:94:kernel/src/syscall.c
int c = uart_getc();
if (c < 0) {
    if (i > 0)
        break;          /* non-blocking after the first byte */
    schedule();         /* yield while waiting */
    continue;
}
```

### 4.  

```c 461:461:kernel/src/syscall.c
return sys_uart_write((const char *)tf->a0, (long)tf->a1);
```

#### 1.  

```c 110:110:kernel/src/syscall.c
static long sys_uart_write(const char *buf, long count)
```

### 5.  

```c 463:463:kernel/src/syscall.c
return sys_exec((const char *)tf->a0, tf);
```

#### 1.  

```c 131:131:kernel/src/syscall.c
static long sys_exec(const char *path, struct trap_frame *tf)
```

#### 2. Load 要 exec 的 image。 

```c 137:148:kernel/src/syscall.c
const void *initrd = (const void *)
    dtb_getprop("/chosen", "linux,initrd-start");
const void   *src;
unsigned long sz;
if (!initrd || cpio_find(initrd, path, &src, &sz) != 0)
    return -1;

unsigned long total = sz + USER_STACK_SIZE;
void *buf = kmalloc(total);
if (!buf)
    return -1;
mem_cpy(buf, src, sz);
```

#### 3. 替換舊 image 為接下來要執行的。 

```c 155:161:kernel/src/syscall.c
unsigned long flags = sie_save_clear();
void *old = self->image_base;
self->image_base = buf;
self->image_size = sz;
self->total_size = total;
sie_restore(flags);
kfree(old);
```

#### 4. 初始化 trap frame: 

- sepc: 指向 image 開頭，接下來會從這邊開始執行
- sp: 更新為新 image 的尾端

```c 164:174:kernel/src/syscall.c
tf->sepc = (uintptr_t)buf;
tf->sp   = ((uintptr_t)buf + total) & ~0xFUL;
tf->tp = (uintptr_t)self;
tf->ra = tf->gp = 0;
tf->t0 = tf->t1 = tf->t2 = 0;
tf->t3 = tf->t4 = tf->t5 = tf->t6 = 0;
tf->a0 = tf->a1 = tf->a2 = tf->a3 = 0;
tf->a4 = tf->a5 = tf->a6 = tf->a7 = 0;
tf->s0 = tf->s1 = tf->s2 = tf->s3 = 0;
tf->s4 = tf->s5 = tf->s6 = tf->s7 = 0;
tf->s8 = tf->s9 = tf->s10 = tf->s11 = 0;
```

### 6.  

```c 465:465:kernel/src/syscall.c
return sys_fork(tf);
```

#### 1.  

```c 198:198:kernel/src/syscall.c
static long sys_fork(struct trap_frame *tf)
```

#### 2. 複製 parent thread image 

```c 210:219:kernel/src/syscall.c
/* 1) Full copy of image+stack into a fresh contiguous buffer. */
ch->image_size = par->image_size;
ch->total_size = par->total_size;
ch->image_base = kmalloc(ch->total_size);
if (!ch->image_base) {
    kfree(ch->kstack_base);
    kfree(ch);
    return -1;
}
mem_cpy(ch->image_base, par->image_base, ch->total_size);
```

#### 3. 複製 parent trap frame 

```c 229:235:kernel/src/syscall.c
uintptr_t top = (uintptr_t)ch->kstack_base + ch->kstack_size;
top &= ~0xFUL;
struct trap_frame *cf =
    (struct trap_frame *)(top - sizeof(struct trap_frame));
/* Use mem_cpy() rather than struct assignment so the compiler does
 * not lower it into a libc memcpy() call (we are -nostdlib). */
mem_cpy(cf, tf, sizeof(*cf));
```

### 7.  

```c 467:467:kernel/src/syscall.c
return sys_waitpid((long)tf->a0);
```

#### 1.  

```c 264:264:kernel/src/syscall.c
static long sys_waitpid(long pid)
```

#### 2. 定位等待的 PID 的 TCB。 

```c 271:277:kernel/src/syscall.c
list_for_each(it, &self->children) {
    struct thread *c = list_entry(it, struct thread, sibling);
    if (c->pid == (int)pid) {
        child = c;
        break;
    }
}
```

#### 3. 等指定的 PID 結束 

```c 286:287:kernel/src/syscall.c
while (child->state != THREAD_ZOMBIE)
    thread_block();
```

### 8.  

```c 469:469:kernel/src/syscall.c
sys_exit((long)tf->a0);
```

#### 1.  

```c 310:310:kernel/src/syscall.c
static void sys_exit(long status)
```

#### 2. 把所有 child 的 parent 設為 init thread。後續 child 結束時，會呼叫 init thread 幫忙收屍。 

```c 317:323:kernel/src/syscall.c
while (!list_empty(&self->children)) {
    struct thread *c = list_entry(self->children.next,
                                  struct thread, sibling);
    list_del(&c->sibling);
    c->parent = &g_bootstrap;
    list_add_tail(&c->sibling, &g_bootstrap.children);
}
```

### 9.  

```c 471:471:kernel/src/syscall.c
return sys_stop((long)tf->a0);
```

#### 1.  

```c 353:353:kernel/src/syscall.c
static long sys_stop(long pid)
```

#### 2. 停止自己的話， 

```c 355:357:kernel/src/syscall.c
if ((int)pid == get_current()->pid) {
    sys_exit(-1);                 /* noreturn */
}
```

#### 3. 定位要停止的 PID 的 TCB 

```c 359:361:kernel/src/syscall.c
struct thread *t = find_thread_by_pid((int)pid);
if (!t || !t->image_base)
    return -1;
```

#### 4. 設定結束狀態，並將自己移出 run queue。 

```c 364:367:kernel/src/syscall.c
t->exit_status = -1;
if (t->state == THREAD_READY)
    list_del(&t->link);
t->state = THREAD_ZOMBIE;
```

#### 5. 歸還 user-space 的記憶體 

```c 371:374:kernel/src/syscall.c
if (t->image_base) {
    kfree(t->image_base);
    t->image_base = NULL;
}
```

#### 6. 喚醒 parent 回收被停止的 thread。 

```c 375:376:kernel/src/syscall.c
if (par)
    thread_wakeup(par);
```

## 5.  

```c 476:477:kernel/src/syscall.c
case SYS_USLEEP:
    return sys_usleep((unsigned int)tf->a0);
```

### 1.  

```c 436:436:kernel/src/syscall.c
static long sys_usleep(unsigned int usec)
```

### 2. 取得當前 TCB 

```c 440:440:kernel/src/syscall.c
struct usleep_token tok = { .t = get_current() };
```

### 3. 設定 timer 與 timer 觸發時執行的 callback。 

```c 441:441:kernel/src/syscall.c
add_timer_us(usleep_cb, &tok, (uint64_t)usec);
```

#### 1.  

```c 174:174:kernel/src/timer.c
void add_timer_us(void (*callback)(void *), void *arg, uint64_t usec)
```

#### 2. 設定 timer 與 callback 

```c 177:177:kernel/src/timer.c
add_timer_ticks(callback, arg, ticks);
```

#### 3.  

```c 115:116:kernel/src/timer.c
static void add_timer_ticks(void (*callback)(void *), void *arg,
                            uint64_t ticks)
```

### 4. Timer callback，會喚醒傳入的 TCB 代表的 thread。 

```c 419:419:kernel/src/syscall.c
static void usleep_cb(void *arg)
```

#### 1. 喚醒 thread 

```c 422:422:kernel/src/syscall.c
thread_wakeup(s->t);
```

