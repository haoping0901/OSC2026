#include "vfs.h"
#include "tmpfs.h"
#include "kmalloc.h"
#include "uart.h"
#include "utils.h"
#include "types.h"
#include "errno.h"
#include "sched.h"

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
 * @brief vfs_escape_mount() – Step out of every mount rooted at @node.
 *
 * The inverse of vfs_follow_mount(), used only when resolving "..".
 * A mounted file system's root has no parent of its own — tmpfs points
 * such a root at itself — so asking the file system for ".." there
 * would stay put instead of leaving the mount. Only the generic layer
 * knows what a mount covers, so the climb happens here: the walk hops
 * to mountpoint first and lets the file system that owns THAT vnode
 * answer "..".
 *
 * Loops because mounts can stack, and stops at the root file system,
 * whose mountpoint is NULL — which is what makes "/.." stay at "/".
 * @param[in] node Vnode a ".." is about to be resolved from.
 * @return The vnode ".." should be resolved from, outside any mount.
 * -------------------------------------------------------------------- */
static struct vnode *vfs_escape_mount(struct vnode *node)
{
    while (node && node->mount && node->mount->root == node &&
           node->mount->mountpoint)
        node = node->mount->mountpoint;
    return node;
}    /* vfs_escape_mount */

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
 *
 * The walk starts at the root file system when @pathname is absolute
 * and at @start otherwise, which is how each task resolves a relative
 * path against its own working directory. A NULL @start falls back to
 * the root so a caller holding no directory (early boot, before any
 * task has a cwd) still resolves absolute paths.
 *
 * Empty components are skipped rather than rejected, so "//", "a//b"
 * and a trailing "/" all collapse to the same path. "." and ".." are
 * passed to the file system like any other name; ".." first escapes any
 * mount rooted at the current directory (see vfs_escape_mount()).
 * @param[in]  start          Directory a relative @pathname resolves
 *                            from; NULL means the root file system.
 * @param[in]  pathname       Absolute or relative path.
 * @param      stop_at_parent Non-zero to stop at the final component's
 *                            parent and report the component in @leaf.
 * @param[out] result         The resolved vnode, or the parent directory
 *                            when @stop_at_parent is set.
 * @param[out] leaf           Buffer receiving the final component; used
 *                            only when @stop_at_parent is set.
 * @param      leaf_sz        Size of @leaf in bytes, NUL included.
 * @return 0 on success, a negative error code on a malformed path, a
 *         missing component, or a component that does not fit @leaf.
 * -------------------------------------------------------------------- */
static int vfs_walk(struct vnode *start, const char *pathname,
                    int stop_at_parent, struct vnode **result,
                    char *leaf, size_t leaf_sz)
{
    if (!g_rootfs || !g_rootfs->root || !result)
        return -EINVAL;
    if (!pathname)
        return -EINVAL;
    if (stop_at_parent && (!leaf || leaf_sz == 0))
        return -EINVAL;

    size_t path_len = 0;
    while (pathname[path_len] != '\0') {
        if (++path_len > VFS_MAX_PATHNAME)
            return -ENAMETOOLONG;
    }

    const char *cur = pathname;
    struct vnode *dir;

    if (*cur == '/') {
        dir = g_rootfs->root;
        cur++;                      /* skip the leading '/' */
    } else {
        dir = start ? start : g_rootfs->root;
    }

    /* Even the starting point may be covered by a mount. */
    dir = vfs_follow_mount(dir);

    /*
     * Nothing left to walk: "/" names the root and "" names the starting
     * directory. Both are valid vnodes to resolve, but neither leaves a
     * final component for a caller that wants to create an entry.
     */
    if (*cur == '\0') {
        if (stop_at_parent)
            return -EINVAL;
        *result = dir;
        return 0;
    }

    while (1) {
        /* Measure the component up to the next separator. */
        size_t len = 0;
        while (cur[len] != '/' && cur[len] != '\0')
            len++;

        /* An empty component comes from "//" or a trailing '/'. Both
         * mean "no movement", so skip it and carry on; this is what
         * collapses repeated separators. */
        if (len == 0) {
            cur++;
            if (*cur == '\0') {
                /* The path ended on separators, e.g. "a/" or "/". The
                 * directory reached so far is the answer. */
                if (stop_at_parent)
                    return -EINVAL;
                *result = dir;
                return 0;
            }
            continue;
        }

        if (len > VFS_MAX_PATHNAME)
            return -ENAMETOOLONG;

        /* Only separators may follow, so this is the final component. */
        size_t skip = len;
        while (cur[skip] == '/')
            skip++;
        int is_last = (cur[skip] == '\0');
        int has_trailing_sep = (skip != len);

        if (is_last && stop_at_parent) {
            /* A trailing separator asserts the name is a directory, so
             * it cannot be the leaf an open or a create acts on: "f/"
             * must not quietly become "f". Resolving such a path is
             * still fine — that is the !stop_at_parent case below, which
             * checks the type it actually found. */
            if (has_trailing_sep)
                return -EINVAL;
            if (len >= leaf_sz)
                return -ENAMETOOLONG;
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

        /* ".." is resolved from outside any mount rooted here, so the
         * file system that owns the mount point answers it. */
        if (comp[0] == '.' && comp[1] == '.' && comp[2] == '\0')
            dir = vfs_escape_mount(dir);

        struct vnode *next = NULL;
        if (!dir->v_ops || !dir->v_ops->lookup)
            return -ENOTDIR;
        if (dir->v_ops->lookup(dir, &next, comp) != 0 || !next)
            return -ENOENT;

        dir = vfs_follow_mount(next);

        if (is_last) {
            /* "f/" named a directory; honour that claim rather than
             * resolving it to the regular file f. */
            if (has_trailing_sep && dir->type != VNODE_TYPE_DIR)
                return -ENOTDIR;
            *result = dir;
            return 0;
        }

        cur += skip;
    }
}    /* vfs_walk */

/** ----------------------------------------------------------------------
 * @brief vfs_open_at() – Open @pathname relative to @start.
 *
 * Fills f_pos, flags and ref_count here instead of in the file system:
 * their meaning is identical for every fs, so centralising them keeps
 * each backend from repeating (and forgetting) the initialisation.
 * @param[in]  start    Directory a relative @pathname resolves from;
 *                      NULL means the root file system. An absolute
 *                      @pathname ignores @start either way.
 * @param[in]  pathname Absolute or relative path.
 * @param      flags    O_CREAT to create a missing file.
 * @param[out] target   Handle for the opened file.
 * @return 0 on success, a negative error code otherwise.
 * -------------------------------------------------------------------- */
int vfs_open_at(struct vnode *start, const char *pathname, int flags,
                struct file **target)
{
    if (!target)
        return -EINVAL;

    struct vnode *dir = NULL;
    char leaf[VFS_MAX_PATHNAME + 1];
    int ret = vfs_walk(start, pathname, 1, &dir, leaf, sizeof(leaf));
    if (ret != 0)
        return ret;
    if (!dir->v_ops || !dir->v_ops->lookup)
        return -ENOTDIR;

    struct vnode *node = NULL;
    if (dir->v_ops->lookup(dir, &node, leaf) == 0 && node) {
        /* The leaf may itself be a mount point, in which case the name
         * denotes the mounted file system's root. */
        node = vfs_follow_mount(node);
    } else {
        /* A failed lookup is ordinary control flow, not an error to
         * report: it is exactly how O_CREAT decides to create. */
        if (!(flags & O_CREAT))
            return -ENOENT;
        if (!dir->v_ops->create)
            return -EPERM;
        if (dir->v_ops->create(dir, &node, leaf) != 0 || !node)
            return -ENOSPC;
    }

    /* A directory holds no byte stream to read or write. Refusing here
     * keeps read()/write() from having to re-check on every call. */
    if (node->type == VNODE_TYPE_DIR)
        return -EISDIR;

    if (!node->f_ops || !node->f_ops->open)
        return -EPERM;

    struct file *file = NULL;
    if (node->f_ops->open(node, &file) != 0 || !file)
        return -ENOMEM;

    file->vnode = node;
    file->f_ops = node->f_ops;
    file->f_pos = 0;
    file->flags = flags;
    file->ref_count = 1;

    *target = file;
    return 0;
}    /* vfs_open_at */

/** ----------------------------------------------------------------------
 * @brief vfs_open() – Open @pathname, creating it when O_CREAT is set.
 *
 * The root-relative form of vfs_open_at().
 * @param[in]  pathname Absolute path beginning with '/'.
 * @param      flags    O_CREAT to create a missing file.
 * @param[out] target   Handle for the opened file.
 * @return 0 on success, a negative error code otherwise.
 * -------------------------------------------------------------------- */
int vfs_open(const char *pathname, int flags, struct file **target)
{
    return vfs_open_at(NULL, pathname, flags, target);
}    /* vfs_open */

/** ----------------------------------------------------------------------
 * @brief vfs_file_get() – Claim one more reference to @file.
 *
 * @param[in,out] file Handle gaining a reference.
 * -------------------------------------------------------------------- */
void vfs_file_get(struct file *file)
{
    if (file)
        file->ref_count++;
}    /* vfs_file_get */

/** ----------------------------------------------------------------------
 * @brief vfs_close() – Drop a reference, releasing the handle at zero.
 *
 * fork() leaves parent and child pointing at one open file description,
 * so the handle must survive until BOTH have closed it: the file system
 * is asked to free it only on the last reference. Without this the
 * second close would free a struct the first had already released.
 * @param[in,out] file Handle previously returned by vfs_open().
 * @return 0 on success, a negative error code on bad argument.
 * -------------------------------------------------------------------- */
int vfs_close(struct file *file)
{
    if (!file || !file->f_ops || !file->f_ops->close)
        return -EINVAL;

    if (--file->ref_count > 0)
        return 0;

    return file->f_ops->close(file) == 0 ? 0 : -EIO;
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
        return -EINVAL;
    int ret = file->f_ops->write(file, buf, len);
    return ret < 0 ? -EIO : ret;
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
        return -EINVAL;
    int ret = file->f_ops->read(file, buf, len);
    return ret < 0 ? -EIO : ret;
}    /* vfs_read */

/** ----------------------------------------------------------------------
 * @brief vfs_mkdir_at() – Create the directory @pathname names.
 *
 * Leaves the duplicate-name check to the file system rather than
 * pre-checking with a lookup here: tmpfs_create() already rejects a
 * name it holds, and testing in both layers invites the two answers to
 * disagree.
 * @param[in] start    Directory a relative @pathname resolves from;
 *                     NULL means the root file system. An absolute
 *                     @pathname ignores @start either way.
 * @param[in] pathname Absolute or relative path of the new directory.
 * @return 0 on success, a negative error code otherwise.
 * -------------------------------------------------------------------- */
int vfs_mkdir_at(struct vnode *start, const char *pathname)
{
    struct vnode *dir = NULL;
    char leaf[VFS_MAX_PATHNAME + 1];

    int ret = vfs_walk(start, pathname, 1, &dir, leaf, sizeof(leaf));
    if (ret != 0)
        return ret;

    /* A file system that offers no mkdir cannot create directories;
     * this is where a read-only fs refuses the call. */
    if (!dir->v_ops || !dir->v_ops->mkdir)
        return -EPERM;

    /* Report an existing name as such rather than as a generic failure:
     * the file system refuses a duplicate, but only this layer can tell
     * that apart from a full directory. */
    struct vnode *existing = NULL;
    if (dir->v_ops->lookup &&
        dir->v_ops->lookup(dir, &existing, leaf) == 0 && existing)
        return -EEXIST;

    struct vnode *node = NULL;
    if (dir->v_ops->mkdir(dir, &node, leaf) != 0 || !node)
        return -ENOSPC;

    return 0;
}    /* vfs_mkdir_at */

/** ----------------------------------------------------------------------
 * @brief vfs_mkdir() – Create the directory named by @pathname.
 *
 * The root-relative form of vfs_mkdir_at().
 * @param[in] pathname Absolute path of the directory to create.
 * @return 0 on success, a negative error code otherwise.
 * -------------------------------------------------------------------- */
int vfs_mkdir(const char *pathname)
{
    return vfs_mkdir_at(NULL, pathname);
}    /* vfs_mkdir */

/** ----------------------------------------------------------------------
 * @brief vfs_mount_at() – Mount @filesystem onto the directory @target.
 *
 * The new mount is published into mountpoint->mounted only once it is
 * fully built, so a failed setup leaves the tree exactly as it was and
 * no traversal can reach a half-constructed mount.
 * @param[in] start      Directory a relative @target resolves from;
 *                       NULL means the root file system. An absolute
 *                       @target ignores @start either way.
 * @param[in] target     Absolute or relative path of the mount point.
 * @param[in] filesystem Registered file-system name.
 * @return 0 on success, a negative error code otherwise.
 * -------------------------------------------------------------------- */
int vfs_mount_at(struct vnode *start, const char *target,
                 const char *filesystem)
{
    if (!target || !filesystem)
        return -EINVAL;

    struct filesystem *fs = find_filesystem(filesystem);
    if (!fs || !fs->setup_mount)
        return -ENODEV;

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
    int ret = vfs_walk(start, target, 1, &dir, leaf, sizeof(leaf));
    if (ret != 0)
        return ret;
    if (!dir->v_ops || !dir->v_ops->lookup)
        return -ENOTDIR;

    struct vnode *mountpoint = NULL;
    if (dir->v_ops->lookup(dir, &mountpoint, leaf) != 0 || !mountpoint)
        return -ENOENT;

    /* Covering a regular file would yield a vnode nothing could ever be
     * looked up inside, so the mount is refused here instead of failing
     * obscurely on the next traversal. */
    if (mountpoint->type != VNODE_TYPE_DIR)
        return -ENOTDIR;

    /* Already covered: a second mount would hide the first with no way
     * left to reach it. */
    if (mountpoint->mounted)
        return -EBUSY;

    struct mount *mnt = (struct mount *)kmalloc(sizeof(struct mount));
    if (!mnt)
        return -ENOMEM;

    mnt->fs = fs;
    mnt->root = NULL;
    mnt->mountpoint = mountpoint;
    if (fs->setup_mount(fs, mnt) != 0 || !mnt->root) {
        kfree(mnt);
        return -ENOMEM;
    }

    mountpoint->mounted = mnt;
    return 0;
}    /* vfs_mount_at */

/** ----------------------------------------------------------------------
 * @brief vfs_mount() – Mount @filesystem onto the directory @target.
 *
 * The root-relative form of vfs_mount_at().
 * @param[in] target     Absolute path of an existing directory.
 * @param[in] filesystem Registered file-system name.
 * @return 0 on success, a negative error code otherwise.
 * -------------------------------------------------------------------- */
int vfs_mount(const char *target, const char *filesystem)
{
    return vfs_mount_at(NULL, target, filesystem);
}    /* vfs_mount */

/** ----------------------------------------------------------------------
 * @brief vfs_lookup_at() – Resolve @pathname starting from @start.
 *
 * @param[in]  start    Directory a relative @pathname resolves from;
 *                      NULL means the root file system. An absolute
 *                      @pathname ignores @start either way.
 * @param[in]  pathname Absolute or relative path to resolve.
 * @param[out] target   Vnode the path names.
 * @return 0 on success, a negative error code on a bad path or a
 *         missing component.
 * -------------------------------------------------------------------- */
int vfs_lookup_at(struct vnode *start, const char *pathname,
                  struct vnode **target)
{
    if (!target)
        return -EINVAL;

    return vfs_walk(start, pathname, 0, target, NULL, 0);
}    /* vfs_lookup_at */

/** ----------------------------------------------------------------------
 * @brief vfs_lookup() – Resolve @pathname to its vnode.
 *
 * The root-relative form of vfs_lookup_at().
 * @param[in]  pathname Absolute path to resolve.
 * @param[out] target   Vnode the path names.
 * @return 0 on success, a negative error code on a bad path or a
 *         missing component.
 * -------------------------------------------------------------------- */
int vfs_lookup(const char *pathname, struct vnode **target)
{
    return vfs_lookup_at(NULL, pathname, target);
}    /* vfs_lookup */

/** ----------------------------------------------------------------------
 * @brief vfs_chdir() – Resolve @pathname to the directory it names.
 *
 * @param[in]  start    Directory a relative @pathname resolves from;
 *                      NULL means the root file system. An absolute
 *                      @pathname ignores @start either way.
 * @param[in]  pathname Absolute or relative path of the new directory.
 * @param[out] target   Vnode of the resolved directory.
 * @return 0 on success, a negative error code when the path does not
 *         resolve or does not name a directory.
 * -------------------------------------------------------------------- */
int vfs_chdir(struct vnode *start, const char *pathname,
              struct vnode **target)
{
    if (!target)
        return -EINVAL;

    struct vnode *node = NULL;
    int ret = vfs_walk(start, pathname, 0, &node, NULL, 0);
    if (ret != 0)
        return ret;

    /* Accepting a file would leave the task with a cwd no later lookup
     * could resolve a component inside. */
    if (node->type != VNODE_TYPE_DIR)
        return -ENOTDIR;

    *target = node;
    return 0;
}    /* vfs_chdir */

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

    /*
     * sched_init() runs before this point, so the bootstrap thread was
     * zeroed while there was still no root to point at. Install it now
     * that one exists; every later task inherits a cwd from its parent
     * or falls back to the root inside vfs_walk().
     */
    g_bootstrap.cwd = mnt->root;
}    /* vfs_init */
