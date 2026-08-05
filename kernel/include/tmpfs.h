#ifndef __TMPFS_H__
#define __TMPFS_H__

#include "vfs.h"

/*
 * In-memory file system.
 *
 * Files live entirely in the heap and vanish on reboot. Directory
 * entries, name length, and file size are all bounded so a node needs
 * no dynamic growth logic.
 */

/** ----------------------------------------------------------------------
 * @brief tmpfs_get_fs() – Hand out the tmpfs file-system descriptor.
 *
 * The descriptor is statically allocated, so the pointer stays valid
 * for the lifetime of the kernel and can be registered directly.
 * @return Pointer to the tmpfs struct filesystem.
 * -------------------------------------------------------------------- */
struct filesystem *tmpfs_get_fs(void);

#endif /* __TMPFS_H__ */
