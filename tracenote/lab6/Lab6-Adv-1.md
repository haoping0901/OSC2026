# Trace Result - 2026-07-05

## 1. mmap system call flow 

<!-- tracenote -->
mmap system call flow
<!-- /tracenote -->

### 1.  

```c 156:156:kernel/src/trap.c
tf->a0 = (uintptr_t)do_syscall(tf);
```

### 2.  

```c 604:604:kernel/src/syscall.c
long do_syscall(struct trap_frame *tf)
```

### 3.  

```c 635:637:kernel/src/syscall.c
case SYS_MMAP:
    return sys_mmap((void *)tf->a0, (unsigned long)tf->a1,
                    (int)tf->a2, (int)tf->a3);
```

### 4. anonymous mmap() 系統呼叫的核心：在呼叫者位址空間挑一塊未使用的使用者 VA、配置一塊歸零的實體記憶體、以指定 prot 映射進去並登記一個 VMA，最終回傳該使用者基底 VA。 

<!-- tracenote -->
anonymous mmap() 系統呼叫的核心：在呼叫者位址空間挑一塊未使用的使用者 VA、配置一塊歸零的實體記憶體、以指定 prot 映射進去並登記一個 VMA，最終回傳該使用者基底 VA。
<!-- /tracenote -->

```c 592:594:kernel/src/syscall.c
static long sys_mmap(void *addr, unsigned long length, int prot, int flags)
{
    return (long)do_mmap(get_current(), addr, length, prot, flags);
```

#### 1. 回傳一個已分頁、已歸零、已映射且登記為 VMA 的使用者記憶體區段基底 VA；任何步驟失敗都回滾並回傳 MAP_FAILED（(void *)-1）。 

<!-- tracenote -->
回傳一個已分頁、已歸零、已映射且登記為 VMA 的使用者記憶體區段基底 VA；任何步驟失敗都回滾並回傳 MAP_FAILED（(void *)-1）。
<!-- /tracenote -->

```c 591:592:kernel/src/mm.c
void *do_mmap(struct thread *t, void *addr, unsigned long length,
              int prot, int flags)
```

#### 2. 擋掉非法參數與不支援的非 anonymous 映射 

<!-- tracenote -->
擋掉非法參數與不支援的非 anonymous 映射
<!-- /tracenote -->

```c 594:597:kernel/src/mm.c
if (length == 0 || !t->pgd)
    return MAP_FAILED;
if (!(flags & MAP_ANONYMOUS))   /* only anonymous mappings supported */
    return MAP_FAILED;
```

#### 3. 找能放下要求大小的連續空間，並回傳 VMA。 

<!-- tracenote -->
找能放下要求大小的連續空間，並回傳 VMA。
<!-- /tracenote -->

```c 601:603:kernel/src/mm.c
unsigned long base = mmap_find_va(t, (unsigned long)addr, len);
if (base == 0)
    return MAP_FAILED;
```

##### 1.  

```c 528:529:kernel/src/mm.c
static unsigned long mmap_find_va(struct thread *t, unsigned long hint,
                                  unsigned long len)
```

##### 2. 從 mmap_top 開始，以 page 為單位往下找長度為 len，且沒有與 vma_list 重疊的空間。 

<!-- tracenote -->
從 mmap_top 開始，以 page 為單位往下找長度為 len，且沒有與 vma_list 重疊的空間。
<!-- /tracenote -->

```c 542:547:kernel/src/mm.c
for (unsigned long base = t->mmap_top - len;
     base >= PAGE_SIZE; base -= PAGE_SIZE) {
    if (!vma_overlaps(t, base, len)) {
        /* Reserve one guard page below this region for the next pick. */
        t->mmap_top = base - PAGE_SIZE;
        return base;
```

#### 4. 設定 PTE flags (Orphaned)

<!-- tracenote -->
設定 PTE flags
<!-- /tracenote -->

```c 559:559:kernel/src/mm.c
unsigned long pte_prot = mmap_prot_to_pte(prot);
```

##### 1.  

```c 564:564:kernel/src/mm.c
static unsigned long mmap_prot_to_pte(int prot)
```

#### 5. 把剛剛取得的 PMA，跟找到的 VMA 的 mapping 資訊存到 PGD。 

<!-- tracenote -->
把剛剛取得的 PMA，跟找到的 VMA 的 mapping 資訊存到 PGD。
<!-- /tracenote -->

```c 728:732:kernel/src/mm.c
if (map_pages(t->pgd, base, len, virt_to_phys(kva), pte_prot) != 0) {
    buddy_free(kva);
    return MAP_FAILED;
}
```

#### 6. 用剛剛取得的 KVA 跟 UVA (user virtual address) 等資訊初始化 VMA struct，並加到 

<!-- tracenote -->
用剛剛取得的 KVA 跟 UVA (user virtual address) 等資訊初始化 VMA struct，並加到
<!-- /tracenote -->

```c 605:609:kernel/src/mm.c
struct vma *vma = vma_alloc(base, len, pte_prot, kva, 1);
if (!vma) {
    /* Leaves were installed but unwinding them needs a teardown that
     * uvm_destroy() will do at exit; freeing the frame avoids a leak
     * now while the (orphaned) leaves resolve to nothing referenced. */
    buddy_free(kva);
    return MAP_FAILED;
}
vma_insert_sorted(t, vma);
```

##### 1.  

```c 428:430:kernel/src/mm.c
struct vma *vma_alloc(unsigned long va, unsigned long len,
                      unsigned long prot, void *kva, unsigned char is_mmap)
```

##### 2. %%Tip%%  

##### 3.  

```c 475:475:kernel/src/mm.c
void vma_insert_sorted(struct thread *t, struct vma *vma)
```

### 5. sret 回 U-mode 

<!-- tracenote -->
sret 回 U-mode
<!-- /tracenote -->

```c 163:163:kernel/src/trap.c
deliver_pending_signal(tf);
```

#### 1.  

```c 50:50:kernel/src/trap.c
static void deliver_pending_signal(struct trap_frame *tf)
```

#### 2. 每次返回 U-mode 前，挑選一個最低編號的待處理 (pending) 信號並依其 handler 種類（忽略 / 預設終止 / 使用者 handler）改寫 trap frame，使下一次 sret 跳進使用者信號處理函式。 

<!-- tracenote -->
每次返回 U-mode 前，挑選一個最低編號的待處理 (pending) 信號並依其 handler 種類（忽略 / 預設終止 / 使用者 handler）改寫 trap frame，使下一次 sret 跳進使用者信號處理函式。
<!-- /tracenote -->

```c 56:56:kernel/src/trap.c
signal_check_and_dispatch(tf);
```

#### 3. 從 sig.pending 位元圖取出一個信號、清除其 bit，並依該信號的 handler 指標決定丟棄、終止行程或改寫 tf 進入使用者 handler。 

<!-- tracenote -->
從 sig.pending 位元圖取出一個信號、清除其 bit，並依該信號的 handler 指標決定丟棄、終止行程或改寫 tf 進入使用者 handler。
<!-- /tracenote -->

```c 184:184:kernel/src/signal.c
void signal_check_and_dispatch(struct trap_frame *tf)
```

#### 4. 在關中斷（sie_save_clear()）的區間內選出最低位的 pending 信號並清位，避免與 IRQ context 競爭同一 bitmap。 

<!-- tracenote -->
在關中斷（sie_save_clear()）的區間內選出最低位的 pending 信號並清位，避免與 IRQ context 競爭同一 bitmap。
<!-- /tracenote -->

```c 193:200:kernel/src/signal.c
unsigned long flags = sie_save_clear();
if (self->sig.pending == 0) {
    sie_restore(flags);
    return;
}
int signum = lowest_bit_index(self->sig.pending);
self->sig.pending &= ~(1UL << signum);
sie_restore(flags);
```

#### 5. SIG_IGN 直接丟棄 

<!-- tracenote -->
SIG_IGN 直接丟棄
<!-- /tracenote -->

```c 204:205:kernel/src/signal.c
if (h == SIG_IGN)
    return;
```

#### 6. SIG_DFL/NULL 走預設終止，把執行緒設為 THREAD_ZOMBIE（由 deliver_pending_signal() 隨後偵測並 schedule()，本執行緒不再 sret 回去）。 

<!-- tracenote -->
SIG_DFL/NULL 走預設終止，把執行緒設為 THREAD_ZOMBIE（由 deliver_pending_signal() 隨後偵測並 schedule()，本執行緒不再 sret 回去）。
<!-- /tracenote -->

```c 207:210:kernel/src/signal.c
if (h == SIG_DFL || h == NULL) {
    signal_default_terminate(self);
    return;
}
```

##### 1. 設定 exit_status = -1、轉 ZOMBIE、喚醒 parent 

<!-- tracenote -->
設定 exit_status = -1、轉 ZOMBIE、喚醒 parent
<!-- /tracenote -->

```c 122:122:kernel/src/signal.c
void signal_default_terminate(struct thread *t)
```

#### 7.  

```c 222:222:kernel/src/signal.c
mem_cpy(&self->sig.saved, tf, sizeof(*tf));
```

#### 8. 在訊號頁的低位址寫入一段 8-byte「li a7, 11; ecall」機器碼，讓 U-mode 訊號 handler ret 後能自動經 SYS_SIGRETURN 系統呼叫回到 kernel。 

<!-- tracenote -->
在訊號頁的低位址寫入一段 8-byte「li a7, 11; ecall」機器碼，讓 U-mode 訊號 handler ret 後能自動經 SYS_SIGRETURN 系統呼叫回到 kernel。
<!-- /tracenote -->

```c 224:226:kernel/src/signal.c
/* Plant the trampoline through the page's kernel-VA backing while we
 * are still in S-mode; the handler will reach it at SIGPAGE_VA in U. */
plant_sigreturn_trampoline(self->sigpage_base);
```

##### 1.  

```c 155:155:kernel/src/signal.c
static void plant_sigreturn_trampoline(void *stack_base)
```

#### 9. sepc = h：sret 落到使用者 handler。 

<!-- tracenote -->
sepc = h：sret 落到使用者 handler。
sp/ra 一律設為信號頁的 USER VA（SIGPAGE_VA），非 kernel-VA backing。
<!-- /tracenote -->

```c 229:239:kernel/src/signal.c
/* The handler runs in U-mode, so sp and ra must be the USER VAs of the
 * signal page (PTE_U), not its kernel-VA backing. Stack grows down
 * from the page top; the trampoline sits at the page base (SIGPAGE_VA)
 * and stays reachable via ra. */
uintptr_t top = SIGPAGE_VA + PAGE_SIZE;
top &= ~0xFUL;

tf->sepc = (uintptr_t)h;
tf->sp   = top;
tf->a0   = (uintptr_t)signum;
tf->ra   = SIGPAGE_VA;              /* sigreturn trampoline base */
```

## 2. mmap VMA 回收 flow 

<!-- tracenote -->
mmap VMA 回收 flow
<!-- /tracenote -->

### 1.  

```c 604:604:kernel/src/syscall.c
long do_syscall(struct trap_frame *tf)
```

### 2. 終止當前行程：reparent 存活子行程、標記自己為 THREAD_ZOMBIE、喚醒可能在 waitpid 阻塞的 parent，然後讓出 CPU 且永不返回。 

<!-- tracenote -->
終止當前行程：reparent 存活子行程、標記自己為 THREAD_ZOMBIE、喚醒可能在 waitpid 阻塞的 parent，然後讓出 CPU 且永不返回。
<!-- /tracenote -->

```c 619:620:kernel/src/syscall.c
case SYS_EXIT:
    sys_exit((long)tf->a0);       /* noreturn */
```

#### 1. 將當前 thread 轉為 ZOMBIE 狀態並交還 CPU，等待 parent 的 waitpid 收屍；不在此處釋放使用者位址空間。 

<!-- tracenote -->
將當前 thread 轉為 ZOMBIE 狀態並交還 CPU，等待 parent 的 waitpid 收屍；不在此處釋放使用者位址空間。
<!-- /tracenote -->

```c 414:414:kernel/src/syscall.c
static void sys_exit(long status)
```

#### 2. 把每個存活子行程過繼給 g_bootstrap，避免子行程變孤兒無人收屍。 

<!-- tracenote -->
把每個存活子行程過繼給 g_bootstrap，避免子行程變孤兒無人收屍。
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

#### 3. 標記自己 THREAD_ZOMBIE，並在關中斷下快照 par 指標。 

<!-- tracenote -->
標記自己 THREAD_ZOMBIE，並在關中斷下快照 par 指標。
<!-- /tracenote -->

```c 389:390:kernel/src/syscall.c
self->state = THREAD_ZOMBIE;
struct thread *par = self->parent;
```

### 3. 清理 zombie 的 idle thread 

<!-- tracenote -->
清理 zombie 的 idle thread
<!-- /tracenote -->

```c 382:382:kernel/src/sched.c
static void idle_thread_body(void)
```

#### 1.  

```c 385:385:kernel/src/sched.c
kill_zombies();
```

#### 2. 排空 g_zombies 串列，釋放每個 zombie 線程持有的三類資源：user 位址空間（VMA + 頁表 + PGD）、核心堆疊、線程結構本體。 

<!-- tracenote -->
排空 g_zombies 串列，釋放每個 zombie 線程持有的三類資源：user 位址空間（VMA + 頁表 + PGD）、核心堆疊、線程結構本體。
<!-- /tracenote -->

```c 354:354:kernel/src/sched.c
static void kill_zombies(void)
```

#### 3. 在關閉中斷的區間取出 zombie thread，並在開始釋放記憶體前開啟中斷，避免消耗時間的釋放動作大幅增加中斷延遲時間。 

<!-- tracenote -->
在關閉中斷的區間取出 zombie thread，並在開始釋放記憶體前開啟中斷，避免消耗時間的釋放動作大幅增加中斷延遲時間。
<!-- /tracenote -->

```c 357:365:kernel/src/sched.c
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

#### 4. 回收 image/stack/sigpage/mmap 的 VMA 

<!-- tracenote -->
回收 image/stack/sigpage/mmap 的 VMA
<!-- /tracenote -->

```c 367:367:kernel/src/sched.c
thread_free_user_vm(z);
```

##### 1.  

```c 331:331:kernel/src/sched.c
void thread_free_user_vm(struct thread *t)
```

##### 2. 釋放 VMA list 中的所有 VMA，並清除特定區塊的指標為 NULL。 (Orphaned)

<!-- tracenote -->
釋放 VMA list 中的所有 VMA，並清除特定區塊的指標為 NULL。
<!-- /tracenote -->

```c 333:338:kernel/src/sched.c
/* VMAs own the image/stack/sigpage/mmap frame blocks; free them here.
 * The old per-region buddy_free calls are gone to avoid a double free. */
vma_unmap_all(t);
t->image_base      = NULL;
t->user_stack_base = NULL;
t->sigpage_base    = NULL;
```

###### 1.  

```c 640:642:kernel/src/mm.c
void vma_unmap_all(struct thread *t)
{
    vma_free_list(&t->vma_list);
```

###### 2.  

```c 624:624:kernel/src/mm.c
void vma_free_list(struct list_head *head)
```

## 3. fork 時，mmap region 的處理 

<!-- tracenote -->
fork 時，mmap region 的處理
<!-- /tracenote -->

### 1. 複製呼叫者行程，建立一個擁有獨立位址空間（私有 PGD、複製的 image/stack/signal page、複製的 mmap 區域）的子行程，並植入一個 trap frame 使子行程首次被排程時自 fork 後的指令繼續執行、回傳值為 0。 

<!-- tracenote -->
複製呼叫者行程，建立一個擁有獨立位址空間（私有 PGD、複製的 image/stack/signal page、複製的 mmap 區域）的子行程，並植入一個 trap frame 使子行程首次被排程時自 fork 後的指令繼續執行、回傳值為 0。
<!-- /tracenote -->

```c 615:616:kernel/src/syscall.c
case SYS_FORK:
    return sys_fork(tf);
```

### 2. 產生子 struct thread，使其位址空間與父行程內容一致但物理上隔離，並讓父子在同一 user VA 佈局下各自獨立執行。 

<!-- tracenote -->
產生子 struct thread，使其位址空間與父行程內容一致但物理上隔離，並讓父子在同一 user VA 佈局下各自獨立執行。
<!-- /tracenote -->

```c 235:235:kernel/src/syscall.c
static long sys_fork(struct trap_frame *tf)
```

### 3.  

```c 243:243:kernel/src/syscall.c
struct thread *ch = thread_alloc_bare();
```

#### 1.  

```c 137:137:kernel/src/sched.c
struct thread *thread_alloc_bare(void)
```

### 4. 為子行程建立完整的 VMA 清單——登錄三塊固定區（image/stack/sigpage），並對父行程每個 mmap 區域配置私有副本後映射到子行程相同的 user VA。 (Orphaned)

<!-- tracenote -->
為子行程建立完整的 VMA 清單——登錄三塊固定區（image/stack/sigpage），並對父行程每個 mmap 區域配置私有副本後映射到子行程相同的 user VA。
<!-- /tracenote -->

```c 409:410:kernel/src/syscall.c
if (fork_copy_vmas(par, ch, img, img_bytes, stk, stk_bytes, sig) != 0)
    goto fail_sig;
```

#### 1.  (Orphaned)

```c 248:250:kernel/src/syscall.c
static int fork_copy_vmas(struct thread *par, struct thread *ch,
                          void *img, unsigned long img_bytes,
                          void *stk, unsigned long stk_bytes, void *sig)
```

#### 2. 先為 image/stack/sigpage 各配一個 vma 節點 

<!-- tracenote -->
先為 image/stack/sigpage 各配一個 vma 節點
<!-- /tracenote -->

```c 509:516:kernel/src/sched.c
struct vma *vi = vma_alloc(USER_CODE_VA, img_bytes,
                           PROT_USER_RWX, img, 0);
struct vma *vs = vma_alloc(USER_STACK_TOP - stk_bytes, stk_bytes,
                           PROT_USER_DATA, stk, 0);
struct vma *vg = vma_alloc(SIGPAGE_VA, PAGE_SIZE,
                           PROT_USER_RWX, sig, 0);
if (!vi || !vs || !vg)
    goto fail_nodes;
```

#### 3. 走訪 par->vma_list，僅處理 is_mmap==1 的區域。 

<!-- tracenote -->
走訪 par->vma_list，僅處理 is_mmap==1 的區域。
<!-- /tracenote -->

```c 59:61:kernel/src/syscall.c
list_for_each(it, &par->vma_list) {
    struct vma *pv = list_entry(it, struct vma, link);
    if (!pv->is_mmap)
        continue;
```

#### 4. 每區配新 buddy 區塊 → 複製父位元組。 (Orphaned)

<!-- tracenote -->
每區配新 buddy 區塊 → 複製父位元組。
<!-- /tracenote -->

```c 282:285:kernel/src/syscall.c
void *nkva = buddy_alloc(pv->len);
if (!nkva)
    goto fail_copies;
mem_cpy(nkva, pv->kva, pv->len);
```

#### 5. 映射到子行程相同 pv->va。 

<!-- tracenote -->
映射到子行程相同 pv->va。
<!-- /tracenote -->

```c 728:732:kernel/src/mm.c
if (map_pages(ch->pgd, pv->va, pv->len,
              virt_to_phys(nkva), pv->prot) != 0) {
    buddy_free(nkva);
    goto fail_copies;
}
```

#### 6. 建 is_mmap=1 節點。 (Orphaned)

<!-- tracenote -->
建 is_mmap=1 節點。
<!-- /tracenote -->

```c 292:296:kernel/src/syscall.c
struct vma *nv = vma_alloc(pv->va, pv->len, pv->prot, nkva, 1);
if (!nv) {
    buddy_free(nkva);
    goto fail_copies;
}
```

#### 7. 只在成功複製 parent 所有 vma 後，才一次 link 到 child 的 vma list。 (Orphaned)

<!-- tracenote -->
只在成功複製 parent 所有 vma 後，才一次 link 到 child 的 vma list。
<!-- /tracenote -->

```c 302:309:kernel/src/syscall.c
vma_insert_sorted(ch, vi);
vma_insert_sorted(ch, vs);
vma_insert_sorted(ch, vg);
while (!list_empty(&copies)) {
    struct vma *v = list_entry(copies.next, struct vma, link);
    list_del(&v->link);
    vma_insert_sorted(ch, v);
}
```

### 5. 在子 kstack 頂端複製父 frame，僅改 a0 與 tp 

<!-- tracenote -->
在子 kstack 頂端複製父 frame，僅改 a0 與 tp
<!-- /tracenote -->

```c 272:280:kernel/src/syscall.c
uintptr_t top = (uintptr_t)ch->kstack_base + ch->kstack_size;
top &= ~0xFUL;
struct trap_frame *cf =
    (struct trap_frame *)(top - sizeof(struct trap_frame));
/* Use mem_cpy() rather than struct assignment so the compiler does
 * not lower it into a libc memcpy() call (we are -nostdlib). */
mem_cpy(cf, tf, sizeof(*cf));
cf->a0 = 0;                     /* fork returns 0 in child */
cf->tp = (uintptr_t)ch;
```

## 4. sys_exec 

<!-- tracenote -->
sys_exec
<!-- /tracenote -->

### 1.  

```c 613:614:kernel/src/syscall.c
case SYS_EXEC:
    return sys_exec((const char *)tf->a0, tf);
```

### 2.  

```c 138:138:kernel/src/syscall.c
static long sys_exec(const char *path, struct trap_frame *tf)
```

### 3. 確認呼叫者為 user process。 

<!-- tracenote -->
確認呼叫者為 user process。
<!-- /tracenote -->

```c 140:142:kernel/src/syscall.c
struct thread *self = get_current();
if (!self->pgd)
    return -1;
```

### 4. 透過 initrd 的 PA（轉成 VA 後 deref）以 cpio 查找檔案。 

<!-- tracenote -->
透過 initrd 的 PA（轉成 VA 後 deref）以 cpio 查找檔案。
<!-- /tracenote -->

```c 145:150:kernel/src/syscall.c
uintptr_t initrd_pa = dtb_getprop("/chosen", "linux,initrd-start");
const void *initrd = initrd_pa ? phys_to_virt(initrd_pa) : 0;
const void   *src;
unsigned long sz;
if (!initrd || cpio_find(initrd, path, &src, &sz) != 0)
    return -1;
```

### 5. 配置新 PGD 

<!-- tracenote -->
配置新 PGD
<!-- /tracenote -->

```c 157:159:kernel/src/syscall.c
unsigned long *new_pgd = pgd_alloc();
if (!new_pgd)
    return -1;
```

### 6. 把 image/stack/sigpage 及所有 mmap 區域整條 list 挪到 old_vmas，讓 uvm_setup_image() 在空 list 上重建 

<!-- tracenote -->
把 image/stack/sigpage 及所有 mmap 區域整條 list 挪到 old_vmas，讓 uvm_setup_image() 在空 list 上重建
<!-- /tracenote -->

```c 166:168:kernel/src/syscall.c
struct list_head old_vmas;
INIT_LIST_HEAD(&old_vmas);
vma_detach_all(self, &old_vmas);
```

#### 1.  

```c 655:655:kernel/src/mm.c
void vma_detach_all(struct thread *t, struct list_head *dst)
```

### 7. 為一個已備妥 PGD 的 thread 建立完整的 user 位址空間：從 buddy allocator 配置 image、stack、signal page 三塊實體連續記憶體，載入程式內容，並以全有全無的方式把它們映射到固定 user VA 並登記為 VMA。 

<!-- tracenote -->
為一個已備妥 PGD 的 thread 建立完整的 user 位址空間：從 buddy allocator 配置 image、stack、signal page 三塊實體連續記憶體，載入程式內容，並以全有全無的方式把它們映射到固定 user VA 並登記為 VMA。
<!-- /tracenote -->

```c 172:172:kernel/src/syscall.c
if (uvm_setup_image(self, src, sz) != 0) {
```

#### 1. 在 t->pgd 中映射好 image（USER_CODE_VA）、stack（USER_STACK_TOP 下方）、signal page（SIGPAGE_VA）三個固定區段，更新 t 的 VM 記帳欄位，並回傳 0；任何中途失敗都完整回滾，回傳 -1。 

<!-- tracenote -->
在 t->pgd 中映射好 image（USER_CODE_VA）、stack（USER_STACK_TOP 下方）、signal page（SIGPAGE_VA）三個固定區段，更新 t 的 VM 記帳欄位，並回傳 0；任何中途失敗都完整回滾，回傳 -1。
<!-- /tracenote -->

```c 549:549:kernel/src/sched.c
int uvm_setup_image(struct thread *t, const void *src, unsigned long sz)
```

#### 2. 把使用者行程的三個固定區段（image、stack、signal page）登記為 VMA 節點並排序插入 t->vma_list，讓它們之後與 mmap 區段被統一管理。 (Orphaned)

<!-- tracenote -->
把使用者行程的三個固定區段（image、stack、signal page）登記為 VMA 節點並排序插入 t->vma_list，讓它們之後與 mmap 區段被統一管理。
<!-- /tracenote -->

```c 597:598:kernel/src/sched.c
if (vma_register_fixed(t, img, img_bytes, stk, stk_bytes, sig) != 0)
    goto fail;
```

##### 1. 產生三個 struct vma（is_mmap=0），分別描述 image、stack、signal page 的 user VA、長度、PTE prot 與後備 buddy 區塊的 kernel VA，並按 VA 升冪插入 t->vma_list。 (Orphaned)

<!-- tracenote -->
產生三個 struct vma（is_mmap=0），分別描述 image、stack、signal page 的 user VA、長度、PTE prot 與後備 buddy 區塊的 kernel VA，並按 VA 升冪插入 t->vma_list。
<!-- /tracenote -->

```c 511:513:kernel/src/sched.c
static int vma_register_fixed(struct thread *t, void *img,
                              unsigned long img_bytes, void *stk,
                              unsigned long stk_bytes, void *sig)
```

#### 3. 重置 t->mmap_top = MMAP_CURSOR_INIT，讓 mmap 自選位址從 signal page 下方一個 guard page 起向下成長。 

<!-- tracenote -->
重置 t->mmap_top = MMAP_CURSOR_INIT，讓 mmap 自選位址從 signal page 下方一個 guard page 起向下成長。
<!-- /tracenote -->

```c 586:586:kernel/src/sched.c
t->mmap_top = MMAP_CURSOR_INIT;
```

### 8. 把 image/stack/sigpage 及所有 mmap 區域整條 list 挪回 self->vma_list。 

<!-- tracenote -->
把 image/stack/sigpage 及所有 mmap 區域整條 list 挪回 self->vma_list。
<!-- /tracenote -->

```c 178:178:kernel/src/syscall.c
vma_reattach(self, &old_vmas);
```

