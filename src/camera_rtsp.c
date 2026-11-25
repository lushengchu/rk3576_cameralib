#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include "rk_mpi_vpss.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "camera_main.h"

static VencContext *RTSPeContext = NULL;
/**********************
  * RTSP
**********************/
#include <rtsp_demo.h>
rtsp_demo_handle g_rtsplive;
rtsp_session_handle session[4];

//rtsp
static void push_video_to_rtsp(void *data, int len, int pts){
    rtsp_sever_tx_video(g_rtsplive,session[0],(const uint8_t *)data,len,pts);
}
static void startRtsp(){
    g_rtsplive = create_rtsp_demo(554);
    session[0] = create_rtsp_session(g_rtsplive,"/live",0);
}

static void stopRtsp(){
    rtsp_del_session (session[0]);
    rtsp_del_demo (g_rtsplive);
}

/**********************
  * LOOP
**********************/
static volatile int rtsp_exit_flag = 0;
#define WAITTIME 200 //33 // 1/fps*1000

static void *camera_rtsp_dvp_venc_loop(void *p)
{
	RK_S32 s32Ret = 0;
	g_printf("%s strat",__func__);
	VencContext *eContext = (VencContext *)p;

    VIDEO_FRAME_INFO_S stViFrame;
	memset(&stViFrame, 0x0 ,sizeof(VIDEO_FRAME_INFO_S));
    VENC_STREAM_S stFrame;
	stFrame.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));

	while (1)
	{
		if(rtsp_exit_flag){
			break;
		}

		s32Ret = RK_MPI_VPSS_GetChnFrame(eContext->s32Grp, VPSS_CHN2, &stViFrame, WAITTIME);
		if (s32Ret == RK_SUCCESS) {
			//stViFrame.stVFrame.u32Height -= 2;
			//stViFrame.stVFrame.u32VirHeight -= 2;
			void *data = RK_MPI_MB_Handle2VirAddr(stViFrame.stVFrame.pMbBlk);
			RK_MPI_SYS_MmzFlushCache(stViFrame.stVFrame.pMbBlk, RK_TRUE);
			//if(check_infrared_frame_tag(data) == INFRARED_FRAME_TAG_1122){
				s32Ret = RK_MPI_VENC_SendFrame(eContext->veChn, &stViFrame, WAITTIME);
				if (s32Ret == RK_SUCCESS) {
					s32Ret = RK_MPI_VENC_GetStream(eContext->veChn, &stFrame, WAITTIME);
					if (s32Ret == RK_SUCCESS) {
						void *pData = RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
						push_video_to_rtsp(pData, stFrame.pstPack->u32Len,stFrame.pstPack->u64PTS);
						RK_MPI_VENC_ReleaseStream(eContext->veChn, &stFrame);
					}
				}
		//	}else {
			//	RK_MPI_VPSS_ReleaseChnFrame(eContext->s32Grp, VPSS_CHN2, &stViFrame);
			//	continue;
		//	}
			RK_MPI_VPSS_ReleaseChnFrame(eContext->s32Grp, VPSS_CHN2, &stViFrame);
		}else {
			continue;
		}

	}
	free(stFrame.pstPack);

	g_printf("%s end",__func__);
	return NULL;
}

static void *camera_rtsp_mipi_venc_loop(void *p)
{
	RK_S32 s32Ret = 0;
	g_printf("%s strat",__func__);
	VencContext *eContext = (VencContext *)p;

    VENC_STREAM_S stFrame;
	stFrame.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));

    //FILE *fp = fopen("/userdata/mipi_h264.bin", "wb+");
	while (1)
	{
		if(rtsp_exit_flag){
			break;
		}

		s32Ret = RK_MPI_VENC_GetStream(eContext->veChn, &stFrame, WAITTIME);
		if (s32Ret == RK_SUCCESS) {
			void *pData = RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
           // fwrite(pData, 1, stFrame.pstPack->u32Len, fp);
			push_video_to_rtsp(pData, stFrame.pstPack->u32Len,stFrame.pstPack->u64PTS);
           // printf(" %s:%d %lld\n", __func__, stFrame.pstPack->u32Len,stFrame.pstPack->u64PTS);
			RK_MPI_VENC_ReleaseStream(eContext->veChn, &stFrame);
		}else {
			continue;
		}

	}
	free(stFrame.pstPack);

   // fclose(fp);
	g_printf("%s end",__func__);
	return NULL;
}

static void *camera_rtsp_drm_venc_loop(void *p)
{
	RK_S32 s32Ret = 0;
	printf("%s strat",__func__);
	VencContext *eContext = (VencContext *)p;

    VENC_STREAM_S stFrame;
	stFrame.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));

    //drm
    CAMDRMINFO drminfo;
    memset(&drminfo, 0x0 , sizeof(CAMDRMINFO));
    drm_crtc_init(&drminfo);

    //零拷贝(与 recorder 一致)：不再自建 MB，每帧把 weston fb 的 dma-buf(fd)直接包成 MB_BLK
    //喂给 VENC(BGRA8888)，由 VENC 硬件做 CSC+旋转。CPU memcpy 读非缓存 fb 太慢。
    VIDEO_FRAME_INFO_S stViFrame;
    memset(&stViFrame, 0x0 ,sizeof(VIDEO_FRAME_INFO_S));
    stViFrame.stVFrame.u32Width = eContext->width;   //1920，真实 fb 宽
    stViFrame.stVFrame.u32Height = eContext->height; //1080，真实 fb 高
    stViFrame.stVFrame.u32VirWidth = eContext->width;
    stViFrame.stVFrame.u32VirHeight = eContext->height;
    stViFrame.stVFrame.enPixelFormat = RK_FMT_BGRA8888;//直接喂 weston 的 BGRA fb，VENC 硬件做 CSC+旋转
    //stViFrame.stVFrame.u32FrameFlag |= s32ReachEOS ? FRAME_FLAG_SNAP_END : 0;
    stViFrame.stVFrame.enCompressMode = COMPRESS_MODE_NONE;

    int mb_err_logged = 0;
    RK_U64 startUs = TEST_COMM_GetNowUs();
    RK_U64 frame_num = 0;
    RK_U64 fpsMs0, fpsMs1;
    RK_U64 Ms0, Ms1, Ms2, Ms3, Ms4, Ms5, Ms6, Ms7;
    RK_U64 delta;
    int fpsms = 45; // 1/22=45ms

   // FILE *fp = fopen("/userdata/drm_h264.bin", "wb+");
    while (1)
    {
		if(rtsp_exit_flag){
            break;
        }
        fpsMs0 = TEST_COMM_GetNowUs() / 1000;
        frame_num++;
_first:
        Ms0 = TEST_COMM_GetNowUs() / 1000;
        // camera_drm_wait_vblank(&drminfo);
        //每帧循环调用，静态屏幕时用小超时兜底，避免帧率掉到很低或死循环重试
        drmModeCrtcPtr crtc = camera_drm_wait_vblank_2_timeout(&drminfo, 20);
        Ms1 = TEST_COMM_GetNowUs() / 1000;
        delta = Ms1 - Ms0;
        if (delta > 33) {
            // r_printf("sel frm use %lu ms > 33ms", delta);
            camera_gst_kms_src_free_crtc_fb(crtc);
            goto _first;
        }
        else {
            // g_printf("sel frm use %lu ms", delta);
        }
        //weston多缓冲轮转/旋转屏幕后会切换fb，按新fb_id更新映射，否则读到的是旧画面（旋转不同步）
        drm_crtc_update_fb(&drminfo, crtc->buffer_id);
        Ms2 = TEST_COMM_GetNowUs()/1000;

        //零拷贝：把 weston fb 的 dma-buf(fd)直接包成 MB_BLK 喂 VENC(BGRA8888)，硬件做 CSC+旋转。
        //去掉 CPU memcpy(读非缓存 fb ~52ms)与 DMA_BUF_IOCTL_SYNC/MmzFlushCache(无 CPU 访问)。
        //fb 归 DRM/weston 所有，pFreeCB=NULL；每帧 import 一次，编码完立即 ReleaseMB。
        int rtsp_fbw = drminfo.fb2 ? (int)drminfo.fb2->width  : 1920;
        int rtsp_fbh = drminfo.fb2 ? (int)drminfo.fb2->height : 1080;
        MB_EXT_CONFIG_S extCfg;
        memset(&extCfg, 0, sizeof(extCfg));
        extCfg.s32Fd = drminfo.dmafd[0];
        extCfg.pu8VirAddr = (RK_U8 *)drminfo.vaddr;
        extCfg.u64Size = (RK_U64)rtsp_fbw * rtsp_fbh * 4;
        extCfg.pFreeCB = NULL;
        MB_BLK fbMb = NULL;
        if (RK_MPI_SYS_CreateMB(&fbMb, &extCfg) != RK_SUCCESS || !fbMb) {
            if (!mb_err_logged) { printf("CreateMB from fb fd failed\n"); mb_err_logged = 1; }
            camera_gst_kms_src_free_crtc_fb(crtc);
            continue;
        }
        stViFrame.stVFrame.pMbBlk = fbMb;
        stViFrame.stVFrame.u32Width = rtsp_fbw;
        stViFrame.stVFrame.u32Height = rtsp_fbh;
        stViFrame.stVFrame.u32VirWidth = rtsp_fbw;
        stViFrame.stVFrame.u32VirHeight = rtsp_fbh;
        Ms3 = TEST_COMM_GetNowUs()/1000;
        // g_printf("copy frm use %lu ms", Ms2 - Ms1);
        stViFrame.stVFrame.u64PTS = TEST_COMM_GetNowUs();
        camera_gst_kms_src_free_crtc_fb(crtc);
        Ms4 = TEST_COMM_GetNowUs()/1000;
        s32Ret = RK_MPI_VENC_SendFrame(eContext->veChn, &stViFrame, -1);
        if(s32Ret != RK_SUCCESS){
            RK_MPI_MB_ReleaseMB(fbMb);
            continue;
        }
        //printf("RK_MPI_VENC_SendFrame %d\n",s32Ret);

        s32Ret = RK_MPI_VENC_GetStream(eContext->veChn, &stFrame, -1);
        if(s32Ret != RK_SUCCESS){
            RK_MPI_MB_ReleaseMB(fbMb);
            continue;
        }
        //printf("RK_MPI_VENC_GetStream %d\n",s32Ret);
        Ms5 = TEST_COMM_GetNowUs()/1000;
        // g_printf("venc frm use %lu ms", Ms4 - Ms3);
        void *pData= RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
       // fwrite(pData, 1, stFrame.pstPack->u32Len, fp);
        push_video_to_rtsp(pData, stFrame.pstPack->u32Len,stFrame.pstPack->u64PTS);
       // printf(" %s:%d %lld\n", __func__, stFrame.pstPack->u32Len,stFrame.pstPack->u64PTS);
        RK_MPI_VENC_ReleaseStream(eContext->veChn, &stFrame);
        //VENC 已同步编码完这一帧，输入 fb 可安全释放回 DRM/weston
        RK_MPI_MB_ReleaseMB(fbMb);
        Ms6 = TEST_COMM_GetNowUs()/1000;
        fpsMs1 = TEST_COMM_GetNowUs()/1000;
        delta = fpsMs1 - fpsMs0;
        if (delta < fpsms){
            // g_printf("delta %lu usleep %lu", delta, fpsms - delta);
           // usleep((fpsms - delta) * 1000);
        }
        else {
            // r_printf("Warming: deal fmt use %lu ms > fpsms %d", delta, fpsms);
            // r_printf("Warming: Ms1-Ms0=%d, Ms2-Ms1=%d, Ms3-Ms2=%d, Ms4-Ms3=%d, Ms5-Ms4=%d, Ms6-Ms5=%d\n",
            // Ms1 - Ms0, Ms2 - Ms1, Ms3 - Ms2, Ms4 - Ms3, Ms5 - Ms4, Ms6 - Ms5);
        }
         //r_printf("deal fmt use %lu ms",TEST_COMM_GetNowUs()/1000 - fpsMs0);
    }

  //  fclose(fp);
    free(stFrame.pstPack);
    drm_crtc_deinit(&drminfo);

    RK_U64 endMs = (TEST_COMM_GetNowUs() - startUs)/1000;
    int avr_fps = frame_num*10000/endMs;
    g_printf("drm end time frame num %lu , %lu ms , avr fps %d.%d",frame_num,endMs,avr_fps/10,avr_fps&0xf);




	g_printf("%s end",__func__);
	return NULL;
}


/**********************
  * EXTERN
**********************/
static pthread_t rtsp_hdl = 0;
static CAPRTSPRTMPMODE rtsp_mode = CAP_RTSP_RTMP_IR;
void camera_rtsp_set_mode(CAPRTSPRTMPMODE mode)
{
	rtsp_mode = mode;
}
CAPRTSPRTMPMODE camera_rtsp_get_mode(void)
{
	return rtsp_mode;
}

int camera_rtsp_start(struct cam_context *msg)
{
	RK_S32 s32Ret = 0;

	g_printf("%s mode %d",__func__,rtsp_mode);
	struct Mpp_venc_api e_api = msg->cam_api;
	RTSPeContext = (VencContext *)malloc(sizeof(VencContext));
	VencContext *eContext = RTSPeContext;
	if(rtsp_mode == CAP_RTSP_RTMP_IR) {
		//eContext = (VencContext *)&msg->camVeContext[0];
		e_api.ve_init(eContext, VENC_DVP_H264);
	}else if(rtsp_mode == CAP_RTSP_RTMP_VL) {
		//eContext = (VencContext *)&msg->camVeContext[1];
		e_api.ve_init(eContext, VENC_MIPI_H264);
	} else {
		e_api.ve_init(eContext, VENC_DRM_H264_ROT);
    }
	//rtsp use vechn 3
	eContext->veChn = 3;

	//use chn 2
	VPSS_CHN_ATTR_S stVpssChnAttr;
	memset(&stVpssChnAttr, 0x0 ,sizeof(VPSS_CHN_ATTR_S));
	stVpssChnAttr.enChnMode = VPSS_CHN_MODE_PASSTHROUGH;
	stVpssChnAttr.enDynamicRange = DYNAMIC_RANGE_SDR8;
	stVpssChnAttr.enPixelFormat = eContext->enPixelFormat;// RK_FMT_BGRA8888;
	stVpssChnAttr.stFrameRate.s32SrcFrameRate = -1;
	stVpssChnAttr.stFrameRate.s32DstFrameRate = -1;
	//stVpssChnAttr.stFrameRate.s32SrcFrameRate = 30;
	//stVpssChnAttr.stFrameRate.s32DstFrameRate = 30;
	stVpssChnAttr.u32Width = eContext->width;
	stVpssChnAttr.u32Height = eContext->height;
	if(stVpssChnAttr.u32Height == 256){
		stVpssChnAttr.u32Height += 2;
	}
	stVpssChnAttr.enCompressMode = COMPRESS_MODE_NONE;
	stVpssChnAttr.u32Depth = 1;

	s32Ret = RK_MPI_VPSS_SetChnAttr(eContext->s32Grp, VPSS_CHN2, &stVpssChnAttr);
	RK_LOGE("RK_MPI_VPSS_SetChnAttr %x", s32Ret);

	s32Ret = RK_MPI_VPSS_EnableChn(eContext->s32Grp, VPSS_CHN2);
	RK_LOGE("RK_MPI_VPSS_EnableChn %x", s32Ret);

	e_api.create_ve(eContext);
	e_api.start_ve(eContext);
	startRtsp();

	rtsp_exit_flag = 0;
	if(rtsp_mode == CAP_RTSP_RTMP_IR){
		pthread_create(&rtsp_hdl, NULL ,camera_rtsp_dvp_venc_loop, (void *)eContext);
    } else if (rtsp_mode == CAP_RTSP_RTMP_VL) {
		pthread_create(&rtsp_hdl, NULL ,camera_rtsp_mipi_venc_loop, (void *)eContext);
	} else {
		pthread_create(&rtsp_hdl, NULL ,camera_rtsp_drm_venc_loop, (void *)eContext);
    }

	return s32Ret;
}

int camera_rtsp_stop(struct cam_context *msg)
{
	int result = 0;
	struct Mpp_venc_api e_api = msg->cam_api;
	VencContext *eContext = RTSPeContext;
	/* if(rtsp_mode == CAP_RTSP_RTMP_IR){ */
		/* eContext = (VencContext *)&msg->camVeContext[0]; */
	/* }else{ */
		/* eContext = (VencContext *)&msg->camVeContext[1]; */
	/* } */

	g_printf("%s ",__func__);
	rtsp_exit_flag = 1;
	pthread_join(rtsp_hdl, NULL);
	stopRtsp();
	e_api.stop_ve(eContext);
	e_api.destroy_ve(eContext);

	result = RK_MPI_VPSS_DisableChn(eContext->s32Grp, VPSS_CHN2);
	RK_LOGE("RK_MPI_VPSS_DisableChn %x", result);
	free(RTSPeContext);

	return result;
}

