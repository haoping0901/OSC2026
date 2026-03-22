ENTRY(_start)

SECTIONS
{
    . = KERNEL_BASE;

    /* Output section */
    .text : {
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
}
