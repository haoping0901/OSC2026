# Trace Result - 2026-04-25

## 1.  

```c 359:359:kernel/src/shell.c
shell_set_timeout(cmd + 11);
```

## 2.  

```c 84:84:kernel/src/shell.c
static void shell_set_timeout(const char *args)
```

## 3. 檢查 cmd 秒數 input 

```c 92:100:kernel/src/shell.c
while (*p >= '0' && *p <= '9') {
    sec = sec * 10 + (*p - '0');
    p++;
    digits++;
}
if (digits == 0 || sec <= 0 || *p != ' ') {
    uart_puts("usage: setTimeout <seconds> <message>\n");
    return;
}
```

## 4. 紀錄當前與要設定的時間 

```c 114:116:kernel/src/shell.c
c->reg_tick = timer_read_ticks();
c->exp_tick = c->reg_tick +
              (uint64_t)sec * timer_get_timebase_freq();
```

## 5. 紀錄時間到時，要輸出的內容。 

```c 118:123:kernel/src/shell.c
unsigned int i = 0;
while (p[i] != '\0' && i < SETTO_MSG_MAX - 1) {
    c->msg[i] = p[i];
    i++;
}
c->msg[i] = '\0';
```

## 6. %%Tip%%  

```c 125:125:kernel/src/shell.c
add_timer(setto_cb, c, sec);
```

### 1.  

```c 104:104:kernel/src/timer.c
void add_timer(void (*callback)(void *), void *arg, int sec)
```

### 2. 初始化紀錄 timer 資訊的 node。 

```c 111:114:kernel/src/timer.c
n->register_tick = now;
n->expire_tick   = now + (uint64_t)sec * g_timebase_freq;
n->cb            = callback;
n->arg           = arg;
```

### 3. 紀錄當前全域中斷開關狀態，並關閉中斷。 

Timer interrupt critical section。

```c 116:116:kernel/src/timer.c
unsigned long flags = sie_save_clear();
```

### 4. 取得  list_head 對應的 timer node 的位址。 

```c 121:121:kernel/src/timer.c
struct timer_node *t = list_entry(p, struct timer_node, link);
```

### 5. 找到 time queue 中，第一個比要新增的 timer 還晚的 timer 時，條出迴圈。 

後續會把要新增的 timer 新增在第一個比它晚的 timer 前 => 產生由小到大排列的 timer queue。

```c 122:123:kernel/src/timer.c
if (n->expire_tick < t->expire_tick)
	break;
```

### 6. 設定最快要觸發的 timer 時，重新設定 timer。 

```c 132:133:kernel/src/timer.c
if (g_timer_queue.next == &n->link)
	sbi_set_timer(n->expire_tick);
```

#### 1.  

```c 61:61:kernel/src/sbi.c
struct sbiret sbi_set_timer(unsigned long long stime_value)
```

### 7. 恢復原先全域中斷狀態。 

```c 135:135:kernel/src/timer.c
sie_restore(flags);
```

## 7. %%Tip%%  

```c 147:147:kernel/src/timer.c
void timer_handle_interrupt(void)
```

### 1. 將過期的 timer 從 list 移出，並呼叫各個 timer 紀錄的 callback 輸出設定/排定/觸發時間。 

```c 151:160:kernel/src/timer.c
while (!list_empty(&g_timer_queue)) {
	struct timer_node *t = list_entry(g_timer_queue.next,
	                                  struct timer_node, link);
	if (t->expire_tick > now)
		break;

	list_del(&t->link);
	t->cb(t->arg);
	kfree(t);
}
```

### 2. timer list 還有 timer 的話，就設置最近的那個 timer 的觸發時間到硬體。 

沒有的話，設定最大值（等於是不會觸發）。
需要設最大值的原因是，RISCV 的 timer interrupt 是在等到當前時間 >= 設定時間就會被觸發，因此須再把時間推到未來。

```c 162:168:kernel/src/timer.c
if (!list_empty(&g_timer_queue)) {
	struct timer_node *h = list_entry(g_timer_queue.next,
	                                  struct timer_node, link);
	sbi_set_timer(h->expire_tick);
} else {
	sbi_set_timer((uint64_t)-1);
}
```

