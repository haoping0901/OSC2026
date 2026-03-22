#ifndef __CPIO_H__
#define __CPIO_H__

void cpio_ls(const void *archive);
int cpio_cat(const void *archive, const char *filename);

#endif // __CPIO_H__
