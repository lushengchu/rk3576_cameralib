#include "rk_comm_vi.h"
#include "rk_comm_vo.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vpss.h"
#include "rk_mpi_vo.h"
#include "camera_main.h"

static void test_vi_event_call_back(RK_VOID *pPrivateData, VI_CB_INFO_S *info) {
    VippContext *vContext = (VippContext *)pPrivateData;

    if (info) {
        if (info->u32Event & VI_EVENT_CONNECT_CHANGE)
            RK_LOGI("event connect change");
        if (info->u32Event & VI_EVENT_SOURCE_CHANGE) {
            RK_LOGI("event source change");
        }
    }
	g_printf("pipeid %d chl %d",vContext->pipeId, vContext->channelId);
}

static RK_S32 vi_set_event_call_back(VippContext *vContext) {
    RK_S32 s32Ret = RK_FAILURE;
    VI_EVENT_CALL_BACK_S stCallbackFunc;

    stCallbackFunc.pfnCallback = test_vi_event_call_back;
    stCallbackFunc.pPrivateData = vContext;

    /* test set change event call back(need after RK_MPI_VI_SetChnAttr)*/
    s32Ret = RK_MPI_VI_RegChnEventCallback(vContext->pipeId, vContext->channelId, &stCallbackFunc);
    //if (s32Ret != RK_SUCCESS) {
        g_printf("RK_MPI_VI_RegChnEventCallback %x", s32Ret);
        //return s32Ret;
    //}

    return s32Ret;
}

/**********************
  * VI VPSS VO
**********************/
static PIXEL_FORMAT_E get_conf_fmt(int fmt)
{
	PIXEL_FORMAT_E tmp = 0x0; 
	switch(fmt){
		case 0:
			tmp = RK_FMT_YUV420SP;
			break;
		case 1:
			tmp = RK_FMT_YUV422_UYVY;
			break;
		case 2:
			tmp = RK_FMT_YUV422_YUYV;
			break;
		case 3:
			tmp = RK_FMT_RGBA8888;
			break;
		case 4:
			tmp = RK_FMT_BGRA8888;
			break;
		case 5:
			tmp = RK_FMT_BGRA5551;
			break;
		default:
			tmp = RK_FMT_YUV420SP;
			break;
	}
	return tmp;
}
static int mpp_vi_init(VippContext *vContext)
{
	int ret = 0;

    printf("sclu %s %d vContext->ccd_index=%d\n", __func__, __LINE__, vContext->ccd_index);
	//初始化参数
	switch(vContext->ccd_index){
		//dvp
		case 0:
			if(emsg.conf_ok){
				vContext->width = camera_conf.vidvp.width;
				vContext->height = camera_conf.vidvp.height;
				vContext->fps = camera_conf.vidvp.fps;
				vContext->enPixelFormat = get_conf_fmt(camera_conf.vidvp.vi_format);
				vContext->vpssenPixelFormat = get_conf_fmt(camera_conf.vidvp.vpss_format);
				vContext->u32Depth = camera_conf.vidvp.vi_depth;
				vContext->u32BufCount = camera_conf.vidvp.vi_bufcnt;
				vContext->vpssu32Depth = camera_conf.vidvp.vpss_depth;
			}else{
				vContext->width = CONFIG_DVP_SRC_WIDTH;
				vContext->height = CONFIG_DVP_SRC_HEIGHT;
				vContext->fps = 25;
				vContext->enPixelFormat = RK_FMT_YUV422_UYVY;
				vContext->vpssenPixelFormat = vContext->enPixelFormat;
				vContext->u32Depth = 0;
				vContext->u32BufCount = 5;
				vContext->vpssu32Depth = 1;
			}
			vContext->devId = 0;
			vContext->channelId = 0;
			vContext->s32Grp = 0;//VPSS GROUP 号 取值范围：[0,VPSS_MAX_GRP_NUM)。
			vContext->bCfg = 0;
			strcpy(vContext->aEntityName, "/dev/video0");
			break;
		//mipi
		case 3:
			if(emsg.conf_ok){
				vContext->width = camera_conf.vimipi.width;
				vContext->height = camera_conf.vimipi.height;
				vContext->fps = camera_conf.vimipi.fps;
				vContext->enPixelFormat = get_conf_fmt(camera_conf.vimipi.vi_format);
				vContext->vpssenPixelFormat = get_conf_fmt(camera_conf.vimipi.vpss_format);
				vContext->u32Depth = camera_conf.vimipi.vi_depth;
				vContext->u32BufCount = camera_conf.vimipi.vi_bufcnt;
				vContext->vpssu32Depth = camera_conf.vimipi.vpss_depth;
			}else{
				vContext->width = CONFIG_MIPI_SRC_WIDTH;
				vContext->height = CONFIG_MIPI_SRC_HEIGHT;
				vContext->fps = 25;
				vContext->enPixelFormat = RK_FMT_YUV420SP;
				vContext->vpssenPixelFormat = RK_FMT_RGBA8888;
				vContext->u32Depth = 0;
				vContext->u32BufCount = 5;
				vContext->vpssu32Depth = 1;
			}
			vContext->devId = 3;
			vContext->channelId = 1;
			vContext->s32Grp = 1;//VPSS GROUP 号 取值范围：[0,VPSS_MAX_GRP_NUM)。
			vContext->bCfg = 0;
			strcpy(vContext->aEntityName, "/dev/video22");
			break;
		default:
			break;
	}

	return ret;
}

static int mpp_create_vi(VippContext *vContext)
{
   	RK_S32 s32Ret = 0;
    VI_DEV_ATTR_S stDevAttr;
	memset(&stDevAttr, 0x0 ,sizeof(VI_DEV_ATTR_S));

	vContext->pipeId = vContext->devId;

	// 0. get dev config status
    s32Ret = RK_MPI_VI_GetDevAttr(vContext->devId, &stDevAttr);
    if (s32Ret == RK_ERR_VI_NOT_CONFIG) {
        // 0-1.config dev
        s32Ret = RK_MPI_VI_SetDevAttr(vContext->devId, &stDevAttr);
        if (s32Ret != RK_SUCCESS) {
            RK_LOGE("RK_MPI_VI_SetDevAttr %x", s32Ret);
        }
    } else {
        RK_LOGE("RK_MPI_VI_SetDevAttr already");
    }

    VI_DEV_BIND_PIPE_S stBindPipe;
	memset(&stBindPipe, 0x0 ,sizeof(VI_DEV_BIND_PIPE_S));
	// 1.get  dev enable status
	s32Ret = RK_MPI_VI_GetDevIsEnable(vContext->devId);
    if (s32Ret != RK_SUCCESS) {
        // 1-2.enable dev
        s32Ret = RK_MPI_VI_EnableDev(vContext->devId);
        if (s32Ret != RK_SUCCESS) {
            RK_LOGE("RK_MPI_VI_EnableDev %x", s32Ret);
        }
        // 1-3.bind dev/pipe
        stBindPipe.u32Num = vContext->pipeId;
        stBindPipe.PipeId[0] = vContext->pipeId;
        s32Ret = RK_MPI_VI_SetDevBindPipe(vContext->devId, &stBindPipe);
        if (s32Ret != RK_SUCCESS) {
            RK_LOGE("RK_MPI_VI_SetDevBindPipe %x", s32Ret);
        }
    } else {
        RK_LOGE("RK_MPI_VI_EnableDev already");
    }

	// 2.config channel
	VI_CHN_ATTR_S stChnAttr;
	memset(&stChnAttr, 0x0 ,sizeof(VI_CHN_ATTR_S));


	stChnAttr.u32Depth = vContext->u32Depth;

    strcpy(stChnAttr.stIspOpt.aEntityName, vContext->aEntityName);
	stChnAttr.stIspOpt.u32BufCount = vContext->u32BufCount;
    stChnAttr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    stChnAttr.stIspOpt.enCaptureType = VI_V4L2_CAPTURE_TYPE_VIDEO_CAPTURE;
	stChnAttr.stSize.u32Width = vContext->width;
    stChnAttr.stSize.u32Height = vContext->height;
    stChnAttr.enCompressMode = COMPRESS_MODE_NONE;
    stChnAttr.enPixelFormat = vContext->enPixelFormat;
	stChnAttr.stFrameRate.s32SrcFrameRate = vContext->fps;
	stChnAttr.stFrameRate.s32DstFrameRate = vContext->fps;
    s32Ret = RK_MPI_VI_SetChnAttr(vContext->pipeId, vContext->channelId, &stChnAttr);
    if (s32Ret != RK_SUCCESS) {
        RK_LOGE("RK_MPI_VI_SetChnAttr %x", s32Ret);
    }
	vi_set_event_call_back(vContext);

	//vpss
    VPSS_GRP_ATTR_S stGrpVpssAttr;
	memset(&stGrpVpssAttr, 0x0 ,sizeof(VPSS_GRP_ATTR_S));

	stGrpVpssAttr.u32MaxW = 4096;
    stGrpVpssAttr.u32MaxH = 4096;
	stGrpVpssAttr.enPixelFormat = vContext->enPixelFormat;
    stGrpVpssAttr.stFrameRate.s32SrcFrameRate = vContext->fps;
    stGrpVpssAttr.stFrameRate.s32DstFrameRate = vContext->fps;
    stGrpVpssAttr.enCompressMode = COMPRESS_MODE_NONE;

    s32Ret = RK_MPI_VPSS_CreateGrp(vContext->s32Grp, &stGrpVpssAttr);
    RK_LOGE("RK_MPI_VPSS_CreateGrp %x", s32Ret);


	//use chn 0
    VPSS_CHN_ATTR_S stVpssChnAttr;
	memset(&stVpssChnAttr, 0x0 ,sizeof(VPSS_CHN_ATTR_S));
	if(vContext->s32Grp == 0){
		stVpssChnAttr.enChnMode = VPSS_CHN_MODE_USER;
		s32Ret = RK_MPI_VPSS_SetVProcDev(vContext->s32Grp, VIDEO_PROC_DEV_RGA);//设置VPSS 可以使的硬件设备，rk3576默认是GPU,这里设置使用RGA
		RK_LOGE("RK_MPI_VPSS_SetVProcDev %x", s32Ret);
	}else{
		stVpssChnAttr.enChnMode = VPSS_CHN_MODE_USER;
		stVpssChnAttr.u32FrameBufCnt = 5;
	}
	stVpssChnAttr.enDynamicRange = DYNAMIC_RANGE_SDR8;
	stVpssChnAttr.enPixelFormat = vContext->vpssenPixelFormat;
	//stVpssChnAttr.stFrameRate.s32SrcFrameRate = -1;
	//stVpssChnAttr.stFrameRate.s32DstFrameRate = -1;
	stVpssChnAttr.stFrameRate.s32SrcFrameRate = vContext->fps;
	stVpssChnAttr.stFrameRate.s32DstFrameRate = vContext->fps;
	stVpssChnAttr.u32Width = vContext->width;
	stVpssChnAttr.u32Height = vContext->height;
	stVpssChnAttr.u32Depth = vContext->vpssu32Depth;
	stVpssChnAttr.enCompressMode = COMPRESS_MODE_NONE;

	s32Ret = RK_MPI_VPSS_SetChnAttr(vContext->s32Grp, VPSS_CHN0, &stVpssChnAttr);
	RK_LOGE("RK_MPI_VPSS_SetChnAttr %x", s32Ret);

	//pre-create chn 1 for recorder (must be done before StartGrp on rk3576)
	VPSS_CHN_ATTR_S stVpssChn1Attr;
	memset(&stVpssChn1Attr, 0x0, sizeof(VPSS_CHN_ATTR_S));
	stVpssChn1Attr.enChnMode = VPSS_CHN_MODE_PASSTHROUGH;
	stVpssChn1Attr.enDynamicRange = DYNAMIC_RANGE_SDR8;
	stVpssChn1Attr.enPixelFormat = vContext->enPixelFormat;
	stVpssChn1Attr.stFrameRate.s32SrcFrameRate = vContext->fps;
	stVpssChn1Attr.stFrameRate.s32DstFrameRate = vContext->fps;
	stVpssChn1Attr.u32Width = vContext->width;
	stVpssChn1Attr.u32Height = vContext->height;
	/* depth 必须保持 1：depth=4(5224bdff19 曾尝试)在不录像时无人消费 CHN1，
	 * 队列挂住 4 帧不归还，5 帧缓冲池被占死后 VPSS 组周转不开，会饿死预览的 CHN0，
	 * 表现为"纯预览卡顿、一录像就恢复、停录又卡"。录像帧率靠 mux 线程+帧队列已能保住。 */
	stVpssChn1Attr.u32Depth = 1;
	stVpssChn1Attr.u32FrameBufCnt = 5;
	stVpssChn1Attr.enCompressMode = COMPRESS_MODE_NONE;
	s32Ret = RK_MPI_VPSS_SetChnAttr(vContext->s32Grp, VPSS_CHN1, &stVpssChn1Attr);
	RK_LOGE("RK_MPI_VPSS_SetChnAttr CHN1 %x", s32Ret);

	// 3.enable channel
	s32Ret = RK_MPI_VI_EnableChn(vContext->pipeId, vContext->channelId);
	if (s32Ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VI_EnableChn %x", s32Ret);
	}

	s32Ret = RK_MPI_VPSS_EnableChn(vContext->s32Grp, VPSS_CHN0);
	RK_LOGE("RK_MPI_VPSS_EnableChn %x", s32Ret);

	s32Ret = RK_MPI_VPSS_EnableChn(vContext->s32Grp, VPSS_CHN1);
	RK_LOGE("RK_MPI_VPSS_EnableChn CHN1 %x", s32Ret);

    s32Ret = RK_MPI_VPSS_EnableBackupFrame(vContext->s32Grp);
	RK_LOGE("RK_MPI_VPSS_EnableBackupFrame %x", s32Ret);

    s32Ret = RK_MPI_VPSS_StartGrp(vContext->s32Grp);
	RK_LOGE("RK_MPI_VPSS_StartGrp %x", s32Ret);

	if(vContext->s32Grp == 1){
		//set crop
        camera_mpp_mipi_set_crop(emsg.mipi_source_x,emsg.mipi_source_y,emsg.mipi_source_w,emsg.mipi_source_h);
	}


#if 0
	if(vContext->s32Grp == 0){
		VPSS_CROP_INFO_S stCropInfo;
		stCropInfo.bEnable = 1;
		//stCropInfo.enCropCoordinate = VPSS_CROP_RATIO_COOR;
		stCropInfo.enCropCoordinate = VPSS_CROP_ABS_COOR;
		stCropInfo.stCropRect.s32X = 0;
		stCropInfo.stCropRect.s32Y = 0;
		stCropInfo.stCropRect.u32Width = 100;
		stCropInfo.stCropRect.u32Height = 100;
		s32Ret = RK_MPI_VPSS_SetChnCrop(vContext->s32Grp, VPSS_CHN0, &stCropInfo);
		RK_LOGE("RK_MPI_VPSS_SetChnCrop %x", s32Ret);
	}
#endif

    // 4.save debug file
    if (vContext->bCfg) {
		VI_SAVE_FILE_INFO_S stDebugFile;
		memset(&stDebugFile, 0x0 ,sizeof(VI_SAVE_FILE_INFO_S));
		stDebugFile.bCfg = vContext->bCfg;
		memcpy(&stDebugFile.aFilePath, "/tmp", strlen("/tmp"));
		snprintf(stDebugFile.aFileName, MAX_VI_FILE_PATH_LEN,"out.yuv");
		s32Ret = RK_MPI_VI_ChnSaveFile(vContext->pipeId, vContext->channelId, &stDebugFile);
        RK_LOGD("RK_MPI_VI_ChnSaveFile %x", s32Ret);
    }

	//bind vi to vpss
	MPP_CHN_S stviChn , stvpssChn  , stvoChn;
	memset(&stviChn, 0, sizeof(MPP_CHN_S));
	memset(&stvpssChn, 0, sizeof(MPP_CHN_S));
	stviChn.enModId    = RK_ID_VI;
	stviChn.s32DevId   = vContext->devId;
	stviChn.s32ChnId   = vContext->channelId;

	stvpssChn.enModId   = RK_ID_VPSS;
	stvpssChn.s32DevId  = vContext->s32Grp;
	stvpssChn.s32ChnId  = VPSS_CHN0;

	s32Ret = RK_MPI_SYS_Bind(&stviChn, &stvpssChn);
	if (s32Ret != RK_SUCCESS) {
		RK_LOGE("vi bind vpss fail:%x", s32Ret);
	}
	return s32Ret;
}

static int mpp_start_vi(VippContext *vContext)
{
   	RK_S32 s32Ret = 0;
	return s32Ret;
}

static int mpp_stop_vi(VippContext *vContext)
{
   	RK_S32 s32Ret = 0;

	//Unbind vi to vpss
	MPP_CHN_S stviChn , stvpssChn  , stvoChn;
	memset(&stviChn, 0, sizeof(MPP_CHN_S));
	memset(&stvpssChn, 0, sizeof(MPP_CHN_S));
	stviChn.enModId    = RK_ID_VI;
	stviChn.s32DevId   = vContext->devId;
	stviChn.s32ChnId   = vContext->channelId;

	stvpssChn.enModId   = RK_ID_VPSS;
	stvpssChn.s32DevId  = vContext->s32Grp;
	stvpssChn.s32ChnId  = VPSS_CHN0;

	s32Ret = RK_MPI_SYS_UnBind(&stviChn, &stvpssChn);
	RK_LOGE("RK_MPI_SYS_UnBind %x", s32Ret);
	return s32Ret;
}

static int mpp_destroy_vi(VippContext *vContext)
{
   	RK_S32 s32Ret = 0;

	s32Ret = RK_MPI_VPSS_DisableChn(vContext->s32Grp, VPSS_CHN1);
	RK_LOGE("RK_MPI_VPSS_DisableChn CHN1 %d %x",vContext->s32Grp,s32Ret);

	s32Ret = RK_MPI_VPSS_DisableChn(vContext->s32Grp, VPSS_CHN0);
	RK_LOGE("RK_MPI_VPSS_DisableChn %d %x",vContext->s32Grp,s32Ret);

	s32Ret = RK_MPI_VPSS_StopGrp(vContext->s32Grp);
	RK_LOGE("RK_MPI_VPSS_StopGrp %d %x",vContext->s32Grp,s32Ret);

	s32Ret = RK_MPI_VPSS_DestroyGrp(vContext->s32Grp);
	RK_LOGE("RK_MPI_VPSS_DestroyGrp %d %x",vContext->s32Grp,s32Ret);

    // 5. disable one chn
    s32Ret = RK_MPI_VI_DisableChn(vContext->pipeId, vContext->channelId);
    RK_LOGE("RK_MPI_VI_DisableChn %x", s32Ret);

	// 6.disable dev(will diabled all chn)
    s32Ret = RK_MPI_VI_DisableDev(vContext->devId);
    RK_LOGE("RK_MPI_VI_DisableDev %d %x", vContext->devId,s32Ret);

	return s32Ret;
}

/**********************
  * VO
**********************/
static int mpp_vo_init(VoContext *pContext){return 0;}
static int mpp_create_vo(VippContext *vContext, VoContext *pContext){return 0;}
static int mpp_start_vo(VippContext *vContext, VoContext *pContext){return 0;}
static int mpp_stop_vo(VoContext *pContext){return 0;}
static int mpp_destroy_vo(VoContext *pContext){return 0;}

/**********************
  * VENC
**********************/
static int mpp_ve_init(VencContext *eContext, enum venc_type type)
{
   	RK_S32 s32Ret = 0;

	switch(type){
		case VENC_DVP_JPG:
		case VENC_DVP_H264:
			if(emsg.conf_ok){
				eContext->width = camera_conf.vedvp.width;
				eContext->height = camera_conf.vedvp.height;
				eContext->bitrate = camera_conf.vedvp.bitrate;
			}else{
				eContext->width = CONFIG_DVP_SRC_WIDTH;
				eContext->height = (CONFIG_DVP_SRC_HEIGHT - CONFIG_DVP_SRC_INFO);
				eContext->bitrate = 1024;
			}
			eContext->veId = 0;
			eContext->veChn = 0;
			eContext->s32Grp = 0;
			eContext->enPixelFormat = RK_FMT_YUV422_UYVY;
			if(type == VENC_DVP_JPG){
				eContext->enType = RK_VIDEO_ID_MJPEG;
			}else{
				eContext->enType = RK_VIDEO_ID_AVC;
			}
			break;
		case VENC_MIPI_JPG:
		case VENC_MIPI_H264:
			if(emsg.conf_ok){
				eContext->bitrate = camera_conf.vemipi_bitrate;
			}else{
				eContext->bitrate = 1024*4;
			}
			eContext->width = emsg.mipi_source_w;
			eContext->height = emsg.mipi_source_h;
			/* 融合裁剪还没给到有效宽高时，按传感器分辨率编码，避免 CreateChn a0048003、VIDn 空文件。 */
			if(eContext->width < 16 || eContext->height < 16){
				printf("mipi venc size %dx%d invalid, fallback %dx%d\n",
					eContext->width, eContext->height,
					CONFIG_MIPI_SRC_WIDTH, CONFIG_MIPI_SRC_HEIGHT);
				eContext->width = CONFIG_MIPI_SRC_WIDTH;
				eContext->height = CONFIG_MIPI_SRC_HEIGHT;
				emsg.mipi_source_x = 0;
				emsg.mipi_source_y = 0;
				emsg.mipi_source_w = eContext->width;
				emsg.mipi_source_h = eContext->height;
			}
			eContext->veId = 0;
			eContext->veChn = 1;
			eContext->s32Grp = 1;
			eContext->enPixelFormat = RK_FMT_YUV420SP;
			if(type == VENC_MIPI_JPG){
				eContext->enType = RK_VIDEO_ID_MJPEG;
			}else{
				eContext->enType = RK_VIDEO_ID_AVC;
			}
			break; 
		case VENC_DRM_JPG:
		case VENC_DRM_H264:
			if(emsg.conf_ok){
				eContext->bitrate = camera_conf.vemipi_bitrate;
			}else{
				eContext->bitrate = 1024;
			}
			eContext->width = CONFIG_DRM_SRC_WIDTH;
			eContext->height = CONFIG_DRM_SRC_HEIGHT;
			eContext->veId = 0;
			eContext->veChn = 2; //如果是rtsp,在camera_rtsp.c中会赋值修改为veChn=3
			eContext->s32Grp = -1;
			eContext->enPixelFormat = RK_FMT_YUV422_UYVY;//RK_FMT_BGRA8888;
			if(type == VENC_DRM_JPG){
				eContext->enType = RK_VIDEO_ID_MJPEG;
			}else{
				eContext->enType = RK_VIDEO_ID_AVC;
			}

			break; 
		case VENC_DRM_H264_ROT:
			//录屏专用：直接把 weston 的 BGRA framebuffer 喂给 VENC，由 VENC 硬件做
			//色彩转换(BGRA->YUV)+旋转(SetChnRotation)，省掉每帧 rga 转屏(实测忙时 26ms)。
			//rtsp 仍走 VENC_DRM_H264(UYVY+rga)，互不影响。
			if(emsg.conf_ok){
				eContext->bitrate = camera_conf.vemipi_bitrate;
			}else{
				eContext->bitrate = 1024;
			}
			eContext->width = CONFIG_DRM_CROP_WIDTH;   //1920，真实 fb 宽
			eContext->height = CONFIG_DRM_CROP_HEIGHT; //1080，真实 fb 高
			eContext->veId = 0;
			eContext->veChn = 2;
			eContext->s32Grp = -1;
			eContext->enPixelFormat = RK_FMT_BGRA8888;
			eContext->enType = RK_VIDEO_ID_AVC;
			break;
		default:
			break;
	}
	return s32Ret;
}

static int mpp_create_ve(VencContext *eContext)
{
   	RK_S32 s32Ret = 0;

    VENC_CHN_ATTR_S stAttr;
	memset(&stAttr , 0x0 , sizeof(VENC_CHN_ATTR_S));
	stAttr.stVencAttr.enType = eContext->enType;
	stAttr.stVencAttr.enPixelFormat = eContext->enPixelFormat;
	if(stAttr.stVencAttr.enType == RK_VIDEO_ID_AVC){
		stAttr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
		stAttr.stRcAttr.stH264Cbr.u32Gop = 60;
		stAttr.stRcAttr.stH264Cbr.u32BitRate = eContext->bitrate;
	}else if(stAttr.stVencAttr.enType == RK_VIDEO_ID_MJPEG){
		/* JPEG 宽高必须 16 对齐。516、1080 会让 CreateChn 失败，红外和可见光都没有 jpg。 */
		int aw = (int)eContext->width & ~15;
		int ah = (int)eContext->height & ~15;
		if(aw >= 16 && ah >= 16 && (aw != (int)eContext->width || ah != (int)eContext->height)){
			printf("jpeg align %ux%u -> %dx%d\n", eContext->width, eContext->height, aw, ah);
			eContext->width = aw;
			eContext->height = ah;
		}
		stAttr.stRcAttr.enRcMode = VENC_RC_MODE_MJPEGCBR;
		stAttr.stRcAttr.stMjpegCbr.u32SrcFrameRateNum = 25;
		stAttr.stRcAttr.stMjpegCbr.u32SrcFrameRateDen = 1;
		stAttr.stRcAttr.stMjpegCbr.fr32DstFrameRateNum = 25;
		stAttr.stRcAttr.stMjpegCbr.fr32DstFrameRateDen = 1;
		stAttr.stRcAttr.stMjpegCbr.u32BitRate = 16384;
		stAttr.stRcAttr.stMjpegCbr.u32StatTime = 1;
	}
	stAttr.stVencAttr.u32PicWidth = eContext->width;
    stAttr.stVencAttr.u32PicHeight = eContext->height;
    stAttr.stVencAttr.u32VirWidth = eContext->width;
    stAttr.stVencAttr.u32VirHeight = eContext->height;
    stAttr.stVencAttr.u32StreamBufCnt = 5;
    stAttr.stVencAttr.u32BufSize = eContext->width * eContext->height;
	if(stAttr.stVencAttr.enType == RK_VIDEO_ID_MJPEG)
		stAttr.stVencAttr.u32BufSize = eContext->width * eContext->height * 3 / 2;

    s32Ret = RK_MPI_VENC_CreateChn(eContext->veChn, &stAttr);
	printf("RK_MPI_VENC_CreateChn %x, eContext->s32Grp=%d eContext->veChn=%d\n", s32Ret, eContext->s32Grp, eContext->veChn);

	if(eContext->s32Grp == 1 && eContext->veChn == 3){//rtsp时候，ir和mipi的s32Grp为1，跑这里
		VENC_CHN_PARAM_S stParam;
		RK_MPI_VENC_GetChnParam(eContext->veChn, &stParam);
		stParam.stCropCfg.enCropType = VENC_CROP_SCALE;
		stParam.stCropCfg.stScaleRect.stSrc.s32X = 0;
		stParam.stCropCfg.stScaleRect.stSrc.s32Y = 0;
		stParam.stCropCfg.stScaleRect.stSrc.u32Width = 0;
		stParam.stCropCfg.stScaleRect.stSrc.u32Height = 0;
		stParam.stCropCfg.stScaleRect.stDst.s32X = 0;
		stParam.stCropCfg.stScaleRect.stDst.s32Y = 0;
		stParam.stCropCfg.stScaleRect.stDst.u32Width = emsg.mipi_source_w;
		stParam.stCropCfg.stScaleRect.stDst.u32Height = emsg.mipi_source_h;
		RK_MPI_VENC_SetChnParam(eContext->veChn, &stParam);

		MPP_CHN_S stSrcChn, stDestChn;
		// bind vpss to venc
		stSrcChn.enModId    = RK_ID_VPSS;
		stSrcChn.s32DevId   = eContext->s32Grp;
		stSrcChn.s32ChnId   = VPSS_CHN2;

		stDestChn.enModId   = RK_ID_VENC;
		stDestChn.s32DevId  = eContext->veId;
		stDestChn.s32ChnId  = eContext->veChn;
		s32Ret = RK_MPI_SYS_Bind(&stSrcChn, &stDestChn);
		RK_LOGE("RK_MPI_SYS_Bind %x", s32Ret);
	}

	if(eContext->s32Grp == 1 && eContext->veChn == 1){
		VENC_CHN_PARAM_S stParam;
		RK_MPI_VENC_GetChnParam(eContext->veChn, &stParam);
		stParam.stCropCfg.enCropType = VENC_CROP_SCALE;
		stParam.stCropCfg.stScaleRect.stSrc.s32X = 0;
		stParam.stCropCfg.stScaleRect.stSrc.s32Y = 0;
		stParam.stCropCfg.stScaleRect.stSrc.u32Width = 0;
		stParam.stCropCfg.stScaleRect.stSrc.u32Height = 0;
		stParam.stCropCfg.stScaleRect.stDst.s32X = 0;
		stParam.stCropCfg.stScaleRect.stDst.s32Y = 0;
		stParam.stCropCfg.stScaleRect.stDst.u32Width = eContext->width;
		stParam.stCropCfg.stScaleRect.stDst.u32Height = eContext->height;
		RK_MPI_VENC_SetChnParam(eContext->veChn, &stParam);
	}


	if(
        (eContext->veChn == 0) || //ir模式
        ((eContext->veChn==3)&&(camera_rtsp_get_mode()==CAP_RTSP_RTMP_IR))
    ) {//rtsp/rtmp模式时候，如果是ir模式,ir也需要截取数据
        VENC_CHN_PARAM_S stParam;
		RK_MPI_VENC_GetChnParam(eContext->veChn, &stParam);
		stParam.stCropCfg.enCropType = VENC_CROP_ONLY;
        /*这里设置拍照和录像参数，只录取红外后半部分的yuv数据*/
        stParam.stCropCfg.stCropRect.s32X = 640;
        stParam.stCropCfg.stCropRect.s32Y = 0;
        stParam.stCropCfg.stCropRect.u32Width = 640;
        stParam.stCropCfg.stCropRect.u32Height = 512;
		RK_MPI_VENC_SetChnParam(eContext->veChn, &stParam);

    }


	if(eContext->veChn == 2){//drm录屏时候:eContext->s32Grp=-1 eContext->veChn=2
		VENC_CHN_PARAM_S stParam;
		RK_MPI_VENC_GetChnParam(eContext->veChn, &stParam);
		stParam.stCropCfg.enCropType = VENC_CROP_ONLY;

		if((emsg.drm_source_w > 0)&&(emsg.drm_source_h > 0)){
			stParam.stCropCfg.stCropRect.s32X = emsg.drm_source_x;
			stParam.stCropCfg.stCropRect.s32Y = emsg.drm_source_y;
			stParam.stCropCfg.stCropRect.u32Width = emsg.drm_source_w;
			stParam.stCropCfg.stCropRect.u32Height = emsg.drm_source_h;
		}else{
			stParam.stCropCfg.stCropRect.s32X = CONFIG_DRM_CROP_X;
			stParam.stCropCfg.stCropRect.s32Y = CONFIG_DRM_CROP_Y;
			stParam.stCropCfg.stCropRect.u32Width = CONFIG_DRM_CROP_WIDTH;
			stParam.stCropCfg.stCropRect.u32Height = CONFIG_DRM_CROP_HEIGHT;
		}
		RK_MPI_VENC_SetChnParam(eContext->veChn, &stParam);
		//录屏 BGRA 路径：用 VENC 硬件旋转代替 rga 转屏。旋转角与拍照/录屏一致，
		//动态取自 camera_drm_get_rga_rotate_deg()(抵消 weston 旋转，常态返回 180)。
		//仅对 BGRA8888 生效，避免影响同样用 veChn==2 的 DRM JPG 拍照(UYVY/MJPEG)。
		if(eContext->enPixelFormat == RK_FMT_BGRA8888){
			ROTATION_E enRot = ROTATION_0;
			switch(camera_drm_get_rga_rotate_deg()){
				case 90:  enRot = ROTATION_90;  break;
				case 180: enRot = ROTATION_180; break;
				case 270: enRot = ROTATION_270; break;
				default:  enRot = ROTATION_0;   break;
			}
			RK_S32 rotRet = RK_MPI_VENC_SetChnRotation(eContext->veChn, enRot);
			printf("VENC SetChnRotation chn=%d rot=%d ret=%x\n", eContext->veChn, (int)enRot, rotRet);
		}

	} else if(eContext->veChn == 3) {//rtsp
#if 1
        if (camera_rtsp_get_mode()==CAP_RTSP_RTMP_SCREEN) {//rtsp 的drm模式时候设置这里
            VENC_CHN_PARAM_S stParam;
            RK_MPI_VENC_GetChnParam(eContext->veChn, &stParam);
            stParam.stCropCfg.enCropType = VENC_CROP_ONLY;

            stParam.stCropCfg.stCropRect.s32X = CONFIG_DRM_CROP_X;
            stParam.stCropCfg.stCropRect.s32Y = CONFIG_DRM_CROP_Y;
            stParam.stCropCfg.stCropRect.u32Width = CONFIG_DRM_CROP_WIDTH;
            stParam.stCropCfg.stCropRect.u32Height = CONFIG_DRM_CROP_HEIGHT;
            RK_MPI_VENC_SetChnParam(eContext->veChn, &stParam);
            //rtsp 录屏 BGRA 路径：与 recorder 一致，用 VENC 硬件旋转代替 rga 转屏。
            //仅对 BGRA8888 生效(IR/VL rtsp 走 VPSS 绑定、非 BGRA，不受影响)。
            if(eContext->enPixelFormat == RK_FMT_BGRA8888){
                ROTATION_E enRot = ROTATION_0;
                switch(camera_drm_get_rga_rotate_deg()){
                    case 90:  enRot = ROTATION_90;  break;
                    case 180: enRot = ROTATION_180; break;
                    case 270: enRot = ROTATION_270; break;
                    default:  enRot = ROTATION_0;   break;
                }
                RK_S32 rotRet = RK_MPI_VENC_SetChnRotation(eContext->veChn, enRot);
                printf("VENC SetChnRotation chn=%d rot=%d ret=%x\n", eContext->veChn, (int)enRot, rotRet);
            }
        }
#endif
	}


	return s32Ret;
}

static int mpp_start_ve(VencContext *eContext)
{
   	RK_S32 s32Ret = 0;

    VENC_RECV_PIC_PARAM_S   stRecvParam;
	memset(&stRecvParam, 0, sizeof(VENC_RECV_PIC_PARAM_S));
    stRecvParam.s32RecvPicNum = -1;
    //stRecvParam.s32RecvPicNum = 1;
	s32Ret = RK_MPI_VENC_StartRecvFrame(eContext->veChn, &stRecvParam);
	RK_LOGE("RK_MPI_VENC_StartRecvFrame chl %d ret %x", eContext->veChn,s32Ret);

	return s32Ret;
}

static int mpp_stop_ve(VencContext *eContext)
{
   	RK_S32 s32Ret = 0;
	return s32Ret;
}

static int mpp_destroy_ve(VencContext *eContext)
{
   	RK_S32 s32Ret = 0;

	s32Ret = RK_MPI_VENC_StopRecvFrame(eContext->veChn);
	RK_LOGE("RK_MPI_VENC_StopRecvFrame %x", s32Ret);

	s32Ret = RK_MPI_VENC_ResetChn(eContext->veChn);
	RK_LOGE("RK_MPI_VENC_ResetChn %x , w %d h %d", s32Ret,eContext->width,eContext->height);

	if(eContext->s32Grp == 1 && eContext->veChn == 3){
		MPP_CHN_S stSrcChn, stDestChn;
		// Unbind vpss to venc (rtsp only)
		stSrcChn.enModId    = RK_ID_VPSS;
		stSrcChn.s32DevId   = eContext->s32Grp;
		stSrcChn.s32ChnId   = VPSS_CHN2;

		stDestChn.enModId   = RK_ID_VENC;
		stDestChn.s32DevId  = eContext->veId;
		stDestChn.s32ChnId  = eContext->veChn;
		s32Ret = RK_MPI_SYS_UnBind(&stSrcChn, &stDestChn);
		RK_LOGE("RK_MPI_SYS_UnBind %x", s32Ret);
	}

	s32Ret = RK_MPI_VENC_DestroyChn(eContext->veChn);
	RK_LOGE("RK_MPI_VENC_DestroyChn %x", s32Ret);

	return s32Ret;
}

void mpp_ve_get_api(struct Mpp_venc_api *api)
{
	api->ve_init = mpp_ve_init;
	api->create_ve = mpp_create_ve;
	api->start_ve = mpp_start_ve;
	api->stop_ve = mpp_stop_ve;
	api->destroy_ve = mpp_destroy_ve;
}

/**********************
  * MPP API
**********************/
static void mpp_log_put(int ret,int index, char *p)
{
	switch(index){
		case 0:
			if(ret == 0){
				g_printf("[dvp] %s %s",p,"sucess");
			}else{
				r_printf("[dvp] %s %s",p,"fail");
			}
			break;
		case 3:
			if(ret == 0){
				g_printf("[mipi] %s %s",p,"sucess");
			}else{
				r_printf("[mipi] %s %s",p,"fail");
			}
			break;
		}
}

int mpp_fun_init(struct Mpp_api *api,VippContext *vContext,VoContext *oContext)
{
	int ret = SUCCESS;
	//init mpp vi
	if(api->vi_init){
		ret = api->vi_init(vContext);
		mpp_log_put(ret,vContext->ccd_index,"mpp_vi init");
	}
	//init mpp vo
	//if(api->vo_init){
	//	ret = api->vo_init(oContext);
	//	mpp_log_put(ret,vContext->ccd_index,"mpp_vo init");
	//}
	//create mpp vi
	if(api->create_vi){
		ret = api->create_vi(vContext);
		mpp_log_put(ret,vContext->ccd_index,"mpp_create vi");
	}
	//create mpp vo
	//if(api->create_vo){
	//	ret = api->create_vo(vContext, oContext);
	//	mpp_log_put(ret,vContext->ccd_index,"mpp_create vo");
	//}
	return ret;
}

int mpp_fun_run(struct Mpp_api *api,VippContext *vContext,VoContext *oContext)
{
	int ret = SUCCESS;
	//start vi
	if(api->start_vi){
		ret = api->start_vi(vContext);
		mpp_log_put(ret,vContext->ccd_index,"mpp_start vi");
	}
	//start vo
	if(api->start_vo){
		ret = api->start_vo(vContext,oContext);
		mpp_log_put(ret,vContext->ccd_index,"mpp_start vo");
	}
	return ret;
}

int mpp_fun_stop(struct Mpp_api *api,VippContext *vContext,VoContext *oContext)
{
	int ret = SUCCESS;
	//stop vo
	if(api->stop_vo){
		ret = api->stop_vo(oContext);
		mpp_log_put(ret,vContext->ccd_index,"mpp_stop vo");
	}
	//stop vi
	if(api->stop_vi){
		ret = api->stop_vi(vContext);
		mpp_log_put(ret,vContext->ccd_index,"mpp_stop vi");
	}

	return ret;
}

int mpp_fun_destroy(struct Mpp_api *api,VippContext *vContext,VoContext *oContext)
{
	int ret = SUCCESS;
	//stop vo
	if(api->stop_vo){
		ret = api->stop_vo(oContext);
		mpp_log_put(ret,vContext->ccd_index,"mpp_stop vo");
	}
	//stop vi
	if(api->stop_vi){
		ret = api->stop_vi(vContext);
		mpp_log_put(ret,vContext->ccd_index,"mpp_stop vi");
	}
	//destroy vo
	if(api->destroy_vo){
		ret = api->destroy_vo(oContext);
		mpp_log_put(ret,vContext->ccd_index,"mpp_destroy vo");
	}
	//destroy vi
	if(api->destroy_vi){
		ret = api->destroy_vi(vContext);
		mpp_log_put(ret,vContext->ccd_index,"mpp_destroy vi");
	}
	return ret;
}

void display_get_api(struct Mpp_api *api)
{
	api->vi_init = mpp_vi_init;
	api->vo_init = NULL;
	api->create_vi = mpp_create_vi;
	api->create_vo = NULL;
	api->start_vi = mpp_start_vi;
	api->start_vo = NULL;
	api->stop_vi = mpp_stop_vi;
	api->stop_vo = NULL;
	api->destroy_vi = mpp_destroy_vi;
	api->destroy_vo = NULL;
}