# sunxi_lvgl_v9.1

这是一个面向全志 F133/Tina Linux 的 LVGL v9.1 移植工程。当前工程已经跑通：

- LVGL v9.1.0
- Linux framebuffer `/dev/fb0`
- ION/CMA 物理连续绘制缓冲
- 全志 G2D 硬件整帧旋转/搬运
- framebuffer 双页翻页显示
- 90 度竖屏显示
- `/dev/input/event2` 触摸输入
- ARGB8888 32 bpp 渲染
- 60 FPS 刷新周期
- LVGL benchmark 性能测试
- LVGL 内置 FPS/CPU 监控显示

当前版本的定位是一个稳定基线版：先保证 LVGL、ION、G2D、fbdev、触摸和 benchmark 全链路稳定运行。后续可以在这个基础上继续做 G2D 图片叠加、alpha blend、局部刷新等更深层优化。

## 1. 当前效果

板端运行日志示例：

```text
[F133_DISPLAY] INFO: display ready: path=/dev/fb0, screen=800x480, lvgl=480x800, draw_stride=1920, draw_buf_size=1536000, fb_stride=3200, fb_mem_size=3072000, front=0, back=1
[F133_TOUCH] INFO: x range from ABS_MT: min=0, max=800
[F133_TOUCH] INFO: y range from ABS_MT: min=0, max=480
[F133_TOUCH] INFO: touch ready: path=/dev/input/event2, screen=800x480, lvgl=480x800, rotation=90, raw_x=[0,800], raw_y=[0,480]
Benchmark Summary (9.1.0 )
```

当前 benchmark 测试结果如下，测试平台为 F133/Tina Linux，LVGL 逻辑分辨率为 `480x800`，物理屏为 `800x480`，G2D 负责 90 度整帧旋转到 framebuffer：

| 场景 | 平均 CPU | 平均 FPS | 平均耗时 | render time | flush time |
| --- | ---: | ---: | ---: | ---: | ---: |
| Empty screen | 65% | 53 | 8 | 1 | 7 |
| Moving wallpaper | 69% | 59 | 11 | 6 | 5 |
| Single rectangle | 69% | 59 | 11 | 0 | 11 |
| Multiple rectangles | 69% | 59 | 11 | 2 | 9 |
| Multiple RGB images | 69% | 59 | 10 | 3 | 7 |
| Multiple ARGB images | 82% | 29 | 26 | 10 | 16 |
| Rotated ARGB images | 91% | 13 | 61 | 53 | 8 |
| Multiple labels | 74% | 54 | 12 | 7 | 5 |
| Screen sized text | 83% | 29 | 27 | 23 | 4 |
| Multiple arcs | 76% | 42 | 19 | 8 | 11 |
| Containers | 54% | 55 | 9 | 2 | 7 |
| Containers with overlay | 80% | 29 | 27 | 23 | 4 |
| Containers with opa | 72% | 46 | 15 | 6 | 9 |
| Containers with opa_layer | 69% | 47 | 14 | 6 | 8 |
| Containers with scrolling | 82% | 29 | 27 | 12 | 15 |
| Widgets demo | 74% | 32 | 22 | 12 | 10 |
| All scenes avg. | 73% | 43 | 18 | 10 | 8 |

从结果看，普通 UI 场景可以接近 60 FPS，复杂 ARGB、旋转图片、大面积文本这类 LVGL 软件绘制压力较大的场景会下降。由于当前 G2D 只负责最终整帧旋转/搬运，图片叠加和 LVGL 内部旋转图像还没有接入 G2D，因此后续仍有优化空间。

## 2. 显示方案

当前显示链路如下：

```text
LVGL 软件渲染
    |
    v
ION/CMA 全帧 draw buffer，ARGB8888，480x800
    |
    | CPU cache flush
    v
G2D 硬件 90 度整帧旋转 + copy
    |
    v
framebuffer 后台页，800x480
    |
    | FBIOPAN_DISPLAY
    v
LCD 扫描显示
```

这一版没有让 LCD 直接扫描用户态 ION buffer，而是让 fbdev 继续管理真正的 front/back 显示页。用户态只申请一块 ION/CMA 全帧绘制缓冲给 LVGL 使用，然后 G2D 把这块 buffer 旋转搬运到 framebuffer 后台页，最后用 `FBIOPAN_DISPLAY` 翻页。

这样做的优点是：

- 不需要改 LCD/framebuffer 驱动。
- G2D 可以直接访问 ION/CMA 物理连续内存。
- fbdev 双页翻页逻辑简单稳定。
- LVGL 不需要知道底层物理地址和 G2D ioctl 细节。
- 90 度屏幕旋转由硬件完成，应用层仍按竖屏 `480x800` 写 UI。

## 3. 三缓冲理解

当前工程中的“三缓冲”不是三块都由用户态直接控制的显示显存，而是：

| 缓冲 | 来源 | 作用 | 使用者 |
| --- | --- | --- | --- |
| Buffer 1 | ION/CMA | LVGL 全帧绘制缓冲 | CPU/LVGL |
| Buffer 2 | framebuffer page 0 | 显示前台页或后台页 | LCD/G2D |
| Buffer 3 | framebuffer page 1 | 显示后台页或前台页 | G2D/LCD |

流程为：

1. LVGL 在 ION/CMA draw buffer 中绘制完整逻辑画面。
2. 最后一块 flush 到来时，移植层刷新 ION cache。
3. G2D 把 draw buffer 旋转搬运到当前 framebuffer 后台页。
4. `FBIOPAN_DISPLAY` 切换 front/back 页。
5. 下一帧继续使用同一块 ION draw buffer 绘制。

## 4. 目录结构

```text
.
├── CMakeLists.txt                    # 工程主 CMake
├── build.sh                          # 一键交叉编译脚本
├── cmake/toolchains/f133_toolchain.cmake
│                                      # F133 RISC-V 交叉工具链配置
├── f133_port/
│   ├── include/
│   │   ├── f133_display.h            # LVGL display 移植层接口
│   │   ├── f133_touch.h              # Linux input 触摸适配接口
│   │   ├── f133_g2d.h                # G2D 封装接口
│   │   ├── f133_ion.h                # ION/libuapi 封装接口
│   │   ├── g2d_driver_enh.h          # 全志 G2D 用户态头文件
│   │   └── ion_mem_alloc.h           # 全志 libuapi ION 头文件
│   ├── lib/
│   │   └── libuapi.so                # 全志 libuapi 动态库
│   └── src/
│       ├── f133_display.c            # fbdev + ION + G2D + LVGL flush
│       ├── f133_touch.c              # /dev/input/eventX 触摸输入
│       ├── f133_g2d.c                # /dev/g2d ioctl 封装
│       └── f133_ion.c                # ION/CMA 内存申请和 cache flush
├── lv_conf.h                         # 当前 LVGL 配置
├── lvgl/                             # LVGL v9.1.0 源码
├── main.c                            # demo 入口，目前运行 benchmark
└── mouse_cursor_icon.c                # 原 LVGL 示例资源
```

## 5. 关键配置

当前 `lv_conf.h` 中几个重要配置：

```c
#define LV_COLOR_DEPTH 32
#define LV_DEF_REFR_PERIOD 16
#define LV_MEM_SIZE (8 * 1024 * 1024U)
#define LV_DRAW_LAYER_SIMPLE_BUF_SIZE (2 * 1024 * 1024)
#define LV_USE_SYSMON 1
#define LV_USE_PERF_MONITOR 1
```

含义：

- `LV_COLOR_DEPTH 32`：使用 ARGB8888/XRGB8888 路径，和当前 framebuffer 32 bpp 匹配。
- `LV_DEF_REFR_PERIOD 16`：刷新周期约 16 ms，目标接近 60 FPS。
- `LV_MEM_SIZE 8MB`：给 LVGL 内部对象、样式、临时数据留足空间。
- `LV_DRAW_LAYER_SIMPLE_BUF_SIZE 2MB`：给半透明图层、overlay、opa_layer 等场景预留较大的临时图层 buffer。
- `LV_USE_PERF_MONITOR 1`：屏幕右下角显示 LVGL 内置 CPU/FPS 监控。

当前 `main.c` 中默认旋转：

```c
display_config.rotation = LV_DISPLAY_ROTATION_90;
```

因此 LVGL 逻辑分辨率会变成 `480x800`，最终由 G2D 旋转到物理屏 `800x480`。

## 6. 编译环境

目标平台：

- SoC：Allwinner F133/D1s 类 RISC-V 平台
- 系统：Tina Linux
- 显示：Linux framebuffer `/dev/fb0`
- G2D：`/dev/g2d`
- 触摸：默认 `/dev/input/event2`
- 工具链：全志 Tina RISC-V glibc toolchain

默认工具链路径写在：

```text
cmake/toolchains/f133_toolchain.cmake
```

默认值为：

```cmake
/home/yuwei/samba/yuwei_work/chip_sdk/Allwinner/D1x/Tina-Linux/prebuilt/gcc/linux-x86/riscv/toolchain-thead-glibc/riscv64-glibc-gcc-thead_20200702
```

如果你的工具链路径不同，可以编译时覆盖：

```bash
cmake -S . -B build/f133 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/f133_toolchain.cmake \
  -DF133_TOOLCHAIN_ROOT=/path/to/riscv64-glibc-gcc-thead_20200702
```

也可以直接修改 `cmake/toolchains/f133_toolchain.cmake` 中的 `F133_TOOLCHAIN_ROOT`。

## 7. 编译方法

推荐直接使用脚本：

```bash
./build.sh
```

脚本会执行：

```bash
cmake -S . -B build/f133 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/f133_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build/f133 --parallel $(nproc)
```

编译成功后输出：

```text
bin/main
```

手动编译也可以：

```bash
cmake -S . -B build/f133 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/f133_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build/f133 -j$(nproc)
```

## 8. 板端运行

把程序拷贝到 F133 板子：

```bash
scp bin/main root@<board-ip>:/root/
```

如果系统运行时找不到 `libuapi.so`，也把库拷贝过去：

```bash
scp f133_port/lib/libuapi.so root@<board-ip>:/usr/lib/
```

板端执行：

```bash
chmod +x /root/main
/root/main
```

或者在板端当前目录执行：

```bash
./main
```

正常会看到：

- 串口/终端打印 F133 display 初始化信息。
- 串口/终端打印 F133 touch 初始化信息。
- 屏幕显示 LVGL benchmark。
- 屏幕右下角显示 CPU/FPS。
- benchmark 完成后打印 `Benchmark Summary (9.1.0)`。

## 9. 触摸适配

默认触摸设备为：

```c
#define F133_TOUCH_DEFAULT_EVENT "/dev/input/event2"
```

触摸适配层会读取：

- `ABS_X` / `ABS_Y`
- `ABS_MT_POSITION_X` / `ABS_MT_POSITION_Y`
- `ABS_MT_TRACKING_ID`
- `BTN_TOUCH`

当显示旋转为 90 度时，触摸坐标会从物理屏坐标转换成 LVGL 竖屏逻辑坐标。当前板端日志示例：

```text
[F133_TOUCH] INFO: touch ready: path=/dev/input/event2, screen=800x480, lvgl=480x800, rotation=90, raw_x=[0,800], raw_y=[0,480]
```

如果你的触摸设备不是 `event2`，可以修改 `f133_port/include/f133_touch.h` 中的默认路径，或者在 `main.c` 中调用 `f133_touch_create(display, "/dev/input/eventX")`。

## 10. 当前 demo

当前 `main.c` 默认运行 benchmark：

```c
lv_demo_benchmark();
```

widgets demo 保留为注释：

```c
// lv_demo_widgets();
// lv_demo_widgets_start_slideshow();
```

如果要看普通 widgets 效果，可以注释 benchmark，打开 widgets demo。

## 11. 常见问题

### 11.1 编译找不到 `ion_mem_alloc.h`

确认文件存在：

```text
f133_port/include/ion_mem_alloc.h
```

并确认 `CMakeLists.txt` 中已经把 `f133_port/include` 加入 include path。

### 11.2 运行找不到 `libuapi.so`

把库放到板端动态库搜索路径，例如：

```bash
cp libuapi.so /usr/lib/
ldconfig
```

Tina Linux 上也可以临时设置：

```bash
export LD_LIBRARY_PATH=/path/to/lib:$LD_LIBRARY_PATH
```

### 11.3 屏幕方向不对

修改 `main.c`：

```c
display_config.rotation = LV_DISPLAY_ROTATION_0;
display_config.rotation = LV_DISPLAY_ROTATION_90;
display_config.rotation = LV_DISPLAY_ROTATION_180;
display_config.rotation = LV_DISPLAY_ROTATION_270;
```

当前默认是 90 度。

### 11.4 触摸方向不对

触摸坐标变换在：

```text
f133_port/src/f133_touch.c
```

重点看 `f133_touch_transform_point()`。不同屏和触摸 IC 可能需要交换 X/Y 或反向某个轴。

### 11.5 benchmark 中旋转 ARGB 图片 FPS 很低

这是当前预期现象。现在 G2D 只负责最终整帧旋转，LVGL 内部的 rotated image、ARGB alpha blend、复杂 overlay 仍然走 CPU 软件绘制，所以 `render time` 会比较高。

后续如果接入 LVGL draw unit，把图片 blit、alpha blend、旋转图片交给 G2D，相关场景还会提升。

## 12. 后续优化方向

当前版本已经足够跑普通 UI。后续可以按优先级继续优化：

1. G2D 加速普通图片 blit。
2. G2D 加速 ARGB 图片 alpha blend。
3. G2D 加速 LVGL 内部 rotated image。
4. G2D 加速大面积 fill/rect。
5. 从整帧旋转 flush 进化到局部脏区旋转 flush。
6. 增加 flush 细分耗时日志，拆分 cache flush、G2D、fb pan 的耗时。
7. 增加触摸校准配置，例如 swap/invert/min/max。
8. 增加真实业务 UI demo，替代 benchmark 作为长期测试入口。

## 13. 当前状态总结

当前工程已经完成：

- LVGL v9.1.0 源码接入。
- F133 ION/CMA 内存申请封装。
- F133 G2D ioctl 封装。
- F133 framebuffer 双页显示封装。
- LVGL display 创建和 flush 回调。
- G2D 90 度整帧硬件旋转。
- Linux input event2 触摸输入。
- ARGB8888 32 bpp 渲染。
- 60 FPS 刷新周期。
- LVGL benchmark 测试。
- LVGL CPU/FPS 屏幕监控。

这版可以作为后续正式 UI 开发和 G2D 深度优化的基础版本。
