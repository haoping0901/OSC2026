#include "uart.h"
#include "shell.h"
#include "dtb.h"
#include "types.h"
#include "buddy.h"

int main(unsigned long hart_id, void *dtb_ptr)
{
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
    uart_puts("Welcome to OPI-RV2!\n");

    /* Step 5: Initialise the buddy page-frame allocator. */
    buddy_init();

    /* Step 6: Continue with the interactive shell. */
    shell();

    return 0;
}