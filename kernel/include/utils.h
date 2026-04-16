#ifndef __UTILS_H__
#define __UTILS_H__

int str_eq(const char *a, const char *b);
int str_startswith(const char *str, const char *prefix);
void print_hex_ulong(unsigned long x);
void print_hex_u32(unsigned int x);
void print_dec_ulong(unsigned long x);

/** ----------------------------------------------------------------------
 * @brief mem_cpy() – Byte-wise memory copy (freestanding replacement).
 *
 * The kernel is built with -nostdlib so a minimal copy helper lives in
 * utils instead of libc. Does not handle overlapping regions.
 * @param dst Destination buffer (must be writable).
 * @param src Source bytes.
 * @param n   Number of bytes to copy.
 * @return Original @dst pointer.
 * -------------------------------------------------------------------- */
void *mem_cpy(void *dst, const void *src, unsigned long n);

#endif // __UTILS_H__