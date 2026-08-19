#include "tmpfs.h"
#include "vfs.h"
#include "kmalloc.h"
#include "utils.h"
#include "types.h"

/* Component name length, NUL excluded. */
#define TMPFS_MAX_NAME      15

/* Entries a single directory can hold. */
#define TMPFS_MAX_ENTRIES   16

/* Largest a regular file may grow. */
#define TMPFS_MAX_FILESIZE  4096

enum tmpfs_type {
    TMPFS_TYPE_FILE,
    TMPFS_TYPE_DIR,
};

/*
 * Backing store for one tmpfs vnode, reachable through vnode->internal.
 *
 * Directory children live in a fixed array rather than a linked list:
 * the entry count is capped at 16 and lookup only ever scans linearly,
 * so an array avoids a per-entry allocation and the list plumbing that
 * would come with it.
 */
struct tmpfs_node {
    char name[TMPFS_MAX_NAME + 1];
    enum tmpfs_type type;
    struct vnode *vnode;
    struct tmpfs_node *parent;
    struct tmpfs_node *entries[TMPFS_MAX_ENTRIES];
    int entry_count;
    char *data;
    size_t size;
};

/* Defined at the bottom of this file, once the handlers they point at
 * exist; declared here because tmpfs_new_node() binds them onto every
 * vnode it builds. */
static struct vnode_operations g_tmpfs_v_ops;
static struct file_operations g_tmpfs_f_ops;

static int tmpfs_lookup(struct vnode *dir_node, struct vnode **target,
                        const char *component_name);
static int tmpfs_create(struct vnode *dir_node, struct vnode **target,
                        const char *component_name);
static int tmpfs_mkdir(struct vnode *dir_node, struct vnode **target,
                       const char *component_name);
static int tmpfs_open(struct vnode *file_node, struct file **target);
static int tmpfs_close(struct file *file);
static int tmpfs_read(struct file *file, void *buf, size_t len);
static int tmpfs_write(struct file *file, const void *buf, size_t len);
static long tmpfs_lseek64(struct file *file, long offset, int whence);

/** ----------------------------------------------------------------------
 * @brief tmpfs_name_len() – Measure a NUL-terminated component name.
 *
 * Stops counting once the tmpfs limit is exceeded so an overlong name
 * cannot drive an unbounded scan.
 * @param[in] name Component name to measure.
 * @return Length in bytes, or TMPFS_MAX_NAME + 1 when it is too long.
 * -------------------------------------------------------------------- */
static size_t tmpfs_name_len(const char *name)
{
    size_t len = 0;

    while (name[len] != '\0') {
        if (++len > TMPFS_MAX_NAME)
            return TMPFS_MAX_NAME + 1;
    }
    return len;
}    /* tmpfs_name_len */

/** ----------------------------------------------------------------------
 * @brief tmpfs_new_node() – Allocate a node and its paired vnode.
 *
 * Both objects are created together because a tmpfs node is only ever
 * reachable through its vnode; allocating them separately would leave a
 * window where one exists without the other.
 * @param[in] name  Component name to store; must already fit the limit.
 * @param     type  Whether the node is a regular file or a directory.
 * @param[in] mount Mount the new vnode belongs to.
 * @return The new node, or NULL when out of memory.
 * -------------------------------------------------------------------- */
static struct tmpfs_node *tmpfs_new_node(const char *name,
                                         enum tmpfs_type type,
                                         struct mount *mount)
{
    struct tmpfs_node *node =
        (struct tmpfs_node *)kmalloc(sizeof(struct tmpfs_node));
    if (!node)
        return NULL;

    struct vnode *vnode = (struct vnode *)kmalloc(sizeof(struct vnode));
    if (!vnode) {
        kfree(node);
        return NULL;
    }

    size_t i = 0;
    while (name[i] != '\0' && i < TMPFS_MAX_NAME) {
        node->name[i] = name[i];
        i++;
    }
    node->name[i] = '\0';

    node->type = type;
    node->entry_count = 0;
    for (int e = 0; e < TMPFS_MAX_ENTRIES; e++)
        node->entries[e] = NULL;
    node->data = NULL;
    node->size = 0;

    /* A root directory has no parent inside its own file system, so it
     * points at itself: ".." from the root then stays at the root
     * without the caller needing a special case. The real parent is
     * filled in by tmpfs_add_entry() for every other node. */
    node->parent = node;

    vnode->mount = mount;
    vnode->v_ops = &g_tmpfs_v_ops;
    vnode->f_ops = &g_tmpfs_f_ops;
    vnode->internal = node;
    vnode->type = (type == TMPFS_TYPE_DIR) ? VNODE_TYPE_DIR
                                           : VNODE_TYPE_FILE;

    /* kmalloc() hands back uninitialised memory: leaving this stale
     * would make the traversal mistake a fresh vnode for a mount point
     * and follow a garbage pointer. */
    vnode->mounted = NULL;

    node->vnode = vnode;
    return node;
}    /* tmpfs_new_node */

/** ----------------------------------------------------------------------
 * @brief tmpfs_lookup() – Find @component_name inside @dir_node.
 *
 * A miss is normal control flow — vfs_open() relies on it to decide
 * whether O_CREAT should create the file — so nothing is printed here.
 *
 * "." and ".." are answered from the node itself rather than the entry
 * array, because tmpfs never stores them as real entries. ".." reads
 * node->parent, which tmpfs_add_entry() sets to the owning directory and
 * tmpfs_alloc_node() points at the node itself for a root: that
 * self-loop is what makes ".." at a file-system root stay put without a
 * special case here. Climbing out of a MOUNTED root is not tmpfs's
 * business — the generic layer steps off the mount before asking, since
 * only it knows what the mount covers.
 * @param[in]  dir_node       Directory to search.
 * @param[out] target         Vnode of the matching entry.
 * @param[in]  component_name Name to match exactly.
 * @return 0 when found, -1 when absent or @dir_node is not a directory.
 * -------------------------------------------------------------------- */
static int tmpfs_lookup(struct vnode *dir_node, struct vnode **target,
                        const char *component_name)
{
    if (!dir_node || !target || !component_name)
        return -1;

    struct tmpfs_node *dir = (struct tmpfs_node *)dir_node->internal;
    if (!dir || dir->type != TMPFS_TYPE_DIR)
        return -1;

    if (str_eq(component_name, ".")) {
        *target = dir_node;
        return 0;
    }

    if (str_eq(component_name, "..")) {
        /* A node built by tmpfs always has a parent (its root points at
         * itself), so the fallback only guards a malformed node. */
        *target = dir->parent ? dir->parent->vnode : dir_node;
        return 0;
    }

    for (int i = 0; i < dir->entry_count; i++) {
        if (str_eq(dir->entries[i]->name, component_name)) {
            *target = dir->entries[i]->vnode;
            return 0;
        }
    }
    return -1;
}    /* tmpfs_lookup */

/** ----------------------------------------------------------------------
 * @brief tmpfs_add_entry() – Add an entry of @type to @dir_node.
 *
 * Creating a directory differs from creating a regular file only in the
 * node type, so both go through one body: the name, capacity, and
 * duplicate checks stay identical instead of drifting apart in two
 * near-copies.
 *
 * Rejects a duplicate name so an existing entry is never silently
 * replaced along with its contents.
 * @param[in]  dir_node       Directory that will own the new entry.
 * @param[out] target         Vnode of the created entry.
 * @param[in]  component_name Name for the new entry.
 * @param      type           Whether to create a regular file or a
 *                            directory.
 * @return 0 on success, -1 when the name is too long, already taken,
 *         the directory is full, or memory ran out.
 * -------------------------------------------------------------------- */
static int tmpfs_add_entry(struct vnode *dir_node, struct vnode **target,
                           const char *component_name,
                           enum tmpfs_type type)
{
    if (!dir_node || !target || !component_name)
        return -1;

    struct tmpfs_node *dir = (struct tmpfs_node *)dir_node->internal;
    if (!dir || dir->type != TMPFS_TYPE_DIR)
        return -1;

    size_t len = tmpfs_name_len(component_name);
    if (len == 0 || len > TMPFS_MAX_NAME)
        return -1;

    if (dir->entry_count >= TMPFS_MAX_ENTRIES)
        return -1;

    struct vnode *existing = NULL;
    if (tmpfs_lookup(dir_node, &existing, component_name) == 0)
        return -1;

    struct tmpfs_node *node = tmpfs_new_node(component_name, type,
                                             dir_node->mount);
    if (!node)
        return -1;

    node->parent = dir;
    dir->entries[dir->entry_count++] = node;
    *target = node->vnode;
    return 0;
}    /* tmpfs_add_entry */

/** ----------------------------------------------------------------------
 * @brief tmpfs_create() – Add a regular file to @dir_node.
 *
 * @param[in]  dir_node       Directory that will own the new file.
 * @param[out] target         Vnode of the created file.
 * @param[in]  component_name Name for the new entry.
 * @return 0 on success, -1 otherwise.
 * -------------------------------------------------------------------- */
static int tmpfs_create(struct vnode *dir_node, struct vnode **target,
                        const char *component_name)
{
    return tmpfs_add_entry(dir_node, target, component_name,
                           TMPFS_TYPE_FILE);
}    /* tmpfs_create */

/** ----------------------------------------------------------------------
 * @brief tmpfs_mkdir() – Add a subdirectory to @dir_node.
 *
 * @param[in]  dir_node       Directory that will own the new subdirectory.
 * @param[out] target         Vnode of the created directory.
 * @param[in]  component_name Name for the new entry.
 * @return 0 on success, -1 otherwise.
 * -------------------------------------------------------------------- */
static int tmpfs_mkdir(struct vnode *dir_node, struct vnode **target,
                       const char *component_name)
{
    return tmpfs_add_entry(dir_node, target, component_name,
                           TMPFS_TYPE_DIR);
}    /* tmpfs_mkdir */

/** ----------------------------------------------------------------------
 * @brief tmpfs_open() – Allocate a handle for @file_node.
 *
 * Only binds the vnode; the generic layer initialises f_pos and flags.
 * @param[in]  file_node Vnode being opened.
 * @param[out] target    Freshly allocated handle.
 * @return 0 on success, -1 on bad argument or allocation failure.
 * -------------------------------------------------------------------- */
static int tmpfs_open(struct vnode *file_node, struct file **target)
{
    if (!file_node || !target)
        return -1;

    struct file *file = (struct file *)kmalloc(sizeof(struct file));
    if (!file)
        return -1;

    file->vnode = file_node;
    file->f_ops = file_node->f_ops;
    file->f_pos = 0;
    file->flags = 0;

    *target = file;
    return 0;
}    /* tmpfs_open */

/** ----------------------------------------------------------------------
 * @brief tmpfs_close() – Free an opened handle.
 *
 * The vnode and its contents survive: they stay linked into the parent
 * directory and must outlive any single handle.
 * @param[in] file Handle to release.
 * @return 0 on success, -1 on bad argument.
 * -------------------------------------------------------------------- */
static int tmpfs_close(struct file *file)
{
    if (!file)
        return -1;

    kfree(file);
    return 0;
}    /* tmpfs_close */

/** ----------------------------------------------------------------------
 * @brief tmpfs_write() – Store @len bytes at the current file position.
 *
 * The data buffer is allocated on first write so an empty file costs
 * nothing beyond its node. A write reaching the size cap is truncated
 * rather than refused, matching the short-write semantics the caller
 * detects through the return value.
 * @param[in] file Handle to write through.
 * @param[in] buf  Source bytes.
 * @param     len  Number of bytes requested.
 * @return Bytes actually written, or -1 when the target is not a file
 *         or the buffer could not be allocated.
 * -------------------------------------------------------------------- */
static int tmpfs_write(struct file *file, const void *buf, size_t len)
{
    if (!file || !buf || !file->vnode)
        return -1;

    struct tmpfs_node *node = (struct tmpfs_node *)file->vnode->internal;
    if (!node || node->type != TMPFS_TYPE_FILE)
        return -1;

    if (file->f_pos >= TMPFS_MAX_FILESIZE)
        return 0;

    if (!node->data) {
        node->data = (char *)kmalloc(TMPFS_MAX_FILESIZE);
        if (!node->data)
            return -1;
    }

    size_t room = TMPFS_MAX_FILESIZE - file->f_pos;
    if (len > room)
        len = room;

    /* Writing past the end leaves a gap; zero it so the file never
     * exposes whatever the allocator handed back. */
    if (file->f_pos > node->size) {
        for (size_t i = node->size; i < file->f_pos; i++)
            node->data[i] = '\0';
    }

    mem_cpy(node->data + file->f_pos, buf, len);
    file->f_pos += len;
    if (file->f_pos > node->size)
        node->size = file->f_pos;

    return (int)len;
}    /* tmpfs_write */

/** ----------------------------------------------------------------------
 * @brief tmpfs_read() – Copy up to @len bytes from the file position.
 *
 * Clamps to the stored size, so a read starting at or past end-of-file
 * returns 0 instead of exposing untouched buffer bytes.
 * @param[in]  file Handle to read from.
 * @param[out] buf  Destination buffer of at least @len bytes.
 * @param      len  Number of bytes requested.
 * @return Bytes actually read, 0 at EOF, or -1 when the target is not a
 *         regular file.
 * -------------------------------------------------------------------- */
static int tmpfs_read(struct file *file, void *buf, size_t len)
{
    if (!file || !buf || !file->vnode)
        return -1;

    struct tmpfs_node *node = (struct tmpfs_node *)file->vnode->internal;
    if (!node || node->type != TMPFS_TYPE_FILE)
        return -1;

    if (!node->data || file->f_pos >= node->size)
        return 0;

    size_t avail = node->size - file->f_pos;
    if (len > avail)
        len = avail;

    mem_cpy(buf, node->data + file->f_pos, len);
    file->f_pos += len;

    return (int)len;
}    /* tmpfs_read */

/** ----------------------------------------------------------------------
 * @brief tmpfs_lseek64() – Reposition the read/write offset.
 *
 * Only SEEK_SET is supported so far, which covers rewinding a handle
 * between a write and a read.
 * @param[in] file   Handle to reposition.
 * @param     offset New absolute offset when @whence is SEEK_SET.
 * @param     whence Reference point for @offset.
 * @return The new offset, or -1 on an unsupported or out-of-range seek.
 * -------------------------------------------------------------------- */
static long tmpfs_lseek64(struct file *file, long offset, int whence)
{
    if (!file || whence != SEEK_SET)
        return -1;
    if (offset < 0 || offset > TMPFS_MAX_FILESIZE)
        return -1;

    file->f_pos = (size_t)offset;
    return offset;
}    /* tmpfs_lseek64 */

/** ----------------------------------------------------------------------
 * @brief tmpfs_setup_mount() – Build the root directory for a mount.
 *
 * Called once per mount; the root vnode points back at @mount so a
 * later traversal can tell which mount a vnode belongs to.
 * @param[in]  fs    File system being mounted.
 * @param[out] mount Mount to populate.
 * @return 0 on success, -1 on bad argument or allocation failure.
 * -------------------------------------------------------------------- */
static int tmpfs_setup_mount(struct filesystem *fs, struct mount *mount)
{
    if (!fs || !mount)
        return -1;

    struct tmpfs_node *root = tmpfs_new_node("/", TMPFS_TYPE_DIR, mount);
    if (!root)
        return -1;

    mount->fs = fs;
    mount->root = root->vnode;
    return 0;
}    /* tmpfs_setup_mount */

static struct vnode_operations g_tmpfs_v_ops = {
    .lookup = tmpfs_lookup,
    .create = tmpfs_create,
    .mkdir  = tmpfs_mkdir,
};

static struct file_operations g_tmpfs_f_ops = {
    .open    = tmpfs_open,
    .close   = tmpfs_close,
    .read    = tmpfs_read,
    .write   = tmpfs_write,
    .lseek64 = tmpfs_lseek64,
};

static struct filesystem g_tmpfs = {
    .name        = "tmpfs",
    .setup_mount = tmpfs_setup_mount,
};

/** ----------------------------------------------------------------------
 * @brief tmpfs_get_fs() – Hand out the tmpfs file-system descriptor.
 *
 * @return Pointer to the statically allocated tmpfs descriptor.
 * -------------------------------------------------------------------- */
struct filesystem *tmpfs_get_fs(void)
{
    return &g_tmpfs;
}    /* tmpfs_get_fs */
