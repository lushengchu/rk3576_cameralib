#include "camera_main.h"

/*************************************************
 * Description: 
 * Parameter:
 * Return:
 * Note:
 *************************************************/
static int camera_config_dump(CAMCONFIG *config)
{
	CAMVICONFIG *vitmp = (CAMVICONFIG *)&config->vidvp;
	CAMVECONFIG *vetmp = (CAMVECONFIG *)&config->vedvp;
	g_printf(">>>>>>>>>>>>>  conf %d >>>>>>>>>>>>>>>>>>",emsg.conf_ok);
	printf("dvp w %d h %d fps %d vifmt %d vpfmt %d videp %d vibuf %d vpdep %d\n",vitmp->width,vitmp->height,vitmp->fps,vitmp->vi_format,vitmp->vpss_format,vitmp->vi_depth,vitmp->vi_bufcnt,vitmp->vpss_depth);
	vitmp = (CAMVICONFIG *)&config->vimipi;
	printf("mipi w %d h %d fps %d vifmt %d vpfmt %d videp %d vibuf %d vpdep %d\n",vitmp->width,vitmp->height,vitmp->fps,vitmp->vi_format,vitmp->vpss_format,vitmp->vi_depth,vitmp->vi_bufcnt,vitmp->vpss_depth);
	printf("dvp ve w %d h %d bitrate %d \n",vetmp->width,vetmp->height,vetmp->bitrate);
	//vetmp = (CAMVECONFIG *)&config->vemipi;
	printf("mipi ve bitrate %d \n",config->vemipi_bitrate);
	printf("mipi source  x %d y %d w %d h %d\n",emsg.mipi_source_x,emsg.mipi_source_y,emsg.mipi_source_w,emsg.mipi_source_h);
	g_printf("<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<");
	return 0;
}

int camera_config(CAMCONFIG *config)
{
    int ret = 0;

	memcpy(&camera_conf, config, sizeof(CAMCONFIG));
	if(camera_conf.vidvp.width != 0){
		emsg.conf_ok = 1;
	}else{
		emsg.conf_ok = 0;
	}

	camera_config_dump(&camera_conf);
    return 0;
}
int camera_config_get(CAMCONFIG *config)
{
    if (config == NULL)
        return -1;
	memcpy(config, &camera_conf, sizeof(CAMCONFIG));
    
    return 0;
}

