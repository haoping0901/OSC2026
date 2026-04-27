# Trace Result - 2026-04-19

## 1.  

```c 124:124:kernel/src/main.c
trap_init();
```

### 1.  

```c 32:32:kernel/src/trap.c
void trap_init(void)
```

## 2. 執行指定檔案 

```c 260:260:kernel/src/shell.c
run_user_program(name);
```

### 1.  

```c 44:44:kernel/src/shell.c
static void run_user_program(const char *name)
```

### 2. 定位指定檔案 data 位置，並找出檔案大小。 

```c 55:55:kernel/src/shell.c
if (cpio_find(initrd, name, &src, &src_size) != 0) {
```

### 3.  

```c 81:81:kernel/src/shell.c
trap_set_user_base(entry);
```

### 4.  

```c 82:82:kernel/src/shell.c
enter_user_mode(entry, user_sp);
```

#### 1.  

```plaintext 236:236:kernel/src/trap_entry.S
enter_user_mode:
```

#### 2. 把當前 kernel stack pointer 寫入 sscratch 

```plaintext 123:123:kernel/src/trap_entry.S
csrw  sscratch, sp
```

