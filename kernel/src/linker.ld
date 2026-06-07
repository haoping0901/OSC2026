ENTRY(_start)

SECTIONS
{
    /*
     * Lab6: the kernel runs in the higher-half virtual window. Symbols are
     * linked at KERNEL_BASE (a high VMA), but the image is loaded at
     * KERNEL_LMA (the physical RAM offset QEMU jumps to). We set the
     * location counter to the VMA and use AT(addr) to fix each section's
     * load address (LMA), keeping the file image at physical addresses.
     *
     * Before paging is enabled, .text.boot executes from the LMA; it is
     * position-independent (-mcmodel=medany, PC-relative) so it runs
     * correctly at the low physical address until satp is written and the
     * high jump in start.S transfers control to the VMA copy.
     */
    . = KERNEL_BASE;
    _kernel_start = .;

    /* Output section */
    .text : AT(KERNEL_LMA) {
        /* input sections list */
        *(.text.boot)
        *(.text)
    }

    .rodata : {
        *(.rodata)
    }

    .data : {
        *(.data)
    }

    . = ALIGN(8);
    .bss : {
        *(.sbss)
        *(.bss)
    }

    /* symbols for startup code */
    _sbss = ADDR(.bss);
    _ebss = ADDR(.bss) + SIZEOF(.bss);

    . = ALIGN(16);
    _stack_top = . + 0x1000; /* reserve 4KB for stack */
    _kernel_end = _stack_top;
}
