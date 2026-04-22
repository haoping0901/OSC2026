# Trace Result - 2026-04-19

## 1.  

```c 128:128:kernel/src/main.c
timer_init();
```

### 1.  

```c 39:39:kernel/src/timer.c
void timer_init(void)
```

### 2. 取得 DTB 定義的 time CSR + 1 (time tick) 的頻率。 

```c 42:43:kernel/src/timer.c
if (g_timebase_freq == 0)
	g_timebase_freq = DEFAULT_TIMEBASE_FREQ;
```

### 3. %%Tip%% 設定兩秒觸發一次的 timer interrupt。 

```c 46:46:kernel/src/timer.c
sbi_set_timer(read_time() + TIMER_INTERVAL_SECS * g_timebase_freq);
```

#### 1.  

```c 23:23:kernel/src/timer.c
static inline uint64_t read_time(void)
```

#### 2. %%Tip%% 讀取 time CSR 並回傳讀取到的值。 

指令格式可參考 riscv spec ch6.3.

```c 27:27:kernel/src/timer.c
asm volatile ("rdtime %0" : "=r"(t));
```

#### 3.  

```c 61:61:kernel/src/sbi.c
struct sbiret sbi_set_timer(unsigned long long stime_value)
```

#### 4. 嘗試用新版 timer EID 發 ecall 產生 trap，請 M-mode trap handler 設定 timer interrupt。 

參數參考 risc-v SBI spec ch.6。

```c 63:64:kernel/src/sbi.c
struct sbiret r = sbi_ecall(SBI_EXT_TIME, SBI_EXT_TIME_SET_TIMER,
                            stime_value, 0, 0, 0, 0, 0);
```

#### 5. 不支援新版的話，就使用舊版的 EID。 

```c 70:71:kernel/src/sbi.c
return sbi_ecall(SBI_EXT_LEGACY_SET_TIMER, 0,
                 stime_value, 0, 0, 0, 0, 0);
```

### 4. 啟用 timer interrupt，使 CPU 不會忽略 timer interrupt。 

SIE layout 須參考 risc ch. 4.1.3.。

```c 49:49:kernel/src/timer.c
asm volatile ("csrs sie, %0" :: "r"((unsigned long)SIE_STIE));
```

### 5. 啟用 S-mode 中的中斷，使 interrupt 在 S-mode 中仍會被處理。 

```c 52:52:kernel/src/timer.c
asm volatile ("csrs sstatus, %0" :: "r"((unsigned long)SSTATUS_SIE));
```

## 2.  

```c 49:49:kernel/src/trap.c
void trap_handler(struct trap_frame *tf)
```

### 1. 因為 interrupt 而進 trap handler 時 

```c 54:54:kernel/src/trap.c
if (cause & SCAUSE_INTR_BIT) {
```

### 2. Interrupt 來自 timer 的話，呼叫 timer interrupt handler。 

```c 57:58:kernel/src/trap.c
if (code == INTR_S_TIMER)
    timer_handle_interrupt();
```

### 3.  

```c 62:62:kernel/src/timer.c
void timer_handle_interrupt(void)
```

### 4. 設定下一次 timer interrupt 觸發時間點。 

```c 70:70:kernel/src/timer.c
sbi_set_timer(read_time() + TIMER_INTERVAL_SECS * g_timebase_freq);
```

## 3.  

```plaintext 21:21:kernel/src/trap_entry.S
trap_entry:
```

### 1. 在 S-mode 收到 trap 時，跳去 Lfrom_kernel label。 

```plaintext 24:24:kernel/src/trap_entry.S
beqz  sp, .Lfrom_kernel
```

### 2. stack 往下長一個 trap frame 的空間。 

```plaintext 136:136:kernel/src/trap_entry.S
addi  sp, sp, -TF_SIZE
```

