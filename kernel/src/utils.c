#include "uart.h"
#include "utils.h"

int str_eq(const char *a, const char *b)
{
    while (*a && *b && (*a == *b)) {
        ++a;
        ++b;
    }
    return (*a == '\0' && *b == '\0');
}

int str_startswith(const char *str, const char *prefix)
{
    while (*prefix) {
        if (*str != *prefix) {
            return 0;
        }
        ++str;
        ++prefix;
    }
    return 1;
}

void *mem_cpy(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void print_hex_ulong(unsigned long x)
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

void print_hex_u32(unsigned int x)
{
    char buf[8];
    int i;

    for (i = 7; i >= 0; --i) {
        unsigned int nibble = x & 0xFU;
        if (nibble < 10U) {
            buf[i] = (char)('0' + nibble);
        } else {
            buf[i] = (char)('a' + (nibble - 10U));
        }
        x >>= 4;
    }
    for (i = 0; i < 8; ++i) {
        uart_putc((unsigned char)buf[i]);
    }
}

void print_dec_ulong(unsigned long x)
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