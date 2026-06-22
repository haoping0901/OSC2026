# Trace Result - 2026-06-22

## 1.  

```c 213:213:kernel/src/shell.c
static void run_user_program(const char *name)
```

### 1.  

```c 215:215:kernel/src/shell.c
struct thread *t = thread_spawn_user(name);
```

### 2.  

```c 579:579:kernel/src/sched.c
struct thread *thread_spawn_user(const char *path)
```

### 3. 幫要執行的 user process 要一塊放 PGD 的空間。 

```c 596:596:kernel/src/sched.c
t->pgd = pgd_alloc();
```

#### 1.  

```c 224:224:kernel/src/mm.c
unsigned long *pgd_alloc(void)
```

#### 2. 清 0 分配到的 page 

```c 229:229:kernel/src/mm.c
zero_page(p);
```

#### 3. 複製 kernel PGD 到剛剛分配的 user process 的 PGD。 

```c 231:233:kernel/src/mm.c
unsigned long *kpgd = kernel_pgd();
for (int i = KERNEL_PGD_HALF; i < PTRS_PER_TABLE; i++)
    p[i] = kpgd[i];        /* share kernel high-half PMD tables */
```

### 4. 配置三塊 buddy 記憶體（image / stack / signal page），把程式 bytes 複製進 image 區，並在 @t->pgd 安裝對應的 Sv39 葉節點 PTE；成功時把這些核心 VA 記入 @t 的 bookkeeping 欄位，失敗時完整 rollback。 

```c 600:600:kernel/src/sched.c
if (uvm_setup_image(t, src, sz) != 0)
```

#### 1. 為一個使用者行程建立其私有位址空間的記憶體內容：配置 image 與 stack 的實體連續記憶體、載入程式映像，並把 image、stack、signal page 三者映射到 @t->pgd 中固定的使用者虛擬位址。 

```c 510:510:kernel/src/sched.c
int uvm_setup_image(struct thread *t, const void *src, unsigned long sz)
```

#### 2. 將使用者程式映像（image）以 PROT_USER_RWX 權限映射到該行程私有位址空間中固定的使用者虛擬位址 USER_CODE_VA（即 VA 0x0），使其在 U-mode 下可被執行。 

```c 539:540:kernel/src/sched.c
if (map_pages(t->pgd, USER_CODE_VA, img_bytes,
              virt_to_phys(img), PROT_USER_RWX) != 0)
```

#### 3. 在 t->pgd 中為 image frame block 建立 img_bytes / PAGE_SIZE 個 4 KiB leaf PTE，使 VA USER_CODE_VA 起始的區間指向 virt_to_phys(img) 起始的實體頁。 

```c 279:280:kernel/src/mm.c
int map_pages(unsigned long *pgd, unsigned long va, unsigned long size,
              unsigned long pa, unsigned long prot)
```

#### 4. 對 pgd[idx] 做「存在則沿用、不存在則建立」，回傳下一級表的 kernel VA。 

```c 285:285:kernel/src/mm.c
unsigned long *pmd = walk_or_create(pgd, PT_INDEX(v, 2));
```

##### 1. 析 Sv39 分頁某一級的索引，回傳下一級表的 kernel VA，若該 entry 尚未存在則配置並清零一張新表、以非葉指標 PTE 連結後回傳。 

```c 250:250:kernel/src/mm.c
static unsigned long *walk_or_create(unsigned long *table, unsigned long idx)
```

##### 2. PTE_V 已設 → 用 PTE_TO_PA() 取出下一級表 PA，再 phys_to_virt() 轉回 kernel VA 回傳 

```c 253:254:kernel/src/mm.c
if (pte & PTE_V)
    return phys_to_virt(PTE_TO_PA(pte));
```

#### 5. 建 PTE 

```c 292:292:kernel/src/mm.c
pte[PT_INDEX(v, 0)] = MAKE_PTE(pa + off, prot);
```

## 2. schedule 時 

```c 157:158:kernel/src/trap.c
if (need_resched_clear())
    schedule();
```

### 1.  

```c 240:240:kernel/src/sched.c
void schedule(void)
```

### 2. 沒有 thread 在等待執行時 

```c 251:261:kernel/src/sched.c
if (list_empty(&g_runq)) {
    /* Nobody else is ready — keep prev on the CPU. Restore the
     * RUNNING state we may have just cleared. BLOCKED prev would
     * deadlock here (no one to wake us), but the only legitimate
     * reason for an empty runq is "idle is the only thread", and
     * idle never blocks. */
    if (prev->state == THREAD_READY)
        prev->state = THREAD_RUNNING;
    sie_restore(flags);
    return;
}
```

### 3. 找下個要執行的 thread 

```c 263:263:kernel/src/sched.c
struct thread *next = list_entry(g_runq.next, struct thread, link);
```

### 4. 按需切換位址空間（satp），再切換執行緒（kernel stack + 暫存器 + tp） 

```c 269:283:kernel/src/sched.c
if (prev != next) {
    /*
     * Install next's address space before switching kernel stacks.
     * A user process uses its private PGD; a kernel-only thread uses
     * the kernel PGD. The kernel high half is identical across all
     * PGDs, so the currently executing kernel code/stack stay valid
     * across the satp write. Skipping the write when the target PGD
     * already matches avoids a needless sfence.vma.
     */
    unsigned long *next_pgd = next->pgd ? next->pgd : kernel_pgd();
    unsigned long *prev_pgd = prev->pgd ? prev->pgd : kernel_pgd();
    if (next_pgd != prev_pgd)
        mm_set_satp(next_pgd);

    switch_to(prev, next);

```

## 3.  

```c 234:234:kernel/src/syscall.c
static long sys_fork(struct trap_frame *tf)
```

### 1.  

```c 247:249:kernel/src/syscall.c
ch->pgd = pgd_alloc();
if (!ch->pgd)
    goto fail_thread;
```

### 2.  

```c 267:283:kernel/src/syscall.c
if (map_pages(ch->pgd, USER_CODE_VA, img_bytes,
              virt_to_phys(img), PROT_USER_RWX) != 0)
    goto fail_stk;
if (map_pages(ch->pgd, USER_STACK_TOP - stk_bytes, stk_bytes,
              virt_to_phys(stk), PROT_USER_DATA) != 0)
    goto fail_stk;

/* 3b) Child's own signal page (PROT_USER_RWX). Contents need not be
 *     copied: the handler stack/trampoline are (re)established at
 *     dispatch time; we only need the backing frame mapped at the same
 *     SIGPAGE_VA so a signal can be delivered to the child in U-mode. */
void *sig = buddy_alloc(PAGE_SIZE);
if (!sig)
    goto fail_stk;
if (map_pages(ch->pgd, SIGPAGE_VA, PAGE_SIZE,
              virt_to_phys(sig), PROT_USER_RWX) != 0)
    goto fail_sig;
```

## 4.  

```c 359:359:kernel/src/sched.c
static void kill_zombies(void)
```

### 1.  

```c 372:372:kernel/src/sched.c
thread_free_user_vm(z);
```

### 2.  

```c 327:327:kernel/src/sched.c
void thread_free_user_vm(struct thread *t)
```

### 3.  

```c 342:342:kernel/src/sched.c
pgd_free(t->pgd);
```

### 4. 釋放一個使用者位址空間「低半部」（user half）的所有中介頁表（PMD、PTE table）並清空對應的 PGD entry，但刻意不釋放葉節點所指向的使用者實體頁框。 

```c 335:335:kernel/src/mm.c
uvm_destroy(pgd);
```

#### 1. 走訪 PGD 低半部（user 空間），自底向上釋放 PTE table → PMD table，並把該 PGD entry 歸零；高半部（kernel 共享映射）完全不碰。 

```c 307:307:kernel/src/mm.c
void uvm_destroy(unsigned long *pgd)
```

#### 2. 只走 0 .. KERNEL_PGD_HALF-1（256），即 Sv39 的低 256 個 PGD slot，對應 user 位址空間；高半部跳過。 

```c 309:309:kernel/src/mm.c
for (int i = 0; i < KERNEL_PGD_HALF; i++) {
```

#### 3. 用 PTE_TO_PA() 取出 PMD table 實體位址，再 phys_to_virt() 轉成線性映射 VA 才能存取。 

```c 312:312:kernel/src/mm.c
unsigned long *pmd = phys_to_virt(PTE_TO_PA(pgd[i]));
```

#### 4. 走 PTRS_PER_TABLE（512）個 PMD entry 

```c 314:314:kernel/src/mm.c
for (int j = 0; j < PTRS_PER_TABLE; j++) {
```

#### 5. 對每個 valid 者取出 PTE table 並 buddy_free(pte)——釋放最底層頁表頁本身。 

```c 315:318:kernel/src/mm.c
if (!(pmd[j] & PTE_V))
    continue;
unsigned long *pte = phys_to_virt(PTE_TO_PA(pmd[j]));
buddy_free(pte);
```

#### 6. PMD 全部處理完後 buddy_free(pmd) 釋放該 PMD table，並把 pgd[i] = 0 標記為無效（關鍵：再次呼叫時 PTE_V 不成立而跳過）。 

```c 320:321:kernel/src/mm.c
buddy_free(pmd);
pgd[i] = 0;
```

## 5. 以 initrd 中指定的程式映像「就地」取代當前行程的整個位址空間，並改寫 trap_frame 使隨後的 sret 直接跳進新程式的進入點，行程 pid 不變。 

```c 610:611:kernel/src/syscall.c
case SYS_EXEC:
    return sys_exec((const char *)tf->a0, tf);
```

### 1. 建立一個全新的 PGD 與映像/堆疊/signal page 對應，原子性地切換 satp 到新位址空間，回收舊位址空間，重設 signal 狀態，最後改寫 trap_frame 讓 sret 落入新程式。 

```c 133:133:kernel/src/syscall.c
static long sys_exec(const char *path, struct trap_frame *tf)
```

### 2. 把映像對應到 USER_CODE_VA、堆疊置於 USER_STACK_TOP 下方，並安裝一個全新 signal page。 

```c 171:171:kernel/src/syscall.c
if (uvm_setup_image(self, src, sz) != 0) {
```

### 3. 切換位址空間並回收舊 VM 

```c 180:189:kernel/src/syscall.c
/* Switch to the new address space, then reclaim the old one via the
 * saved pointers (satp no longer points at it). */
mm_set_satp(new_pgd);
if (old_image)
    buddy_free(old_image);
if (old_stack)
    buddy_free(old_stack);
if (old_sig)
    buddy_free(old_sig);
pgd_free(old_pgd);
```

