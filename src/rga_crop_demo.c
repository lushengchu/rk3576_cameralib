/*
 * Copyright (C) 2022  Rockchip Electronics Co., Ltd.
 * Authors:
 *     YuQiaowei <cerf.yu@rock-chips.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_NDEBUG 0
#undef LOG_TAG
#define LOG_TAG "rga_crop_demo"

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <linux/stddef.h>

#include "RgaUtils.h"
#include "im2d.hpp"

//#include "utils.h"

#define LOCAL_FILE_PATH "/data"

float get_bpp_from_format(int format) {
    //这个函数定义太长了，直接用exteral的rga_crop_demo.cpp代码打印出来的，RK_FORMAT_UYVY_422返回的就是2.000
    return 2.000;
}



int rga_splict_image(void *src_buf, void *dst_buf, int src_width, int src_height) {
    int ret = 0;
    int  src_format;
    int dst_width, dst_height, dst_format;
    int src_buf_size, dst_buf_size;

    rga_buffer_t src_img, dst_img;
    im_rect rect;
    rga_buffer_handle_t src_handle, dst_handle;

    memset(&src_img, 0, sizeof(src_img));
    memset(&dst_img, 0, sizeof(dst_img));
    memset(&rect, 0, sizeof(rect));

    printf("sclu %s %d\n", __func__, __LINE__);
    src_format = RK_FORMAT_UYVY_422;

    dst_width = 640;
    dst_height = 512;
    dst_format = RK_FORMAT_UYVY_422;

    src_buf_size = src_width * src_height * get_bpp_from_format(src_format);
    dst_buf_size = dst_width * dst_height * get_bpp_from_format(dst_format);


    memset(dst_buf, 0x80, dst_buf_size);

    printf("sclu %s %d\n", __func__, __LINE__);
    dst_handle = importbuffer_virtualaddr(dst_buf, dst_buf_size);
    printf("sclu %s %d\n", __func__, __LINE__);
    src_handle = importbuffer_virtualaddr(src_buf, src_buf_size);
    printf("sclu %s %d\n", __func__, __LINE__);
    if (src_handle == 0 || dst_handle == 0) {
        printf("importbuffer failed!\n");
        goto release_buffer;
    }

    printf("sclu %s %d\n", __func__, __LINE__);
    src_img = wrapbuffer_handle(src_handle, src_width, src_height, src_format);
    dst_img = wrapbuffer_handle(dst_handle, dst_width, dst_height, dst_format);

    printf("sclu %s %d\n", __func__, __LINE__);
    /*
     * Crop a rectangular area(200*200) of src.
        ---------------------------    --------------
        |        src_img          |    |            |
        |     --------------      |    |  dst_img   |
        |     |  src_rect  |      | => |            |
        |     |            |      |    --------------
        |     --------------      |
        ---------------------------
     */

    rect.x = 640;
    rect.y = 0;
    rect.width = 640;
    rect.height = 512;
#if 0
     ret = imcheck(src_img, dst_img, {}, {});
    if (IM_STATUS_NOERROR != ret) {
        printf("%d, check error! %s", __LINE__, imStrError((IM_STATUS)ret));
        return -1;
    }
#endif
    ret = imcrop(src_img, dst_img, rect);
    if (ret == IM_STATUS_SUCCESS) {
        printf("%s running success!\n", LOG_TAG);
    } else {
        printf("%s running failed, %s\n", LOG_TAG, imStrError((IM_STATUS)ret));
        goto release_buffer;
    }



release_buffer:
    if (src_handle)
        releasebuffer_handle(src_handle);
    if (dst_handle)
        releasebuffer_handle(dst_handle);

  //  if (dst_buf)
   //     free(dst_buf);

    return ret;
}
