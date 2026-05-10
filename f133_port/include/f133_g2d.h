#ifndef F133_G2D_H
#define F133_G2D_H

/**
 * @file f133_g2d.h
 * @brief F133 LVGL 显示移植层使用的 G2D 硬件加速封装接口。
 *
 * 本模块只负责封装 `/dev/g2d` 设备和基础 G2D 操作，主要能力包括：
 * - 打开和关闭 G2D 设备节点
 * - 描述物理连续图像 buffer 和矩形区域
 * - 执行硬件 blit 搬运
 * - 执行硬件旋转搬运
 * - 执行硬件填充
 *
 * 注意：本头文件定义的是移植层自己的轻量 API，不直接暴露全志内核
 * `g2d_driver.h`/`g2d_driver_enh.h` 中的大量 ioctl 结构体。具体实现可在
 * `f133_g2d.c` 中包含全志 G2D 头文件，并将本模块的枚举和结构体转换为
 * 内核 ioctl 需要的参数。
 *
 * G2D 只处理硬件搬运和变换。ION 内存申请、cache 刷新、LVGL flush 回调、
 * 三缓冲状态机和 framebuffer 显示切换应分别放在 ION/display 层中。
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 默认 G2D 设备节点路径。
 *
 * F133 Tina Linux 上通常由 `kmod-sunxi-g2d` 创建 `/dev/g2d`。
 */
#define F133_G2D_DEFAULT_DEVICE "/dev/g2d"

/**
 * @brief G2D 图像像素格式。
 *
 * 这里只列出 LVGL framebuffer/G2D 搬运常用格式。实现层需要把这些值转换为
 * 全志 G2D 驱动中的 `g2d_fmt_enh`，例如 `G2D_FORMAT_ARGB8888`、
 * `G2D_FORMAT_RGB565` 等。
 */
typedef enum {
    /** 32 位 ARGB8888，每像素 4 字节，带 alpha 通道。 */
    F133_G2D_FORMAT_ARGB8888,

    /** 32 位 XRGB8888，每像素 4 字节，最高 8 位不作为有效 alpha 使用。 */
    F133_G2D_FORMAT_XRGB8888,

    /** 24 位 RGB888，每像素 3 字节。 */
    F133_G2D_FORMAT_RGB888,

    /** 16 位 RGB565，每像素 2 字节。 */
    F133_G2D_FORMAT_RGB565,
} f133_g2d_format_t;

/**
 * @brief G2D 旋转角度。
 *
 * 旋转方向以 G2D 输出结果为准。具体映射到全志驱动时，需要转换为
 * `G2D_ROT_0`、`G2D_ROT_90`、`G2D_ROT_180`、`G2D_ROT_270`。
 */
typedef enum {
    /** 不旋转。 */
    F133_G2D_ROT_0,

    /** 顺时针或驱动定义方向的 90 度旋转，最终方向由实现层映射决定。 */
    F133_G2D_ROT_90,

    /** 180 度旋转。 */
    F133_G2D_ROT_180,

    /** 270 度旋转。 */
    F133_G2D_ROT_270,
} f133_g2d_rotation_t;

/**
 * @brief 图像中的矩形区域。
 *
 * `x`、`y` 表示区域左上角坐标，`width`、`height` 表示区域宽高，单位均为
 * 像素。该结构既可用于源区域，也可用于目标区域。
 */
typedef struct {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} f133_g2d_rect_t;

/**
 * @brief 可交给 G2D 访问的图像 buffer 描述符。
 *
 * `phys` 必须是 G2D 可访问的物理地址，通常来自 ION 物理连续内存。
 * `width`、`height` 是整张图像的像素尺寸。
 * `stride_bytes` 是一行图像占用的字节数，允许大于 `width * bytes_per_pixel`。
 * `format` 指明像素格式。
 *
 * 在 CPU/LVGL 写入该 buffer 后，提交给 G2D 前必须先通过 ION 层刷新 cache。
 */
typedef struct {
    uintptr_t phys;
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    f133_g2d_format_t format;
} f133_g2d_image_t;

/**
 * @brief G2D 设备上下文。
 *
 * `fd` 是 `/dev/g2d` 的文件描述符。`opened` 用来记录设备是否已经打开，便于
 * `f133_g2d_deinit()` 做成对关闭。
 */
typedef struct {
    int fd;
    bool opened;
} f133_g2d_t;

/**
 * @brief 打开 G2D 设备。
 *
 * @param g2d 待初始化的 G2D 上下文。
 * @param device_path G2D 设备节点路径；传入 NULL 时实现层可使用
 * `F133_G2D_DEFAULT_DEVICE`。
 * @return true 表示打开成功，false 表示参数无效或设备打开失败。
 */
bool f133_g2d_init(f133_g2d_t *g2d, const char *device_path);

/**
 * @brief 关闭 G2D 设备。
 *
 * @param g2d 待关闭的 G2D 上下文。
 */
void f133_g2d_deinit(f133_g2d_t *g2d);

/**
 * @brief 使用 G2D 将源图像区域搬运到目标图像区域。
 *
 * 源图像和目标图像必须位于 G2D 可访问的物理连续内存中。若源 buffer 刚由
 * CPU 写入，调用前必须先刷新 cache。该接口适合普通全帧 copy 或局部 copy。
 *
 * @param g2d 已打开的 G2D 上下文。
 * @param src 源图像描述符。
 * @param src_rect 源图像区域。
 * @param dst 目标图像描述符。
 * @param dst_rect 目标图像区域。
 * @return 0 表示成功，负数表示失败。
 */
int f133_g2d_blit(f133_g2d_t *g2d,
                  const f133_g2d_image_t *src,
                  const f133_g2d_rect_t *src_rect,
                  const f133_g2d_image_t *dst,
                  const f133_g2d_rect_t *dst_rect);

/**
 * @brief 使用 G2D 将源图像区域旋转后搬运到目标图像区域。
 *
 * 该接口用于 LVGL 横竖屏适配、显示方向修正等场景。旋转 90/270 度时，目标
 * 区域的宽高通常应与源区域的高宽互换。
 *
 * @param g2d 已打开的 G2D 上下文。
 * @param src 源图像描述符。
 * @param src_rect 源图像区域。
 * @param dst 目标图像描述符。
 * @param dst_rect 目标图像区域。
 * @param rotation 旋转角度。
 * @return 0 表示成功，负数表示失败。
 */
int f133_g2d_rotate(f133_g2d_t *g2d,
                    const f133_g2d_image_t *src,
                    const f133_g2d_rect_t *src_rect,
                    const f133_g2d_image_t *dst,
                    const f133_g2d_rect_t *dst_rect,
                    f133_g2d_rotation_t rotation);

/**
 * @brief 使用 G2D 填充目标图像中的指定区域。
 *
 * `argb_color` 使用 0xAARRGGBB 表示颜色。`alpha` 为全局透明度，255 表示
 * 完全不透明，0 表示完全透明。具体 alpha 语义由实现层映射到 G2D 驱动。
 *
 * @param g2d 已打开的 G2D 上下文。
 * @param dst 目标图像描述符。
 * @param dst_rect 目标填充区域。
 * @param argb_color ARGB8888 格式颜色值。
 * @param alpha 全局透明度。
 * @return 0 表示成功，负数表示失败。
 */
int f133_g2d_fill(f133_g2d_t *g2d,
                  const f133_g2d_image_t *dst,
                  const f133_g2d_rect_t *dst_rect,
                  uint32_t argb_color,
                  uint8_t alpha);

/**
 * @brief 检查图像描述符是否基本有效。
 *
 * @param image 待检查的图像描述符。
 * @return true 表示物理地址、宽高和 stride 等字段满足基本要求。
 */
bool f133_g2d_image_is_valid(const f133_g2d_image_t *image);

/**
 * @brief 检查矩形区域是否基本有效。
 *
 * @param rect 待检查的矩形区域。
 * @return true 表示宽高均非 0。
 */
bool f133_g2d_rect_is_valid(const f133_g2d_rect_t *rect);

#ifdef __cplusplus
}
#endif

#endif /* F133_G2D_H */
