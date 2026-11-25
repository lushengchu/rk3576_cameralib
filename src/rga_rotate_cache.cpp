#include <string.h>
#include <stdio.h>

#include "rga/im2d.hpp"
#include "rga_rotate_cache.h"

#define SRC_FMT   RK_FORMAT_BGRA_8888   /* weston framebuffer */
#define DST_FMT   RK_FORMAT_UYVY_422    /* VENC 输入 */
#define SRC_BPP   4
#define DST_BPP   2
#define SRC_CACHE_N 4                    /* 与 camera_drm.c 的 fb 缓存槽数一致 */

struct src_ent {
    void *va;
    int   size;
    rga_buffer_handle_t h;
};

static void *g_dst_va   = NULL;
static int   g_dst_alloc = 0;
static rga_buffer_handle_t g_dst_h = 0;
static struct src_ent g_src[SRC_CACHE_N];

extern "C" int rga_rot_init(void *dst_va, int dst_alloc_size)
{
    rga_rot_deinit();

    g_dst_va    = dst_va;
    g_dst_alloc = dst_alloc_size;
    g_dst_h     = importbuffer_virtualaddr(dst_va, dst_alloc_size);
    if (!g_dst_h) {
        printf("rga_rot_init: import dst failed\n");
        return -1;
    }
    return 0;
}

extern "C" int rga_rot_do(void *src_va, int src_w, int src_h, int rotate_deg)
{
    if (!g_dst_h || !src_va)
        return -1;

    int src_size = src_w * src_h * SRC_BPP;

    /* 查 src 句柄缓存; 未命中才导入一次并缓存(weston 的 scanout buffer 稳定复用) */
    rga_buffer_handle_t sh = 0;
    for (int i = 0; i < SRC_CACHE_N; i++) {
        if (g_src[i].va == src_va && g_src[i].size == src_size) {
            sh = g_src[i].h;
            break;
        }
    }
    if (!sh) {
        sh = importbuffer_virtualaddr(src_va, src_size);
        if (!sh) {
            printf("rga_rot_do: import src failed\n");
            return -2;
        }
        int slot = -1;
        for (int i = 0; i < SRC_CACHE_N; i++) {
            if (!g_src[i].va) { slot = i; break; }
        }
        if (slot < 0) {
            /* 缓存满: 淘汰 0 号槽(weston 一般只用 2~3 个 buffer, 极少走到这里) */
            slot = 0;
            releasebuffer_handle(g_src[0].h);
        }
        g_src[slot].va   = src_va;
        g_src[slot].size = src_size;
        g_src[slot].h    = sh;
    }

    int rotation;
    switch (rotate_deg) {
    case 90:  rotation = IM_HAL_TRANSFORM_ROT_90;  break;
    case 180: rotation = IM_HAL_TRANSFORM_ROT_180; break;
    case 270: rotation = IM_HAL_TRANSFORM_ROT_270; break;
    default:  rotation = 0; break;
    }

    /* 90/270 交换 dst 宽高; 0/180 不交换(与 rga_cvtcolor_demo 行为一致) */
    int swap = (rotate_deg == 90 || rotate_deg == 270);
    int dst_w = swap ? src_h : src_w;
    int dst_h = swap ? src_w : src_h;

    int dst_need = dst_w * dst_h * DST_BPP;
    if (dst_need > g_dst_alloc)
        dst_need = g_dst_alloc;
    memset(g_dst_va, 0x80, dst_need);

    rga_buffer_t src_img = wrapbuffer_handle(sh, src_w, src_h, SRC_FMT);
    rga_buffer_t dst_img = wrapbuffer_handle(g_dst_h, dst_w, dst_h, DST_FMT);

    IM_STATUS st = imrotate(src_img, dst_img, rotation);
    if (st != IM_STATUS_SUCCESS) {
        printf("rga_rot_do: imrotate failed %s\n", imStrError(st));
        return -3;
    }
    return 0;
}

extern "C" void rga_rot_deinit(void)
{
    for (int i = 0; i < SRC_CACHE_N; i++) {
        if (g_src[i].h)
            releasebuffer_handle(g_src[i].h);
        g_src[i].va   = NULL;
        g_src[i].size = 0;
        g_src[i].h    = 0;
    }
    if (g_dst_h) {
        releasebuffer_handle(g_dst_h);
        g_dst_h = 0;
    }
    g_dst_va    = NULL;
    g_dst_alloc = 0;
}
