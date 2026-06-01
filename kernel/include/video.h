#ifndef __VIDEO_H__
#define __VIDEO_H__

/*
 * QEMU ramfb / OPI-RV2 raw framebuffer driver.
 *
 * The kernel registers a 1920x1080 XRGB8888 framebuffer through QEMU's
 * fw_cfg "etc/ramfb" entry; on real hardware the same FB_BASE points to
 * the SoC's physical framebuffer aperture. The bottom-half user-visible
 * API is a single blit that centers an arbitrary BMP-shaped buffer.
 */

#ifdef QEMU
#define FB_BASE     0x87000000UL
#else
#define FB_BASE     0x7f700000UL
#endif

#define FB_WIDTH    1920
#define FB_HEIGHT   1080
#define FB_BPP      4

void video_init(void);
void video_bmp_display(unsigned int *bmp_image, int width, int height);

#endif /* __VIDEO_H__ */
