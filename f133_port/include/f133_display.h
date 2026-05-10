#ifndef F133_DISPLAY_H
#define F133_DISPLAY_H

/**
 * @file f133_display.h
 * @brief F133 LVGL 显示移植层接口。
 *
 * 本模块位于应用层 `main.c` 和底层 ION/G2D/fbdev 之间，负责把 F133 平台
 * 相关的显示资源封装成一个 LVGL display。建议由本模块集中管理：
 * - LVGL display 创建与销毁
 * - framebuffer 设备打开和显示参数获取
 * - ION 全帧绘制缓冲分配
 * - 三缓冲状态管理
 * - LVGL flush 回调
 * - G2D 搬运/旋转任务提交
 * - framebuffer pan/display 切换
 *
 * `main.c` 不应直接关心 ION 物理地址、G2D ioctl、cache 刷新或三缓冲状态机。
 * 应用层只需要创建 display、启动 LVGL UI，然后周期性调用 `lv_timer_handler()`。
 *
 * 注意：本头文件只暴露显示移植层的外部 API。具体的 ION 申请逻辑放在
 * `f133_ion.c`，G2D 命令封装放在 `f133_g2d.c`，不要把底层细节泄漏到应用层。
 */

#include <stdbool.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 默认 framebuffer 设备节点路径。
 *
 * F133 Tina Linux 上常用主屏 framebuffer 为 `/dev/fb0`。
 */
#define F133_DISPLAY_DEFAULT_FB "/dev/fb0"

/**
 * @brief 默认全帧缓冲数量。
 *
 * 第一版稳定方案中的“三缓冲”由 1 块 ION 全帧绘制缓冲，加 framebuffer
 * 驱动内部的 front/back 双页组成。后续如需更强并行，可再扩展为多块 ION
 * 绘制缓冲。
 */
#define F133_DISPLAY_DEFAULT_BUFFER_COUNT 3U

/**
 * @brief F133 显示移植层上下文的不透明声明。
 *
 * 具体字段应只在 `f133_display.c` 内部定义。应用层通过公开 API 持有和释放
 * 该对象，不直接访问内部状态。
 */
typedef struct f133_display f133_display_t;

/**
 * @brief F133 显示移植层配置。
 *
 * 该结构用于描述创建 LVGL display 所需的平台参数。调用者可以先调用
 * `f133_display_config_default()` 填充默认值，再按需修改分辨率、旋转方向、
 * 设备路径等字段。
 *
 * 本移植层固定按 F133 三缓冲 + G2D 硬件旋转方案实现，因此不再提供
 * “是否启用 G2D 旋转”和“是否启用三缓冲”的开关；这些能力由实现层默认开启。
 */
typedef struct {
    /** framebuffer 设备路径；为 NULL 时使用 `F133_DISPLAY_DEFAULT_FB`。 */
    const char *fb_path;

    /** G2D 设备路径；为 NULL 时由 G2D 层使用默认 `/dev/g2d`。 */
    const char *g2d_path;

    /** LVGL 逻辑横向分辨率，单位为像素。为 0 时实现层可从 fbdev 获取。 */
    uint32_t hor_res;

    /** LVGL 逻辑纵向分辨率，单位为像素。为 0 时实现层可从 fbdev 获取。 */
    uint32_t ver_res;

    /** LVGL 显示旋转方向，使用 `LV_DISPLAY_ROTATION_0/90/180/270`。 */
    lv_display_rotation_t rotation;

    /** LVGL 绘制缓冲颜色格式，例如 `LV_COLOR_FORMAT_ARGB8888` 或 RGB565。 */
    lv_color_format_t color_format;

    /** 逻辑全帧缓冲数量；当前稳定方案固定为 3。 */
    uint32_t buffer_count;
} f133_display_config_t;

/**
 * @brief 填充一份默认显示配置。
 *
 * 默认配置应面向 F133 framebuffer + ION + G2D 方案，例如默认使用 `/dev/fb0`、
 * `/dev/g2d`、ARGB8888、三缓冲和 0 度旋转。具体默认分辨率可由实现层决定，
 * 或保留为 0 表示从 framebuffer 查询。
 *
 * @param config 待填充的配置结构体。
 */
void f133_display_config_default(f133_display_config_t *config);

/**
 * @brief 检查显示配置是否满足基本要求。
 *
 * 该函数只做轻量参数检查，例如 buffer 数量、颜色格式和分辨率组合是否合法。
 * 设备节点是否存在、ION/G2D 是否能打开，应在 `f133_display_create()` 中处理。
 *
 * @param config 待检查的配置结构体。
 * @return true 表示配置通过基本检查，false 表示配置无效。
 */
bool f133_display_config_is_valid(const f133_display_config_t *config);

/**
 * @brief 创建 F133 LVGL display。
 *
 * 成功后，本模块会完成 framebuffer、ION buffer、G2D 和 LVGL display 的初始化，
 * 并注册对应的 LVGL flush 回调。调用者拿到返回对象后，可以通过
 * `f133_display_get_lvgl_display()` 获取底层 `lv_display_t *`。
 *
 * @param config 显示配置；传入 NULL 时实现层可使用默认配置。
 * @return 成功返回显示上下文指针，失败返回 NULL。
 */
f133_display_t *f133_display_create(const f133_display_config_t *config);

/**
 * @brief 销毁 F133 LVGL display。
 *
 * 该函数应释放创建 display 时申请的所有资源，包括 LVGL display、ION buffer、
 * G2D 设备、framebuffer 映射和工作线程等。调用后 `display` 不可再使用。
 *
 * @param display 由 `f133_display_create()` 返回的显示上下文。
 */
void f133_display_destroy(f133_display_t *display);

/**
 * @brief 获取内部 LVGL display 指针。
 *
 * 应用层一般只在需要设置 LVGL 显示属性或调试时使用该指针。普通 UI 创建流程
 * 可以不直接操作它。
 *
 * @param display F133 显示上下文。
 * @return 成功返回 `lv_display_t *`，失败返回 NULL。
 */
lv_display_t *f133_display_get_lvgl_display(f133_display_t *display);

/**
 * @brief 获取 framebuffer 物理屏幕横向分辨率。
 *
 * 该值来自 fbdev 的 `xres`，表示 LCD 实际扫描的横向像素数。它与 LVGL 逻辑
 * 分辨率不同：当启用 90/270 度硬件旋转时，LVGL 的 hor/ver 会交换，但这里
 * 仍返回真实屏幕宽度。
 *
 * @param display F133 显示上下文。
 * @return 成功返回屏幕宽度，失败返回 0。
 */
uint32_t f133_display_get_screen_width(f133_display_t *display);

/**
 * @brief 获取 framebuffer 物理屏幕纵向分辨率。
 *
 * 该值来自 fbdev 的 `yres`，表示 LCD 实际扫描的纵向像素数。触摸适配层用它
 * 把 input event 原始坐标先映射到物理屏幕坐标，再根据硬件旋转方向换算到
 * LVGL 逻辑坐标。
 *
 * @param display F133 显示上下文。
 * @return 成功返回屏幕高度，失败返回 0。
 */
uint32_t f133_display_get_screen_height(f133_display_t *display);

/**
 * @brief 获取当前显示配置中的硬件旋转方向。
 *
 * 该方向由 `f133_display_config_t.rotation` 指定，显示层用它配置 G2D 旋转，
 * 触摸层也必须使用同一个方向做坐标变换，才能保证点击位置和画面一致。
 *
 * @param display F133 显示上下文。
 * @return 成功返回当前旋转方向，失败返回 `LV_DISPLAY_ROTATION_0`。
 */
lv_display_rotation_t f133_display_get_rotation(f133_display_t *display);

/**
 * @brief 主动等待当前显示刷新完成。
 *
 * 如果后续实现了 G2D 工作线程或异步刷新队列，该接口可用于退出前等待队列清空，
 * 或用于测试时确认当前帧已经完成搬运和显示切换。
 *
 * @param display F133 显示上下文。
 * @param timeout_ms 超时时间，单位毫秒；0 表示不等待或由实现层定义为立即检查。
 * @return true 表示刷新已完成，false 表示超时或参数无效。
 */
bool f133_display_wait_flush_done(f133_display_t *display, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* F133_DISPLAY_H */
