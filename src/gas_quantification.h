#ifndef _GAS_QUANTIFICATION_H_
#define _GAS_QUANTIFICATION_H_

#include <stdint.h>

/******************************************************************************
 *  Public Defines and Macros
 */

#define SOCKET_PIPE_NAME            "/tmp/CoreFxPipe_gas_analit.service"

#define GAS_ANALYSE_VENDOR_RUSS      0
#define GAS_ANALYSE_VENDOR_XIAN      1
#define GAS_ANALYSE_SELECT_DISABLE   (-1)
#define GAS_ANALYSE_SELECT_RUSS      GAS_ANALYSE_VENDOR_RUSS
#define GAS_ANALYSE_SELECT_XIAN      GAS_ANALYSE_VENDOR_XIAN

#define GAS_RESULT_MASK_BUFF_NUM     4

struct SendHead {
    int16_t Version;            /* must set to 1 */
    int16_t OperationType;      /* 0:end 1:handshake 2:data */
    int32_t Width;              /* 320 */
    int32_t Height;             /* 256 */
    int16_t RegionX;            /* x+w < Width */
    int16_t RegionY;            /* y+h < height */
    int16_t RegionWidth;
    int16_t RegionHeight;
    float Distance;             /* 检测距离: 单位: 米, 默认 2.0 */
    uint8_t FilterIntensity;    /* 滤波系数: 默认128 [+4,-2] */
    uint8_t Offset_4;
    int16_t TempMin;
    int16_t TempMax;
    uint8_t Offset_5;
    uint8_t Offset_6;
} __attribute__ ((packed));

struct SendBody {
    uint8_t *uyvy;      /* src yuv422 uyvy */
    uint8_t *y8;        /* convert src yuv422 to y8 */
    uint8_t *raw;       /* src y14 raw */
};

struct SendPacket {
    struct SendHead head;
    struct SendBody body;
};

struct ResponseHead {
    union {
        uint64_t index;     /* byte: 0-7 index unsigned int64 */
        double ppm;         /* byte: 0-7 ppm (double) */
    };
    double rate;            /* byte: 8-15 calculated leak rate (double) */
    double integral;        /* byte: 16-23 (double) */
    double avg;             /* byte: 24-31 (double) */
} __attribute__ ((packed));

struct ResponseBody {
	pthread_mutex_t mutex[GAS_RESULT_MASK_BUFF_NUM];
	int w, r;
    int ready[GAS_RESULT_MASK_BUFF_NUM];           /* ready flag */
    uint8_t *mask[GAS_RESULT_MASK_BUFF_NUM];       /* mask buff */
    uint8_t *uyvy;                                 /* uyvy buff */
};

struct ResponsePacket {
    struct ResponseHead head;
    struct ResponseBody body;
};

struct GasQuantificationAttrRuss {
    int sock_fd;
};

struct GasQuantificationAttrXian {
    void *handle;
};

struct GasQuantificationAttr {
    int select;
    int enabled;
    pthread_t threadID;
    union {
        struct GasQuantificationAttrRuss GasRuss;
        struct GasQuantificationAttrXian GasXian;
    };
    int32_t width, height;
    int16_t regionX, regionY, regionW, regionH;
    float distance;
    uint8_t filterIntensity;
    uint8_t paintedTransparency;
    int16_t tempMin, tempMax;
};

struct Pixel {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
};

struct GasQuantificationCtx {
    struct GasQuantificationAttr attr;
    struct SendPacket send;
    struct ResponsePacket response;
};

/******************************************************************************
 *  Public Variable Declaration
 */

 /******************************************************************************
 *  Public Function Declaration
 */
/**********************************************************************
 * Description:
 * Parameter:
 * Return:
 * Note:
 **********************************************************************/
struct GasQuantificationCtx *get_gas_quantification_ctx();

/**********************************************************************
 * Description:
 * Parameter:
 * Return:
 * Note:
 **********************************************************************/
int camera_gas_quantification_start();
int camera_gas_quantification_stop();

/**********************************************************************
 * Description:
 * Parameter:
 * Return:
 * Note:
 **********************************************************************/
int camera_gas_quantification_dealNewFrame_russ(uint8_t *data, uint8_t *LeakData, uint64_t *LeakIndex, double *LeakRate, double *LeakIntegral, double *LeakAvg);
int camera_gas_quantification_dealNewFrame_xian(uint8_t *data, uint8_t *LeakData, double *Leakppm, int flag);

#endif

/*****************************************************************************
 *     end of file
 *****************************************************************************/