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

static struct filesystem *find_filesystem(const char *name);

/** ----------------------------------------------------------------------
 * @brief register_filesystem() – Add @fs to the known file-system table.
 *
 * Scans for an already-registered file system of the same name first so
 * repeated calls are harmless.
 * @param[in] fs File system to register.
 * @return 0 on success, -1 on bad argument or full table.
 * -------------------------------------------------------------------- */
int register_filesystem(struct filesystem *fs)
{
    if (!fs || !fs->name || !fs->setup_mount)
        return -1;

    if (find_filesystem(fs->name))
        return 0;

    if (g_fs_count >= VFS_MAX_FS)
        return -1;

    g_fs_list[g_fs_count++] = fs;
    return 0;
}    /* register_filesystem */

/** ----------------------------------------------------------------------
 * @brief find_filesystem() – Look @name up in the registration table.
 *
 * @param[in] name File-system name to match exactly.
 * @return The registered file system, or NULL when @name is unknown.
 * -------------------------------------------------------------------- */
static struct filesystem *find_filesystem(const char *name)
{
    if (!name)
        return NULL;

    for (int i = 0; i < g_fs_count; i++) {
        if (str_eq(g_fs_list[i]->name, name))
            return g_fs_list[i];
    }
    return NULL;
}    /* find_filesystem */

/** ----------------------------------------------------------------------
 * @brief vfs_follow_mount() – Step into whatever is mounted on @node.
 *
 * Every place a traversal obtains a vnode funnels through here, which
 * is what keeps "a lookup crosses mount points" a single rule rather
 * than a condition repeated at each call site.
 *
 * Loops instead of testing once because a file system may be mounted
 * onto the root vnode of another mount, stacking two covers on what is
 * reached by one name.
 * @param[in] node Vnode a traversal just reached; may be NULL.
 * @return The vnode the name really denotes, after crossing any mounts.
 * -------------------------------------------------------------------- */
static struct vnode *vfs_follow_mount(struct vnode *node)
{
    while (node && node->mounted && node->mounted->root)
        node = node->mounted->root;
    return node;
}    /* vfs_follow_mount */

/** ----------------------------------------------------------------------
 * @brief vfs_walk() – Resolve @pathname, crossing every mount point.
 *
 * Serves the two shapes of traversal the VFS needs from one body, so
 * the component splitting and the mount crossing exist in a single
 * place. With @stop_at_parent the walk halts one component early and
 * hands back that component's name: creating an entry needs the
 * directory that will own it, and the name is not NUL-terminated inside
 * @pathname so it must be copied out. Without it the walk resolves the
 * whole path.
 * @param[in]  pathname       Absolute path beginning with '/'.
 * @param      stop_at_parent Non-zero to stop at the final component's
 *                            parent and report the component in @leaf.
 * @param[out] result         The resolved vnode, or the parent directory
 *                            when @stop_at_parent is set.
 * @param[out] leaf           Buffer receiving the final component; used
 *                            only when @stop_at_parent is set.
 * @param      leaf_sz        Size of @leaf in bytes, NUL included.
 * @return 0 on success, -1 on a malformed path, a missing component, or
 *         a component that does not fit @leaf.
 * -------------------------------------------------------------------- */
static int vfs_walk(const char *pathname, int stop_at_parent,
                    struct vnode **result, char *leaf, size_t leaf_sz)
{
    if (!g_rootfs || !g_rootfs->root || !result)
        return -1;
    if (!pathname || pathname[0] != '/')
        return -1;
    if (stop_at_parent && (!leaf || leaf_sz == 0))
        return -1;

    size_t path_len = 0;
    while (pathname[path_len] != '\0') {
        if (++path_len > VFS_MAX_PATHNAME)
            return -1;
    }

    /* Even the starting point may be covered by a mount. */
    struct vnode *dir = vfs_follow_mount(g_rootfs->root);
    const char *cur = pathname + 1;      /* skip the leading '/' */

    /* "/" names the root itself: a valid thing to resolve, but not a
     * valid entry inside a directory to create or open. */
    if (*cur == '\0') {
        if (stop_at_parent)
            return -1;
        *result = dir;
        return 0;
    }

    while (1) {
        /* Measure the component up to the next separator. */
        size_t len = 0;
        while (cur[len] != '/' && cur[len] != '\0')
            len++;

        /* An empty component means "//" or a trailing '/'; neither
         * names anything to act on. */
        if (len == 0 || len > VFS_MAX_PATHNAME)
            return -1;

        int is_last = (cur[len] == '\0');

        if (is_last && stop_at_parent) {
            if (len >= leaf_sz)
                return -1;
            for (size_t i = 0; i < len; i++)
                leaf[i] = cur[i];
            leaf[len] = '\0';
            *result = dir;
            return 0;
        }

        char comp[VFS_MAX_PATHNAME + 1];
        for (size_t i = 0; i < len; i++)
            comp[i] = cur[i];
        comp[len] = '\0';

        struct vnode *next = NULL;
        if (!dir->v_ops || !dir->v_ops->lookup)
            return -1;
        if (dir->v_ops->lookup(dir, &next, comp) != 0 || !next)
            return -1;

        dir = vfs_follow_mount(next);

        if (is_last) {
            *result = dir;
            return 0;
        }

        cur += len + 1;

        /* Path ended with '/', leaving no component to act on. */
        if (*cur == '\0')
            return -1;
    }
}    /* vfs_walk */

/** ----------------------------------------------------------------------
 * @brief vfs_open() – Open @pathname, creating it when O_CREAT is set.
 *
 * Fills f_pos and flags here instead of in the file system: their
 * meaning is identical for every fs, so centralising them keeps each
 * backend from repeating (and forgetting) the initialisation.
 * @param[in]  pathname Absolute path beginning with '/'.
 * @param      flags    O_CREAT to create a missing file.
 * @param[out] target   Handle for the opened file.
 * @return 0 on success, -1 otherwise.
 * -------------------------------------------------------------------- */
int vfs_open(const char *pathname, int flags, struct file **target)
{
    if (!target)
        return -1;

    struct vnode *dir = NULL;
    char leaf[VFS_MAX_PATHNAME + 1];
    if (vfs_walk(pathname, 1, &dir, leaf, sizeof(leaf)) != 0)
        return -1;
    if (!dir->v_ops || !dir->v_ops->lookup)
        return -1;

    struct vnode *node = NULL;
    if (dir->v_ops->lookup(dir, &node, leaf) == 0 && node) {
        /* The leaf may itself be a mount point, in which case the name
         * denotes the mounted file system's root. */
        node = vfs_follow_mount(node);
    } else {
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
 * @param[in] file Handle previously returned by vfs_open().
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
 * @param[in] file Handle previously returned by vfs_open().
 * @param[in] buf  Source bytes.
 * @param     len  Number of bytes requested.
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
 * @param[in]  file Handle previously returned by vfs_open().
 * @param[out] buf  Destination buffer.
 * @param      len  Number of bytes requested.
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
 * Leaves the duplicate-name check to the file system rather than
 * pre-checking with a lookup here: tmpfs_create() already rejects a
 * name it holds, and testing in both layers invites the two answers to
 * disagree.
 * @param[in] pathname Absolute path of the directory to create.
 * @return 0 on success, -1 otherwise.
 * -------------------------------------------------------------------- */
int vfs_mkdir(const char *pathname)
{
    struct vnode *dir = NULL;
    char leaf[VFS_MAX_PATHNAME + 1];

    if (vfs_walk(pathname, 1, &dir, leaf, sizeof(leaf)) != 0)
        return -1;

    /* A file system that offers no mkdir cannot create directories;
     * this is where a read-only fs refuses the call. */
    if (!dir->v_ops || !dir->v_ops->mkdir)
        return -1;

    struct vnode *node = NULL;
    if (dir->v_ops->mkdir(dir, &node, leaf) != 0 || !node)
        return -1;

    return 0;
}    /* vfs_mkdir */

/** ----------------------------------------------------------------------
 * @brief vfs_mount() – Mount @filesystem onto the directory @target.
 *
 * The new mount is published into mountpoint->mounted only once it is
 * fully built, so a failed setup leaves the tree exactly as it was and
 * no traversal can reach a half-constructed mount.
 * @param[in] target     Absolute path of an existing directory.
 * @param[in] filesystem Registered file-system name.
 * @return 0 on success, -1 otherwise.
 * -------------------------------------------------------------------- */
int vfs_mount(const char *target, const char *filesystem)
{
    if (!target || !filesystem)
        return -1;

    struct filesystem *fs = find_filesystem(filesystem);
    if (!fs || !fs->setup_mount)
        return -1;

    /* Resolve the parent and look the final component up directly,
     * rather than resolving @target whole: a full walk would cross a
     * mount already covering @target and hand back the mounted file
     * system's root, so mounting twice on one directory would stack a
     * second mount instead of being refused. Stopping at the parent
     * keeps the covered vnode itself in view.
     *
     * The root is deliberately not reachable this way: it is mounted by
     * vfs_init() and re-mounting it is not supported. */
    struct vnode *dir = NULL;
    char leaf[VFS_MAX_PATHNAME + 1];
    if (vfs_walk(target, 1, &dir, leaf, sizeof(leaf)) != 0)
        return -1;
    if (!dir->v_ops || !dir->v_ops->lookup)
        return -1;

    struct vnode *mountpoint = NULL;
    if (dir->v_ops->lookup(dir, &mountpoint, leaf) != 0 || !mountpoint)
        return -1;

    /* Covering a regular file would yield a vnode nothing could ever be
     * looked up inside, so the mount is refused here instead of failing
     * obscurely on the next traversal. */
    if (mountpoint->type != VNODE_TYPE_DIR)
        return -1;

    /* Already covered: a second mount would hide the first with no way
     * left to reach it. */
    if (mountpoint->mounted)
        return -1;

    struct mount *mnt = (struct mount *)kmalloc(sizeof(struct mount));
    if (!mnt)
        return -1;

    mnt->fs = fs;
    mnt->root = NULL;
    mnt->mountpoint = mountpoint;
    if (fs->setup_mount(fs, mnt) != 0 || !mnt->root) {
        kfree(mnt);
        return -1;
    }

    mountpoint->mounted = mnt;
    return 0;
}    /* vfs_mount */

/** ----------------------------------------------------------------------
 * @brief vfs_lookup() – Resolve @pathname to its vnode.
 *
 * @param[in]  pathname Absolute path to resolve.
 * @param[out] target   Vnode the path names.
 * @return 0 on success, -1 on a bad path or a missing component.
 * -------------------------------------------------------------------- */
int vfs_lookup(const char *pathname, struct vnode **target)
{
    if (!target)
        return -1;

    return vfs_walk(pathname, 0, target, NULL, 0);
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
    /* Nothing sits above the root file system, so it covers no vnode. */
    mnt->mountpoint = NULL;
    if (tmpfs->setup_mount(tmpfs, mnt) != 0) {
        uart_puts("[VFS] tmpfs mount setup failed.\n");
        kfree(mnt);
        return;
    }

    g_rootfs = mnt;
}    /* vfs_init */
