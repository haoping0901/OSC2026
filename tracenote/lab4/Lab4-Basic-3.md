# Trace Result - 2026-04-22

## 1.  

```c 134:134:kernel/src/main.c
uart_init();
```

### 1.  

```c 213:213:kernel/src/uart.c
void uart_init(void)
```

### 2. 啟用 UART 中斷。 

```c 219:220:kernel/src/uart.c
unsigned int mcr = mmio_read(UART_BASE + UART_MCR);
mmio_write(UART_BASE + UART_MCR, mcr | MCR_OUT2);
```

### 3. 關閉 TX interrupt，避免沒有要傳送東西時，仍一直觸發代表 FIFO 還有空間的 IRQ。 

TIE 在 uart_putc 把 byte 放進 tx_buf 後才打開。

```c 224:225:kernel/src/uart.c
unsigned int ier = mmio_read(UART_BASE + UART_IER);
mmio_write(UART_BASE + UART_IER, (ier & ~IER_TIE) | IER_RAVIE);
```

## 2. 取得 interrupt controller 的 base address 與 IRQ number。 

```c 140:147:kernel/src/main.c
#ifdef QEMU
    uintptr_t plic_base = dtb_getprop("/soc/plic", "reg");
#else
    uintptr_t plic_base = dtb_getprop("/soc/interrupt-controller",
                                     "reg");
#endif
    unsigned int uart_irq = (unsigned int)dtb_getprop("/soc/serial",
                                                     "interrupts");
```

## 3.  

```c 150:150:kernel/src/main.c
plic_init(plic_base, uart_irq, plic_ctx);
```

### 1. plic_init(plic_base, uart_irq, PLIC_CTX_HART0_S); 

```c 35:35:kernel/src/plic.c
void plic_init(uintptr_t base, unsigned int irq, unsigned int ctx)
```

### 2. 設定中斷優先級。 

可以設 [1, 1023]，越低的優先級越高。
Ref. ch 4

```c 41:41:kernel/src/plic.c
mmio_w32(base + PLIC_PRIORITY_OFF + 4UL * irq, 1U);
```

### 3. 設定 interrupt enable bit，以啟用全域中斷。 

Ref. ch 6 & ch 1.1

```c 43:48:kernel/src/plic.c
/* Enable bit for (ctx, irq) */
uintptr_t enable_reg = base + PLIC_ENABLE_OFF
                     + PLIC_ENABLE_STRIDE * ctx
                     + 4UL * (irq / 32U);
unsigned int bit = 1U << (irq % 32U);
mmio_w32(enable_reg, mmio_r32(enable_reg) | bit);
```

### 4. 設定中斷優先級 threshold。設為 0 代表任何 priority >= 1 的中斷都會被送到 target。 

Ref. Ch7

```c 51:51:kernel/src/plic.c
mmio_w32(base + PLIC_THRESHOLD_OFF + PLIC_CONTEXT_STRIDE * ctx, 0U);
```

## 4. 啟用 supervisor-level 的 external interrupts。 

```c 156:156:kernel/src/main.c
asm volatile ("csrs sie, %0" :: "r"((unsigned long)SIE_SEIE));
```

## 5. 啟用 UART interrupt 

```c 157:157:kernel/src/main.c
uart_enable_irq_mode();
```

### 1. 啟用 UART IRQ mode 

```c 235:238:kernel/src/uart.c
void uart_enable_irq_mode(void)
{
    g_uart_irq_mode = 1;
}
```

### 2. %%Tip%% UART interrupt handler 

```c 248:248:kernel/src/uart.c
void uart_handle_interrupt(void)
```

#### 1. RX FIFO 有資料的時候，就不斷去 RBR CSR 上拿 data，並放到 RX buffer 中。 

```c 250:253:kernel/src/uart.c
while (mmio_read(UART_BASE + UART_LSR) & LSR_DR) {
    unsigned char ch = mmio_read(UART_BASE + UART_RBR) & 0xFF;
    ring_push(&rx_buf, ch);
}
```

#### 2. 當 TX buffer 有 data，且 UART 準備好要傳 data 時，會從 TX buffer 拿 data 放到 TX FIFO 傳輸出去，直到 TX FIFO 滿了，或是 TX buffer 空了。 

```c 255:259:kernel/src/uart.c
while (!ring_empty(&tx_buf) &&
       (mmio_read(UART_BASE + UART_LSR) & LSR_TDRQ)) {
    int c = ring_pop(&tx_buf);
    mmio_write(UART_BASE + UART_THR, (unsigned char)c);
}
```

#### 3. 如果 TX buffer 空了，就關掉傳輸 data 的 interrupt (TIE)，在需要傳輸 data 時 (呼叫 uart_putc()) 再開啟傳輸中斷 (TIE)。 

```c 261:265:kernel/src/uart.c
if (ring_empty(&tx_buf)) {
    unsigned int ier = mmio_read(UART_BASE + UART_IER);
    if (ier & IER_TIE)
        mmio_write(UART_BASE + UART_IER, ier & ~IER_TIE);
}
```

### 3. %%Tip%%  

```c 309:309:kernel/src/uart.c
void uart_putc(unsigned char c)
```

#### 1. 開啟中斷時 

```c 314:314:kernel/src/uart.c
if (g_uart_irq_mode) {
```

#### 2. %%Tip%% TX buffer 滿了的話，就讓 CPU 休眠直到某次 ISR 結束後，TX buffer 被清出空間了。 

```c 316:317:kernel/src/uart.c
while (ring_full(&tx_buf))
    asm volatile ("wfi");
```

#### 3. Critical section 起點：關閉 S-mode 中斷，避免 ISR 在 

```c 319:319:kernel/src/uart.c
unsigned long sie = sie_save_clear();
```

##### 1. 讀取 sstatus SIE bit 當前的狀態後，將 SIE bit clear，並回傳讀取到的狀態。 

```c 189:189:kernel/src/uart.c
static inline unsigned long sie_save_clear(void)
```

##### 2. 格式：csrrc rd, csr, rs1。 

語意：rd = csr; csr = csr & ~rs1

```c 192:192:kernel/src/uart.c
"csrrc %0, sstatus, %1"
```

##### 3. %0 輸出到 prev，%1 的輸入為 SSTATUS_SIE。 

```c 193:193:kernel/src/uart.c
"=r"(prev) : "r"((unsigned long)SSTATUS_SIE)
```

#### 4. 把字元寫進 TX buffer。 

```c 320:320:kernel/src/uart.c
ring_push(&tx_buf, c);
```

#### 5. %%Tip%% 開啟傳輸中斷 (TIE)，這個中斷只在需要傳 data 時開啟，這是為了避免 idle 時，CPU 仍不斷收到可傳輸的中斷。 

```c 322:323:kernel/src/uart.c
if (!(ier & IER_TIE))
    mmio_write(UART_BASE + UART_IER, ier | IER_TIE);
```

#### 6. Critical section 終點：恢復 S-mode 中斷 

```c 324:324:kernel/src/uart.c
sie_restore(sie);
```

### 4. %%Tip%%  

```c 274:274:kernel/src/uart.c
int uart_getc(void)
```

#### 1. 啟用中斷時 

```c 278:278:kernel/src/uart.c
if (g_uart_irq_mode) {
```

#### 2. RX buffer 空的時候，就等待 LSR.DR bit 被設置 

```c 279:280:kernel/src/uart.c
while (ring_empty(&rx_buf))
    asm volatile ("wfi");
```

## 6.  

```c 58:58:kernel/src/trap.c
void trap_handler(struct trap_frame *tf)
```

### 1. 處理外部中斷時 

```c 68:68:kernel/src/trap.c
} else if (code == INTR_S_EXT) {
```

### 2. %%Tip%%  

```c 69:69:kernel/src/trap.c
unsigned int irq = plic_claim();
```

#### 1.  

```c 62:62:kernel/src/plic.c
unsigned int plic_claim(void)
```

#### 2. 取得給定的 context 優先級最高的 IRQ number，並鎖定 PLIC gateway 對該 IRQ 的接收。 

```c 64:65:kernel/src/plic.c
return mmio_r32(g_plic_base + PLIC_CLAIM_OFF
                + PLIC_CONTEXT_STRIDE * g_plic_ctx);
```

### 3. %%Tip%%  

```c 73:73:kernel/src/trap.c
plic_complete(irq);
```

#### 1.  

```c 75:75:kernel/src/plic.c
void plic_complete(unsigned int irq)
```

#### 2. 讓 PLIC 開始接收 input 給的 IRQ。 

參考 riscv-plic ch9。

```c 77:78:kernel/src/plic.c
mmio_w32(g_plic_base + PLIC_CLAIM_OFF
         + PLIC_CONTEXT_STRIDE * g_plic_ctx, irq);
```

