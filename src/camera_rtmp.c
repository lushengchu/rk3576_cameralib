#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "camera_main.h"

/**********************
  * RTMP
**********************/

static pthread_t rtmp_pthd = 0;
#if 0
int camera_rtmp_run(struct cam_context *msg)
{
	char cmd[300] = {0x0};
	camera_rtsp_start(msg);
	snprintf(cmd, sizeof(cmd),"%s %s",RTMP_PATH_LIVE,emsg.rtmp_url);
	g_printf("system > %s",cmd);
	int pid_sta = system(cmd);
	if(emsg.cam_sta == CAM_CAMERA_RTMPSTART){
		emsg.cam_sta = ERR_RTMP_INVALID_URL;
		camera_update_qt_sta();
	}
	//g_printf("ffmpeg off pid sta %d",pid_sta);
	camera_rtsp_stop(msg);
	return 0;
}
#else
static void *camera_rtmp_run(void *p)
{
	char cmd[300] = {0x0};
	snprintf(cmd, sizeof(cmd),"%s \"%s\"",RTMP_PATH_LIVE,emsg.rtmp_url);
	g_printf("system > %s",cmd);
	int pid_sta = system(cmd);
	if(emsg.cam_sta != CAM_CAMERA_RTMPSTOP){
		//camera_update_qt_sta(ERR_RTMP_INVALID_URL);
	}
	return 0;
}

int camera_rtmp_start(struct cam_context *msg)
{
	if(!emsg.rtsp_en){
		camera_rtsp_start(msg);
	}
	pthread_create(&rtmp_pthd, NULL ,camera_rtmp_run, NULL);
	return 0;
}

int camera_rtmp_stop(struct cam_context *msg)
{
	system(RTMP_PATH_OFF);
	pthread_join(rtmp_pthd, NULL);
	if(!emsg.rtsp_en){
		camera_rtsp_stop(msg);
	}
	return 0;
}
#endif

