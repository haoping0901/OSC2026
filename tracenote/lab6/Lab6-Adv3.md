# Trace Result - 2026-07-26

## 1. fork 時建立 CoW 共享 

<!-- tracenote -->
fork 時建立 CoW 共享
<!-- /tracenote -->

### 1.  

```c 618:619:kernel/src/syscall.c
case SYS_FORK:
    return sys_fork(tf);
```

### 2.  

```c 238:238:kernel/src/syscall.c
static long sys_fork(struct trap_frame *tf)
```

### 3.  

```c 251:251:kernel/src/syscall.c
ch->pgd = pgd_alloc();
```

### 4.  

```c 256:256:kernel/src/syscall.c
if (uvm_clone_vma(ch, par) != 0)
```

#### 1. Caller: 

<!-- tracenote -->
Caller:
- 唯一 caller 是 syscall.c 的 sys_fork()（SYS_FORK 經 do_syscall() 分派），在配置好子行程的 thread 與私有 PGD 之後呼叫
<!-- /tracenote -->

#### 2. Timing: 

<!-- tracenote -->
Timing:
- 使用者行程執行 fork() ecall、trap 進 S-mode 之後。
- 前置條件：ch->vma_list 為空、ch->pgd 已配置（kernel 高半部已共享）、par->pgd 為當前存活的位址空間（satp 仍指向它）。
- 失敗時 sys_fork() 走 fail_vm 以 vma_free_list() + pgd_free() 回收半成品。
<!-- /tracenote -->

#### 3. Design Rationale: 

<!-- tracenote -->
Design Rationale:
- CoW 而非全量複製：fork 成本從「複製整個 image」降為「走一遍 page table + metadata」，且 fork 後常接 exec()，全量複製多半是浪費。
- sigpage 必須 eager copy：kernel 透過 t->sigpage_base 這個 linear-map 別名寫入 trampoline 狀態，繞過使用者 PTE 的唯讀保護——若 sigpage 走 CoW 共享，一方的 signal delivery 會直接寫穿到另一方的頁面；且 U-mode 觸發的 CoW break 會換掉 frame，讓 sigpage_base 指向過期的舊 frame。
- 失敗路徑免逐項回滾：半成品全掛在 ch 上，caller 用 vma_free_list() + pgd_free()（內含 dec-and-test 的 buddy_free()）單點回收；父行程被降級的 PTE 則在下次寫入時由 do_cow_fault() 發現 refcount == 1 而自癒升回可寫。
<!-- /tracenote -->

#### 4. 完成後子行程擁有與父行程完全相同的 VA 佈局： 

<!-- tracenote -->
完成後子行程擁有與父行程完全相同的 VA 佈局：
- 所有 VMA metadata（範圍、prot、initrd file backing、is_mmap）都在 ch->vma_list 上；
- 父行程實際填入過的頁面，雙方 PTE 指向同一實體 frame（可寫者降為唯讀 + PTE_COW）；
- 未觸碰過的頁面留待子行程日後 demand-fault 自行填入。
- 回傳 0 成功、-1 OOM。
<!-- /tracenote -->

```c 927:927:kernel/src/mm.c
int uvm_clone_vma(struct thread *ch, struct thread *par)
```

#### 5. 走遍 par->vma_list 

<!-- tracenote -->
走遍 par->vma_list
<!-- /tracenote -->

```c 930:930:kernel/src/mm.c
list_for_each(it, &par->vma_list) {
```

#### 6. %%Tip%% Phase 1：VMA metadata 克隆 

<!-- tracenote -->
Phase 1：VMA metadata 克隆
<!-- /tracenote -->

#### 7. 為每個父 VMA 建立對等的 metadata 節點並依 VA 排序插入子行程。 

<!-- tracenote -->
為每個父 VMA 建立對等的 metadata 節點並依 VA 排序插入子行程。
- file_src/file_len 一併繼承 → 子行程 demand-fault image 頁時仍能從 initrd 讀入內容
<!-- /tracenote -->

```c 931:938:kernel/src/mm.c
struct vma *pv = list_entry(it, struct vma, link);

struct vma *nv = vma_alloc(pv->va, pv->len, pv->prot,
                           pv->file_src, pv->file_len,
                           pv->is_mmap);
if (!nv)
    return -1;
vma_insert_sorted(ch, nv);
```

#### 8. %%Tip%% Phase 2：sigpage 例外 — 立即實體複製 

<!-- tracenote -->
Phase 2：sigpage 例外 — 立即實體複製
<!-- /tracenote -->

#### 9. SIGPAGE_VA 不走共享路徑，而是配一頁新 frame、把父行程 sigpage 內容整頁複製後映射給子行程。 

<!-- tracenote -->
SIGPAGE_VA 不走共享路徑，而是配一頁新 frame、把父行程 sigpage 內容整頁複製後映射給子行程。
<!-- /tracenote -->

```c 940:957:kernel/src/mm.c
if (pv->va == SIGPAGE_VA) {
    /* Eager private copy — see the sigpage note above. */
    unsigned long *pte = pt_lookup(par->pgd, pv->va);
    if (!pte || !(*pte & PTE_V))
        return -1;

    void *np = buddy_alloc(PAGE_SIZE);
    if (!np)
        return -1;
    mem_cpy(np, phys_to_virt(PTE_TO_PA(*pte)), PAGE_SIZE);

    if (map_pages(ch->pgd, pv->va, PAGE_SIZE,
                  virt_to_phys(np), pv->prot) != 0) {
        buddy_free(np);
        return -1;
    }
    continue;
}
```

#### 10. %%Tip%% Phase 3：逐頁 CoW 共享 

<!-- tracenote -->
Phase 3：逐頁 CoW 共享
<!-- /tracenote -->

#### 11. 對非 sigpage 的 VMA 逐頁檢查父 PTE 

<!-- tracenote -->
對非 sigpage 的 VMA 逐頁檢查父 PTE
<!-- /tracenote -->

```c 959:959:kernel/src/mm.c
for (unsigned long off = 0; off < pv->len; off += PAGE_SIZE) {
```

#### 12. 未 present 的頁面直接 continue：不建 PTE、不加引用，子行程日後從自己繼承的 VMA demand-fault 出私有 frame 

<!-- tracenote -->
未 present 的頁面直接 continue：不建 PTE、不加引用，子行程日後從自己繼承的 VMA demand-fault 出私有 frame
<!-- /tracenote -->

```c 960:962:kernel/src/mm.c
unsigned long *ppte = pt_lookup(par->pgd, pv->va + off);
if (!ppte || !(*ppte & PTE_V))
    continue;       /* never touched: child faults later */
```

#### 13. 可寫 VMA 則雙方同時撤掉 PTE_W 並掛上 PTE_COW。 

<!-- tracenote -->
可寫 VMA 則雙方同時撤掉 PTE_W 並掛上 PTE_COW。
<!-- /tracenote -->

```c 965:970:kernel/src/mm.c
if (pv->prot & PTE_W) {
    /* Withhold W on BOTH sides so either side's first
     * store faults into the CoW break. */
    shared = (shared & ~PTE_W) | PTE_COW;
    *ppte  = shared;
}
```

#### 14. present 的頁面讓子行程映射同一實體 frame 

<!-- tracenote -->
present 的頁面讓子行程映射同一實體 frame
<!-- /tracenote -->

```c 972:974:kernel/src/mm.c
if (map_pages(ch->pgd, pv->va + off, PAGE_SIZE,
              PTE_TO_PA(shared),
              shared & PTE_FLAGS_MASK) != 0)
```

#### 15. 每共享一頁就 buddy_ref_inc() 記一次引用；之後任一方寫入時 do_page_fault() → do_cow_fault() 依 refcount 決定「複製一份」或「refcount == 1 時原地升回可寫」 

<!-- tracenote -->
每共享一頁就 buddy_ref_inc() 記一次引用；之後任一方寫入時 do_page_fault() → do_cow_fault() 依 refcount 決定「複製一份」或「refcount == 1 時原地升回可寫」
<!-- /tracenote -->

```c 976:976:kernel/src/mm.c
buddy_ref_inc(phys_to_virt(PTE_TO_PA(shared)));
```

##### 1. 增加特定 frame page 的 reference cnt 

<!-- tracenote -->
增加特定 frame page 的 reference cnt
<!-- /tracenote -->

```c 476:476:kernel/src/buddy.c
void buddy_ref_inc(void *ptr)
```

#### 16. %%Tip%% Phase 4：TLB / I-cache 收尾 

<!-- tracenote -->
Phase 4：TLB / I-cache 收尾
<!-- /tracenote -->

#### 17. - 父行程的 PTE 是原地改寫的，TLB 可能還快取著舊的可寫翻譯；一次全域 sfence.vma 統一沖掉（fork 頻率低，全域 flush 比逐頁划算） 

<!-- tracenote -->
- 父行程的 PTE 是原地改寫的，TLB 可能還快取著舊的可寫翻譯；一次全域 sfence.vma 統一沖掉（fork 頻率低，全域 flush 比逐頁划算）
- fence.i 讓子行程複製到的 sigpage trampoline 指令對 I-cache 可見
<!-- /tracenote -->

```c 980:984:kernel/src/mm.c
/* The parent may still hold writable translations for the pages
 * downgraded above; flush them all at once (fork is infrequent). */
asm volatile ("sfence.vma zero, zero" ::: "memory");
/* The copied sigpage contains executable bytes; sync the I-cache. */
asm volatile ("fence.i" ::: "memory");
```

## 2. 寫入 CoW 頁 → break（含 refcount==1 fast path） 

<!-- tracenote -->
寫入 CoW 頁 → break（含 refcount==1 fast path）
<!-- /tracenote -->

### 1.  

```c 167:168:kernel/src/trap.c
if (cause == EXC_INST_PAGE_FAULT || cause == EXC_LOAD_PAGE_FAULT ||
    cause == EXC_STORE_PAGE_FAULT) {
```

### 2.  

```c 180:180:kernel/src/trap.c
if (do_page_fault(tf) == 0) {
```

### 3.  

```c 840:840:kernel/src/mm.c
int do_page_fault(struct trap_frame *tf)
```

### 4. 確認 fault 位址落在具寫入權限的 VMA 內。 

<!-- tracenote -->
確認 fault 位址落在具寫入權限的 VMA 內。
<!-- /tracenote -->

```c 847:847:kernel/src/mm.c
struct vma *v = vma_find(cur, addr);
```

### 5. 在使用者對一個 fork 時共享的 CoW 頁面執行寫入操作觸發 store page fault 時，依據該實體頁的參照計數決定就地升權或另配私有頁複製，以完成 copy-on-write 共享的破除。 

<!-- tracenote -->
在使用者對一個 fork 時共享的 CoW 頁面執行寫入操作觸發 store page fault 時，依據該實體頁的參照計數決定就地升權或另配私有頁複製，以完成 copy-on-write 共享的破除。
<!-- /tracenote -->

```c 861:862:kernel/src/mm.c
if (tf->scause == EXC_STORE_PAGE_FAULT && (*pte & PTE_COW))
    return do_cow_fault(cur, v, pte, addr);
```

#### 1. Caller: 讓唯一呼叫點在 mm.c:862 的 do_page_fault()；do_page_fault() 本身則由 trap.c:180 的 trap dispatcher 在 scause 為 EXC_INST/LOAD/STORE_PAGE_FAULT 時呼叫。 

<!-- tracenote -->
Caller: 讓唯一呼叫點在 mm.c:862 的 do_page_fault()；do_page_fault() 本身則由 trap.c:180 的 trap dispatcher 在 scause 為 EXC_INST/LOAD/STORE_PAGE_FAULT 時呼叫。
<!-- /tracenote -->

#### 2. Timing: do_page_fault() 先以 vma_find() 確認 fault 位址落在具寫入權限的 VMA 內，再以 pt_lookup() 取得 leaf PTE；只有當 tf->scause == EXC_STORE_PAGE_FAULT 且 該 PTE 同時 present（PTE_V）與帶 PTE_COW 時才進入此函式，此檢查發生在 S-mode 內核錯誤判斷之前——因為 syscall 在 SUM 下對 user buffer 的合法寫入，也會以 S-mode 觸發同一個 store fault。 

<!-- tracenote -->
Timing: do_page_fault() 先以 vma_find() 確認 fault 位址落在具寫入權限的 VMA 內，再以 pt_lookup() 取得 leaf PTE；只有當 tf->scause == EXC_STORE_PAGE_FAULT 且 該 PTE 同時 present（PTE_V）與帶 PTE_COW 時才進入此函式，此檢查發生在 S-mode 內核錯誤判斷之前——因為 syscall 在 SUM 下對 user buffer 的合法寫入，也會以 S-mode 觸發同一個 store fault。
<!-- /tracenote -->

#### 3. 讓 @addr 所在頁重新變成可寫，且不影響其他仍共享同一實體頁的行程。 

<!-- tracenote -->
讓 @addr 所在頁重新變成可寫，且不影響其他仍共享同一實體頁的行程。
<!-- /tracenote -->

```c 771:771:kernel/src/mm.c
static int do_cow_fault(struct thread *t, struct vma *v,
```

#### 4. 先重新打開 SSTATUS_SIE——因為硬體在 trap 進入時已清除該位，而後續的 UART TX 與 buddy allocator 呼叫都仰賴中斷。 

<!-- tracenote -->
先重新打開 SSTATUS_SIE——因為硬體在 trap 進入時已清除該位，而後續的 UART TX 與 buddy allocator 呼叫都仰賴中斷。
<!-- /tracenote -->

```c 774:774:kernel/src/mm.c
asm volatile ("csrs sstatus, %0" :: "r"((unsigned long)SSTATUS_SIE));
```

#### 5. 由 *pte 換算出 old_frame 的核心虛擬位址，並以 buddy_ref_read() 讀參照計數。 

<!-- tracenote -->
由 *pte 換算出 old_frame 的核心虛擬位址，並以 buddy_ref_read() 讀參照計數。
<!-- /tracenote -->

```c 776:778:kernel/src/mm.c
void *old_frame = phys_to_virt(PTE_TO_PA(*pte));

if (buddy_ref_read(old_frame) == 1) {
```

##### 1.  

```c 501:501:kernel/src/buddy.c
unsigned int buddy_ref_read(void *ptr)
```

##### 2. 拿到 PA 

<!-- tracenote -->
拿到 PA
<!-- /tracenote -->

```c 506:506:kernel/src/buddy.c
uintptr_t addr = frame_ptr_to_pa(ptr);
```

##### 3. 回傳 pa 對應紀錄的 ref cnt。 

<!-- tracenote -->
回傳 pa 對應紀錄的 ref cnt。
<!-- /tracenote -->

```c 510:510:kernel/src/buddy.c
return g_ref_array[addr_to_idx(addr)];
```

#### 6. 計數為 1（唯一持有者）：代表對方行程已結束或已先行破除共享，直接就地在 PTE 上補回 PTE_W、設 PTE_D（dirty，因為即將被寫入）、清除 PTE_COW，不配置、不複製。 

<!-- tracenote -->
計數為 1（唯一持有者）：代表對方行程已結束或已先行破除共享，直接就地在 PTE 上補回 PTE_W、設 PTE_D（dirty，因為即將被寫入）、清除 PTE_COW，不配置、不複製。
<!-- /tracenote -->

```c 780:780:kernel/src/mm.c
*pte = (*pte | PTE_W | PTE_D) & ~PTE_COW;
```

#### 7. 計數 > 1（仍被共享） 

<!-- tracenote -->
計數 > 1（仍被共享）
<!-- /tracenote -->

```c 781:781:kernel/src/mm.c
} else {
```

#### 8.  

```c 782:782:kernel/src/mm.c
void *np = buddy_alloc(PAGE_SIZE);
```

#### 9. 複製舊頁內容 

<!-- tracenote -->
複製舊頁內容
<!-- /tracenote -->

```c 788:788:kernel/src/mm.c
mem_cpy(np, old_frame, PAGE_SIZE);
```

#### 10. 安裝新 PTE 

<!-- tracenote -->
安裝新 PTE
<!-- /tracenote -->

```c 789:789:kernel/src/mm.c
*pte = MAKE_PTE(virt_to_phys(np), v->prot);
```

#### 11. 釋放舊頁上這一份參照 

<!-- tracenote -->
釋放舊頁上這一份參照
<!-- /tracenote -->

```c 790:790:kernel/src/mm.c
buddy_free(old_frame);  /* drop this side's reference */
```

##### 1. 對 ptr 對應的頁框執行 dec-and-test； 

<!-- tracenote -->
對 ptr 對應的頁框執行 dec-and-test；
- 若仍有其他持有者則僅遞減計數並返回
- 若是最後一份參照則將整個分配區塊標回 free，並嘗試與其 buddy 迭代合併後掛回對應 order 的 free list
<!-- /tracenote -->

```c 385:385:kernel/src/buddy.c
void buddy_free(void *ptr)
```

##### 2. 三層防呆：空指標、地址落在受管理區間外、以及該頁被標為 FRAME_RESERVED（不可分配的保留頁），任一情況直接返回。 

<!-- tracenote -->
三層防呆：空指標、地址落在受管理區間外、以及該頁被標為 FRAME_RESERVED（不可分配的保留頁），任一情況直接返回。
<!-- /tracenote -->

```c 387:400:kernel/src/buddy.c
if (!ptr)
    return;

/* Caller hands back the VA we returned from buddy_alloc(); convert it
 * to a PA to resume PA-based bookkeeping. */
uintptr_t addr = frame_ptr_to_pa(ptr);
if (addr < g_buddy_base || addr >= g_buddy_end)
    return;

unsigned long idx = addr_to_idx(addr);

/* Guard against freeing a reserved page. */
if (g_frame_array[idx] == FRAME_RESERVED)
    return;
```

##### 3. 防止其他 CoW 路徑在 fault handler（SIE 開啟）中搶佔造成計數競態 

<!-- tracenote -->
防止其他 CoW 路徑在 fault handler（SIE 開啟）中搶佔造成計數競態
<!-- /tracenote -->

```c 409:409:kernel/src/buddy.c
unsigned long flags = sie_save_clear();
```

##### 4. 若計數已是 0（雙重釋放的訊號）僅印警告並返回 

<!-- tracenote -->
若計數已是 0（雙重釋放的訊號）僅印警告並返回
<!-- /tracenote -->

```c 410:415:kernel/src/buddy.c
if (g_ref_array[idx] == 0) {
    sie_restore(flags);
    uart_puts("[Buddy] WARN: free of zero-ref block idx=");
    print_dec_ulong(idx);
    uart_puts("\n");
    return;
```

##### 5. 遞減後若仍 >0 代表還有其他 sharer，直接返回，不動 free list。 

<!-- tracenote -->
遞減後若仍 >0 代表還有其他 sharer，直接返回，不動 free list。
<!-- /tracenote -->

```c 417:420:kernel/src/buddy.c
g_ref_array[idx]--;
if (g_ref_array[idx] > 0) {
    sie_restore(flags);
    return;
```

#### 12. 只 sfence.vma 該單一頁對應的位址——因為只有 @addr 這條唯讀轉換是 stale，其餘 TLB entry 不受影響 

<!-- tracenote -->
只 sfence.vma 該單一頁對應的位址——因為只有 @addr 這條唯讀轉換是 stale，其餘 TLB entry 不受影響
<!-- /tracenote -->

```c 795:797:kernel/src/mm.c
/* Only this VA's read-only translation can be stale; flush it. */
asm volatile ("sfence.vma %0, zero" :: "r"(addr & ~(PAGE_SIZE - 1))
              : "memory");
```

