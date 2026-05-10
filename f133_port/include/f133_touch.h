#ifndef F133_TOUCH_H
#define F133_TOUCH_H

/**
 * @file f133_touch.h
 * @brief F133 LVGL 触摸输入适配层接口。
 *
 * 本模块负责把 Linux input event 触摸设备转换成 LVGL pointer indev。
 * 它会读取 `/dev/input/eventX` 的 ABS/KEY 事件，并结合 F133 显示层中的
 * 物理屏幕分辨率与 G2D 硬件旋转方向，把触摸原始坐标映射为 LVGL 逻辑坐标。
 *
 * 当前实现以单点触摸为主，支持常见事件：
 * - `ABS_X` / `ABS_Y`
 * - `ABS_MT_POSITION_X` / `ABS_MT_POSITION_Y`
 * - `ABS_MT_TRACKING_ID`
 * - `BTN_TOUCH`
 */

#include "f133_display.h"
#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 默认触摸 input 设备节点。
 *
 * 当前板子的触摸设备由用户确认为 `/dev/input/event2`。
 */
#define F133_TOUCH_DEFAULT_EVENT "/dev/input/event2"

/**
 * @brief F133 触摸适配层上下文的不透明声明。
 *
 * 应用层只需要持有该指针用于退出时释放，不应直接访问内部 fd、坐标范围或
 * LVGL indev 状态。
 */
typedef struct f133_touch f133_touch_t;

/**
 * @brief 创建 F133 触摸输入设备。
 *
 * 创建成功后，本模块会：
 * - 打开指定 input event 设备；
 * - 查询触摸 ABS 坐标范围；
 * - 创建 LVGL pointer indev；
 * - 绑定到传入显示上下文对应的 LVGL display；
 * - 根据显示层的硬件旋转方向做坐标变换。
 *
 * @param display 已创建成功的 F133 显示上下文。
 * @param event_path input event 设备路径；传 NULL 时使用 `F133_TOUCH_DEFAULT_EVENT`。
 * @return 成功返回触摸上下文，失败返回 NULL。
 */
f133_touch_t *f133_touch_create(f133_display_t *display, const char *event_path);

/**
 * @brief 销毁 F133 触摸输入设备。
 *
 * 该函数会删除 LVGL indev、关闭 input event fd，并释放上下文内存。调用后
 * `touch` 不可再使用。
 *
 * @param touch 由 `f133_touch_create()` 返回的触摸上下文。
 */
void f133_touch_destroy(f133_touch_t *touch);

/**
 * @brief 获取内部 LVGL indev 指针。
 *
 * 应用层一般不需要直接使用该指针；如果后续需要设置光标、group 或调试输入
 * 行为，可以通过该接口取得。
 *
 * @param touch F133 触摸上下文。
 * @return 成功返回 `lv_indev_t *`，失败返回 NULL。
 */
lv_indev_t *f133_touch_get_lvgl_indev(f133_touch_t *touch);

#ifdef __cplusplus
}
#endif

#endif /* F133_TOUCH_H */
