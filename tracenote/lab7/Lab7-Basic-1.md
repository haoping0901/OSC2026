# Trace Result - 2026-08-05

## 1. 開機掛載 rootfs 

<!-- tracenote -->
開機掛載 rootfs
<!-- /tracenote -->

### 1. 在核心開機序列中把 tmpfs 註冊進 VFS 並建立掛在 / 的根掛載點，讓後續所有路徑操作有起點可走。 

<!-- tracenote -->
在核心開機序列中把 tmpfs 註冊進 VFS 並建立掛在 / 的根掛載點，讓後續所有路徑操作有起點可走。
<!-- /tracenote -->

```c 210:210:kernel/src/main.c
vfs_init();
```

### 2. 把靜態的 g_tmpfs descriptor 放入 g_fs_list[]、配置一個 struct mount、請 tmpfs 生出根目錄 vnode，最後把該 mount 發佈到全域 g_rootfs。任何一步失敗都讓 g_rootfs 維持 NULL。 

<!-- tracenote -->
把靜態的 g_tmpfs descriptor 放入 g_fs_list[]、配置一個 struct mount、請 tmpfs 生出根目錄 vnode，最後把該 mount 發佈到全域 g_rootfs。任何一步失敗都讓 g_rootfs 維持 NULL。
<!-- /tracenote -->

```c 265:265:kernel/src/vfs.c
void vfs_init(void)
```

### 3. 回傳 static 的 g_tmpfs，生命週期等同核心，可直接註冊。 

<!-- tracenote -->
回傳 static 的 g_tmpfs，生命週期等同核心，可直接註冊。
<!-- /tracenote -->

```c 267:267:kernel/src/vfs.c
struct filesystem *tmpfs = tmpfs_get_fs();
```

### 4. 把 g_tmpfs 加到 g_fs_list 

<!-- tracenote -->
把 g_tmpfs 加到 g_fs_list
<!-- /tracenote -->

```c 269:269:kernel/src/vfs.c
if (register_filesystem(tmpfs) != 0) {
```

#### 1.  

```c 23:23:kernel/src/vfs.c
int register_filesystem(struct filesystem *fs)
```

#### 2. 檢查 fs->name 與 fs->setup_mount 非 NULL 

<!-- tracenote -->
檢查 fs->name 與 fs->setup_mount 非 NULL
<!-- /tracenote -->

```c 25:25:kernel/src/vfs.c
if (!fs || !fs->name || !fs->setup_mount)
```

#### 3. 以 str_eq() 線性掃描去重複 

<!-- tracenote -->
以 str_eq() 線性掃描去重複
<!-- /tracenote -->

```c 28:29:kernel/src/vfs.c
for (int i = 0; i < g_fs_count; i++) {
    if (str_eq(g_fs_list[i]->name, fs->name))
```

#### 4. 表滿（g_fs_count >= VFS_MAX_FS，即 8）時回傳 -1 

<!-- tracenote -->
表滿（g_fs_count >= VFS_MAX_FS，即 8）時回傳 -1
<!-- /tracenote -->

```c 33:34:kernel/src/vfs.c
if (g_fs_count >= VFS_MAX_FS)
    return -1;
```

#### 5. 登記到 VFS 的檔案系統表 

<!-- tracenote -->
登記到 VFS 的檔案系統表
<!-- /tracenote -->

```c 36:36:kernel/src/vfs.c
g_fs_list[g_fs_count++] = fs;
```

### 5. 配置代表「掛載實例」的 struct mount 

<!-- tracenote -->
配置代表「掛載實例」的 struct mount
<!-- /tracenote -->

```c 274:274:kernel/src/vfs.c
struct mount *mnt = (struct mount *)kmalloc(sizeof(struct mount));
```

### 6.  

```c 282:282:kernel/src/vfs.c
if (tmpfs->setup_mount(tmpfs, mnt) != 0) {
```

#### 1.  

```c 351:351:kernel/src/tmpfs.c
static int tmpfs_setup_mount(struct filesystem *fs, struct mount *mount)
```

#### 2. 以名稱 "/"、型別 TMPFS_TYPE_DIR 建立根節點 

<!-- tracenote -->
以名稱 "/"、型別 TMPFS_TYPE_DIR 建立根節點
<!-- /tracenote -->

```c 356:356:kernel/src/tmpfs.c
struct tmpfs_node *root = tmpfs_new_node("/", TMPFS_TYPE_DIR, mount);
```

##### 1. 配置一組「tmpfs 私有節點 + 通用 struct vnode」並完成雙向綁定。為 tmpfs 中所有檔案與目錄的唯一建構入口。 

<!-- tracenote -->
配置一組「tmpfs 私有節點 + 通用 struct vnode」並完成雙向綁定。為 tmpfs 中所有檔案與目錄的唯一建構入口。
<!-- /tracenote -->

```c 85:87:kernel/src/tmpfs.c
static struct tmpfs_node *tmpfs_new_node(const char *name,
                                         enum tmpfs_type type,
                                         struct mount *mount)
```

##### 2. 複製 node name，最多複製 TMPFS_MAX_NAME 長度，並在最後補 '\0'。 

<!-- tracenote -->
複製 node name，最多複製 TMPFS_MAX_NAME 長度，並在最後補 '\0'。
<!-- /tracenote -->

```c 101:105:kernel/src/tmpfs.c
while (name[i] != '\0' && i < TMPFS_MAX_NAME) {
    node->name[i] = name[i];
    i++;
}
node->name[i] = '\0';
```

##### 3. 初始化專用於描述 tmpfs 資訊的 tmpfs_node 

<!-- tracenote -->
初始化專用於描述 tmpfs 資訊的 tmpfs_node
<!-- /tracenote -->

```c 107:112:kernel/src/tmpfs.c
node->type = type;
node->entry_count = 0;
for (int e = 0; e < TMPFS_MAX_ENTRIES; e++)
    node->entries[e] = NULL;
node->data = NULL;
node->size = 0;
```

##### 4. 初始化 VFS 的通用 vnode。 

<!-- tracenote -->
初始化 VFS 的通用 vnode。
<!-- /tracenote -->

```c 114:117:kernel/src/tmpfs.c
vnode->mount = mount;
vnode->v_ops = &g_tmpfs_v_ops;
vnode->f_ops = &g_tmpfs_f_ops;
vnode->internal = node;
```

## 2. 建立並寫入檔案（vfs_open 的 O_CREAT 路徑） 

<!-- tracenote -->
建立並寫入檔案（vfs_open 的 O_CREAT 路徑）
<!-- /tracenote -->

### 1.  

```c 614:615:kernel/src/shell.c
} else if (str_eq(cmd, "vfstest") != 0) {
    test_vfs();
```

### 2.  

```c 360:360:kernel/src/shell.c
static void test_vfs(void)
```

### 3.  

```c 371:371:kernel/src/shell.c
vfs_open("/test.txt", O_CREAT, &f) == 0 && f != NULL);
```

#### 1. 是 VFS 的檔案開啟進入點：把絕對路徑解析成「父目錄 + 葉節點名」，透過父目錄的 lookup（失敗時在 O_CREAT 下改用 create）取得 vnode，再委派檔案系統配置 struct file 並由通用層完成 f_pos/flags 初始化。 

<!-- tracenote -->
是 VFS 的檔案開啟進入點：把絕對路徑解析成「父目錄 + 葉節點名」，透過父目錄的 lookup（失敗時在 O_CREAT 下改用 create）取得 vnode，再委派檔案系統配置 struct file 並由通用層完成 f_pos/flags 初始化。
<!-- /tracenote -->

```c 133:133:kernel/src/vfs.c
int vfs_open(const char *pathname, int flags, struct file **target)
```

#### 2. %%Tip%% Phase 1：路徑解析 → 取得父目錄與葉名 

<!-- tracenote -->
Phase 1：路徑解析 → 取得父目錄與葉名
- 先把 pathname 拆成「擁有最終名稱的目錄」與「葉名字串」，而非直接解析到目標 vnode。
<!-- /tracenote -->

#### 3. 把一條絕對路徑切成「最後一層名稱所屬的父目錄 vnode」與「該最後一層名稱字串」兩部分，供 vfs_open() 用同一次走訪同時服務查找與建檔。 

<!-- tracenote -->
把一條絕對路徑切成「最後一層名稱所屬的父目錄 vnode」與「該最後一層名稱字串」兩部分，供 vfs_open() 用同一次走訪同時服務查找與建檔。
<!-- /tracenote -->

```c 140:140:kernel/src/vfs.c
if (vfs_resolve(pathname, &dir, leaf, sizeof(leaf)) != 0)
```

##### 1.  輸出 *parent（持有最後一層名稱的目錄 vnode）與 NUL-terminated 的 leaf 字串；任何格式錯誤、中間目錄不存在、或 component 塞不進 leaf 都回 -1，且不保證 out 參數被寫入。 

<!-- tracenote -->
 輸出 *parent（持有最後一層名稱的目錄 vnode）與 NUL-terminated 的 leaf 字串；任何格式錯誤、中間目錄不存在、或 component 塞不進 leaf 都回 -1，且不保證 out 參數被寫入。
<!-- /tracenote -->

```c 59:60:kernel/src/vfs.c
static int vfs_resolve(const char *pathname, struct vnode **parent,
                       char *leaf, size_t leaf_sz)
```

##### 2. %%Tip%% Phase 1：前置驗證與長度上限 

<!-- tracenote -->
Phase 1：前置驗證與長度上限
- 先確認 root mount 可用、路徑為絕對路徑，再以邊掃邊比的方式限制總長度。
<!-- /tracenote -->

##### 3. 缺少 root mount 或路徑非 / 開頭即拒絕，不支援相對路徑 

<!-- tracenote -->
缺少 root mount 或路徑非 / 開頭即拒絕，不支援相對路徑
<!-- /tracenote -->

```c 62:65:kernel/src/vfs.c
if (!g_rootfs || !g_rootfs->root)
    return -1;
if (!pathname || pathname[0] != '/')
    return -1;
```

##### 4. path_len 累加時就檢查上限，避免對未終止字串無限掃描 

<!-- tracenote -->
path_len 累加時就檢查上限，避免對未終止字串無限掃描
<!-- /tracenote -->

```c 67:71:kernel/src/vfs.c
size_t path_len = 0;
while (pathname[path_len] != '\0') {
    if (++path_len > VFS_MAX_PATHNAME)
        return -1;
}
```

##### 5. cur 跳過開頭 / 後若立刻是 '\0'，代表輸入是 "/"；root 本身不是「某目錄內的一個 entry」，不能當 open/create 目標 

<!-- tracenote -->
cur 跳過開頭 / 後若立刻是 '\0'，代表輸入是 "/"；root 本身不是「某目錄內的一個 entry」，不能當 open/create 目標
<!-- /tracenote -->

```c 74:80:kernel/src/vfs.c
const char *cur = pathname + 1;      /* skip the leading '/' */

/* A trailing '/' would make the leaf empty, and "/" alone names the
 * root itself rather than an entry inside a directory. Neither is a
 * valid target for open/create. */
if (*cur == '\0')
    return -1;
```

##### 6. %%Tip%% Phase 2：component 切分與 leaf 判定 

<!-- tracenote -->
Phase 2：component 切分與 leaf 判定
- 主迴圈每輪量測到下一個 / 或 '\0' 的長度，並依有無後續分隔符決定是收尾還是繼續下降。
<!-- /tracenote -->

##### 7. - len == 0 擋掉連續 //（空 component） 

<!-- tracenote -->
- len == 0 擋掉連續 //（空 component）
- len >= leaf_sz 在複製前就檢查，預留 NUL 空間，複製本身無溢位風險
<!-- /tracenote -->

```c 85:89:kernel/src/vfs.c
while (cur[len] != '/' && cur[len] != '\0')
    len++;

if (len == 0 || len >= leaf_sz)
    return -1;
```

##### 8. 沒有後續分隔符 → 當前 component 就是 leaf：逐字複製並補 '\0'，*parent 設為當前 dir（唯一的成功回傳點） 

<!-- tracenote -->
沒有後續分隔符 → 當前 component 就是 leaf：逐字複製並補 '\0'，*parent 設為當前 dir（唯一的成功回傳點）
<!-- /tracenote -->

```c 91:97:kernel/src/vfs.c
/* No separator left: this component is the leaf. */
if (cur[len] == '\0') {
    for (size_t i = 0; i < len; i++)
        leaf[i] = cur[i];
    leaf[len] = '\0';
    *parent = dir;
    return 0;
```

##### 9. %%Tip%% Phase 3：中間目錄下降 

<!-- tracenote -->
Phase 3：中間目錄下降
<!-- /tracenote -->

##### 10. 把 component 複製到 stack 上的 comp，因為 pathname 內的 component 不是獨立 NUL-terminated 字串，而 lookup 需要可比較的完整字串。 

<!-- tracenote -->
把 component 複製到 stack 上的 comp，因為 pathname 內的 component 不是獨立 NUL-terminated 字串，而 lookup 需要可比較的完整字串。
<!-- /tracenote -->

```c 100:105:kernel/src/vfs.c
/* Intermediate component: it must already exist and be usable
 * as a directory for the next round. */
char comp[VFS_MAX_PATHNAME + 1];
for (size_t i = 0; i < len; i++)
    comp[i] = cur[i];
comp[len] = '\0';
```

##### 11. - 每輪都重新檢查 dir->v_ops->lookup：下一層 vnode 由 fs 提供，不能假設所有 vnode 都具備目錄操作 

<!-- tracenote -->
- 每輪都重新檢查 dir->v_ops->lookup：下一層 vnode 由 fs 提供，不能假設所有 vnode 都具備目錄操作
- 中間層不做隱式建立（O_CREAT 只作用於 leaf），lookup 失敗即整體失敗
<!-- /tracenote -->

```c 108:111:kernel/src/vfs.c
if (!dir->v_ops || !dir->v_ops->lookup)
    return -1;
if (dir->v_ops->lookup(dir, &next, comp) != 0 || !next)
    return -1;
```

###### 1.  

```c 252:252:kernel/src/vfs.c
int vfs_lookup(const char *pathname, struct vnode **target)
```

###### 2.  

```c 133:134:kernel/src/tmpfs.c
static int tmpfs_lookup(struct vnode *dir_node, struct vnode **target,
                        const char *component_name)
```

###### 3. lookup 只能對 directory node 進行，不應該對 file node 做。 

<!-- tracenote -->
lookup 只能對 directory node 進行，不應該對 file node 做。
<!-- /tracenote -->

```c 140:141:kernel/src/tmpfs.c
if (!dir || dir->type != TMPFS_TYPE_DIR)
    return -1;
```

###### 4. 遍歷所有 dir node 的 entry 

<!-- tracenote -->
遍歷所有 dir node 的 entry
<!-- /tracenote -->

```c 143:143:kernel/src/tmpfs.c
for (int i = 0; i < dir->entry_count; i++) {
```

###### 5. 找到要找的 node（名稱跟 component_name 相同）後回傳該 node。 

<!-- tracenote -->
找到要找的 node（名稱跟 component_name 相同）後回傳該 node。
<!-- /tracenote -->

```c 144:145:kernel/src/tmpfs.c
if (str_eq(dir->entries[i]->name, component_name)) {
    *target = dir->entries[i]->vnode;
```

##### 12. cur += len + 1 越過分隔符後若已到字串尾，表示路徑以 / 結尾（如 /a/），沒有 leaf 可操作 → -1 

<!-- tracenote -->
cur += len + 1 越過分隔符後若已到字串尾，表示路徑以 / 結尾（如 /a/），沒有 leaf 可操作 → -1
<!-- /tracenote -->

```c 114:118:kernel/src/vfs.c
cur += len + 1;

/* Path ended with '/', leaving no leaf to act on. */
if (*cur == '\0')
    return -1;
```

#### 4. 回傳後仍重複檢查 dir->v_ops->lookup，因為根目錄這條路徑不會經過 vfs_resolve() 內的迴圈檢查 

<!-- tracenote -->
回傳後仍重複檢查 dir->v_ops->lookup，因為根目錄這條路徑不會經過 vfs_resolve() 內的迴圈檢查
<!-- /tracenote -->

```c 142:143:kernel/src/vfs.c
if (!dir->v_ops || !dir->v_ops->lookup)
    return -1;
```

#### 5. %%Tip%% Phase 2：lookup 失敗即為 O_CREAT 的判斷依據 

<!-- tracenote -->
Phase 2：lookup 失敗即為 O_CREAT 的判斷依據
<!-- /tracenote -->

#### 6. 查不到檔案時： 

<!-- tracenote -->
查不到檔案時：
<!-- /tracenote -->

```c 146:146:kernel/src/vfs.c
if (dir->v_ops->lookup(dir, &node, leaf) != 0) {
```

##### 1.  

```c 133:134:kernel/src/tmpfs.c
static int tmpfs_lookup(struct vnode *dir_node, struct vnode **target,
                        const char *component_name)
```

#### 7. - 無 O_CREAT 時，miss 直接轉為 -1 

<!-- tracenote -->
- 無 O_CREAT 時，miss 直接轉為 -1
- 沒有定義 create 操作時直接回傳 -1
<!-- /tracenote -->

```c 149:152:kernel/src/vfs.c
if (!(flags & O_CREAT))
    return -1;
if (!dir->v_ops->create)
    return -1;
```

#### 8. lookup 未命中且帶 O_CREAT → 呼叫 create 在同一個父目錄下新增節點 

<!-- tracenote -->
lookup 未命中且帶 O_CREAT → 呼叫 create 在同一個父目錄下新增節點
<!-- /tracenote -->

```c 153:154:kernel/src/vfs.c
if (dir->v_ops->create(dir, &node, leaf) != 0 || !node)
    return -1;
```

##### 1.  

```c 163:164:kernel/src/tmpfs.c
static int tmpfs_create(struct vnode *dir_node, struct vnode **target,
                        const char *component_name)
```

##### 2. 取出 dir vnode。 

<!-- tracenote -->
取出 dir vnode。
<!-- /tracenote -->

```c 169:171:kernel/src/tmpfs.c
struct tmpfs_node *dir = (struct tmpfs_node *)dir_node->internal;
if (!dir || dir->type != TMPFS_TYPE_DIR)
    return -1;
```

##### 3. 取得並檢查 node name 長度，超過上限就回傳 -1。 

<!-- tracenote -->
取得並檢查 node name 長度，超過上限就回傳 -1。
<!-- /tracenote -->

```c 173:175:kernel/src/tmpfs.c
size_t len = tmpfs_name_len(component_name);
if (len == 0 || len > TMPFS_MAX_NAME)
    return -1;
```

###### 1.  

```c 63:63:kernel/src/tmpfs.c
static size_t tmpfs_name_len(const char *name)
```

###### 2. 長度超過 tmpfs 最大長度時直接回傳最大長度 + 1。 

<!-- tracenote -->
長度超過 tmpfs 最大長度時直接回傳最大長度 + 1。
<!-- /tracenote -->

```c 67:69:kernel/src/tmpfs.c
while (name[len] != '\0') {
    if (++len > TMPFS_MAX_NAME)
        return TMPFS_MAX_NAME + 1;
```

##### 4. 已存在相同 name 的 vnode 就直接 return。 

<!-- tracenote -->
已存在相同 name 的 vnode 就直接 return。
<!-- /tracenote -->

```c 180:182:kernel/src/tmpfs.c
struct vnode *existing = NULL;
if (tmpfs_lookup(dir_node, &existing, component_name) == 0)
    return -1;
```

###### 1.  

```c 133:134:kernel/src/tmpfs.c
static int tmpfs_lookup(struct vnode *dir_node, struct vnode **target,
                        const char *component_name)
```

##### 5. 建新 node。 

<!-- tracenote -->
建新 node。
<!-- /tracenote -->

```c 184:186:kernel/src/tmpfs.c
struct tmpfs_node *node = tmpfs_new_node(component_name,
                                         TMPFS_TYPE_FILE,
                                         dir_node->mount);
```

###### 1.  

```c 85:87:kernel/src/tmpfs.c
static struct tmpfs_node *tmpfs_new_node(const char *name,
                                         enum tmpfs_type type,
                                         struct mount *mount)
```

##### 6. 把新 node 紀錄到所屬 dir vnode，以及 caller 提供的參數中。 

<!-- tracenote -->
把新 node 紀錄到所屬 dir vnode，以及 caller 提供的參數中。
<!-- /tracenote -->

```c 190:191:kernel/src/tmpfs.c
dir->entries[dir->entry_count++] = node;
*target = node->vnode;
```

#### 9. %%Tip%% Phase 3：委派 fs 配置 handle 並補齊通用欄位 

<!-- tracenote -->
Phase 3：委派 fs 配置 handle 並補齊通用欄位
- vnode 確定後轉入 f_ops->open，由檔案系統配置 handle，通用層再覆寫共通語意欄位。
<!-- /tracenote -->

#### 10. 為已存在的 vnode 配置一個 struct file 控制代碼，讓後續 read/write 有承載檔案位置的物件。 

<!-- tracenote -->
為已存在的 vnode 配置一個 struct file 控制代碼，讓後續 read/write 有承載檔案位置的物件。
<!-- /tracenote -->

```c 160:162:kernel/src/vfs.c
struct file *file = NULL;
if (node->f_ops->open(node, &file) != 0 || !file)
    return -1;
```

##### 1. 從 heap 配置一個 struct file，將其 vnode 與 f_ops 指向被開啟的節點，並回傳給呼叫端；失敗時回傳 -1 且不改動 *target。 

<!-- tracenote -->
從 heap 配置一個 struct file，將其 vnode 與 f_ops 指向被開啟的節點，並回傳給呼叫端；失敗時回傳 -1 且不改動 *target。
<!-- /tracenote -->

```c 203:203:kernel/src/tmpfs.c
static int tmpfs_open(struct vnode *file_node, struct file **target)
```

#### 11. - 通用層無條件覆寫 vnode、f_ops、f_pos、flags，即使後端已自行填過 

<!-- tracenote -->
- 通用層無條件覆寫 vnode、f_ops、f_pos、flags，即使後端已自行填過
- flags 只在此保存
<!-- /tracenote -->

```c 164:167:kernel/src/vfs.c
file->vnode = node;
file->f_ops = node->f_ops;
file->f_pos = 0;
file->flags = flags;
```

### 4. 寫入資料，並回傳寫入長度。 

<!-- tracenote -->
寫入資料，並回傳寫入長度。
<!-- /tracenote -->

```c 375:375:kernel/src/shell.c
int written = vfs_write(f, msg, (size_t)msg_len);
```

#### 1.  

```c 194:194:kernel/src/vfs.c
int vfs_write(struct file *file, const void *buf, size_t len)
```

#### 2.  

```c 198:198:kernel/src/vfs.c
return file->f_ops->write(file, buf, len);
```

#### 3.  

```c 251:251:kernel/src/tmpfs.c
static int tmpfs_write(struct file *file, const void *buf, size_t len)
```

#### 4. 寫入位置超出檔案大小就直接回傳。 

<!-- tracenote -->
寫入位置超出檔案大小就直接回傳。
<!-- /tracenote -->

```c 260:261:kernel/src/tmpfs.c
if (file->f_pos >= TMPFS_MAX_FILESIZE)
    return 0;
```

#### 5. 配置存放 data 的空間。 

<!-- tracenote -->
配置存放 data 的空間。
<!-- /tracenote -->

```c 263:266:kernel/src/tmpfs.c
if (!node->data) {
    node->data = (char *)kmalloc(TMPFS_MAX_FILESIZE);
    if (!node->data)
        return -1;
```

#### 6. 寫入位置在檔案目前結尾之後時，先補 0 避免殘留之前的 data。 

<!-- tracenote -->
寫入位置在檔案目前結尾之後時，先補 0 避免殘留之前的 data。
<!-- /tracenote -->

```c 273:277:kernel/src/tmpfs.c
/* Writing past the end leaves a gap; zero it so the file never
 * exposes whatever the allocator handed back. */
if (file->f_pos > node->size) {
    for (size_t i = node->size; i < file->f_pos; i++)
        node->data[i] = '\0';
```

#### 7. 複製資料後，更新檔案位置跟檔案大小。 

<!-- tracenote -->
複製資料後，更新檔案位置跟檔案大小。
<!-- /tracenote -->

```c 280:283:kernel/src/tmpfs.c
mem_cpy(node->data + file->f_pos, buf, len);
file->f_pos += len;
if (file->f_pos > node->size)
    node->size = file->f_pos;
```

#### 8. 回傳寫入長度 

<!-- tracenote -->
回傳寫入長度
<!-- /tracenote -->

```c 285:285:kernel/src/tmpfs.c
return (int)len;
```

### 5. 釋放 file handler。 

<!-- tracenote -->
釋放 file handler。
<!-- /tracenote -->

```c 377:377:kernel/src/shell.c
vfs_report("close", vfs_close(f) == 0);
```

#### 1.  

```c 179:179:kernel/src/vfs.c
int vfs_close(struct file *file)
```

#### 2.  

```c 229:229:kernel/src/tmpfs.c
static int tmpfs_close(struct file *file)
```

#### 3. 釋放 file handler 後回傳 

<!-- tracenote -->
釋放 file handler 後回傳
<!-- /tracenote -->

```c 234:235:kernel/src/tmpfs.c
kfree(file);
return 0;
```

### 6. 讀檔並確認長度跟內容與前面寫入的相同。 

<!-- tracenote -->
讀檔並確認長度跟內容與前面寫入的相同。
<!-- /tracenote -->

```c 388:390:kernel/src/shell.c
int got = vfs_read(f, buf, sizeof(buf) - 1);
vfs_report("read back the same length", got == msg_len);
vfs_report("read back the same bytes", str_eq(buf, msg));
```

#### 1.  

```c 209:209:kernel/src/vfs.c
int vfs_read(struct file *file, void *buf, size_t len)
```

#### 2.  

```c 299:299:kernel/src/tmpfs.c
static int tmpfs_read(struct file *file, void *buf, size_t len)
```

#### 3. 沒有 data 或檔案位置在檔案結尾之後就直接回傳。 

<!-- tracenote -->
沒有 data 或檔案位置在檔案結尾之後就直接回傳。
<!-- /tracenote -->

```c 308:309:kernel/src/tmpfs.c
if (!node->data || file->f_pos >= node->size)
    return 0;
```

#### 4. 檔案位置到結尾的資料比要讀的還少的話就只讀到結尾的資料量。 

<!-- tracenote -->
檔案位置到結尾的資料比要讀的還少的話就只讀到結尾的資料量。
<!-- /tracenote -->

```c 311:313:kernel/src/tmpfs.c
size_t avail = node->size - file->f_pos;
if (len > avail)
    len = avail;
```

#### 5. 複製資料到 read buf，移動檔案位置到最後讀完的位置後，再回傳複製的資料量。 

<!-- tracenote -->
複製資料到 read buf，移動檔案位置到最後讀完的位置後，再回傳複製的資料量。
<!-- /tracenote -->

```c 315:318:kernel/src/tmpfs.c
mem_cpy(buf, node->data + file->f_pos, len);
file->f_pos += len;

return (int)len;
```

### 7. 測試寫超過檔案大小上限的資料量時是否正常運作。 

<!-- tracenote -->
測試寫超過檔案大小上限的資料量時是否正常運作。
<!-- /tracenote -->

```c 419:433:kernel/src/shell.c
/* Boundary: writing past the 4096-byte cap is truncated, not
 * refused, so the return value reports the short write. */
f = NULL;
if (vfs_open("/big.txt", O_CREAT, &f) == 0 && f) {
    static char big[5000];
    for (int i = 0; i < (int)sizeof(big); i++)
        big[i] = 'A';
    int n = vfs_write(f, big, sizeof(big));
    vfs_report("write beyond 4096 is truncated to 4096", n == 4096);
    vfs_report("further write at the cap returns 0",
               vfs_write(f, big, 16) == 0);
    vfs_close(f);
} else {
    vfs_report("write beyond 4096 is truncated to 4096", 0);
}
```

#### 1.  

```c 133:133:kernel/src/vfs.c
int vfs_open(const char *pathname, int flags, struct file **target)
```

