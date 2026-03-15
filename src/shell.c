#include "uart.h"
#include "sbi.h"

#define SHELL_BUF_SIZE 128

#define BOOT_MAGIC 0x544F4F42UL /* "BOOT" */
#ifdef QEMU
#define KERNEL_LOAD_ADDR 0x82000000UL
#else
#define KERNEL_LOAD_ADDR 0x20000000UL
#endif // QEMU

/* Read one byte from UART; retry on error. Returns 0..255. */
static unsigned char uart_getc_byte(void)
{
    int c;

    do {
        c = uart_getc();
    } while (c < 0);

    return (unsigned char)(c & 0xFF);
}

static void print_hex_ulong(unsigned long x)
{
    char buf[2 * sizeof(unsigned long)];
    int i;

    for (i = (int)(sizeof(unsigned long) * 2) - 1; i >= 0; --i) {
        unsigned long nibble = x & 0xFUL;
        if (nibble < 10UL) {
            buf[i] = (char)('0' + nibble);
        } else {
            buf[i] = (char)('a' + (nibble - 10UL));
        }
        x >>= 4;
    }

    for (i = 0; i < (int)(sizeof(unsigned long) * 2); ++i) {
        uart_putc((unsigned char)buf[i]);
    }
}

static void print_dec_ulong(unsigned long x)
{
    char buf[32];
    int i = 0;

    if (x == 0) {
        uart_putc('0');
        return;
    }

    while (x > 0 && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (x % 10));
        x /= 10;
    }

    while (i > 0) {
        uart_putc((unsigned char)buf[--i]);
    }
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *b && (*a == *b)) {
        ++a;
        ++b;
    }
    return (*a == '\0' && *b == '\0');
}

static void shell_print_help(void)
{
    uart_puts("  help  - show all commands.\n");
    uart_puts("  hello - print Hello world.\n");
    uart_puts("  info  - print system info.\n");
    uart_puts("  load  - receive kernel over UART and boot.\n");
}

static void shell_load_kernel(void)
{
    unsigned int magic;
    unsigned int size;
    volatile unsigned char *dst;
    unsigned int i;

    uart_puts("Waiting for kernel over UART...\n");

    /* Read 8-byte header: magic (LE) + size (LE) */
    magic = (unsigned int)uart_getc_byte()
            | ((unsigned int)uart_getc_byte() << 8)
            | ((unsigned int)uart_getc_byte() << 16)
            | ((unsigned int)uart_getc_byte() << 24);
    size = (unsigned int)uart_getc_byte()
           | ((unsigned int)uart_getc_byte() << 8)
           | ((unsigned int)uart_getc_byte() << 16)
           | ((unsigned int)uart_getc_byte() << 24);

    if (magic != BOOT_MAGIC) {
        uart_puts("Invalid header (bad magic).\n");
        return;
    }

    dst = (volatile unsigned char *)KERNEL_LOAD_ADDR;
    for (i = 0; i < size; ++i)
        dst[i] = uart_getc_raw();

    uart_puts("Loaded ");
    print_dec_ulong((unsigned long)size);
    uart_puts(" bytes, jumping to ");
    print_hex_ulong((unsigned long)dst);
    uart_puts("...\n");

    /* Jump to loaded kernel; do not return. */
    ((void (*)())dst)();
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