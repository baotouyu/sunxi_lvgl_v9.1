#include "f133_touch.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define F133_TOUCH_LOGI(fmt, ...) fprintf(stderr, "[F133_TOUCH] INFO: " fmt "\n", ##__VA_ARGS__)
#define F133_TOUCH_LOGW(fmt, ...) fprintf(stderr, "[F133_TOUCH] WARN: " fmt "\n", ##__VA_ARGS__)
#define F133_TOUCH_LOGE(fmt, ...) fprintf(stderr, "[F133_TOUCH] ERROR: " fmt "\n", ##__VA_ARGS__)

typedef struct {
    int minimum;
    int maximum;
} f133_touch_abs_range_t;

struct f133_touch {
    int fd;
    lv_indev_t *indev;
    lv_display_t *lv_disp;

    uint32_t screen_width;
    uint32_t screen_height;
    lv_display_rotation_t rotation;

    f133_touch_abs_range_t x_range;
    f133_touch_abs_range_t y_range;

    int raw_x;
    int raw_y;
    bool has_x;
    bool has_y;
    lv_indev_state_t state;
};

static const char *f133_touch_path_or_default(const char *event_path)
{
    return event_path != NULL ? event_path : F133_TOUCH_DEFAULT_EVENT;
}

static const char *f133_touch_rotation_name(lv_display_rotation_t rotation)
{
    switch(rotation) {
        case LV_DISPLAY_ROTATION_0:
            return "0";
        case LV_DISPLAY_ROTATION_90:
            return "90";
        case LV_DISPLAY_ROTATION_180:
            return "180";
        case LV_DISPLAY_ROTATION_270:
            return "270";
        default:
            return "unknown";
    }
}

static bool f133_touch_query_one_abs_range(int fd, int code, f133_touch_abs_range_t *range)
{
    struct input_absinfo absinfo;

    if(range == NULL) {
        return false;
    }

    memset(&absinfo, 0, sizeof(absinfo));
    if(ioctl(fd, EVIOCGABS(code), &absinfo) < 0) {
        return false;
    }

    if(absinfo.maximum <= absinfo.minimum) {
        return false;
    }

    range->minimum = absinfo.minimum;
    range->maximum = absinfo.maximum;
    return true;
}

static void f133_touch_query_abs_range(int fd,
                                       int primary_code,
                                       int fallback_code,
                                       uint32_t fallback_size,
                                       const char *name,
                                       f133_touch_abs_range_t *range)
{
    if(f133_touch_query_one_abs_range(fd, primary_code, range)) {
        F133_TOUCH_LOGI("%s range from ABS_MT: min=%d, max=%d", name, range->minimum, range->maximum);
        return;
    }

    if(f133_touch_query_one_abs_range(fd, fallback_code, range)) {
        F133_TOUCH_LOGI("%s range from ABS: min=%d, max=%d", name, range->minimum, range->maximum);
        return;
    }

    range->minimum = 0;
    range->maximum = fallback_size > 0U ? (int)fallback_size - 1 : 0;
    F133_TOUCH_LOGW("%s range not reported; fallback to min=%d, max=%d",
                    name,
                    range->minimum,
                    range->maximum);
}

static uint32_t f133_touch_scale_raw_to_pixel(int raw, const f133_touch_abs_range_t *range, uint32_t pixel_count)
{
    int clamped;
    int64_t numerator;
    int64_t denominator;

    if(range == NULL || pixel_count == 0U) {
        return 0U;
    }

    if(range->maximum <= range->minimum || pixel_count == 1U) {
        return 0U;
    }

    clamped = raw;
    if(clamped < range->minimum) {
        clamped = range->minimum;
    }
    if(clamped > range->maximum) {
        clamped = range->maximum;
    }

    numerator = (int64_t)(clamped - range->minimum) * (int64_t)(pixel_count - 1U);
    denominator = (int64_t)(range->maximum - range->minimum);
    return (uint32_t)((numerator + denominator / 2) / denominator);
}

static int32_t f133_touch_clamp_i32(int32_t value, int32_t minimum, int32_t maximum)
{
    if(value < minimum) {
        return minimum;
    }
    if(value > maximum) {
        return maximum;
    }
    return value;
}

static lv_point_t f133_touch_transform_point(f133_touch_t *touch)
{
    uint32_t px;
    uint32_t py;
    int32_t x;
    int32_t y;
    int32_t lv_w;
    int32_t lv_h;
    lv_point_t point;

    point.x = 0;
    point.y = 0;

    if(touch == NULL || !touch->has_x || !touch->has_y) {
        return point;
    }

    px = f133_touch_scale_raw_to_pixel(touch->raw_x, &touch->x_range, touch->screen_width);
    py = f133_touch_scale_raw_to_pixel(touch->raw_y, &touch->y_range, touch->screen_height);

    /*
     * 这里的 rotation 必须和显示层 G2D 旋转一致。
     * 先把原始触摸映射到 LCD 物理坐标(px, py)，再反推到 LVGL 逻辑坐标。
     */
    switch(touch->rotation) {
        case LV_DISPLAY_ROTATION_90:
            x = (int32_t)(touch->screen_height - 1U - py);
            y = (int32_t)px;
            break;
        case LV_DISPLAY_ROTATION_180:
            x = (int32_t)(touch->screen_width - 1U - px);
            y = (int32_t)(touch->screen_height - 1U - py);
            break;
        case LV_DISPLAY_ROTATION_270:
            x = (int32_t)py;
            y = (int32_t)(touch->screen_width - 1U - px);
            break;
        case LV_DISPLAY_ROTATION_0:
        default:
            x = (int32_t)px;
            y = (int32_t)py;
            break;
    }

    lv_w = lv_display_get_horizontal_resolution(touch->lv_disp);
    lv_h = lv_display_get_vertical_resolution(touch->lv_disp);
    if(lv_w > 0) {
        x = f133_touch_clamp_i32(x, 0, lv_w - 1);
    }
    if(lv_h > 0) {
        y = f133_touch_clamp_i32(y, 0, lv_h - 1);
    }

    point.x = x;
    point.y = y;
    return point;
}

static void f133_touch_process_event(f133_touch_t *touch, const struct input_event *event)
{
    if(touch == NULL || event == NULL) {
        return;
    }

    if(event->type == EV_ABS) {
        if(event->code == ABS_X || event->code == ABS_MT_POSITION_X) {
            touch->raw_x = event->value;
            touch->has_x = true;
        }
        else if(event->code == ABS_Y || event->code == ABS_MT_POSITION_Y) {
            touch->raw_y = event->value;
            touch->has_y = true;
        }
        else if(event->code == ABS_MT_TRACKING_ID) {
            touch->state = event->value < 0 ? LV_INDEV_STATE_RELEASED : LV_INDEV_STATE_PRESSED;
        }
    }
    else if(event->type == EV_KEY) {
        if(event->code == BTN_TOUCH || event->code == BTN_MOUSE) {
            touch->state = event->value == 0 ? LV_INDEV_STATE_RELEASED : LV_INDEV_STATE_PRESSED;
        }
    }
}

static void f133_touch_read_events(f133_touch_t *touch)
{
    struct input_event event;
    ssize_t bytes_read;

    if(touch == NULL || touch->fd < 0) {
        return;
    }

    while(1) {
        bytes_read = read(touch->fd, &event, sizeof(event));
        if(bytes_read == (ssize_t)sizeof(event)) {
            f133_touch_process_event(touch, &event);
            continue;
        }

        if(bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }

        if(bytes_read < 0 && errno == EINTR) {
            continue;
        }

        if(bytes_read < 0) {
            F133_TOUCH_LOGE("failed to read input event: errno=%d(%s)", errno, strerror(errno));
        }
        else if(bytes_read > 0) {
            F133_TOUCH_LOGW("short input event read: bytes=%ld", (long)bytes_read);
        }
        return;
    }
}

static void f133_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    f133_touch_t *touch;

    if(indev == NULL || data == NULL) {
        return;
    }

    touch = (f133_touch_t *)lv_indev_get_driver_data(indev);
    if(touch == NULL) {
        data->state = LV_INDEV_STATE_RELEASED;
        data->point.x = 0;
        data->point.y = 0;
        return;
    }

    f133_touch_read_events(touch);
    data->state = touch->state;
    data->point = f133_touch_transform_point(touch);
}

f133_touch_t *f133_touch_create(f133_display_t *display, const char *event_path)
{
    const char *path;
    f133_touch_t *touch;
    int open_flags;
    int32_t lv_w;
    int32_t lv_h;

    if(display == NULL) {
        F133_TOUCH_LOGE("display is NULL");
        return NULL;
    }

    path = f133_touch_path_or_default(event_path);
    touch = (f133_touch_t *)calloc(1, sizeof(*touch));
    if(touch == NULL) {
        F133_TOUCH_LOGE("failed to allocate touch context");
        return NULL;
    }

    touch->fd = -1;
    touch->lv_disp = f133_display_get_lvgl_display(display);
    touch->screen_width = f133_display_get_screen_width(display);
    touch->screen_height = f133_display_get_screen_height(display);
    touch->rotation = f133_display_get_rotation(display);
    touch->state = LV_INDEV_STATE_RELEASED;

    if(touch->lv_disp == NULL || touch->screen_width == 0U || touch->screen_height == 0U) {
        F133_TOUCH_LOGE("invalid display information: lv_disp=%p, screen=%ux%u",
                        (void *)touch->lv_disp,
                        touch->screen_width,
                        touch->screen_height);
        f133_touch_destroy(touch);
        return NULL;
    }

    open_flags = O_RDONLY | O_NONBLOCK;
#ifdef O_CLOEXEC
    open_flags |= O_CLOEXEC;
#endif
    touch->fd = open(path, open_flags);
    if(touch->fd < 0) {
        F133_TOUCH_LOGE("failed to open touch device: path=%s, errno=%d(%s)",
                        path,
                        errno,
                        strerror(errno));
        f133_touch_destroy(touch);
        return NULL;
    }

    f133_touch_query_abs_range(touch->fd,
                               ABS_MT_POSITION_X,
                               ABS_X,
                               touch->screen_width,
                               "x",
                               &touch->x_range);
    f133_touch_query_abs_range(touch->fd,
                               ABS_MT_POSITION_Y,
                               ABS_Y,
                               touch->screen_height,
                               "y",
                               &touch->y_range);

    touch->indev = lv_indev_create();
    if(touch->indev == NULL) {
        F133_TOUCH_LOGE("failed to create LVGL input device");
        f133_touch_destroy(touch);
        return NULL;
    }

    lv_indev_set_type(touch->indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch->indev, f133_touch_read_cb);
    lv_indev_set_driver_data(touch->indev, touch);
    lv_indev_set_display(touch->indev, touch->lv_disp);

    lv_w = lv_display_get_horizontal_resolution(touch->lv_disp);
    lv_h = lv_display_get_vertical_resolution(touch->lv_disp);
    F133_TOUCH_LOGI("touch ready: path=%s, screen=%ux%u, lvgl=%dx%d, rotation=%s, raw_x=[%d,%d], raw_y=[%d,%d]",
                    path,
                    touch->screen_width,
                    touch->screen_height,
                    lv_w,
                    lv_h,
                    f133_touch_rotation_name(touch->rotation),
                    touch->x_range.minimum,
                    touch->x_range.maximum,
                    touch->y_range.minimum,
                    touch->y_range.maximum);

    return touch;
}

void f133_touch_destroy(f133_touch_t *touch)
{
    if(touch == NULL) {
        return;
    }

    if(touch->indev != NULL) {
        lv_indev_set_driver_data(touch->indev, NULL);
        lv_indev_delete(touch->indev);
        touch->indev = NULL;
    }

    if(touch->fd >= 0) {
        close(touch->fd);
        touch->fd = -1;
    }

    free(touch);
}

lv_indev_t *f133_touch_get_lvgl_indev(f133_touch_t *touch)
{
    return touch != NULL ? touch->indev : NULL;
}
