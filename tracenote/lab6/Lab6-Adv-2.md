# Trace Result - 2026-07-05

## 1. U-mode 合法 demand fault（首次 touch） 

<!-- tracenote -->
U-mode 合法 demand fault（首次 touch）
<!-- /tracenote -->

### 1. - EXC_INST_PAGE_FAULT：取指令（instruction fetch）時轉換失敗——pc 指向的頁無效或不可執行。 

<!-- tracenote -->
- EXC_INST_PAGE_FAULT：取指令（instruction fetch）時轉換失敗——pc 指向的頁無效或不可執行。
  - 需要 PTE_X 權限
- EXC_LOAD_PAGE_FAULT：讀取記憶體（load 指令，如 ld、lw）時轉換失敗。
  - 需要 PTE_R 權限
- EXC_STORE_PAGE_FAULT：寫入記憶體（store/AMO 指令，如 sd、sw）時轉換失敗
  - 需要 PTE_W 權限
<!-- /tracenote -->

```c 167:168:kernel/src/trap.c
if (cause == EXC_INST_PAGE_FAULT || cause == EXC_LOAD_PAGE_FAULT ||
    cause == EXC_STORE_PAGE_FAULT) {
```

### 2. Demand paging 的核心決策點：對 scause 12/13/15 的 page fault，判定屬於「合法但尚未填頁的 VMA 存取」則就地配置並映射一頁後回傳 0 讓硬體重試原指令，否則視為 segfault 殺掉 U-mode 行程或回傳 -1 交由 kernel 診斷停機。 

<!-- tracenote -->
Demand paging 的核心決策點：對 scause 12/13/15 的 page fault，判定屬於「合法但尚未填頁的 VMA 存取」則就地配置並映射一頁後回傳 0 讓硬體重試原指令，否則視為 segfault 殺掉 U-mode 行程或回傳 -1 交由 kernel 診斷停機。
<!-- /tracenote -->

```c 180:180:kernel/src/trap.c
if (do_page_fault(tf) == 0) {
```

#### 1. 依 tf->stval（faulting VA）與 tf->scause（存取類型）對照目前執行緒的 VMA list，產生三種結果之一：populate 一頁並回傳 0、以 do_exit(-1) 殺掉行程（不返回）、或回傳 -1 標示 kernel bug。 

<!-- tracenote -->
依 tf->stval（faulting VA）與 tf->scause（存取類型）對照目前執行緒的 VMA list，產生三種結果之一：populate 一頁並回傳 0、以 do_exit(-1) 殺掉行程（不返回）、或回傳 -1 標示 kernel bug。
<!-- /tracenote -->

```c 764:764:kernel/src/mm.c
int do_page_fault(struct trap_frame *tf)
```

#### 2. 排除無 pgd 的 kernel thread 

<!-- tracenote -->
排除無 pgd 的 kernel thread
<!-- /tracenote -->

```c 766:768:kernel/src/mm.c
struct thread *cur = get_current();
if (!cur || !cur->pgd)
    return -1;
```

#### 3. 在指定 thread 的 VMA 串列中線性搜尋，回傳涵蓋給定使用者虛擬位址的那個 struct vma（找不到則回傳 NULL），供 page fault handler 判斷該位址是否屬於合法已宣告的使用者區域。 

<!-- tracenote -->
在指定 thread 的 VMA 串列中線性搜尋，回傳涵蓋給定使用者虛擬位址的那個 struct vma（找不到則回傳 NULL），供 page fault handler 判斷該位址是否屬於合法已宣告的使用者區域。
<!-- /tracenote -->

```c 770:771:kernel/src/mm.c
unsigned long addr = tf->stval;
struct vma *v = vma_find(cur, addr);
```

##### 1. 給定位址 addr，回傳 t->vma_list 中滿足 addr ∈ [va, va+len) 的 VMA 節點指標；無任何區域涵蓋時回傳 NULL。 

<!-- tracenote -->
給定位址 addr，回傳 t->vma_list 中滿足 addr ∈ [va, va+len) 的 VMA 節點指標；無任何區域涵蓋時回傳 NULL。
<!-- /tracenote -->

```c 454:454:kernel/src/mm.c
struct vma *vma_find(struct thread *t, unsigned long addr)
```

##### 2. 走訪 t->vma_list 

<!-- tracenote -->
走訪 t->vma_list
<!-- /tracenote -->

```c 457:457:kernel/src/mm.c
list_for_each(it, &t->vma_list) {
```

##### 3. 對每個節點做半開區間包含測試，命中即 early return。 

<!-- tracenote -->
對每個節點做半開區間包含測試，命中即 early return。
- 區間測試 addr >= v->va && addr < v->va + v->len 為半開區間 [va, va+len)
<!-- /tracenote -->

```c 458:460:kernel/src/mm.c
struct vma *v = list_entry(it, struct vma, link);
if (addr >= v->va && addr < v->va + v->len)
    return v;
```

#### 4. scause → need：instruction fault 要 PTE_X、load 要 PTE_R、store 要 PTE_W。 

<!-- tracenote -->
scause → need：instruction fault 要 PTE_X、load 要 PTE_R、store 要 PTE_W。
<!-- /tracenote -->

```c 773:779:kernel/src/mm.c
unsigned long need;
if (tf->scause == EXC_INST_PAGE_FAULT)
    need = PTE_X;
else if (tf->scause == EXC_LOAD_PAGE_FAULT)
    need = PTE_R;
else
    need = PTE_W;
```

#### 5. violation 條件一：VA 不在任何 VMA 內，或 VMA 的 prot 不含 need 

<!-- tracenote -->
violation 條件一：VA 不在任何 VMA 內，或 VMA 的 prot 不含 need
<!-- /tracenote -->

```c 781:781:kernel/src/mm.c
int violation = (!v || !(v->prot & need));
```

#### 6. 在 Sv39 三層頁表中對指定虛擬位址做不配置中間頁表的唯讀 walk，回傳 leaf PTE slot 的指標，供呼叫端自行判斷該頁是否已被實際 populate。 

<!-- tracenote -->
在 Sv39 三層頁表中對指定虛擬位址做不配置中間頁表的唯讀 walk，回傳 leaf PTE slot 的指標，供呼叫端自行判斷該頁是否已被實際 populate。
<!-- /tracenote -->

```c 783:783:kernel/src/mm.c
unsigned long *pte = pt_lookup(cur->pgd, addr);
```

##### 1. 將 va 依序解出 PGD → PMD → PTE 三層索引，回傳最底層 PTE slot 的位址；任何一層中間表不存在（!PTE_V）即回傳 NULL。回傳的 slot 本身可能仍是 invalid PTE — 呼叫端須自行測試 *slot & PTE_V。 

<!-- tracenote -->
將 va 依序解出 PGD → PMD → PTE 三層索引，回傳最底層 PTE slot 的位址；任何一層中間表不存在（!PTE_V）即回傳 NULL。回傳的 slot 本身可能仍是 invalid PTE — 呼叫端須自行測試 *slot & PTE_V。
<!-- /tracenote -->

```c 315:315:kernel/src/mm.c
unsigned long *pt_lookup(unsigned long *pgd, unsigned long va)
```

##### 2. PT_INDEX(va, 2) 取 PGD 索引，entry 無 PTE_V → early return NULL。 

<!-- tracenote -->
PT_INDEX(va, 2) 取 PGD 索引，entry 無 PTE_V → early return NULL。
<!-- /tracenote -->

```c 317:319:kernel/src/mm.c
unsigned long pte = pgd[PT_INDEX(va, 2)];
if (!(pte & PTE_V))
    return NULL;
```

#### 7. violation 條件二：leaf PTE 已存在（PTE_V set）卻仍 fault——present + faulting 等同權限違規，同時斬斷「populate 後仍無限重複 fault」的迴圈風險。 

<!-- tracenote -->
violation 條件二：leaf PTE 已存在（PTE_V set）卻仍 fault——present + faulting 等同權限違規，同時斬斷「populate 後仍無限重複 fault」的迴圈風險。
<!-- /tracenote -->

```c 784:784:kernel/src/mm.c
if (pte && (*pte & PTE_V))
```

#### 8. SSTATUS_SPP = 1（fault 發生在 S-mode）→ 回傳 -1：kernel 解參考了 in_user_range() 本應擋下的指標，屬 kernel bug，交由 caller 的診斷停機路徑。 

<!-- tracenote -->
SSTATUS_SPP = 1（fault 發生在 S-mode）→ 回傳 -1：kernel 解參考了 in_user_range() 本應擋下的指標，屬 kernel bug，交由 caller 的診斷停機路徑。
<!-- /tracenote -->

```c 789:790:kernel/src/mm.c
if (tf->sstatus & SSTATUS_SPP)
    return -1;      /* S-mode deref outside checks: kernel bug */
```

#### 9. SPP = 0（U-mode）→ 列印 segfault 訊息後 do_exit(-1)，永不返回。 

<!-- tracenote -->
SPP = 0（U-mode）→ 列印 segfault 訊息後 do_exit(-1)，永不返回。
<!-- /tracenote -->

```c 791:793:kernel/src/mm.c
asm volatile ("csrs sstatus, %0"
              :: "r"((unsigned long)SSTATUS_SIE));
uart_puts("[Segmentation fault]: Kill Process\n");
```

#### 10. 把自己標成 THREAD_ZOMBIE、將子行程過繼給 g_bootstrap、喚醒等待中的 parent，然後永久讓出 CPU，實際資源回收延後給 reaper。 

<!-- tracenote -->
把自己標成 THREAD_ZOMBIE、將子行程過繼給 g_bootstrap、喚醒等待中的 parent，然後永久讓出 CPU，實際資源回收延後給 reaper。
<!-- /tracenote -->

```c 794:794:kernel/src/mm.c
do_exit(-1);        /* never returns */
```

##### 1.  

```c 374:374:kernel/src/syscall.c
void do_exit(long status)
```

##### 2. 在 sie_save_clear() 臨界區內記錄 exit status。 

<!-- tracenote -->
在 sie_save_clear() 臨界區內記錄 exit status。
<!-- /tracenote -->

```c 378:379:kernel/src/syscall.c
unsigned long flags = sie_save_clear();
self->exit_status = (int)status;
```

##### 3. 把所有存活子行程搬到 g_bootstrap.children。 

<!-- tracenote -->
把所有存活子行程搬到 g_bootstrap.children。
<!-- /tracenote -->

```c 381:387:kernel/src/syscall.c
while (!list_empty(&self->children)) {
    struct thread *c = list_entry(self->children.next,
                                  struct thread, sibling);
    list_del(&c->sibling);
    c->parent = &g_bootstrap;
    list_add_tail(&c->sibling, &g_bootstrap.children);
}
```

##### 4. 標記自己為 ZOMBIE。 

<!-- tracenote -->
標記自己為 ZOMBIE。
<!-- /tracenote -->

```c 389:389:kernel/src/syscall.c
self->state = THREAD_ZOMBIE;
```

#### 11. 合法 fault 則先重開 SSTATUS_SIE（硬體在 trap entry 清掉了它）。 

<!-- tracenote -->
合法 fault 則先重開 SSTATUS_SIE（硬體在 trap entry 清掉了它）。
<!-- /tracenote -->

```c 797:797:kernel/src/mm.c
asm volatile ("csrs sstatus, %0" :: "r"((unsigned long)SSTATUS_SIE));
```

#### 12. 在合法 VMA 內發生 page fault 時，配置並填入一個 4 KiB 實體頁框、安裝 leaf PTE，讓 sret 重試原指令即可成功存取。 

<!-- tracenote -->
在合法 VMA 內發生 page fault 時，配置並填入一個 4 KiB 實體頁框、安裝 leaf PTE，讓 sret 重試原指令即可成功存取。
<!-- /tracenote -->

```c 798:798:kernel/src/mm.c
if (demand_page(cur, v, addr) != 0) {
```

##### 1. 為 faulting VA 所在的頁面建立「零頁或檔案內容 → 實體頁框 → user 頁表映射」的完整鏈路；成功回傳 0（caller 以不動 sepc 的 sret 重試指令），OOM 回傳 -1（caller 殺掉 process）。 

<!-- tracenote -->
為 faulting VA 所在的頁面建立「零頁或檔案內容 → 實體頁框 → user 頁表映射」的完整鏈路；成功回傳 0（caller 以不動 sepc 的 sret 重試指令），OOM 回傳 -1（caller 殺掉 process）。
<!-- /tracenote -->

```c 709:709:kernel/src/mm.c
static int demand_page(struct thread *t, struct vma *v, unsigned long addr)
```

##### 2. 將 addr 對齊到 page base，向 buddy 要一頁並清零。 

<!-- tracenote -->
將 addr 對齊到 page base，向 buddy 要一頁並清零。
<!-- /tracenote -->

```c 711:716:kernel/src/mm.c
unsigned long page_base = addr & ~(PAGE_SIZE - 1);

void *frame = buddy_alloc(PAGE_SIZE);
if (!frame)
    return -1;
zero_page(frame);
```

##### 3. VMA 有 initrd 且此頁與 [0, file_len) 重疊時： 

<!-- tracenote -->
VMA 有 initrd 且此頁與 [0, file_len) 重疊時：
<!-- /tracenote -->

```c 719:719:kernel/src/mm.c
if (v->file_src && off < v->file_len) {
```

##### 4. 複製長度 n 夾在 PAGE_SIZE 內，只填「檔案覆蓋到的部分」。 

<!-- tracenote -->
複製長度 n 夾在 PAGE_SIZE 內，只填「檔案覆蓋到的部分」。
<!-- /tracenote -->

```c 720:723:kernel/src/mm.c
unsigned long n = v->file_len - off;
if (n > PAGE_SIZE)
    n = PAGE_SIZE;
mem_cpy(frame, (const char *)v->file_src + off, n);
```

##### 5. VMA 的 prot 安裝 leaf PTE；中間頁表配置失敗時歸還 frame，避免洩漏。 

<!-- tracenote -->
VMA 的 prot 安裝 leaf PTE；中間頁表配置失敗時歸還 frame，避免洩漏。
<!-- /tracenote -->

```c 728:730:kernel/src/mm.c
if (map_pages(t->pgd, page_base, PAGE_SIZE,
              virt_to_phys(frame), v->prot) != 0) {
    buddy_free(frame);
```

##### 6. 只沖掉該 VA 的舊（invalid）translation，而非全域 flush。 

<!-- tracenote -->
只沖掉該 VA 的舊（invalid）translation，而非全域 flush。
<!-- /tracenote -->

```c 735:735:kernel/src/mm.c
asm volatile ("sfence.vma %0, zero" :: "r"(page_base) : "memory");
```

## 2. fork 逐頁複製 

<!-- tracenote -->
fork 逐頁複製
情境：user 呼叫 fork()，原本 img/stk/sig/mmap 三軌特例收斂為單一迴圈。
<!-- /tracenote -->

### 1.  

```c 615:616:kernel/src/syscall.c
case SYS_FORK:
    return sys_fork(tf);
```

### 2.  

```c 235:235:kernel/src/syscall.c
static long sys_fork(struct trap_frame *tf)
```

### 3. 完成後 ch->vma_list 擁有與父行程相同的區域佈局（含 image 的 initrd file backing），ch->pgd 中只映射父行程實際 present 的頁面——每頁都是獨立的私有拷貝，位於相同的 user VA。 

<!-- tracenote -->
完成後 ch->vma_list 擁有與父行程相同的區域佈局（含 image 的 initrd file backing），ch->pgd 中只映射父行程實際 present 的頁面——每頁都是獨立的私有拷貝，位於相同的 user VA。
<!-- /tracenote -->

```c 253:254:kernel/src/syscall.c
if (uvm_clone_vma(ch, par) != 0)
    goto fail_vm;
```

#### 1. 完成後 ch->vma_list 擁有與父行程相同的區域佈局（含 image 的 initrd file backing），ch->pgd 中只映射父行程實際 present 的頁面——每頁都是獨立的私有拷貝，位於相同的 user VA。 

<!-- tracenote -->
完成後 ch->vma_list 擁有與父行程相同的區域佈局（含 image 的 initrd file backing），ch->pgd 中只映射父行程實際 present 的頁面——每頁都是獨立的私有拷貝，位於相同的 user VA。
<!-- /tracenote -->

```c 829:829:kernel/src/mm.c
int uvm_clone_vma(struct thread *ch, struct thread *par)
```

#### 2. 對父行程每個 VMA 

<!-- tracenote -->
對父行程每個 VMA
<!-- /tracenote -->

```c 832:832:kernel/src/mm.c
list_for_each(it, &par->vma_list) {
```

#### 3. 完整繼承 va/len/prot/file_src/file_len/is_mmap——連 file backing 都繼承，子行程日後 demand fault 時能自行從 initrd 載入 

<!-- tracenote -->
完整繼承 va/len/prot/file_src/file_len/is_mmap——連 file backing 都繼承，子行程日後 demand fault 時能自行從 initrd 載入
<!-- /tracenote -->

```c 835:840:kernel/src/mm.c
struct vma *nv = vma_alloc(pv->va, pv->len, pv->prot,
                           pv->file_src, pv->file_len,
                           pv->is_mmap);
if (!nv)
    return -1;
vma_insert_sorted(ch, nv);
```

#### 4. 以 PAGE_SIZE 步進走訪該 VMA 範圍 

<!-- tracenote -->
以 PAGE_SIZE 步進走訪該 VMA 範圍
<!-- /tracenote -->

```c 842:842:kernel/src/mm.c
for (unsigned long off = 0; off < pv->len; off += PAGE_SIZE) {
```

#### 5. 查父行程的 leaf PTE 判斷該頁是否 present。 

<!-- tracenote -->
查父行程的 leaf PTE 判斷該頁是否 present。
<!-- /tracenote -->

```c 843:843:kernel/src/mm.c
unsigned long *pte = pt_lookup(par->pgd, pv->va + off);
```

#### 6. PTE 不存在或無 PTE_V → continue：該頁父行程從未觸碰，子行程之後透過 do_page_fault() → demand_page() 自行載入。 

<!-- tracenote -->
PTE 不存在或無 PTE_V → continue：該頁父行程從未觸碰，子行程之後透過 do_page_fault() → demand_page() 自行載入。
<!-- /tracenote -->

```c 844:845:kernel/src/mm.c
if (!pte || !(*pte & PTE_V))
    continue;       /* never touched: child faults later */
```

#### 7. Present 的頁面：buddy_alloc() 配一個新 frame，mem_cpy() 從父行程 frame 的 kernel VA 別名（phys_to_virt()）整頁拷貝 

<!-- tracenote -->
Present 的頁面：buddy_alloc() 配一個新 frame，mem_cpy() 從父行程 frame 的 kernel VA 別名（phys_to_virt()）整頁拷貝
<!-- /tracenote -->

```c 847:850:kernel/src/mm.c
void *np = buddy_alloc(PAGE_SIZE);
if (!np)
    return -1;
mem_cpy(np, phys_to_virt(PTE_TO_PA(*pte)), PAGE_SIZE);
```

#### 8. 以相同 user VA、相同 prot map_pages() 進 ch->pgd 

<!-- tracenote -->
以相同 user VA、相同 prot map_pages() 進 ch->pgd
<!-- /tracenote -->

```c 852:853:kernel/src/mm.c
if (map_pages(ch->pgd, pv->va + off, PAGE_SIZE,
              virt_to_phys(np), pv->prot) != 0) {
```

## 3. teardown（單一所有權的收斂點） 

<!-- tracenote -->
teardown（單一所有權的收斂點）
情境：exit 被 reap、exec 換象、fork 失敗回滾——三條路共用同一釋放邏輯。
<!-- /tracenote -->

### 1.  

```c 331:331:kernel/src/sched.c
void thread_free_user_vm(struct thread *t)
```

### 2. 清空給定的 thread 的 vma list。 

<!-- tracenote -->
清空給定的 thread 的 vma list。
<!-- /tracenote -->

```c 333:333:kernel/src/sched.c
vma_unmap_all(t);
```

### 3.  

```c 336:337:kernel/src/sched.c
if (t->pgd) {
    pgd_free(t->pgd);
```

#### 1. 清掉 

<!-- tracenote -->
清掉
<!-- /tracenote -->

```c 374:374:kernel/src/mm.c
void pgd_free(unsigned long *pgd)
```

