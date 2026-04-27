# Trace Result - 2026-04-11

## 1. 初始化 buddy system: 

- 以 order 規劃記憶體並紀錄結果到 free_list
- 紀錄 page 狀態到 frame array

```c 59:59:kernel/src/main.c
buddy_startup_init(mem_base, mem_size);
```

### 1.  

```c 458:458:kernel/src/buddy.c
void buddy_startup_init(uintptr_t mem_base, uintptr_t mem_size)
```

## 2. %%Tip%%  

```c 71:71:kernel/src/main.c
buddy_startup_reserve((uintptr_t)dtb_ptr, (uintptr_t)dtb_ptr + dtb_size);
```

### 1. 紀錄 reserved memory region 到 g_sa_reserves。 

```c 490:492:kernel/src/buddy.c
g_sa_reserves[g_sa_reserve_count].start = start;
g_sa_reserves[g_sa_reserve_count].end   = end;
g_sa_reserve_count++;
```

## 3.  

```c 82:82:kernel/src/main.c
uintptr_t initrd_start = dtb_getprop("/chosen", "linux,initrd-start");
```

### 1.  

```c 264:264:kernel/src/dtb.c
uintptr_t dtb_getprop(const char *path, const char *prop_name)
```

### 2.  

```c 267:267:kernel/src/dtb.c
const void *value = _dtb_getprop(path, prop_name, &len);
```

### 3.  

```c 136:137:kernel/src/dtb.c
const void *_dtb_getprop(const char *node_path, const char *prop_name,
                       int *lenp)
```

### 4. 紀錄當前位在 device tree 的哪一層 

```c 160:160:kernel/src/dtb.c
int current_depth = -1;
```

### 5. %%Tip%% 紀錄目前配對到幾層了 

```c 161:161:kernel/src/dtb.c
int matched_depth = -1;
```

### 6. 遍歷 struct block 找出要找的 node 的 property。 

```c 163:163:kernel/src/dtb.c
while (cur < struct_end) {
```

### 7. %%Tip%% 找到 device tree 某 level 的起點時 

```c 167:167:kernel/src/dtb.c
if (token == FDT_BEGIN_NODE) {
```

#### 1. 儲存節點名稱起點，並移動到名稱後。 

```c 169:172:kernel/src/dtb.c
while (*cur)
    ++cur;
++cur;
cur = (char *)align_32((uint64_t)cur);
```

#### 2. 還沒找到目標節點時 

```c 184:184:kernel/src/dtb.c
if (matched_depth < 0) {
```

#### 3. 取得 path segment 的開頭。 

假設要找的是 /soc/test，當前走到 /soc/serial，current_depth 會是 1，seg 會指向 test 的開頭，seg_len 會被設為 "/soc/" 的長度。

```c 193:195:kernel/src/dtb.c
const char *seg = path_segment(node_path,
                               current_depth - 1,
                               &seg_len);
```

#### 4. 當節點名稱比對正確，並且當前 tree level 跟要找的 node 的 level 相同的話，代表找到要找的 node。 

```c 205:209:kernel/src/dtb.c
if (node_name_match(node_name, seg_buf)) {
    if (current_depth == wanted_depth)
        matched_depth = current_depth;
    /* else: continue descending */
}
```

### 8.  

```c 215:215:kernel/src/dtb.c
} else if (token == FDT_END_NODE) {
```

### 9. %%Tip%%  

```c 220:220:kernel/src/dtb.c
} else if (token == FDT_PROP) {
```

#### 1. 已經找到目標節點時。 

match_depth 只會在找到目標 node 時被設成 > 0 的數。

```c 227:227:kernel/src/dtb.c
if (matched_depth >= 0) {
```

#### 2. 比較當前看到的 property 跟要找的 property 是不是一樣的。 

```c 229:237:kernel/src/dtb.c
const char *a = name, *b = prop_name;
int eq = 1;
while (*a || *b) {
    if (*a != *b) {
        eq = 0;
        break;
    }
    ++a, ++b;
}
```

#### 3. 一樣的話就回傳 property 的內容跟長度。 

```c 238:242:kernel/src/dtb.c
if (eq == 1) {
    if (lenp)
        *lenp = (int)prop_len;
    return cur;
}
```

### 10.  

```c 251:251:kernel/src/dtb.c
} else if (token == FDT_END) {
```

## 4. 將 reserved-memory 下的所有 property 的 reg 涵蓋的範圍設成 reserved。 

```c 95:95:kernel/src/main.c
dtb_walk_reserved_memory(reserve_startup_region);
```

### 1.  

```c 413:413:kernel/src/dtb.c
void dtb_walk_reserved_memory(void (*cb)(uintptr_t base, uintptr_t size))
```

### 2. 找到紀錄預留記憶體空間的 reserved-memory node 後，切換 state 為 1。 

```c 450:451:kernel/src/dtb.c
if (node_name_match(node_name, "reserved-memory"))
    state = 1;
```

### 3. 找完 reserved-memory node 下的所有 property，就離開迴圈。 

```c 455:456:kernel/src/dtb.c
if (state == 1 && current_depth == 1)
    state = 2;  /* exited /reserved-memory */
```

### 4. 是 reserved-memory node 下的 property 時 

```c 465:465:kernel/src/dtb.c
if (state == 1 && current_depth == 2) {
```

### 5. 找到 reserved-memory node 下的 reg property 時 

```c 476:476:kernel/src/dtb.c
if (is_reg) {
```

### 6. 以 16-byte (8-byte addr & 8-byte size) 為單位取出此 node 所有 reserved 的 memory 區間。 

```c 480:487:kernel/src/dtb.c
for (uint32_t e = 0; e < entries; e++, p += 4) {
    uint64_t addr = ((uint64_t)bswap_32(p[0]) << 32) |
                     (uint64_t)bswap_32(p[1]);
    uint64_t sz   = ((uint64_t)bswap_32(p[2]) << 32) |
                     (uint64_t)bswap_32(p[3]);
    if (sz > 0)
        cb((uintptr_t)addr, (uintptr_t)sz);
}
```

## 5. %%Tip%%  

```c 105:105:kernel/src/main.c
int         *frame_array    = (int *)buddy_startup_alloc(fa_bytes);
```

### 1.  

```c 501:501:kernel/src/buddy.c
void *buddy_startup_alloc(unsigned long size)
```

### 2. %%Tip%% 把目前登記的所有保留區域按起始位址從小到大排序 

```c 508:508:kernel/src/buddy.c
sa_sort_reserves();
```

#### 1.  

```c 409:409:kernel/src/buddy.c
static void sa_sort_reserves(void)
```

#### 2. 把 reserve region 起始位址較大的往後擺 

```c 413:416:kernel/src/buddy.c
while (j >= 0 && g_sa_reserves[j].start > ks) {
    g_sa_reserves[j + 1] = g_sa_reserves[j];
    j--;
}
```

### 3. %%Tip%% 找可用空間 

```c 514:514:kernel/src/buddy.c
uintptr_t ret = sa_find_first_free(size);
```

#### 1.  

```c 432:432:kernel/src/buddy.c
static uintptr_t sa_find_first_free(unsigned long size)
```

#### 2. 第一次找時，從 memory region 起點開始找足夠放 size 的空間。否則從上次找到的空間的結尾開始找。 

```c 437:437:kernel/src/buddy.c
uintptr_t cursor = g_sa_mem_base;
```

#### 3. 掃目前紀錄過得所有保留區域。 

P.S. 前面有從小到大排序過了。

```c 439:439:kernel/src/buddy.c
for (int i = 0; i <= g_sa_reserve_count; ++i) {
```

#### 4. 定位可用空間的終點為下一個保留區間的起點。 

```c 441:443:kernel/src/buddy.c
uintptr_t window_end = (i < g_sa_reserve_count)
                       ? g_sa_reserves[i].start
                       : g_sa_mem_end;
```

#### 5. 起點加需要的空間沒碰到終點的話，代表找到足夠到的空間。 

```c 448:449:kernel/src/buddy.c
if (cursor + size <= window_end)
    return cursor;
```

#### 6. 更新下個起點為保留區間的終點。 

```c 452:453:kernel/src/buddy.c
if (i < g_sa_reserve_count)
    cursor = g_sa_reserves[i].end;
```

### 4. 紀錄這次找到的空間，避免後續被用到。 

```c 521:523:kernel/src/buddy.c
g_sa_reserves[g_sa_reserve_count].start = ret;
g_sa_reserves[g_sa_reserve_count].end   = ret + size;
g_sa_reserve_count++;
```

#### 1.  (Orphaned)

```c 422:423:kernel/src/buddy.c
static int sa_overlaps_reserved(uintptr_t a, uintptr_t b)
```

## 6. 初始化 frame_array 

```c 113:113:kernel/src/main.c
buddy_init(mem_base, mem_size, frame_array, total_pages);
```

### 1.  

```c 170:171:kernel/src/buddy.c
void buddy_init(uintptr_t base, uintptr_t size,
                int *ext_frame_array, unsigned long frame_count)
```

### 2. 初始化 free_list，free_list[i] 後續會指向 order 為 i 的 page frame pool 的開頭。 

```c 179:180:kernel/src/buddy.c
for (int i = 0; i <= MAX_ORDER; i++)
    INIT_LIST_HEAD(&free_list[i]);
```

## 7.  

```c 118:118:kernel/src/main.c
buddy_startup_replay_reserves();
```

### 1.  

```c 533:533:kernel/src/buddy.c
void buddy_startup_replay_reserves(void)
```

### 2. 把剛剛紀錄過得保留區域在 buddy system 中也設為保留。 

```c 539:540:kernel/src/buddy.c
for (int i = 0; i < g_sa_reserve_count; i++)
    buddy_reserve(g_sa_reserves[i].start, g_sa_reserves[i].end);
```

## 8.  

```c 121:121:kernel/src/main.c
buddy_build_free_lists();
```

### 1.  

```c 231:231:kernel/src/buddy.c
void buddy_build_free_lists(void)
```

### 2. 從最大的 order 開始分配 memroy。 

```c 248:248:kernel/src/buddy.c
int order = MAX_ORDER;
```

### 3. 從大到小遍歷所有 order (優先分配大空間)。 

```c 249:249:kernel/src/buddy.c
while (order > 0) {
```

### 4. 如果 page idx 為 order 倍數，並且 idx 後的空間足夠一個 order 所需的大小的話，就不再繼續往下一個更小的 order 找可用的 order。 

```c 253:253:kernel/src/buddy.c
if ((idx & (block_pages - 1)) == 0 && idx + block_pages <= TOTAL_PAGES)
    break;
```

### 5. 確保找到的 order block 中，沒有被 reserved 的 frame page。 

```c 260:265:kernel/src/buddy.c
for (unsigned long k = 0; k < block; k++) {
    if (frame_array[idx + k] == FRAME_RESERVED) {
        clean = 0;
        break;
    }
}
```

### 6. %%Tip%% 把剛剛找到的 order 紀錄到 frame array 與 free_list 上。 

```c 271:271:kernel/src/buddy.c
block_push(idx, order);
```

## 9. 初始化 dynamic allocation 會用到的 

```c 124:124:kernel/src/main.c
kmalloc_init(page_pool_idx, total_pages);
```

### 1.  

```c 87:87:kernel/src/kmalloc.c
void kmalloc_init(signed char *ext_page_pool_idx, unsigned long page_count)
```

### 2. 初始化紀錄 page 是否被分配給特定 order 的 array。 

```c 98:99:kernel/src/kmalloc.c
for (unsigned long i = 0; i < total; i++)
    page_pool_idx[i] = -1;
```

## 10.  

```c 127:127:kernel/src/main.c
shell();
```

### 1.  

```c 224:224:kernel/src/shell.c
shell_handle_command(buf);
```

### 2. 測試 memory allocation 

```c 182:182:kernel/src/shell.c
test_alloc_1();
```

## 11.  

```c 106:106:kernel/src/kmalloc.c
void *kmalloc(unsigned long size)
```

### 1. %%Tip%% 找出需要的 memory size <= 哪個 order 

```c 112:112:kernel/src/kmalloc.c
int pidx = find_pool(size);
```

### 2. %%Tip%% 需要的 memory size 大於所有 order 時 (pidx = -1) 

```c 113:113:kernel/src/kmalloc.c
if (pidx < 0) {
```

### 3. 改為呼叫大範圍分配的 buddy system memory allocation。 

```c 114:114:kernel/src/kmalloc.c
void *ptr = buddy_alloc(size);
```

### 4. 取得空間後，更新紀錄 page 是被用做哪個 order 的 array。 

```c 119:120:kernel/src/kmalloc.c
for (unsigned long i = 0; i < pages; i++)
    page_pool_idx[base + i] = -1;
```

### 5. %%Tip%% 沒有作為 order 分配的 page 時 

```c 129:129:kernel/src/kmalloc.c
if (pool->free_list == NULL) {
```

### 6. 先取得一個 page，後續會將其以 order 來分配 

```c 130:130:kernel/src/kmalloc.c
void *page = buddy_alloc(PAGE_SIZE);
```

### 7. 紀錄此 page 會作以 order 為單位分配 

```c 135:135:kernel/src/kmalloc.c
page_pool_idx[pg_idx] = (signed char)pidx;
```

### 8. 把所有大小為 order 的 chunk 紀錄到 pools[order] 的 free list 開頭， 

```c 140:145:kernel/src/kmalloc.c
for (unsigned long i = 0; i < num_chunks; i++) {
    chunk_node_t *node =
        (chunk_node_t *)((uintptr_t)page + i * chunk_sz);
    node->next = pool->free_list;
    pool->free_list = node;
}
```

### 9. 取出 pools[order] free_list 的開頭使用 

```c 149:150:kernel/src/kmalloc.c
chunk_node_t *chunk = pool->free_list;
pool->free_list = chunk->next;
```

## 12.  

```c 156:156:kernel/src/kmalloc.c
void kfree(void *ptr)
```

### 1. 取得 align page size 的 addr，並轉換為 page idx。 

```c 164:165:kernel/src/kmalloc.c
uintptr_t page_base = addr & ~(PAGE_SIZE - 1);
unsigned long pg_idx = addr_to_page_idx(page_base);
```

### 2. 取得 page 的分配狀態。 

```c 167:167:kernel/src/kmalloc.c
signed char pidx = page_pool_idx[pg_idx];
```

### 3. page 以 order 來分配的話，將要歸還得 addr 加到 pools[order] free_list 的開頭。 

```c 176:179:kernel/src/kmalloc.c
pool_t *pool = &pools[pidx];
chunk_node_t *node = (chunk_node_t *)ptr;
node->next = pool->free_list;
pool->free_list = node;
```

