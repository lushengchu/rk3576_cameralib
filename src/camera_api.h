#ifndef _CAMERA_API_H_
#define _CAMERA_API_H_

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif /* __cplusplus */

#define FGCAMERA_VERSION    "0.4.14"

#define RTMP_PATH_LIVE  "/oem/app/4g_live"
#define RTMP_PATH_OFF   "/oem/app/4g_off"

typedef enum camera_status {
    /* CAM STATUS */
    CAM_CAMERA_IDLE = 0,
    /*截屏用的参数 --start*/
    CAM_CAMERA_CAPTURE_IR,
    CAM_CAMERA_CAPTURE_MIPI,
    CAM_CAMERA_CAPTURE_SCREEN,
    CAM_CAMERA_CAPTURE_IR_MIPI,
    CAM_CAMERA_CAPTURE_IR_SCREEN,
    CAM_CAMERA_CAPTURE_MIPI_SCREEN,
    CAM_CAMERA_CAPTURE_ALL,
    /*截屏用的参数 --end*/
    CAM_CAMERA_RECORDSTART,
    CAM_CAMERA_RECORDPAUSE,
    CAM_CAMERA_RECORDSTOP,
    CAM_CAMERA_RTSPSTART,
    CAM_CAMERA_RTSPSTOP,
    CAM_CAMERA_RTMPSTART,
    CAM_CAMERA_RTMPSTOP,

    /* DIS STATUS */
    DIS_CAMERA_IDLE,
    DIS_CAMERA_SRC,
    DIS_CAMERA_OUT_IR,
    DIS_CAMERA_OUT_VL,

    /* AI STATUS */
    AI_CAMERA_IDLE,
    AI_CAMERA_START,
    AI_CAMERA_STOP,

    /* ERR CODE */
    ERR_RTMP_INVALID_URL,
    ERR_DVP_LOOP_TIMEOUT,
    ERR_MIPI_LOOP_TIMEOUT
}CAMSTA;

/*添加:录屏用的参数*/
typedef enum cam_cap_type {
    CAP_IR_JPG = 0,
    CAP_MIPI_JPG,
    CAP_SCREEN_JPG,
    CAP_IR_MIPI_JPG,
}CAPTYPE;

typedef enum cam_record_type {
    CAM_IR_RECORD = 0,
    CAM_MIPI_RECORD,
    CAM_SCREEN_RECORD,
    CAM_IR_MIPI_RECORD,
    CAM_IR_SCREEN_RECORD,
    CAM_MIPI_SCREEN_RECORD,
    CAM_ALL_RECORD,
}RECORDTYPE;


typedef struct Cam_viconfig {
    int width;
    int height;
    int fps;
    int vi_format;
    int vpss_format;
    int vi_depth;       // 默认0
    int vi_bufcnt;      // 默认5
    int vpss_depth;     // 默认1
}CAMVICONFIG;

typedef struct Cam_veconfig {
    int width;
    int height;
    int bitrate;
}CAMVECONFIG;

typedef struct Cam_config {
    CAMVICONFIG vidvp;
    CAMVICONFIG vimipi;
    CAMVECONFIG vedvp;
    int vemipi_bitrate;
}CAMCONFIG;

typedef struct Qt_display {
    void *context;
    void (*get_frame)(void *context, unsigned char *buffer, long long int size, int bytesPerLine, uint64_t u64PTS);
}QTDISPLAY;

typedef struct Qt_getname {
    void *context;
    void (*get_filename)(void *context, const char *name, CAPTYPE type);
}QTGETNAME;

typedef struct Qt_getaixy {
    void *context;
    void (*get_aixy)(void *context, int res_num , int index, char *name, float prop, int x ,int y, int w, int h);
}QTGETAI;

/*
   unsigned char dis_mode
   BIT(0) ----- IR enable
   BIT(1) ----- VL enable
*/
typedef struct Qt_getsta {
    void *context;
    void (*cam_status)(void *context, CAMSTA cam_sta, unsigned char dis_mode, CAMSTA ai_sta, int rtsp_en, int rtmp_en);
}QTGETSTA;

typedef struct Qt_getGasLeak {
    void *context;
    void (*get_gas_leak)(void *context, unsigned long long int Index, double LeakRate, double LeakIntegral, double LeakAvg);
}QTGETGASLEAK;

/***********************************************************************
 *                        register camera
***********************************************************************/
typedef struct Qt_CAM {
    QTDISPLAY dvp_dis;
    QTDISPLAY dvp_raw;
    QTDISPLAY mipi_dis;
    QTGETNAME get_name;
    QTGETAI get_ai;
    QTGETSTA get_sta;
    QTGETGASLEAK gas_leak;
}QTCAM;

/***********************************************************************
 *                        extern fun
***********************************************************************/
/**********************************************************************
 * Description: 获取版本号
 * Parameter:
 * Return: vx.x.x_commitid
 * Note:
 **********************************************************************/
char *qt_camera_get_version(void);

/**********************************************************************
 * Description: 获取设备类型
 * Parameter:
 * Return: 0:FG100 1:FG100IPC 2:FG100PTC
 * Note:
 **********************************************************************/
int qt_camera_get_device_model(void);

/**********************************************************************
 * Description: 注册函数
 * Parameter:
 * Return: SUCCESS/FAILURE
 * Note: 注册QTCAM结构体回调函数及上下文
 **********************************************************************/
int qt_camera_register_get_framer(QTCAM *info);

/**********************************************************************
 * Description: 配置函数
 * Parameter:
 * Return:
 * Note: 配置分辨率码率等,在 qt_camera_init 之前配置,不配置则按默认参数
        其中format:
        YUV420SP    : 0                RGBA8888  : 3
        YUV422_UYVY : 1                BGRA8888  : 4
        YUV422_YUYV : 2                BGRA5551  : 5
 **********************************************************************/
int qt_camera_config(CAMCONFIG *config);

/**********************************************************************
 * Description: 初始化函数
 * Parameter:
 * Return: SUCCESS/FAILURE
 * Note: 初始化 ROCKIT 以及申请双路VI-VPSS通道,对应程序结束要调用 qt_camera_exit
 **********************************************************************/
int qt_camera_init(void);

/**********************************************************************
 * Description: 退出函数
 * Parameter:
 * Return: SUCCESS/FAILURE
 * Note: 释放 ROCKIT 资源,对应 qt_camera_init
 **********************************************************************/
int qt_camera_exit(void);

/**********************************************************************
 * Description: 启动接收帧函数
 * Parameter: DIS_CAMERA_OUT_IR/DIS_CAMERA_OUT_VL
 * Return: SUCCESS/FAILURE
 * Note: 启动DVP/MIPI接收帧函数,启动后接收每帧会回调对应注册函数,需要停止时调用 qt_camera_stop_frame
 **********************************************************************/
int qt_camera_start_frame(CAMSTA dis_sta);

/**********************************************************************
 * Description: 释放使用帧函数
 * Parameter: pts, DIS_CAMERA_OUT_IR/DIS_CAMERA_OUT_VL
 * Return: SUCCESS/FAILURE
 * Note: DVP通路释放或MIPI通路释放,根据PTS与dis_sta类型释放对应帧,对应注册进去的 get_frame 函数,成组对应
 **********************************************************************/
int qt_camera_release_frame(uint64_t pts, CAMSTA dis_sta);

/**********************************************************************
 * Description: 停止接收帧函数
 * Parameter: DIS_CAMERA_OUT_IR/DIS_CAMERA_OUT_VL
 * Return: SUCCESS/FAILURE
 * Note: 根据dis_sta类型停止对应接收帧,对应 qt_camera_start_frame
 **********************************************************************/
int qt_camera_stop_frame(CAMSTA dis_sta);

/**********************************************************************
 * Description: camera控制函数
 * Parameter: cam_sta: CAM_CAMERA_CAPTURE/CAM_CAMERA_RECORDSTART/CAM_CAMERA_RTSPSTART/CAM_CAMERA_RTSPSTOPCAM_CAMERA_RECORDSTOP
 *            name:例如 "20230621_140347" 可为NULL
 *            input:
 *              sta_sta为CAM_CAMERA_RECORDSTART时:
 *              struct input {
                    int mode;   // 0:ir; 1:mipi; 2:screen; 3:ir+mipi; 4:ir+screen; 5:mipi+screen; 6:all(ir+mipi+screen)
                    int aichl;  // 0:dvp; 1:mipi; 2:dump
                    char cardname[20]; // 用于BT MAC地址, 例如 "1C:52:16:AB:DB:88",为 NULL则使用默认声卡
                }
 * Return: SUCCESS/FAILURE
 * Note:
 **********************************************************************/
int qt_camera_cam_control(CAMSTA cam_sta, char *name, void *input);

/**********************************************************************
 * Description: 设置媒体存放位置
 * Parameter:
 * Return: SUCCESS/FAILURE
 * Note: 照片/录像存放位置,缩略图存放位置
 **********************************************************************/
int qt_camera_cam_set_media_path(char *imgtmppath, char *mediapath, char *thumbnailPath);

/***********************************************************************
 *                        RTSP/RTMP
***********************************************************************/
typedef enum cam_rtsp_rtmp_mode {
    CAP_RTSP_RTMP_IR = 0,
    CAP_RTSP_RTMP_VL,
    CAP_RTSP_RTMP_SCREEN
}CAPRTSPRTMPMODE;

/**********************************************************************
 * Description: 设置网络推流的模式
 * Parameter: CAP_RTSP_IR/CAP_RTSP_VL
 * Return: SUCCESS/FAILURE
 * Note: 不支持动态设置,默认为IR
 **********************************************************************/
int qt_camera_rtsp_rtmp_set_mode(CAPRTSPRTMPMODE mode);

/**********************************************************************
 * Description: 设置 RTMP url
 * Parameter: "rtmp://192.168.43.31:1955/live"
 * Return: SUCCESS/FAILURE
 * Note: 不支持动态设置
    RTSP 不需要设置,默认推送 IP+/live/video
    RTMP 需要先设置 url
 **********************************************************************/
int qt_camera_rtmp_set_url(char *url);

/***********************************************************************
 *                        AI
***********************************************************************/
/**********************************************************************
 * Description: 控制AI开关
 * Parameter: AI_CAMERA_START/AI_CAMERA_STOP
 * Return: SUCCESS/FAILURE
 * Note: START时model必须有效,label预留,关闭path可传NULL
 **********************************************************************/
int qt_camera_ai_control(CAMSTA ai_sta, const char *model_path, const char *label_path);

/**********************************************************************
 * Description: AI算法选择
 * Parameter: 0:rockchip 1:洛阳理工
 * Return: SUCCESS/FAILURE
 * Note:
 **********************************************************************/
int qt_camera_ai_set_vendor(int vendor);

/***********************************************************************
 *                        MIPI
***********************************************************************/
/**********************************************************************
 * Description: 设置 mipi 源截取
 * Parameter: 0,0,1920,1080
 * Return: SUCCESS/FAILURE
 * Note: 针对 mipi x+w<=1920 y+h<=1080, x/y (2)对齐 w/h (16)对齐
 **********************************************************************/
int qt_camera_source_vl_set_crop(int x, int y, int w, int h);

/***********************************************************************
 *                        DRM
***********************************************************************/
/**********************************************************************
 * Description: 设置截屏坐标
 * Parameter: 0,0,1920,1080
 * Return: SUCCESS/FAILURE
 * Note: 针对 mipi x+w<=1920 y+h<=1080
 **********************************************************************/
int qt_camera_source_drm_set_crop(int x, int y, int w, int h);

/***********************************************************************
 *                        RECORD
***********************************************************************/
/**********************************************************************
 * Description: 设置阈值每多少帧 raw 数据写一帧
 * Parameter: 1/15
 * Return: SUCCESS/FAILURE
 * Note:
 **********************************************************************/
int qt_camera_record_set_saveraw(int cntnum);

/***********************************************************************
 *                        GAS ANALIT
***********************************************************************/
/**********************************************************************
 * Description: 气体定量检测算法使能
 * Parameter: 0:关闭 1:开启
 * Return: SUCCESS/FAILURE
 * Note:
 **********************************************************************/
int qt_camera_gas_analit_enable(int enable);

/**********************************************************************
 * Description: 气体定量检测参数设定
 * Parameter: 区域: x,y,w,h
 *              距离:distance 单位:米, 默认2.0
 *              过滤强度: filterIntensity 默认128 [+4,-2]
 *              透明度: paintedTransparency 0~255
 *              温度最值: TempMin,TempMax in c*10 (ex.36.7°c= 367)
 *              vendor:  0:russ 1:xian
 * Return: SUCCESS/FAILURE
 * Note:
 **********************************************************************/
int qt_camera_gas_analit_set_region(int x, int y, int w, int h);
int qt_camera_gas_analit_set_distance(float distance);
int qt_camera_gas_analit_set_filterIntensity(unsigned char filterIntensity);
int qt_camera_gas_analit_set_paintedTransparency(unsigned char paintedTransparency);
int qt_camera_gas_analit_set_temperature(int tempMin, int tempMax);
int qt_camera_gas_analit_set_vendor(int vendor);

int camera_config_get(CAMCONFIG *config);

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif /* __cplusplus */

#endif /* _CAMERA_API_H_ */
