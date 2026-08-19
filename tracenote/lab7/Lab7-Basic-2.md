# Trace Result - 2026-08-12

## 1. 路徑走訪與跨越掛載點（核心機制） 

<!-- tracenote -->
路徑走訪與跨越掛載點（核心機制）
<!-- /tracenote -->

### 1. 對 /mnt/inner.txt 執行 open()，而 /mnt 上已掛載另一個 tmpfs。 

<!-- tracenote -->
對 /mnt/inner.txt 執行 open()，而 /mnt 上已掛載另一個 tmpfs。
<!-- /tracenote -->

```c 554:554:kernel/src/shell.c
if (vfs_open("/mnt/inner.txt", O_CREAT, &f) == 0 && f) {
```

### 2.  

```c 335:335:kernel/src/vfs.c
int vfs_open(const char *pathname, int flags, struct file **target)
```

### 3. 取得「將擁有 leaf 的父目錄」，而非 leaf 本身 

<!-- tracenote -->
取得「將擁有 leaf 的父目錄」，而非 leaf 本身
<!-- /tracenote -->

```c 282:282:kernel/src/vfs.c
if (vfs_walk(pathname, 1, &dir, leaf, sizeof(leaf)) != 0)
```

#### 1. 依 stop_at_parent 產生兩種結果 

<!-- tracenote -->
依 stop_at_parent 產生兩種結果
- 關閉時 *result 為整條路徑解析到的 vnode;
- 開啟時 *result 為最後一段的父目錄,leaf 收到 NUL 結尾的最後一段名稱。
- 任何一段查不到、路徑格式不合法、或 leaf 塞不下,一律回傳 -1。
<!-- /tracenote -->

```c 138:140:kernel/src/vfs.c
static int vfs_walk(const char *pathname, int stop_at_parent,
                    struct vnode **result, char *leaf, size_t leaf_sz)
```

#### 2. %%Tip%% Phase 1:參數與長度驗證 

<!-- tracenote -->
Phase 1:參數與長度驗證
- 三道 early return 分別擋掉未初始化的 rootfs、非絕對路徑、以及 stop_at_parent 模式下缺少輸出緩衝區的呼叫。長度掃描邊走邊比對,避免對超長字串做完整 strlen。
<!-- /tracenote -->

#### 3. 需要目標節點的父節點時，如果沒有提供 output buf 或是 buf size 的話直接回傳。 

<!-- tracenote -->
需要目標節點的父節點時，如果沒有提供 output buf 或是 buf size 的話直接回傳。
<!-- /tracenote -->

```c 146:147:kernel/src/vfs.c
if (stop_at_parent && (!leaf || leaf_sz == 0))
    return -1;
```

#### 4. 檢查路徑長度是否合法 

<!-- tracenote -->
檢查路徑長度是否合法
<!-- /tracenote -->

```c 149:152:kernel/src/vfs.c
size_t path_len = 0;
while (pathname[path_len] != '\0') {
    if (++path_len > VFS_MAX_PATHNAME)
        return -1;
```

#### 5. %%Tip%% Phase 2:起點決定與 "/" 特例 

<!-- tracenote -->
Phase 2:起點決定與 "/" 特例
<!-- /tracenote -->

#### 6. 找到真正該使用的 vnode (Orphaned)

<!-- tracenote -->
找到真正該使用的 vnode
<!-- /tracenote -->

```c 116:116:kernel/src/vfs.c
struct vnode *dir = vfs_follow_mount(g_rootfs->root);
```

##### 1.  

```c 73:73:kernel/src/vfs.c
static struct vnode *vfs_follow_mount(struct vnode *node)
```

#### 7. - "/" 在 stop_at_parent 模式下回 -1:根沒有父目錄,也不是可建立的 entry (Orphaned)

<!-- tracenote -->
- "/" 在 stop_at_parent 模式下回 -1:根沒有父目錄,也不是可建立的 entry
- "/" 在完整解析模式下是合法結果,直接回傳根 vnode
<!-- /tracenote -->

```c 119:125:kernel/src/vfs.c
/* "/" names the root itself: a valid thing to resolve, but not a
 * valid entry inside a directory to create or open. */
if (*cur == '\0') {
    if (stop_at_parent)
        return -1;
    *result = dir;
    return 0;
```

#### 8. %%Tip%% Phase 3:逐段掃描與 stop_at_parent 提前收手 

<!-- tracenote -->
Phase 3:逐段掃描與 stop_at_parent 提前收手
<!-- /tracenote -->

#### 9. 量測本段長度至下一個 '/' 或 '\0' 

<!-- tracenote -->
量測本段長度至下一個 '/' 或 '\0'
<!-- /tracenote -->

```c 182:184:kernel/src/vfs.c
size_t len = 0;
while (cur[len] != '/' && cur[len] != '\0')
    len++;
```

#### 10. 以下情況直接回傳： 

<!-- tracenote -->
以下情況直接回傳：
- len == 0 代表 "//" 或結尾 '/',不指涉任何可操作的名稱。
- 路徑長度超過上限。
<!-- /tracenote -->

```c 219:220:kernel/src/tmpfs.c
if (len == 0 || len > VFS_MAX_PATHNAME)
    return -1;
```

#### 11. 如果檢查的這段路徑是最後一段，且要求停在父目錄。 

<!-- tracenote -->
如果檢查的這段路徑是最後一段，且要求停在父目錄。
<!-- /tracenote -->

```c 212:212:kernel/src/vfs.c
if (is_last && stop_at_parent) {
```

#### 12. 目標 vnode name 放的進 output buf 的話，就把該段名稱複製進 output buf 後，回傳 parent vnode。 

<!-- tracenote -->
目標 vnode name 放的進 output buf 的話，就把該段名稱複製進 output buf 後，回傳 parent vnode。
<!-- /tracenote -->

```c 220:223:kernel/src/vfs.c
if (len >= leaf_sz)
    return -1;
for (size_t i = 0; i < len; i++)
    leaf[i] = cur[i];
```

#### 13. %%Tip%% Phase 4:查找、穿越掛載點與前進 

<!-- tracenote -->
Phase 4:查找、穿越掛載點與前進
<!-- /tracenote -->

#### 14. 把本段複製成 NUL 結尾的 comp 後交給 dir->v_ops->lookup()。 

<!-- tracenote -->
把本段複製成 NUL 結尾的 comp 後交給 dir->v_ops->lookup()。
<!-- /tracenote -->

```c 236:243:kernel/src/vfs.c
char comp[VFS_MAX_PATHNAME + 1];
for (size_t i = 0; i < len; i++)
    comp[i] = cur[i];
comp[len] = '\0';

struct vnode *next = NULL;
if (!dir->v_ops || !dir->v_ops->lookup)
    return -1;
if (dir->v_ops->lookup(dir, &next, comp) != 0 || !next)
    return -1;
```

##### 1.  

```c 158:158:kernel/src/tmpfs.c
static int tmpfs_lookup(struct vnode *dir_node, struct vnode **target,
```

#### 15. 把 parent vnode 指向真正掛載的 vnode。 

<!-- tracenote -->
把 parent vnode 指向真正掛載的 vnode。
<!-- /tracenote -->

```c 245:245:kernel/src/vfs.c
dir = vfs_follow_mount(next);
```

#### 16. cur += len + 1 跳過分隔符;若跳完就是字串結尾,代表路徑以 '/' 收尾,無段可操作,回 -1 

<!-- tracenote -->
cur += len + 1 跳過分隔符;若跳完就是字串結尾,代表路徑以 '/' 收尾,無段可操作,回 -1
<!-- /tracenote -->

```c 190:191:kernel/src/vfs.c
cur += len + 1;

/* Path ended with '/', leaving no component to act on. */
if (*cur == '\0')
    return -1;
```

### 4. 找到要找的節點時，取出真正被掛載的 vnode。 

<!-- tracenote -->
找到要找的節點時，取出真正被掛載的 vnode。
<!-- /tracenote -->

```c 289:292:kernel/src/vfs.c
if (dir->v_ops->lookup(dir, &node, leaf) == 0 && node) {
    /* The leaf may itself be a mount point, in which case the name
     * denotes the mounted file system's root. */
    node = vfs_follow_mount(node);
```

#### 1.  

```c 158:159:kernel/src/tmpfs.c
static int tmpfs_lookup(struct vnode *dir_node, struct vnode **target,
                        const char *component_name)
```

### 5. 沒找到節點時： 

<!-- tracenote -->
沒找到節點時：
<!-- /tracenote -->

```c 161:161:kernel/src/vfs.c
} else {
```

### 6. 新建一個節點 

<!-- tracenote -->
新建一個節點
<!-- /tracenote -->

```c 300:301:kernel/src/vfs.c
if (dir->v_ops->create(dir, &node, leaf) != 0 || !node)
    return -1;
```

#### 1.  

```c 248:250:kernel/src/tmpfs.c
static int tmpfs_create(struct vnode *dir_node, struct vnode **target,
                        const char *component_name)
{
```

#### 2.  

```c 207:209:kernel/src/tmpfs.c
static int tmpfs_add_entry(struct vnode *dir_node, struct vnode **target,
                           const char *component_name,
                           enum tmpfs_type type)
```

#### 3. 建一個節點，包含 vfs node 跟 fs node。 

<!-- tracenote -->
建一個節點，包含 vfs node 跟 fs node。
<!-- /tracenote -->

```c 229:230:kernel/src/tmpfs.c
struct tmpfs_node *node = tmpfs_new_node(component_name, type,
                                         dir_node->mount);
```

##### 1.  

```c 88:90:kernel/src/tmpfs.c
static struct tmpfs_node *tmpfs_new_node(const char *name,
                                         enum tmpfs_type type,
                                         struct mount *mount)
```

### 7.  

```c 313:313:kernel/src/vfs.c
if (node->f_ops->open(node, &file) != 0 || !file)
```

## 2. 掛載檔案系統 

<!-- tracenote -->
掛載檔案系統
<!-- /tracenote -->

### 1.  

```c 536:536:kernel/src/shell.c
vfs_report("mount tmpfs at /mnt", vfs_mount("/mnt", "tmpfs") == 0);
```

### 2.  

```c 537:537:kernel/src/vfs.c
int vfs_mount(const char *target, const char *filesystem)
```

### 3. %%Tip%% Phase 1：參數與檔案系統查表 

<!-- tracenote -->
Phase 1：參數與檔案系統查表
<!-- /tracenote -->

### 4. - str_eq() 線性掃描 g_fs_list，找不到即拒絕 

<!-- tracenote -->
- str_eq() 線性掃描 g_fs_list，找不到即拒絕
- 額外檢查 fs->setup_mount，雖然 register_filesystem() 已擋過，但此處不依賴其他函式的不變式
<!-- /tracenote -->

```c 477:479:kernel/src/vfs.c
struct filesystem *fs = find_filesystem(filesystem);
if (!fs || !fs->setup_mount)
    return -1;
```

#### 1. 查找 fs list 中是否有要找的 fs。 

<!-- tracenote -->
查找 fs list 中是否有要找的 fs。
<!-- /tracenote -->

```c 17:17:kernel/src/vfs.c
static struct filesystem *find_filesystem(const char *name)
```

### 5. %%Tip%% Phase 2：以 stop_at_parent 模式取得未被穿越的掛載點 

<!-- tracenote -->
Phase 2：以 stop_at_parent 模式取得未被穿越的掛載點
<!-- /tracenote -->

### 6. 停在最後一段的父目錄，並把 leaf 名稱複製出來（pathname 內的 component 沒有 NUL 結尾） 

<!-- tracenote -->
停在最後一段的父目錄，並把 leaf 名稱複製出來（pathname 內的 component 沒有 NUL 結尾）
<!-- /tracenote -->

```c 492:492:kernel/src/vfs.c
if (vfs_walk(target, 1, &dir, leaf, sizeof(leaf)) != 0)
```

### 7. 直接呼叫 dir->v_ops->lookup() 而非 vfs_walk 的完整解析，因此跳過 vfs_follow_mount()，拿到的是掛載點 vnode 本身而非被蓋住的 fs root 

<!-- tracenote -->
直接呼叫 dir->v_ops->lookup() 而非 vfs_walk 的完整解析，因此跳過 vfs_follow_mount()，拿到的是掛載點 vnode 本身而非被蓋住的 fs root
<!-- /tracenote -->

```c 499:499:kernel/src/vfs.c
if (dir->v_ops->lookup(dir, &mountpoint, leaf) != 0 || !mountpoint)
```

#### 1.  

```c 158:159:kernel/src/tmpfs.c
static int tmpfs_lookup(struct vnode *dir_node, struct vnode **target,
                        const char *component_name)
```

### 8. %%Tip%% Phase 3：型別與重複掛載檢查 

<!-- tracenote -->
Phase 3：型別與重複掛載檢查
<!-- /tracenote -->

### 9. 拒絕覆蓋 regular file：結果會是一個沒有任何操作能作用其上的東西（因為被覆蓋成目錄型態的節點，後續進行 read / write 操作時都會直接 return -1）。 

<!-- tracenote -->
拒絕覆蓋 regular file：結果會是一個沒有任何操作能作用其上的東西（因為被覆蓋成目錄型態的節點，後續進行 read / write 操作時都會直接 return -1）。
<!-- /tracenote -->

```c 505:505:kernel/src/vfs.c
if (mountpoint->type != VNODE_TYPE_DIR)
```

### 10. 避免二次覆蓋，導致前次覆蓋的 (Orphaned)

<!-- tracenote -->
避免二次覆蓋，導致前次覆蓋的
<!-- /tracenote -->

```c 350:353:kernel/src/vfs.c
/* Already covered: a second mount would hide the first with no way
 * left to reach it. */
if (mountpoint->mounted)
    return -1;
```

### 11. %%Tip%% Phase 4：建構後才發布（fail-safe publish） 

<!-- tracenote -->
Phase 4：建構後才發布（fail-safe publish）
<!-- /tracenote -->

### 12. mnt->mountpoint 記錄被覆蓋的 vnode，供未來 ".." 爬出掛載點使用 

<!-- tracenote -->
mnt->mountpoint 記錄被覆蓋的 vnode，供未來 ".." 爬出掛載點使用
<!-- /tracenote -->

```c 525:525:kernel/src/vfs.c
mountpoint->mounted = mnt;
```

## 3. vfs_mkdir() 與 vfs_lookup() 

<!-- tracenote -->
vfs_mkdir() 與 vfs_lookup()
<!-- /tracenote -->

### 1. %%Tip%% mkdir 

<!-- tracenote -->
mkdir
<!-- /tracenote -->

### 2.  

```c 477:477:kernel/src/shell.c
vfs_report("mkdir /dir1/dir2", vfs_mkdir("/dir1/dir2") == 0);
```

### 3.  

```c 453:453:kernel/src/vfs.c
int vfs_mkdir(const char *pathname)
```

### 4.  

```c 282:282:kernel/src/vfs.c
if (vfs_walk(pathname, 1, &dir, leaf, sizeof(leaf)) != 0)
```

#### 1.  

```c 138:140:kernel/src/vfs.c
static int vfs_walk(const char *pathname, int stop_at_parent,
                    struct vnode **result, char *leaf, size_t leaf_sz)
```

### 5.  

```c 440:441:kernel/src/vfs.c
if (dir->v_ops->mkdir(dir, &node, leaf) != 0 || !node)
    return -1;
```

#### 1.  

```c 263:264:kernel/src/tmpfs.c
static int tmpfs_mkdir(struct vnode *dir_node, struct vnode **target,
                       const char *component_name)
```

#### 2.  

```c 207:209:kernel/src/tmpfs.c
static int tmpfs_add_entry(struct vnode *dir_node, struct vnode **target,
                           const char *component_name,
                           enum tmpfs_type type)
```

#### 3. 拿出 parent node 的 data (Orphaned)

<!-- tracenote -->
拿出 parent node 的 data
<!-- /tracenote -->

```c 194:197:kernel/src/tmpfs.c
struct tmpfs_node *dir = (struct tmpfs_node *)dir_node->internal;
```

#### 4. 確認 parent node 紀錄的 entries 有沒有包含要找的檔案（component_name）。 

<!-- tracenote -->
確認 parent node 紀錄的 entries 有沒有包含要找的檔案（component_name）。
<!-- /tracenote -->

```c 226:226:kernel/src/tmpfs.c
if (tmpfs_lookup(dir_node, &existing, component_name) == 0)
```

##### 1.  

```c 158:159:kernel/src/tmpfs.c
static int tmpfs_lookup(struct vnode *dir_node, struct vnode **target,
                        const char *component_name)
```

##### 2. 找 parent node 底下的 entries 中，有沒有名稱跟要找的檔名 (component_name) 一樣的。 

<!-- tracenote -->
找 parent node 底下的 entries 中，有沒有名稱跟要找的檔名 (component_name) 一樣的。
<!-- /tracenote -->

```c 180:182:kernel/src/tmpfs.c
for (int i = 0; i < dir->entry_count; i++) {
    if (str_eq(dir->entries[i]->name, component_name)) {
        *target = dir->entries[i]->vnode;
```

#### 5. 建一個節點，包含 vfs node 跟 fs node。 

<!-- tracenote -->
建一個節點，包含 vfs node 跟 fs node。
<!-- /tracenote -->

```c 229:230:kernel/src/tmpfs.c
struct tmpfs_node *node = tmpfs_new_node(component_name, type,
                                         dir_node->mount);
```

### 6. %%Tip%% lookup 

<!-- tracenote -->
lookup
<!-- /tracenote -->

### 7.  

```c 510:510:kernel/src/shell.c
vfs_report("lookup /dir1/dir2", vfs_lookup("/dir1/dir2", &node) == 0);
```

### 8.  

```c 571:571:kernel/src/vfs.c
int vfs_lookup(const char *pathname, struct vnode **target)
```

### 9.  (Orphaned)

```c 383:383:kernel/src/vfs.c
return vfs_walk(pathname, 0, target, NULL, 0);
```

#### 1.  

```c 138:140:kernel/src/vfs.c
static int vfs_walk(const char *pathname, int stop_at_parent,
                    struct vnode **result, char *leaf, size_t leaf_sz)
```

### 10.  

```c 138:140:kernel/src/vfs.c
static int vfs_walk(const char *pathname, int stop_at_parent,
                    struct vnode **result, char *leaf, size_t leaf_sz)
```

