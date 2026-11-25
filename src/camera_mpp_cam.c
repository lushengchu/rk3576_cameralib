#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <arpa/inet.h>
#include "rk_mpi_vi.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vpss.h"
#include "rk_mpi_vo.h"
#include "rk_mpi_mb.h"
#include "camera_main.h"
#include "gas_quantification.h"
#include "rga.h"
#include "drmrga.h"
#include <drm_fourcc.h>

#ifdef __cplusplus
extern "C" {
#endif
int rga_splict_image(void *src_buf, void *yuv_buf, void *raw_buf);
#ifdef __cplusplus
}
#endif
/******************************************************************************
 *  Private Defines and Macros
 */
#define DVP_GRP 		CAM_GRP_INDEX_DVP
#define MIPI_GRP 		CAM_GRP_INDEX_MIPI

/******************************************************************************
 *  Private Variable Prototypes
 */
static volatile unsigned char wbc_loop = 0;
static volatile unsigned char dvp_loop = 0;
static VIDEO_FRAME_INFO_S dvp_stViFrame[3];
static volatile unsigned char mipi_loop = 0;
static VIDEO_FRAME_INFO_S mipi_stViFrame[3];

/******************************************************************************
 *  Private Function Prototypes
 */

 /******************************************************************************
 *  Public Function Prototypes
 */

/**********************************************************************
 * Description:
 * Parameter:
 * Return:
 * Note:
 **********************************************************************/
RK_U64 TEST_COMM_GetNowUs()
{
    struct timespec time = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (RK_U64)time.tv_sec * 1000000 + (RK_U64)time.tv_nsec / 1000; /* microseconds */
}
/**********************************************************************
 * Description:
 * Parameter:
 * Return:
 * Note:
 **********************************************************************/
void get_save_filename(SAVE_FILETYPE type)
{
	char timebuf[64] = {0x00};
	struct timeval tv;
	struct tm *lt;

	gettimeofday(&tv, NULL);
	lt = localtime(&tv.tv_sec);
	snprintf(timebuf, sizeof(timebuf) - 1,
			"%d%02d%02d_%02d%02d%02d",
			lt->tm_year + 1900,
			lt->tm_mon + 1,
			lt->tm_mday,
			lt->tm_hour,
			lt->tm_min,
			lt->tm_sec);

	if (emsg.mediaPath[0] == 0x0) {
		strcpy(emsg.mediaPath, "/tmp");
	}
	if (emsg.imgTmpPath[0] == 0x0) {
		strcpy(emsg.imgTmpPath, "/tmp");
	}
	switch(type){
		case SAVE_CAPTURE_JPG:
			if (emsg.capture_path[0][0] != 0x0){
				strcpy(timebuf, emsg.capture_path[0]);
			}
			snprintf(emsg.capture_path[0], MAX_FILE_PATH_SIZE - 1,"%s/IMG_ir_%s.jpg",emsg.imgTmpPath,timebuf);
			snprintf(emsg.capture_path[1], MAX_FILE_PATH_SIZE - 1,"%s/IMG_ir_%s.raw",emsg.imgTmpPath,timebuf);
			snprintf(emsg.capture_path[2], MAX_FILE_PATH_SIZE - 1,"%s/IMG_ir_%s.yuv",emsg.imgTmpPath,timebuf);
			snprintf(emsg.capture_path[3], MAX_FILE_PATH_SIZE - 1,"%s/IMG_tv_%s.jpg",emsg.imgTmpPath,timebuf);
			snprintf(emsg.capture_path[4], MAX_FILE_PATH_SIZE - 1,"%s/IMG_dp_%s.jpg",emsg.imgTmpPath,timebuf);
			break;
		case SAVE_RECORDER_MP4:
			if (emsg.record_path[0][0] != 0x0){
				strcpy(timebuf, emsg.record_path[0]);
			}
			snprintf(emsg.record_path[0], MAX_FILE_PATH_SIZE - 1,"%s/VIDr_%s.mp4",emsg.mediaPath,timebuf);
			snprintf(emsg.record_path[1], MAX_FILE_PATH_SIZE - 1,"%s/VIDr_%s.raw",emsg.mediaPath,timebuf);
			snprintf(emsg.record_path[2], MAX_FILE_PATH_SIZE - 1,"%s/VIDn_%s.mp4",emsg.mediaPath,timebuf);
			snprintf(emsg.record_path[3], MAX_FILE_PATH_SIZE - 1,"%s/VIDd_%s.mp4",emsg.mediaPath,timebuf);
			break;
		default:
			break;
	}
}
/**********************************************************************
 * Description: Test file
 * Parameter:
 * Return:
 * Note:
 **********************************************************************/
FILE *test_file_create(char *file)
{
	FILE *fp;
    fp = fopen(file, "wb+");
	return fp;
}
void test_file_write(FILE *fp, void *data, int len)
{
	fwrite(data, 1, len, fp);
	fflush(fp);
}
void test_file_close(FILE *fp)
{
	fclose(fp);
}

/**********************************************************************
 * Description: wbc_loop
 * Parameter:
 * Return:
 * Note:
 **********************************************************************/
int camera_wbc_get_loop(void)
{
	return wbc_loop;
}
void camera_wbc_set_loop(unsigned char loop)
{
	wbc_loop = loop;
}
void camera_mpp_wbc_loop(VippContext *vContext)
{
	RK_S32 s32Ret;
	RK_U32 VoWbc;
    VO_WBC_SOURCE_S stWbcSource;
    VO_WBC_ATTR_S stWbcAttr;
    VIDEO_FRAME_INFO_S stViFrame;
    static FILE *fp;
	void *data;
	g_printf("%s strat ",__func__);

    /* WBC bind source */
    VoWbc = 0;
	memset(&stWbcSource, 0x0 ,sizeof(VO_WBC_SOURCE_S));
    stWbcSource.enSourceType = VO_WBC_SOURCE_VIDEO;
    stWbcSource.u32SourceId = RK358X_VO_DEV_HD0;
    s32Ret = RK_MPI_VO_SetWbcSource(VoWbc, &stWbcSource);
	if (s32Ret < 0) {
		RK_LOGE("RK_MPI_VO_SetWbcSource failed, ret=%x", s32Ret);
	}

    s32Ret = RK_MPI_VO_SetWbcDepth(VoWbc, 5);
	if (s32Ret < 0) {
		RK_LOGE("RK_MPI_VO_SetWbcDepth failed, ret=%x", s32Ret);
	}

	memset(&stWbcAttr, 0x0 ,sizeof(VO_WBC_ATTR_S));
    //stWbcAttr.enPixelFormat = RK_FMT_YUV420SP;
    stWbcAttr.enPixelFormat = RK_FMT_RGB888;
    stWbcAttr.stTargetSize.u32Width =  1024;
    stWbcAttr.stTargetSize.u32Height =  768;
    s32Ret = RK_MPI_VO_SetWbcAttr(VoWbc, &stWbcAttr);
	if (s32Ret < 0) {
		RK_LOGE("RK_MPI_VO_SetWbcAttr failed, ret=%x", s32Ret);
	}
    s32Ret = RK_MPI_VO_EnableWbc(VoWbc);
	if (s32Ret < 0) {
		RK_LOGE("RK_MPI_VO_EnableWbc failed, ret=%x", s32Ret);
	}

	wbc_loop = 1;
    fp = fopen("/tmp/wbc.yuv", "wb+");
	memset(&stViFrame, 0x0 ,sizeof(VIDEO_FRAME_INFO_S));
	while (1) {
		// wbc get frame
		s32Ret = RK_MPI_VO_GetWbcFrame(VoWbc, &stViFrame, 33);
		// printf("stViFrame.stVFrame.u32Width=%d,stViFrame.stVFrame.u32Height=%d\n", stViFrame.stVFrame.u32Width, stViFrame.stVFrame.u32Height);
		if (s32Ret == RK_SUCCESS){
			data = RK_MPI_MB_Handle2VirAddr(stViFrame.stVFrame.pMbBlk);
			if (data == NULL) {
				RK_LOGE("RK_MPI_MB_Handle2VirAddr failed, data is NULL");
			}
			// RK_MPI_MB_GetLength(ctx->stViFrame.stVFrame.pMbBlk)
			fwrite(data, 1, stViFrame.stVFrame.u32Width * stViFrame.stVFrame.u32Height * 3, fp);
			fflush(fp);
			s32Ret = RK_MPI_VO_ReleaseWbcFrame(VoWbc, &stViFrame);
			if (s32Ret < 0) {
				RK_LOGE("RK_MPI_VO_ReleaseWbcFrame failed, ret=%x", s32Ret);
			}
		}
		usleep(33000);

		if (wbc_loop == 0){
			break;
		}

	}
	fclose(fp);
	s32Ret = RK_MPI_VO_DisableWbc(VoWbc);
	if (s32Ret < 0) {
		RK_LOGE("RK_MPI_VO_DisableWbc failed, ret=%x", s32Ret);
	}

	g_printf("%s end", __func__);
	return;
}
/**********************************************************************
 * Description: DVP
 * Parameter:
 * Return:
 * Note:
 **********************************************************************/
int camera_dvp_get_loop(void)
{
	return dvp_loop;
}
void camera_dvp_set_loop(unsigned char loop)
{
	dvp_loop = loop;
}

void *camera_mpp_dvp_loop(void *p)
{
	RK_S32 i, s32Ret;
	struct dis_context *msg = (struct dis_context *)p;
	VippContext *vContext = &msg->disViContext[0];
	void *data = NULL;
    RK_S32 waitTime = 200;
	int errCount = 0;
	int delayptsCount = 0;
	memset(&dvp_stViFrame[0], 0x0 ,sizeof(VIDEO_FRAME_INFO_S));
	memset(&dvp_stViFrame[1], 0x0 ,sizeof(VIDEO_FRAME_INFO_S));
	memset(&dvp_stViFrame[2], 0x0 ,sizeof(VIDEO_FRAME_INFO_S));
	dvp_loop = 1;
	int len = vContext->width * vContext->height * 2;
	int len_line = vContext->width * 2;
	int index = 0;
	emsg.dvp_source_x = 0;
	emsg.dvp_source_y = 0;
	emsg.dvp_source_w = vContext->width;
	emsg.dvp_source_h = vContext->height - 2;
	int GasLeakOsdUpdate = 0;
	uint8_t *LeakData = malloc(emsg.dvp_source_w * emsg.dvp_source_h * 2);
	uint64_t LeakIndex = 0;
	double LeakRate = 0, LeakIntegral = 0, LeakAvg = 0;
	double Leakppm = 0;
	g_printf("%s strat ",__func__);

    const int ARRAY_SIZE = 3;
    const int ELEMENT_SIZE = 640 * 516 * 2;
    char (*dvp_yuv_data)[ELEMENT_SIZE] = NULL;
    char (*dvp_raw_data)[ELEMENT_SIZE] = NULL;

    dvp_yuv_data = (char (*)[ELEMENT_SIZE])malloc(ARRAY_SIZE * sizeof(*dvp_yuv_data));
    dvp_raw_data = (char (*)[ELEMENT_SIZE])malloc(ARRAY_SIZE * sizeof(*dvp_raw_data));
    if (dvp_yuv_data == NULL || dvp_raw_data == NULL) {
        // 内存分配失败，处理错误
        fprintf(stderr, "dvp_yuv_data malloc error\n");
        // 释放已分配的内存
        if (dvp_yuv_data != NULL) {
            free(dvp_yuv_data);
        }
        if (dvp_raw_data != NULL) {
            free(dvp_raw_data);
        }
        return -1;
    }

	while (1) {
		if (dvp_loop == 0) {
			break;
		}
		if ((dvp_stViFrame[0].stVFrame.u64PTS) && (dvp_stViFrame[1].stVFrame.u64PTS) && (dvp_stViFrame[2].stVFrame.u64PTS)) {
			if (++delayptsCount >= 40) {
				delayptsCount = 0;
			//	 r_printf("timeout %dms dvp delay, need release %lu %lu %lu", delayptsCount, dvp_stViFrame[0].stVFrame.u64PTS, dvp_stViFrame[1].stVFrame.u64PTS, dvp_stViFrame[2].stVFrame.u64PTS);
			}
			usleep(1000);
			continue;
		}
		for (i = 0; i < 3; i++) {
			if (dvp_stViFrame[i].stVFrame.u64PTS == 0) {
				index = i;
			}
		}
		delayptsCount = 0;

		s32Ret = RK_MPI_VPSS_GetChnFrame(vContext->s32Grp, VPSS_CHN0, &dvp_stViFrame[index], waitTime);
		if (s32Ret == RK_SUCCESS) {
			errCount = 0;
			// y_printf("dvp get pts: %lu, PTS %lu %lu %lu\n", dvp_stViFrame[index].stVFrame.u64PTS, dvp_stViFrame[0].stVFrame.u64PTS, dvp_stViFrame[1].stVFrame.u64PTS, dvp_stViFrame[2].stVFrame.u64PTS);

			s32Ret = RK_MPI_SYS_MmzFlushCache(dvp_stViFrame[index].stVFrame.pMbBlk, RK_TRUE);
			if (s32Ret < 0) {
				RK_LOGE("RK_MPI_SYS_MmzFlushCache failed, ret=%x", s32Ret);
			}
			
            data = RK_MPI_MB_Handle2VirAddr(dvp_stViFrame[index].stVFrame.pMbBlk);
			if (data == NULL) {
				RK_LOGE("RK_MPI_MB_Handle2VirAddr failed, data is NULL");
			}


          //  RK_LOGE("sclu %s %d get dvp data u64PTS=%lu\n", __func__, __LINE__, dvp_stViFrame[index].stVFrame.u64PTS);
            rga_splict_image(data, &dvp_yuv_data[index], &dvp_raw_data[index]);
           // drmDspFrame(1280, 512, 1280, 512, -1,
             //       data, DRM_FORMAT_NV12);
         //   display_win1(data, 0,  RK_FORMAT_YCbCr_420_SP, 1280, 512, 0);

            if (cmsg.qtinfo.dvp_dis.get_frame)
                cmsg.qtinfo.dvp_dis.get_frame(cmsg.qtinfo.dvp_dis.context, dvp_yuv_data[index], len/2, len_line/2, dvp_stViFrame[index].stVFrame.u64PTS);
            
            if (cmsg.qtinfo.dvp_raw.get_frame)
                cmsg.qtinfo.dvp_raw.get_frame(cmsg.qtinfo.dvp_raw.context, dvp_raw_data[index], len/2, len_line/2, dvp_stViFrame[index].stVFrame.u64PTS);
#if 0
			if (check_infrared_frame_tag(data) == INFRARED_FRAME_TAG_1122) {
				if (dvp_loop) {
					/* 俄罗斯半定量算法 */
					if ((emsg.gas_analit_en == 1) && (emsg.gas_analit_vendor == GAS_ANALYSE_SELECT_RUSS)) {
						// A. 处理一帧原始图像数据
						s32Ret = camera_gas_quantification_dealNewFrame_russ(data, LeakData, &LeakIndex, &LeakRate, &LeakIntegral, &LeakAvg);
						if (s32Ret < 0) {
							r_printf("camera_gas_quantification_dealNewFrame_russ failed\n");
						}
						// B. 当前帧的处理结果送给QT
						cmsg.qtinfo.dvp_dis.get_frame(cmsg.qtinfo.dvp_dis.context, LeakData, len, len_line, dvp_stViFrame[index].stVFrame.u64PTS);
						cmsg.qtinfo.gas_leak.get_gas_leak(cmsg.qtinfo.gas_leak.context, LeakIndex, LeakRate, LeakIntegral, LeakAvg);
						GasLeakOsdUpdate = 1;
					}
					/* 西安半定量算法 */
					else if ((emsg.gas_analit_en == 1) && (emsg.gas_analit_vendor == GAS_ANALYSE_SELECT_XIAN)) {
						// A. 处理一帧原始图像数据(仅拷贝)
						s32Ret = camera_gas_quantification_dealNewFrame_xian(data, LeakData, &Leakppm, INFRARED_FRAME_TAG_1122);
						if (s32Ret < 0) {
							r_printf("camera_gas_quantification_dealNewFrame_xian YUV failed\n");
						}
						// B. 上一帧的处理结果送给QT
						cmsg.qtinfo.dvp_dis.get_frame(cmsg.qtinfo.dvp_dis.context, LeakData, len, len_line, dvp_stViFrame[index].stVFrame.u64PTS);
						cmsg.qtinfo.gas_leak.get_gas_leak(cmsg.qtinfo.gas_leak.context, 0, 0, 0, Leakppm);
						GasLeakOsdUpdate = 1;
					}
					else {
						cmsg.qtinfo.dvp_dis.get_frame(cmsg.qtinfo.dvp_dis.context, data, len, len_line, dvp_stViFrame[index].stVFrame.u64PTS);
						if (GasLeakOsdUpdate == 1) {
							cmsg.qtinfo.gas_leak.get_gas_leak(cmsg.qtinfo.gas_leak.context, 0, 0, 0, 0);
							GasLeakOsdUpdate = 0;
						}
					}
				}
				else {
					s32Ret = RK_MPI_VPSS_ReleaseChnFrame(vContext->s32Grp, VPSS_CHN0, &dvp_stViFrame[index]);
					if (s32Ret < 0) {
						RK_LOGE("RK_MPI_VPSS_ReleaseChnFrame failed, ret=%x", s32Ret);
					}
					dvp_stViFrame[index].stVFrame.u64PTS = 0;
				}
			}
			else if (check_infrared_frame_tag(data) == INFRARED_FRAME_TAG_55AA) {
				if (dvp_loop) {
					// A. 原始RAW数据送给QT
					cmsg.qtinfo.dvp_raw.get_frame(cmsg.qtinfo.dvp_raw.context, data, len, len_line, dvp_stViFrame[index].stVFrame.u64PTS);
					// B. 处理一帧ARW数据
					if ((emsg.gas_analit_en == 1) && (emsg.gas_analit_vendor == GAS_ANALYSE_SELECT_XIAN)) {
						s32Ret = camera_gas_quantification_dealNewFrame_xian(data, NULL, NULL, INFRARED_FRAME_TAG_55AA);
						if (s32Ret < 0) {
							r_printf("camera_gas_quantification_dealNewFrame_xian RAW failed\n");
						}
					}
				}
				else {
					s32Ret = RK_MPI_VPSS_ReleaseChnFrame(vContext->s32Grp, VPSS_CHN0, &dvp_stViFrame[index]);
					if (s32Ret < 0) {
						RK_LOGE("RK_MPI_VPSS_ReleaseChnFrame failed, ret=%x", s32Ret);
					}
					dvp_stViFrame[index].stVFrame.u64PTS = 0;
				}
			}
			else
#endif

//1. 这里不能直接RK_MPI_VPSS_ReleaseChnFrame,而是qt处理完这一帧数据后，调用camera_mpp_dvp_release来释放的
//2. 这里现在使用了rga_splict_image函数，已经把数据拷贝出去了，可以直接release
#if 1
            {
				s32Ret = RK_MPI_VPSS_ReleaseChnFrame(vContext->s32Grp, VPSS_CHN0, &dvp_stViFrame[index]);
				if (s32Ret < 0) {
					RK_LOGE("RK_MPI_VPSS_ReleaseChnFrame failed, ret=%x", s32Ret);
				}
				dvp_stViFrame[index].stVFrame.u64PTS = 0;
			}
#endif
		}
		else {
			errCount++;
			r_printf("dvp RK_MPI_VPSS_GetChnFrame timeout %x, err %d, cur pts %lu %lu %lu", s32Ret,errCount,dvp_stViFrame[0].stVFrame.u64PTS, dvp_stViFrame[1].stVFrame.u64PTS, dvp_stViFrame[2].stVFrame.u64PTS);
			if (errCount >= 25) { 	// waitTime * 25 = 5s
				errCount = 0;
				camera_update_qt_sta(ERR_DVP_LOOP_TIMEOUT);
			}
		}
	}
#if 0
	while(1) {
		if ((!dvp_stViFrame[0].stVFrame.u64PTS) && (!dvp_stViFrame[1].stVFrame.u64PTS) && (!dvp_stViFrame[2].stVFrame.u64PTS)) {
			break;
		}
		usleep(1000);
	}
#endif
    free(dvp_yuv_data);
    free(dvp_raw_data);
	free(LeakData);
	g_printf("%s end",__func__);
	return NULL;
}
void camera_mpp_dvp_release(uint64_t pts)
{
#if 0
	RK_S32 s32Ret;

//	 g_printf("dvp release pts: %lu, PTS %lu %lu %lu \n", pts, dvp_stViFrame[0].stVFrame.u64PTS, dvp_stViFrame[1].stVFrame.u64PTS, dvp_stViFrame[2].stVFrame.u64PTS);
	if (pts == dvp_stViFrame[0].stVFrame.u64PTS) {
		s32Ret = RK_MPI_VPSS_ReleaseChnFrame(DVP_GRP, VPSS_CHN0, &dvp_stViFrame[0]);
		if (s32Ret < 0) {
			RK_LOGE("RK_MPI_VO_ReleaseWbcFrame failed, ret=%x", s32Ret);
		}
		dvp_stViFrame[0].stVFrame.u64PTS = 0;
	}
	else if (pts == dvp_stViFrame[1].stVFrame.u64PTS) {
		s32Ret = RK_MPI_VPSS_ReleaseChnFrame(DVP_GRP, VPSS_CHN0, &dvp_stViFrame[1]);
		if (s32Ret < 0) {
			RK_LOGE("RK_MPI_VO_ReleaseWbcFrame failed, ret=%x", s32Ret);
		}
		dvp_stViFrame[1].stVFrame.u64PTS = 0;
	}
	else if (pts == dvp_stViFrame[2].stVFrame.u64PTS) {
		s32Ret = RK_MPI_VPSS_ReleaseChnFrame(DVP_GRP, VPSS_CHN0, &dvp_stViFrame[2]);
		if (s32Ret < 0) {
			RK_LOGE("RK_MPI_VO_ReleaseWbcFrame failed, ret=%x", s32Ret);
		}
		dvp_stViFrame[2].stVFrame.u64PTS = 0;
	}
	else {
		r_printf("dvp err release pts: %lu, PTS %lu %lu %lu", pts, dvp_stViFrame[0].stVFrame.u64PTS, dvp_stViFrame[1].stVFrame.u64PTS, dvp_stViFrame[2].stVFrame.u64PTS);
	}

#endif
}

#if 0
uint16_t check_infrared_frame_tag(void *data)
{
    uint16_t i, tag = 0;
    uint32_t offset;

	if (emsg.conf_ok){
		offset = camera_conf.vidvp.width * (camera_conf.vidvp.height - CONFIG_DVP_SRC_INFO) * 2;
	}
	else{
		offset = CONFIG_DVP_SRC_WIDTH * (CONFIG_DVP_SRC_HEIGHT - CONFIG_DVP_SRC_INFO) * 2;
	}

    for (i = 0; i < 4; i++) {
        tag |= *(uint16_t *)(data + offset + 2 * i);
    }

	tag = ntohs(tag);
    if ((tag != INFRARED_FRAME_TAG_1122) && (tag != INFRARED_FRAME_TAG_55AA) && (tag != INFRARED_FRAME_TAG_66BB)) {
        r_printf("!!!ERR unknow infrared frame type %x", tag);
    }

    return tag;
}
#endif
/**********************************************************************
 * Description: MIPI
 * Parameter:
 * Return:
 * Note:
 **********************************************************************/
int camera_mipi_get_loop(void)
{
	return mipi_loop;
}
void camera_mipi_set_loop(unsigned char loop)
{
	mipi_loop = loop;
}
void *camera_mpp_mipi_loop(void *p)
{
	RK_S32 i, s32Ret;
	struct dis_context *msg = (struct dis_context *)p;
	VippContext *vContext = &msg->disViContext[1];
    RK_S32 waitTime = 200;
	int errCount = 0;
	int delayptsCount = 0;
	memset(&mipi_stViFrame[0], 0x0 ,sizeof(VIDEO_FRAME_INFO_S));
	memset(&mipi_stViFrame[1], 0x0 ,sizeof(VIDEO_FRAME_INFO_S));
	memset(&mipi_stViFrame[2], 0x0 ,sizeof(VIDEO_FRAME_INFO_S));
	mipi_loop = 1;
	int index = 0;
	int len = 0, len_line = 0;
	void *data;
	g_printf("%s strat ",__func__);

	while (1) {
		if (mipi_loop == 0) {
			break;
		}
		if ((mipi_stViFrame[0].stVFrame.u64PTS) && (mipi_stViFrame[1].stVFrame.u64PTS) && (mipi_stViFrame[2].stVFrame.u64PTS)) {
			if (++delayptsCount >= 40){
				// r_printf("timeout %dms mipi delay, need release %lu %lu %lu", delayptsCount, mipi_stViFrame[0].stVFrame.u64PTS, mipi_stViFrame[1].stVFrame.u64PTS, mipi_stViFrame[2].stVFrame.u64PTS);
				delayptsCount = 0;
			}
			usleep(1000);
			continue;
		}
		for (i = 0; i < 3; i++){
			if (mipi_stViFrame[i].stVFrame.u64PTS == 0) {
				index = i;
			}
		}
		delayptsCount = 0;
		s32Ret = RK_MPI_VPSS_GetChnFrame(vContext->s32Grp, VPSS_CHN0, &mipi_stViFrame[index], waitTime);
		if (s32Ret == RK_SUCCESS) {
			errCount = 0;
			// y_printf("mipi get pts: %lu PTS %lu %lu %lu\n", mipi_stViFrame[index].stVFrame.u64PTS, mipi_stViFrame[0].stVFrame.u64PTS, mipi_stViFrame[1].stVFrame.u64PTS, mipi_stViFrame[2].stVFrame.u64PTS);
			data = RK_MPI_MB_Handle2VirAddr(mipi_stViFrame[index].stVFrame.pMbBlk);
			if (data == NULL) {
				RK_LOGE("RK_MPI_MB_Handle2VirAddr failed, data is NULL");
			}
			s32Ret = RK_MPI_SYS_MmzFlushCache(mipi_stViFrame[index].stVFrame.pMbBlk, RK_TRUE);
			if (s32Ret < 0) {
				RK_LOGE("RK_MPI_SYS_MmzFlushCache failed, ret=%x", s32Ret);
			}
			len = emsg.mipi_source_w * emsg.mipi_source_h * 4;
			len_line = emsg.mipi_source_w * 4;
			if (mipi_loop){
				cmsg.qtinfo.mipi_dis.get_frame(cmsg.qtinfo.mipi_dis.context, data, len, len_line, mipi_stViFrame[index].stVFrame.u64PTS);
			} else {
				/*
				 * 正在退出(mipi_loop 已被置 0)：这一帧是刚取到的，不会再交给上层回调去释放，
				 * 必须在这里直接释放。否则它占用的 slot 的 u64PTS 永远非 0，
				 * 下面“等待三帧全部释放”的循环会一直空转，camera_mpp_mipi_loop 线程无法退出，
				 * qt_camera_stop_frame 里的 pthread_join 就会挂死，程序无法自动退出。
				 */
				RK_MPI_VPSS_ReleaseChnFrame(MIPI_GRP, VPSS_CHN0, &mipi_stViFrame[index]);
				mipi_stViFrame[index].stVFrame.u64PTS = 0;
			}
		}
		else {
			errCount++;
			r_printf("mipi RK_MPI_VPSS_GetChnFrame timeout %x ,err %d,cur pts %lu %lu %lu", s32Ret,errCount,mipi_stViFrame[0].stVFrame.u64PTS, mipi_stViFrame[1].stVFrame.u64PTS, mipi_stViFrame[2].stVFrame.u64PTS);
			if (errCount >= 25) { 	// 200*25 = 5s
				errCount = 0;
				camera_update_qt_sta(ERR_MIPI_LOOP_TIMEOUT);
			}
		}
	}

	while(1) {
		if ((!mipi_stViFrame[0].stVFrame.u64PTS) && (!mipi_stViFrame[1].stVFrame.u64PTS) && (!mipi_stViFrame[2].stVFrame.u64PTS)){
			break;
		}
		usleep(1000);
	}

	g_printf("%s end",__func__);
	return NULL;
}
void camera_mpp_mipi_release(uint64_t pts)
{
	RK_S32 s32Ret;

	// g_printf("mipi release pts: %lu PTS %lu %lu %lu \n",pts,mipi_stViFrame[0].stVFrame.u64PTS, mipi_stViFrame[1].stVFrame.u64PTS, mipi_stViFrame[2].stVFrame.u64PTS);
	if (pts == mipi_stViFrame[0].stVFrame.u64PTS) {
		s32Ret = RK_MPI_VPSS_ReleaseChnFrame(MIPI_GRP, VPSS_CHN0, &mipi_stViFrame[0]);
		if (s32Ret < 0) {
			RK_LOGE("RK_MPI_VO_ReleaseWbcFrame failed, ret=%x", s32Ret);
		}
		mipi_stViFrame[0].stVFrame.u64PTS = 0;
	}
	else if (pts == mipi_stViFrame[1].stVFrame.u64PTS){
		s32Ret = RK_MPI_VPSS_ReleaseChnFrame(MIPI_GRP, VPSS_CHN0, &mipi_stViFrame[1]);
		if (s32Ret < 0) {
			RK_LOGE("RK_MPI_VO_ReleaseWbcFrame failed, ret=%x", s32Ret);
		}
		mipi_stViFrame[1].stVFrame.u64PTS = 0;
	}
	else if (pts == mipi_stViFrame[2].stVFrame.u64PTS){
		s32Ret = RK_MPI_VPSS_ReleaseChnFrame(MIPI_GRP, VPSS_CHN0, &mipi_stViFrame[2]);
		if (s32Ret < 0) {
			RK_LOGE("RK_MPI_VO_ReleaseWbcFrame failed, ret=%x", s32Ret);
		}
		mipi_stViFrame[2].stVFrame.u64PTS = 0;
	}
	else {
		r_printf("mipi err release pts: %lu PTS %lu %lu %lu",pts,mipi_stViFrame[0].stVFrame.u64PTS, mipi_stViFrame[1].stVFrame.u64PTS, mipi_stViFrame[2].stVFrame.u64PTS);
	}

}
int camera_mpp_mipi_set_crop(int x, int y, int w, int h)
{
	RK_S32 s32Ret;
	VPSS_CROP_INFO_S stCropInfo;
	/* 宽高为 0 时 SetChnAttr 会把预览通道收成 0，可见光画面消失。 */
	if(w < 16 || h < 16){
		RK_LOGE("mipi crop ignore invalid %d,%d %dx%d", x, y, w, h);
		return RK_FAILURE;
	}
	stCropInfo.bEnable = 1;
	VPSS_CHN_ATTR_S stVpssChnAttr;

	//stCropInfo.enCropCoordinate = VPSS_CROP_RATIO_COOR;
	stCropInfo.enCropCoordinate = VPSS_CROP_ABS_COOR;
	stCropInfo.stCropRect.s32X = x;
	stCropInfo.stCropRect.s32Y = y;
	stCropInfo.stCropRect.u32Width = w;
	stCropInfo.stCropRect.u32Height = h;
	s32Ret = RK_MPI_VPSS_SetGrpCrop(MIPI_GRP, &stCropInfo);
	if (s32Ret < 0) {
		RK_LOGE("RK_MPI_VPSS_SetGrpCrop failed, ret=%x", s32Ret);
	}

	// s32Ret = RK_MPI_VPSS_SetChnCrop(vContext->s32Grp, VPSS_CHN0, &stCropInfo);
	// if (s32Ret < 0) {
	// 	RK_LOGE("RK_MPI_VPSS_SetChnCrop failed, ret=%x", s32Ret);
	// }

	memset(&stVpssChnAttr, 0x0 ,sizeof(VPSS_CHN_ATTR_S));
	s32Ret = RK_MPI_VPSS_GetChnAttr(MIPI_GRP, VPSS_CHN0, &stVpssChnAttr);
	if (s32Ret < 0) {
		RK_LOGE("RK_MPI_VPSS_GetChnAttr failed, ret=%x", s32Ret);
	}
	stVpssChnAttr.u32Width = w;
	stVpssChnAttr.u32Height = h;
	s32Ret = RK_MPI_VPSS_SetChnAttr(MIPI_GRP, VPSS_CHN0, &stVpssChnAttr);
	if (s32Ret < 0) {
		RK_LOGE("RK_MPI_VPSS_SetChnAttr failed, ret=%x", s32Ret);
	}

	return s32Ret;
}
