#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rk_mpi_vi.h"
#include "rk_mpi_vpss.h"
#include "rk_mpi_mb.h"
#include "camera_main.h"
#include "camera_ai.h"

/*>>>>>>>>>>>>>>>>>>>>>>
 test load file
 <<<<<<<<<<<<<<<<<<<<<<<<<<<<<*/
static unsigned char* load_data(FILE* fp, size_t ofst, size_t sz)
{
    unsigned char* data;
    int            ret;

    data = NULL;

    if (NULL == fp) {
        return NULL;
    }

    ret = fseek(fp, ofst, SEEK_SET);
    if (ret != 0) {
        printf("blob seek failure.\n");
        return NULL;
    }

    data = (unsigned char*)malloc(sz);
    if (data == NULL) {
        printf("buffer malloc failure.\n");
        return NULL;
    }
    ret = fread(data, 1, sz, fp);
    return data;
}

static unsigned char* load_model(const char* filename, int* model_size)
{
    FILE*          fp;
    unsigned char* data;

    fp = fopen(filename, "rb");
    if (NULL == fp) {
        printf("Open file %s failed.\n", filename);
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    int size = ftell(fp);

    data = load_data(fp, 0, size);

    fclose(fp);

    *model_size = size;
    return data;
}
static unsigned int load_file(const char *name, void *dst)
{
    FILE *fp = fopen(name, "rb");
    unsigned int size = 0;
    size_t read_count = 0;

    if (fp != NULL) {
        fseek(fp, 0, SEEK_END);
        size = ftell(fp);

        fseek(fp, 0, SEEK_SET);
        read_count = fread(dst, size, 1, fp);
        if(read_count != 1)
        {
            printf("Read file %s failed.\n", name);
        }

        fclose(fp);
    } else {
        printf("Load file %s failed.\n", name);
    }

    return size;
}

/*>>>>>>>>>>>>>>>>>>>>>>
 ai odet deal
 <<<<<<<<<<<<<<<<<<<<<<<<<<<<<*/
void camera_ai_detect_res_deal(detect_result_group_t *res)
{
	AI_LOG("res->count %d ",res->count);
	if(res->count > 0){
		for (int i = 0; i < res->count; i++) {
			detect_result_t* det_result = &(res->results[i]);
			cmsg.qtinfo.get_ai.get_aixy(cmsg.qtinfo.get_ai.context,res->count,i,det_result->name, det_result->prop,
					det_result->box.left, det_result->box.top,\
					det_result->box.right-det_result->box.left, det_result->box.bottom-det_result->box.top);
		}
	}else{
		cmsg.qtinfo.get_ai.get_aixy(cmsg.qtinfo.get_ai.context,res->count,0,"null",0,0,0,0,0);
	}
}

/*>>>>>>>>>>>>>>>>>>>>>>
 ai loop odet
 <<<<<<<<<<<<<<<<<<<<<<<<<<<<<*/
static volatile unsigned char odet_loop = 0;

void camera_ai_set_odet_loop(unsigned char loop)
{
	odet_loop = loop;
}

#define AI_GRP CAM_GRP_INDEX_DVP
int camera_ai_start(void *p)
{
	RK_S32 s32Ret = 0;
	g_printf("%s strat ",__func__);

	//use chn 3
	VPSS_CHN_ATTR_S stVpssChnAttr;
	memset(&stVpssChnAttr, 0x0 ,sizeof(VPSS_CHN_ATTR_S));
	stVpssChnAttr.enChnMode = VPSS_CHN_MODE_PASSTHROUGH;
	stVpssChnAttr.enDynamicRange = DYNAMIC_RANGE_SDR8;
	stVpssChnAttr.enPixelFormat = RK_FMT_RGB888;
	//stVpssChnAttr.enPixelFormat = RK_FMT_YUV422_UYVY;
	stVpssChnAttr.stFrameRate.s32SrcFrameRate = -1;
	stVpssChnAttr.stFrameRate.s32DstFrameRate = -1;
	stVpssChnAttr.u32Width = CONFIG_DVP_SRC_WIDTH;
	stVpssChnAttr.u32Height = CONFIG_DVP_SRC_HEIGHT;
	stVpssChnAttr.enCompressMode = COMPRESS_MODE_NONE;
	stVpssChnAttr.u32Depth = 1;

	s32Ret = RK_MPI_VPSS_SetChnAttr(AI_GRP, VPSS_CHN3, &stVpssChnAttr);
	RK_LOGE("RK_MPI_VPSS_SetChnAttr %d %x",AI_GRP, s32Ret);

	s32Ret = RK_MPI_VPSS_EnableChn(AI_GRP, VPSS_CHN3);
	RK_LOGE("RK_MPI_VPSS_EnableChn %d %x",AI_GRP, s32Ret);


	RK_S32 waitTime = 200; 
	int errCount = 0;
	static VIDEO_FRAME_INFO_S stViFrame;
	memset(&stViFrame, 0x0 ,sizeof(VIDEO_FRAME_INFO_S));

	odet_loop = 1;
	//UYVY
	//int len = stVpssChnAttr.u32Width * stVpssChnAttr.u32Height * 2;
	//RGB
	int len = stVpssChnAttr.u32Width * stVpssChnAttr.u32Height * 3;

	//yolov5_init(YOLOV5_MODEL_PATH, LABEL_NALE_TXT_PATH);
	yolov5_init(emsg.ai_path[0], emsg.ai_path[1]);
	//FILE *fp = test_file_create("/tmp/ai.yuv");
	int raw_size = 0;
	//unsigned char *raw_data = load_model("/userdata/ai/gas.raw", &raw_size);
	while (1) {
		s32Ret = RK_MPI_VPSS_GetChnFrame(AI_GRP, VPSS_CHN3, &stViFrame, waitTime);
		if (s32Ret == RK_SUCCESS) {
			void *data = RK_MPI_MB_Handle2VirAddr(stViFrame.stVFrame.pMbBlk);
			RK_MPI_SYS_MmzFlushCache(stViFrame.stVFrame.pMbBlk, RK_TRUE);

			//if(check_infrared_frame_tag(data) == INFRARED_FRAME_TAG_1122)
			uint16_t tag_value = 0;
			uint16_t tag_tmp = 0;
			uint32_t frame_tag_offset = CONFIG_DVP_SRC_WIDTH * (CONFIG_DVP_SRC_HEIGHT - CONFIG_DVP_SRC_INFO) * 3;
			{
				//uint32_t frame_tag_offset = 640 * 645 * 3;

				for (int i = 0; i < 4; i++) {
					tag_value |= *(uint16_t *)(data + frame_tag_offset + 3 * i);
				}
				tag_tmp = tag_value & 0xff00;
				tag_value <<= 8;
				tag_value |= (tag_tmp >> 8);

			}

			if(tag_value < 0x0200)
			{
				AI_LOG("infrared frame type %04x", tag_value);
				//deal
				//test_file_write(fp, data, len);
				yolov5_detect(data, CONFIG_DVP_SRC_WIDTH, (CONFIG_DVP_SRC_HEIGHT - CONFIG_DVP_SRC_INFO));
			}else{
				AI_LOG("no odet infrared frame type %04x\n", tag_value);
			}

			RK_MPI_VPSS_ReleaseChnFrame(AI_GRP, VPSS_CHN3, &stViFrame);
		} else { 
			errCount ++; 
			RK_LOGE("RK_MPI_VPSS_GetChnFrame timeout %x ,err %d", s32Ret,errCount);
		}

		if(odet_loop == 0)
		{
			break;
		}

	}
	//if (raw_data) { free(raw_data);}
	//test_file_close(fp);
	s32Ret = RK_MPI_VPSS_DisableChn(AI_GRP, VPSS_CHN3);
	RK_LOGE("RK_MPI_VPSS_DisableChn %d %x",AI_GRP, s32Ret);

	yolov5_deinit();
	g_printf("%s end",__func__);
	return s32Ret;
}


