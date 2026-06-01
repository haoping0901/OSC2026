# Trace Result - 2026-06-01

## 1.  

```c 556:556:kernel/src/shell.c
shell_kill_pid(cmd + 5);
```

### 1.  

```c 290:290:kernel/src/shell.c
static void shell_kill_pid(const char *args)
```

### 2. 取出 pid 

```c 297:307:kernel/src/shell.c
while (*p == ' ')
    p++;
while (*p >= '0' && *p <= '9') {
    pid = pid * 10 + (*p - '0');
    p++;
    digits++;
}
if (digits == 0 || *p != ' ') {
    uart_puts("usage: kill <pid> <signum>\n");
    return;
}
```

### 3.  

```c 309:320:kernel/src/shell.c
while (*p == ' ')
    p++;
digits = 0;
while (*p >= '0' && *p <= '9') {
    signum = signum * 10 + (*p - '0');
    p++;
    digits++;
}
if (digits == 0 || signum <= 0 || signum >= NSIG) {
    uart_puts("usage: kill <pid> <signum>\n");
    return;
}
```

### 4. 設定特定 signal 的 pending status 

```c 328:328:kernel/src/shell.c
signal_post(t, signum);
```

#### 1.  

```c 84:84:kernel/src/signal.c
void signal_post(struct thread *t, int signum)
```

## 2.  

```c 132:132:kernel/src/syscall.c
static long sys_exec(const char *path, struct trap_frame *tf)
```

### 1.  

```c 170:170:kernel/src/syscall.c
signal_release(self);
```

## 3. 設定特定 signal number 對應的 handler 

```c 478:478:kernel/src/syscall.c
static long sys_signal(int signum, void (*handler)(void))
```

## 4.  

```c 499:499:kernel/src/syscall.c
static long sys_kill(int pid, int signum)
```

### 1.  

```c 506:506:kernel/src/syscall.c
signal_post(t, signum);
```

## 5.  

```c 519:519:kernel/src/syscall.c
static long sys_sigreturn(struct trap_frame *tf)
```

### 1.  

```c 521:521:kernel/src/syscall.c
return signal_return(tf);
```

### 2.  

```c 256:256:kernel/src/signal.c
long signal_return(struct trap_frame *tf)
```

## 6.  

```c 85:85:kernel/src/trap.c
void trap_handler(struct trap_frame *tf)
```

### 1.  

```c 129:129:kernel/src/trap.c
deliver_pending_signal(tf);
```

### 2.  

```c 48:48:kernel/src/trap.c
static void deliver_pending_signal(struct trap_frame *tf)
```

### 3.  

```c 186:186:kernel/src/signal.c
void signal_check_and_dispatch(struct trap_frame *tf)
```

### 4. 找最低位被 set 的 bit index 

```c 200:200:kernel/src/signal.c
int signum = lowest_bit_index(self->sig.pending);
```

#### 1.  

```c 50:50:kernel/src/signal.c
static int lowest_bit_index(unsigned long x)
```

### 5. 清 pending state，代表已處理這個 signal。 

```c 201:201:kernel/src/signal.c
self->sig.pending &= ~(1UL << signum);
```

### 6. 取出 signal handler 

```c 204:204:kernel/src/signal.c
void (*h)(void) = self->sig.handlers[signum];
```

### 7. 呼叫預設 handler 

```c 210:210:kernel/src/signal.c
signal_default_terminate(self);
```

#### 1.  

```c 124:124:kernel/src/signal.c
void signal_default_terminate(struct thread *t)
```

#### 2. 釋放 signal stack 

```c 141:141:kernel/src/signal.c
signal_release(t);
```

##### 1.  

```c 103:103:kernel/src/signal.c
void signal_release(struct thread *t)
```

### 8. 設定 signal handler return 後，會自動送 signal return ecall，把控制權交回 kernel。 

```c 228:228:kernel/src/signal.c
plant_sigreturn_trampoline(stk);
```

#### 1.  

```c 157:157:kernel/src/signal.c
static void plant_sigreturn_trampoline(void *stack_base)
```

### 9. 設定 sret 後要執行的 signal handler。 

```c 235:235:kernel/src/signal.c
tf->sepc = (uintptr_t)h;
```

