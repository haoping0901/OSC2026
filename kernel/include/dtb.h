#ifndef DTH_H
#define DTH_H

#include "types.h"

void dtb_set_addr(void *addr);
void *dtb_get_addr(void);

/*
 * dtb_getprop - convenience wrapper: returns the first 32- or 64-bit value
 * of a property (big-endian byte-swapped).  Returns 0 on failure.
 */
uintptr_t dtb_getprop(const char *path, const char *prop_name);

/*
 * _dtb_getprop - low-level property lookup.
 * Returns a pointer to the raw (big-endian) property data, or NULL on failure.
 * *lenp is set to the byte length of the property data.
 */
const void *_dtb_getprop(const char *node_path, const char *prop_name,
                         int *lenp);

/* Return the value of fdt_header.totalsize (byte-swapped to host order). */
uintptr_t dtb_get_totalsize(void);

/*
 * dtb_get_memory_region - parse the first DTB node with device_type="memory".
 * Fills *base and *size with the physical address and byte length.
 * Returns 0 on success, -1 if no memory node is found.
 */
int dtb_get_memory_region(uintptr_t *base, uintptr_t *size);

/*
 * dtb_walk_reserved_memory - iterate over every direct child of the
 * /reserved-memory node that has a "reg" property.
 * For each such child, cb(base, size) is called with host-order values.
 * Does nothing if the node does not exist (e.g. QEMU virt DTB).
 */
void dtb_walk_reserved_memory(void (*cb)(uintptr_t base, uintptr_t size));

#endif
