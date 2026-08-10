/*
 * QEMU ramfb driver implementation.
 *
 * Talks to QEMU's fw_cfg DMA interface to register a host-visible
 * framebuffer at FB_BASE in 1920x1080 XRGB8888 format. After
 * video_init() succeeds the kernel can blit pixels by simply storing
 * them into [FB_BASE, FB_BASE + FB_WIDTH*FB_HEIGHT*4); QEMU samples
 * the same memory each refresh and pushes it to the GTK/SDL window.
 *
 * On real hardware (OPI-RV2) FB_BASE instead points to the SoC's
 * dedicated framebuffer aperture; the fw_cfg registration becomes a
 * no-op (no fw_cfg device exists), so we only execute that path under
 * the QEMU build flag. The blit path stays the same because the SoC
 * also expects raw XRGB8888 pixels in its aperture, but it requires a
 * cbo.flush so the writes leave the data cache.
 */

#include <stdint.h>

#include "video.h"
#include "utils.h"
#include "uart.h"
#include "mm.h"

#define XRGB8888  875713112

#define QEMU_PACKED __attribute__((packed))

/* Hand-rolled byte-swap helpers. We avoid __builtin_bswap*() because at
 * -O0 GCC lowers them into libgcc helpers (__bswapsi2 / __bswapdi2)
 * that the freestanding -nostdlib link cannot resolve. */
static inline uint16_t bswap16(uint16_t x)
{
    return (uint16_t)((x >> 8) | (x << 8));
}

static inline uint32_t bswap32(uint32_t x)
{
    return ((x & 0x000000ffU) << 24) |
           ((x & 0x0000ff00U) <<  8) |
           ((x & 0x00ff0000U) >>  8) |
           ((x & 0xff000000U) >> 24);
}

static inline uint64_t bswap64(uint64_t x)
{
    return ((uint64_t)bswap32((uint32_t)x) << 32) |
           (uint64_t)bswap32((uint32_t)(x >> 32));
}

struct QEMU_PACKED RAMFBCfg {
    uint64_t addr;
    uint32_t fourcc;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
};

/*
 * fw_cfg lives in the MMIO aperture below the RAM base. After paging
 * the kernel runs in the higher half, so these registers must be reached
 * through their linear-map VAs (phys_to_virt), not their raw PAs.
 */
#define FW_CFG_BASE   0x10100000UL
#define FW_CFG_SELECT ((uint16_t *)phys_to_virt(FW_CFG_BASE + 0x08))
#define FW_CFG_DATA   ((uint64_t *)phys_to_virt(FW_CFG_BASE + 0x00))
#define FW_CFG_DMA    ((uint64_t *)phys_to_virt(FW_CFG_BASE + 0x10))

#define FW_CFG_DMA_CTL_ERROR  0x01
#define FW_CFG_DMA_CTL_READ   0x02
#define FW_CFG_DMA_CTL_SKIP   0x04
#define FW_CFG_DMA_CTL_SELECT 0x08
#define FW_CFG_DMA_CTL_WRITE  0x10

#define FW_CFG_FILE_DIR 0x19

struct QEMU_PACKED FWCfgFile {
    uint32_t size;
    uint16_t select;
    uint16_t reserved;
    char     name[56];
};

struct QEMU_PACKED FWCfgFiles {
    uint32_t            count;
    struct FWCfgFile    f[];
};

struct QEMU_PACKED FWCfgDmaAccess {
    uint32_t control;
    uint32_t length;
    uint64_t address;
};

/** ----------------------------------------------------------------------
 * @brief mem_ncmp() – Byte-wise compare of at most @n bytes.
 *
 * Local replacement for libc strncmp(): the kernel is built with
 * -nostdlib and we only need the "stop on first difference or first
 * NUL" semantics for fw_cfg directory walking.
 * @param[in] a First buffer.
 * @param[in] b Second buffer.
 * @param     n Maximum bytes to inspect.
 * @return 0 if equal up to @n bytes (or to a NUL), non-zero otherwise.
 * -------------------------------------------------------------------- */
static int mem_ncmp(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca != cb)
            return (int)ca - (int)cb;
        if (ca == 0)
            return 0;
    }
    return 0;
}

/** ----------------------------------------------------------------------
 * @brief fw_cfg_dma_transfer() – Issue one fw_cfg DMA descriptor.
 *
 * Builds a big-endian FWCfgDmaAccess on the stack, points the device
 * at it via the FW_CFG_DMA MMIO register, then busy-waits on the
 * control field clearing. The device flips control to 0 on success or
 * sets the error bit on failure; we ignore the error bit and let the
 * caller observe by re-reading the destination buffer.
 * QEMU dereferences the descriptor and the data buffer as guest-physical
 * addresses, so both the buffer and the on-stack descriptor are converted
 * from their kernel VAs back to PAs (higher-half paging) before being
 * handed to the device.
 * @param[in,out] address User buffer (read into / written from).
 * @param         length  Bytes to transfer.
 * @param         control fw_cfg control word (already in host byte order).
 * -------------------------------------------------------------------- */
static void fw_cfg_dma_transfer(void *address, uint32_t length,
                                uint32_t control)
{
    struct FWCfgDmaAccess access = {
        .control = bswap32(control),
        .length  = bswap32(length),
        .address = bswap64((uint64_t)virt_to_phys(address)),
    };
    *FW_CFG_DMA = bswap64((uint64_t)virt_to_phys(&access));
    while (bswap32(access.control) & ~FW_CFG_DMA_CTL_ERROR)
        ;
}

/** ----------------------------------------------------------------------
 * @brief fw_cfg_read_entry() – Select entry @e and DMA-read it.
 * @param[out] buf Destination buffer.
 * @param      e   fw_cfg entry id.
 * @param      len Bytes to read.
 * -------------------------------------------------------------------- */
static void fw_cfg_read_entry(void *buf, int e, int len)
{
    uint32_t control = (e << 16) | FW_CFG_DMA_CTL_SELECT |
                       FW_CFG_DMA_CTL_READ;
    fw_cfg_dma_transfer(buf, len, control);
}

/** ----------------------------------------------------------------------
 * @brief fw_cfg_write_entry() – Select entry @e and DMA-write to it.
 * @param[in] buf Source buffer.
 * @param     e   fw_cfg entry id.
 * @param     len Bytes to write.
 * -------------------------------------------------------------------- */
static void fw_cfg_write_entry(void *buf, int e, int len)
{
    uint32_t control = (e << 16) | FW_CFG_DMA_CTL_SELECT |
                       FW_CFG_DMA_CTL_WRITE;
    fw_cfg_dma_transfer(buf, len, control);
}

/** ----------------------------------------------------------------------
 * @brief fw_cfg_find_file() – Look up an fw_cfg file by path name.
 *
 * Reads the directory header, then streams the file entries one by one
 * (the device auto-advances after the directory header read), comparing
 * each name against @name. Returns the first matching entry's selector.
 * @param[in] name Slash-separated fw_cfg file name (e.g. "etc/ramfb").
 * @return Selector id on success, -1 if not found.
 * -------------------------------------------------------------------- */
static int fw_cfg_find_file(const char *name)
{
    uint32_t count = 0;
    fw_cfg_read_entry(&count, FW_CFG_FILE_DIR, sizeof(count));
    count = bswap32(count);
    for (uint32_t i = 0; i < count; i++) {
        struct FWCfgFile file;
        fw_cfg_dma_transfer(&file, sizeof(file), FW_CFG_DMA_CTL_READ);
        if (mem_ncmp(name, file.name, sizeof(file.name)) == 0)
            return bswap16(file.select);
    }
    return -1;
}

#define CACHE_BLOCK_SIZE 64
#define cbo_flush(start)                            \
    ({                                              \
        unsigned long __v = (unsigned long)(start); \
        __asm__ __volatile__(                       \
            "cbo.flush"                             \
            " 0(%0)"                                \
            :                                       \
            : "rK"(__v)                             \
            : "memory");                            \
    })

/** ----------------------------------------------------------------------
 * @brief flush_dcache() – Force pending stores in [addr, addr+len) out.
 *
 * Walks the affected cache lines and issues cbo.flush on each one,
 * with __sync_synchronize() fences before and around the loop so the
 * compiler/CPU cannot reorder the stores around the flush. On QEMU
 * cbo.flush is a no-op (everything is coherent) and the loop is
 * harmless; on OPI-RV2 it is required so the GPU sees the new pixels.
 * @param[in] addr Start byte address.
 * @param     len  Number of bytes covered.
 * -------------------------------------------------------------------- */
static void flush_dcache(void *addr, unsigned long len)
{
    unsigned long start = (unsigned long)addr & ~(CACHE_BLOCK_SIZE - 1);
    __sync_synchronize();
    for (unsigned long line = start;
         line < (unsigned long)addr + len;
         line += CACHE_BLOCK_SIZE) {
        cbo_flush(line);
        __sync_synchronize();
    }
}

/** ----------------------------------------------------------------------
 * @brief video_init() – Register the framebuffer with the host.
 *
 * Builds an etc/ramfb descriptor pointing at FB_BASE and writes it
 * through fw_cfg DMA. After this call QEMU starts sampling FB_BASE
 * each refresh; before it the GTK window stays empty. On non-QEMU
 * builds the SoC framebuffer is already live, but we still keep the
 * call: fw_cfg_find_file() will return -1 and the write becomes a
 * harmless no-op against the absent device window.
 * -------------------------------------------------------------------- */
void video_init(void)
{
#ifdef QEMU
    struct RAMFBCfg cfg = {
        .addr   = bswap64(FB_BASE),
        .fourcc = bswap32(XRGB8888),
        .flags  = bswap32(0),
        .width  = bswap32(FB_WIDTH),
        .height = bswap32(FB_HEIGHT),
        .stride = bswap32(FB_WIDTH * FB_BPP),
    };
    int sel = fw_cfg_find_file("etc/ramfb");
    if (sel < 0)
        return;
    fw_cfg_write_entry(&cfg, sel, sizeof(struct RAMFBCfg));
#endif
}

/** ----------------------------------------------------------------------
 * @brief video_bmp_display() – Center-blit a BMP-shaped buffer to FB.
 *
 * Copies the user-supplied @width x @height pixel array into the
 * framebuffer, centered. Each row is mem_cpy'd then flushed; rows are
 * independent so a timer IRQ between rows is harmless (worst case: one
 * frame of tearing on the next refresh, which the spec accepts).
 * @param[in] bmp_image Pointer to width*height XRGB8888 pixels.
 * @param     width     Pixel width of the source image.
 * @param     height    Pixel height of the source image.
 * -------------------------------------------------------------------- */
void video_bmp_display(unsigned int *bmp_image, int width, int height)
{
    /* FB_BASE is the guest-physical aperture handed to QEMU in video_init();
     * the kernel writes pixels through its linear-map VA (paging). */
    unsigned int *fb = (unsigned int *)phys_to_virt(FB_BASE);
    int start_x = (FB_WIDTH - width) / 2;
    int start_y = (FB_HEIGHT - height) / 2;
    for (int y = 0; y < height; y++) {
        void *dst = fb + (start_y + y) * FB_WIDTH + start_x;
        mem_cpy(dst, bmp_image + y * width,
                (unsigned long)width * sizeof(unsigned int));
        flush_dcache(dst, (unsigned long)width * sizeof(unsigned int));
    }
}
