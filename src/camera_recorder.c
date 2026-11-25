#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include "rk_mpi_vpss.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_ai.h"
#include "rk_mpi_aenc.h"
#include "camera_main.h"
#include "rga.h"
#include "rga_rotate_cache.h"
#include "mp4_muxer_ffmpeg.h"

//#define DVP_YUV_SAVE_FILE_DEBUG	1

/* ============================================================
 * 帧队列：把 MP4_Muxer_WriteVideo 从取帧线程剥离到独立线程，
 * 让 GetChnFrame→ReleaseChnFrame 尽量快，避免 SD 写阻塞导致 VPSS 丢帧。
 * ============================================================ */
#define VENC_FRAME_QUEUE_SIZE 64

typedef struct {
	unsigned char  data[512 * 1024];  /* 最大 512KB 一帧，H264 足够 */
	unsigned int   len;
	RK_U64         ts;
} venc_frame_entry_t;

typedef struct {
	venc_frame_entry_t buf[VENC_FRAME_QUEUE_SIZE];
	int                head;   /* 写端（取帧线程） */
	int                tail;   /* 读端（mux 线程） */
	int                count;
	pthread_mutex_t    mutex;
	pthread_cond_t     cond;
	volatile int       exit_flag;
} venc_frame_queue_t;

static void frame_queue_init(venc_frame_queue_t *q)
{
	q->head = q->tail = q->count = 0;
	q->exit_flag = 0;
	pthread_mutex_init(&q->mutex, NULL);
	pthread_cond_init(&q->cond, NULL);
}

static void frame_queue_destroy(venc_frame_queue_t *q)
{
	pthread_mutex_destroy(&q->mutex);
	pthread_cond_destroy(&q->cond);
}

/* 取帧线程调用：把编码后的帧推入队列，立即返回 */
static int frame_queue_push(venc_frame_queue_t *q, const unsigned char *data,
			    unsigned int len, RK_U64 ts)
{
	pthread_mutex_lock(&q->mutex);
	if (q->count >= VENC_FRAME_QUEUE_SIZE) {
		/* 队列满，丢弃最旧一帧（保实时） */
		q->tail = (q->tail + 1) % VENC_FRAME_QUEUE_SIZE;
		q->count--;
	}
	venc_frame_entry_t *e = &q->buf[q->head];
	if (len > sizeof(e->data)) len = sizeof(e->data);
	memcpy(e->data, data, len);
	e->len = len;
	e->ts  = ts;
	q->head = (q->head + 1) % VENC_FRAME_QUEUE_SIZE;
	q->count++;
	pthread_cond_signal(&q->cond);
	pthread_mutex_unlock(&q->mutex);
	return 0;
}

/* mux 线程调用：从队列取帧写 SD */
static int frame_queue_pop(venc_frame_queue_t *q, venc_frame_entry_t *out)
{
	pthread_mutex_lock(&q->mutex);
	while (q->count == 0 && !q->exit_flag) {
		pthread_cond_wait(&q->cond, &q->mutex);
	}
	if (q->count == 0 && q->exit_flag) {
		pthread_mutex_unlock(&q->mutex);
		return -1;
	}
	venc_frame_entry_t *e = &q->buf[q->tail];
	memcpy(out, e, sizeof(*out));
	q->tail = (q->tail + 1) % VENC_FRAME_QUEUE_SIZE;
	q->count--;
	pthread_mutex_unlock(&q->mutex);
	return 0;
}

static void frame_queue_signal_exit(venc_frame_queue_t *q)
{
	q->exit_flag = 1;
	pthread_cond_signal(&q->cond);
}

static void *camera_recorder_aenc_mp4_start(void *p);
static void *camera_recorder_global_aenc_start(void *p);
/**********************
  * MP4
**********************/
static volatile int recorder_exit_flag = 0;
struct record_context{
	int mode;
	struct cam_context *msg;
};
#define SAVE_RAW_FRAME_NUM 5
#define WAITTIME 33 // 1/fps*1000

static MUX_INFO mux_dvp;
static MUX_INFO mux_mipi;
static MUX_INFO mux_drm;
static pthread_t recorder0 = 0,recorder1 = 0,recorder2 = 0;
static pthread_t paenc_global = 0;

/* 独立 mux 线程 + 帧队列（DVP/MIPI 用，DRM 已有自己的节流逻辑暂不改） */
static pthread_t mux_thread_dvp = 0, mux_thread_mipi = 0, mux_thread_drm = 0;
static venc_frame_queue_t frame_q_dvp, frame_q_mipi, frame_q_drm;
static MUX_INFO *g_audio_mux_list[3] = {NULL, NULL, NULL};  // 全局音频写入列表
static int g_audio_mux_count = 0;
static volatile int g_audio_ready = 0;  // 音频线程就绪标志

#define AENC_LOG_ENABLE
#ifdef AENC_LOG_ENABLE
#define ro_log(x, ...)  printf("[ROCKIT_AI2AENC] " x "\n",## __VA_ARGS__)
#endif

static int camera_recorder_vpss_init(VencContext *eContext)
{
	// CHN1 is pre-created and enabled in mpp_create_vi before StartGrp (rk3576 requirement).
	// No need to SetChnAttr/EnableChn again here.
	RK_LOGE("VPSS CHN1 already enabled for grp %d, skip re-init", eContext->s32Grp);
	return 0;
}

static int camera_recorder_init(struct cam_context *msg, char *card_name, int mode)
{
	RK_S32 s32Ret = 0;

	VencContext *eContext = (VencContext *)&msg->camVeContext;
	struct Mpp_venc_api e_api = msg->cam_api;
#if 0
	//ai aenc
	AUDIO_DEV aiDevId = 0;
	AI_CHN AiChn = 0;
    AIO_ATTR_S aiAttr;
    memset(&aiAttr, 0, sizeof(AIO_ATTR_S));

	if(card_name[0] == 0x0){
		snprintf((char *)aiAttr.u8CardName,sizeof(aiAttr.u8CardName), "%s", "hw:0,0");
	}else{
		snprintf((char *)aiAttr.u8CardName,sizeof(aiAttr.u8CardName), "%s%s%s", "bluealsa:DEV=",card_name,",PROFILE=sco");
	}
    aiAttr.soundCard.channels = 2;
    aiAttr.soundCard.sampleRate = 48000;
    aiAttr.soundCard.bitWidth = AUDIO_BIT_WIDTH_16;
    aiAttr.enBitwidth = AUDIO_BIT_WIDTH_16;
    aiAttr.enSamplerate = AUDIO_SAMPLE_RATE_16000;
    aiAttr.enSoundmode = AUDIO_SOUND_MODE_STEREO;

    aiAttr.u32FrmNum = 4;
    aiAttr.u32PtNumPerFrm = AUDIO_LEN;
    //aiAttr.u32PtNumPerFrm = 1024;
    aiAttr.u32EXFlag = 0;
    aiAttr.u32ChnCnt = 2;

    RK_S32 result;
    result = RK_MPI_AI_SetPubAttr(aiDevId, &aiAttr);
    result = RK_MPI_AI_Enable(aiDevId);

	result =  RK_MPI_AI_EnableChn(aiDevId, AiChn);
    result = RK_MPI_AI_EnableReSmp(aiDevId, AiChn,AUDIO_SAMPLE_RATE_16000);

	//aenc init
    AENC_CHN_ATTR_S stAencAttr;
    AENC_CHN AencChn = 0;
    memset(&stAencAttr, 0, sizeof(AENC_CHN_ATTR_S));
    stAencAttr.enType = RK_AUDIO_ID_PCM_ALAW;//RK_AUDIO_ID_MP2;
    stAencAttr.stCodecAttr.u32Channels = 2;
    stAencAttr.stCodecAttr.u32SampleRate = 16000;
    stAencAttr.stCodecAttr.enBitwidth = AUDIO_BIT_WIDTH_16;
    stAencAttr.u32BufCount = 4;

    result = RK_MPI_AENC_CreateChn(AencChn, &stAencAttr);

	//bind
    MPP_CHN_S stSrcChn, stDstChn; 
	stSrcChn.enModId = RK_ID_AI;
	stSrcChn.s32DevId = aiDevId;
    stSrcChn.s32ChnId = AiChn;

    stDstChn.enModId = RK_ID_AENC;
    stDstChn.s32DevId = aiDevId;
    stDstChn.s32ChnId = AencChn;

    result = RK_MPI_SYS_Bind(&stSrcChn, &stDstChn);
	ro_log("%s %d ret %x",__func__,__LINE__,result);
#endif
	switch(mode){
		case CAM_IR_RECORD:
			e_api.ve_init(&eContext[0], VENC_DVP_H264);
			camera_recorder_vpss_init(&eContext[0]);
			e_api.create_ve(&eContext[0]);
			e_api.start_ve(&eContext[0]);
			break;
		case CAM_MIPI_RECORD:
			e_api.ve_init(&eContext[1], VENC_MIPI_H264);
			camera_recorder_vpss_init(&eContext[1]);
            e_api.create_ve(&eContext[1]);
            e_api.start_ve(&eContext[1]);
			break;
    	case CAM_SCREEN_RECORD:
			e_api.ve_init(&eContext[2], VENC_DRM_H264_ROT);
			e_api.create_ve(&eContext[2]);
			e_api.start_ve(&eContext[2]);
			break;
		case CAM_IR_MIPI_RECORD:
			e_api.ve_init(&eContext[0], VENC_DVP_H264);
			e_api.ve_init(&eContext[1], VENC_MIPI_H264);
			for(int i = 0; i<2; i++){
				camera_recorder_vpss_init(&eContext[i]);
				e_api.create_ve(&eContext[i]);
				e_api.start_ve(&eContext[i]);
			}
			break;
		case CAM_IR_SCREEN_RECORD:
			e_api.ve_init(&eContext[0], VENC_DVP_H264);
			e_api.ve_init(&eContext[2], VENC_DRM_H264_ROT);
            camera_recorder_vpss_init(&eContext[0]);
            e_api.create_ve(&eContext[0]);
            e_api.start_ve(&eContext[0]);
            camera_recorder_vpss_init(&eContext[2]);
            e_api.create_ve(&eContext[2]);
            e_api.start_ve(&eContext[2]);
			break;
		case CAM_MIPI_SCREEN_RECORD:
			e_api.ve_init(&eContext[1], VENC_MIPI_H264);
			e_api.ve_init(&eContext[2], VENC_DRM_H264_ROT);
            camera_recorder_vpss_init(&eContext[1]);
            e_api.create_ve(&eContext[1]);
            e_api.start_ve(&eContext[1]);
            camera_recorder_vpss_init(&eContext[2]);
            e_api.create_ve(&eContext[2]);
            e_api.start_ve(&eContext[2]);
			break;
		case CAM_ALL_RECORD:
			e_api.ve_init(&eContext[0], VENC_DVP_H264);
			e_api.ve_init(&eContext[1], VENC_MIPI_H264);
			e_api.ve_init(&eContext[2], VENC_DRM_H264_ROT);
			for(int i = 0; i < 3; i++){
				camera_recorder_vpss_init(&eContext[i]);
				e_api.create_ve(&eContext[i]);
				e_api.start_ve(&eContext[i]);
			}
			break;
	}

    return s32Ret;
}

/* DVP mux 线程：从帧队列取帧写 SD，不阻塞取帧线程 */
static void *camera_recorder_dvp_mux_thread(void *p)
{
	(void)p;
	g_printf("%s start", __func__);
	venc_frame_entry_t frame;
	while (frame_queue_pop(&frame_q_dvp, &frame) == 0) {
		MP4_Muxer_WriteVideo(&mux_dvp, frame.data, frame.len, frame.ts);
	}
	g_printf("%s end", __func__);
	return NULL;
}

static void *camera_recorder_dvp_mp4_start(void *p)
{
	RK_S32 s32Ret = 0;
	g_printf("%s strat",__func__);

	struct record_context *pt = (struct record_context *)p;
	struct cam_context *msg = pt->msg;
	int mode = pt->mode;
	VencContext *eContext = (VencContext *)&msg->camVeContext[0];

	// if(mode != 2){
		mux_dvp.w = eContext->width;
		mux_dvp.h = eContext->height;
		mux_dvp.fps = 25;
		MP4_Muxer_Create(&mux_dvp, emsg.record_path[0]);
	// }

	/* 启动独立 mux 线程 */
	frame_queue_init(&frame_q_dvp);
	pthread_create(&mux_thread_dvp, NULL, camera_recorder_dvp_mux_thread, NULL);

    VENC_STREAM_S stFrame;
	stFrame.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));

    VIDEO_FRAME_INFO_S stViFrame;
	memset(&stViFrame, 0x0 ,sizeof(VIDEO_FRAME_INFO_S));
	int rawframe_num = 0;
	int save_raw_cnt = 0;
	if(emsg.raw_cnt_num == 0)emsg.raw_cnt_num = 15;

	RK_U64 startUs = TEST_COMM_GetNowUs();
	RK_U64 frame_num = 0;
	/* 丢帧计数：定位掉帧是否发生在 VPSS 取帧环节 */
	RK_U64 vpss_get_fail = 0;
	RK_U64 venc_send_fail = 0;
	RK_U64 venc_get_fail = 0;
	/* 时间戳调试：追踪 VENC PTS 与软件时钟的偏移 */
	RK_U64 first_venc_pts = 0;
	RK_U64 first_sw_ts = 0;
	RK_U64 max_ts_diff = 0;
	RK_U64 min_ts_diff = 0xFFFFFFFFFFFFFFFFULL;
	RK_U64 last_ts_diff = 0;
	RK_U64 ts_diff_samples = 0;

	g_printf("[DVP-REC] START file=%s, shared_base_ts=%lu, sw_start=%lu", 
		emsg.record_path[0], mux_dvp.shared_base_ts, startUs);

	while (1)
	{
		//统一退出条件：与MIPI/DRM一致，确保三路视频时长相同
		if(recorder_exit_flag >= 1){
			break;
		}
        s32Ret = RK_MPI_VPSS_GetChnFrame(eContext->s32Grp, VPSS_CHN1, &stViFrame, WAITTIME);
        if (s32Ret == RK_SUCCESS) {
            void *data = RK_MPI_MB_Handle2VirAddr(stViFrame.stVFrame.pMbBlk);
            (void)data;
            RK_MPI_SYS_MmzFlushCache(stViFrame.stVFrame.pMbBlk, RK_TRUE);
            s32Ret = RK_MPI_VENC_SendFrame(eContext->veChn, &stViFrame, WAITTIME);
            if (s32Ret == RK_SUCCESS) {
                s32Ret = RK_MPI_VENC_GetStream(eContext->veChn, &stFrame, WAITTIME);
                if (s32Ret == RK_SUCCESS) {
                    void *pData = RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
                    RK_U64 venc_pts = stFrame.pstPack->u64PTS;
                    RK_U64 sw_ts = TEST_COMM_GetNowUs();
                    RK_U64 dvp_ts = sw_ts;  /* 统一使用 CLOCK_MONOTONIC，与 FFmpeg 标准流程一致 */
                    
                    /* 记录首帧时间戳偏移 */
                    if (frame_num == 0) {
                        first_venc_pts = venc_pts;
                        first_sw_ts = sw_ts;
                        g_printf("[DVP-REC] first frame: venc_pts=%lu, sw_ts=%lu, diff=%ld",
                            venc_pts, sw_ts, (int64_t)venc_pts - (int64_t)sw_ts);
                    }
                    
                    /* 定期采样时间戳差值 */
                    if (venc_pts && (frame_num % 50 == 0)) {
                        int64_t diff = (int64_t)venc_pts - (int64_t)sw_ts;
                        last_ts_diff = diff < 0 ? -diff : diff;
                        if (last_ts_diff > max_ts_diff) max_ts_diff = last_ts_diff;
                        if (last_ts_diff < min_ts_diff) min_ts_diff = last_ts_diff;
                        ts_diff_samples++;
                        if (ts_diff_samples % 10 == 0) {
                            g_printf("[DVP-REC] frame=%lu, venc_pts=%lu, sw_ts=%lu, diff=%ld, min_diff=%lu, max_diff=%lu",
                                frame_num, venc_pts, sw_ts, (int64_t)venc_pts - (int64_t)sw_ts,
                                min_ts_diff, max_ts_diff);
                        }
                    }
                    
                    /* 推入帧队列，立即返回，不再同步写 SD */
                    frame_queue_push(&frame_q_dvp, (unsigned char *)pData,
                                     (unsigned int)stFrame.pstPack->u32Len, dvp_ts);
                    RK_MPI_VENC_ReleaseStream(eContext->veChn, &stFrame);
                } else {
                    venc_get_fail++;
                }
            } else {
                venc_send_fail++;
            }
            frame_num++;
            RK_MPI_VPSS_ReleaseChnFrame(eContext->s32Grp, VPSS_CHN1, &stViFrame);
        } else {
            vpss_get_fail++;
        }
        //取帧失败时stViFrame.pMbBlk为NULL，不能release，否则rockit刷屏"mb (nil) fail to release"
    }
	RK_U64 endUs = (TEST_COMM_GetNowUs() - startUs)/1000;
	g_printf("[DVP-REC] END file=%s, frames=%lu, time=%lu ms (%.1f s), expect %d frames (25 fps), actual avr fps %.1f",
		emsg.record_path[0], frame_num, endUs, endUs/1000.0,
		(int)(endUs/1000.0 * 25),
		(double)frame_num * 1000.0 / endUs);
	g_printf("[DVP-REC] drop counter: vpss_get_fail=%lu, venc_send_fail=%lu, venc_get_fail=%lu",
		vpss_get_fail, venc_send_fail, venc_get_fail);
	g_printf("[DVP-REC] ts debug: first_venc=%lu, first_sw=%lu, diff=%ld, samples=%lu, min_diff=%lu, max_diff=%lu, last_diff=%lu",
		first_venc_pts, first_sw_ts, (int64_t)first_venc_pts - (int64_t)first_sw_ts,
		ts_diff_samples, min_ts_diff, max_ts_diff, last_ts_diff);
	free(stFrame.pstPack);

	/* 通知 mux 线程退出并等待 */
	frame_queue_signal_exit(&frame_q_dvp);
	if (mux_thread_dvp != 0) {
		pthread_join(mux_thread_dvp, NULL);
		mux_thread_dvp = 0;
	}
	frame_queue_destroy(&frame_q_dvp);

	/* 不再在此处 Close，由主函数统一在音频线程退出后关闭，避免 use-after-free */
	g_printf("%s end",__func__);
    return NULL;
}

/* MIPI mux 线程：从帧队列取帧写 SD，不阻塞取帧线程 */
static void *camera_recorder_mipi_mux_thread(void *p)
{
	(void)p;
	g_printf("%s start", __func__);
	venc_frame_entry_t frame;
	while (frame_queue_pop(&frame_q_mipi, &frame) == 0) {
		MP4_Muxer_WriteVideo(&mux_mipi, frame.data, frame.len, frame.ts);
	}
	g_printf("%s end", __func__);
	return NULL;
}

static void *camera_recorder_mipi_mp4_start(void *p)
{
	RK_S32 s32Ret = 0;
	g_printf("%s strat",__func__);
	VencContext *eContext = (VencContext *)p;

	mux_mipi.w = eContext->width;
	mux_mipi.h = eContext->height;
	mux_mipi.fps = 25;
	MP4_Muxer_Create(&mux_mipi, emsg.record_path[2]);

	/* 启动独立 mux 线程 */
	frame_queue_init(&frame_q_mipi);
	pthread_create(&mux_thread_mipi, NULL, camera_recorder_mipi_mux_thread, NULL);

    VENC_STREAM_S stFrame;
	stFrame.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));

	VIDEO_FRAME_INFO_S stViFrame;
	memset(&stViFrame, 0x0, sizeof(VIDEO_FRAME_INFO_S));

	RK_U64 startUs = TEST_COMM_GetNowUs();
	RK_U64 frame_num = 0;
	/* 丢帧计数：定位掉帧是否发生在 VPSS 取帧环节 */
	RK_U64 vpss_get_fail = 0;
	RK_U64 venc_send_fail = 0;
	RK_U64 venc_get_fail = 0;
	/* 时间戳调试：追踪 VENC PTS 与软件时钟的偏移 */
	RK_U64 first_venc_pts = 0;
	RK_U64 first_sw_ts = 0;
	RK_U64 max_ts_diff = 0;
	RK_U64 min_ts_diff = 0xFFFFFFFFFFFFFFFFULL;
	RK_U64 last_ts_diff = 0;
	RK_U64 ts_diff_samples = 0;

	g_printf("[MIPI-REC] START file=%s, shared_base_ts=%lu, sw_start=%lu", 
		emsg.record_path[2], mux_mipi.shared_base_ts, startUs);

	while (1) 
	{
		if(recorder_exit_flag){
			break;
		}

		s32Ret = RK_MPI_VPSS_GetChnFrame(eContext->s32Grp, VPSS_CHN1, &stViFrame, WAITTIME);
		if (s32Ret == RK_SUCCESS) {
			RK_MPI_SYS_MmzFlushCache(stViFrame.stVFrame.pMbBlk, RK_TRUE);
			s32Ret = RK_MPI_VENC_SendFrame(eContext->veChn, &stViFrame, WAITTIME);
			if (s32Ret == RK_SUCCESS) {
				s32Ret = RK_MPI_VENC_GetStream(eContext->veChn, &stFrame, WAITTIME);
				if (s32Ret == RK_SUCCESS) {
					void *pData = RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
					RK_U64 venc_pts = stFrame.pstPack->u64PTS;
					RK_U64 sw_ts = TEST_COMM_GetNowUs();
					RK_U64 mipi_ts = sw_ts;  /* 统一使用 CLOCK_MONOTONIC，与 FFmpeg 标准流程一致 */
					
					/* 记录首帧时间戳偏移 */
					if (frame_num == 0) {
						first_venc_pts = venc_pts;
						first_sw_ts = sw_ts;
						g_printf("[MIPI-REC] first frame: venc_pts=%lu, sw_ts=%lu, diff=%ld",
							venc_pts, sw_ts, (int64_t)venc_pts - (int64_t)sw_ts);
					}
					
					/* 定期采样时间戳差值 */
					if (venc_pts && (frame_num % 50 == 0)) {
						int64_t diff = (int64_t)venc_pts - (int64_t)sw_ts;
						last_ts_diff = diff < 0 ? -diff : diff;
						if (last_ts_diff > max_ts_diff) max_ts_diff = last_ts_diff;
						if (last_ts_diff < min_ts_diff) min_ts_diff = last_ts_diff;
						ts_diff_samples++;
						if (ts_diff_samples % 10 == 0) {
							g_printf("[MIPI-REC] frame=%lu, venc_pts=%lu, sw_ts=%lu, diff=%ld, min_diff=%lu, max_diff=%lu",
								frame_num, venc_pts, sw_ts, (int64_t)venc_pts - (int64_t)sw_ts,
								min_ts_diff, max_ts_diff);
						}
					}
					
					/* 推入帧队列，立即返回，不再同步写 SD */
					frame_queue_push(&frame_q_mipi, (unsigned char *)pData,
					                 (unsigned int)stFrame.pstPack->u32Len, mipi_ts);
					RK_MPI_VENC_ReleaseStream(eContext->veChn, &stFrame);
				} else {
					venc_get_fail++;
				}
			} else {
				venc_send_fail++;
			}
			frame_num++;
			RK_MPI_VPSS_ReleaseChnFrame(eContext->s32Grp, VPSS_CHN1, &stViFrame);
		} else {
			vpss_get_fail++;
		}
	}
	RK_U64 endUs = (TEST_COMM_GetNowUs() - startUs)/1000;
	g_printf("[MIPI-REC] END file=%s, frames=%lu, time=%lu ms (%.1f s), expect %d frames (25 fps), actual avr fps %.1f",
		emsg.record_path[2], frame_num, endUs, endUs/1000.0,
		(int)(endUs/1000.0 * 25),
		(double)frame_num * 1000.0 / endUs);
	g_printf("[MIPI-REC] drop counter: vpss_get_fail=%lu, venc_send_fail=%lu, venc_get_fail=%lu",
		vpss_get_fail, venc_send_fail, venc_get_fail);
	g_printf("[MIPI-REC] ts debug: first_venc=%lu, first_sw=%lu, diff=%ld, samples=%lu, min_diff=%lu, max_diff=%lu, last_diff=%lu",
		first_venc_pts, first_sw_ts, (int64_t)first_venc_pts - (int64_t)first_sw_ts,
		ts_diff_samples, min_ts_diff, max_ts_diff, last_ts_diff);
	free(stFrame.pstPack);

	/* 通知 mux 线程退出并等待 */
	frame_queue_signal_exit(&frame_q_mipi);
	if (mux_thread_mipi != 0) {
		pthread_join(mux_thread_mipi, NULL);
		mux_thread_mipi = 0;
	}
	frame_queue_destroy(&frame_q_mipi);

	/* 不再在此处 Close，由主函数统一在音频线程退出后关闭，避免 use-after-free */
	g_printf("%s end",__func__);
    return NULL;
}

static void *camera_recorder_aenc_mp4_start(void *p)
{
#if 0
	AUDIO_STREAM_S pstStream;
	MUX_AUDIO_STREAM_S out_stStream;
    memset(&out_stStream, 0, sizeof(MUX_AUDIO_STREAM_S));
	AUDIO_DEV aiDevId = 0;
	AI_CHN AiChn = 0;
    AENC_CHN AencChn = 0;
	MUX_INFO *mux = (MUX_INFO *)p;

    RK_S32 result;
	g_printf("%s strat",__func__);
	while(1){

		if(recorder_exit_flag){
			break;
		}
		result = RK_MPI_AENC_GetStream(AencChn, &pstStream, WAITTIME);
		if (result == RK_SUCCESS) {
			MB_BLK bBlk = pstStream.pMbBlk;

			out_stStream.pStream = RK_MPI_MB_Handle2VirAddr(bBlk);
			out_stStream.u32Len = pstStream.u32Len;
			out_stStream.u32Seq = pstStream.u32Seq;
			out_stStream.u64TimeStamp = pstStream.u64TimeStamp;
			MP4_Muxer_WriteAudio(mux, &out_stStream);
			if (out_stStream.pStream) {
				RK_MPI_AENC_ReleaseStream(AencChn, &pstStream);
			}
		}
	}
	//deinit aenc
	RK_MPI_AENC_DestroyChn(AencChn);

	//deinit ai
    RK_MPI_AI_DisableReSmp(aiDevId, AiChn);
    result = RK_MPI_AI_DisableChn(aiDevId, AiChn);
	ro_log("%s %d ret %x",__func__,__LINE__,result);
    result = RK_MPI_AI_Disable(aiDevId);
	ro_log("%s %d ret %x",__func__,__LINE__,result);
#else

#endif
    mp3_encoder(&recorder_exit_flag, p);
	g_printf("%s end",__func__);
	return NULL;
}

/* 定位用：枚举 /proc/asound 下所有 pcm 流的 substatus（真实目录名形如 pcmC0D0p/pcmC0D0c，
 * 不是 pcm0p）。录像启动时若播放流(CARD0/D0p)仍被 Qt 播放器占用、或录音流(CARD0/D0c)
 * 的 rate 显示 48000 而非配置的 16000，即可坐实时钟被播放侧污染。 */
static void ai_dump_substatus_file(const char *path)
{
	FILE *fp = fopen(path, "r");
	if (!fp) return;
	char line[256];
	int n = 0;
	while (fgets(line, sizeof(line), fp) && n < 14) {
		int l = strlen(line);
		while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = 0;
		if (l > 0) g_printf("[ALSA-DUMP] %s| %s", strrchr(path,'/') ? path+1 : path, line);
		n++;
	}
	fclose(fp);
}

static void ai_dump_alsa_state(const char *tag)
{
	g_printf("[ALSA-DUMP] ===== %s =====", tag);
	ai_dump_substatus_file("/proc/asound/cards");

	DIR *d = opendir("/proc/asound");
	if (!d) { g_printf("[ALSA-DUMP] /proc/asound open failed"); return; }
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (strncmp(de->d_name, "card", 4) != 0) continue;
		char cardpath[128];
		snprintf(cardpath, sizeof(cardpath), "/proc/asound/%s", de->d_name);
		DIR *cd = opendir(cardpath);
		if (!cd) continue;
		struct dirent *pe;
		while ((pe = readdir(cd)) != NULL) {
			if (strncmp(pe->d_name, "pcm", 3) != 0) continue;
			char ss[200];
			snprintf(ss, sizeof(ss), "%s/%s/substatus", cardpath, pe->d_name);
			ai_dump_substatus_file(ss);
		}
		closedir(cd);
	}
	closedir(d);
}

static void *camera_recorder_global_aenc_start(void *p)
{
	(void)p;
	g_printf("%s start, mux_count=%d", __func__, g_audio_mux_count);
	
	if(g_audio_mux_count == 0){
		g_printf("%s no mux need audio, exit", __func__);
		return NULL;
	}

	// 调用 mp3_encoder 的初始化部分（复用 mp3_encoder 里的 open_device_ai + init_mpi_aenc）
	// 这里直接调用 mp3_encoder，但让它只初始化一次，然后我们在外面读 AENC 数据
	// 最简单的方式：让第一个 mux 调用 mp3_encoder，但 mp3_encoder 内部会独占 AI
	// 所以我们需要自己实现：初始化 AI+AENC，然后循环读数据写入所有 mux
	
	RK_S32 u32SampleRate = 48000;
	RK_S32 u32Channel = 2;
	/* 改用 48k 采集：Qt 播放会把共享 codec 时钟带到 48k，播完立刻录像时 AI 按 16k 配置
	 * 却拿到 48k 时钟的数据，音频轨膨胀 3 倍(录10s文件34s)。48k 与播放侧一致，
	 * 干净/被污染两种状态下硬件都按配置速率跑。
	 * 16000Hz 用 576*2*channel，32000/44100/48000 用 1152*2*channel */
	RK_U32 u32FrameCnt = 1152 * 2 * u32Channel;  // = 4608
	RK_CHAR *pCodecName = (RK_CHAR *)"mp3";
	
	RegisterAencMp3();
	
	ai_dump_alsa_state("before_ai_open");
	if(open_device_ai(u32SampleRate, u32SampleRate, u32Channel, u32FrameCnt)){
		g_printf("%s open_device_ai failed", __func__);
		return NULL;
	}
	ai_dump_alsa_state("after_ai_open");
	
	if(init_mpi_aenc(u32SampleRate, u32Channel)){
		g_printf("%s init_mpi_aenc failed", __func__);
		return NULL;
	}
	
	// bind AI -> AENC
	MPP_CHN_S stSrcChn, stDestChn;
	stSrcChn.enModId = RK_ID_AI;
	stSrcChn.s32DevId = 0;
	stSrcChn.s32ChnId = 0;
	stDestChn.enModId = RK_ID_AENC;
	stDestChn.s32DevId = 0;
	stDestChn.s32ChnId = 0;
	
	RK_S32 ret = RK_MPI_SYS_Bind(&stSrcChn, &stDestChn);
	if(ret){
		g_printf("%s Bind AI to AENC failed! ret=%d", __func__, ret);
		return NULL;
	}
	
	g_printf("%s AENC init done, start reading stream", __func__);
	
	AUDIO_STREAM_S pstStream;
	MUX_AUDIO_STREAM_S out_stStream;
	memset(&out_stStream, 0, sizeof(MUX_AUDIO_STREAM_S));

	/* 定位"音频轨时长膨胀"：周期性对比 AENC 时间戳推进量 vs CLOCK_MONOTONIC 推进量。
	 * ratio≈1.0 正常；ratio≈3.0(=48000/16000) 说明声卡/编码器实际按 48k 时钟出数据，
	 * 而 AI 按 16k 配置，时间戳与采样数都跑快 3 倍。packets/s 同理：正常约 27.7/s(16k/576)。 */
	int audio_dbg_cnt = 0;
	RK_U64 audio_dbg_first_ts = 0;
	RK_U64 audio_dbg_first_wall = 0;
	int audio_warmup_logged = 0;

	while(1){
		if(recorder_exit_flag){
			break;
		}
		
		ret = RK_MPI_AENC_GetStream(0, &pstStream, WAITTIME);
		if(ret == RK_SUCCESS){
			MB_BLK bBlk = pstStream.pMbBlk;
			RK_VOID *pstFrame = RK_MPI_MB_Handle2VirAddr(bBlk);
			RK_S32 frameSize = pstStream.u32Len;
			RK_U64 timeStamp = pstStream.u64TimeStamp;

			if(audio_dbg_cnt == 0){
				audio_dbg_first_ts = timeStamp;
				audio_dbg_first_wall = TEST_COMM_GetNowUs();
			}
			audio_dbg_cnt++;
			if(audio_dbg_cnt % 100 == 0){
				RK_U64 wall_now = TEST_COMM_GetNowUs();
				RK_U64 ts_adv = timeStamp - audio_dbg_first_ts;
				RK_U64 wall_adv = wall_now - audio_dbg_first_wall;
				double ratio = (wall_adv > 0) ? ((double)ts_adv / (double)wall_adv) : 0.0;
				double pps = (wall_adv > 0) ? ((double)(audio_dbg_cnt - 1) * 1000000.0 / (double)wall_adv) : 0.0;
				g_printf("[AENC-CLK] packets=%d ts_adv=%llu us wall_adv=%llu us ratio=%.3f packets/s=%.1f seq=%u len=%d",
					audio_dbg_cnt, (unsigned long long)ts_adv, (unsigned long long)wall_adv,
					ratio, pps, pstStream.u32Seq, frameSize);
				/* ratio 明显偏离 1 时快照一次声卡状态，看是谁把时钟带偏的 */
				if((ratio > 1.5 || ratio < 0.7) && audio_dbg_cnt == 100){
					ai_dump_alsa_state("ratio_abnormal");
				}
			}

			if(pstFrame && frameSize > 0 &&
			   (TEST_COMM_GetNowUs() - audio_dbg_first_wall) >= 500000){
				if(!audio_warmup_logged){
					audio_warmup_logged = 1;
					g_printf("[AENC-WARMUP] skipped first 500ms audio (pop on codec start after playback)");
				}
				out_stStream.pStream = pstFrame;
				out_stStream.u32Len = frameSize;
				out_stStream.u32Seq = pstStream.u32Seq;
				out_stStream.u64TimeStamp = timeStamp;
				
				// 写入所有需要录音的 mux（检查 g_OutFmt_Ctx 非 NULL，避免往未创建的 mux 写）
				for(int i = 0; i < g_audio_mux_count; i++){
					if(g_audio_mux_list[i] && g_audio_mux_list[i]->ai_enable && g_audio_mux_list[i]->g_OutFmt_Ctx){
						MP4_Muxer_WriteAudio(g_audio_mux_list[i], &out_stStream);
					}
				}
			}
			
			RK_MPI_AENC_ReleaseStream(0, &pstStream);
		}
	}
	
	g_printf("%s exit, cleaning up", __func__);
	
	// cleanup
	RK_MPI_SYS_UnBind(&stSrcChn, &stDestChn);
	RK_MPI_AI_DisableChn(0, 0);
	RK_MPI_AI_Disable(0);
	RK_MPI_AENC_DestroyChn(0);
	UnRegisterAencMp3();
	
	g_printf("%s end", __func__);
	return NULL;
}

#if 0
static void *camera_recorder_drm_mp4_start(void *p)
{
	RK_S32 s32Ret = 0;
	g_printf("%s strat",__func__);

	struct record_context *pt = (struct record_context *)p;
	struct cam_context *msg = pt->msg;
	struct Mpp_venc_api e_api = msg->cam_api;
	//int mode = pt->mode;
	VencContext *eContext = (VencContext *)&msg->camVeContext[2];
    VENC_STREAM_S stFrame;
	stFrame.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));

	//drm
	/* CAMDRMINFO drminfo; */
	/* memset(&drminfo, 0x0 , sizeof(CAMDRMINFO)); */
	/* drm_crtc_init(&drminfo); */

	//RGBA8888
	int len = eContext->width * eContext->height * 4;
	// for input image pool
	MB_POOL_CONFIG_S stMbPoolCfg;
	memset(&stMbPoolCfg, 0, sizeof(MB_POOL_CONFIG_S));
    stMbPoolCfg.u64MBSize = len;
    stMbPoolCfg.u32MBCnt  = 1;
    stMbPoolCfg.enAllocType = MB_ALLOC_TYPE_DMA;
    stMbPoolCfg.bPreAlloc = RK_TRUE;
    MB_POOL pool = RK_MPI_MB_CreatePool(&stMbPoolCfg);
	MB_BLK blk = RK_MPI_MB_GetMB(pool, len, RK_TRUE);
	RK_U8 *pVirAddr = (RK_U8 *)(RK_MPI_MB_Handle2VirAddr(blk));

	VIDEO_FRAME_INFO_S stViFrame;
	memset(&stViFrame, 0x0 ,sizeof(VIDEO_FRAME_INFO_S));
	stViFrame.stVFrame.pMbBlk = blk;
	stViFrame.stVFrame.u32Width = eContext->width;
	stViFrame.stVFrame.u32Height = eContext->height;
	stViFrame.stVFrame.u32VirWidth = eContext->width;
	stViFrame.stVFrame.u32VirHeight = eContext->height;
	stViFrame.stVFrame.enPixelFormat = (PIXEL_FORMAT_E)eContext->enPixelFormat;
	//stViFrame.stVFrame.u32FrameFlag |= s32ReachEOS ? FRAME_FLAG_SNAP_END : 0;
	stViFrame.stVFrame.enCompressMode = COMPRESS_MODE_NONE;

	mux_drm.w = eContext->width;
	mux_drm.h = eContext->height;
	mux_drm.fps = 22;
	MP4_Muxer_Create(&mux_drm, emsg.record_path[3]);

	if(mux_drm.ai_enable){
		g_printf("%s ai_enable",__func__);
		pthread_create(&paenc_drm, NULL ,camera_recorder_aenc_mp4_start, (void *)&mux_drm);
	}
	RK_U64 startUs = TEST_COMM_GetNowUs();
	RK_U64 frame_num = 0;
	RK_U64 fpsMs0,fpsMs1;
	//int fpsms = 45; // 1/22=45ms
	while (1) 
	{
		if(recorder_exit_flag){
			break;
		}
		//fpsMs0 = TEST_COMM_GetNowUs()/1000;
		system("echo dump > /sys/kernel/debug/dri/0/video_port3/vop_dump/dump");
		int fd = open("/data/dump.bin", O_RDONLY, 0644);
		int read_len = read(fd, pVirAddr, len);
		frame_num++;

		stViFrame.stVFrame.u64PTS = TEST_COMM_GetNowUs()/1000;
		RK_MPI_SYS_MmzFlushCache(blk, RK_TRUE);
		s32Ret = RK_MPI_VENC_SendFrame(eContext->veChn, &stViFrame, WAITTIME);
		//printf("RK_MPI_VENC_SendFrame %d\n",s32Ret);
		s32Ret = RK_MPI_VENC_GetStream(eContext->veChn, &stFrame, WAITTIME);
		//printf("RK_MPI_VENC_GetStream %d\n",s32Ret);
		void *pData= RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
		MP4_Muxer_WriteVideo(&mux_drm, (unsigned char *)pData, (unsigned int)stFrame.pstPack->u32Len);
		RK_MPI_VENC_ReleaseStream(eContext->veChn, &stFrame);
		close(fd);
		/* fpsMs1 = TEST_COMM_GetNowUs()/1000 - fpsMs0; */
		/* if(fpsMs1 < fpsms){ */
			/* g_printf("fpsMs1 %lu usleep %lu",fpsMs1,fpsms-fpsMs1); */
			/* usleep((fpsms-fpsMs1)*1000); */
		/* } */
		/* r_printf("deal fmt use %lu ms",TEST_COMM_GetNowUs()/1000 - fpsMs0); */
	}

	if(mux_drm.ai_enable){
		pthread_join(paenc_drm, NULL);
	}
	MP4_Muxer_Close(&mux_drm);
	free(stFrame.pstPack);
	//drm_crtc_deinit(&drminfo);
	e_api.stop_ve(eContext);
	e_api.destroy_ve(eContext);
	s32Ret = RK_MPI_MB_ReleaseMB(blk);
	printf("RK_MPI_MB_ReleaseMB %d\n",s32Ret);
	s32Ret = RK_MPI_MB_DestroyPool(pool);
	printf("RK_MPI_MB_DestroyPool %d\n",s32Ret);

	RK_U64 endMs = (TEST_COMM_GetNowUs() - startUs)/1000;
	int avr_fps = frame_num*10000/endMs;
	g_printf("drm end time frame num %lu , %lu ms , avr fps %d.%d",frame_num,endMs,avr_fps/10,avr_fps&0xf);
    return NULL;
}
#else
/* DRM mux 线程：从帧队列取帧写 SD，不阻塞取帧线程 */
static void *camera_recorder_drm_mux_thread(void *p)
{
	(void)p;
	g_printf("%s start", __func__);
	venc_frame_entry_t frame;
	while (frame_queue_pop(&frame_q_drm, &frame) == 0) {
		MP4_Muxer_WriteVideo(&mux_drm, frame.data, frame.len, frame.ts);
	}
	g_printf("%s end", __func__);
	return NULL;
}

static void *camera_recorder_drm_mp4_start(void *p)
{
	RK_S32 s32Ret = 0;
	g_printf("%s strat",__func__);

	struct record_context *pt = (struct record_context *)p;
	struct cam_context *msg = pt->msg;
	struct Mpp_venc_api e_api = msg->cam_api;
	//int mode = pt->mode;
	VencContext *eContext = (VencContext *)&msg->camVeContext[2];
    VENC_STREAM_S stFrame;
	stFrame.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));

	//drm
	CAMDRMINFO drminfo;
	memset(&drminfo, 0x0 , sizeof(CAMDRMINFO));
	drm_crtc_init(&drminfo);

	//零拷贝：不再自建 MB 缓冲，每帧把 weston 的 fb dma-buf(fd)直接用 RK_MPI_SYS_CreateMB
	//包成 MB_BLK 喂给 VENC(BGRA8888)，由 VENC 硬件做 CSC+旋转。CPU memcpy 读非缓存 fb 太慢(~52ms)。
	VIDEO_FRAME_INFO_S stViFrame;
	memset(&stViFrame, 0x0 ,sizeof(VIDEO_FRAME_INFO_S));
	stViFrame.stVFrame.u32Width = eContext->width;   //1920，真实 fb 宽
	stViFrame.stVFrame.u32Height = eContext->height; //1080，真实 fb 高
	stViFrame.stVFrame.u32VirWidth = eContext->width;
	stViFrame.stVFrame.u32VirHeight = eContext->height;
	stViFrame.stVFrame.enPixelFormat = RK_FMT_BGRA8888;//直接喂 weston 的 BGRA fb，VENC 硬件做 CSC+旋转
	//stViFrame.stVFrame.u32FrameFlag |= s32ReachEOS ? FRAME_FLAG_SNAP_END : 0;
	stViFrame.stVFrame.enCompressMode = COMPRESS_MODE_NONE;

	mux_drm.w = eContext->width;
	mux_drm.h = eContext->height;
	mux_drm.fps = 22;
	MP4_Muxer_Create(&mux_drm, emsg.record_path[3]);

	/* 启动独立 mux 线程 */
	frame_queue_init(&frame_q_drm);
	pthread_create(&mux_thread_drm, NULL, camera_recorder_drm_mux_thread, NULL);

	int mb_err_logged = 0;
	RK_U64 startUs = TEST_COMM_GetNowUs();
	RK_U64 frame_num = 0;
	/* 时间戳调试：追踪 VENC PTS 与软件时钟的偏移 */
	RK_U64 first_venc_pts = 0;
	RK_U64 first_sw_ts = 0;
	RK_U64 max_ts_diff = 0;
	RK_U64 min_ts_diff = 0xFFFFFFFFFFFFFFFFULL;
	RK_U64 last_ts_diff = 0;
	RK_U64 ts_diff_samples = 0;

	g_printf("[DRM-REC] START file=%s, shared_base_ts=%lu, sw_start=%lu", 
		emsg.record_path[3], mux_drm.shared_base_ts, startUs);
	RK_U64 fpsMs0, fpsMs1;
	RK_U64 Ms0, Ms1, Ms2, Ms3, Ms4, Ms5, Ms6, Ms7;
	RK_U64 delta;
	int fpsms = 45; // 1/22=45ms
	RK_U64 acc_wait=0, acc_upd=0, acc_rga=0, acc_flush=0, acc_venc=0, acc_mux=0, acc_loop=0, acc_cnt=0;
	while (1) 
	{
		if (recorder_exit_flag) {
			break;
		}
		fpsMs0 = TEST_COMM_GetNowUs() / 1000;
_first:
		Ms0 = TEST_COMM_GetNowUs() / 1000;
		// camera_drm_wait_vblank(&drminfo);
		//等 fb_id 变化(抓新帧)。weston 忙时 GL fence 要 31~39ms，小超时根本等不到变化、
		//每帧白白耗满超时再抓当前 fb。DRM 是原子 pageflip，当前 fb 必为完整一帧，屏幕没变化时
		//录到重复帧对录屏是正确的。实测 stage：超时 20->8 后 wait 19.2->10.8ms。再降到 4ms
		//把 proc(46.7ms)压回 45ms 节流预算内，稳住帧率。
		drmModeCrtcPtr crtc = camera_drm_wait_vblank_2_timeout(&drminfo, 4);
		Ms1 = TEST_COMM_GetNowUs() / 1000;
		delta = Ms1 - Ms0;
		if (delta > 33) {
			// r_printf("sel frm use %lu ms > 33ms", delta);
			camera_gst_kms_src_free_crtc_fb(crtc);
			//修复：重试前检查退出标志，避免vblank持续超时导致死循环无法退出
			if (recorder_exit_flag) {
				break;
			}
			goto _first;
		}
		else {
			// g_printf("sel frm use %lu ms", delta);
		}
		//weston多缓冲轮转/旋转屏幕后会切换fb，按新fb_id更新映射，否则读到的是旧画面（旋转不同步）
		drm_crtc_update_fb(&drminfo, crtc->buffer_id);
		Ms2 = TEST_COMM_GetNowUs()/1000;

        //零拷贝：把 weston fb 的 dma-buf(fd)直接包成 MB_BLK 喂给 VENC，由 VENC 硬件做
        //CSC(BGRA->YUV)+旋转。CPU memcpy 读非缓存 fb ~52ms 是帧率瓶颈，这里彻底去掉拷贝，
        //也不再需要 DMA_BUF_IOCTL_SYNC/MmzFlushCache(那是为 CPU 读写做缓存一致性，现在没有 CPU 访问)。
        //fb 归 DRM/weston 所有，pFreeCB=NULL(不接管释放)；每帧 import 一次，编码完立即 ReleaseMB。
        int rec_fbw = drminfo.fb2 ? (int)drminfo.fb2->width  : 1920;
        int rec_fbh = drminfo.fb2 ? (int)drminfo.fb2->height : 1080;
        MB_EXT_CONFIG_S extCfg;
        memset(&extCfg, 0, sizeof(extCfg));
        extCfg.s32Fd = drminfo.dmafd[0];
        extCfg.pu8VirAddr = (RK_U8 *)drminfo.vaddr;
        extCfg.u64Size = (RK_U64)rec_fbw * rec_fbh * 4;
        extCfg.pFreeCB = NULL;
        MB_BLK fbMb = NULL;
        if (RK_MPI_SYS_CreateMB(&fbMb, &extCfg) != RK_SUCCESS || !fbMb) {
            if (!mb_err_logged) { printf("CreateMB from fb fd failed\n"); mb_err_logged = 1; }
            camera_gst_kms_src_free_crtc_fb(crtc);
            continue;
        }
        stViFrame.stVFrame.pMbBlk = fbMb;
        stViFrame.stVFrame.u32Width = rec_fbw;
        stViFrame.stVFrame.u32Height = rec_fbh;
        stViFrame.stVFrame.u32VirWidth = rec_fbw;
        stViFrame.stVFrame.u32VirHeight = rec_fbh;
		Ms3 = TEST_COMM_GetNowUs()/1000;
		// g_printf("copy frm use %lu ms", Ms2 - Ms1);
		stViFrame.stVFrame.u64PTS = TEST_COMM_GetNowUs(); //PTS单位是us，之前除以1000变成ms导致时间戳错误
		camera_gst_kms_src_free_crtc_fb(crtc);
		Ms4 = TEST_COMM_GetNowUs()/1000;
		s32Ret = RK_MPI_VENC_SendFrame(eContext->veChn, &stViFrame, WAITTIME);
        if(s32Ret != RK_SUCCESS){
            RK_MPI_MB_ReleaseMB(fbMb);
            continue;  // 送帧失败，跳过这帧
        }
		//printf("RK_MPI_VENC_SendFrame %d\n",s32Ret);

		s32Ret = RK_MPI_VENC_GetStream(eContext->veChn, &stFrame, WAITTIME);
        if(s32Ret != RK_SUCCESS){
            RK_MPI_MB_ReleaseMB(fbMb);
            continue;  // 送帧失败，跳过这帧
        }
		//printf("RK_MPI_VENC_GetStream %d\n",s32Ret);
		Ms5 = TEST_COMM_GetNowUs()/1000;
		// g_printf("venc frm use %lu ms", Ms4 - Ms3);
		void *pData= RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
		RK_U64 venc_pts = stFrame.pstPack->u64PTS;
		RK_U64 sw_ts = TEST_COMM_GetNowUs();
		RK_U64 drm_ts = venc_pts ? venc_pts : sw_ts;
		
		/* 记录首帧时间戳偏移 */
		if (frame_num == 0) {
			first_venc_pts = venc_pts;
			first_sw_ts = sw_ts;
			g_printf("[DRM-REC] first frame: venc_pts=%lu, sw_ts=%lu, diff=%ld",
				venc_pts, sw_ts, (int64_t)venc_pts - (int64_t)sw_ts);
		}
		
		/* 定期采样时间戳差值 */
		if (venc_pts && (frame_num % 50 == 0)) {
			int64_t diff = (int64_t)venc_pts - (int64_t)sw_ts;
			last_ts_diff = diff < 0 ? -diff : diff;
			if (last_ts_diff > max_ts_diff) max_ts_diff = last_ts_diff;
			if (last_ts_diff < min_ts_diff) min_ts_diff = last_ts_diff;
			ts_diff_samples++;
			if (ts_diff_samples % 10 == 0) {
				g_printf("[DRM-REC] frame=%lu, venc_pts=%lu, sw_ts=%lu, diff=%ld, min_diff=%lu, max_diff=%lu",
					frame_num, venc_pts, sw_ts, (int64_t)venc_pts - (int64_t)sw_ts,
					min_ts_diff, max_ts_diff);
			}
		}
		/* 推入帧队列，立即返回，不再同步写 SD */
		frame_queue_push(&frame_q_drm, (unsigned char *)pData,
		                 (unsigned int)stFrame.pstPack->u32Len, drm_ts);
		RK_MPI_VENC_ReleaseStream(eContext->veChn, &stFrame);
		//VENC 已同步编码完这一帧(SendFrame->GetStream 阻塞返回)，输入 fb 可安全释放回 DRM/weston
		RK_MPI_MB_ReleaseMB(fbMb);
		frame_num++;
		Ms6 = TEST_COMM_GetNowUs()/1000;
		fpsMs1 = TEST_COMM_GetNowUs()/1000;
		delta = fpsMs1 - fpsMs0;
		acc_wait += Ms1 - Ms0; acc_upd += Ms2 - Ms1; acc_rga += Ms3 - Ms2;
		acc_flush += Ms4 - Ms3; acc_venc += Ms5 - Ms4; acc_mux += Ms6 - Ms5;
		acc_loop += delta; acc_cnt++;
		if (delta < fpsms){
			// g_printf("delta %lu usleep %lu", delta, fpsms - delta);
			usleep((fpsms - delta) * 1000);
		}
		else {
			// r_printf("Warming: deal fmt use %lu ms > fpsms %d", delta, fpsms);
			// r_printf("Warming: Ms1-Ms0=%d, Ms2-Ms1=%d, Ms3-Ms2=%d, Ms4-Ms3=%d, Ms5-Ms4=%d, Ms6-Ms5=%d\n",
					// Ms1 - Ms0, Ms2 - Ms1, Ms3 - Ms2, Ms4 - Ms3, Ms5 - Ms4, Ms6 - Ms5);
		}
		// r_printf("deal fmt use %lu ms",TEST_COMM_GetNowUs()/1000 - fpsMs0);
	}

	/* 通知 mux 线程退出并等待 */
	frame_queue_signal_exit(&frame_q_drm);
	if (mux_thread_drm != 0) {
		pthread_join(mux_thread_drm, NULL);
		mux_thread_drm = 0;
	}
	frame_queue_destroy(&frame_q_drm);

	MP4_Muxer_Close(&mux_drm);
	free(stFrame.pstPack);
	drm_crtc_deinit(&drminfo);
	e_api.stop_ve(eContext);
	e_api.destroy_ve(eContext);

	RK_U64 endMs = (TEST_COMM_GetNowUs() - startUs)/1000;
	g_printf("[DRM-REC] END file=%s, frames=%lu, time=%lu ms (%.1f s), expect %d frames (22 fps), actual avr fps %.1f",
		emsg.record_path[3], frame_num, endMs, endMs/1000.0,
		(int)(endMs/1000.0 * 22),
		(double)frame_num * 1000.0 / endMs);
	g_printf("[DRM-REC] ts debug: first_venc=%lu, first_sw=%lu, diff=%ld, samples=%lu, min_diff=%lu, max_diff=%lu, last_diff=%lu",
		first_venc_pts, first_sw_ts, (int64_t)first_venc_pts - (int64_t)first_sw_ts,
		ts_diff_samples, min_ts_diff, max_ts_diff, last_ts_diff);
	if (acc_cnt) {
		g_printf("drm stage avg(ms) over %lu frames: wait %.1f, updatefb %.1f, import %.1f, flush %.1f, venc %.1f, proc_total %.1f",
			acc_cnt,
			(double)acc_wait/acc_cnt, (double)acc_upd/acc_cnt, (double)acc_rga/acc_cnt,
			(double)acc_flush/acc_cnt, (double)acc_venc/acc_cnt,
			(double)acc_loop/acc_cnt);
	}
	return NULL;
}
#endif

static void camera_recorder_send_filename(void)
{
	//cmsg.qtinfo.get_name.get_filename(emsg.capture_path[event - ACK_CAMERA_CAPTURE0], event - ACK_CAMERA_CAPTURE0);
	for(int i = 0; i<4 ; i++){
		memset(emsg.record_path[i],0x0,sizeof(emsg.record_path[i]));
	}
}

static struct record_context pContext;
int camera_recorder_start(struct cam_context *msg)
{
	int ret = SUCCESS;
	char card_name[128] = {0};
	//init start
	recorder_exit_flag = 3;
	if(emsg.record_path[3][0] != 0x0){
		memcpy(card_name, emsg.record_path[3], sizeof(card_name));
	}
	int mode = 0;
	int aimode = 0;
	mode = emsg.record_path[2][0];
	aimode = emsg.record_path[2][1];
	g_printf("mode %d aimode %d",mode,aimode);

	memset(&mux_dvp, 0x0 , sizeof(MUX_INFO));
	memset(&mux_mipi, 0x0 , sizeof(MUX_INFO));
	memset(&mux_drm, 0x0 , sizeof(MUX_INFO));

	//修复三路视频时长不一致问题：使用统一的录像开始时间作为基准时间戳
	//这样所有muxer的PTS都相对于同一时刻，时长一致
	RK_U64 record_start_us = TEST_COMM_GetNowUs();
	mux_dvp.shared_base_ts = record_start_us;
	mux_mipi.shared_base_ts = record_start_us;
	mux_drm.shared_base_ts = record_start_us;

	// aimode 兼容旧值 + 支持位掩码新值：
	// 旧值：0=DVP录音, 1=MIPI录音, 2=DRM录音
	// 新值(位掩码)：bit0=DVP, bit1=MIPI, bit2=DRM，可组合（如 3=DVP+MIPI, 6=MIPI+DRM, 7=全开）
    /*
        3:DVP + MIPI 录音
        5:DVP + DRM 录音
        6:MIPI + DRM 录音
        7:all
    */
	g_audio_mux_count = 0;
	if(aimode >= 0 && aimode <= 2){
		// 兼容旧值
		switch(aimode){
			case 0: mux_dvp.ai_enable = 1; break;
			case 1: mux_mipi.ai_enable = 1; break;
			case 2: mux_drm.ai_enable = 1; break;
		}
	}else if (aimode <= 7) {//客户传递100表示静音，所以这里要判断
		// 新值：位掩码，支持任意组合
		if(aimode & 0x01) mux_dvp.ai_enable = 1;
		if(aimode & 0x02) mux_mipi.ai_enable = 1;
		if(aimode & 0x04) mux_drm.ai_enable = 1;
	}

	// 收集所有需要录音的 mux 到全局列表
	if(mux_dvp.ai_enable){
		g_audio_mux_list[g_audio_mux_count++] = &mux_dvp;
	}
	if(mux_mipi.ai_enable){
		g_audio_mux_list[g_audio_mux_count++] = &mux_mipi;
	}
	if(mux_drm.ai_enable){
		g_audio_mux_list[g_audio_mux_count++] = &mux_drm;
	}
	g_printf("audio enable: mux_count=%d", g_audio_mux_count);

	get_save_filename(SAVE_RECORDER_MP4);
//	camera_thumbnail_start(msg,mode);
	camera_recorder_init(msg,card_name,mode);

	memset(&pContext, 0x0 , sizeof(struct record_context));
	pContext.mode = mode;
	pContext.msg = msg;

	//必须在创建录像线程之前把退出标志清零。开头设的 3 只用于停掉上一次录像残留的线程
	//(上一次 stop 已 join 完毕)；若留到线程创建之后才清零，DVP/MIPI 线程一进入循环就
	//读到 flag>=1 立刻 break，录出 0 帧(线程串行调度时必现，DRM 因创建较晚才侥幸躲过)。
	recorder_exit_flag = 0;

	switch(mode){
		case CAM_IR_RECORD:
			pthread_create(&recorder0, NULL ,camera_recorder_dvp_mp4_start, (void *)&pContext);
			break;
		case CAM_MIPI_RECORD:
			pthread_create(&recorder1, NULL ,camera_recorder_mipi_mp4_start, (void *)&msg->camVeContext[1]);
			break;
		case CAM_SCREEN_RECORD:
			pthread_create(&recorder1, NULL ,camera_recorder_drm_mp4_start, (void *)&pContext);
			break;
        case CAM_IR_MIPI_RECORD:
			pthread_create(&recorder0, NULL ,camera_recorder_dvp_mp4_start, (void *)&pContext);
			pthread_create(&recorder1, NULL ,camera_recorder_mipi_mp4_start, (void *)&msg->camVeContext[1]);
			break;
        case CAM_IR_SCREEN_RECORD:
			pthread_create(&recorder0, NULL ,camera_recorder_dvp_mp4_start, (void *)&pContext);
			pthread_create(&recorder1, NULL ,camera_recorder_drm_mp4_start, (void *)&pContext);
			break;
        case CAM_MIPI_SCREEN_RECORD:
			pthread_create(&recorder1, NULL ,camera_recorder_mipi_mp4_start, (void *)&msg->camVeContext[1]);
			pthread_create(&recorder2, NULL ,camera_recorder_drm_mp4_start, (void *)&pContext);
			break;
		case CAM_ALL_RECORD:
			pthread_create(&recorder0, NULL ,camera_recorder_dvp_mp4_start, (void *)&pContext);
			pthread_create(&recorder1, NULL ,camera_recorder_mipi_mp4_start, (void *)&msg->camVeContext[1]);
			pthread_create(&recorder2, NULL ,camera_recorder_drm_mp4_start, (void *)&pContext);
			break;
	}

	// 启动全局音频线程（只初始化一次 AI+AENC，同时写入所有需要录音的 mux）
	// 注意：等录像线程启动并创建好 MP4 muxer 后再启动音频线程，避免 AENC 队列溢出丢帧
	// 音频线程内部会用条件变量等待 g_audio_ready 信号

	// 等待所有录像线程启动完成（给 MP4_Muxer_Create 时间）
	usleep(200000);  // 200ms
	
	// 现在 mux 应该都创建好了，启动音频线程
	if(g_audio_mux_count > 0){
		g_audio_ready = 1;  // 先置位，音频线程启动后直接开始写
		pthread_create(&paenc_global, NULL, camera_recorder_global_aenc_start, NULL);
		g_printf("audio mux ready, start global aenc thread");
	}

	return ret;
}

int camera_recorder_stop(struct cam_context *msg)
{
	int ret = -1;
	struct Mpp_venc_api e_api = msg->cam_api;
	VencContext *eContext = (VencContext *)&msg->camVeContext;
	/*
	 * 这里不能用 emsg.record_path[2][0] 取 mode！
	 * 录像启动时 camera_recorder_start() 会调用 get_save_filename(SAVE_RECORDER_MP4)，
	 * 它用 snprintf 把 record_path[2] 覆盖成 MIPI 录像的输出文件路径
	 * (形如 "/xxx/VIDn_xxx.mp4")，所以此刻 record_path[2][0] 已经变成 '/'(47)，
	 * 而不再是启动时的 mode(例如 CAM_MIPI_SCREEN_RECORD=5)。
	 * 若继续用它，下面的 switch 匹配不到任何 case，MIPI 的 VENC(eContext[1]) 不会被
	 * 销毁、VPSS_CHN1->VENC 的绑定不会被解除；于是第二次录像 RK_MPI_SYS_Bind 会失败
	 * (返回 a0028010)，VENC 取不到帧，录出的 MP4 播放黑屏无图像。
	 * 正确做法是使用启动时已保存好的 pContext.mode。
	 */
	int mode = pContext.mode;

	while(1){
		if(recorder_exit_flag == 0){
			usleep(500*1000);
			break;
		}
		usleep(10*1000);
	}
	printf("recorder_exit_flag!!\n");
	RK_U64 stop_start_us = TEST_COMM_GetNowUs();
	g_printf("[REC-STOP] START closing all muxers at %lu us", stop_start_us);
	recorder_exit_flag = 1;
	//所有线程统一在 flag>=1 时退出，无需分阶段递增
	//join 顺序不重要，因为三路线程几乎同时退出，确保视频时长一致
    if (recorder0 != 0)
        pthread_join(recorder0, NULL);
    if (recorder1 != 0)
        pthread_join(recorder1, NULL);
    if(recorder2 != 0)
		pthread_join(recorder2, NULL);

    recorder0 = recorder1 = recorder2 = 0;

    // 等待全局音频线程结束
    if(paenc_global != 0){
        pthread_join(paenc_global, NULL);
        paenc_global = 0;
    }

    /* 所有线程都已退出，安全关闭 muxer（避免 use-after-free）
     * 注意：mux_drm 由 DRM 录像线程自己关闭，这里不重复关闭 */
    if (mux_dvp.g_OutFmt_Ctx != NULL) {
        g_printf("[REC-STOP] Closing DVP muxer...");
        MP4_Muxer_Close(&mux_dvp);
        g_printf("[REC-STOP] DVP muxer closed");
    }
    if (mux_mipi.g_OutFmt_Ctx != NULL) {
        g_printf("[REC-STOP] Closing MIPI muxer...");
        MP4_Muxer_Close(&mux_mipi);
        g_printf("[REC-STOP] MIPI muxer closed");
    }
    
    RK_U64 stop_end_us = TEST_COMM_GetNowUs();
    g_printf("[REC-STOP] COMPLETE all muxers closed in %lu ms", (stop_end_us - stop_start_us)/1000);

    /*
     * 下面的销毁必须与 camera_recorder_init() 的创建严格对称：
     *   IR(eContext[0]) / MIPI(eContext[1]) 由本函数销毁；
     *   DRM(eContext[2]) 由 drm 录像线程(camera_recorder_drm_mp4_start)自己在结束时销毁，这里不能重复销毁。
     *   注意：VPSS CHN1 已在 mpp_create_vi 中预创建，由 mpp_destroy_vi 销毁，这里不再 DisableChn。
     */
    switch (mode) {
        case 0: /* CAM_IR_RECORD: 只创建了 IR(eContext[0]) */
            e_api.stop_ve(&eContext[0]);
            e_api.destroy_ve(&eContext[0]);
            break;
        case 1: /* CAM_MIPI_RECORD: 只创建了 MIPI(eContext[1]) */
            e_api.stop_ve(&eContext[1]);
            e_api.destroy_ve(&eContext[1]);
            break;
        case 2: /* CAM_SCREEN_RECORD: 只创建了 DRM(eContext[2])，且由 drm 录像线程自行销毁，这里无需处理。 */
            break;
        case 3: /* CAM_IR_MIPI_RECORD */
            e_api.stop_ve(&eContext[0]);
            e_api.destroy_ve(&eContext[0]);
            e_api.stop_ve(&eContext[1]);
            e_api.destroy_ve(&eContext[1]);
            break;
        case 4: /* CAM_IR_SCREEN_RECORD */
            e_api.stop_ve(&eContext[0]);
            e_api.destroy_ve(&eContext[0]);
            break;
        case 5: /* CAM_MIPI_SCREEN_RECORD */
            e_api.stop_ve(&eContext[1]);
            e_api.destroy_ve(&eContext[1]);
            break;
        case 6: /* CAM_ALL_RECORD */
            e_api.stop_ve(&eContext[0]);
            e_api.destroy_ve(&eContext[0]);
            e_api.stop_ve(&eContext[1]);
            e_api.destroy_ve(&eContext[1]);
            break;
    }

	camera_recorder_send_filename();
	return ret;
}