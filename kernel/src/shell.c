#include "uart.h"
#include "sbi.h"
#include "utils.h"
#include "dtb.h"
#include "cpio.h"
#include "buddy.h"
#include "types.h"

#define SHELL_BUF_SIZE 128

#define BOOT_MAGIC 0x544F4F42UL /* "BOOT" */
#ifdef QEMU
#define KERNEL_LOAD_ADDR 0x82000000UL
#else
#define KERNEL_LOAD_ADDR 0x20000000UL
#endif // QEMU

static void shell_print_help(void)
{
    uart_puts("  help  - show all commands.\n");
    uart_puts("  hello - print Hello world.\n");
    uart_puts("  info  - print system info.\n");
    uart_puts("  load  - receive kernel over UART and boot.\n");
    uart_puts("  ls    - list files in the initial ramdisk.\n");
    uart_puts("  cat   - print content of a file in the initial ramdisk.\n");
    uart_puts("  test  - run memory allocator test.\n");
}

/* ---------- Lab 3 test case --------------------------------------------- */

static void test_alloc_1(void)
{
    uart_puts("=== Testing page frame allocation ===\n");

    uart_puts("alloc 4000\n");
    char *ptr1 = (char *)buddy_alloc(4000);
    uart_puts("alloc 8000\n");
    char *ptr2 = (char *)buddy_alloc(8000);
    uart_puts("alloc 4000\n");
    char *ptr3 = (char *)buddy_alloc(4000);
    uart_puts("alloc 4000\n");
    char *ptr4 = (char *)buddy_alloc(4000);

    uart_puts("free 4000\n");
    buddy_free(ptr1);
    uart_puts("free 8000\n");
    buddy_free(ptr2);
    uart_puts("free 4000\n");
    buddy_free(ptr3);
    uart_puts("free 4000\n");
    buddy_free(ptr4);

    uart_puts("alloc 16\n");
    char *ptr5 = (char *)buddy_alloc(16);
    uart_puts("alloc 32\n");
    char *ptr6 = (char *)buddy_alloc(32);

    uart_puts("free 16\n");
    buddy_free(ptr5);
    uart_puts("free 32\n");
    buddy_free(ptr6);

    // Test allocate new page if the cache is not enough
    void *ptr[102];
    for (int i = 0; i < 100; i++) {
        ptr[i] = (char *)buddy_alloc(128);
    }
    for (int i = 0; i < 100; i++) {
        buddy_free(ptr[i]);
    }

    // Test exceeding the maximum size
    char *ptr7 = (char *)buddy_alloc(TOTAL_PAGES * PAGE_SIZE + 1);
    if (ptr7 == NULL) {
        uart_puts("Allocation failed as expected for size > MAX_ALLOC_SIZE\n");
    } else {
        uart_puts("Unexpected allocation success for size > MAX_ALLOC_SIZE\n");
        buddy_free(ptr7);
    }

    uart_puts("=== Page frame allocation test done ===\n");
}

static void shell_load_kernel(void)
{
    unsigned int magic;
    unsigned int size;
    volatile unsigned char *dst;
    unsigned int i;

    uart_puts("Waiting for kernel over UART...\n");

    /* Read 8-byte header: magic (LE) + size (LE) */
    magic = (unsigned int)uart_getc() | ((unsigned int)uart_getc() << 8) |
            ((unsigned int)uart_getc() << 16) |
            ((unsigned int)uart_getc() << 24);
    size = (unsigned int)uart_getc() | ((unsigned int)uart_getc() << 8) |
           ((unsigned int)uart_getc() << 16) |
           ((unsigned int)uart_getc() << 24);

    if (magic != BOOT_MAGIC) {
        uart_puts("Invalid header (bad magic).\n");
        return;
    }

    dst = (volatile unsigned char *)KERNEL_LOAD_ADDR;
    for (i = 0; i < size; ++i)
        dst[i] = uart_getc_raw();

    uart_puts("Loaded ");
    print_dec_ulong((unsigned long)size);
    uart_puts(" bytes, jumping to 0x");
    print_hex_ulong((unsigned long)dst);
    uart_puts(" ...\n");

    /* Jump to loaded kernel; do not return. */
    ((void (*)(unsigned long, void *))dst)(0, dtb_get_addr());
}

static void shell_print_info(void)
{
    struct sbiret ret;

    ret = sbi_get_spec_version();
    if (ret.error == 0) {
        uart_puts("OpenSBI spec version: 0x");
        print_hex_ulong((unsigned long)ret.value);
        uart_puts("\n");
    } else {
        uart_puts("OpenSBI spec version: error ");
        print_dec_ulong((unsigned long)ret.error);
        uart_puts("\n");
    }

    ret = sbi_get_impl_id();
    if (ret.error == 0) {
        uart_puts("Implementation ID: 0x");
        print_hex_ulong((unsigned long)ret.value);
        uart_puts("\n");
    } else {
        uart_puts("Implementation ID: error ");
        print_dec_ulong((unsigned long)ret.error);
        uart_puts("\n");
    }

    ret = sbi_get_impl_version();
    if (ret.error == 0) {
        uart_puts("Implementation version: 0x");
        print_hex_ulong((unsigned long)ret.value);
        uart_puts("\n");
    } else {
        uart_puts("Implementation version: error ");
        print_dec_ulong((unsigned long)ret.error);
        uart_puts("\n");
    }
}

static void shell_handle_command(const char *cmd)
{
    if (str_eq(cmd, "help") != 0) {
        shell_print_help();
    } else if (str_eq(cmd, "hello") != 0) {
        uart_puts("Hello world.\n");
    } else if (str_eq(cmd, "info") != 0) {
        shell_print_info();
    } else if (str_eq(cmd, "load") != 0) {
        shell_load_kernel();
    }
    else if (str_eq(cmd, "test") != 0) {
        test_alloc_1();
    }
    else if (str_eq(cmd, "ls") != 0)
    {
        cpio_ls((void *)dtb_getprop("/chosen", "linux,initrd-start"));
    } else if (str_startswith(cmd, "cat ")) {
        const char *filename = cmd + 4;
        /* skip leading spaces */
        while (*filename == ' ') {
            filename++;
        }
        if (*filename == '\0') {
            uart_puts("usage: cat <filename>\n");
        } else {
            cpio_cat((void *)dtb_getprop("/chosen", "linux,initrd-start"), filename);
        }
    } else if (str_eq(cmd, "cat") != 0) {
        uart_puts("usage: cat <filename>\n");
    } else if (*cmd != '\0') {
        uart_puts("Unknown command: ");
        uart_puts(cmd);
        uart_puts("\n");
    }
}

void shell(void)
{
    char buf[SHELL_BUF_SIZE];
    int len = 0;

    uart_puts("opi-rv2> ");

    while (1) {
        int c = uart_getc();

        if (c < 0) {
            continue;
        }

        if (c == '\n') {
            uart_putc('\n');
            buf[len] = '\0';
            shell_handle_command(buf);
            len = 0;
            uart_puts("opi-rv2> ");
        } else if (c == '\b' || c == 127) {
            if (len > 0) {
                len--;
                uart_puts("\b \b");
            }
        } else {
            if (len < SHELL_BUF_SIZE - 1) {
                buf[len++] = (char)c;
                uart_putc((char)c);
            }
        }
    }
}