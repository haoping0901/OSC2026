#include "vfs.h"
#include "tmpfs.h"
#include "kmalloc.h"
#include "uart.h"
#include "utils.h"
#include "types.h"

/* Registered file systems. Entries are never removed, so g_fs_count
 * doubles as the high-water mark of the table. */
static struct filesystem *g_fs_list[VFS_MAX_FS];
static int g_fs_count;

struct mount *g_rootfs;

/** ----------------------------------------------------------------------
 * @brief register_filesystem() – Add @fs to the known file-system table.
 *
 * Scans for an already-registered file system of the same name first so
 * repeated calls are harmless.
 * @param fs File system to register.
 * @return 0 on success, -1 on bad argument or full table.
 * -------------------------------------------------------------------- */
int register_filesystem(struct filesystem *fs)
{
    if (!fs || !fs->name || !fs->setup_mount)
        return -1;

    for (int i = 0; i < g_fs_count; i++) {
        if (str_eq(g_fs_list[i]->name, fs->name))
            return 0;
    }

    if (g_fs_count >= VFS_MAX_FS)
        return -1;

    g_fs_list[g_fs_count++] = fs;
    return 0;
}    /* register_filesystem */

/** ----------------------------------------------------------------------
 * @brief vfs_resolve() – Split @pathname into parent directory and leaf.
 *
 * Walks every component but the last from the root vnode, so the caller
 * receives the directory that will own the final name together with the
 * name itself. Returning the parent (rather than the target) is what
 * lets vfs_open() serve the found and the O_CREAT paths from a single
 * traversal: creating a missing file requires its parent directory.
 *
 * The leaf is copied into @leaf because a path component is not
 * NUL-terminated inside @pathname; the file system needs a standalone
 * string to compare against.
 * @param pathname Absolute path beginning with '/'.
 * @param parent   Out: directory holding the final component.
 * @param leaf     Out: buffer receiving the final component.
 * @param leaf_sz  Size of @leaf in bytes, NUL included.
 * @return 0 on success, -1 on a malformed path, a missing intermediate
 *         directory, or a component that does not fit @leaf.
 * -------------------------------------------------------------------- */
static int vfs_resolve(const char *pathname, struct vnode **parent,
                       char *leaf, size_t leaf_sz)
{
    if (!g_rootfs || !g_rootfs->root)
        return -1;
    if (!pathname || pathname[0] != '/')
        return -1;

    size_t path_len = 0;
    while (pathname[path_len] != '\0') {
        if (++path_len > VFS_MAX_PATHNAME)
            return -1;
    }

    struct vnode *dir = g_rootfs->root;
    const char *cur = pathname + 1;      /* skip the leading '/' */

    /* A trailing '/' would make the leaf empty, and "/" alone names the
     * root itself rather than an entry inside a directory. Neither is a
     * valid target for open/create. */
    if (*cur == '\0')
        return -1;

    while (1) {
        /* Measure the component up to the next separator. */
        size_t len = 0;
        while (cur[len] != '/' && cur[len] != '\0')
            len++;

        if (len == 0 || len >= leaf_sz)
            return -1;

        /* No separator left: this component is the leaf. */
        if (cur[len] == '\0') {
            for (size_t i = 0; i < len; i++)
                leaf[i] = cur[i];
            leaf[len] = '\0';
            *parent = dir;
            return 0;
        }

        /* Intermediate component: it must already exist and be usable
         * as a directory for the next round. */
        char comp[VFS_MAX_PATHNAME + 1];
        for (size_t i = 0; i < len; i++)
            comp[i] = cur[i];
        comp[len] = '\0';

        struct vnode *next = NULL;
        if (!dir->v_ops || !dir->v_ops->lookup)
            return -1;
        if (dir->v_ops->lookup(dir, &next, comp) != 0 || !next)
            return -1;

        dir = next;
        cur += len + 1;

        /* Path ended with '/', leaving no leaf to act on. */
        if (*cur == '\0')
            return -1;
    }
}    /* vfs_resolve */

/** ----------------------------------------------------------------------
 * @brief vfs_open() – Open @pathname, creating it when O_CREAT is set.
 *
 * Fills f_pos and flags here instead of in the file system: their
 * meaning is identical for every fs, so centralising them keeps each
 * backend from repeating (and forgetting) the initialisation.
 * @param pathname Absolute path beginning with '/'.
 * @param flags    O_CREAT to create a missing file.
 * @param target   Out: handle for the opened file.
 * @return 0 on success, -1 otherwise.
 * -------------------------------------------------------------------- */
int vfs_open(const char *pathname, int flags, struct file **target)
{
    if (!target)
        return -1;

    struct vnode *dir = NULL;
    char leaf[VFS_MAX_PATHNAME + 1];
    if (vfs_resolve(pathname, &dir, leaf, sizeof(leaf)) != 0)
        return -1;
    if (!dir->v_ops || !dir->v_ops->lookup)
        return -1;

    struct vnode *node = NULL;
    if (dir->v_ops->lookup(dir, &node, leaf) != 0) {
        /* A failed lookup is ordinary control flow, not an error to
         * report: it is exactly how O_CREAT decides to create. */
        if (!(flags & O_CREAT))
            return -1;
        if (!dir->v_ops->create)
            return -1;
        if (dir->v_ops->create(dir, &node, leaf) != 0 || !node)
            return -1;
    }

    if (!node->f_ops || !node->f_ops->open)
        return -1;

    struct file *file = NULL;
    if (node->f_ops->open(node, &file) != 0 || !file)
        return -1;

    file->vnode = node;
    file->f_ops = node->f_ops;
    file->f_pos = 0;
    file->flags = flags;

    *target = file;
    return 0;
}    /* vfs_open */

/** ----------------------------------------------------------------------
 * @brief vfs_close() – Release an opened file handle.
 *
 * @param file Handle previously returned by vfs_open().
 * @return 0 on success, -1 on bad argument.
 * -------------------------------------------------------------------- */
int vfs_close(struct file *file)
{
    if (!file || !file->f_ops || !file->f_ops->close)
        return -1;
    return file->f_ops->close(file);
}    /* vfs_close */

/** ----------------------------------------------------------------------
 * @brief vfs_write() – Write @len bytes from @buf at the file position.
 *
 * @param file Handle previously returned by vfs_open().
 * @param buf  Source bytes.
 * @param len  Number of bytes requested.
 * @return Bytes written, or -1 on bad argument.
 * -------------------------------------------------------------------- */
int vfs_write(struct file *file, const void *buf, size_t len)
{
    if (!file || !buf || !file->f_ops || !file->f_ops->write)
        return -1;
    return file->f_ops->write(file, buf, len);
}    /* vfs_write */

/** ----------------------------------------------------------------------
 * @brief vfs_read() – Read up to @len bytes into @buf.
 *
 * @param file Handle previously returned by vfs_open().
 * @param buf  Destination buffer.
 * @param len  Number of bytes requested.
 * @return Bytes read (0 at EOF), or -1 on bad argument.
 * -------------------------------------------------------------------- */
int vfs_read(struct file *file, void *buf, size_t len)
{
    if (!file || !buf || !file->f_ops || !file->f_ops->read)
        return -1;
    return file->f_ops->read(file, buf, len);
}    /* vfs_read */

/** ----------------------------------------------------------------------
 * @brief vfs_mkdir() – Create the directory named by @pathname.
 *
 * Placeholder: directory creation is not supported yet.
 * @param pathname Absolute path of the directory to create.
 * @return -1 always.
 * -------------------------------------------------------------------- */
int vfs_mkdir(const char *pathname)
{
    (void)pathname;
    return -1;
}    /* vfs_mkdir */

/** ----------------------------------------------------------------------
 * @brief vfs_mount() – Mount @filesystem onto the directory @target.
 *
 * Placeholder: only the root mount exists so far.
 * @param target     Absolute path of an existing directory.
 * @param filesystem Registered file-system name.
 * @return -1 always.
 * -------------------------------------------------------------------- */
int vfs_mount(const char *target, const char *filesystem)
{
    (void)target;
    (void)filesystem;
    return -1;
}    /* vfs_mount */

/** ----------------------------------------------------------------------
 * @brief vfs_lookup() – Resolve @pathname to its vnode.
 *
 * Placeholder: vfs_open() resolves paths internally for now.
 * @param pathname Absolute path to resolve.
 * @param target   Out: vnode the path names.
 * @return -1 always.
 * -------------------------------------------------------------------- */
int vfs_lookup(const char *pathname, struct vnode **target)
{
    (void)pathname;
    (void)target;
    return -1;
}    /* vfs_lookup */

/** ----------------------------------------------------------------------
 * @brief vfs_init() – Register tmpfs and mount it as the root fs.
 *
 * g_rootfs stays NULL on any failure so a later vfs_open() reports an
 * error instead of walking a partially built tree.
 * -------------------------------------------------------------------- */
void vfs_init(void)
{
    struct filesystem *tmpfs = tmpfs_get_fs();

    if (register_filesystem(tmpfs) != 0) {
        uart_puts("[VFS] failed to register tmpfs.\n");
        return;
    }

    struct mount *mnt = (struct mount *)kmalloc(sizeof(struct mount));
    if (!mnt) {
        uart_puts("[VFS] out of memory building root mount.\n");
        return;
    }

    mnt->fs = tmpfs;
    mnt->root = NULL;
    if (tmpfs->setup_mount(tmpfs, mnt) != 0) {
        uart_puts("[VFS] tmpfs mount setup failed.\n");
        kfree(mnt);
        return;
    }

    g_rootfs = mnt;
}    /* vfs_init */
