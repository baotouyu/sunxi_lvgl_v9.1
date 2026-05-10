#include "f133_g2d.h"

#include "g2d_driver_enh.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static bool f133_g2d_is_ready(const f133_g2d_t *g2d)
{
    return g2d != NULL && g2d->opened && g2d->fd >= 0;
}

static bool f133_g2d_format_to_driver(f133_g2d_format_t format, g2d_fmt_enh *out)
{
    if(out == NULL) return false;

    switch(format) {
        case F133_G2D_FORMAT_ARGB8888:
            *out = G2D_FORMAT_ARGB8888;
            return true;
        case F133_G2D_FORMAT_XRGB8888:
            *out = G2D_FORMAT_XRGB8888;
            return true;
        case F133_G2D_FORMAT_RGB888:
            *out = G2D_FORMAT_RGB888;
            return true;
        case F133_G2D_FORMAT_RGB565:
            *out = G2D_FORMAT_RGB565;
            return true;
        default:
            return false;
    }
}

static bool f133_g2d_rotation_to_driver(f133_g2d_rotation_t rotation, g2d_blt_flags_h *out)
{
    if(out == NULL) return false;

    switch(rotation) {
        case F133_G2D_ROT_0:
            *out = G2D_ROT_0;
            return true;
        case F133_G2D_ROT_90:
            *out = G2D_ROT_90;
            return true;
        case F133_G2D_ROT_180:
            *out = G2D_ROT_180;
            return true;
        case F133_G2D_ROT_270:
            *out = G2D_ROT_270;
            return true;
        default:
            return false;
    }
}

static uint32_t f133_g2d_format_bpp(f133_g2d_format_t format)
{
    switch(format) {
        case F133_G2D_FORMAT_ARGB8888:
        case F133_G2D_FORMAT_XRGB8888:
            return 4U;
        case F133_G2D_FORMAT_RGB888:
            return 3U;
        case F133_G2D_FORMAT_RGB565:
            return 2U;
        default:
            return 0U;
    }
}

static bool f133_g2d_rect_in_image(const f133_g2d_image_t *image, const f133_g2d_rect_t *rect)
{
    if(!f133_g2d_image_is_valid(image) || !f133_g2d_rect_is_valid(rect)) return false;

    uint64_t x2 = (uint64_t)rect->x + rect->width;
    uint64_t y2 = (uint64_t)rect->y + rect->height;
    return x2 <= image->width && y2 <= image->height;
}

static uint32_t f133_g2d_stride_pixels(const f133_g2d_image_t *image)
{
    uint32_t bpp = f133_g2d_format_bpp(image->format);
    if(bpp == 0U) return 0U;
    return image->stride_bytes / bpp;
}

static bool f133_g2d_fill_image(g2d_image_enh *out, const f133_g2d_image_t *image, const f133_g2d_rect_t *rect)
{
    if(out == NULL || !f133_g2d_rect_in_image(image, rect)) return false;

    g2d_fmt_enh format;
    if(!f133_g2d_format_to_driver(image->format, &format)) return false;

    uint32_t stride_pixels = f133_g2d_stride_pixels(image);
    if(stride_pixels == 0U) return false;

    memset(out, 0, sizeof(*out));
    out->format = format;
    out->laddr[0] = image->phys;
    out->width = stride_pixels;
    out->height = image->height;
    out->clip_rect.x = (int32_t)rect->x;
    out->clip_rect.y = (int32_t)rect->y;
    out->clip_rect.w = rect->width;
    out->clip_rect.h = rect->height;
    out->mode = G2D_GLOBAL_ALPHA;
    out->alpha = 255U;
    out->color = 0xee8899U;
    out->use_phy_addr = 1U;
    return true;
}

bool f133_g2d_rect_is_valid(const f133_g2d_rect_t *rect)
{
    return rect != NULL && rect->width != 0U && rect->height != 0U;
}

bool f133_g2d_image_is_valid(const f133_g2d_image_t *image)
{
    if(image == NULL || image->phys == 0U || image->width == 0U || image->height == 0U) return false;

    uint32_t bpp = f133_g2d_format_bpp(image->format);
    if(bpp == 0U || image->stride_bytes == 0U || image->stride_bytes % bpp != 0U) return false;

    uint64_t min_stride = (uint64_t)image->width * bpp;
    return image->stride_bytes >= min_stride;
}

bool f133_g2d_init(f133_g2d_t *g2d, const char *device_path)
{
    if(g2d == NULL) return false;
    if(f133_g2d_is_ready(g2d)) return true;

    const char *path = device_path != NULL ? device_path : F133_G2D_DEFAULT_DEVICE;
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if(fd < 0) {
        fprintf(stderr, "f133_g2d: open %s failed: %s\n", path, strerror(errno));
        g2d->fd = -1;
        g2d->opened = false;
        return false;
    }

    g2d->fd = fd;
    g2d->opened = true;
    return true;
}

void f133_g2d_deinit(f133_g2d_t *g2d)
{
    if(g2d == NULL) return;

    if(g2d->opened && g2d->fd >= 0) {
        close(g2d->fd);
    }

    g2d->fd = -1;
    g2d->opened = false;
}

int f133_g2d_blit(f133_g2d_t *g2d,
                  const f133_g2d_image_t *src,
                  const f133_g2d_rect_t *src_rect,
                  const f133_g2d_image_t *dst,
                  const f133_g2d_rect_t *dst_rect)
{
    if(!f133_g2d_is_ready(g2d)) return -EINVAL;

    g2d_blt_h info;
    memset(&info, 0, sizeof(info));
    info.flag_h = G2D_ROT_0;

    if(!f133_g2d_fill_image(&info.src_image_h, src, src_rect) ||
       !f133_g2d_fill_image(&info.dst_image_h, dst, dst_rect)) {
        return -EINVAL;
    }

    if(ioctl(g2d->fd, G2D_CMD_BITBLT_H, (uintptr_t)&info) < 0) {
        return -errno;
    }

    return 0;
}

int f133_g2d_rotate(f133_g2d_t *g2d,
                    const f133_g2d_image_t *src,
                    const f133_g2d_rect_t *src_rect,
                    const f133_g2d_image_t *dst,
                    const f133_g2d_rect_t *dst_rect,
                    f133_g2d_rotation_t rotation)
{
    if(!f133_g2d_is_ready(g2d)) return -EINVAL;

    g2d_blt_h info;
    memset(&info, 0, sizeof(info));
    if(!f133_g2d_rotation_to_driver(rotation, &info.flag_h)) return -EINVAL;

    if(!f133_g2d_fill_image(&info.src_image_h, src, src_rect) ||
       !f133_g2d_fill_image(&info.dst_image_h, dst, dst_rect)) {
        return -EINVAL;
    }

    if(ioctl(g2d->fd, G2D_CMD_BITBLT_H, (uintptr_t)&info) < 0) {
        return -errno;
    }

    return 0;
}

int f133_g2d_fill(f133_g2d_t *g2d,
                  const f133_g2d_image_t *dst,
                  const f133_g2d_rect_t *dst_rect,
                  uint32_t argb_color,
                  uint8_t alpha)
{
    if(!f133_g2d_is_ready(g2d)) return -EINVAL;

    g2d_fillrect_h info;
    memset(&info, 0, sizeof(info));
    if(!f133_g2d_fill_image(&info.dst_image_h, dst, dst_rect)) return -EINVAL;

    info.dst_image_h.mode = G2D_PIXEL_ALPHA;
    info.dst_image_h.color = argb_color;
    info.dst_image_h.alpha = alpha;

    if(ioctl(g2d->fd, G2D_CMD_FILLRECT_H, (uintptr_t)&info) < 0) {
        return -errno;
    }

    return 0;
}
