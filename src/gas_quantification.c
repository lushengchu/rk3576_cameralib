#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include "camera_main.h"
#include "gas_quantification.h"
#include "c_gass_detec.h"
#include "palette.h"

/******************************************************************************
 *  Private Defines and Macros
 */
#define Y8_FRAME_SIZE               (CONFIG_DVP_SRC_WIDTH * CONFIG_DVP_SRC_HEIGHT)

/******************************************************************************
 *  Private Variable Prototypes
 */
static struct GasQuantificationCtx s_gas_quantification_ctx;
static uint8_t s_frame_buff_bit8[1 + GAS_RESULT_MASK_BUFF_NUM][Y8_FRAME_SIZE];
static uint8_t s_frame_buff_bit16[3][Y8_FRAME_SIZE * 2];

/******************************************************************************
 *  Private Function Prototypes
 */
/**********************************************************************
 * Description:
 * Parameter:
 * Return:
 * Note:
 **********************************************************************/
static int uyvy_to_rgba_no_palette(int width, int height, uint8_t *uyvy, uint8_t *rgba)
{
    int i, j;
    int c, d, e, r, g, b;
    uint8_t u, y0, v, y1;

    for (i = 0; i < height; i++) {
        for (j = 0; j < width; j += 2) {
            // 读取 UYVY 数据
            u = uyvy[(i * width + j) * 2];
            y0 = uyvy[(i * width + j) * 2 + 1];
            v = uyvy[(i * width + j) * 2 + 2];
            y1 = uyvy[(i * width + j) * 2 + 3];

            // 转换第一个像素
            c = y0 - 16;
            d = u - 128;
            e = v - 128;

            r = (298 * c + 409 * e + 128) >> 8;
            g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            b = (298 * c + 516 * d + 128) >> 8;

            // 限制 RGB 值在 0-255 范围内
            r = (r < 0) ? 0 : (r > 255) ? 255 : r;
            g = (g < 0) ? 0 : (g > 255) ? 255 : g;
            b = (b < 0) ? 0 : (b > 255) ? 255 : b;

            // 写入第一个像素的 RGBA 数据
            rgba[(i * width + j) * 4] = (uint8_t)r;
            rgba[(i * width + j) * 4 + 1] = (uint8_t)g;
            rgba[(i * width + j) * 4 + 2] = (uint8_t)b;
            rgba[(i * width + j) * 4 + 3] = 255; // 不透明

            // 转换第二个像素
            c = y1 - 16;

            r = (298 * c + 409 * e + 128) >> 8;
            g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            b = (298 * c + 516 * d + 128) >> 8;

            // 限制 RGB 值在 0-255 范围内
            r = (r < 0) ? 0 : (r > 255) ? 255 : r;
            g = (g < 0) ? 0 : (g > 255) ? 255 : g;
            b = (b < 0) ? 0 : (b > 255) ? 255 : b;

            // 写入第二个像素的 RGBA 数据
            rgba[(i * width + j + 1) * 4] = (uint8_t)r;
            rgba[(i * width + j + 1) * 4 + 1] = (uint8_t)g;
            rgba[(i * width + j + 1) * 4 + 2] = (uint8_t)b;
            rgba[(i * width + j + 1) * 4 + 3] = 255; // 不透明
        }
    }

    return 0;
}
/**********************************************************************
 * Description:
 * Parameter:
 * Return:
 * Note:
 **********************************************************************/
static int uyvy_to_rgba(int width, int height, uint8_t *uyvy, uint8_t *rgba, int palette_index)
{
    int i, index;
    uint8_t *pallette;

    if (palette_index >= 0) {
        pallette = (uint8_t *)&palette_rgba[1024 * palette_index];
        for (i = 0; i < width * height; i++) {
            index = *(uint8_t *)(uyvy + 2 * i + 1);
            rgba[4 * i + 0] = pallette[4 * index + 0];
            rgba[4 * i + 1] = pallette[4 * index + 1];
            rgba[4 * i + 2] = pallette[4 * index + 2];
            rgba[4 * i + 3] = pallette[4 * index + 3];
        }
    }
    else {
        uyvy_to_rgba_no_palette(width, height, uyvy, rgba);
    }

    return 0;
}
/**********************************************************************
 * Description:
 * Parameter:
 * Return:
 * Note:
 **********************************************************************/
static int rgba_to_uyvy_no_palette(int width, int height, uint8_t *rgba, uint8_t *uyvy)
{
    int i, j;
    uint8_t r0, g0, b0, r1, g1, b1, a;
    int y0, y1, u, v;

    for (i = 0; i < height; i++) {
        for (j = 0; j < width; j += 2) {
            // 读取第一个像素的 RGBA 数据
            r0 = rgba[(i * width + j) * 4 + 0];
            g0 = rgba[(i * width + j) * 4 + 1];
            b0 = rgba[(i * width + j) * 4 + 2];

            // 读取第二个像素的 RGBA 数据
            r1 = rgba[(i * width + j + 1) * 4 + 0];
            g1 = rgba[(i * width + j + 1) * 4 + 1];
            b1 = rgba[(i * width + j + 1) * 4 + 2];

            // 转换第一个像素为 YUV
            y0 = (int)(0.299 * r0 + 0.587 * g0 + 0.114 * b0);
            u = (int)(-0.14713 * r0 - 0.28886 * g0 + 0.436 * b0) + 128;
            v = (int)(0.615 * r0 - 0.51499 * g0 - 0.10001 * b0) + 128;

            // 转换第二个像素为 YUV
            y1 = (int)(0.299 * r1 + 0.587 * g1 + 0.114 * b1);

            // 限制 YUV 值在 0-255 范围内
            y0 = (y0 < 0) ? 0 : (y0 > 255) ? 255 : y0;
            u = (u < 0) ? 0 : (u > 255) ? 255 : u;
            v = (v < 0) ? 0 : (v > 255) ? 255 : v;
            y1 = (y1 < 0) ? 0 : (y1 > 255) ? 255 : y1;

            // 写入 UYVY 数据
            uyvy[(i * width + j) * 2] = (uint8_t)u;
            uyvy[(i * width + j) * 2 + 1] = (uint8_t)y0;
            uyvy[(i * width + j) * 2 + 2] = (uint8_t)v;
            uyvy[(i * width + j) * 2 + 3] = (uint8_t)y1;
        }
    }

    return 0;
}
/**********************************************************************
 * Description:
 * Parameter:
 * Return:
 * Note:
 **********************************************************************/
static int rgba_to_uyvy(int width, int height, uint8_t *rgba, uint8_t *uyvy, int palette_index)
{
    if (palette_index >= 0) {
        /*
            process ...
        */
    }
    else {
        rgba_to_uyvy_no_palette(width, height, rgba, uyvy);
    }

    return 0;
}
/**********************************************************************
 * Description: 处理一帧数据
 * Parameter:
 * Return:
 * Note:
 **********************************************************************/
static int gas_quantification_dealNewFrame(struct GasQuantificationAttr *attr, struct SendPacket *send, struct ResponsePacket *response)
{
    int i, w, ret;
    int select, sock_fd;
    int len, read_len;
    uint8_t *mask = NULL;
    double ppm = 0;

    select = attr->select;
    if (select == GAS_ANALYSE_SELECT_RUSS) {
        // 2. Sending frame: header
        memset(&send->head, 0, sizeof(send->head));
        send->head.Version = 1;
        send->head.OperationType = 2;
        send->head.Width = attr->width;
        send->head.Height = attr->height;
        send->head.RegionX = attr->regionX;
        send->head.RegionY = attr->regionY;
        send->head.RegionWidth = attr->regionW;
        send->head.RegionHeight = attr->regionH;
        send->head.Distance = attr->distance;
        send->head.FilterIntensity = attr->filterIntensity;
        send->head.TempMin = attr->tempMin;
        send->head.TempMax = attr->tempMax;
        sock_fd = attr->GasRuss.sock_fd;
        ret = write(sock_fd, &send->head, sizeof(send->head));
        if (ret != sizeof(send->head)) {
            printf("%s write header failed, ret=%d\n", __func__, ret);
            return -1;
        }

        // 3. Sending frame: body
        len = attr->width * attr->height;
        ret = write(sock_fd, send->body.y8, len);
        if (ret != len) {
            printf("%s write data failed, len=%d, ret=%d\n", __func__, len, ret);
            return -1;
        }

        // 4. Receiving frame: header
        memset(&response->head, 0, sizeof(response->head));
        ret = read(sock_fd, &response->head, sizeof(response->head));
        if (ret != sizeof(response->head)) {
            printf("%s read head failed, ret=%d, len=%ld\n", __func__, ret, sizeof(response->head));
            return -1;
        }

        // 5. Receiving frame: have body when index >= 300
        if (response->head.index >= 300) {
            for (i = 0; i < GAS_RESULT_MASK_BUFF_NUM; i++) {
                if (response->body.ready[i] == 0) {
                    len = attr->width * attr->height;
                    read_len = 0;
                    while (1) {
                        ret = read(sock_fd, response->body.mask[i] + read_len, len);
                        read_len += ret;
                        if (read_len == len) {
                            break;
                        }
                    }
                    response->body.ready[i] = 1;
                    break;
                }
            }
        }
        else {
            for (i = 0; i < GAS_RESULT_MASK_BUFF_NUM; i++) {
                response->body.ready[i] = -1;
            }
        }
    }
    else if (select == GAS_ANALYSE_SELECT_XIAN) {
        w = response->body.w;
        response->body.ready[w] = 0;
        // pthread_mutex_lock(&response->body.mutex[w]);
        setRectangle(attr->GasXian.handle, attr->regionX, attr->regionY, attr->regionW, attr->regionH);
        mask = dealNewFrame(attr->GasXian.handle, send->body.raw, &ppm);
        if (mask == NULL) {
            response->head.ppm = 0;
            memset(response->body.mask[w], 0, attr->width * attr->height);
        }
        else {
            response->head.ppm = ppm;
            memcpy(response->body.mask[w], mask, attr->width * attr->height);
        }
        // pthread_mutex_unlock(&response->body.mutex[w]);
        response->body.ready[w] = 1;
        response->body.r = w;
        response->body.w = (w + 1) % GAS_RESULT_MASK_BUFF_NUM;
    }

    return 0;
}
/**********************************************************************
 * Description:图像渲染
 * Parameter:
 * Return:
 * Note:
 **********************************************************************/
static int gas_quantification_imageFusion(int width, int height, uint8_t *src, uint8_t *mask, uint8_t *dst, int transparency)
{
    int i, ret;
    int palette_index;
    uint8_t buff[3][Y8_FRAME_SIZE * 4];
    struct Pixel bg, fg, blend;
    float alpha, inv_alpha;

    /* 原始图像: YUV422格式转换为RGBA格式 */
    palette_index = 0;  // 白热
    ret = uyvy_to_rgba(width, height, src, buff[0], palette_index);
    if (ret < 0) {
        printf("uyvy to rgba err ret=%d\n", ret);
    }

    /* 算法掩码: Y8转换为RGBA格式, 暂时使用红色渐变 */
    for (i = 0; i < width * height; i++) {
        buff[1][4 * i + 0] = 255;
        buff[1][4 * i + 1] = 255 - mask[i];
        buff[1][4 * i + 2] = 255 - mask[i];
        if (mask[i] == 0) {
            buff[1][4 * i + 3] = 0;      /* 0:完全透明 255:完全不透明 */
        }
        else {
            buff[1][4 * i + 3] = transparency & 0xFF;
        }
    }
    /* 原始图像与算法掩码RGBA叠加 */
    for (i = 0; i < width * height; i++) {
        alpha = fg.a / 255.0f;
        inv_alpha = 1.0f - alpha;
        memcpy(&bg, buff[0] + 4 * i, sizeof(struct Pixel));
        memcpy(&fg, buff[1] + 4 * i, sizeof(struct Pixel));
        blend.r = (uint8_t)(fg.r * alpha + bg.r * inv_alpha);
        blend.g = (uint8_t)(fg.g * alpha + bg.g * inv_alpha);
        blend.b = (uint8_t)(fg.b * alpha + bg.b * inv_alpha);
        blend.a = 255;
        memcpy(&buff[2][4 * i], &blend, sizeof(struct Pixel));
    }

    palette_index = -1;
    ret = rgba_to_uyvy(width, height, buff[2], dst, palette_index);
    if (ret < 0) {
        printf("rgba to uyvy ret=%d\n", ret);
    }

    return 0;
}
/**********************************************************************
 * Description:
 * Parameter:
 * Return:
 * Note:
 **********************************************************************/
static int gas_quantification_init(struct GasQuantificationCtx *ctx)
{
    int i, ret;
    int select, sock_fd;
    struct SendHead head;
    struct sockaddr_un addr;
    struct GasQuantificationAttr *attr = &ctx->attr;
    struct SendPacket *send = &ctx->send;
    struct ResponsePacket *response = &ctx->response;

    // 1. attr init
	attr->select = emsg.gas_analit_vendor;
	attr->width = emsg.dvp_source_w;
	attr->height = emsg.dvp_source_h;
	attr->regionX = emsg.gas_analit_x;
	attr->regionY = emsg.gas_analit_y;
	attr->regionW = emsg.gas_analit_w;
	attr->regionH = emsg.gas_analit_h;
	attr->distance = emsg.gas_analit_distance;
	attr->filterIntensity = emsg.gas_analit_filterIntensity;
	attr->paintedTransparency = emsg.gas_analit_paintedTransparency;

    // 2. send packet init
    memset(send, 0, sizeof(struct SendPacket));
    send->body.y8 = s_frame_buff_bit8[0];
    send->body.uyvy = s_frame_buff_bit16[0];
    send->body.raw = s_frame_buff_bit16[1];

    // 3. response packet init
    memset(response, 0, sizeof(struct ResponsePacket));
    for (i = 0; i < GAS_RESULT_MASK_BUFF_NUM; i++) {
        response->body.mask[i] = s_frame_buff_bit8[i + 1];
    }
    response->body.uyvy = s_frame_buff_bit16[2];
    if (attr->select == GAS_ANALYSE_SELECT_RUSS) {
        // 0. Create a UNIX domain socket
        sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock_fd == -1) {
            printf("%s socket failed, ret=%d\n", __func__, sock_fd);
            return -1;
        }
        memset(&addr, 0, sizeof(struct sockaddr_un));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, SOCKET_PIPE_NAME, sizeof(addr.sun_path) - 1);
        ret = connect(sock_fd, (struct sockaddr *) &addr, sizeof(struct sockaddr_un));
        if (ret == -1) {
            printf("%s connect failed, ret=%d\n", __func__, ret);
            return -1;
        }
        memset(&head, 0, sizeof(struct SendHead));
        head.Version = 1;           /* must set to 1 */
        head.OperationType = 1;     /* 1:handshake */
        head.Width = attr->width;
        head.Height = attr->height;
        head.RegionX = attr->regionX;
        head.RegionY = attr->regionY;
        head.RegionWidth = attr->regionW;
        head.RegionHeight = attr->regionH;
        head.Distance = attr->distance;
        head.FilterIntensity = attr->filterIntensity;
        // 1. send hand shake
        ret = write(sock_fd, &head, sizeof(struct SendHead));
        if (ret != sizeof(struct SendHead)) {
            printf("%s send handshake header failed, ret=%d\n", __func__, ret);
            return -1;
        }
        attr->GasRuss.sock_fd = sock_fd;
    }
    else if (attr->select == GAS_ANALYSE_SELECT_XIAN) {
	    attr->GasXian.handle = image_processor_create();
    }

    response->body.r = 0;
    response->body.w = 0;
    for (i = 0; i < GAS_RESULT_MASK_BUFF_NUM; i++) {
	    pthread_mutex_init(&response->body.mutex[i], NULL);
    }

    return 0;
}
/**********************************************************************
 * Description:
 * Parameter:
 * Return:
 * Note:
 **********************************************************************/
static int gas_quantification_deinit(struct GasQuantificationCtx *ctx)
{
    int ret;
    int select, sock_fd;
    struct SendHead head;
    struct GasQuantificationAttr *attr = &ctx->attr;
    struct ResponsePacket *response = &ctx->response;

    select = attr->select;
    if (select == GAS_ANALYSE_SELECT_RUSS) {
        sock_fd = attr->GasRuss.sock_fd;
        if (sock_fd != -1) {
            memset(&head, 0, sizeof(struct SendHead));
            head.Version = 1;
            head.OperationType = 0;
            ret = write(sock_fd, &head, sizeof(struct SendHead));
            if (ret != sizeof(struct SendHead)) {
                printf("%s write failed, ret=%d\n", __func__, ret);
            }
            close(sock_fd);
        }
    }
    else if (select == GAS_ANALYSE_SELECT_XIAN) {
        image_processor_destroy(attr->GasXian.handle);
    }

    return 0;
}
/**********************************************************************
 * Description:
 * Parameter:
 * Return:
 * Note:
 **********************************************************************/
static int gas_quantification_region_update(struct GasQuantificationCtx *quantification)
{
    quantification->attr.regionX = emsg.gas_analit_x;
    quantification->attr.regionY = emsg.gas_analit_y;
    quantification->attr.regionW = emsg.gas_analit_w;
    quantification->attr.regionH = emsg.gas_analit_h;
    quantification->attr.paintedTransparency = emsg.gas_analit_paintedTransparency;

    return 0;
}
/**********************************************************************
 * Description:
 * Parameter:
 * Return:
 * Note:
 **********************************************************************/
static void *GasQuantificationFunction(void *p)
{
    int ret;
    struct GasQuantificationCtx *GasQuantification = get_gas_quantification_ctx();
    struct GasQuantificationAttr *attr = &GasQuantification->attr;
    struct SendPacket *send = &GasQuantification->send;
    struct ResponsePacket *response = &GasQuantification->response;
    RK_U64 s1, s2, delta;
    int sval = -1;

    for (;;) {
		sem_wait(&csem.gasQuantificationeSem);
        if (attr->enabled == 0) {
            break;
        }
        s1 = TEST_COMM_GetNowUs();
        ret = gas_quantification_dealNewFrame(attr, send, response);
        if (ret < 0) {
            printf("gas_quantification_dealNewFrame failed\n");
        }
        s2 = TEST_COMM_GetNowUs();
        delta = s2 -s1;
        if (delta > 120 * 1000) {
            printf("%s consume too long time=%ld us\n", __func__, delta);
        }
    }

    return NULL;
}
/******************************************************************************
 *  Private Function Prototypes
 */
/**********************************************************************
 * Description: 俄罗斯半定量气体识别算法接口
 * Parameter: 输入: data --> YUV422 YUV帧的数据转成Y8之后送算法
 *            输出: LeakData --> YUV422
 *            输出: Leakindex, LeakRate, LeakIntegral, LeakAvg
 * Return:
 * Note:
 **********************************************************************/
int camera_gas_quantification_dealNewFrame_russ(uint8_t *data, uint8_t *LeakData, uint64_t *Leakindex, double *LeakRate, double *LeakIntegral, double *LeakAvg)
{
    int i, ret;
    struct GasQuantificationCtx *GasQuantification = get_gas_quantification_ctx();
    struct GasQuantificationAttr *attr = &GasQuantification->attr;
    struct SendPacket *send = &GasQuantification->send;
    struct ResponsePacket *response = &GasQuantification->response;

    if (attr->GasRuss.sock_fd > 0) {
        ret = gas_quantification_region_update(GasQuantification);
        if (ret < 0) {
            printf("gas_quantification_region_update failed\n");
        }
        memcpy(send->body.uyvy, data, attr->width * attr->height * 2);
        for (i = 0; i < attr->width * attr->height; i++) {
            send->body.y8[i] = send->body.uyvy[i * 2 + 1];
        }

        /* 另起线程处理气体分析任务 */
	    sem_post(&csem.gasQuantificationeSem);

        for (i = 0; i < GAS_RESULT_MASK_BUFF_NUM; i++) {
            if (response->body.ready[i] == 1) {
                ret = gas_quantification_imageFusion(attr->width, attr->height, send->body.uyvy, response->body.mask[i], response->body.uyvy, attr->paintedTransparency);
                if (ret < 0) {
                    printf("%s imageFusion failed ret=%d\n", __func__, ret);
                    *Leakindex = 0;
                    *LeakRate = 0;
                    *LeakIntegral = 0;
                    *LeakAvg = 0;
                }
                else {
                    *Leakindex = response->head.index;
                    *LeakRate = response->head.rate;
                    *LeakIntegral = response->head.integral;
                    *LeakAvg = response->head.avg;
                }
                memcpy(LeakData, response->body.uyvy, attr->width * attr->height * 2);
                response->body.ready[i] = 0;
                break;
            }
            else if (response->body.ready[i] == -1) {
                *Leakindex = 0;
                *LeakRate = 0;
                *LeakIntegral = 0;
                *LeakAvg = 0;
                memcpy(LeakData, send->body.uyvy, attr->width * attr->height * 2);
                response->body.ready[i] = 0;
                break;
            }
        }
        if (i == GAS_RESULT_MASK_BUFF_NUM) {
            printf("%s time out\n", __func__);
            memcpy(LeakData, send->body.uyvy, attr->width * attr->height * 2);
            *Leakindex = 0;
            *LeakRate = 0;
            *LeakIntegral = 0;
            *LeakAvg = 0;
        }
    }

    return 0;
}
/**********************************************************************
 * Description:西安半定量气体识别算法接口
 * Parameter: 输入: data--> YUV422或RAW YUV帧的数据用于图像融合,RAW帧数据送算法处理
 *            输出: *LeakData --> YUV422
 *            输出: ppm
 * Return:
 * Note:
 **********************************************************************/
int camera_gas_quantification_dealNewFrame_xian(uint8_t *data, uint8_t *LeakData, double *LeakPPM, int flag)
{
    int i, r, ret;
    double ppm = 0;
    struct GasQuantificationCtx *GasQuantification = get_gas_quantification_ctx();
    struct GasQuantificationAttr *attr = &GasQuantification->attr;
    struct SendPacket *send = &GasQuantification->send;
    struct ResponsePacket *response = &GasQuantification->response;
    int sval = -1;

    if (flag == INFRARED_FRAME_TAG_1122) {
        memcpy(send->body.uyvy, data, attr->width * attr->height * 2);
        r = response->body.r;
        if (response->body.ready[r] == 1) {
            // pthread_mutex_lock(&response->body.mutex[r]);
            ret = gas_quantification_imageFusion(attr->width, attr->height, send->body.uyvy, response->body.mask[r], response->body.uyvy, attr->paintedTransparency);
            if (ret < 0) {
                printf("%s imageFusion failed ret=%d\n", __func__, ret);
                *LeakPPM = 0;
            }
            else {
                *LeakPPM = response->head.ppm;
            }
            // pthread_mutex_unlock(&response->body.mutex[r]);
        }
        memcpy(LeakData, response->body.uyvy, attr->width * attr->height * 2);
    }
    else if (flag == INFRARED_FRAME_TAG_55AA) {
        ret = gas_quantification_region_update(GasQuantification);
        if (ret < 0) {
            printf("gas_quantification_region_update failed\n");
        }
        memcpy(send->body.raw, data, attr->width * attr->height * 2);

        /* 另起线程处理气体分析任务 */
	    sem_post(&csem.gasQuantificationeSem);
    }

    return 0;
}
/**********************************************************************
 * Description:
 * Parameter:
 * Return:
 * Note:
 **********************************************************************/
struct GasQuantificationCtx *get_gas_quantification_ctx()
{
    return &s_gas_quantification_ctx;
}
/**********************************************************************
 * Description:
 * Parameter:
 * Return:
 * Note:
 **********************************************************************/
int camera_gas_quantification_start()
{
    int i, ret;
    struct GasQuantificationCtx *GasQuantification = get_gas_quantification_ctx();

    if (GasQuantification->attr.enabled == 1) {
        return 0;
    }
    else {
        ret = gas_quantification_init(GasQuantification);
        if (ret < 0) {
            printf("gas_quantification_init failed, ret=%d\n", ret);
            return ret;
        }
        GasQuantification->attr.enabled = 1;
        pthread_create(&GasQuantification->attr.threadID, NULL, GasQuantificationFunction, NULL);
    }

    return 0;
}
/**********************************************************************
 * Description:
 * Parameter:
 * Return:
 * Note:
 **********************************************************************/
int camera_gas_quantification_stop()
{
    struct GasQuantificationCtx *GasQuantification = get_gas_quantification_ctx();

    if (GasQuantification->attr.enabled == 0) {
        return 0;
    }
    else {
        GasQuantification->attr.enabled = 0;
        sem_post(&csem.gasQuantificationeSem);
        pthread_join(GasQuantification->attr.threadID, NULL);
        gas_quantification_deinit(GasQuantification);
    }

    return 0;
}
/*****************************************************************************
 *     end of file
 *****************************************************************************/