#ifndef DTH_H
#define DTH_H

#include "types.h"

void dtb_set_addr(void *addr);
void *dtb_get_addr(void);
uintptr_t dtb_getprop(const char *path, const char *prop_name);

#endif