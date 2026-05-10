#include "lvgl/lvgl.h"
#include "lvgl/demos/lv_demos.h"

#include "f133_display.h"
#include "f133_touch.h"

#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

static uint32_t app_tick_get_ms(void)
{
    struct timespec ts;

    if(clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0U;
    }

    return (uint32_t)((uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL);
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(app_tick_get_ms);

    /*
     * F133 显示移植层会完成：
     * - 打开 /dev/fb0 并检查 framebuffer 双页；
     * - 申请 1 块 ION/CMA 全帧绘制缓冲；
     * - 初始化 G2D；
     * - 创建 LVGL display，并用 G2D 整帧旋转到 framebuffer 后台页。
     */
    f133_display_config_t display_config;
    f133_display_config_default(&display_config);

    /*
     * 如需 90/180/270 度硬件旋转，可以在这里改：
     * display_config.rotation = LV_DISPLAY_ROTATION_90;
     */
    display_config.rotation = LV_DISPLAY_ROTATION_90;

    f133_display_t *display = f133_display_create(&display_config);
    if(display == NULL) {
        fprintf(stderr, "main: failed to create F133 display\n");
        return 1;
    }

    /*
     * 触摸 pointer indev 本身不依赖 group。
     * 这里保留默认 group，是为了后续切回 widgets demo 时，chart 等控件调用
     * lv_group_add_obj(lv_group_get_default(), obj) 不会拿到 NULL。
     */
    lv_group_t *default_group = lv_group_create();
    if(default_group == NULL) {
        fprintf(stderr, "main: failed to create LVGL default group\n");
        f133_display_destroy(display);
        return 1;
    }
    lv_group_set_default(default_group);

    /*
     * 触摸输入使用 Linux input event2。
     * 触摸层会按当前 G2D 硬件旋转方向把物理触摸坐标换算到 LVGL 逻辑坐标。
     */
    f133_touch_t *touch = f133_touch_create(display, F133_TOUCH_DEFAULT_EVENT);
    if(touch == NULL) {
        fprintf(stderr, "main: failed to create F133 touch, continue without touch\n");
    }

    /*
     * 当前切到 LVGL benchmark demo，用来压测 LVGL 软件绘制、ION cache flush、
     * G2D 90 度整帧旋转和 framebuffer 翻页这条完整链路。
     */
    lv_demo_benchmark();

    /* widgets demo 先保留为注释，后续需要看 UI 效果时可以快速切回来。 */
    // lv_demo_widgets();
    // lv_demo_widgets_start_slideshow();

    /*Handle LVGL tasks*/
    while(1) {
        lv_timer_handler();
        usleep(5000);
    }

    f133_touch_destroy(touch);
    f133_display_destroy(display);
    return 0;
}
