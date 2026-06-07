#include "mm.h"   /* KERNEL_VA_OFFSET (== PAGE_OFFSET) */

.section ".text.boot"
.global _start

/*
 * Lab6 boot flow (ex61 order):
 *   setup_vm  -> high jump -> clear BSS -> drop_identity_map -> main
 *
 * On entry PC executes from the physical LMA (low address) because paging
 * is off. setup_vm() builds the identity + higher-half maps and turns on
 * Sv39; the identity map keeps low-address fetch valid across the satp
 * write. We then jump to the higher-half copy of the next instruction,
 * clear BSS (page tables are in .data, unaffected), tear down the identity
 * map, and tail-call main() — all now running in the higher half.
 */
_start:
    /* Save hart_id (a0) and DTB pointer (a1) – physical address from
     * OpenSBI – across the calls below. s0/s1 are callee-saved.        */
    mv s0, a0          /* hart_id  */
    mv s1, a1          /* dtb_ptr (PA) */

    /* Boot stack: PC is low here, so la resolves to a low physical
     * stack address, which is fine before/right after enabling paging. */
    la sp, _stack_top

    call setup_vm      /* build page tables, write satp, sfence.vma */

    /* ── Jump to the higher-half copy of 1f ──────────────────────────
     * la is PC-relative (medany); with PC still low it yields 1f's low
     * physical address. Add KERNEL_VA_OFFSET to get the higher-half VA,
     * then jr there. The higher-half map (just built) makes that fetch
     * succeed, and from here on PC stays in the higher half.            */
    la   t0, 1f
    li   t1, KERNEL_VA_OFFSET
    add  t0, t0, t1
    jr   t0

.align 2
1:
    /* Now running in the higher half. Reload sp so symbol resolution
     * gives the higher-half stack VA. */
    la sp, _stack_top

    /* Clear BSS (safe now: page tables live in .data). */
    la t0, _sbss
    la t1, _ebss
_init_bss:
    bge t0, t1, _bss_done
    sd zero, 0(t0)
    addi t0, t0, 8
    j _init_bss
_bss_done:
    call drop_identity_map   /* zero low PGD entries + sfence.vma */

    /* call main(hart_id, dtb_ptr) and loop forever if it returns */
    mv a0, s0          /* hart_id */
    mv a1, s1          /* dtb_ptr (PA – main converts via phys_to_virt) */
    call main
_hang:
    j _hang

.end /* label end of file */
