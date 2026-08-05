#ifndef __VFS_H__
#define __VFS_H__

#include "types.h"

/*
 * Virtual File System.
 *
 * A thin dispatch layer that lets the kernel talk to any file system
 * through two operation tables: vnode_operations for name-space work
 * (lookup/create/mkdir on a directory) and file_operations for data
 * work (open/close/read/write/lseek on an opened file).
 *
 * The generic layer never inspects file-system private state; each fs
 * hangs its own bookkeeping off vnode->internal.
 */

/* Longest absolute path the resolver accepts, NUL excluded. */
#define VFS_MAX_PATHNAME    255

/* Registered file-system slots. Grown only when a new fs is added. */
#define VFS_MAX_FS          8

/*
 * open() flags. The value matches the conventional POSIX bit so a
 * user-space caller can eventually pass it straight through.
 */
#define O_CREAT             00000100

/* lseek64() whence values. */
#define SEEK_SET            0

struct mount;
struct filesystem;
struct vnode_operations;
struct file_operations;

struct vnode {
    struct mount *mount;
    struct vnode_operations *v_ops;
    struct file_operations *f_ops;
    void *internal;
};

struct file {
    struct vnode *vnode;
    size_t f_pos;
    struct file_operations *f_ops;
    int flags;
};

struct mount {
    struct vnode *root;
    struct filesystem *fs;
};

struct filesystem {
    const char *name;
    int (*setup_mount)(struct filesystem *fs, struct mount *mount);
};

struct file_operations {
    int (*open)(struct vnode *file_node, struct file **target);
    int (*close)(struct file *file);
    int (*read)(struct file *file, void *buf, size_t len);
    int (*write)(struct file *file, const void *buf, size_t len);
    long (*lseek64)(struct file *file, long offset, int whence);
};

struct vnode_operations {
    int (*lookup)(struct vnode *dir_node, struct vnode **target,
                  const char *component_name);
    int (*create)(struct vnode *dir_node, struct vnode **target,
                  const char *component_name);
    int (*mkdir)(struct vnode *dir_node, struct vnode **target,
                 const char *component_name);
};

/* The mount sitting at "/". Published so a file system can compare a
 * vnode against the root when resolving a path. */
extern struct mount *g_rootfs;

/** ----------------------------------------------------------------------
 * @brief register_filesystem() – Add @fs to the known file-system table.
 *
 * Registration is idempotent: re-registering a name that is already
 * present succeeds without adding a duplicate slot, so a caller need
 * not track whether it has run before.
 * @param fs File system to register; must outlive the kernel (statically
 *           allocated by the fs implementation).
 * @return 0 on success, -1 on bad argument or when the table is full.
 * -------------------------------------------------------------------- */
int register_filesystem(struct filesystem *fs);

/** ----------------------------------------------------------------------
 * @brief vfs_open() – Open @pathname, optionally creating the file.
 *
 * Resolves @pathname to its parent directory, asks that directory to
 * look the final component up, and creates the file through the parent
 * when it is missing and O_CREAT is set. The generic layer owns f_pos
 * and flags in the returned handle; the file system only allocates the
 * struct file and binds its vnode.
 * @param pathname Absolute path beginning with '/'.
 * @param flags    O_CREAT to create the file when it does not exist.
 * @param target   Out: handle for the opened file.
 * @return 0 on success, -1 on bad path, missing file, or allocation
 *         failure.
 * -------------------------------------------------------------------- */
int vfs_open(const char *pathname, int flags, struct file **target);

/** ----------------------------------------------------------------------
 * @brief vfs_close() – Release an opened file handle.
 *
 * Forwards to the file system, which frees the handle. The underlying
 * vnode stays alive because it remains linked into the directory tree.
 * @param file Handle previously returned by vfs_open().
 * @return 0 on success, -1 on bad argument.
 * -------------------------------------------------------------------- */
int vfs_close(struct file *file);

/** ----------------------------------------------------------------------
 * @brief vfs_write() – Write @len bytes from @buf at the file position.
 *
 * The file system advances file->f_pos, since only it knows the size
 * limits that may truncate a short write.
 * @param file Handle previously returned by vfs_open().
 * @param buf  Source bytes.
 * @param len  Number of bytes requested.
 * @return Bytes actually written (may be short), or -1 on bad argument.
 * -------------------------------------------------------------------- */
int vfs_write(struct file *file, const void *buf, size_t len);

/** ----------------------------------------------------------------------
 * @brief vfs_read() – Read up to @len bytes into @buf at the position.
 *
 * A read that starts at or past end-of-file yields 0.
 * @param file Handle previously returned by vfs_open().
 * @param buf  Destination buffer of at least @len bytes.
 * @param len  Number of bytes requested.
 * @return Bytes actually read (0 at EOF), or -1 on bad argument.
 * -------------------------------------------------------------------- */
int vfs_read(struct file *file, void *buf, size_t len);

/** ----------------------------------------------------------------------
 * @brief vfs_mkdir() – Create the directory named by @pathname.
 *
 * Not implemented yet; multi-level path handling arrives with the
 * directory support work.
 * @param pathname Absolute path of the directory to create.
 * @return -1 always.
 * -------------------------------------------------------------------- */
int vfs_mkdir(const char *pathname);

/** ----------------------------------------------------------------------
 * @brief vfs_mount() – Mount @filesystem onto the directory @target.
 *
 * Not implemented yet; only the root mount built by vfs_init() exists.
 * @param target     Absolute path of an existing directory.
 * @param filesystem Registered file-system name.
 * @return -1 always.
 * -------------------------------------------------------------------- */
int vfs_mount(const char *target, const char *filesystem);

/** ----------------------------------------------------------------------
 * @brief vfs_lookup() – Resolve @pathname to its vnode.
 *
 * Not implemented yet; vfs_open() currently performs its own resolution.
 * @param pathname Absolute path to resolve.
 * @param target   Out: vnode the path names.
 * @return -1 always.
 * -------------------------------------------------------------------- */
int vfs_lookup(const char *pathname, struct vnode **target);

/** ----------------------------------------------------------------------
 * @brief vfs_init() – Register tmpfs and mount it as the root fs.
 *
 * Must run after the dynamic allocator is up, because both the mount
 * and the root vnode are heap allocated. Leaves g_rootfs NULL if the
 * root mount cannot be built, which makes every later vfs_open() fail
 * rather than dereference a half-built tree.
 * -------------------------------------------------------------------- */
void vfs_init(void);

#endif /* __VFS_H__ */
