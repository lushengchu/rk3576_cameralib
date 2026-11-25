#ifndef RGA_ROTATE_CACHE_H
#define RGA_ROTATE_CACHE_H

#ifdef __cplusplus
extern "C" {
#endif

/* 带句柄缓存的 RGA 旋转+格式转换(BGRA8888 -> UYVY422)。
 * 用途: 替代每帧都 importbuffer_virtualaddr 的 rga_cvtcolor_demo。
 * 预置库里的 rga_cvtcolor_demo 每帧导入 3 个缓冲(src/dst/temp), 单次约 45ms
 * 且随映射累积递增, 把 DRM 录屏拖到 ~8fps。这里把 dst 只导入一次、src 按
 * vaddr 缓存(weston 在固定的几个 scanout buffer 间轮转, vaddr 稳定),
 * 每帧只调 imrotate, 输出与 rga_cvtcolor_demo 完全一致。 */

/* 导入 dst 缓冲(VENC 输入的 UYVY 帧)。dst_alloc_size 为整块缓冲字节数。
 * 成功返回 0。可重复调用(会先释放旧句柄)。 */
int  rga_rot_init(void *dst_va, int dst_alloc_size);

/* 每帧调用: 把 src_va 处的 BGRA8888(src_w x src_h)旋转 rotate_deg 后
 * 转成 UYVY 写入 init 时的 dst。rotate_deg 取 0/90/180/270。成功返回 0。 */
int  rga_rot_do(void *src_va, int src_w, int src_h, int rotate_deg);

/* 释放所有缓存句柄。录屏循环结束后调用。 */
void rga_rot_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* RGA_ROTATE_CACHE_H */
