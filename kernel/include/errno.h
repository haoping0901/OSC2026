#ifndef __ERRNO_H__
#define __ERRNO_H__

/*
 * Error numbers.
 *
 * Values match the Linux/POSIX assignment so a user-space caller can
 * eventually interpret them with the conventional names. Kernel code
 * returns the NEGATED value (-ENOENT, ...) so a single sign test
 * separates an error from a valid non-negative result such as a file
 * descriptor or a byte count.
 */

#define EPERM       1   /* Operation not permitted */
#define ENOENT      2   /* No such file or directory */
#define EIO         5   /* I/O error */
#define EBADF       9   /* Bad file descriptor */
#define ENOMEM      12  /* Out of memory */
#define EFAULT      14  /* Bad address (unreadable user pointer) */
#define EBUSY       16  /* Resource busy (target already mounted) */
#define EEXIST      17  /* File exists */
#define ENODEV      18  /* No such device (unknown file system) */
#define ENOTDIR     20  /* Not a directory */
#define EISDIR      21  /* Is a directory */
#define EINVAL      22  /* Invalid argument */
#define EMFILE      24  /* Too many open files (fd table full) */
#define ENOSPC      28  /* No space left on device */
#define ENAMETOOLONG 36 /* Filename too long */
#define ENOSYS      38  /* Function not implemented */

#endif /* __ERRNO_H__ */
