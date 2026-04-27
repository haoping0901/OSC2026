#include "plic.h"
#include "types.h"

#define PLIC_PRIORITY_OFF       0x000000UL
#define PLIC_ENABLE_OFF         0x002000UL
#define PLIC_ENABLE_STRIDE      0x80UL
#define PLIC_THRESHOLD_OFF      0x200000UL
#define PLIC_CONTEXT_STRIDE     0x1000UL
#define PLIC_CLAIM_OFF          0x200004UL

static uintptr_t g_plic_base;
static unsigned int g_plic_ctx;

static inline void mmio_w32(uintptr_t addr, unsigned int val)
{
    *((volatile unsigned int *)addr) = val;
}

static inline unsigned int mmio_r32(uintptr_t addr)
{
    return *((volatile unsigned int *)addr);
}

/** ----------------------------------------------------------------------
 * @brief plic_init() – Configure the PLIC for one IRQ source.
 *
 * Sets priority of source @irq to 1, enables it in the specified hart
 * context (hart0 S-mode = ctx 1 on OrangePi RV2), and drops the
 * context priority threshold to 0 so any priority >= 1 interrupt is
 * delivered.
 * @param base PLIC MMIO base address (e.g. 0xE0000000).
 * @param irq  Interrupt source number (matches DTS "interrupts" cell).
 * @param ctx  PLIC context index for the receiving (hart, privilege).
 * -------------------------------------------------------------------- */
void plic_init(uintptr_t base, unsigned int irq, unsigned int ctx)
{
    g_plic_base = base;
    g_plic_ctx  = ctx;

    /* Priority[irq] = 1 */
    mmio_w32(base + PLIC_PRIORITY_OFF + 4UL * irq, 1U);

    /* Enable bit for (ctx, irq) */
    uintptr_t enable_reg = base + PLIC_ENABLE_OFF
                         + PLIC_ENABLE_STRIDE * ctx
                         + 4UL * (irq / 32U);
    unsigned int bit = 1U << (irq % 32U);
    mmio_w32(enable_reg, mmio_r32(enable_reg) | bit);

    /* Threshold[ctx] = 0 */
    mmio_w32(base + PLIC_THRESHOLD_OFF + PLIC_CONTEXT_STRIDE * ctx, 0U);
}

/** ----------------------------------------------------------------------
 * @brief plic_claim() – Acknowledge the highest-priority pending IRQ.
 *
 * Reading the claim/complete register returns the IRQ id of the
 * highest priority pending source for this context and clears its
 * pending bit. Returns 0 when no interrupt is pending.
 * @return IRQ id of claimed interrupt, or 0 if none.
 * -------------------------------------------------------------------- */
unsigned int plic_claim(void)
{
    return mmio_r32(g_plic_base + PLIC_CLAIM_OFF
                    + PLIC_CONTEXT_STRIDE * g_plic_ctx);
}

/** ----------------------------------------------------------------------
 * @brief plic_complete() – Signal EOI for a previously claimed IRQ.
 *
 * Writes the IRQ id back to the claim/complete register, allowing the
 * PLIC to deliver further interrupts from the same source.
 * @param irq IRQ id previously returned by plic_claim().
 * -------------------------------------------------------------------- */
void plic_complete(unsigned int irq)
{
    mmio_w32(g_plic_base + PLIC_CLAIM_OFF
             + PLIC_CONTEXT_STRIDE * g_plic_ctx, irq);
}
