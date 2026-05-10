#ifndef F133_ION_H
#define F133_ION_H

/**
 * @file f133_ion.h
 * @brief F133 LVGL 显示移植层使用的 ION 内存封装接口。
 *
 * 本模块只负责用户态 ION/libuapi 内存适配层，主要能力包括：
 * - 申请物理连续的帧缓冲内存
 * - 获取可传给 G2D 等硬件模块使用的物理地址
 * - 在硬件读取前刷新 CPU cache
 * - 释放不再使用的 ION buffer
 *
 * 具体实现预期基于全志 libuapi 提供的 `ion_mem_alloc.h` 接口：
 * - `GetMemAdapterOpsS()`
 * - `SunxiMemOpen()`
 * - `SunxiMemPalloc()`
 * - `SunxiMemGetPhysicAddressCpu()`
 * - `SunxiMemFlushCache()`
 * - `SunxiMemPfree()`
 * - `SunxiMemClose()`
 *
 * 注意：本模块不要混入 LVGL 刷新流程或 G2D ioctl 逻辑。LVGL display、
 * 三缓冲状态机和 G2D 命令提交应放到独立的 display/g2d 移植层中。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct SunxiMemOpsS;

/**
 * @brief ION 分配器上下文。
 *
 * `memops` 指向全志 libuapi 的内存操作表。`opened` 用来记录当前上下文
 * 是否已经成功打开，便于 `f133_ion_deinit()` 做成对关闭。
 */
typedef struct {
    struct SunxiMemOpsS *memops;
    bool opened;
} f133_ion_t;

/**
 * @brief 单个物理连续 ION buffer 描述符。
 *
 * `virt` 是 CPU 可访问的虚拟地址。
 * `phys` 是 G2D 等硬件模块使用的物理地址。
 * `size` 是本次分配的字节数，也用于 cache 刷新。
 *
 * 零初始化后的实例表示空 buffer，可以安全传给 `f133_ion_free()` 做清理。
 */
typedef struct {
    void *virt;
    uintptr_t phys;
    size_t size;
} f133_ion_buffer_t;

/**
 * @brief 初始化 ION 分配器上下文。
 *
 * 所有 ION buffer 申请前都必须先调用此函数。实现时可以允许重复初始化：
 * 如果上下文已经打开，直接返回成功即可。
 *
 * @param ion 待初始化的 ION 分配器上下文。
 * @return true 表示初始化成功，false 表示 libuapi/ION 打开失败或参数无效。
 */
bool f133_ion_init(f133_ion_t *ion);

/**
 * @brief 关闭 ION 分配器上下文。
 *
 * 此函数只关闭 `ion` 持有的 libuapi 内存适配器，不会自动释放已经申请的
 * `f133_ion_buffer_t`。调用者应先释放所有 buffer，再反初始化上下文。
 *
 * @param ion 待关闭的 ION 分配器上下文。
 */
void f133_ion_deinit(f133_ion_t *ion);

/**
 * @brief 申请一块物理连续 ION buffer。
 *
 * 成功后会填充 `out->virt`、`out->phys` 和 `out->size`。该 buffer 可作为
 * LVGL 全帧绘制缓冲，也可在刷新 cache 后交给 G2D 读取或写入。
 *
 * @param ion 已初始化的 ION 分配器上下文。
 * @param size 申请大小，单位为字节，必须大于 0。
 * @param out 输出 buffer 描述符。
 * @return true 表示申请成功，false 表示参数无效或内存申请失败。
 */
bool f133_ion_alloc(f133_ion_t *ion, size_t size, f133_ion_buffer_t *out);

/**
 * @brief 释放一块 ION buffer。
 *
 * 实现时应在释放成功后清空 `buffer`，这样错误路径或重复清理路径更安全。
 *
 * @param ion 已初始化的 ION 分配器上下文。
 * @param buffer 由 `f133_ion_alloc()` 返回的 buffer 描述符。
 */
void f133_ion_free(f133_ion_t *ion, f133_ion_buffer_t *buffer);

/**
 * @brief 将 ION 虚拟地址转换为物理地址。
 *
 * 当已有 libuapi 分配出的虚拟地址，需要传给 G2D 等硬件模块时使用。
 * 常规 buffer 创建建议优先使用 `f133_ion_alloc()`，因为它会同时记录虚拟
 * 地址、物理地址和大小。
 *
 * @param ion 已初始化的 ION 分配器上下文。
 * @param virt ION 分配器返回的 CPU 虚拟地址。
 * @return 成功返回物理地址，失败返回 0。
 */
uintptr_t f133_ion_get_phys(f133_ion_t *ion, void *virt);

/**
 * @brief 刷新一段 CPU 写过的内存范围，确保硬件能读到最新数据。
 *
 * LVGL/CPU 写完像素后，在把 buffer 提交给 G2D 或其他硬件模块读取前，
 * 应调用此函数刷新 cache。若需要处理“硬件写完后 CPU 再读”的方向，后续
 * 可能还需要根据平台能力补充 invalidate 语义。
 *
 * @param ion 已初始化的 ION 分配器上下文。
 * @param virt 需要刷新的虚拟地址起始位置。
 * @param size 需要刷新的字节数。
 */
void f133_ion_flush_range(f133_ion_t *ion, void *virt, size_t size);

/**
 * @brief 刷新整个 ION buffer 覆盖的 cache 范围。
 *
 * 这是 `f133_ion_flush_range()` 的便捷封装，用于按 buffer 描述符刷新整块
 * 帧缓冲。
 *
 * @param ion 已初始化的 ION 分配器上下文。
 * @param buffer 需要刷新的 buffer 描述符。
 */
void f133_ion_flush_cache(f133_ion_t *ion, const f133_ion_buffer_t *buffer);

/**
 * @brief 判断 buffer 描述符是否指向一块可用分配。
 *
 * @param buffer 待检查的 buffer 描述符。
 * @return true 表示虚拟地址非空、物理地址非 0、大小非 0。
 */
bool f133_ion_buffer_is_valid(const f133_ion_buffer_t *buffer);

/**
 * @brief 将 buffer 描述符清空为空状态。
 *
 * 此函数只清空描述符本身，不释放实际内存。已经分配的 buffer 必须使用
 * `f133_ion_free()` 释放。
 *
 * @param buffer 待清空的 buffer 描述符。
 */
void f133_ion_buffer_clear(f133_ion_buffer_t *buffer);

#ifdef __cplusplus
}
#endif

#endif /* F133_ION_H */
