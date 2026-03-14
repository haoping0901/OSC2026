.section ".text.boot"
.global _start

_start:
    /* initialize stack pointer */
    la t0, _sbss
    la t1, _ebss

_init_bss:  /* clear the bss section */
    bge t0, t1, _c_prog
    sd zero, 0(t0)
    addi t0, t0, 8
    j _init_bss

_c_prog:  /* call main and loop forever if it returns */
    la sp, _stack_top
    call main
    j _c_prog

.end /* label end of file */
