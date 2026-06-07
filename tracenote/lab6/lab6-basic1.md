# Trace Result - 2026-06-07

## 1. 開機後，啟用 paging 並將 virtual address 

### 1.  

```plaintext 27:27:kernel/src/start.S
call setup_vm      /* build page tables, write satp, sfence.vma */
```

### 2. 建 linear page table 

```c 145:145:kernel/src/mm.c
void setup_vm(void)
```

### 3. 建 PMD table 

```c 152:152:kernel/src/mm.c
map_linear_range(pmd[g], gib_base);
```

#### 1.  

```c 123:123:kernel/src/mm.c
static void map_linear_range(unsigned long *table, unsigned long gib_base)
```

#### 2. 判斷 pa 是 RAM 還是 device，並設定對應 PTE flags。 

```c 108:108:kernel/src/mm.c
static inline unsigned long leaf_prot(unsigned long pa)
```

#### 3. 設定 PTE 的 physical page number 跟 flags。 

```c 127:127:kernel/src/mm.c
table[i] = MAKE_PTE(pa, leaf_prot(pa));
```

### 4. 先 map 同一塊 physical memory 到之後使用的高位 virtual address (0xffffffc000000000UL) 跟相同的 virtual address。 

```c 155:157:kernel/src/mm.c
unsigned long pmd_pte = MAKE_PTE((unsigned long)pmd[g], PTE_V);
pgd[g]               = pmd_pte;   /* identity half    */
pgd[hh_pgd_base + g] = pmd_pte;   /* higher-half half */
```

### 5. 載入 1f label 的位址，並加上 VA offset 後跳過去執行。 

在 setup_vm 中已經開啟 paging，因此在高位執行是找的到程式的。

```plaintext 34:37:kernel/src/start.S
la   t0, 1f
li   t1, KERNEL_VA_OFFSET
add  t0, t0, t1
jr   t0
```

### 6.  

```plaintext 54:54:kernel/src/start.S
call drop_identity_map   /* zero low PGD entries + sfence.vma */
```

#### 1. 把 identity mapping 資訊移除，避免後續存取低位 virtual memory 時，用到先前紀錄的 kernel 資訊。 

```c 177:177:kernel/src/mm.c
void drop_identity_map(void)
```

#### 2. Flush TLB，確保低位 virtual memory 的 identity mapping 被移除。 

```c 182:182:kernel/src/mm.c
asm volatile ("sfence.vma zero, zero" ::: "memory");
```

## 2. malloc 

### 1.  

```c 92:92:kernel/src/kmalloc.c
void *kmalloc(unsigned long size)
```

### 2. 配置 memory，回傳的位址是 virtual address。 

```c 100:100:kernel/src/kmalloc.c
void *ptr = buddy_alloc(size);
```

### 3. 維護 buddy system 各個 struccture 時，都以 physical address 進行。 

```c 104:104:kernel/src/kmalloc.c
unsigned long base  = ptr_to_page_idx(ptr);
```

