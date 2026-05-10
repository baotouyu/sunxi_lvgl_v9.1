#include "f133_display.h"
#include "f133_ion.h"
#include "f133_g2d.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#define F133_DISPLAY_LOGE(fmt, ...) \
    fprintf(stderr, "[F133_DISPLAY] ERROR: " fmt "\n", ##__VA_ARGS__)

#define F133_DISPLAY_LOGI(fmt, ...) \
    fprintf(stderr, "[F133_DISPLAY] INFO: " fmt "\n", ##__VA_ARGS__)

/* 检查 LVGL 旋转枚举是否属于 display 层支持的四个标准方向。 */
static bool f133_display_rotation_is_valid(lv_display_rotation_t rotation)
{
    switch(rotation) {
        case LV_DISPLAY_ROTATION_0:
        case LV_DISPLAY_ROTATION_90:
        case LV_DISPLAY_ROTATION_180:
        case LV_DISPLAY_ROTATION_270:
            return true;
        default:
            return false;
    }
}

/* 只允许 G2D 封装层已经支持的常用 framebuffer 像素格式。 */
static bool f133_display_color_format_is_valid(lv_color_format_t color_format)
{
    switch(color_format) {
        case LV_COLOR_FORMAT_RGB565:
        case LV_COLOR_FORMAT_RGB888:
        case LV_COLOR_FORMAT_XRGB8888:
        case LV_COLOR_FORMAT_ARGB8888:
            return true;
        default:
            return false;
    }
}

static bool f133_display_color_format_to_g2d(lv_color_format_t color_format, f133_g2d_format_t *out)
{
    if(out == NULL) {
        return false;
    }

    switch(color_format) {
        case LV_COLOR_FORMAT_RGB565:
            *out = F133_G2D_FORMAT_RGB565;
            return true;
        case LV_COLOR_FORMAT_RGB888:
            *out = F133_G2D_FORMAT_RGB888;
            return true;
        case LV_COLOR_FORMAT_XRGB8888:
            *out = F133_G2D_FORMAT_XRGB8888;
            return true;
        case LV_COLOR_FORMAT_ARGB8888:
            *out = F133_G2D_FORMAT_ARGB8888;
            return true;
        default:
            return false;
    }
}

/* 根据 LVGL 颜色格式换算每像素字节数，后续申请全帧 ION buffer 时会用到。 */
static uint32_t f133_display_color_format_bpp(lv_color_format_t color_format)
{
    switch(color_format) {
        case LV_COLOR_FORMAT_RGB565:
            return 2U;
        case LV_COLOR_FORMAT_RGB888:
            return 3U;
        case LV_COLOR_FORMAT_XRGB8888:
        case LV_COLOR_FORMAT_ARGB8888:
            return 4U;
        default:
            return 0U;
    }
}

static bool f133_display_rotation_swaps_xy(lv_display_rotation_t rotation)
{
    return rotation == LV_DISPLAY_ROTATION_90 || rotation == LV_DISPLAY_ROTATION_270;
}

static bool f133_display_rotation_to_g2d(lv_display_rotation_t rotation, f133_g2d_rotation_t *out)
{
    if(out == NULL) {
        return false;
    }

    /*
     * 这里沿用全志 LVGL8 sunxifb 的硬件旋转方向映射：
     * LVGL 逻辑方向是显示坐标系的旋转描述，G2D flag 是源图像搬运到物理屏时的
     * 旋转动作，两者在 90/270 度上方向相反。
     */
    switch(rotation) {
        case LV_DISPLAY_ROTATION_0:
            *out = F133_G2D_ROT_0;
            return true;
        case LV_DISPLAY_ROTATION_90:
            *out = F133_G2D_ROT_270;
            return true;
        case LV_DISPLAY_ROTATION_180:
            *out = F133_G2D_ROT_180;
            return true;
        case LV_DISPLAY_ROTATION_270:
            *out = F133_G2D_ROT_90;
            return true;
        default:
            return false;
    }
}

static const char *f133_display_fb_path_or_default(const char *fb_path)
{
    return fb_path != NULL ? fb_path : F133_DISPLAY_DEFAULT_FB;
}

static const char *f133_display_g2d_path_or_default(const char *g2d_path)
{
    return g2d_path != NULL ? g2d_path : F133_G2D_DEFAULT_DEVICE;
}

static bool f133_display_mul_u32(uint32_t a, uint32_t b, uint32_t *out)
{
    if(out == NULL) {
        return false;
    }

    if(a != 0U && b > UINT32_MAX / a) {
        return false;
    }

    *out = a * b;
    return true;
}

static bool f133_display_mul_size(size_t a, size_t b, size_t *out)
{
    if(out == NULL) {
        return false;
    }

    if(a != 0U && b > SIZE_MAX / a) {
        return false;
    }

    *out = a * b;
    return true;
}

static uintptr_t f133_display_fb_page_phys(const f133_display_t *display, uint32_t page_index);
static int f133_display_present_back_page(f133_display_t *display);
static void f133_display_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map);

struct f133_display {
    /* 创建 display 时传入的配置快照，后续 flush 和销毁流程都以这里为准。 */
    f133_display_config_t config;

    /* framebuffer 设备文件描述符，对应 /dev/fb0。 */
    int fb_fd;

    /* framebuffer 可变参数，例如分辨率、虚拟分辨率、像素格式等。 */
    struct fb_var_screeninfo vinfo;

    /* framebuffer 固定参数，例如显存物理地址、显存长度、line_length 等。 */
    struct fb_fix_screeninfo finfo;

    /* mmap 后的 framebuffer 用户态虚拟地址。 */
    void *fb_mem;

    /* framebuffer mmap 区域大小，通常等于 finfo.smem_len。 */
    size_t fb_mem_size;

    /* 单个 framebuffer 显示页大小，单位字节，等于 line_length * yres。 */
    size_t fb_page_size;

    /* 当前 framebuffer 可用显示页数量，第一版要求至少 2 页。 */
    uint32_t fb_page_count;

    /* LCD 当前正在扫描的 framebuffer 页索引。 */
    uint32_t fb_front_index;

    /* G2D 下一次要写入的 framebuffer 后台页索引。 */
    uint32_t fb_back_index;

    /* LVGL 逻辑横向分辨率，可能来自 config，也可能来自 fbdev。 */
    uint32_t hor_res;

    /* LVGL 逻辑纵向分辨率，可能来自 config，也可能来自 fbdev。 */
    uint32_t ver_res;

    /* 当前 LVGL 绘制格式每像素字节数，例如 ARGB8888 为 4。 */
    uint32_t bytes_per_pixel;

    /* ION 绘制缓冲单行字节数，第一版按 hor_res * bytes_per_pixel 计算。 */
    uint32_t draw_stride;

    /* 单个全帧 ION 绘制缓冲大小，单位字节。 */
    uint32_t draw_buf_size;

    /* ION 内存分配器上下文，用于申请物理连续全帧 buffer。 */
    f133_ion_t ion;

    /* G2D 设备上下文，用于硬件搬运和旋转。 */
    f133_g2d_t g2d;

    /* ION 全帧绘制缓冲数组；第一版只使用 draw_buf[0]，其余位置为后续并行优化预留。 */
    f133_ion_buffer_t draw_buf[F133_DISPLAY_DEFAULT_BUFFER_COUNT];

    /* LVGL display 对象，flush_cb 通过 user_data 回到本上下文。 */
    lv_display_t *lv_disp;
};

static uintptr_t f133_display_fb_page_phys(const f133_display_t *display, uint32_t page_index)
{
    if(display == NULL || page_index >= display->fb_page_count) {
        return 0U;
    }

    return (uintptr_t)display->finfo.smem_start + (uintptr_t)((size_t)page_index * display->fb_page_size);
}

static int f133_display_present_back_page(f133_display_t *display)
{
    if(display == NULL || display->fb_page_count < 2U) {
        return -EINVAL;
    }

    struct fb_var_screeninfo pan_info = display->vinfo;
    pan_info.yoffset = display->fb_back_index * display->vinfo.yres;

    if(ioctl(display->fb_fd, FBIOPAN_DISPLAY, &pan_info) < 0) {
        return -errno;
    }

    uint32_t old_front = display->fb_front_index;
    display->fb_front_index = display->fb_back_index;
    display->fb_back_index = old_front;
    display->vinfo = pan_info;
    return 0;
}

static void f133_display_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;

    f133_display_t *display = (f133_display_t *)lv_display_get_user_data(disp);
    if(display == NULL) {
        lv_display_flush_ready(disp);
        return;
    }

    /*
     * DIRECT 单缓冲模式下，LVGL 会把脏区画到全屏 ION buffer 的正确位置。
     * 非最后一个脏区只需要释放 LVGL；最后一个脏区到来时，ION buffer 已经是一张
     * 完整的新画面，此时再做整帧 cache flush、G2D 旋转和 framebuffer 翻页。
     */
    if(!lv_display_flush_is_last(disp)) {
        lv_display_flush_ready(disp);
        return;
    }

    f133_ion_flush_cache(&display->ion, &display->draw_buf[0]);

    f133_g2d_format_t g2d_format;
    f133_g2d_rotation_t g2d_rotation;
    if(!f133_display_color_format_to_g2d(display->config.color_format, &g2d_format) ||
       !f133_display_rotation_to_g2d(display->config.rotation, &g2d_rotation)) {
        F133_DISPLAY_LOGE("failed to map LVGL format or rotation to G2D");
        lv_display_flush_ready(disp);
        return;
    }

    uintptr_t dst_phys = f133_display_fb_page_phys(display, display->fb_back_index);
    if(dst_phys == 0U) {
        F133_DISPLAY_LOGE("invalid framebuffer back page physical address");
        lv_display_flush_ready(disp);
        return;
    }

    f133_g2d_image_t src = {
        .phys = display->draw_buf[0].phys,
        .width = display->hor_res,
        .height = display->ver_res,
        .stride_bytes = display->draw_stride,
        .format = g2d_format,
    };
    f133_g2d_rect_t src_rect = {
        .x = 0U,
        .y = 0U,
        .width = display->hor_res,
        .height = display->ver_res,
    };
    f133_g2d_image_t dst = {
        .phys = dst_phys,
        .width = display->vinfo.xres,
        .height = display->vinfo.yres,
        .stride_bytes = display->finfo.line_length,
        .format = g2d_format,
    };
    f133_g2d_rect_t dst_rect = {
        .x = 0U,
        .y = 0U,
        .width = display->vinfo.xres,
        .height = display->vinfo.yres,
    };

    int ret = f133_g2d_rotate(&display->g2d, &src, &src_rect, &dst, &dst_rect, g2d_rotation);
    if(ret < 0) {
        F133_DISPLAY_LOGE("G2D rotate blit failed: ret=%d", ret);
        lv_display_flush_ready(disp);
        return;
    }

    ret = f133_display_present_back_page(display);
    if(ret < 0) {
        F133_DISPLAY_LOGE("FBIOPAN_DISPLAY failed: ret=%d", ret);
    }

    lv_display_flush_ready(disp);
}

static void f133_display_unmap_fb(f133_display_t *display)
{
    if(display == NULL || display->fb_mem == NULL || display->fb_mem_size == 0U) {
        return;
    }

    if(munmap(display->fb_mem, display->fb_mem_size) < 0) {
        F133_DISPLAY_LOGE("failed to unmap framebuffer: addr=%p, size=%lu, errno=%d(%s)",
                          display->fb_mem,
                          (unsigned long)display->fb_mem_size,
                          errno,
                          strerror(errno));
    }

    display->fb_mem = NULL;
    display->fb_mem_size = 0U;
}

static void f133_display_close_fb(f133_display_t *display)
{
    if(display == NULL || display->fb_fd < 0) {
        return;
    }

    close(display->fb_fd);
    display->fb_fd = -1;
}

void f133_display_config_default(f133_display_config_t *config)
{
    if(config == NULL) {
        return;
    }

    /*
     * 默认值固定服务于 F133 三缓冲方案：
     * - 分辨率填 0，后续 create 阶段从 fbdev 自动获取；
     * - 默认 ARGB8888，对应当前 lv_conf.h 的 LV_COLOR_DEPTH 32；
     * - buffer_count 固定为 3，用于 LVGL/G2D/屏幕显示流水线。
     */
    *config = (f133_display_config_t) {
        .fb_path = F133_DISPLAY_DEFAULT_FB,
        .g2d_path = F133_G2D_DEFAULT_DEVICE,
        .hor_res = 0U,
        .ver_res = 0U,
        .rotation = LV_DISPLAY_ROTATION_0,
        .color_format = LV_COLOR_FORMAT_ARGB8888,
        .buffer_count = F133_DISPLAY_DEFAULT_BUFFER_COUNT,
    };
}

bool f133_display_config_is_valid(const f133_display_config_t *config)
{
    if(config == NULL) {
        return false;
    }

    /* 本移植层按三缓冲流水线实现，第一版不支持 1/2 buffer 或更多 buffer。 */
    if(config->buffer_count != F133_DISPLAY_DEFAULT_BUFFER_COUNT) {
        return false;
    }

    if(!f133_display_rotation_is_valid(config->rotation)) {
        return false;
    }

    if(!f133_display_color_format_is_valid(config->color_format)) {
        return false;
    }

    /* 分辨率要么都交给 fbdev 自动获取，要么同时由调用者明确指定。 */
    if((config->hor_res == 0U) != (config->ver_res == 0U)) {
        return false;
    }

    return true;
}

/*
 * 创建 F133 显示上下文。
 *
 * 当前实现采用第一版稳定方案：1 块 ION/CMA 全帧绘制缓冲 + framebuffer
 * front/back 双页 + G2D 整帧硬件旋转。LVGL 使用 DIRECT 模式，只把脏区画到
 * ION 全帧缓冲的正确位置；一帧最后一个 flush 到来时，再由 G2D 把整帧旋转
 * 到 framebuffer 后台页，并通过 FBIOPAN_DISPLAY 翻页。
 *
 * 初始化步骤：
 * 1. 先确认配置是否合法，路径为空时补默认 /dev/fb0 和 /dev/g2d；
 * 2. 申请并初始化 f133_display_t，上下文里的 fd 先置为 -1，方便失败路径统一清理；
 * 3. 打开 framebuffer 设备，读取固定参数和可变参数；
 * 4. 根据配置或 fbdev 返回值确定 LVGL 逻辑分辨率；
 * 5. 根据颜色格式计算每像素字节数，并计算单行跨度和单帧缓冲大小；
 * 6. 校验 framebuffer 显存大小和行跨度，再 mmap 到用户态；
 * 7. 检查 framebuffer 是否至少有 2 个可翻页显示页，并计算 front/back 页索引；
 * 8. 初始化 ION，申请 1 块全帧 ION/CMA draw buffer；
 * 9. 初始化 G2D，创建 LVGL display，注册 flush_cb；
 * 10. 任意步骤失败都调用 f133_display_destroy()，避免 fd、mmap、ION 或 LVGL 泄漏。
 */
f133_display_t *f133_display_create(const f133_display_config_t *config)
{
    f133_display_config_t actual_config;
    f133_display_t *display;
    const char *fb_path;
    uint32_t fb_bytes_per_pixel;
    uint32_t min_fb_stride;
    size_t min_fb_size;
    size_t min_double_fb_size;

    /*
     * 允许调用者传 NULL，表示完全使用默认配置。
     * 如果调用者传入结构体，但路径字段为 NULL，则只补路径默认值，不覆盖
     * 调用者设置的分辨率、旋转和颜色格式。
     */
    if(config == NULL) {
        f133_display_config_default(&actual_config);
    }
    else {
        actual_config = *config;
        actual_config.fb_path = f133_display_fb_path_or_default(actual_config.fb_path);
        actual_config.g2d_path = f133_display_g2d_path_or_default(actual_config.g2d_path);
    }

    /* 创建前先做轻量参数检查，避免后面打开设备后才发现配置明显错误。 */
    if(!f133_display_config_is_valid(&actual_config)) {
        F133_DISPLAY_LOGE("invalid display config; cannot create display");
        return NULL;
    }

    /*
     * calloc 会把结构体清零，ION/G2D/LVGL 指针类字段默认就是空状态。
     * fb_fd 不是 0 表示无效，所以 calloc 后需要单独改成 -1。
     */
    display = (f133_display_t *)calloc(1, sizeof(*display));
    if(display == NULL) {
        F133_DISPLAY_LOGE("failed to allocate display context");
        return NULL;
    }

    display->fb_fd = -1;
    display->config = actual_config;
    fb_path = f133_display_fb_path_or_default(display->config.fb_path);

    /*
     * 打开 framebuffer 设备。
     * 这里使用 O_RDWR，因为后续 mmap 和 pan/display 切换通常需要读写权限。
     */
    display->fb_fd = open(fb_path, O_RDWR);
    if(display->fb_fd < 0) {
        F133_DISPLAY_LOGE("failed to open framebuffer: path=%s, errno=%d(%s)",
                          fb_path, errno, strerror(errno));
        f133_display_destroy(display);
        return NULL;
    }

    /*
     * 固定参数包含显存物理地址、显存长度和 line_length。
     * 后面 G2D 输出到屏幕时，会使用 smem_start 作为目标物理地址。
     */
    if(ioctl(display->fb_fd, FBIOGET_FSCREENINFO, &display->finfo) < 0) {
        F133_DISPLAY_LOGE("failed to read framebuffer fixed info: path=%s, errno=%d(%s)",
                          fb_path, errno, strerror(errno));
        f133_display_destroy(display);
        return NULL;
    }

    /*
     * 可变参数包含当前屏幕分辨率、虚拟分辨率、像素位宽等。
     * 如果配置中 hor_res/ver_res 为 0，就使用这里读到的 xres/yres。
     */
    if(ioctl(display->fb_fd, FBIOGET_VSCREENINFO, &display->vinfo) < 0) {
        F133_DISPLAY_LOGE("failed to read framebuffer variable info: path=%s, errno=%d(%s)",
                          fb_path, errno, strerror(errno));
        f133_display_destroy(display);
        return NULL;
    }

    /*
     * 分辨率支持两种来源：
     * - config 显式指定：用于 LVGL 逻辑分辨率和硬件旋转场景；
     * - config 填 0：直接使用 fbdev 当前物理分辨率。
     */
    if(actual_config.hor_res != 0U && actual_config.ver_res != 0U) {
        display->hor_res = actual_config.hor_res;
        display->ver_res = actual_config.ver_res;
    }
    else if(f133_display_rotation_swaps_xy(actual_config.rotation)) {
        display->hor_res = display->vinfo.yres;
        display->ver_res = display->vinfo.xres;
    }
    else {
        display->hor_res = display->vinfo.xres;
        display->ver_res = display->vinfo.yres;
    }
    display->bytes_per_pixel = f133_display_color_format_bpp(actual_config.color_format);

    /* 分辨率或像素字节数为 0 时，后续无法申请有效的全帧 buffer。 */
    if(display->hor_res == 0U || display->ver_res == 0U || display->bytes_per_pixel == 0U) {
        F133_DISPLAY_LOGE("invalid display parameters: hor_res=%u, ver_res=%u, bytes_per_pixel=%u",
                          display->hor_res, display->ver_res, display->bytes_per_pixel);
        f133_display_destroy(display);
        return NULL;
    }

    /*
     * 第一版 draw_stride 按 LVGL 逻辑宽度 * 每像素字节数计算。
     * 这里不直接使用 fbdev 的 line_length，因为 ION 绘制缓冲和 framebuffer
     * 目标缓冲是两类内存；后续 G2D 搬运时会分别描述源 stride 和目标 stride。
     */
    if(!f133_display_mul_u32(display->hor_res, display->bytes_per_pixel, &display->draw_stride) ||
       !f133_display_mul_u32(display->draw_stride, display->ver_res, &display->draw_buf_size)) {
        F133_DISPLAY_LOGE("draw buffer size overflow: hor_res=%u, ver_res=%u, bytes_per_pixel=%u",
                          display->hor_res, display->ver_res, display->bytes_per_pixel);
        f133_display_destroy(display);
        return NULL;
    }

    /*
     * framebuffer 是 G2D 最终写入的目标，因此这里先检查 fbdev 返回的显存描述：
     * - smem_start 后续会作为 G2D 目标物理地址；
     * - smem_len 是 mmap 的长度；
     * - line_length 是目标图像 stride，必须能覆盖一行屏幕像素。
     */
    if(display->finfo.smem_start == 0U || display->finfo.smem_len == 0U || display->finfo.line_length == 0U) {
        F133_DISPLAY_LOGE("invalid framebuffer memory info: smem_start=0x%lx, smem_len=%u, line_length=%u",
                          (unsigned long)display->finfo.smem_start,
                          display->finfo.smem_len,
                          display->finfo.line_length);
        f133_display_destroy(display);
        return NULL;
    }

    if(display->vinfo.bits_per_pixel == 0U || (display->vinfo.bits_per_pixel % 8U) != 0U) {
        F133_DISPLAY_LOGE("unsupported framebuffer bits_per_pixel: %u", display->vinfo.bits_per_pixel);
        f133_display_destroy(display);
        return NULL;
    }

    fb_bytes_per_pixel = display->vinfo.bits_per_pixel / 8U;
    if(fb_bytes_per_pixel != display->bytes_per_pixel) {
        F133_DISPLAY_LOGE("framebuffer pixel size mismatch: fb_bpp=%u, draw_bytes_per_pixel=%u",
                          display->vinfo.bits_per_pixel,
                          display->bytes_per_pixel);
        f133_display_destroy(display);
        return NULL;
    }

    if(!f133_display_mul_u32(display->vinfo.xres, fb_bytes_per_pixel, &min_fb_stride) ||
       display->finfo.line_length < min_fb_stride) {
        F133_DISPLAY_LOGE("invalid framebuffer stride: line_length=%u, min_stride=%u",
                          display->finfo.line_length,
                          min_fb_stride);
        f133_display_destroy(display);
        return NULL;
    }

    if(!f133_display_mul_size((size_t)display->finfo.line_length, (size_t)display->vinfo.yres, &min_fb_size) ||
       (size_t)display->finfo.smem_len < min_fb_size) {
        F133_DISPLAY_LOGE("invalid framebuffer memory size: smem_len=%u, min_size=%lu",
                          display->finfo.smem_len,
                          (unsigned long)min_fb_size);
        f133_display_destroy(display);
        return NULL;
    }

    display->fb_page_size = min_fb_size;
    display->fb_page_count = (uint32_t)((size_t)display->finfo.smem_len / display->fb_page_size);

    if(display->vinfo.yres_virtual < display->vinfo.yres * 2U ||
       display->fb_page_count < 2U ||
       !f133_display_mul_size(display->fb_page_size, 2U, &min_double_fb_size) ||
       (size_t)display->finfo.smem_len < min_double_fb_size) {
        F133_DISPLAY_LOGE("framebuffer double buffering is not available: yres=%u, yres_virtual=%u, smem_len=%u, page_size=%lu",
                          display->vinfo.yres,
                          display->vinfo.yres_virtual,
                          display->finfo.smem_len,
                          (unsigned long)display->fb_page_size);
        f133_display_destroy(display);
        return NULL;
    }

    if(display->vinfo.yres == 0U ||
       display->vinfo.yoffset % display->vinfo.yres != 0U ||
       display->vinfo.yoffset / display->vinfo.yres >= display->fb_page_count) {
        F133_DISPLAY_LOGE("invalid framebuffer yoffset for page flip: yoffset=%u, yres=%u, page_count=%u",
                          display->vinfo.yoffset,
                          display->vinfo.yres,
                          display->fb_page_count);
        f133_display_destroy(display);
        return NULL;
    }

    display->fb_front_index = display->vinfo.yoffset / display->vinfo.yres;
    display->fb_back_index = display->fb_front_index == 0U ? 1U : 0U;

    /*
     * 映射 framebuffer 显存到用户态。
     * 当前阶段只是先保存映射地址，后续调试或 CPU fallback 可以直接访问；
     * 真正的刷新路径仍计划优先让 G2D 写入 finfo.smem_start 对应的物理显存。
     */
    display->fb_mem_size = display->finfo.smem_len;
    display->fb_mem = mmap(NULL, display->fb_mem_size, PROT_READ | PROT_WRITE, MAP_SHARED, display->fb_fd, 0);
    if(display->fb_mem == MAP_FAILED) {
        F133_DISPLAY_LOGE("failed to mmap framebuffer: size=%lu, errno=%d(%s)",
                          (unsigned long)display->fb_mem_size,
                          errno,
                          strerror(errno));
        display->fb_mem = NULL;
        display->fb_mem_size = 0U;
        f133_display_destroy(display);
        return NULL;
    }

    /*
     * 申请一块 ION/CMA 全帧绘制缓冲。
     * LVGL 通过 virt 写像素，G2D 通过 phys 读同一块物理连续内存。
     */
    if(!f133_ion_init(&display->ion)) {
        F133_DISPLAY_LOGE("failed to initialize ION allocator");
        f133_display_destroy(display);
        return NULL;
    }

    if(!f133_ion_alloc(&display->ion, display->draw_buf_size, &display->draw_buf[0])) {
        F133_DISPLAY_LOGE("failed to allocate ION draw buffer: size=%u", display->draw_buf_size);
        f133_display_destroy(display);
        return NULL;
    }

    /* 打开 G2D 设备，后续 flush_cb 中用它执行整帧 rotate blit。 */
    if(!f133_g2d_init(&display->g2d, display->config.g2d_path)) {
        F133_DISPLAY_LOGE("failed to initialize G2D device");
        f133_display_destroy(display);
        return NULL;
    }

    /*
     * 创建 LVGL display。
     * 注意：这里不调用 lv_display_set_rotation()，因为旋转由 G2D 硬件完成；
     * LVGL 看到的是已经按 config.rotation 推导出的逻辑分辨率。
     */
    display->lv_disp = lv_display_create((int32_t)display->hor_res, (int32_t)display->ver_res);
    if(display->lv_disp == NULL) {
        F133_DISPLAY_LOGE("failed to create LVGL display");
        f133_display_destroy(display);
        return NULL;
    }

    lv_display_set_color_format(display->lv_disp, display->config.color_format);
    lv_display_set_user_data(display->lv_disp, display);
    lv_display_set_flush_cb(display->lv_disp, f133_display_flush_cb);
    /*
     * DIRECT 单缓冲模式：buffer 是完整屏幕大小，但 LVGL 仍只重绘脏区。
     * 这样 draw_buf[0] 始终保存一张完整未旋转画面，适合 G2D 在最后 flush 时
     * 整帧旋转到 framebuffer 后台页。
     */
    lv_display_set_buffers(display->lv_disp,
                           display->draw_buf[0].virt,
                           NULL,
                           display->draw_buf_size,
                           LV_DISPLAY_RENDER_MODE_DIRECT);

    /* 记录关键尺寸，方便在板端确认 fbdev、LVGL 逻辑分辨率和 ARGB8888 缓冲大小是否一致。 */
    F133_DISPLAY_LOGI("display ready: path=%s, screen=%ux%u, lvgl=%ux%u, draw_stride=%u, draw_buf_size=%u, fb_stride=%u, fb_mem_size=%lu, front=%u, back=%u",
                      fb_path,
                      display->vinfo.xres,
                      display->vinfo.yres,
                      display->hor_res,
                      display->ver_res,
                      display->draw_stride,
                      display->draw_buf_size,
                      display->finfo.line_length,
                      (unsigned long)display->fb_mem_size,
                      display->fb_front_index,
                      display->fb_back_index);

    return display;
}

void f133_display_destroy(f133_display_t *display)
{
    if(display == NULL) {
        return;
    }

    if(display->lv_disp != NULL) {
        lv_display_delete(display->lv_disp);
        display->lv_disp = NULL;
    }

    f133_g2d_deinit(&display->g2d);

    for(uint32_t i = 0U; i < F133_DISPLAY_DEFAULT_BUFFER_COUNT; i++) {
        f133_ion_free(&display->ion, &display->draw_buf[i]);
    }
    f133_ion_deinit(&display->ion);

    f133_display_unmap_fb(display);
    f133_display_close_fb(display);
    free(display);
}

lv_display_t *f133_display_get_lvgl_display(f133_display_t *display)
{
    return display != NULL ? display->lv_disp : NULL;
}

uint32_t f133_display_get_screen_width(f133_display_t *display)
{
    return display != NULL ? display->vinfo.xres : 0U;
}

uint32_t f133_display_get_screen_height(f133_display_t *display)
{
    return display != NULL ? display->vinfo.yres : 0U;
}

lv_display_rotation_t f133_display_get_rotation(f133_display_t *display)
{
    return display != NULL ? display->config.rotation : LV_DISPLAY_ROTATION_0;
}

bool f133_display_wait_flush_done(f133_display_t *display, uint32_t timeout_ms)
{
    (void)timeout_ms;

    return display != NULL;
}
