# Trace Result - 2026-08-19

## 1. 相對路徑 open：open("rel.txt", O_CREAT) 

<!-- tracenote -->
相對路徑 open：open("rel.txt", O_CREAT)
- 情境：某個 user process 先前已呼叫 chdir("/dir")，現在以相對路徑建立檔案。
<!-- /tracenote -->

### 1.  

```c 144:144:kernel/src/trap.c
if (cause == EXC_ECALL_U) {
```

### 2.  

```c 156:156:kernel/src/trap.c
tf->a0 = (uintptr_t)do_syscall(tf);
```

### 3.  

```c 912:912:kernel/src/syscall.c
long do_syscall(struct trap_frame *tf)
```

### 4.  

```c 946:947:kernel/src/syscall.c
case SYS_OPEN:
    return sys_open((const char *)tf->a0, (int)tf->a1);
```

### 5.  

```c 734:734:kernel/src/syscall.c
static long sys_open(const char *pathname, int flags)
```

### 6. 將使用者空間傳入的 NUL-terminated 路徑字串，以「逐位元組先驗證再讀取」的方式安全複製進 kernel 的定長緩衝區。 

<!-- tracenote -->
將使用者空間傳入的 NUL-terminated 路徑字串，以「逐位元組先驗證再讀取」的方式安全複製進 kernel 的定長緩衝區。
<!-- /tracenote -->

```c 739:739:kernel/src/syscall.c
int ret = copy_path_from_user(pathname, path, sizeof(path));
```

#### 1. 產出一份保證 NUL-terminated、長度不超過 sz 的 kernel 端路徑副本；任何一個位元組落在 caller 的 VMA 之外就以 -EFAULT 中止，字串在 sz 內未終止則回 -ENAMETOOLONG。成功時回 0，kbuf 才可交給 VFS 層。 

<!-- tracenote -->
產出一份保證 NUL-terminated、長度不超過 sz 的 kernel 端路徑副本；任何一個位元組落在 caller 的 VMA 之外就以 -EFAULT 中止，字串在 sz 內未終止則回 -ENAMETOOLONG。成功時回 0，kbuf 才可交給 VFS 層。
<!-- /tracenote -->

```c 84:84:kernel/src/syscall.c
static int copy_path_from_user(const char *upath, char *kbuf, size_t sz)
```

#### 2. - 以 in_user_range(upath + i, 1) 對單一位元組做 VMA 涵蓋檢查，而非對整段字串 

<!-- tracenote -->
- 以 in_user_range(upath + i, 1) 對單一位元組做 VMA 涵蓋檢查，而非對整段字串
- 檢查通過後才 kbuf[i] = upath[i]，讀到 '\0' 立即回 0（NUL 本身已被複製進 kbuf）
<!-- /tracenote -->

```c 89:94:kernel/src/syscall.c
for (size_t i = 0; i < sz; i++) {
    if (!in_user_range(upath + i, 1))
        return -EFAULT;
    kbuf[i] = upath[i];
    if (kbuf[i] == '\0')
        return 0;
```

#### 3. 迴圈上限 sz 同時是終止保證：跑滿 sz 個位元組仍未見 NUL → -ENAMETOOLONG，kbuf 內容視為無效（不保證終止），呼叫端因 ret != 0 而不會使用它 

<!-- tracenote -->
迴圈上限 sz 同時是終止保證：跑滿 sz 個位元組仍未見 NUL → -ENAMETOOLONG，kbuf 內容視為無效（不保證終止），呼叫端因 ret != 0 而不會使用它
<!-- /tracenote -->

```c 96:96:kernel/src/syscall.c
return -ENAMETOOLONG;
```

### 7. 把一條（相對於 start 目錄或絕對的）路徑解析成葉節點 vnode、必要時依 O_CREAT 建立該檔，最後透過檔案系統配置並初始化一個 open file description 交還給呼叫端。 

<!-- tracenote -->
把一條（相對於 start 目錄或絕對的）路徑解析成葉節點 vnode、必要時依 O_CREAT 建立該檔，最後透過檔案系統配置並初始化一個 open file description 交還給呼叫端。
<!-- /tracenote -->

```c 744:744:kernel/src/syscall.c
ret = vfs_open_at(self->cwd, path, flags, &file);
```

#### 1. 回傳一個 *target 指向的 struct file，其 vnode / f_ops 已綁定葉節點，且 f_pos = 0、flags、ref_count = 1 由 generic layer 填妥；失敗時回傳負的 errno 且不改動 *target。 

<!-- tracenote -->
回傳一個 *target 指向的 struct file，其 vnode / f_ops 已綁定葉節點，且 f_pos = 0、flags、ref_count = 1 由 generic layer 填妥；失敗時回傳負的 errno 且不改動 *target。
<!-- /tracenote -->

```c 274:275:kernel/src/vfs.c
int vfs_open_at(struct vnode *start, const char *pathname, int flags,
                struct file **target)
```

#### 2. %%Tip%% Phase 1：解析到 parent directory 並取出 leaf 名稱 

<!-- tracenote -->
Phase 1：解析到 parent directory 並取出 leaf 名稱
- 先驗證輸出參數，再以 stop_at_parent = 1 呼叫 vfs_walk()，讓走訪停在最後一層元件的父目錄。
<!-- /tracenote -->

#### 3. VFS 唯一的路徑解析引擎，把一條絕對或相對路徑逐段拆解、跨越沿途所有 mount point，解析成最終 vnode（或最後一段的父目錄加上該段名稱）。 

<!-- tracenote -->
VFS 唯一的路徑解析引擎，把一條絕對或相對路徑逐段拆解、跨越沿途所有 mount point，解析成最終 vnode（或最後一段的父目錄加上該段名稱）。
<!-- /tracenote -->

```c 282:282:kernel/src/vfs.c
int ret = vfs_walk(start, pathname, 1, &dir, leaf, sizeof(leaf));
```

##### 1. 輸出 *result（完整解析結果，或 stop_at_parent 時的父目錄）與 leaf（最後一段名稱）；失敗時回傳 -EINVAL / -ENAMETOOLONG / -ENOENT / -ENOTDIR。 

<!-- tracenote -->
輸出 *result（完整解析結果，或 stop_at_parent 時的父目錄）與 leaf（最後一段名稱）；失敗時回傳 -EINVAL / -ENAMETOOLONG / -ENOENT / -ENOTDIR。
<!-- /tracenote -->

```c 138:140:kernel/src/vfs.c
static int vfs_walk(struct vnode *start, const char *pathname,
                    int stop_at_parent, struct vnode **result,
                    char *leaf, size_t leaf_sz)
```

##### 2. %%Tip%% Phase 1：長度檢查與起始點決定 

<!-- tracenote -->
Phase 1：長度檢查與起始點決定
- 先線性掃描長度上限，再依首字元 / 決定從 root 或 start 出發。
<!-- /tracenote -->

##### 3. 以計數迴圈取代 strlen()，超過 VFS_MAX_PATHNAME（255）即 -ENAMETOOLONG 

<!-- tracenote -->
以計數迴圈取代 strlen()，超過 VFS_MAX_PATHNAME（255）即 -ENAMETOOLONG
<!-- /tracenote -->

```c 150:153:kernel/src/vfs.c
while (pathname[path_len] != '\0') {
    if (++path_len > VFS_MAX_PATHNAME)
        return -ENAMETOOLONG;
}
```

##### 4. 絕對路徑無條件從 g_rootfs->root 出發並吃掉前導 / 

<!-- tracenote -->
絕對路徑無條件從 g_rootfs->root 出發並吃掉前導 /
<!-- /tracenote -->

```c 158:160:kernel/src/vfs.c
if (*cur == '/') {
    dir = g_rootfs->root;
    cur++;                      /* skip the leading '/' */
```

##### 5. 相對路徑用 caller 傳入的 start（即 thread->cwd） 

<!-- tracenote -->
相對路徑用 caller 傳入的 start（即 thread->cwd）
<!-- /tracenote -->

```c 161:162:kernel/src/vfs.c
} else {
    dir = start ? start : g_rootfs->root;
```

##### 6. 起始點本身也可能被 mount 蓋住，故先 vfs_follow_mount() 一次 

<!-- tracenote -->
起始點本身也可能被 mount 蓋住，故先 vfs_follow_mount() 一次
<!-- /tracenote -->

```c 166:166:kernel/src/vfs.c
dir = vfs_follow_mount(dir);
```

##### 7. %%Tip%% Phase 2：空路徑的 early return 

<!-- tracenote -->
Phase 2：空路徑的 early return
<!-- /tracenote -->

##### 8. - "/" 解析為 root、"" 解析為 start 本身，兩者都是合法 vnode 

<!-- tracenote -->
- "/" 解析為 root、"" 解析為 start 本身，兩者都是合法 vnode
- 但都沒留下「最後一段」可供建立條目，所以 stop_at_parent 模式回 -EINVAL
<!-- /tracenote -->

```c 173:177:kernel/src/vfs.c
if (*cur == '\0') {
    if (stop_at_parent)
        return -EINVAL;
    *result = dir;
    return 0;
```

##### 9. %%Tip%% Phase 3：component 切分與空段折疊 

<!-- tracenote -->
Phase 3：component 切分與空段折疊
- 主迴圈每輪量出一段名稱長度，長度為 0 表示遇到 // 或結尾 /。
<!-- /tracenote -->

##### 10. - 空段代表「不移動」，直接跳過 

<!-- tracenote -->
- 空段代表「不移動」，直接跳過
- 路徑以分隔符收尾時，目前走到的目錄即答案
<!-- /tracenote -->

```c 189:199:kernel/src/vfs.c
if (len == 0) {
    cur++;
    if (*cur == '\0') {
        /* The path ended on separators, e.g. "a/" or "/". The
         * directory reached so far is the answer. */
        if (stop_at_parent)
            return -EINVAL;
        *result = dir;
        return 0;
    }
    continue;
```

##### 11. %%Tip%% Phase 4：末段判定與 stop_at_parent 出口 

<!-- tracenote -->
Phase 4：末段判定與 stop_at_parent 出口
- 跳過該段後方連續的 /，藉此同時得知「是否為最後一段」與「是否帶尾隨分隔符」。
<!-- /tracenote -->

##### 12. has_trailing_sep 表示 caller 宣稱該名稱是目錄；此時它不能當作 open/create 的 leaf，避免 "f/" 悄悄變成 "f" 

<!-- tracenote -->
has_trailing_sep 表示 caller 宣稱該名稱是目錄；此時它不能當作 open/create 的 leaf，避免 "f/" 悄悄變成 "f"
<!-- /tracenote -->

```c 210:210:kernel/src/vfs.c
int has_trailing_sep = (skip != len);
```

##### 13. 名稱以逐字元複製方式寫入 leaf 並補 NUL，因為 pathname 內部沒有終止符 

<!-- tracenote -->
名稱以逐字元複製方式寫入 leaf 並補 NUL，因為 pathname 內部沒有終止符
<!-- /tracenote -->

```c 222:224:kernel/src/vfs.c
for (size_t i = 0; i < len; i++)
    leaf[i] = cur[i];
leaf[len] = '\0';
```

##### 14. *result 是父目錄，caller（如 vfs_open_at()）接手自行 lookup / create 

<!-- tracenote -->
*result 是父目錄，caller（如 vfs_open_at()）接手自行 lookup / create
<!-- /tracenote -->

```c 225:225:kernel/src/vfs.c
*result = dir;
```

##### 15. %%Tip%% Phase 5：.. 逃離 mount 與 lookup 下降 

<!-- tracenote -->
Phase 5：.. 逃離 mount 與 lookup 下降
<!-- /tracenote -->

##### 16. .. 先 vfs_escape_mount() 跳到 mountpoint，讓擁有 mountpoint 的那個 fs 回答 ..；否則 tmpfs_lookup() 對 fs root 的 .. 會回自己（自環）而困在 mount 內 

<!-- tracenote -->
.. 先 vfs_escape_mount() 跳到 mountpoint，讓擁有 mountpoint 的那個 fs 回答 ..；否則 tmpfs_lookup() 對 fs root 的 .. 會回自己（自環）而困在 mount 內
- mountpoint：被掛載的節點
<!-- /tracenote -->

```c 236:237:kernel/src/vfs.c
if (comp[0] == '.' && comp[1] == '.' && comp[2] == '\0')
    dir = vfs_escape_mount(dir);
```

###### 1. 輸入一個 vnode，回傳「應該用來解析 .. 的 vnode」——把所有以該 vnode 為 root 的 mount 逐層剝除，停在最外層的 mountpoint 或 rootfs root。 

<!-- tracenote -->
輸入一個 vnode，回傳「應該用來解析 .. 的 vnode」——把所有以該 vnode 為 root 的 mount 逐層剝除，停在最外層的 mountpoint 或 rootfs root。
<!-- /tracenote -->

```c 96:96:kernel/src/vfs.c
static struct vnode *vfs_escape_mount(struct vnode *node)
```

###### 2. 單一 while 迴圈，每輪把 node 換成其所屬 mount 的 mountpoint 

<!-- tracenote -->
單一 while 迴圈，每輪把 node 換成其所屬 mount 的 mountpoint
- node->mount->root == node：確認目前站在 mount 的 root，中間目錄不受影響
  - root: 掛載其他節點的
- node->mount->mountpoint：rootfs 的 mountpoint 由 vfs_init() 設為 NULL（vfs.c:631），迴圈自然停住，這正是 /.. 停留在 / 的原因
<!-- /tracenote -->

```c 98:100:kernel/src/vfs.c
while (node && node->mount && node->mount->root == node &&
       node->mount->mountpoint)
    node = node->mount->mountpoint;
```

##### 17. 缺 v_ops->lookup 代表這不是目錄 → -ENOTDIR；lookup 失敗 → -ENOENT 

<!-- tracenote -->
缺 v_ops->lookup 代表這不是目錄 → -ENOTDIR；lookup 失敗 → -ENOENT
- tmpfs_lookup() 在一個 tmpfs 目錄的固定大小 entry 陣列中，以名稱精確比對出單一路徑元件對應的 struct vnode，並自行回答 . 與 .. 這兩個從不實際存在於陣列中的名稱。
<!-- /tracenote -->

```c 240:243:kernel/src/vfs.c
if (!dir->v_ops || !dir->v_ops->lookup)
    return -ENOTDIR;
if (dir->v_ops->lookup(dir, &next, comp) != 0 || !next)
    return -ENOENT;
```

###### 1. 將 (目錄 vnode, 元件名稱) 轉為 (子 vnode, 0)；名稱不存在或 dir_node 並非目錄時回傳 -1 且不改動 *target。 

<!-- tracenote -->
將 (目錄 vnode, 元件名稱) 轉為 (子 vnode, 0)；名稱不存在或 dir_node 並非目錄時回傳 -1 且不改動 *target。
<!-- /tracenote -->

```c 158:159:kernel/src/tmpfs.c
static int tmpfs_lookup(struct vnode *dir_node, struct vnode **target,
                        const char *component_name)
```

###### 2. . 直接回傳 dir_node 自身 

<!-- tracenote -->
. 直接回傳 dir_node 自身
<!-- /tracenote -->

```c 168:170:kernel/src/tmpfs.c
if (str_eq(component_name, ".")) {
    *target = dir_node;
    return 0;
```

###### 3. .. 讀 dir->parent->vnode 

<!-- tracenote -->
.. 讀 dir->parent->vnode
- parent 由 tmpfs_add_entry() 設為擁有者目錄
- 檔案系統根節點的 parent 被 tmpfs_alloc_node() 指向自己（tmpfs.c:121），這個自環讓 /.. 原地不動，無需在此加特例
- 三元運算的 NULL fallback 只防禦結構異常的節點，正常路徑不會取到
<!-- /tracenote -->

```c 173:177:kernel/src/tmpfs.c
if (str_eq(component_name, "..")) {
    /* A node built by tmpfs always has a parent (its root points at
     * itself), so the fallback only guards a malformed node. */
    *target = dir->parent ? dir->parent->vnode : dir_node;
    return 0;
```

##### 18. 把一個路徑走訪剛取得的 vnode 換成「覆蓋在它之上的檔案系統根目錄」，讓一次 lookup 能自動跨越掛載點。 

<!-- tracenote -->
把一個路徑走訪剛取得的 vnode 換成「覆蓋在它之上的檔案系統根目錄」，讓一次 lookup 能自動跨越掛載點。


<!-- /tracenote -->

```c 245:245:kernel/src/vfs.c
dir = vfs_follow_mount(next);
```

###### 1. 輸入一個可能是掛載點的 vnode，回傳該名稱真正所指的 vnode——即穿透所有覆蓋層後最內層的 mount root；若無掛載則原樣返回。 

<!-- tracenote -->
輸入一個可能是掛載點的 vnode，回傳該名稱真正所指的 vnode——即穿透所有覆蓋層後最內層的 mount root；若無掛載則原樣返回。
<!-- /tracenote -->

```c 73:73:kernel/src/vfs.c
static struct vnode *vfs_follow_mount(struct vnode *node)
```

##### 19. 末段若帶尾隨 / 卻不是目錄 → -ENOTDIR，尊重 caller 的型別宣稱 

<!-- tracenote -->
末段若帶尾隨 / 卻不是目錄 → -ENOTDIR，尊重 caller 的型別宣稱
<!-- /tracenote -->

```c 247:253:kernel/src/vfs.c
if (is_last) {
    /* "f/" named a directory; honour that claim rather than
     * resolving it to the regular file f. */
    if (has_trailing_sep && dir->type != VNODE_TYPE_DIR)
        return -ENOTDIR;
    *result = dir;
    return 0;
```

#### 4. dir 是 vfs_walk() 交回來的路徑前綴的最後一層（parent directory）。若它沒有 lookup，代表使用者給的路徑把一個「非目錄」當成目錄在穿越，例如 open("/f/g") 而 /f 是普通檔案，因此在這樣的情況下回傳 ENOTDIR，不是 EINVAL 或 EPERM。 

<!-- tracenote -->
dir 是 vfs_walk() 交回來的路徑前綴的最後一層（parent directory）。若它沒有 lookup，代表使用者給的路徑把一個「非目錄」當成目錄在穿越，例如 open("/f/g") 而 /f 是普通檔案，因此在這樣的情況下回傳 ENOTDIR，不是 EINVAL 或 EPERM。
<!-- /tracenote -->

```c 285:286:kernel/src/vfs.c
if (!dir->v_ops || !dir->v_ops->lookup)
    return -ENOTDIR;
```

#### 5. %%Tip%% Phase 2：leaf lookup 與 O_CREAT 分岔 

<!-- tracenote -->
Phase 2：leaf lookup 與 O_CREAT 分岔
- 向父目錄查詢 leaf；查不到時，「不存在」被當成正常控制流而非錯誤，交由 O_CREAT 決定行為。
<!-- /tracenote -->

#### 6. 找到既有節點時再跑一次 vfs_follow_mount()：leaf 本身可能是 mount point，此時該名字實際指向被掛載 fs 的 root 

<!-- tracenote -->
找到既有節點時再跑一次 vfs_follow_mount()：leaf 本身可能是 mount point，此時該名字實際指向被掛載 fs 的 root
<!-- /tracenote -->

```c 289:292:kernel/src/vfs.c
if (dir->v_ops->lookup(dir, &node, leaf) == 0 && node) {
    /* The leaf may itself be a mount point, in which case the name
     * denotes the mounted file system's root. */
    node = vfs_follow_mount(node);
```

#### 7. 找不到既有節點 

<!-- tracenote -->
找不到既有節點
- 無 O_CREAT → -ENOENT
- 檔案系統未提供 create（唯讀 fs）→ -EPERM，generic layer 不需知道該 fs 的讀寫性質
- create 失敗一律折成 -ENOSPC
<!-- /tracenote -->

```c 293:301:kernel/src/vfs.c
} else {
    /* A failed lookup is ordinary control flow, not an error to
     * report: it is exactly how O_CREAT decides to create. */
    if (!(flags & O_CREAT))
        return -ENOENT;
    if (!dir->v_ops->create)
        return -EPERM;
    if (dir->v_ops->create(dir, &node, leaf) != 0 || !node)
        return -ENOSPC;
```

#### 8. %%Tip%% Phase 3：型別與 f_ops 檢查、handle 初始化 

<!-- tracenote -->
Phase 3：型別與 f_ops 檢查、handle 初始化
- 葉節點確定後，先擋掉目錄，再委派 fs 配置 struct file，最後由 generic layer 補齊共用欄位。
<!-- /tracenote -->

#### 9. 開檔，包含配置 file handler 並初始化其欄位。 

<!-- tracenote -->
開檔，包含配置 file handler 並初始化其欄位。
- ref_count = 1 對應第一個 descriptor；sys_fork() 每繼承一個 slot 就 vfs_file_get() 加一
<!-- /tracenote -->

```c 312:320:kernel/src/vfs.c
struct file *file = NULL;
if (node->f_ops->open(node, &file) != 0 || !file)
    return -ENOMEM;

file->vnode = node;
file->f_ops = node->f_ops;
file->f_pos = 0;
file->flags = flags;
file->ref_count = 1;
```

##### 1.  

```c 278:278:kernel/src/tmpfs.c
static int tmpfs_open(struct vnode *file_node, struct file **target)
```

### 8. 綁定 file handler 到 fd table。 

<!-- tracenote -->
綁定 file handler 到 fd table。
<!-- /tracenote -->

```c 748:748:kernel/src/syscall.c
int fd = fd_alloc(self, file);
```

## 2. 跨 mount 的 ..：open("/mnt/../dir/rel.txt", 0) 

<!-- tracenote -->
跨 mount 的 ..：open("/mnt/../dir/rel.txt", 0)
- 情境：/mnt 上已 mount 一個新的 tmpfs。使用者從 mount 內部往上一層走。
<!-- /tracenote -->

### 1. %%Tip%% 情境：/mnt 上已 mount 一個新的 tmpfs。使用者從 mount 內部往上一層走。 

<!-- tracenote -->
情境：/mnt 上已 mount 一個新的 tmpfs。使用者從 mount 內部往上一層走。
- 這是本次最關鍵的設計點——若未特別處理，.. 會錯誤地停在 mounted fs 的 root。
<!-- /tracenote -->

### 2.  

```c 734:734:kernel/src/syscall.c
static long sys_open(const char *pathname, int flags)
```

### 3.  

```c 744:744:kernel/src/syscall.c
ret = vfs_open_at(self->cwd, path, flags, &file);
```

#### 1.  

```c 274:275:kernel/src/vfs.c
int vfs_open_at(struct vnode *start, const char *pathname, int flags,
                struct file **target)
```

#### 2.  

```c 282:282:kernel/src/vfs.c
int ret = vfs_walk(start, pathname, 1, &dir, leaf, sizeof(leaf));
```

#### 3.  

```c 138:140:kernel/src/vfs.c
static int vfs_walk(struct vnode *start, const char *pathname,
                    int stop_at_parent, struct vnode **result,
                    char *leaf, size_t leaf_sz)
```

#### 4. %%Tip%% component "mnt" 

<!-- tracenote -->
component "mnt"
<!-- /tracenote -->

#### 5. 呼叫 tmpfs_lookup(root, "mnt")，把 next 指向 /mnt 的 vnode（rootfs 內的那一個） 

<!-- tracenote -->
呼叫 tmpfs_lookup(root, "mnt")，把 next 指向 /mnt 的 vnode（rootfs 內的那一個）
<!-- /tracenote -->

```c 242:243:kernel/src/vfs.c
if (dir->v_ops->lookup(dir, &next, comp) != 0 || !next)
    return -ENOENT;
```

##### 1.  

```c 158:159:kernel/src/tmpfs.c
static int tmpfs_lookup(struct vnode *dir_node, struct vnode **target,
                        const char *component_name)
```

#### 6. node->mounted != NULL → dir = mounted->root 

<!-- tracenote -->
node->mounted != NULL → dir = mounted->root
- 此刻 dir 已是「mounted tmpfs 的 root」，其 parent 指向自己
<!-- /tracenote -->

```c 245:245:kernel/src/vfs.c
dir = vfs_follow_mount(next);
```

#### 7. %%Tip%% component ".." 

<!-- tracenote -->
component ".."
<!-- /tracenote -->

#### 8. 先脫離 mount，再解 ".." 

<!-- tracenote -->
先脫離 mount，再解 ".."
- 回到 rootfs 內的 /mnt vnode（被覆蓋的那一個）
<!-- /tracenote -->

```c 237:237:kernel/src/vfs.c
dir = vfs_escape_mount(dir);
```

##### 1.  

```c 158:159:kernel/src/tmpfs.c
static int tmpfs_lookup(struct vnode *dir_node, struct vnode **target,
                        const char *component_name)
```

#### 9. tmpfs_lookup(dir="/mnt", "..") → dir->parent->vnode == "/" 

<!-- tracenote -->
tmpfs_lookup(dir="/mnt", "..") → dir->parent->vnode == "/"
<!-- /tracenote -->

```c 242:243:kernel/src/vfs.c
if (dir->v_ops->lookup(dir, &next, comp) != 0 || !next)
    return -ENOENT;
```

##### 1.  

```c 158:159:kernel/src/tmpfs.c
static int tmpfs_lookup(struct vnode *dir_node, struct vnode **target,
                        const char *component_name)
```

## 3. fork 的 fd 繼承與 refcount 生命週期 

<!-- tracenote -->
fork 的 fd 繼承與 refcount 生命週期
- 情境：parent 先 open() 一個檔案，接著 fork()；父子最終各自結束。
<!-- /tracenote -->

### 1. SYS_FORK 

<!-- tracenote -->
SYS_FORK
<!-- /tracenote -->

#### 1.  

```c 156:156:kernel/src/trap.c
tf->a0 = (uintptr_t)do_syscall(tf);
```

#### 2.  

```c 923:924:kernel/src/syscall.c
case SYS_FORK:
    return sys_fork(tf);
```

#### 3.  

```c 327:327:kernel/src/syscall.c
static long sys_fork(struct trap_frame *tf)
```

#### 4. 配置 child thread，並初始化各欄位，包含設定 fd table 跟 thread 的當前路徑。 

<!-- tracenote -->
配置 child thread，並初始化各欄位，包含設定 fd table 跟 thread 的當前路徑。
<!-- /tracenote -->

```c 335:335:kernel/src/syscall.c
struct thread *ch = thread_alloc_bare();
```

##### 1.  

```c 137:137:kernel/src/sched.c
struct thread *thread_alloc_bare(void)
```

##### 2. fd_table 全 NULL 

<!-- tracenote -->
fd_table 全 NULL
<!-- /tracenote -->

```c 148:148:kernel/src/sched.c
zero_words(t, sizeof(*t));
```

##### 3. 繼承 creator 當前的路徑 

<!-- tracenote -->
繼承 creator 當前的路徑
<!-- /tracenote -->

```c 155:157:kernel/src/sched.c
/* Inherit the creator's working directory; the fd table stays empty
 * (zeroed above) because only fork() duplicates open files. */
t->cwd         = t->parent ? t->parent->cwd : NULL;
```

#### 5. 將 file handler 的 ref cnt + 1。 

<!-- tracenote -->
將 file handler 的 ref cnt + 1。
<!-- /tracenote -->

```c 405:405:kernel/src/syscall.c
vfs_file_get(f);
```

##### 1.  

```c 345:345:kernel/src/vfs.c
void vfs_file_get(struct file *file)
```

#### 6. 複製 parent 的 file handler。 

<!-- tracenote -->
複製 parent 的 file handler。
<!-- /tracenote -->

```c 406:406:kernel/src/syscall.c
ch->fd_table[fd] = f;
```

### 2. 子程序結束 

<!-- tracenote -->
子程序結束
<!-- /tracenote -->

#### 1.  

```c 536:538:kernel/src/syscall.c
static void sys_exit(long status)
{
    do_exit(status);
```

#### 2.  

```c 488:488:kernel/src/syscall.c
void do_exit(long status)
```

#### 3. 更新 fd table 所有 file handler 的 ref cnt，並且 ref cnt 等於 0 時釋放 file handler 的空間。 

<!-- tracenote -->
更新 fd table 所有 file handler 的 ref cnt，並且 ref cnt 等於 0 時釋放 file handler 的空間。
<!-- /tracenote -->

```c 498:498:kernel/src/syscall.c
fd_table_close_all(self);
```

##### 1.  

```c 145:145:kernel/src/syscall.c
static void fd_table_close_all(struct thread *t)
```

##### 2. 更新 file handler 的 ref_count，並且在沒有 thread 開啟特定檔案時，釋放該檔案的 handler。 

<!-- tracenote -->
更新 file handler 的 ref_count，並且在沒有 thread 開啟特定檔案時，釋放該檔案的 handler。
<!-- /tracenote -->

```c 152:152:kernel/src/syscall.c
vfs_close(f);
```

##### 3.  

```c 361:361:kernel/src/vfs.c
int vfs_close(struct file *file)
```

##### 4. 還有 thread 紀錄 file handler 時直接回傳 0。 

<!-- tracenote -->
還有 thread 紀錄 file handler 時直接回傳 0。
<!-- /tracenote -->

```c 366:367:kernel/src/vfs.c
if (--file->ref_count > 0)
    return 0;
```

##### 5. 釋放 file handler 的空間。 

<!-- tracenote -->
釋放 file handler 的空間。
<!-- /tracenote -->

```c 369:369:kernel/src/vfs.c
return file->f_ops->close(file) == 0 ? 0 : -EIO;
```

###### 1.  

```c 304:304:kernel/src/tmpfs.c
static int tmpfs_close(struct file *file)
```

###### 2. 釋放 file handler 的空間。 

<!-- tracenote -->
釋放 file handler 的空間。
<!-- /tracenote -->

```c 309:309:kernel/src/tmpfs.c
kfree(file);
```

## 4. SYS_CLOSE 

<!-- tracenote -->
SYS_CLOSE
<!-- /tracenote -->

```c 948:949:kernel/src/syscall.c
case SYS_CLOSE:
    return sys_close((int)tf->a0);
```

### 1.  

```c 764:764:kernel/src/syscall.c
static long sys_close(int fd)
```

## 5. SYS_READ 

<!-- tracenote -->
SYS_READ
<!-- /tracenote -->

```c 950:952:kernel/src/syscall.c
case SYS_READ:
    return sys_read((int)tf->a0, (void *)tf->a1,
                    (unsigned long)tf->a2);
```

### 1.  

```c 783:783:kernel/src/syscall.c
static long sys_read(int fd, void *buf, unsigned long count)
```

### 2.  

```c 795:795:kernel/src/syscall.c
return vfs_read(file, buf, (size_t)count);
```

### 3.  

```c 396:396:kernel/src/vfs.c
int vfs_read(struct file *file, void *buf, size_t len)
```

### 4.  

```c 400:400:kernel/src/vfs.c
int ret = file->f_ops->read(file, buf, len);
```

#### 1.  

```c 374:374:kernel/src/tmpfs.c
static int tmpfs_read(struct file *file, void *buf, size_t len)
```

## 6. SYS_WRITE 

<!-- tracenote -->
SYS_WRITE
<!-- /tracenote -->

```c 953:955:kernel/src/syscall.c
case SYS_WRITE:
    return sys_write((int)tf->a0, (const void *)tf->a1,
                     (unsigned long)tf->a2);
```

### 1.  

```c 805:805:kernel/src/syscall.c
static long sys_write(int fd, const void *buf, unsigned long count)
```

### 2.  

```c 817:817:kernel/src/syscall.c
return vfs_write(file, buf, (size_t)count);
```

### 3.  

```c 380:380:kernel/src/vfs.c
int vfs_write(struct file *file, const void *buf, size_t len)
```

### 4.  

```c 384:384:kernel/src/vfs.c
int ret = file->f_ops->write(file, buf, len);
```

### 5.  

```c 326:326:kernel/src/tmpfs.c
static int tmpfs_write(struct file *file, const void *buf, size_t len)
```

## 7. SYS_MKDIR 

<!-- tracenote -->
SYS_MKDIR
<!-- /tracenote -->

```c 956:957:kernel/src/syscall.c
case SYS_MKDIR:
    return sys_mkdir((const char *)tf->a0, (unsigned int)tf->a1);
```

### 1.  

```c 829:829:kernel/src/syscall.c
static long sys_mkdir(const char *pathname, unsigned int mode)
```

### 2.  

```c 840:840:kernel/src/syscall.c
return vfs_mkdir_at(self->cwd, path);
```

### 3.  

```c 417:417:kernel/src/vfs.c
int vfs_mkdir_at(struct vnode *start, const char *pathname)
```

### 4.  

```c 440:441:kernel/src/vfs.c
if (dir->v_ops->mkdir(dir, &node, leaf) != 0 || !node)
    return -ENOSPC;
```

### 5.  

```c 263:264:kernel/src/tmpfs.c
static int tmpfs_mkdir(struct vnode *dir_node, struct vnode **target,
                       const char *component_name)
```

### 6.  

```c 207:209:kernel/src/tmpfs.c
static int tmpfs_add_entry(struct vnode *dir_node, struct vnode **target,
                           const char *component_name,
                           enum tmpfs_type type)
```

## 8.  

```c 958:961:kernel/src/syscall.c
case SYS_MOUNT:
    return sys_mount((const char *)tf->a0, (const char *)tf->a1,
                     (const char *)tf->a2, (unsigned long)tf->a3,
                     (const void *)tf->a4);
```

### 1.  

```c 856:858:kernel/src/syscall.c
static long sys_mount(const char *src, const char *target,
                      const char *filesystem, unsigned long flags,
                      const void *data)
```

### 2.  

```c 876:876:kernel/src/syscall.c
return vfs_mount_at(self->cwd, target_path, fs_name);
```

### 3.  

```c 471:472:kernel/src/vfs.c
int vfs_mount_at(struct vnode *start, const char *target,
                 const char *filesystem)
```

