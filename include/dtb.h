#ifndef DTH_H
#define DTH_H

#include "types.h"

void dtb_set_addr(void *addr);
void *dtb_get_addr(void);
uintptr_t dtb_get_reg(const char *path);
const void *dtb_getprop(const char *node_path, const char *prop_name,
                       int *lenp);

#endif