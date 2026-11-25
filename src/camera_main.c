#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include "camera_main.h"
#include "gas_quantification.h"

struct event_msg  emsg = {0};
struct camera_msg cmsg = {0};
struct camera_sem csem = {0};
CAMCONFIG camera_conf = {0};

const char *curtime()
{
	static char __timebuf[64];
	struct timeval tv;
	struct tm *lt;

	gettimeofday(&tv, NULL);
	lt = localtime(&tv.tv_sec);
	memset(__timebuf, 0, sizeof(__timebuf));
	snprintf(__timebuf, sizeof(__timebuf) - 1,
			"%04d-%02d-%02d_%02d:%02d:%02d.%03d",
			lt->tm_year + 1900,
			lt->tm_mon + 1,
			lt->tm_mday,
			lt->tm_hour,
			lt->tm_min,
			lt->tm_sec,
			(int)(tv.tv_usec / 1000));
	return __timebuf;
}

void camera_update_qt_sta(CAMSTA once_call)
{
	g_printf("emsg.rtsp_en %d, emsg.rtmp_en %d",emsg.rtsp_en,emsg.rtmp_en);
	if(once_call){
		cmsg.qtinfo.get_sta.cam_status(cmsg.qtinfo.get_sta.context,once_call,emsg.dis_mode,emsg.ai_sta,emsg.rtsp_en,emsg.rtmp_en);
	}else{
		cmsg.qtinfo.get_sta.cam_status(cmsg.qtinfo.get_sta.context,emsg.cam_sta,emsg.dis_mode,emsg.ai_sta,emsg.rtsp_en,emsg.rtmp_en);
	}
}

static void camera_exit(void)
{
    printf("sclu %s %d\n", __func__, __LINE__);
	csem.exit_flag = 1;
	sem_post(&csem.camSem);
	sem_post(&csem.disSem);
	sem_post(&csem.PkgSem);
	camera_wbc_set_loop(0);
	camera_dvp_set_loop(0);
	camera_mipi_set_loop(0);
}

/*
   mipi
 */
#define MIPI_POWER_EN 1
static int mipi_busy = 0;
static void camera_mipi_init(void)
{
#if MIPI_POWER_EN
	//enable mipi -> vpss
	struct dis_context *msg = (struct dis_context *)&cmsg.disContext;
	if(emsg.cam_initok && !mipi_busy){
		mpp_fun_init(&msg->dis_api,&msg->disViContext[1],NULL);
	}
	mipi_busy++;
#else
	return;
#endif
}
static void camera_mipi_destroy(void)
{
#if MIPI_POWER_EN
	struct dis_context *msg = (struct dis_context *)&cmsg.disContext;
	mipi_busy--;
	if(emsg.cam_initok && !mipi_busy){
		mpp_fun_destroy(&msg->dis_api,&msg->disViContext[1],NULL);
	}
#else
	return;
#endif
}

static void *CamDealFunction(void *p)
{
	int ret = SUCCESS;
	struct cam_context *msg = (struct cam_context *)p;
	mpp_ve_get_api(&msg->cam_api);
	emsg.cam_sta = CAM_CAMERA_IDLE;

	//cam
	while(1){
        printf("sclu %s %d emsg.cam_sta=%d\n", __func__, __LINE__, emsg.cam_sta);
		sem_wait(&csem.camSem);
        printf("sclu %s %d emsg.cam_sta=%d\n", __func__, __LINE__, emsg.cam_sta);
		if(csem.exit_flag){
            printf("sclu %s %d \n", __func__, __LINE__);
			break;
		}else if(emsg.cam_initok == 0){
			usleep(10*1000);
			sem_post(&csem.camSem);
            printf("sclu %s %d \n", __func__, __LINE__);
			continue;
		}
        printf("sclu %s %d emsg.cam_sta=%d\n", __func__, __LINE__, emsg.cam_sta);
		switch(emsg.cam_sta){
			case CAM_CAMERA_CAPTURE_IR:
			case CAM_CAMERA_CAPTURE_MIPI:
			case CAM_CAMERA_CAPTURE_SCREEN:
            case CAM_CAMERA_CAPTURE_IR_MIPI:
            case CAM_CAMERA_CAPTURE_IR_SCREEN:
            case CAM_CAMERA_CAPTURE_MIPI_SCREEN:
			case CAM_CAMERA_CAPTURE_ALL:
				g_printf("CAM_CAMERA_CAPTURE");
				camera_mipi_init();
				camera_takepicture_start(msg);
				camera_mipi_destroy();
				emsg.cam_sta = CAM_CAMERA_IDLE;
				camera_update_qt_sta(0);
				break;
			case CAM_CAMERA_RECORDSTART:
				g_printf("CAM_CAMERA_RECORDSTART");
				camera_mipi_init();
				camera_recorder_start(msg);
				break;
			case CAM_CAMERA_RECORDSTOP:
				g_printf("CAM_CAMERA_RECORDSTOP");
				camera_recorder_stop(msg);
				camera_mipi_destroy();
				emsg.cam_sta = CAM_CAMERA_IDLE;
				camera_update_qt_sta(0);
				break;
			case CAM_CAMERA_RTSPSTART:
				g_printf("CAM_CAMERA_RTSPSTART");
				if(camera_rtsp_get_mode() == CAP_RTSP_RTMP_VL){
					camera_mipi_init();
				}
				camera_rtsp_start(msg);
				emsg.cam_sta = CAM_CAMERA_IDLE;
				camera_update_qt_sta(0);
				break;
			case CAM_CAMERA_RTSPSTOP:
				g_printf("CAM_CAMERA_RTSPSTOP");
				camera_rtsp_stop(msg);
				if(camera_rtsp_get_mode() == CAP_RTSP_RTMP_VL){
					camera_mipi_destroy();
				}
				emsg.rtsp_en = 0;
				emsg.cam_sta = CAM_CAMERA_IDLE;
				camera_update_qt_sta(0);
				break;
			case CAM_CAMERA_RTMPSTART:
				g_printf("CAM_CAMERA_RTMPSTART");
				if(camera_rtsp_get_mode() == CAP_RTSP_RTMP_VL){
					camera_mipi_init();
				}
				camera_rtmp_start(msg);
				emsg.cam_sta = CAM_CAMERA_IDLE;
				camera_update_qt_sta(0);
				break;
			case CAM_CAMERA_RTMPSTOP:
				g_printf("CAM_CAMERA_RTMPSTOP");
				camera_rtmp_stop(msg);
				if(camera_rtsp_get_mode() == CAP_RTSP_RTMP_VL){
					camera_mipi_destroy();
				}
				emsg.rtmp_en = 0;
				emsg.cam_sta = CAM_CAMERA_IDLE;
				camera_update_qt_sta(0);
				break;
			default:
				break;
		}
	}
	y_printf("--- cam destroy");
	if(emsg.cam_sta == CAM_CAMERA_RECORDSTART){
		camera_recorder_stop(msg);
	}
	if(emsg.rtmp_en){
		system(RTMP_PATH_OFF);
		emsg.rtmp_en = 0;
	}
	if(emsg.rtsp_en){
		camera_rtsp_stop(msg);
		emsg.rtsp_en = 0;
	}
	y_printf("--- cam destroy end---");
	emsg.cam_sta = CAM_CAMERA_IDLE;
	return NULL;
}

static void *DisDealFunction(void *p)
{
	int ret = SUCCESS;
	struct dis_context *msg = (struct dis_context *)p;
	display_get_api(&msg->dis_api);
	//enable dvp -> vpss
	emsg.dis_sta = DIS_CAMERA_SRC;
	sem_post(&csem.disSem);

	//display
	while(1){
		sem_wait(&csem.disSem);
		if(csem.exit_flag){
			if(emsg.cam_sta != CAM_CAMERA_IDLE){
				usleep(10*1000);
				sem_post(&csem.disSem);
				continue;
			}
			break;
		}

		switch(emsg.dis_sta){
			case DIS_CAMERA_SRC:
				g_printf("DIS_CAMERA_SRC");
				if(!emsg.cam_initok){
					msg->disViContext[0].ccd_index = 0;
					msg->disViContext[1].ccd_index = 3;
					// 仅 ir 常开 节省功耗
#if MIPI_POWER_EN //这里意思是默认初始化时候只初始化ir相机
					for(int i = 0; i < 1; i++)
#else
					for(int i = 0; i < 2; i++)
#endif
					{
						mpp_fun_init(&msg->dis_api,&msg->disViContext[i],NULL);
					}
					emsg.cam_initok = 1;
				}
                printf("sclu %s %d\n", __func__, __LINE__);
				if(emsg.dis_mode & BIT(0)){
                    printf("sclu %s %d\n", __func__, __LINE__);
					if(!msg->disViContext[0].viThreadID){
                        printf("sclu %s %d\n", __func__, __LINE__);
						g_printf("DIS_CAMERA_OUT_IR");
						mpp_fun_run(&msg->dis_api,&msg->disViContext[0],NULL);
						pthread_create(&msg->disViContext[0].viThreadID, NULL, camera_mpp_dvp_loop, (void *)msg);
					}
				}
				if(emsg.dis_mode & BIT(1)){
					if(!msg->disViContext[1].viThreadID){
						g_printf("DIS_CAMERA_OUT_VL");
						//mpp_fun_init(&msg->dis_api,&msg->disViContext[1],NULL);
						camera_mipi_init();
						mpp_fun_run(&msg->dis_api,&msg->disViContext[1],NULL);
						pthread_create(&msg->disViContext[1].viThreadID, NULL, camera_mpp_mipi_loop, (void *)msg);
					}
				}
				break;
			default:
				break;
		}
	}
	y_printf("--- dis destroy");
#if MIPI_POWER_EN
	emsg.cam_initok = 0;
	if(emsg.dis_mode & BIT(0)){
		if(msg->disViContext[0].viThreadID){
			pthread_join(msg->disViContext[0].viThreadID, NULL);
			msg->disViContext[0].viThreadID = 0;
		}
		mpp_fun_destroy(&msg->dis_api,&msg->disViContext[0],NULL);
	}
	if(emsg.dis_mode & BIT(1)){
		if(msg->disViContext[1].viThreadID){
			pthread_join(msg->disViContext[1].viThreadID, NULL);
			msg->disViContext[1].viThreadID = 0;
		}
		mpp_fun_destroy(&msg->dis_api,&msg->disViContext[1],NULL);
	}
#else
	if(emsg.dis_sta != DIS_CAMERA_IDLE){
		emsg.cam_initok = 0;
		for(int i = 0; i < 2; i++){
			if(msg->disViContext[i].viThreadID){
				pthread_join(msg->disViContext[i].viThreadID, NULL);
				msg->disViContext[i].viThreadID = 0;
			}
			mpp_fun_destroy(&msg->dis_api,&msg->disViContext[i],NULL);
		}
	}
#endif
	y_printf("--- dis destroy end---");
	emsg.dis_sta = DIS_CAMERA_IDLE;

	return NULL;
}

static void *AiDealFunction(void *p)
{
	int ret = SUCCESS;

	emsg.ai_sta = AI_CAMERA_IDLE;
	while(1){
		sem_wait(&csem.aiSem);
		if(csem.exit_flag){
			break;
		}else if(emsg.cam_initok == 0){
			usleep(10*1000);
			sem_post(&csem.aiSem);
			continue;
		}

		switch(emsg.ai_sta){
			case AI_CAMERA_START:
				g_printf("AI_CAMERA_START");
				camera_ai_start(NULL);
				emsg.ai_sta = AI_CAMERA_IDLE;
				camera_update_qt_sta(0);
				break;
			default:
				break;
		}
	}
	y_printf("--- ai destroy end---");
	emsg.ai_sta = AI_CAMERA_IDLE;
	return NULL;
}

static void *PkgDealFunction(void *p)
{
	int result = SUCCESS;
	struct camera_msg *cmsg = (struct camera_msg *)p;

	//sys init
    RK_MPI_SYS_Init();
	sys_sem_init();
	//mipi source init
	emsg.mipi_source_x = 0;
	emsg.mipi_source_y = 0;
	emsg.mipi_source_w = CONFIG_MIPI_SRC_WIDTH;
	emsg.mipi_source_h = CONFIG_MIPI_SRC_HEIGHT;

	//cam
	cmsg->pthread_sta |= BIT(0);
	//display
	cmsg->pthread_sta |= BIT(1);
	//ai
	cmsg->pthread_sta |= BIT(2);

	sem_post(&csem.PkgSem);
	while(1){
		sem_wait(&csem.PkgSem);
		//cam pthread
		if((cmsg->pthread_sta & BIT(0))&&(!cmsg->camThreadID)){
			g_printf("sclu create cam thread");
			pthread_create(&cmsg->camThreadID, NULL, CamDealFunction, &cmsg->camContext);
		}
#if 1
		//dis pthread
		if((cmsg->pthread_sta & BIT(1))&&(!cmsg->disThreadID)){
			g_printf("create dis thread");
			pthread_create(&cmsg->disThreadID, NULL, DisDealFunction, &cmsg->disContext);
		}
		//ai pthread
		if((cmsg->pthread_sta & BIT(2))&&(!cmsg->aiThreadID)){
			g_printf("create ai thread");
			pthread_create(&cmsg->aiThreadID, NULL, AiDealFunction, NULL);
		}
#endif
		if(csem.exit_flag){
			break;
		}
	}

	if(cmsg->camThreadID)
		pthread_join(cmsg->camThreadID, NULL);
	if(cmsg->disThreadID)
		pthread_join(cmsg->disThreadID, NULL);

	cmsg->camThreadID = 0;
	cmsg->disThreadID = 0;

    /* exit mpp system */
	RK_MPI_SYS_Exit();
	sys_sem_deinit();
	return NULL;
}

/* 打印库的编译时间（年月日时分秒），__DATE__/__TIME__由编译器在编译时自动写入 */
static void camera_print_build_time(void)
{
	static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	char mon[4] = {0};
	int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
	const char *p;

	sscanf(__DATE__, "%3s %d %d", mon, &day, &year);
	sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
	p = strstr(months, mon);
	int month = p ? (int)(p - months) / 3 + 1 : 0;
	g_printf("----- build time: %04d-%02d-%02d %02d:%02d:%02d -----", year, month, day, hh, mm, ss);
}

static int camera_init(void)
{
	csem.exit_flag = 0;
    pthread_create(&cmsg.PkgDealThreadID, NULL, PkgDealFunction, &cmsg);
	g_printf("----- Rain camera start  -----");
    g_printf("----- %s  ----- ver %s -----",GIT_VERSION,RAIN_CAMERA_VERSION);
	g_printf("----- %s -------------",qt_camera_get_version());
	camera_print_build_time();
	return 0;
}

static int camera_destroy(void)
{
	camera_exit();
	pthread_join(cmsg.PkgDealThreadID, NULL);
	cmsg.PkgDealThreadID = 0;
    g_printf("----- Rain camera end   -----");
	return 0;
}

/* ----------------------
          QT CAMERA
----------------------- */
int qt_camera_register_get_framer(QTCAM *info)
{
	memcpy(&cmsg.qtinfo, info, sizeof(QTCAM));
	return SUCCESS;
}

int qt_camera_config(CAMCONFIG *config)
{
	camera_config(config);
	return SUCCESS;
}

int qt_camera_init(void)
{
	if(!emsg.cam_initok){
        camera_init();
#if 0
        if (display_init(1080, 1920) < 0) {
            printf("display_init failed\n");
        } else {
            printf("display_init success\n");
        }
#else
     //   initDrmDsp(1);
#endif
	}
	return SUCCESS;
}

int qt_camera_exit(void)
{
    //display_exit();
	camera_destroy();
	return SUCCESS;
}

int qt_camera_start_frame(CAMSTA dis_sta)
{
	y_printf("%s cur dis_mode %d use dis_mode %d",__func__,emsg.dis_mode,dis_sta);

	while(emsg.cam_initok == 0){
		usleep(50*1000);
	}
	switch(dis_sta){
		case DIS_CAMERA_OUT_IR:
			if(!(emsg.dis_mode & BIT(0))){
				emsg.dis_mode |= BIT(0);
				camera_update_qt_sta(0);
                printf("sclu %s %d\n", __func__, __LINE__);
				sem_post(&csem.disSem);
			}
			break;
		case DIS_CAMERA_OUT_VL:
			if(!(emsg.dis_mode & BIT(1))){
				emsg.dis_mode |= BIT(1);
                printf("sclu %s %d\n", __func__, __LINE__);
				camera_update_qt_sta(0);
				sem_post(&csem.disSem);
			}
			break;
		default:
			break;
	}
	return SUCCESS;
}

int qt_camera_release_frame(uint64_t pts, CAMSTA dis_sta)
{
	//g_printf("%s cur dis_mode %d use dis_sta %d pts %lld",__func__,emsg.dis_mode,dis_sta,pts);
	switch(dis_sta){
		case DIS_CAMERA_OUT_IR:
			camera_mpp_dvp_release(pts);
			break;
		case DIS_CAMERA_OUT_VL:
			camera_mpp_mipi_release(pts);
			break;
		default:
			break;
	}
	return SUCCESS;
}

int qt_camera_stop_frame(CAMSTA dis_sta)
{
	y_printf("%s cur dis_mode %d use dis_sta %d",__func__,emsg.dis_mode,dis_sta);
	struct dis_context *msg = (struct dis_context *)&cmsg.disContext;
	switch(dis_sta){
		case DIS_CAMERA_OUT_IR:
			if(emsg.dis_mode & BIT(0)){
				emsg.dis_mode &= ~BIT(0);
				camera_dvp_set_loop(0);
				pthread_join(msg->disViContext[0].viThreadID, NULL);
				msg->disViContext[0].viThreadID = 0;
				mpp_fun_stop(&msg->dis_api,&msg->disViContext[0],NULL);
				camera_update_qt_sta(0);
			}
			break;
		case DIS_CAMERA_OUT_VL:
			if(emsg.dis_mode & BIT(1)){
				emsg.dis_mode &= ~BIT(1);
				camera_mipi_set_loop(0);
				pthread_join(msg->disViContext[1].viThreadID, NULL);
				msg->disViContext[1].viThreadID = 0;
				//mpp_fun_stop(&msg->dis_api,&msg->disViContext[1],NULL);
				//mpp_fun_destroy(&msg->dis_api,&msg->disViContext[1],NULL);
				camera_mipi_destroy();
				camera_update_qt_sta(0);
			}
			break;
		default:
			break;
	}
	return SUCCESS;
}

typedef struct _recordinput{
	int mode;
	int aichl;
	char cardname[20];
}recordinput;
int qt_camera_cam_control(CAMSTA cam_sta, char *name, void *input)
{
	y_printf("%s cur cam_sta %d, use sta %d",__func__,emsg.cam_sta,cam_sta);
	if((emsg.cam_sta == CAM_CAMERA_IDLE)||
			((emsg.cam_sta == CAM_CAMERA_RECORDSTART)&&(cam_sta == CAM_CAMERA_RECORDSTOP))||
			(cam_sta >= CAM_CAMERA_RTSPSTART)){
		switch(cam_sta){
			case CAM_CAMERA_CAPTURE_IR:
			case CAM_CAMERA_CAPTURE_MIPI:
			case CAM_CAMERA_CAPTURE_SCREEN:
            case CAM_CAMERA_CAPTURE_IR_MIPI:
            case CAM_CAMERA_CAPTURE_IR_SCREEN:
            case CAM_CAMERA_CAPTURE_MIPI_SCREEN:
			case CAM_CAMERA_CAPTURE_ALL:
				if(name != NULL){
					strcpy(emsg.capture_path[0], name);//这个赋值没用到，后面被覆盖了
				}
				emsg.capture_path[1][0] = cam_sta;
				break;
			case CAM_CAMERA_RECORDSTART:
				if(name != NULL){
					strcpy(emsg.record_path[0], name);
				}
				/* 封装和 MP3 编码固定 48k/1152。MP4 不接受 16k/576 的 MPEG-2 MP3，
				 * avformat_write_header 会失败并把这一路文件丢掉。
				 * 蓝牙 SCO 的实际采样率在打开声卡时再重采样到 48k。 */
				emsg.audio_sample_rate = 48000;
				emsg.audio_frame_samples = 1152;
				if(input != NULL){
					recordinput *rinput = (recordinput *)input;
					if(rinput->cardname[0] != '\0'){
						/* 采集线程可能正在读这个名字。相同 MAC 不要先清空再写回去。 */
						if(strncmp(emsg.audio_card_name, rinput->cardname, sizeof(rinput->cardname)) != 0){
							memset(emsg.audio_card_name, 0, sizeof(emsg.audio_card_name));
							memcpy(emsg.audio_card_name, rinput->cardname, sizeof(rinput->cardname));
							emsg.audio_card_name[sizeof(emsg.audio_card_name) - 1] = '\0';
						}
					}else{
						emsg.audio_card_name[0] = '\0';
					}
					emsg.record_path[2][0] = rinput->mode;
					emsg.record_path[2][1] = rinput->aichl;
					y_printf("%s record mode %d aichl %d cardname %s rate %d frame %d", __func__,
						rinput->mode, rinput->aichl, emsg.audio_card_name,
						emsg.audio_sample_rate, emsg.audio_frame_samples);
				}
				break;
			case CAM_CAMERA_RECORDSTOP:
				break;
			case CAM_CAMERA_RTSPSTART:
				if(!emsg.rtsp_en){
					emsg.rtsp_en = 1;
				}else{
					goto _errEvent;
				}
				break;
			case CAM_CAMERA_RTSPSTOP:
				if(!emsg.rtsp_en){
					goto _errEvent;
				}
				break;
			case CAM_CAMERA_RTMPSTART:
				if(!emsg.rtmp_en){
					emsg.rtmp_en = 1;
				}else{
					goto _errEvent;
				}
				break;
			case CAM_CAMERA_RTMPSTOP:
				if(!emsg.rtmp_en){
					goto _errEvent;
				}
				break;
			default:
				break;
		}
		emsg.cam_sta = cam_sta;
        printf("sclu %s %d sem_post\n", __func__, __LINE__);
		sem_post(&csem.camSem);
	}

_errEvent:
	camera_update_qt_sta(0);
	return SUCCESS;
}

//递归创建目录：单层 mkdir 在父目录不存在时会失败(报错且不创建)，
//这里逐级补建所有缺失的父目录。已存在的层级 mkdir 返回 EEXIST，忽略即可。
static int camera_mkdir_p(const char *path)
{
	char tmp[MAX_FILE_PATH_SIZE];
	if(!path || !path[0]){
		return -1;
	}
	snprintf(tmp, sizeof(tmp), "%s", path);
	size_t len = strlen(tmp);
	if(tmp[len - 1] == '/'){
		tmp[len - 1] = 0;
	}
	for(char *p = tmp + 1; *p; p++){
		if(*p == '/'){
			*p = 0;
			mkdir(tmp, 0777);
			*p = '/';
		}
	}
	mkdir(tmp, 0777);
	return 0;
}

int qt_camera_cam_set_media_path(char *imgtmppath, char *mediapath, char *thumbnailPath)
{
	memset(emsg.imgTmpPath,0x0,sizeof(emsg.imgTmpPath));
	strcpy(emsg.imgTmpPath, imgtmppath);
	memset(emsg.mediaPath,0x0,sizeof(emsg.mediaPath));
	strcpy(emsg.mediaPath, mediapath);
	memset(emsg.thumbnailPath,0x0,sizeof(emsg.thumbnailPath));
	strcpy(emsg.thumbnailPath, thumbnailPath);
	if(access(emsg.imgTmpPath, F_OK) != 0){
		camera_mkdir_p(emsg.imgTmpPath);
	}
	if(access(emsg.mediaPath, F_OK) != 0){
		camera_mkdir_p(emsg.mediaPath);
	}
	if(access(emsg.thumbnailPath, F_OK) != 0){
		camera_mkdir_p(emsg.thumbnailPath);
	}
	y_printf("%s set imgtmp path %s, media path %s, thumbnail path %s",__func__,emsg.imgTmpPath,emsg.mediaPath,emsg.thumbnailPath);
	return SUCCESS;
}

int qt_camera_rtsp_rtmp_set_mode(CAPRTSPRTMPMODE mode)
{
	camera_rtsp_set_mode(mode);
	return SUCCESS;
}

int qt_camera_rtmp_set_url(char *url)
{
	memset(emsg.rtmp_url,0x0,sizeof(emsg.rtmp_url));
	strcpy(emsg.rtmp_url, url);
	return SUCCESS;
}

int qt_camera_ai_control(CAMSTA ai_sta, const char *model_path, const char *label_path)
{
	y_printf("%s ai_sta %d",__func__,ai_sta);
	switch(ai_sta){
		case AI_CAMERA_START:
			if(emsg.ai_sta == AI_CAMERA_IDLE){
				memset(emsg.ai_path[0],0x0,sizeof(emsg.ai_path[0]));
				//memset(emsg.ai_path[1],0x0,sizeof(emsg.ai_path[1]));
				strcpy(emsg.ai_path[0], model_path);
				//strcpy(emsg.ai_path[1], label_path);
				emsg.ai_sta = AI_CAMERA_START;
				sem_post(&csem.aiSem);
				camera_update_qt_sta(0);
			}
			break;
		case AI_CAMERA_STOP:
			if(emsg.ai_sta == AI_CAMERA_START){
				camera_ai_set_odet_loop(0);
			}
			break;
		default:
			break;
	}
	return SUCCESS;
}
int qt_camera_ai_set_vendor(int vendor)
{
	y_printf("%s ai vendor=%d", __func__, vendor);
	return 0;
}
int qt_camera_source_vl_set_crop(int x, int y, int w, int h)
{
	if(w < 16 || h < 16){
		y_printf("%s ignore invalid crop x %d y %d w %d h %d, keep %d %d %d %d",
			__func__, x, y, w, h,
			emsg.mipi_source_x, emsg.mipi_source_y, emsg.mipi_source_w, emsg.mipi_source_h);
		return SUCCESS;
	}
	emsg.mipi_source_x = x; //2
	emsg.mipi_source_y = y; //2
	emsg.mipi_source_w = w; //16
	if(h == 1072)h = 1080;
	emsg.mipi_source_h = h; //16
	//emsg.mipi_source_w = RAIN_ALIGN_16(w);
	//emsg.mipi_source_h = RAIN_ALIGN_16(h);
	y_printf("%s x %d y %d w %d h %d",__func__,x,y,emsg.mipi_source_w,emsg.mipi_source_h);

	if(emsg.cam_initok && (emsg.dis_mode & BIT(1))){
		camera_mpp_mipi_set_crop(emsg.mipi_source_x,emsg.mipi_source_y,emsg.mipi_source_w,emsg.mipi_source_h);
	}
	return SUCCESS;
}

int qt_camera_source_drm_set_crop(int x, int y, int w, int h)
{
	emsg.drm_source_x = x;
	emsg.drm_source_y = y;
	emsg.drm_source_w = w;
	emsg.drm_source_h = h;
	y_printf("%s x %d y %d w %d h %d", __func__, x, y, w, h);

	return SUCCESS;
}

int qt_camera_record_set_saveraw(int cntnum)
{
	emsg.raw_cnt_num = cntnum;
	return SUCCESS;
}

int qt_camera_gas_analit_enable(int enable)
{
	g_printf("%s: enable: %d to %d\n", __func__, emsg.gas_analit_en, enable);

	emsg.gas_analit_en = enable & 0x01;
	if (emsg.gas_analit_en == 1) {
		camera_gas_quantification_start();
	}
	else {
		camera_gas_quantification_stop();
	}

	return SUCCESS;
}
int qt_camera_gas_analit_set_region(int x, int y, int w, int h)
{
	emsg.gas_analit_x = x;
	emsg.gas_analit_y = y;
	emsg.gas_analit_w = w;
	emsg.gas_analit_h = h;
	return SUCCESS;
}
int qt_camera_gas_analit_set_distance(float distance)
{
	emsg.gas_analit_distance = distance;
	return SUCCESS;
}
int qt_camera_gas_analit_set_filterIntensity(unsigned char filterIntensity)
{
	emsg.gas_analit_filterIntensity = filterIntensity;
	return SUCCESS;
}
int qt_camera_gas_analit_set_temperature(int tempMin, int tempMax)
{
	emsg.gas_analit_tempMin = tempMin;
	emsg.gas_analit_tempMax = tempMax;
	return SUCCESS;
}
int qt_camera_gas_analit_set_vendor(int vendor)
{
	g_printf("%s: vendor: %d to %d\n", __func__, emsg.gas_analit_vendor, vendor);
	emsg.gas_analit_vendor = vendor;

	if (emsg.gas_analit_en == 1) {
		emsg.gas_analit_en = 0;
		camera_gas_quantification_stop();
	}

	return SUCCESS;
}
int qt_camera_gas_analit_set_paintedTransparency(unsigned char paintedTransparency)
{
	emsg.gas_analit_paintedTransparency = paintedTransparency;
	return SUCCESS;
}

char *qt_camera_get_version(void)
{
	static char pv[64] = {0};
	sprintf(pv,"%s_%s", RAIN_CAMERA_VERSION, GIT_VERSION);
	return pv;
}
int qt_camera_get_device_model(void)
{
	int device_model = 0;

	return device_model;
}
