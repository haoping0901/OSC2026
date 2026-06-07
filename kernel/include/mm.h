#ifndef __MM_H__
#define __MM_H__

/*
 * Higher-half linear-map offset.
 *
 * After setup_vm() the whole accessible physical address space is mapped
 * linearly at this fixed offset, so every physical address PA has a
 * canonical kernel virtual address VA = PA + KERNEL_VA_OFFSET. The kernel
 * is linked and runs entirely in this higher-half window.
 *
 * This value MUST stay in sync with:
 *   - the linker VMA base in linker.ld (KERNEL_BASE)
 *   - PAGE_OFFSET used by start.S for the high jump
 */
#define KERNEL_VA_OFFSET   0xffffffc000000000UL

#ifndef __ASSEMBLER__

/**
 * phys_to_virt() - map a physical address to its linear kernel VA.
 *
 * Valid only for addresses covered by the linear map established in
 * setup_vm() (all RAM banks plus the MMIO apertures within [0, LINEAR_MAP)).
 */
static inline void *phys_to_virt(unsigned long pa)
{
    return (void *)(pa + KERNEL_VA_OFFSET);
}

/**
 * virt_to_phys() - map a linear kernel VA back to its physical address.
 *
 * Inverse of phys_to_virt(); valid only for higher-half linear-map VAs.
 */
static inline unsigned long virt_to_phys(void *va)
{
    return (unsigned long)va - KERNEL_VA_OFFSET;
}

#endif /* __ASSEMBLER__ */

#endif /* __MM_H__ */
