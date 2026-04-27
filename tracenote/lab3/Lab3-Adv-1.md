# Trace Result - 2026-04-05

## 1. 初始化 buddy system: 

- 以 order 規劃記憶體並紀錄結果到 free_list
- 紀錄 page 狀態到 frame array

```c 30:30:kernel/src/main.c
buddy_init();
```

### 1.  

```c 164:164:kernel/src/buddy.c
void buddy_init(void)
```

### 2. 初始化 free_list，free_list[i] 後續會指向 order 為 i 的 page frame pool 的開頭。 

```c 167:168:kernel/src/buddy.c
for (int i = 0; i <= MAX_ORDER; i++)
    INIT_LIST_HEAD(&free_list[i]);
```

### 3. 遍歷整個 memory 

```c 179:179:kernel/src/buddy.c
while (idx < TOTAL_PAGES) {
```

### 4. 從最大的 order 開始分配 memroy。 

```c 185:185:kernel/src/buddy.c
int order = MAX_ORDER;
```

### 5. 從大到小遍歷所有 order (優先分配大空間)。 

```c 186:186:kernel/src/buddy.c
while (order > 0) {
```

### 6. 如果 page idx 為 order 倍數，並且 idx 後的空間足夠一個 order 所需的大小的話，就不再繼續往下一個更小的 order 找可用的 order。 

```c 188:189:kernel/src/buddy.c
if ((idx & (block_pages - 1)) == 0 && idx + block_pages <= TOTAL_PAGES)
    break;
```

### 7. %%Tip%% 把剛剛找到的 order 紀錄到 frame array 與 free_list 上。 

```c 192:192:kernel/src/buddy.c
block_push(idx, order);
```

## 2.  

```c 33:33:kernel/src/main.c
kmalloc_init();
```

### 1.  

```c 86:86:kernel/src/kmalloc.c
void kmalloc_init(void)
```

## 3. %%Tip%%  

```c 103:103:kernel/src/kmalloc.c
void *kmalloc(unsigned long size)
```

### 1. %%Tip%% 找出需要的 memory size <= 哪個 order 

```c 109:109:kernel/src/kmalloc.c
int pidx = find_pool(size);
```

### 2. %%Tip%% 需要的 memory size 大於所有 order 時 (pidx = -1) 

```c 110:110:kernel/src/kmalloc.c
if (pidx < 0) {
```

### 3. 改為呼叫大範圍分配的 buddy system memory allocation。 

```c 111:111:kernel/src/kmalloc.c
void *ptr = buddy_alloc(size);
```

### 4. 取得空間後，更新紀錄 page 是被用做哪個 order 的 array。 

```c 116:117:kernel/src/kmalloc.c
for (unsigned long i = 0; i < pages; i++)
    page_pool_idx[base + i] = -1;
```

### 5. %%Tip%% 沒有作為 order 分配的 page 時 

```c 126:126:kernel/src/kmalloc.c
if (pool->free_list == NULL) {
```

### 6. 先取得一個 page，後續會將其以 order 來分配 

```c 127:127:kernel/src/kmalloc.c
void *page = buddy_alloc(PAGE_SIZE);
```

### 7. 紀錄此 page 會作以 order 為單位分配 

```c 132:132:kernel/src/kmalloc.c
page_pool_idx[pg_idx] = (signed char)pidx;
```

### 8. 把所有大小為 order 的 chunk 紀錄到 pools[order] 的 free list 開頭， 

```c 137:142:kernel/src/kmalloc.c
for (unsigned long i = 0; i < num_chunks; i++) {
    chunk_node_t *node =
        (chunk_node_t *)((uintptr_t)page + i * chunk_sz);
    node->next = pool->free_list;
    pool->free_list = node;
}
```

### 9. 取出 pools[order] free_list 的開頭使用 

```c 146:147:kernel/src/kmalloc.c
chunk_node_t *chunk = pool->free_list;
pool->free_list = chunk->next;
```

## 4. %%Tip%%  

```c 153:153:kernel/src/kmalloc.c
void kfree(void *ptr)
```

### 1. 取得 align page size 的 addr，並轉換為 page idx。 

```c 161:162:kernel/src/kmalloc.c
uintptr_t page_base = addr & ~(PAGE_SIZE - 1);
unsigned long pg_idx = addr_to_page_idx(page_base);
```

### 2. 取得 page 的分配狀態。 

```c 164:164:kernel/src/kmalloc.c
signed char pidx = page_pool_idx[pg_idx];
```

### 3. page 以 order 來分配的話，將要歸還得 addr 加到 pools[order] free_list 的開頭。 

```c 173:176:kernel/src/kmalloc.c
pool_t *pool = &pools[pidx];
chunk_node_t *node = (chunk_node_t *)ptr;
node->next = pool->free_list;
pool->free_list = node;
```

