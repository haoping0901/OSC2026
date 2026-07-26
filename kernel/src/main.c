#include "uart.h"
#include "shell.h"
#include "dtb.h"
#include "types.h"
#include "buddy.h"
#include "kmalloc.h"
#include "trap.h"
#include "timer.h"
#include "plic.h"
#include "riscv.h"
#include "sched.h"
#include "utils.h"
#include "video.h"
#include "mm.h"

/* Kernel image boundaries exported by the linker script. */
extern char _kernel_start[];
extern char _kernel_end[];

/*
 * Adapter for dtb_walk_reserved_memory(): it provides (base, size) but
 * buddy_startup_reserve() takes (start, end).
 */
static void reserve_startup_region(uintptr_t base, uintptr_t size)
{
    buddy_startup_reserve(base, base + size);
}

int main(unsigned long hart_id, void *dtb_ptr)
{
    /* start.S hands us physical addresses (DTB pointer in a1, device
     * bases resolved from the DTB are PAs). Paging is on and the kernel runs
     * in the higher half, so every PA we dereference or feed to a driver is
     * routed through phys_to_virt() here — keeping the PA->VA boundary in
     * main.c. Buddy bookkeeping intentionally stays in PA (see buddy.c). */
    uintptr_t dtb_pa = (uintptr_t)dtb_ptr;

    /* Step 1: Register the DTB address (as a VA) so the parser can read it. */
    dtb_set_addr(phys_to_virt(dtb_pa));

    /* Step 2: Resolve UART base address from the devicetree.
     *
     *  OrangePi RV2 : /soc/serial
     *  QEMU virt    : /soc/serial  (node name "serial@10000000")
     *
     * Both boards use the same logical path; node_name_match() in dtb.c
     * handles the "@<unit-addr>" suffix transparently.
     */
    uintptr_t u_base_pa = dtb_getprop("/soc/serial", "reg");
    uart_set_base((unsigned long)phys_to_virt(u_base_pa));

    uart_puts("Welcome to OPI-RV2!\n");
    uart_puts("Uart base address (PA): 0x");
    print_hex_ulong(u_base_pa);
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

    /* Step 4: Initialize the startup (bump) allocator.  It lives inside
     * buddy.c and allocates metadata before the buddy system is ready. */
    buddy_startup_init(mem_base, mem_size);

    /* Step 5: Register all reserved regions with the startup allocator so
     * that buddy_startup_alloc() will not place metadata on top of them. */

    /* 5-a: DTB blob */
    uintptr_t dtb_size = dtb_get_totalsize();
    uart_puts("[DTB] Address (PA): 0x");
    print_hex_ulong(dtb_pa);
    uart_puts(" Size: 0x");
    print_hex_ulong(dtb_size);
    uart_puts("\n");
    /* DTB pointer is already a PA; buddy reserves in PA. */
    buddy_startup_reserve(dtb_pa, dtb_pa + dtb_size);

    /* 5-b: Kernel image (text + rodata + data + bss + stack).
     * _kernel_start/_kernel_end are higher-half VAs (linker VMA); convert
     * back to PA so the reserve lands on the right physical frames. */
    uintptr_t kern_start_pa = virt_to_phys(_kernel_start);
    uintptr_t kern_end_pa   = virt_to_phys(_kernel_end);
    uart_puts("[Kernel] Address (PA): 0x");
    print_hex_ulong(kern_start_pa);
    uart_puts(" - 0x");
    print_hex_ulong(kern_end_pa);
    uart_puts("\n");
    buddy_startup_reserve(kern_start_pa, kern_end_pa);

    /* 5-c: Initramfs — present only when a bootloader passes it via /chosen */
    uintptr_t initrd_start = dtb_getprop("/chosen", "linux,initrd-start");
    uintptr_t initrd_end   = dtb_getprop("/chosen", "linux,initrd-end");
    if (initrd_start != 0 && initrd_end > initrd_start) {
        uart_puts("[Initrd] Address: 0x");
        print_hex_ulong(initrd_start);
        uart_puts(" - 0x");
        print_hex_ulong(initrd_end);
        uart_puts("\n");
        buddy_startup_reserve(initrd_start, initrd_end);
    }

    /* 5-d: Platform-specific regions from /reserved-memory node */
    uart_puts("[DTB] Walking /reserved-memory regions...\n");
    dtb_walk_reserved_memory(reserve_startup_region);

    /* Step 6: Allocate metadata arrays via the startup allocator. */
    unsigned long total_pages    = mem_size / PAGE_SIZE;
    unsigned long fa_bytes       = total_pages * sizeof(int);
    unsigned long ra_bytes       = total_pages * sizeof(unsigned int);
    unsigned long ppi_bytes      = total_pages * sizeof(signed char);

    int          *frame_array   = (int *)buddy_startup_alloc(fa_bytes);
    unsigned int *ref_array     = (unsigned int *)buddy_startup_alloc(ra_bytes);
    signed char  *page_pool_idx = (signed char *)buddy_startup_alloc(ppi_bytes);

    /* Step 7: Initialize the buddy allocator with the dynamically
     * allocated frame array and per-frame reference-count array.
     * Free lists are NOT built yet. */
    buddy_init(mem_base, mem_size, frame_array, ref_array, total_pages);

    /* Step 8: Replay all regions that were registered with the startup
     * allocator (reserved regions + metadata allocations) into the buddy
     * system in one pass, avoiding a second traversal of the DTB. */
    buddy_startup_replay_reserves();

    /* Step 9: Build free lists — skips all RESERVED pages. */
    buddy_build_free_lists();

    /* Step 10: Initialize the dynamic memory allocator (chunk pools). */
    kmalloc_init(page_pool_idx, total_pages);

    /* Step 11: Install the S-mode trap vector before any user program
     * can execute ecall or hit an exception. */
    trap_init();
    uart_puts("[Trap] stvec installed.\n");

    /* Step 12: Configure UART0 + PLIC for interrupt-driven I/O (Ex3).
     * Must run BEFORE timer_init(): once sstatus.SIE is on, timer IRQs
     * call uart_puts() from the handler; routing that through the ring
     * buffer avoids re-entering the LSR.TDRQ busy-wait while the main
     * thread is also writing.                                          */
    uart_init();
    uart_puts("[UART] RX/TX IRQ configured.\n");

    /* PLIC node name differs across platforms: QEMU virt exposes it as
     * "plic@..." while the OrangePi RV2 DTS labels it
     * "interrupt-controller@...". Both still live under /soc. */
#ifdef QEMU
    uintptr_t plic_base_pa = dtb_getprop("/soc/plic", "reg");
#else
    uintptr_t plic_base_pa = dtb_getprop("/soc/interrupt-controller",
                                     "reg");
#endif /* QEMU */

    /* PLIC base from DTB is a PA; drive the controller via its VA. */
    uintptr_t plic_base = (uintptr_t)phys_to_virt(plic_base_pa);
    unsigned int uart_irq = (unsigned int)dtb_getprop("/soc/serial",
                                                     "interrupts");

    unsigned int plic_ctx = PLIC_CTX_HART_S(hart_id);
    plic_init(plic_base, uart_irq, plic_ctx);
    trap_set_uart_irq(uart_irq);

    /* Enable S-mode external interrupts. sstatus.SIE is still off at
     * this point (timer_init has not run yet); it will be turned on
     * by timer_init() below. */
    asm volatile ("csrs sie, %0" :: "r"((unsigned long)SIE_SEIE));
    uart_enable_irq_mode();

    uart_puts("[PLIC] UART0 IRQ ");
    print_dec_ulong(uart_irq);
    uart_puts(" enabled (ctx ");
    print_dec_ulong(plic_ctx);
    uart_puts(").\n");

    /* Step 13: Enable the core timer interrupt (Ex2). */
    timer_init();
    uart_puts("[Timer] Core timer interrupt enabled.\n");

    /* Step 13.5: Register the QEMU ramfb framebuffer.
     * Done after timer_init() so all earlier subsystems are live; if
     * fw_cfg/ramfb is unavailable we just print a warning instead of
     * panicking — boards without ramfb should keep booting normally. */
    video_init();
    uart_puts("[Video] ramfb registered at PA 0x");
    print_hex_ulong((unsigned long)FB_BASE);
    uart_puts(".\n");

    /* Step 14: Adopt the boot context as the bootstrap thread and
     * spawn the idle thread. After this point any code running on the
     * main path is "the bootstrap thread"; any future
     * thread_create()/schedule() call composes correctly. */
    sched_init();
    uart_puts("[Sched] bootstrap + idle thread ready.\n");

    /* Step 15: Continue with the interactive shell. */
    shell();

    return 0;
}
