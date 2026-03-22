.section ".text.boot"
.global _start

_start:
    /* Save hart_id (a0) and DTB pointer (a1) across BSS clear.
     * a0/a1 are caller-saved, but s0/s1 are callee-saved and safe
     * to use here since we own the stack setup.                    */
    mv s0, a0          /* hart_id  */
    mv s1, a1          /* dtb_ptr  – passed by OpenSBI/U-Boot in a1 */

    /* Initialize stack pointer before anything else */
    la t0, _sbss
    la t1, _ebss

_init_bss:  /* clear the bss section */
    bge t0, t1, _c_prog
    sd zero, 0(t0)
    addi t0, t0, 8
    j _init_bss

_c_prog:  /* call main(hart_id, dtb_ptr) and loop forever if it returns */
    la sp, _stack_top
    mv a0, s0          /* hart_id */
    mv a1, s1          /* dtb_ptr */
    call main
    j _c_prog

.end /* label end of file */
