#include "dtb.h"
#include "uart.h"



#define FDT_HEADER_MAGIC 0xD00DFEED
/*
    Ref. https://github.com/devicetree-org/devicetree-specification/blob/main/
         source/chapter5-flattened-format.rst#id9
 */
struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

#define FDT_BEGIN_NODE  0x00000001
#define FDT_END_NODE    0x00000002
#define FDT_PROP        0x00000003
#define FDT_NOP         0x00000004
#define FDT_END         0x00000009

struct fdt_prop {
    uint32_t len;
    uint32_t nameoff;
};

static char *g_dtb_addr;

static inline uint32_t bswap_32(uint32_t x)
{
    return ((x & 0xFF000000) >> 24) |
           ((x & 0x00FF0000) >> 8) |
           ((x & 0x0000FF00) << 8) |
           ((x & 0x000000FF) << 24);
}

static inline uint64_t align_32(uint64_t addr)
{
    return (addr + 3) & ~((uint64_t)3);
}

/*
 * node_name_match - compare a DTB node name against a path segment.
 *
 * DTB node names often include a unit-address: "serial@10000000".
 * This helper treats the part before '@' as the effective name, so
 * node_name_match("serial@10000000", "serial") returns 1 (true).
 */
static int node_name_match(const char *node_name, const char *seg)
{
    while (*seg) {
        if (*node_name == '\0' || *node_name == '@')
            return 0;           /* seg still has chars but name ran out */
        if (*node_name != *seg)
            return 0;
        node_name++;
        seg++;
    }
    /* seg exhausted: node_name must be end-of-string or '@' */
    return (*node_name == '\0' || *node_name == '@');
}

/*
 * path_segment - return pointer to the i-th component of a '/' separated
 * path, and store its length in *len.  Returns NULL when i >= depth.
 * path must start with '/'.  Depth 0 is the root (""), depth 1 is the
 * first real component, etc.
 */
static const char *path_segment(const char *path, int idx, int *seg_len)
{
    if (!path || path[0] != '/')
        return 0;

    const char *p = path + 1;   /* skip leading '/' */
    int i = 0;

    while (1) {
        if (*p == '\0') {
            /* we reached the end without finding idx */
            return 0;
        }
        const char *start = p;
        while (*p && *p != '/')
            p++;
        if (i == idx) {
            *seg_len = (int)(p - start);
            return start;
        }
        i++;
        if (*p == '/')
            p++;
    }
}

/* Count how many '/' separated components a path has (root="/" → 0) */
static int path_depth(const char *path)
{
    if (!path || path[0] != '/')
        return -1;
    if (path[1] == '\0')
        return 0;   /* root */
    int n = 0;
    for (const char *p = path + 1; *p; p++)
        if (*p == '/')
            n++;
    return n + 1;   /* e.g. "/soc/serial" has 2 components */
}

void dtb_set_addr(void *addr)
{
    g_dtb_addr = (char *)addr;
}

void *dtb_get_addr(void)
{
    return (void *)g_dtb_addr;
}

/*
 * dtb_getprop - locate a property inside the DTB.
 *
 * node_path examples: "/", "/soc", "/soc/serial"
 * prop_name examples: "reg", "compatible"
 *
 * Returns a pointer to the raw property data, or NULL on failure.
 * The data is big-endian; callers must byte-swap as needed.
 */
const void *_dtb_getprop(const char *node_path, const char *prop_name,
                       int *lenp)
{
    if (!g_dtb_addr || !node_path || !prop_name)
        return 0;

    struct fdt_header *header = (struct fdt_header *)g_dtb_addr;
    if (bswap_32(header->magic) != FDT_HEADER_MAGIC)
        return 0;

    int wanted_depth = path_depth(node_path); /* number of components */

    uint32_t struct_size = bswap_32(header->size_dt_struct);
    char *dt_struct = (char *)header + bswap_32(header->off_dt_struct);
    char *dt_strings = (char *)header + bswap_32(header->off_dt_strings);

    char *cur        = dt_struct;
    char *struct_end = dt_struct + struct_size;

    /*
     * current_depth tracks how many levels deep we are in the tree.
     * depth 0 = inside root node.
     * matched_depth is set when we find the target node.
     */
    int current_depth = -1;     /* -1 = not yet inside root */
    int matched_depth = -1;     /* -1 = target not yet found */

    while (cur < struct_end) {
        uint32_t token = bswap_32(*(uint32_t *)cur);
        cur += 4;

        if (token == FDT_BEGIN_NODE) {
            const char *node_name = cur;
            while (*cur)
                cur++;
            cur++;
            cur = (char *)align_32((uint64_t)cur);

            current_depth++;

            /*
             * Check whether this node matches the corresponding segment
             * of the wanted path at this depth.
             *
             * current_depth 0 = root node (path segment index -1, always OK)
             * current_depth 1 = first path component (index 0)
             * …
             */
            if (matched_depth < 0) {
                /* Still trying to match the prefix up to wanted_depth */
                if (current_depth == 0) {
                    /* Root node always matches */
                    if (wanted_depth == 0)
                        matched_depth = 0;  /* target IS the root */
                } else {
                    /* Segment index within path = current_depth - 1 */
                    int seg_len;
                    const char *seg = path_segment(node_path,
                                                   current_depth - 1,
                                                   &seg_len);
                    if (seg) {
                        /* Temporarily null-terminate comparison */
                        char seg_buf[64];
                        int copy_len = seg_len < 63 ? seg_len : 63;
                        int i;
                        for (i = 0; i < copy_len; i++)
                            seg_buf[i] = seg[i];
                        seg_buf[i] = '\0';

                        if (node_name_match(node_name, seg_buf)) {
                            if (current_depth == wanted_depth)
                                matched_depth = current_depth;
                            /* else: continue descending */
                        }
                        /* else: wrong branch — we'll go deeper but won't match */
                    }
                }
            }

        } else if (token == FDT_END_NODE) {
            if (matched_depth == current_depth)
                matched_depth = -1;     /* exited the target node */
            current_depth--;

        } else if (token == FDT_PROP) {
            struct fdt_prop *prop = (struct fdt_prop *)cur;
            cur += sizeof(struct fdt_prop);

            uint32_t prop_len = bswap_32(prop->len);
            const char *name = dt_strings + bswap_32(prop->nameoff);

            if (matched_depth >= 0) {
                /* We are inside the target node — check each property */
                const char *a = name, *b = prop_name;
                int eq = 1;
                while (*a || *b) {
                    if (*a != *b) {
                        eq = 0;
                        break;
                    }
                    ++a, ++b;
                }
                if (eq == 1) {
                    if (lenp)
                        *lenp = (int)prop_len;
                    return cur;
                }
            }

            cur += prop_len;
            cur = (char *)align_32((uint64_t)cur);

        } else if (token == FDT_NOP) {
            continue;

        } else if (token == FDT_END) {
            break;

        } else {
            break;
        }
    }

    if (lenp)
        *lenp = 0;
    return 0;
}

uintptr_t dtb_getprop(const char *path, const char *prop_name)
{
    int len = 0;
    const void *value = _dtb_getprop(path, prop_name, &len);
    if (!value || len < 4)
        return 0;

    const uint32_t *p32 = (const uint32_t *)value;
    if (len >= 8) {
        uint64_t hi = bswap_32(p32[0]);
        uint64_t lo = bswap_32(p32[1]);
        return (uintptr_t)((hi << 32) | lo);
    }

    return (uintptr_t)bswap_32(p32[0]);
}
