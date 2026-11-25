#ifndef _CAMERA_MAIN_H_
#define _CAMERA_MAIN_H_

#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include "rk_mpi_sys.h"
#include "camera_api.h"
#include <drm.h>

extern const char *curtime();
#define r_printf(x, ...)  printf("\r\n\e[31m\e[1m" "%s " x "\e[0m\r\n",curtime() , ## __VA_ARGS__)
#define g_printf(x, ...)  printf("\r\n\e[32m\e[1m" "%s " x "\e[0m\r\n",curtime() ,## __VA_ARGS__)
#define y_printf(x, ...)  printf("\r\n\e[33m\e[1m" "%s " x "\e[0m\r\n",curtime() , ## __VA_ARGS__)

#define line_lnf y_printf("%s,%d",__func__,__LINE__)

#define SUCCESS              0
#define FAILURE              (-1)

#define BIT(x) (0x1 << x)
#define MAX_FILE_PATH_SIZE 128
#define MAX_URL_PATH_SIZE  200

#define CAM_GRP_INDEX_DVP  0
#define CAM_GRP_INDEX_MIPI 1

#define RAIN_ALIGN(x, a)         (((x) + (a) - 1) & ~((a) - 1))
#define RAIN_ALIGN_16(x)         RAIN_ALIGN(x, 16)

#define CONFIG_DVP_SRC_INFO    0
#define CONFIG_DVP_SRC_WIDTH   1280
#define CONFIG_DVP_SRC_HEIGHT  516
//#define CONFIG_MIPI_SRC_WIDTH  3840
//#define CONFIG_MIPI_SRC_HEIGHT 2160
#define CONFIG_MIPI_SRC_WIDTH  1920
#define CONFIG_MIPI_SRC_HEIGHT 1080

#define CONFIG_DRM_SRC_WIDTH   1088
#define CONFIG_DRM_SRC_HEIGHT  1920
#define CONFIG_DRM_CROP_X      0
#define CONFIG_DRM_CROP_Y      0
#define CONFIG_DRM_CROP_WIDTH  1920
#define CONFIG_DRM_CROP_HEIGHT 1080


#define CONFIG_CAP_TIMEOUT 200
#define CONFIG_CAP_ERR_CNT 15


#define INFRARED_FRAME_TAG_55AA                0x55AA
#define INFRARED_FRAME_TAG_1122                0x1122
#define INFRARED_FRAME_TAG_66BB                0x66BB


#define AUDIO_LEN 1152
/**
  rockit

	dvp :
	VI	/dev/video0
		devId 0 pipeId 0 channelId 0
	VPSS
		Grp 0
		ch0 vo
		ch1 cap/recorde
		ch2 rtsp/rtmp
		ch3 ai

	mipi :
	VI	/dev/video22
		devId 3 pipeId 3 channelId 3
	VPSS
		Grp 1
		ch0 vo
		ch1 cap/recorde
		ch2 rtsp/rtmp

	VENC :
		ch0 dvp JPG/H264
		ch1 mipi JPG/H264
		ch2 drm JPG/H264
		ch3 rtsp/rtmp

 **/

//drm
#include <sys/stat.h>
#include <xf86drmMode.h>
#define CRTC_INDEX 0
typedef unsigned int guint;
typedef int gint;

#include <sys/mman.h>
typedef struct camdrm_info{
	int fdcard;
	guint crtc_id;
	gint dmafd[4];
	struct stat st[4];
	char *vaddr;
	drmModeFB2Ptr fb2;
	//weston多缓冲轮转/旋转屏幕后会切换fb，录屏循环要按当前fb_id动态更新mmap
	guint cur_fb_id;
	struct camdrm_fb_cache {
		guint fb_id;
		char *vaddr;
		size_t size;
		int dmafd;
		drmModeFB2Ptr fb2;
	} fbcache[4];
}CAMDRMINFO;

typedef struct VippContext
{
	int ccd_index; //0 dvp; 3 mipi;

	//vi
    RK_S32 width;
    RK_S32 height;
    RK_S32 devId;
    RK_S32 pipeId;
    RK_S32 channelId;
    RK_S32 s32Grp;
    RK_S32 bCfg;
    RK_S32 fps;
    RK_S32 u32Depth;
    RK_S32 vpssu32Depth;
    RK_S32 u32BufCount;
    char aEntityName[20];
	PIXEL_FORMAT_E enPixelFormat;
	PIXEL_FORMAT_E vpssenPixelFormat;
    pthread_t viThreadID;
}VippContext;

typedef struct VoContext
{
	//vo
}VoContext;

enum venc_type{
	VENC_DVP_JPG = 0,
	VENC_MIPI_JPG,
	VENC_DVP_H264,
	VENC_MIPI_H264,
	VENC_DRM_JPG,
	VENC_DRM_H264,
	VENC_DRM_H264_ROT
};

typedef struct VencContext
{
	//venc
    RK_S32 width;
    RK_S32 height;
    RK_S32 s32Grp;
    RK_S32 s32GrpChnId;
    RK_S32 veId;
    RK_S32 veChn;
	RK_U32 bitrate;
	PIXEL_FORMAT_E enPixelFormat;
	RK_CODEC_ID_E enType;
}VencContext;

struct Mpp_api
{
	int (*vi_init)(VippContext *vContext);
	int (*vo_init)(VoContext *pContext);
	int (*create_vi)(VippContext *vContext);
	int (*create_vo)(VippContext *vContext, VoContext *pContext);
	int (*start_vi)(VippContext *vContext);
	int (*start_vo)(VippContext *vContext, VoContext *pContext);
	int (*stop_vi)(VippContext *vContext);
	int (*stop_vo)(VoContext *pContext);
	int (*destroy_vi)(VippContext *vContext);
	int (*destroy_vo)(VoContext *pContext);
};
struct Mpp_venc_api
{
	int (*ve_init)(VencContext *eContext, enum venc_type);
	int (*create_ve)(VencContext *eContext);
	int (*start_ve)(VencContext *eContext);
	int (*stop_ve)(VencContext *eContext);
	int (*destroy_ve)(VencContext *eContext);
};

struct cam_context{
	VencContext camVeContext[3];
	struct Mpp_venc_api cam_api;
};
struct dis_context{
	VippContext disViContext[2];
	//VoContext disVoContext[2];
	struct Mpp_api dis_api;
};

struct camera_msg{
    pthread_t PkgDealThreadID;
	char pthread_sta;

    pthread_t camThreadID;
	struct cam_context camContext;

    pthread_t disThreadID;
	struct dis_context disContext;

    pthread_t aiThreadID;

	QTCAM qtinfo;
};

typedef enum _save_filetype{
	SAVE_CAPTURE_JPG = 0,
	SAVE_RECORDER_MP4
}SAVE_FILETYPE;

/*
   unsigned char dis_mode
   BIT(0)   -----  IR enable
   BIT(1)   -----  VL enable
*/
struct event_msg {
	volatile int cam_initok;
	int conf_ok;
	unsigned char dis_mode;
	enum camera_status cam_sta;
	enum camera_status dis_sta;
	enum camera_status ai_sta;
	char thumbnailPath[MAX_FILE_PATH_SIZE];
	char mediaPath[MAX_FILE_PATH_SIZE];
	char imgTmpPath[MAX_FILE_PATH_SIZE];
	char record_path[4][MAX_FILE_PATH_SIZE];
	char capture_path[5][MAX_FILE_PATH_SIZE];
	char ai_path[2][MAX_FILE_PATH_SIZE];
	char rtmp_url[MAX_URL_PATH_SIZE];
	int mipi_source_x;
	int mipi_source_y;
	int mipi_source_w;
	int mipi_source_h;
	int raw_cnt_num;
	int rtsp_en;
	int rtmp_en;
	int drm_source_x;
	int drm_source_y;
	int drm_source_w;
	int drm_source_h;
	int dvp_source_x;
	int dvp_source_y;
	int dvp_source_w;
	int dvp_source_h;
	int gas_analit_en;
	int gas_analit_x;
	int gas_analit_y;
	int gas_analit_w;
	int gas_analit_h;
	float gas_analit_distance;
	unsigned char gas_analit_filterIntensity;
	unsigned char gas_analit_paintedTransparency;
	int gas_analit_tempMin;
	int gas_analit_tempMax;
	int gas_analit_vendor;
};
struct camera_sem{
	unsigned char exit_flag;
	sem_t PkgSem;
	sem_t camSem;
	sem_t disSem;
	sem_t aiSem;
	sem_t gasQuantificationeSem;
};

extern struct event_msg emsg;
extern struct camera_sem csem;
extern struct camera_msg cmsg;
extern CAMCONFIG camera_conf;

/*
enable vp0 vp2 vp3

&vp0 {
	rockchip,plane-mask = <(1 << ROCKCHIP_VOP2_CLUSTER0 | 1 << ROCKCHIP_VOP2_ESMART0)>;
	rockchip,primary-plane = <ROCKCHIP_VOP2_ESMART0>;
};

&vp1 {
	/delete-property/ rockchip,plane-mask;
	/delete-property/ rockchip,primary-plane;
};

&vp2 {
	rockchip,plane-mask = <(1 << ROCKCHIP_VOP2_CLUSTER2 | 1 << ROCKCHIP_VOP2_ESMART2 |
							 1 << ROCKCHIP_VOP2_CLUSTER1 | 1 << ROCKCHIP_VOP2_ESMART1)>;
	rockchip,primary-plane = <ROCKCHIP_VOP2_ESMART2>;
};

&vp3 {
	rockchip,plane-mask = <(1 << ROCKCHIP_VOP2_CLUSTER3 | 1 << ROCKCHIP_VOP2_ESMART3)>;
	rockchip,primary-plane = <ROCKCHIP_VOP2_ESMART3>;
};

 */

#define RK358X_VO_DEV_HD0 0
#define RK358X_VO_DEV_HD1 1
#define RK358X_VO_DEV_HD2 2
#define RK358X_VO_DEV_HD3 3
#define RK358X_VOP_LAYER_CLUSTER_0 0
#define RK358X_VOP_LAYER_CLUSTER_1 1
#define RK358X_VOP_LAYER_CLUSTER_2 2
#define RK358X_VOP_LAYER_CLUSTER_3 3
#define RK358X_VOP_LAYER_ESMART_0 4
#define RK358X_VOP_LAYER_ESMART_1 5
#define RK358X_VOP_LAYER_ESMART_2 6
#define RK358X_VOP_LAYER_ESMART_3 7

/**********************
  * extern fun
**********************/
//sem
extern int sys_sem_init(void);
extern int sys_sem_deinit(void);
//mpp api
extern int mpp_fun_init(struct Mpp_api *api,VippContext *vContext,VoContext *oContext);
extern int mpp_fun_run(struct Mpp_api *api,VippContext *vContext,VoContext *oContext);
extern int mpp_fun_stop(struct Mpp_api *api,VippContext *vContext,VoContext *oContext);
extern int mpp_fun_destroy(struct Mpp_api *api,VippContext *vContext,VoContext *oContext);
extern void display_get_api(struct Mpp_api *api);
extern void mpp_ve_get_api(struct Mpp_venc_api *api);
//mpp cam
extern void camera_wbc_set_loop(unsigned char loop);
extern void *camera_mpp_dvp_loop(void *p);
extern void *camera_mpp_mipi_loop(void *p);
extern void camera_dvp_set_loop(unsigned char loop);
extern void camera_mipi_set_loop(unsigned char loop);
extern void camera_mpp_dvp_release(uint64_t pts);
extern void camera_mpp_mipi_release(uint64_t pts);
extern RK_U64 TEST_COMM_GetNowUs();
extern uint16_t check_infrared_frame_tag(void *data);
extern void get_save_filename(SAVE_FILETYPE type);
extern FILE *test_file_create(char *file);
extern void test_file_write(FILE *fp, void *data , int len);
extern void test_file_close(FILE *fp);
//config
extern int camera_config(CAMCONFIG *config);
//cam control
extern int camera_takepicture_start(struct cam_context *msg);
extern int camera_thumbnail_start(struct cam_context *msg, int mode);
extern int camera_recorder_start(struct cam_context *msg);
extern int camera_recorder_stop(struct cam_context *msg);
extern int camera_rtsp_start(struct cam_context *msg);
extern int camera_rtsp_stop(struct cam_context *msg);
extern int camera_rtmp_start(struct cam_context *msg);
extern int camera_rtmp_stop(struct cam_context *msg);
extern CAPRTSPRTMPMODE camera_rtsp_get_mode(void);
//ai control
extern int camera_ai_start(void *p);
extern void camera_ai_set_odet_loop(unsigned char loop);
//qt control
extern void camera_rtsp_set_mode(CAPRTSPRTMPMODE mode);
extern int camera_mpp_mipi_set_crop(int x, int y, int w, int h);
extern void camera_update_qt_sta(CAMSTA once_call);
//drm
extern int drm_crtc_init(CAMDRMINFO *info);
extern int drm_crtc_deinit(CAMDRMINFO *info);
extern int drm_crtc_update_fb(CAMDRMINFO *info, guint fb_id);
extern guint camera_drm_wait_vblank(CAMDRMINFO *info);
extern drmModeCrtcPtr camera_drm_wait_vblank_2(CAMDRMINFO *info);
extern drmModeCrtcPtr camera_drm_wait_vblank_2_timeout(CAMDRMINFO *info, int timeout_ms);
extern void camera_gst_kms_src_free_crtc_fb (drmModeCrtcPtr crtc);
/* 读 weston 当前输出旋转角, 换算成截图/录屏时 RGA 应用的旋转角(度, 90/270)。
 * 用它抵消 weston 已做的旋转, 使输出方向与屏幕旋转无关。 */
extern int camera_drm_get_rga_rotate_deg(void);

int rga_cvtcolor_demo(void *src_buf, void *dst_buf, int w, int h, int rotate_deg);
int mp3_encoder(int *quit, void *p); 


#endif  /* _CAMERA_MAIN_H_ */
