#ifndef __PLIC_H__
#define __PLIC_H__

#include "types.h"

/* -----------------------------------------------------------------------
 * RISC-V Platform-Level Interrupt Controller (PLIC) driver.
 *
 * Register map (base = plic_base):
 *   priority[N]          = base + 0x000000 + 4*N
 *   pending[N/32]        = base + 0x001000 + 4*(N/32)    (read-only)
 *   enable[ctx][N/32]    = base + 0x002000 + 0x80*ctx + 4*(N/32)
 *   threshold[ctx]       = base + 0x200000 + 0x1000*ctx
 *   claim_complete[ctx]  = base + 0x200004 + 0x1000*ctx
 *
 * A context is one (hart, privilege) pair. For OrangePi RV2 with hart0
 * running in S-mode the context number is 1 (interrupts-extended
 * interleaves M-ext and S-ext per hart, so hart0 M=0, hart0 S=1).
 * ----------------------------------------------------------------------- */

/* PLIC context numbering convention used by both OrangePi RV2 and QEMU
 * virt: each hart occupies two consecutive context slots — M-mode at the
 * even index, S-mode at the following odd one. Hart N's S-mode context
 * is therefore (2*N + 1). This matches the ordering of phandles in the
 * DTS "interrupts-extended" property of the PLIC node. */
#define PLIC_CTX_HART_S(hart_id)    (2U * (unsigned int)(hart_id) + 1U)

void plic_init(uintptr_t base, unsigned int irq, unsigned int ctx);
unsigned int plic_claim(void);
void plic_complete(unsigned int irq);

#endif /* __PLIC_H__ */
