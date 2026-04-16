#ifndef __CPIO_H__
#define __CPIO_H__

void cpio_ls(const void *archive);
int cpio_cat(const void *archive, const char *filename);

/** ----------------------------------------------------------------------
 * @brief cpio_find() – Locate a file inside an SVR4 newc cpio archive.
 *
 * Walks the archive until TRAILER!!! is reached, comparing each entry
 * name with @target. On match, returns the payload pointer and size via
 * the out-parameters; on failure, leaves them untouched.
 * @param archive Base of the cpio blob (as passed by the bootloader).
 * @param target  NUL-terminated filename to look up.
 * @param data    Out: pointer to the file payload inside the archive.
 * @param size    Out: payload size in bytes.
 * @return 0 on success, -1 if not found or archive is invalid.
 * -------------------------------------------------------------------- */
int cpio_find(const void *archive, const char *target,
              const void **data, unsigned long *size);

#endif // __CPIO_H__
