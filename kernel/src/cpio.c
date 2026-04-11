#include "cpio.h"
#include "uart.h"
#include "utils.h"
#include "types.h"

/*
 * CPIO New ASCII Format header
 */
struct cpio_newc_header {
    char c_magic[6];
    char c_ino[8];
    char c_mode[8];
    char c_uid[8];
    char c_gid[8];
    char c_nlink[8];
    char c_mtime[8];
    char c_filesize[8];
    char c_devmajor[8];
    char c_devminor[8];
    char c_rdevmajor[8];
    char c_rdevminor[8];
    char c_namesize[8];
    char c_check[8];
};

/* Helper to parse 8-byte hex string into integer */
static unsigned long hex2int(const char *hex, int len)
{
    unsigned long val = 0;
    int i;
    for (i = 0; i < len; i++) {
        val <<= 4;
        if (hex[i] >= '0' && hex[i] <= '9') {
            val += hex[i] - '0';
        } else if (hex[i] >= 'A' && hex[i] <= 'F') {
            val += hex[i] - 'A' + 10;
        } else if (hex[i] >= 'a' && hex[i] <= 'f') {
            val += hex[i] - 'a' + 10;
        }
    }
    return val;
}

/* Align address to 4-byte boundary */
static inline unsigned long align_4(unsigned long addr)
{
    return (addr + 3) & ~3UL;
}

void cpio_ls(const void *archive)
{
    if (!archive) {
        uart_puts("cpio: No initrd found.\n");
        return;
    }

    const char *ptr = (const char *)archive;

    while (1) {
        const struct cpio_newc_header *hdr = (const struct cpio_newc_header *)ptr;

        /* Check magic number "070701" */
        if (hdr->c_magic[0] != '0' || hdr->c_magic[1] != '7' ||
            hdr->c_magic[2] != '0' || hdr->c_magic[3] != '7' ||
            hdr->c_magic[4] != '0' || hdr->c_magic[5] != '1') {
            uart_puts("cpio: Invalid magic.\n");
            break;
        }

        unsigned long namesize = hex2int(hdr->c_namesize, 8);
        unsigned long filesize = hex2int(hdr->c_filesize, 8);

        const char *filename = ptr + sizeof(struct cpio_newc_header);

        /* Check for end of archive */
        if (str_eq(filename, "TRAILER!!!")) {
            break;
        }

        /* Filter current directory '.' */
        if (!str_eq(filename, ".")) {
            uart_puts(filename);
            uart_puts("\n");
        }

        /* Next header: align(header + namesize) + align(filesize) */
        unsigned long next_hdr = align_4((unsigned long)filename + namesize);
        next_hdr = align_4(next_hdr + filesize);
        ptr = (const char *)next_hdr;
    }
}

int cpio_cat(const void *archive, const char *target_filename)
{
    if (!archive) {
        uart_puts("cpio: No initrd found.\n");
        return -1;
    }

    const char *ptr = (const char *)archive;
    while (1) {
        const struct cpio_newc_header *hdr = (const struct cpio_newc_header *)ptr;

        if (hdr->c_magic[0] != '0' || hdr->c_magic[1] != '7' ||
            hdr->c_magic[2] != '0' || hdr->c_magic[3] != '7' ||
            hdr->c_magic[4] != '0' || hdr->c_magic[5] != '1') {
            uart_puts("cpio: Invalid magic.\n");
            return -1;
        }

        unsigned long namesize = hex2int(hdr->c_namesize, 8);
        unsigned long filesize = hex2int(hdr->c_filesize, 8);

        const char *filename = ptr + sizeof(struct cpio_newc_header);

        if (str_eq(filename, "TRAILER!!!")) {
            break;
        }

        unsigned long file_data = align_4((unsigned long)filename + namesize);

        if (str_eq(filename, target_filename)) {
            const char *data = (const char *)file_data;
            unsigned long i;
            for (i = 0; i < filesize; i++) {
                uart_putc(data[i]);
            }
            if (filesize > 0 && data[filesize - 1] != '\n') {
                uart_putc('\n');
            }
            return 0;
        }

        unsigned long next_hdr = align_4(file_data + filesize);
        ptr = (const char *)next_hdr;
    }

    uart_puts("cat: ");
    uart_puts(target_filename);
    uart_puts(": No such file or directory\n");

    return -1;
}
