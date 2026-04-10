#include "uart.h"
#include "shell.h"
#include "dtb.h"
#include "types.h"
#include "buddy.h"
#include "kmalloc.h"
#include "utils.h"

/* Kernel image boundaries exported by the linker script. */
extern char _kernel_start[];
extern char _kernel_end[];

/*
 * Adapter: dtb_walk_reserved_memory provides (base, size) but
 * buddy_reserve takes (start, end).
 */
static void reserve_dtb_region(uintptr_t base, uintptr_t size)
{
    buddy_reserve(base, base + size);
}

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
    uart_set_base(u_base);

    uart_puts("Welcome to OPI-RV2!\n");
    uart_puts("Uart base address: 0x");
    print_hex_ulong(u_base);
    uart_puts("\n");

    /* Step 3: Retrieve the first usable memory region from the DTB
     * <memory> node instead of using hardcoded addresses. */
    uintptr_t mem_base = 0;
    uintptr_t mem_size = 0;
    if (dtb_get_memory_region(&mem_base, &mem_size) != 0) {
        uart_puts("[FATAL] Cannot find memory node in DTB\n");
        return -1;
    }
    uart_puts("[Mem] base=0x");
    print_hex_ulong(mem_base);
    uart_puts(" size=0x");
    print_hex_ulong(mem_size);
    uart_puts("\n");

    /* Step 4: Initialize the buddy allocator over the entire region.
     * Free lists are NOT built yet — we mark reservations first. */
    buddy_init(mem_base, mem_size);

    /* Step 5: Reserve all four categories of critical memory regions. */

    /* 5-a: DTB blob itself */
    uintptr_t dtb_size = dtb_get_totalsize();
    uart_puts("[DTB] Address: 0x");
    print_hex_ulong((uintptr_t)dtb_ptr);
    uart_puts(" Size: 0x");
    print_hex_ulong(dtb_size);
    uart_puts("\n");
    buddy_reserve((uintptr_t)dtb_ptr, (uintptr_t)dtb_ptr + dtb_size);

    /* 5-b: Kernel image (text + rodata + data + bss + stack) */
    uart_puts("[Kernel] Address: 0x");
    print_hex_ulong((uintptr_t)_kernel_start);
    uart_puts(" - 0x");
    print_hex_ulong((uintptr_t)_kernel_end);
    uart_puts("\n");
    buddy_reserve((uintptr_t)_kernel_start, (uintptr_t)_kernel_end);

    /* 5-c: Initramfs — present only when a bootloader passes it via /chosen */
    uintptr_t initrd_start = dtb_getprop("/chosen", "linux,initrd-start");
    uintptr_t initrd_end   = dtb_getprop("/chosen", "linux,initrd-end");
    if (initrd_start != 0 && initrd_end > initrd_start) {
        uart_puts("[Initrd] Address: 0x");
        print_hex_ulong(initrd_start);
        uart_puts(" - 0x");
        print_hex_ulong(initrd_end);
        uart_puts("\n");
        buddy_reserve(initrd_start, initrd_end);
    }

    /* 5-d: Platform-specific regions from /reserved-memory node */
    uart_puts("[DTB] Walking /reserved-memory regions...\n");
    dtb_walk_reserved_memory(reserve_dtb_region);

    /* Step 6: Build free lists — skips all RESERVED pages. */
    buddy_build_free_lists();

    /* Step 7: Initialize the dynamic memory allocator (chunk pools). */
    kmalloc_init();

    /* Step 8: Continue with the interactive shell. */
    shell();

    return 0;
}
