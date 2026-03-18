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