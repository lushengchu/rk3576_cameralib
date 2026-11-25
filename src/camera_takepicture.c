#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <errno.h>
#include "rk_mpi_vpss.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "camera_main.h"

enum camera_save_cap{
	ACK_CAMERA_CAPTURE0 = 0,
	ACK_CAMERA_CAPTURE1,
	ACK_CAMERA_CAPTURE2,
	ACK_CAMERA_CAPTURE3,
	ACK_CAMERA_CAPTURE4,
	ACK_CAMERA_ERR
};

struct cap_context{
	CAPTYPE mode;
	struct cam_context *msg;
};
/**********************
  * JPG
**********************/
static int camera_takepicture_vpss_init(VencContext *eContext)
{
	RK_S32 s32Ret = 0;
	//use chn 1
	VPSS_CHN_ATTR_S stVpssChnAttr;
	memset(&stVpssChnAttr, 0x0 ,sizeof(VPSS_CHN_ATTR_S));
	stVpssChnAttr.enChnMode = VPSS_CHN_MODE_PASSTHROUGH;
	stVpssChnAttr.enDynamicRange = DYNAMIC_RANGE_SDR8;
	stVpssChnAttr.enPixelFormat = eContext->enPixelFormat;
	stVpssChnAttr.stFrameRate.s32SrcFrameRate = -1;
	stVpssChnAttr.stFrameRate.s32DstFrameRate = -1;
	stVpssChnAttr.u32Width = eContext->width;
	stVpssChnAttr.u32Height = eContext->height;
	//if(eContext->s32Grp == 0){
	//	stVpssChnAttr.u32Height = eContext->height;
	//}
	stVpssChnAttr.enCompressMode = COMPRESS_MODE_NONE;
	stVpssChnAttr.u32Depth = 1;

	s32Ret = RK_MPI_VPSS_SetChnAttr(eContext->s32Grp, VPSS_CHN1, &stVpssChnAttr);
	RK_LOGE("RK_MPI_VPSS_SetChnAttr %d %x",eContext->s32Grp, s32Ret);

	s32Ret = RK_MPI_VPSS_EnableChn(eContext->s32Grp, VPSS_CHN1);
	RK_LOGE("RK_MPI_VPSS_EnableChn %d %x",eContext->s32Grp, s32Ret);

	return s32Ret;
}
#if 0
static int camera_takepicture_init(struct cam_context *msg , int mode, int dvp_en)
{
	RK_S32 s32Ret = 0;
	VencContext *eContext = (VencContext *)&msg->camVeContext;
	struct Mpp_venc_api e_api = msg->cam_api;

	switch(mode){
		case 1:
			//init dvp
			e_api.ve_init(&eContext[0], VENC_DVP_JPG);
			//init dump
			e_api.ve_init(&eContext[2], VENC_DRM_JPG);
			camera_takepicture_vpss_init(&eContext[0]);
			e_api.create_ve(&eContext[0]);
			e_api.start_ve(&eContext[0]);
			e_api.create_ve(&eContext[2]);
			e_api.start_ve(&eContext[2]);
			break;
		case 2:
			if(dvp_en){
				//init dvp
				e_api.ve_init(&eContext[0], VENC_DVP_JPG);
				camera_takepicture_vpss_init(&eContext[0]);
				e_api.create_ve(&eContext[0]);
				e_api.start_ve(&eContext[0]);
			}
			//init mipi
			e_api.ve_init(&eContext[1], VENC_MIPI_JPG);
			//init dump
			e_api.ve_init(&eContext[2], VENC_DRM_JPG);

			camera_takepicture_vpss_init(&eContext[1]);
			for(int i = 1; i<3; i++){
				e_api.create_ve(&eContext[i]);
				e_api.start_ve(&eContext[i]);
			}
			break;
		default:
			//init dvp
			e_api.ve_init(&eContext[0], VENC_DVP_JPG);
			//init mipi
			e_api.ve_init(&eContext[1], VENC_MIPI_JPG);

			for(int i = 0; i<2; i++){
				camera_takepicture_vpss_init(&eContext[i]);
				e_api.create_ve(&eContext[i]);
				e_api.start_ve(&eContext[i]);
			}
			break;
	}

    return s32Ret;
}
#endif

static void camera_cap_send_filename(enum camera_save_cap event)
{
	switch(event){
		case ACK_CAMERA_CAPTURE0:
		case ACK_CAMERA_CAPTURE1:
		case ACK_CAMERA_CAPTURE2:
		case ACK_CAMERA_CAPTURE3:
		case ACK_CAMERA_CAPTURE4:
			cmsg.qtinfo.get_name.get_filename(cmsg.qtinfo.get_name.context,emsg.capture_path[event - ACK_CAMERA_CAPTURE0], event - ACK_CAMERA_CAPTURE0);
			memset(emsg.capture_path[event - ACK_CAMERA_CAPTURE0],0x0,sizeof(emsg.capture_path[event - ACK_CAMERA_CAPTURE0]));
			break;
		case ACK_CAMERA_ERR:
			if ((emsg.cam_sta<=CAM_CAMERA_CAPTURE_ALL) && (emsg.cam_sta >= CAM_CAMERA_CAPTURE_IR)) {
				g_printf("%s ACK_CAMERA_ERR",__func__);
				cmsg.qtinfo.get_name.get_filename(cmsg.qtinfo.get_name.context, "error", 0);
			}
		default:
			break;
	}
}

#include <sys/stat.h>
#include <libgen.h>
#include <limits.h>
#include <errno.h>
#include <string.h>

static int ensure_dir_for_file(const char *filepath)
{
    char path[PATH_MAX];
    char *p = NULL;
    struct stat st;
    size_t len;

    if (filepath == NULL || *filepath == '\0')
        return -1;

    snprintf(path, sizeof(path), "%s", filepath);
    len = strlen(path);
    if (len == 0 || len >= sizeof(path))
        return -1;

    if (path[len - 1] == '/') {
        path[len - 1] = '\0';
    }

    for (p = path + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (stat(path, &st) != 0) {
                if (errno == ENOENT) {
                    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
                        *p = '/';
                        return -1;
                    }
                } else {
                    *p = '/';
                    return -1;
                }
            } else if (!S_ISDIR(st.st_mode)) {
                *p = '/';
                errno = ENOTDIR;
                return -1;
            }
            *p = '/';
        }
    }

    {
        char *dir = dirname(path);
        if (dir == NULL)
            return -1;
        if (stat(dir, &st) != 0) {
            if (errno == ENOENT) {
                if (mkdir(dir, 0755) != 0 && errno != EEXIST)
                    return -1;
            } else {
                return -1;
            }
        } else if (!S_ISDIR(st.st_mode)) {
            errno = ENOTDIR;
            return -1;
        }
    }

    return 0;
}

static void *camera_takepicture_dvp_jpg_start(void *p)
{
	RK_S32 s32Ret = 0;
	struct cap_context *pt = (struct cap_context *)p;
	//int mode = pt->mode;
	struct cam_context *msg = pt->msg;
	VencContext *eContext = (VencContext *)&msg->camVeContext;
	struct Mpp_venc_api e_api = msg->cam_api;
    VENC_STREAM_S stFrame;
	stFrame.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));
	FILE *fp = NULL;
    
    const char *path = emsg.capture_path[0];

    if (ensure_dir_for_file(path) != 0) {
        r_printf("%s %d create dir for %s failed errno=%d(%s)",
                __func__, __LINE__, path, errno, strerror(errno));
        camera_cap_send_filename(ACK_CAMERA_ERR);
        free(stFrame.pstPack);
        return NULL;
    }

    fp = fopen(path, "wb+");
    if (fp == NULL) {
        r_printf("%s %d fopen %s failed errno=%d(%s)",
                __func__, __LINE__, path, errno, strerror(errno));
        camera_cap_send_filename(ACK_CAMERA_ERR);
        free(stFrame.pstPack);
        return NULL;
    }
    g_printf("%s path=%s strat", __func__, emsg.capture_path[0]);
	VIDEO_FRAME_INFO_S stViFrame;
	memset(&stViFrame, 0x0 ,sizeof(VIDEO_FRAME_INFO_S));
	/*
	   BIT(0) raw 55aa
	   BIT(1) yuv 66bb
	   BIT(2) jpg 1122
	*/
    RK_S32 waitTime = 22; // 1/pContext->fps*1000
	void *data = NULL;
	void *pData = NULL;
	//init dvp
	e_api.ve_init(&eContext[0], VENC_DVP_JPG);
	camera_takepicture_vpss_init(&eContext[0]);
	e_api.create_ve(&eContext[0]);
	e_api.start_ve(&eContext[0]);
	//int dvplen = eContext[0].width * (eContext[0].height) * 2;
	int errCount = 0;
	while (1) 
	{
		if(errCount >= CONFIG_CAP_ERR_CNT){
			camera_cap_send_filename(ACK_CAMERA_ERR);
			fclose(fp);
			break;
		}

		s32Ret = RK_MPI_VPSS_GetChnFrame(eContext[0].s32Grp, VPSS_CHN1, &stViFrame, waitTime);
		if (s32Ret == RK_SUCCESS) {
			data = RK_MPI_MB_Handle2VirAddr(stViFrame.stVFrame.pMbBlk);
			RK_MPI_SYS_MmzFlushCache(stViFrame.stVFrame.pMbBlk, RK_TRUE);
#if 0
			if(!(save_frame&BIT(0)) && (check_infrared_frame_tag(data) == INFRARED_FRAME_TAG_55AA)){
				save_frame |= BIT(0);
				fwrite(data, 1, dvplen, fp1);
				fflush(fp1);
				fclose(fp1);
				camera_cap_send_filename(ACK_CAMERA_CAPTURE1);
			}else if(!(save_frame&BIT(1)) && (check_infrared_frame_tag(data) == INFRARED_FRAME_TAG_66BB)){
				save_frame |= BIT(1);
				fwrite(data, 1, dvplen, fp2);
				fflush(fp2);
				fclose(fp2);
				camera_cap_send_filename(ACK_CAMERA_CAPTURE2);
			}else if(!(save_frame&BIT(2)) && (check_infrared_frame_tag(data) == INFRARED_FRAME_TAG_1122)){
#endif
			//	stViFrame.stVFrame.u32Height -= 2;
			//	stViFrame.stVFrame.u32VirHeight -= 2;
				/* JPEG 高度按 16 对齐后比 VPSS 帧矮，按编码器尺寸送，避免 SendFrame 因尺寸不一致失败。 */
				if(stViFrame.stVFrame.u32Width > (RK_U32)eContext[0].width)
					stViFrame.stVFrame.u32Width = eContext[0].width;
				if(stViFrame.stVFrame.u32VirWidth > (RK_U32)eContext[0].width)
					stViFrame.stVFrame.u32VirWidth = eContext[0].width;
				if(stViFrame.stVFrame.u32Height > (RK_U32)eContext[0].height)
					stViFrame.stVFrame.u32Height = eContext[0].height;
				if(stViFrame.stVFrame.u32VirHeight > (RK_U32)eContext[0].height)
					stViFrame.stVFrame.u32VirHeight = eContext[0].height;
				RK_MPI_VENC_SendFrame(eContext[0].veChn, &stViFrame, CONFIG_CAP_TIMEOUT);
				s32Ret = RK_MPI_VENC_GetStream(eContext[0].veChn, &stFrame, CONFIG_CAP_TIMEOUT);
				pData = (s32Ret == RK_SUCCESS) ? RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk) : NULL;
				if(pData == NULL || stFrame.pstPack->u32Len == 0){
					//拿不到编码流则释放后重试，避免 fwrite(NULL) 段错误
					r_printf("%s dvp GetStream fail %x",__func__, s32Ret);
					if(s32Ret == RK_SUCCESS)
						RK_MPI_VENC_ReleaseStream(eContext[0].veChn, &stFrame);
					RK_MPI_VPSS_ReleaseChnFrame(eContext[0].s32Grp, VPSS_CHN1, &stViFrame);
					errCount ++;
					continue;
				}
				fwrite(pData, 1, stFrame.pstPack->u32Len, fp);
				fflush(fp);
				fclose(fp);
				camera_cap_send_filename(ACK_CAMERA_CAPTURE0);
				RK_MPI_VENC_ReleaseStream(eContext[0].veChn, &stFrame);
                RK_LOGE("sclu dvp tackpicture ok\n");
#if 0
			}else {
				RK_MPI_VPSS_ReleaseChnFrame(eContext[0].s32Grp, VPSS_CHN1, &stViFrame);
				errCount ++; 
				continue;
			}
#endif
			RK_MPI_VPSS_ReleaseChnFrame(eContext[0].s32Grp, VPSS_CHN1, &stViFrame);
            break;
		}else {
			errCount ++; 
			continue;
		}
	}
	free(stFrame.pstPack);
	e_api.stop_ve(&eContext[0]);
	e_api.destroy_ve(&eContext[0]);
	//不能再 DisableChn(VPSS_CHN1):CHN1 是 mpp_create_vi 在 StartGrp 前为录像预建的共享通道,
	//拍照只是借用;若在此关闭,拍照后再录像时录像线程 GetChnFrame 取不到帧(0 帧、MP4 无法播放)。

	g_printf("%s end",__func__);
    return NULL;
}
#if 0
static void *camera_takepicture_mipi_jpg_start_test(void *p)
{
	RK_S32 s32Ret = 0;
	g_printf("%s strat",__func__);
	struct cam_context *msg = (struct cam_context *)p;
	VencContext *eContext = (VencContext *)&msg->camVeContext;
	struct Mpp_venc_api e_api = msg->cam_api;
	VIDEO_FRAME_INFO_S stViFrame;
	memset(&stViFrame, 0x0 ,sizeof(VIDEO_FRAME_INFO_S));
	FILE *fp = NULL;
	fp = fopen("/tmp/mipi.jpg", "wb+");

	VENC_STREAM_S stFrame;
	stFrame.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));
	void *pData = NULL;
	void *data = NULL;
	s32Ret = RK_MPI_VPSS_GetChnFrame(eContext[1].s32Grp, VPSS_CHN1, &stViFrame, -1);
	if (s32Ret == RK_SUCCESS) {
		//data = RK_MPI_MB_Handle2VirAddr(stViFrame.stVFrame.pMbBlk);
		RK_MPI_SYS_MmzFlushCache(stViFrame.stVFrame.pMbBlk, RK_TRUE);
		g_printf("vif h %d vh %d len %lu",stViFrame.stVFrame.u32Height,stViFrame.stVFrame.u32VirHeight,RK_MPI_MB_GetLength(stViFrame.stVFrame.pMbBlk));
		RK_MPI_VENC_SendFrame(eContext[1].veChn, &stViFrame, -1);
		RK_MPI_VENC_GetStream(eContext[1].veChn, &stFrame, -1);
		pData= RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
		g_printf("get frm size %u ",stFrame.pstPack->u32Len);
		fwrite(pData, 1, stFrame.pstPack->u32Len, fp);
		//fwrite(data, 1,	RK_MPI_MB_GetLength(stViFrame.stVFrame.pMbBlk), fp);
		fflush(fp);
	}
	free(stFrame.pstPack);
	fclose(fp);
	e_api.stop_ve(&eContext[1]);
	e_api.destroy_ve(&eContext[1]);
	s32Ret = RK_MPI_VPSS_DisableChn(eContext[1].s32Grp, VPSS_CHN1);
	RK_LOGE("RK_MPI_VPSS_DisableChn %d %x",eContext[1].s32Grp, s32Ret);
	g_printf("%s end",__func__);

	return NULL;
}
#endif

static void *camera_takepicture_mipi_jpg_start(void *p)
{
	RK_S32 s32Ret = 0;
	g_printf("%s start",__func__);
	struct cam_context *msg = (struct cam_context *)p;
	VencContext *eContext = (VencContext *)&msg->camVeContext;
	struct Mpp_venc_api e_api = msg->cam_api;
    VENC_STREAM_S stFrame;
	stFrame.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));
	if(stFrame.pstPack == NULL){
		r_printf("stFrame.pstPack malloc err!!");
		return NULL;
	}
    FILE *fp3 = NULL;
    const char *path = emsg.capture_path[3];

    if (ensure_dir_for_file(path) != 0) {
        r_printf("%s %d create dir for %s failed errno=%d(%s)",
                __func__, __LINE__, path, errno, strerror(errno));
        camera_cap_send_filename(ACK_CAMERA_ERR);
        free(stFrame.pstPack);
        return NULL;
    }

    fp3 = fopen(path, "wb+");
    if (fp3 == NULL) {
        r_printf("%s %d fopen %s failed errno=%d(%s)",
                __func__, __LINE__, path, errno, strerror(errno));
        camera_cap_send_filename(ACK_CAMERA_ERR);
        free(stFrame.pstPack);
        return NULL;
    }
    //init mipi
    e_api.ve_init(&eContext[1], VENC_MIPI_JPG);
    camera_takepicture_vpss_init(&eContext[1]);
	e_api.create_ve(&eContext[1]);
	e_api.start_ve(&eContext[1]);

	/* 和录像一样手动送帧。绑定 VPSS 后 JPEG 经常收不到流，可见光 jpg 就是空的。 */
	VIDEO_FRAME_INFO_S stViFrame;
	memset(&stViFrame, 0x0, sizeof(VIDEO_FRAME_INFO_S));
	int drop_frame = 0;
	int errCount = 0;
	while(1)
	{
		if(errCount >= CONFIG_CAP_ERR_CNT){
			camera_cap_send_filename(ACK_CAMERA_ERR);
			fclose(fp3);
			break;
		}
		s32Ret = RK_MPI_VPSS_GetChnFrame(eContext[1].s32Grp, VPSS_CHN1, &stViFrame, CONFIG_CAP_TIMEOUT);
		if (s32Ret != RK_SUCCESS) {
			errCount++;
			continue;
		}
		if(++drop_frame <= 5){
			RK_MPI_VPSS_ReleaseChnFrame(eContext[1].s32Grp, VPSS_CHN1, &stViFrame);
			printf("mipi drop %d\n", drop_frame);
			continue;
		}
		RK_MPI_SYS_MmzFlushCache(stViFrame.stVFrame.pMbBlk, RK_TRUE);
		if(stViFrame.stVFrame.u32Width > (RK_U32)eContext[1].width)
			stViFrame.stVFrame.u32Width = eContext[1].width;
		if(stViFrame.stVFrame.u32VirWidth > (RK_U32)eContext[1].width)
			stViFrame.stVFrame.u32VirWidth = eContext[1].width;
		if(stViFrame.stVFrame.u32Height > (RK_U32)eContext[1].height)
			stViFrame.stVFrame.u32Height = eContext[1].height;
		if(stViFrame.stVFrame.u32VirHeight > (RK_U32)eContext[1].height)
			stViFrame.stVFrame.u32VirHeight = eContext[1].height;
		RK_MPI_VENC_SendFrame(eContext[1].veChn, &stViFrame, CONFIG_CAP_TIMEOUT);
		s32Ret = RK_MPI_VENC_GetStream(eContext[1].veChn, &stFrame, CONFIG_CAP_TIMEOUT);
		void *pData = (s32Ret == RK_SUCCESS) ? RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk) : NULL;
		if(pData == NULL || stFrame.pstPack->u32Len == 0){
			r_printf("%s mipi GetStream fail %x", __func__, s32Ret);
			if(s32Ret == RK_SUCCESS)
				RK_MPI_VENC_ReleaseStream(eContext[1].veChn, &stFrame);
			RK_MPI_VPSS_ReleaseChnFrame(eContext[1].s32Grp, VPSS_CHN1, &stViFrame);
			errCount++;
			continue;
		}
		fwrite(pData, 1, stFrame.pstPack->u32Len, fp3);
		fflush(fp3);
		fclose(fp3);
		camera_cap_send_filename(ACK_CAMERA_CAPTURE3);
		RK_MPI_VENC_ReleaseStream(eContext[1].veChn, &stFrame);
		RK_MPI_VPSS_ReleaseChnFrame(eContext[1].s32Grp, VPSS_CHN1, &stViFrame);
		RK_LOGE("sclu mipi tackpicture ok\n");
		break;
	}
	free(stFrame.pstPack);
	e_api.stop_ve(&eContext[1]);
	e_api.destroy_ve(&eContext[1]);
	//不能再 DisableChn(VPSS_CHN1):CHN1 为录像预建的共享通道,拍照只借用;关闭会导致拍照后录像取不到帧。

	g_printf("%s end",__func__);

    return NULL;
}

static void *camera_takepicture_drm_jpg_start(void *p)
{
	RK_S32 s32Ret = 0;
	g_printf("%s %d",__func__, __LINE__);
	struct cap_context *pt = (struct cap_context *)p;
	//int mode = pt->mode;
	struct cam_context *msg = pt->msg;
	VencContext *eContext = (VencContext *)&msg->camVeContext;
	struct Mpp_venc_api e_api = msg->cam_api;
    VENC_STREAM_S stFrame;
	stFrame.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));
	FILE *fp = NULL;
    const char *path = emsg.capture_path[4];

    if (ensure_dir_for_file(path) != 0) {
        r_printf("%s %d create dir for %s failed errno=%d(%s)",
                __func__, __LINE__, path, errno, strerror(errno));
        camera_cap_send_filename(ACK_CAMERA_ERR);
        free(stFrame.pstPack);
        return NULL;
    }

    fp = fopen(path, "wb+");
    if (fp == NULL) {
        r_printf("%s %d fopen %s failed errno=%d(%s)",
                __func__, __LINE__, path, errno, strerror(errno));
        camera_cap_send_filename(ACK_CAMERA_ERR);
        free(stFrame.pstPack);
        return NULL;
    }
    g_printf("%s %d",__func__, __LINE__);
	//init dump
	e_api.ve_init(&eContext[2], VENC_DRM_JPG);
	e_api.create_ve(&eContext[2]);
	e_api.start_ve(&eContext[2]);

	//drm
	CAMDRMINFO drminfo;
	memset(&drminfo, 0x0 , sizeof(CAMDRMINFO));
	g_printf("%s %d",__func__, __LINE__);
	drm_crtc_init(&drminfo);

	g_printf("%s %d",__func__, __LINE__);
	//拍照仍走 rga：RK3576 的 JPEG 硬件编码器(hal_jpege_vpu720)不支持 BGRA8888 输入
	//(报 "Unsupported input format 0x0001000c")。只有 SetChnRotation!=0 时 VENC 才会先做 CSC
	//预处理把 BGRA 转 YUV，ROTATION_0 会把裸 BGRA 直接送 JPEG 核 → 花屏。即 BGRA+VENC 旋转
	//无法得到"不旋转(0°)"的正常 JPEG。故拍照用 rga_cvtcolor_demo 把 BGRA 转 UYVY 并按需旋转，
	//喂 JPEG 编码器支持的 UYVY；录像/rtsp 是 H264(支持 BGRA)，仍走 VENC 零拷贝旋转。
	int len = eContext[2].width * eContext[2].height * 4;
	// for input image pool
	MB_POOL_CONFIG_S stMbPoolCfg;
	memset(&stMbPoolCfg, 0, sizeof(MB_POOL_CONFIG_S));
	stMbPoolCfg.u64MBSize = len;
	stMbPoolCfg.u32MBCnt  = 1;
	stMbPoolCfg.enAllocType = MB_ALLOC_TYPE_DMA;
	stMbPoolCfg.bPreAlloc = RK_TRUE;
	MB_POOL pool = RK_MPI_MB_CreatePool(&stMbPoolCfg);
	MB_BLK blk = RK_MPI_MB_GetMB(pool, len, RK_TRUE);
	VIDEO_FRAME_INFO_S stViFrame;
	memset(&stViFrame, 0x0 ,sizeof(VIDEO_FRAME_INFO_S));
	stViFrame.stVFrame.pMbBlk = blk;
	stViFrame.stVFrame.u32Width = eContext[2].height;
	stViFrame.stVFrame.u32Height = eContext[2].width;
	stViFrame.stVFrame.u32VirWidth = eContext[2].height;
	stViFrame.stVFrame.u32VirHeight = eContext[2].width;
	stViFrame.stVFrame.enPixelFormat = RK_FMT_YUV422_UYVY;
	//stViFrame.stVFrame.u32FrameFlag |= s32ReachEOS ? FRAME_FLAG_SNAP_END : 0;
	stViFrame.stVFrame.enCompressMode = COMPRESS_MODE_NONE;

	RK_U8 *pVirAddr = (RK_U8 *)(RK_MPI_MB_Handle2VirAddr(blk));
	{
		//wait_vblank返回的是最新帧的fb_id，weston多缓冲轮转后要用它更新映射，
		//否则截到的可能不是最新帧（旋转屏幕后同样要跟随）
		guint new_fb = camera_drm_wait_vblank(&drminfo);
		drm_crtc_update_fb(&drminfo, new_fb);
	}

   // printf("eContext[2].width=%d eContext[2].height=%d drminfo.st[0].st_size=%d\n", eContext[2].width, eContext[2].height, drminfo.st[0].st_size);

    // 修复白屏闪烁：DMA缓冲区同步，确保GPU写入的数据对CPU可见
    struct dma_buf_sync sync_start;
    sync_start.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
    ioctl(drminfo.dmafd[0], DMA_BUF_IOCTL_SYNC, &sync_start);

    //按 weston 真实输出的 fb 尺寸导入(实测横屏 1920x1080)。旋转角动态取自
    //camera_drm_get_rga_rotate_deg()(抵消 weston 旋转，使输出相对桌面正立)。
    int cap_fbw = drminfo.fb2 ? (int)drminfo.fb2->width  : 1920;
    int cap_fbh = drminfo.fb2 ? (int)drminfo.fb2->height : 1080;
    rga_cvtcolor_demo(drminfo.vaddr, pVirAddr, cap_fbw, cap_fbh, camera_drm_get_rga_rotate_deg());

    // DMA缓冲区同步结束
    struct dma_buf_sync sync_end;
    sync_end.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    ioctl(drminfo.dmafd[0], DMA_BUF_IOCTL_SYNC, &sync_end);

	RK_MPI_SYS_MmzFlushCache(blk, RK_FALSE);
#if 0
    FILE *fp = fopen("/tmp/in0w1920-h1080-bgra8888.bin", "wb+");
    fwrite(drminfo.vaddr, 1, drminfo.st[0].st_size, fp);
    fflush(fp);
    printf("sclu rgb888 data size=%d", drminfo.st[0].st_size);
#endif
	//capture
	s32Ret = RK_MPI_VENC_SendFrame(eContext[2].veChn, &stViFrame, CONFIG_CAP_TIMEOUT);
	s32Ret = RK_MPI_VENC_GetStream(eContext[2].veChn, &stFrame, CONFIG_CAP_TIMEOUT);
	void *pData = (s32Ret == RK_SUCCESS) ? RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk) : NULL;

	if(pData != NULL && stFrame.pstPack->u32Len > 0){
		fwrite(pData, 1, stFrame.pstPack->u32Len, fp);
		fflush(fp);
		fclose(fp);
		/* if(mode == 1){ */
			/* camera_cap_send_filename(ACK_CAMERA_CAPTURE3); */
		/* }else{ */
			/* camera_cap_send_filename(ACK_CAMERA_CAPTURE0); */
		/* } */
		camera_cap_send_filename(ACK_CAMERA_CAPTURE4);
		RK_MPI_VENC_ReleaseStream(eContext[2].veChn, &stFrame);
	}else{
		//拿不到编码流，避免 fwrite(NULL) 崩溃；仍走后面的资源清理
		r_printf("%s drm GetStream fail %x",__func__, s32Ret);
		fclose(fp);
		camera_cap_send_filename(ACK_CAMERA_ERR);
		if(s32Ret == RK_SUCCESS)
			RK_MPI_VENC_ReleaseStream(eContext[2].veChn, &stFrame);
	}

	free(stFrame.pstPack);
	drm_crtc_deinit(&drminfo);
	e_api.stop_ve(&eContext[2]);
	e_api.destroy_ve(&eContext[2]);
	s32Ret = RK_MPI_MB_ReleaseMB(blk);
	printf("RK_MPI_MB_ReleaseMB %d\n",s32Ret);
	s32Ret = RK_MPI_MB_DestroyPool(pool);
	printf("RK_MPI_MB_DestroyPool %d\n",s32Ret);

	g_printf("%s end",__func__);

    return NULL;
}

int camera_takepicture_start(struct cam_context *msg)
{
	int ret = SUCCESS;
	pthread_t capture = 0;
	pthread_t capture1 = 0;
	int mode = 0;
	
	
    mode = emsg.capture_path[1][0];
	//camera_takepicture_init(msg,mode,1);
	get_save_filename(SAVE_CAPTURE_JPG);//获取保存图片名

	//g_printf("capture %s %s %s %s",emsg.capture_path[0],emsg.capture_path[1],emsg.capture_path[2],emsg.capture_path[3]);
	struct cap_context p;
	p.mode = mode;
	p.msg = msg;

    printf("sclu %s %d mode=%d\n", __func__, __LINE__, mode);

    usleep(1000);

	switch(mode){
		case CAM_CAMERA_CAPTURE_IR:
			pthread_create(&capture, NULL ,camera_takepicture_dvp_jpg_start, (void *)&p);
			pthread_join(capture, NULL);
			break;
		case CAM_CAMERA_CAPTURE_MIPI:
			pthread_create(&capture1, NULL ,camera_takepicture_mipi_jpg_start, (void *)msg);
			pthread_join(capture1, NULL);
			break;
		case CAM_CAMERA_CAPTURE_SCREEN:
			pthread_create(&capture1, NULL ,camera_takepicture_drm_jpg_start, (void *)&p);
			pthread_join(capture1, NULL);
			break;
        case CAM_CAMERA_CAPTURE_IR_MIPI:
			pthread_create(&capture, NULL ,camera_takepicture_dvp_jpg_start, (void *)&p);
			pthread_join(capture, NULL);
			pthread_create(&capture1, NULL ,camera_takepicture_mipi_jpg_start, (void *)msg);
			pthread_join(capture1, NULL);
            break;
        case CAM_CAMERA_CAPTURE_IR_SCREEN:
			pthread_create(&capture, NULL ,camera_takepicture_dvp_jpg_start, (void *)&p);
			pthread_join(capture, NULL);
			pthread_create(&capture1, NULL ,camera_takepicture_drm_jpg_start, (void *)&p);
			pthread_join(capture1, NULL);
            break;
        case CAM_CAMERA_CAPTURE_MIPI_SCREEN:
			pthread_create(&capture1, NULL ,camera_takepicture_drm_jpg_start, (void *)&p);
			pthread_join(capture1, NULL);
			pthread_create(&capture1, NULL ,camera_takepicture_mipi_jpg_start, (void *)msg);
			pthread_join(capture1, NULL);
            break;
        case CAM_CAMERA_CAPTURE_ALL:
			pthread_create(&capture, NULL ,camera_takepicture_dvp_jpg_start, (void *)&p);
			pthread_join(capture, NULL);
			pthread_create(&capture1, NULL ,camera_takepicture_mipi_jpg_start, (void *)msg);
			pthread_join(capture1, NULL);
			pthread_create(&capture1, NULL ,camera_takepicture_drm_jpg_start, (void *)&p);
			pthread_join(capture1, NULL);
			break;
	}

	return ret;
}

static void *camera_thumbnail_jpg_start(void *p)
{
	RK_S32 s32Ret = 0;
	g_printf("%s strat",__func__);
	struct cap_context *pt = (struct cap_context *)p;
	struct cam_context *msg = pt->msg;
	VencContext *eContext = (VencContext *)&msg->camVeContext;
	struct Mpp_venc_api e_api = msg->cam_api;
	int mode = pt->mode;

    VENC_STREAM_S stFrame;
	stFrame.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));
	if(stFrame.pstPack == NULL){
		r_printf("stFrame.pstPack malloc err!!");
		while(1);
	}
	char timebuf[64] = {0x00};
	sscanf(emsg.record_path[0]+strlen(emsg.record_path[0])-24, "%*[^0-9]%[^.]" ,timebuf);
	if(emsg.thumbnailPath[0] == 0x0){
		strcpy(emsg.thumbnailPath, "/tmp");
	}

	VIDEO_FRAME_INFO_S stViFrame;
	memset(&stViFrame, 0x0 ,sizeof(VIDEO_FRAME_INFO_S));
    RK_S32 waitTime = 22; // 1/pContext->fps*1000
	int errCount = 0;

	//dvp drop frame
	int drop_frame = 0;
	//dvp
	// if((mode == 0)||(mode == 1)){
		snprintf(emsg.capture_path[0], MAX_FILE_PATH_SIZE - 1,"%s/VIDr_%s.jpg",emsg.thumbnailPath,timebuf);
        FILE *fp = NULL;
        const char *path = emsg.capture_path[0];

        if (ensure_dir_for_file(path) != 0) {
            r_printf("%s %d create dir for %s failed errno=%d(%s)",
                    __func__, __LINE__, path, errno, strerror(errno));
           // return NULL;
        }

        fp = fopen(path, "wb+");
        if (fp == NULL) {
            r_printf("%s %d fopen %s failed errno=%d(%s)",
                    __func__, __LINE__, path, errno, strerror(errno));
         //   return NULL;
        }



		//init dvp
		e_api.ve_init(&eContext[0], VENC_DVP_JPG);
		camera_takepicture_vpss_init(&eContext[0]); //这里初始化的是dvp拍照通道
		e_api.create_ve(&eContext[0]);
		e_api.start_ve(&eContext[0]);
		while (1) 
		{
			s32Ret = RK_MPI_VPSS_GetChnFrame(eContext[0].s32Grp, VPSS_CHN1, &stViFrame, CONFIG_CAP_TIMEOUT);
			if (s32Ret == RK_SUCCESS) {
				if(++drop_frame <= 5){
					s32Ret = RK_MPI_VPSS_ReleaseChnFrame(eContext[0].s32Grp, VPSS_CHN1, &stViFrame);
					printf("dvp drop %d\n",drop_frame);
					continue;
				}
				void *data = RK_MPI_MB_Handle2VirAddr(stViFrame.stVFrame.pMbBlk);
				s32Ret = RK_MPI_SYS_MmzFlushCache(stViFrame.stVFrame.pMbBlk, RK_TRUE);
				RK_LOGE("RK_MPI_SYS_MmzFlushCache %x ",s32Ret);
			//	if(check_infrared_frame_tag(data) == INFRARED_FRAME_TAG_1122){
					//stViFrame.stVFrame.u32Height -= 2;
					//stViFrame.stVFrame.u32VirHeight -= 2;
					s32Ret = RK_MPI_VENC_SendFrame(eContext[0].veChn, &stViFrame, CONFIG_CAP_TIMEOUT);
					RK_LOGE("RK_MPI_VENC_SendFrame %x ",s32Ret);
					s32Ret = RK_MPI_VENC_GetStream(eContext[0].veChn, &stFrame, CONFIG_CAP_TIMEOUT);
					RK_LOGE("RK_MPI_VENC_GetStream %x ",s32Ret);
					void *pData= RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
					//fwrite(pData, 1, stFrame.pstPack->u32Len, fp);
					//fflush(fp);
					s32Ret = RK_MPI_VENC_ReleaseStream(eContext[0].veChn, &stFrame);
					RK_LOGE("RK_MPI_VENC_ReleaseStream %x ",s32Ret);
					s32Ret = RK_MPI_VPSS_ReleaseChnFrame(eContext[0].s32Grp, VPSS_CHN1, &stViFrame);
					RK_LOGE("RK_MPI_VPSS_ReleaseChnFrame %x ",s32Ret);
					break;
#if 0
            }else {
					s32Ret = RK_MPI_VPSS_ReleaseChnFrame(eContext[0].s32Grp, VPSS_CHN1, &stViFrame);
					RK_LOGE("RK_MPI_VPSS_ReleaseChnFrame %x ",s32Ret);
					errCount ++; 
					if(errCount >= CONFIG_CAP_ERR_CNT){
						camera_cap_send_filename(ACK_CAMERA_ERR);
						break;
					}
					continue;
				}
#endif
			}else {
				errCount ++; 
				if(errCount >= CONFIG_CAP_ERR_CNT){
					camera_cap_send_filename(ACK_CAMERA_ERR);
					break;
				}
				continue;
			}
		}

        if (fp) {
            fclose(fp);
        }
		camera_cap_send_filename(ACK_CAMERA_CAPTURE0);
		e_api.stop_ve(&eContext[0]);
		e_api.destroy_ve(&eContext[0]);
		//不能再 DisableChn(VPSS_CHN1):CHN1 为录像预建的共享通道,关闭会导致之后录像取不到帧。
		memset(emsg.capture_path[0], 0x0 ,sizeof(emsg.capture_path[0]));
	// }
	free(stFrame.pstPack);
	
	pthread_t capture = 0;
	switch(mode){
		case 1:
			//mipi
			snprintf(emsg.capture_path[3], MAX_FILE_PATH_SIZE - 1,"%s/VIDn_%s.jpg",emsg.thumbnailPath,timebuf);
			pthread_create(&capture, NULL ,camera_takepicture_mipi_jpg_start, (void *)msg);
			pthread_join(capture, NULL);
			memset(emsg.capture_path[3], 0x0 ,sizeof(emsg.capture_path[3]));
	//	case 1:
	//		//drm
	//		snprintf(emsg.capture_path[4], MAX_FILE_PATH_SIZE - 1,"%s/VIDd_%s.jpg",emsg.thumbnailPath,timebuf);
	//		pthread_create(&capture, NULL ,camera_takepicture_drm_jpg_start, (void *)p);
	//		pthread_join(capture, NULL);
	//		memset(emsg.capture_path[4], 0x0 ,sizeof(emsg.capture_path[4]));
	//		break;
	//	case 2:
			//mipi
	//		snprintf(emsg.capture_path[3], MAX_FILE_PATH_SIZE - 1,"%s/VIDn_%s.jpg",emsg.thumbnailPath,timebuf);
	//		pthread_create(&capture, NULL ,camera_takepicture_mipi_jpg_start, (void *)msg);
	//		pthread_join(capture, NULL);
	//		memset(emsg.capture_path[3], 0x0 ,sizeof(emsg.capture_path[3]));
	//		break;
	}

	g_printf("%s end",__func__);
    return NULL;
}

int camera_thumbnail_start(struct cam_context *msg, int mode)
{
	int ret = SUCCESS;
	pthread_t thumbnail = 0;
	//camera_takepicture_init(msg,mode,0);
	struct cap_context p;
	p.mode = mode;
	p.msg = msg;

	pthread_create(&thumbnail, NULL ,camera_thumbnail_jpg_start, (void *)&p);
	pthread_join(thumbnail, NULL);
	return ret;
}
