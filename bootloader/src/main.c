#include "uart.h"
#include "shell.h"
#include "dtb.h"
#include "utils.h"
#include "types.h"

#ifdef QEMU
#define RELOCATE_ADDR 0x82000000UL
#else
#define RELOCATE_ADDR 0x20000000UL
#endif // QEMU

extern unsigned long _code_start;
extern unsigned long _code_end;

static int need_relocated = 1;

void code_relocate(void *dtb_ptr)
{
    unsigned long *relocate_addr = (unsigned long *)RELOCATE_ADDR;
    unsigned long *code_start = (unsigned long *)&_code_start;
    unsigned long *code_end = (unsigned long *)&_code_end;

    while (code_start != code_end)
    {
        *relocate_addr++ = *code_start++;
    }

    asm volatile("fence.i" ::: "memory");

    ((void (*)(unsigned long, void *))RELOCATE_ADDR)(0, dtb_ptr);
}

int main(unsigned long hart_id, void *dtb_ptr)
{
    if (need_relocated == 1) {
        need_relocated = 0;
        code_relocate(dtb_ptr);
    }
    /* Step 1: Register the DTB address so the parser can use it. */
    dtb_set_addr(dtb_ptr);

    /* Step 2: Resolve UART base address from the devicetree.
     *
     *  OrangePi RV2 : /soc/serial
     *  QEMU virt    : /soc/serial  (node name "serial@10000000")
     *
     * Both boards use the same logical path; node_name_match() in dtb.c
     * handles the "@<unit-addr>" suffix transparently.
     */
    uintptr_t u_base = dtb_getprop("/soc/serial", "reg");

    /* Step 3: Override the UART driver's base address before first use. */
    uart_set_base(u_base);

    /* Step 4: Announce the resolved address (sanity check). */
    uart_puts("Welcome to OPI-RV2 bootloader!\n");

    /* Step 5: Continue with the interactive shell. */
    shell();

    return 0;
}