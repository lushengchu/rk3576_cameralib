#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include "libdrm_macros.h"
#include "camera_main.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_mb.h"
#include "mp4_muxer_ffmpeg.h"
//#include "sys/uio.h"
//#include <drm_fourcc.h>

/* weston 的输出旋转配置, 由 /etc/profile.d/weston.sh 里 WESTON_DRM_CONFIG 指定,
 * weston 会动态 watch 该文件, 形如 "output:all:rotate180" / "output:DSI-1:rotate90" */
#define WESTON_DRM_CONF_PATH "/tmp/.weston_drm.conf"

/* 读 weston 配置里当前的输出旋转角(0/90/180/270)。
 * 读不到文件或没有 rotate 配置时返回 0(weston 默认不转)。 */
static int drm_read_weston_rotate(void)
{
	FILE *fp;
	char line[256];
	int rotate = 0;

	fp = fopen(WESTON_DRM_CONF_PATH, "r");
	if (!fp)
		return 0;
	while (fgets(line, sizeof(line), fp)) {
		char *p = strstr(line, "rotate");
		if (p) {
			int v = atoi(p + 6);
			if (v == 0 || v == 90 || v == 180 || v == 270)
				rotate = v;
		}
	}
	fclose(fp);
	return rotate;
}

/* 返回截图/录屏时 RGA 应该用的旋转角(度)。
 * 目的: 抵消 weston 已做的旋转, 使输出方向与屏幕旋转无关——
 * 即用命令把 weston 转 180° 后, 拍照/录屏画面方向仍保持一致。
 *
 * 注意: 现在 weston 的 fb 实测是横屏 1920x1080(不是旧的竖屏 1088x1920)。
 * rga_cvtcolor_demo 对 90/270 会交换 dst 宽高(输出变竖屏 1080x1920),
 * 其 stride(2160) 与 VENC/stViFrame 按横屏 1920 读取(stride 3840)不匹配,
 * 画面会撕裂/错乱。因此横屏 fb 下只能用整转 0/180(不交换宽高)。
 * 映射: RGA角 = (360 - weston角) % 360   (净旋转恒为 0, 输出方向与屏幕旋转无关)
 *   实测板子常态 weston=180 -> RGA=180 (与之前写死、验证正确的 180 一致)
 *   若命令把 weston 转回 0   -> RGA=0
 * weston=90/270 时 fb 为竖屏, 属另一套几何, 此公式给出 270/90 沿用旧行为。 */
int camera_drm_get_rga_rotate_deg(void)
{
	int deg = (180 - drm_read_weston_rotate()) % 360;
	if (deg < 0)
		deg += 360;
    printf("sclu deg=%d\n", deg);
	return 0;
}

/**********************
  * DRM API
**********************/
static guint gst_kms_src_get_crtc_fb(int fd, guint crtc_id)
{
	drmModeCrtcPtr crtc;
	guint fb_id;

	crtc = drmModeGetCrtc (fd, crtc_id);
	if (!crtc)
		return 0;

	fb_id = crtc->buffer_id;
	//g_printf("%s %d fb_id %d , crtc->buffer_id %d",__func__,__LINE__,fb_id,crtc->buffer_id);

	drmModeFreeCrtc (crtc);
	return fb_id;
}

/* From libdrm 2.4.109 : xf86drm.c */
static int _drmCloseBufferHandle(int fd, uint32_t handle)
{
	struct drm_gem_close args;

	memset(&args, 0, sizeof(args));
	args.handle = handle;
	return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &args);
}

static guint gst_kms_src_find_best_crtc (int fd)
{
	drmModeCrtcPtr crtc;
	drmModeResPtr res;
	guint crtc_id;
	gint i;

	res = drmModeGetResources (fd);
	if (!res)
		return 0;

	for (i = CRTC_INDEX, crtc_id = 0; i < res->count_crtcs; i++) {
		crtc = drmModeGetCrtc (fd, res->crtcs[i]);
		r_printf("res->count_crtcs %d , res->crtcs[i] %d i %d , crtc->mode_valid %d",res->count_crtcs,res->crtcs[i],i,crtc->mode_valid);
		if (crtc && crtc->mode_valid) {
			drmModeFreeCrtc (crtc);
			crtc_id = res->crtcs[i];
			break;
		}
		drmModeFreeCrtc (crtc);
	}
	drmModeFreeResources (res);


	return crtc_id;
}

int drm_crtc_init(CAMDRMINFO *info)
{
	info->fdcard = drmOpen ("rockchip", NULL);
	info->crtc_id = gst_kms_src_find_best_crtc(info->fdcard);
	int fbcrtc = gst_kms_src_get_crtc_fb (info->fdcard,info->crtc_id);
	if (drm_crtc_update_fb(info, fbcrtc) != SUCCESS) {
		r_printf("drm_crtc_update_fb %d err !", fbcrtc);
		return FAILURE;
	}

	/* 输出帧缓冲区信息 */
	printf("Framebuffer ID: %d\n", info->fb2->fb_id);
	printf("Size: %dx%d\n", info->fb2->width, info->fb2->height);
	printf("Pixel_format %x \n", info->fb2->pixel_format);
	printf("Handle: %d\n", info->fb2->handles[0]);

	return SUCCESS;
}

static void drm_fb_cache_release(int fdcard, struct camdrm_fb_cache *c)
{
	if (!c->fb_id && !c->fb2)
		return;
	if (c->vaddr && c->vaddr != MAP_FAILED)
		munmap(c->vaddr, c->size);
	if (c->fb2) {
		for (int i = 0; i < 4; i++) {
			if (c->fb2->handles[i])
				_drmCloseBufferHandle(fdcard, c->fb2->handles[i]);
		}
		drmModeFreeFB2(c->fb2);
	}
	if (c->dmafd > 0)
		close(c->dmafd);
	memset(c, 0, sizeof(*c));
}

/* 按当前fb_id更新屏幕缓冲区映射。
 * weston是多缓冲（2~3个）轮转pageflip的，旋转屏幕/重启合成器后会切换到新的fb，
 * 只在初始化时mmap一次的写法会一直读到旧缓冲（画面冻结、旋转不同步），
 * 录屏循环每帧拿到新的fb_id后要调用本函数更新。缓存多个fb避免每帧重复mmap。 */
int drm_crtc_update_fb(CAMDRMINFO *info, guint fb_id)
{
	int i, slot = -1;
	struct camdrm_fb_cache *c;

	if (fb_id == 0)
		return FAILURE;
	if (fb_id == info->cur_fb_id && info->vaddr)
		return SUCCESS;

	//先找已有缓存（weston的fb在固定几个缓冲间轮转）
	for (i = 0; i < 4; i++) {
		if (info->fbcache[i].fb_id == fb_id) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		//找空槽位，找不到就淘汰一个非当前的槽位重新映射（屏幕旋转后旧fb不再被weston写入）
		for (i = 0; i < 4; i++) {
			if (info->fbcache[i].fb_id == 0) {
				slot = i;
			break;
			}
		}
		if (slot < 0) {
			slot = 0;
			for (i = 0; i < 4; i++) {
				if (info->fbcache[i].fb_id != info->cur_fb_id) {
					slot = i;
					break;
				}
			}
			drm_fb_cache_release(info->fdcard, &info->fbcache[slot]);
		}
		c = &info->fbcache[slot];
		c->fb2 = drmModeGetFB2(info->fdcard, fb_id);
		if (!c->fb2) {
			r_printf("drmModeGetFB2 %d err", fb_id);
			return FAILURE;
		}
		c->dmafd = -1;
		if (drmPrimeHandleToFD(info->fdcard, c->fb2->handles[0], DRM_CLOEXEC | DRM_RDWR,
				&c->dmafd) < 0) {
			r_printf("drmPrimeHandleToFD err !");
			drm_fb_cache_release(info->fdcard, c);
			return FAILURE;
		}
		struct stat stmp;
		fstat(c->dmafd, &stmp);
		c->size = stmp.st_size;
		c->vaddr = mmap(0, c->size, PROT_READ, MAP_SHARED, c->dmafd, 0);
		if (c->vaddr == MAP_FAILED) {
			r_printf("mmap fb %d failed: %s", fb_id, strerror(errno));
			c->vaddr = NULL;
			drm_fb_cache_release(info->fdcard, c);
			return FAILURE;
		}
		c->fb_id = fb_id;
	}

	//顶层字段指向当前缓存槽位，原有代码继续用info->vaddr/info->fb2
	c = &info->fbcache[slot];
	info->vaddr = c->vaddr;
	info->fb2 = c->fb2;
	info->dmafd[0] = c->dmafd;
	info->st[0].st_size = c->size;
	info->cur_fb_id = fb_id;
	return SUCCESS;
}

int drm_crtc_deinit(CAMDRMINFO *info)
{
	int i;
	for (i = 0; i < 4; i++)
		drm_fb_cache_release(info->fdcard, &info->fbcache[i]);
	info->vaddr = NULL;
	info->fb2 = NULL;
	info->cur_fb_id = 0;

	drmClose(info->fdcard);

	return SUCCESS;
}

static guint gst_kms_src_get_next_fb_id (int fd, guint crtc_id)
{
  //GstClockTimeDiff diff;
  //gboolean sync = self->sync_fb && !self->fb_id;
  static RK_U64 last_frame_time = 0;
  RK_U64 duration = 0, diff = 0;
  guint fb_id;

  duration = 33333;

  while (1) {
	  //duration = TEST_COMM_GetNowUs();
	  diff = TEST_COMM_GetNowUs() - last_frame_time;

	  //r_printf("%d fd %d crtc_id %d diff %lu duration %lu last_frame_time %lu flag %d",__LINE__,fd,crtc_id,diff, duration,last_frame_time,(diff >= duration));
	  fb_id = gst_kms_src_get_crtc_fb (fd, crtc_id);
	  if (!fb_id)
		  return 0;

	  if (diff >= duration){
		  last_frame_time = TEST_COMM_GetNowUs();
		  return fb_id;
	  }

	  usleep (1000);
  }

  return 0;
}

static guint gst_kms_src_get_next_fb_id_sync (int fd, guint crtc_id)
{
	guint fb_id = 0;
	guint fb_last_id = gst_kms_src_get_crtc_fb (fd, crtc_id);

	while (1) {

		fb_id = gst_kms_src_get_crtc_fb (fd, crtc_id);

		if (fb_last_id != fb_id){
			return fb_id;
		}

	}
	return 0;
}

/**********************
  * VBLANK TEST
**********************/
//static guint fb_last_id = 0;
guint camera_drm_wait_vblank(CAMDRMINFO *info)
{
  //static RK_U64 last_frame_time = 0;
  //RK_U64 duration = 0, diff = 0;
  guint fb_id = 0;
  guint fb_last_id = gst_kms_src_get_crtc_fb(info->fdcard, info->crtc_id);
  int timeout = 3000; //最长等3s，屏幕静止时fb_id不变，不能死等（否则线程死循环，进程卡死无法退出）

  //duration = 33333;
  while (timeout-- > 0) {

	  //diff = TEST_COMM_GetNowUs() - last_frame_time;
	  fb_id = gst_kms_src_get_crtc_fb (info->fdcard, info->crtc_id);

	  //if (diff >= duration){
		  //last_frame_time = TEST_COMM_GetNowUs();
		  if (fb_last_id != fb_id){
			  return fb_id;
		  }
	  //}

	  usleep (1000);
  }

  //超时兜底：屏幕是静态画面，直接用当前fb抓取即可
  //r_printf("wait vblank timeout, use current fb %d", fb_last_id);
  return fb_last_id;
}

static drmModeCrtcPtr camera_gst_kms_src_get_crtc_fb(int fd, guint crtc_id)
{
	drmModeCrtcPtr crtc;

	crtc = drmModeGetCrtc (fd, crtc_id);
	if (!crtc)
		return 0;

	return crtc;
}

void camera_gst_kms_src_free_crtc_fb(drmModeCrtcPtr crtc)
{
	drmModeFreeCrtc (crtc);
}

drmModeCrtcPtr camera_drm_wait_vblank_2_timeout(CAMDRMINFO *info, int timeout_ms)
{
	guint fb_id = 0;
	drmModeCrtcPtr crtc = camera_gst_kms_src_get_crtc_fb(info->fdcard, info->crtc_id);
	guint fb_last_id = crtc->buffer_id;
	camera_gst_kms_src_free_crtc_fb(crtc);
	int timeout = timeout_ms; //屏幕静止时fb_id不变，不能死等（否则录像/拍照线程死循环，进程卡死无法退出）

	while (timeout-- > 0) {
		crtc = camera_gst_kms_src_get_crtc_fb(info->fdcard, info->crtc_id);
		fb_id = crtc->buffer_id;
		if (fb_last_id != fb_id) {
			// printf("fb_last_id=%d, fb_id=%d, curtime=%s\n", fb_last_id, fb_id, curtime());
			return crtc;
		}
		camera_gst_kms_src_free_crtc_fb(crtc);
		usleep(1000);
	}

	//超时兜底：屏幕是静态画面，直接用当前fb抓取即可（每帧循环调用时timeout要小，否则帧率掉到很低）
//	r_printf("wait vblank timeout %d ms, use current fb %d", timeout_ms, fb_last_id);
	return camera_gst_kms_src_get_crtc_fb(info->fdcard, info->crtc_id);
}

drmModeCrtcPtr camera_drm_wait_vblank_2(CAMDRMINFO *info)
{
	return camera_drm_wait_vblank_2_timeout(info, 3000);
}
/**********************
  * CAMERA test demo
**********************/
#if 0
static int camera_drm_capture(struct cam_context *msg)
{
	CAMDRMINFO drminfo;
	memset(&drminfo, 0x0 , sizeof(CAMDRMINFO));

	struct Mpp_venc_api e_api = msg->cam_api;
	VencContext eContext;
	memset(&eContext, 0, sizeof(VencContext));
    VENC_STREAM_S stFrame;
	VIDEO_FRAME_INFO_S stViFrame;
	RK_S32 s32Ret = 0;

	memset(&stViFrame, 0x0 ,sizeof(VIDEO_FRAME_INFO_S));
	stFrame.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));

	//drm init
	drm_crtc_init(&drminfo);

	// ve init
	e_api.ve_init(&eContext, VENC_DRM_JPG);
	e_api.create_ve(&eContext);
	e_api.start_ve(&eContext);
	//RGBA8888
	int len = eContext.width * eContext.height * 4;
	// for input image pool
	MB_POOL_CONFIG_S stMbPoolCfg;
	memset(&stMbPoolCfg, 0, sizeof(MB_POOL_CONFIG_S));
    stMbPoolCfg.u64MBSize = len;
    stMbPoolCfg.u32MBCnt  = 1;
    stMbPoolCfg.enAllocType = MB_ALLOC_TYPE_DMA;
    stMbPoolCfg.bPreAlloc = RK_TRUE;
    MB_POOL pool = RK_MPI_MB_CreatePool(&stMbPoolCfg);
	MB_BLK blk = RK_MPI_MB_GetMB(pool, len, RK_TRUE);
	stViFrame.stVFrame.pMbBlk = blk;
	stViFrame.stVFrame.u32Width = eContext.width;
	stViFrame.stVFrame.u32Height = eContext.height;
	stViFrame.stVFrame.u32VirWidth = eContext.width;
	stViFrame.stVFrame.u32VirHeight = eContext.height;
	stViFrame.stVFrame.enPixelFormat = (PIXEL_FORMAT_E)eContext.enPixelFormat;
	//stViFrame.stVFrame.u32FrameFlag |= s32ReachEOS ? FRAME_FLAG_SNAP_END : 0;
	stViFrame.stVFrame.enCompressMode = COMPRESS_MODE_NONE;

	RK_U8 *pVirAddr = (RK_U8 *)(RK_MPI_MB_Handle2VirAddr(blk));
	memcpy(pVirAddr, drminfo.vaddr, drminfo.st[0].st_size);
	RK_MPI_SYS_MmzFlushCache(blk, RK_FALSE);

	//capture
	s32Ret = RK_MPI_VENC_SendFrame(eContext.veChn, &stViFrame, -1);
	printf("RK_MPI_VENC_SendFrame %x\n",s32Ret);
	s32Ret = RK_MPI_VENC_GetStream(eContext.veChn, &stFrame, -1);
	printf("RK_MPI_VENC_GetStream %x\n",s32Ret);
	void *pData= RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);

	FILE *fp = fopen("/tmp/dma_data.bin", "wb+");
	fwrite(pData, 1, stFrame.pstPack->u32Len, fp);
	fflush(fp);
	fclose(fp);

	free(stFrame.pstPack);
	drm_crtc_deinit(&drminfo);
	e_api.stop_ve(&eContext);
	e_api.destroy_ve(&eContext);
	s32Ret = RK_MPI_MB_ReleaseMB(blk);
	printf("RK_MPI_MB_ReleaseMB %d\n",s32Ret);
	s32Ret = RK_MPI_MB_DestroyPool(pool);
	printf("RK_MPI_MB_DestroyPool %d\n",s32Ret);
	return 0;
}   

static int camera_drm_mp4_use_fd_id(struct cam_context *msg)
{
	drm_public int fdcard;
	char *vaddr;
	struct stat st[4];
	gint i,dmafd[4], size[4];

	//time test
	RK_U64 startUs = TEST_COMM_GetNowUs();

	fdcard = drmOpen ("rockchip", NULL);
	g_printf("fd %d",fdcard);
	guint crtc_id = gst_kms_src_find_best_crtc(fdcard);

	RK_U64 s2Us = (TEST_COMM_GetNowUs() - startUs)/1000;
	g_printf("drm init  time %lu ms",s2Us);

	// ve test
	RK_S32 s32Ret = 0;
	//VencContext *eContext = (VencContext *)&msg->camVeContext;
	struct Mpp_venc_api e_api = msg->cam_api;
	VencContext eContext;
	memset(&eContext, 0, sizeof(VencContext));
	e_api.ve_init(&eContext, VENC_DRM_H264);
	e_api.create_ve(&eContext);
	e_api.start_ve(&eContext);

	RK_U64 s3Us = (TEST_COMM_GetNowUs() - startUs)/1000 - s2Us;
	g_printf("ve time %lu ms",s3Us);

	//mp4 mux
	static MUX_INFO mux_drm;
	memset(&mux_drm, 0x0 , sizeof(MUX_INFO));
	mux_drm.ai_enable = 0;
	mux_drm.w = 1350;
	mux_drm.h = 1080;
	mux_drm.fps = 25;
	MP4_Muxer_Create(&mux_drm,"/tmp/dma_data.mp4");

	//RGBA8888
	int len = eContext.width * eContext.height * 4;

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

	RK_U64 s1Us = (TEST_COMM_GetNowUs() - startUs)/1000 - s2Us - s3Us;
	g_printf("mb time %lu ms",s1Us);

	//vaddr = mmap(pVirAddr, st[0].st_size, PROT_READ, MAP_SHARED, dmafd[0], 0);

    VENC_STREAM_S stFrame;
	stFrame.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));

	//RK_MPI_SYS_MmzFlushCache(blk, RK_FALSE);

	VIDEO_FRAME_INFO_S stViFrame;
	memset(&stViFrame, 0x0 ,sizeof(VIDEO_FRAME_INFO_S));
	stViFrame.stVFrame.pMbBlk = blk;
	//stViFrame.stVFrame.pMbBlk = RK_MPI_MB_VirAddr2Handle(vaddr);
	//stViFrame.stVFrame.pMbBlk = vaddr;
	stViFrame.stVFrame.u32Width = eContext.width;
	stViFrame.stVFrame.u32Height = eContext.height;
	stViFrame.stVFrame.u32VirWidth = eContext.width;
	stViFrame.stVFrame.u32VirHeight = eContext.height;
	stViFrame.stVFrame.enPixelFormat = (PIXEL_FORMAT_E)eContext.enPixelFormat;
	//stViFrame.stVFrame.u32FrameFlag |= s32ReachEOS ? FRAME_FLAG_SNAP_END : 0;
	stViFrame.stVFrame.enCompressMode = COMPRESS_MODE_NONE;
	//stViFrame.stVFrame.u64PTS = TEST_COMM_GetNowUs();

	RK_U64 s0Us = (TEST_COMM_GetNowUs() - startUs)/1000 - s2Us - s1Us - s3Us;
	g_printf("st time %lu ms",s0Us);

	/* int fbcrtc = gst_kms_src_get_crtc_fb (fdcard,crtc_id); */
	/* r_printf("fdcard %d crtc_id %d crtc %d",fdcard,crtc_id,fbcrtc); */
	/* drmModeFB2Ptr fb2 = drmModeGetFB2 (fdcard,fbcrtc); */
	drmModeFB2Ptr fb2 = NULL;
	///* 输出帧缓冲区信息 */
	/* printf("Framebuffer ID: %d\n", fb2->fb_id); */
	/* printf("Size: %dx%d\n", fb2->width, fb2->height); */
	/* printf("Pixel_format %x \n", fb2->pixel_format); */
	/* printf("Handle: %d\n", fb2->handles[0]); */


	int delay = 30*30;
	int frm_cnt = 0;
	//FILE *fp = fopen("/tmp/dma_data.bin", "wb+");
	//int file_fd = open("/tmp/dma_data.bin", O_CREAT|O_WRONLY, 0644);
	RK_U64 s4Us = (TEST_COMM_GetNowUs() - startUs)/1000;
	RK_U64 s6Us,s7Us;
	RK_U64 s11Us,s12Us;
#if 1
	int fbcrtc = gst_kms_src_get_crtc_fb (fdcard,crtc_id);
	r_printf("fdcard %d crtc_id %d crtc %d",fdcard,crtc_id,fbcrtc);
	fb2 = drmModeGetFB2 (fdcard,fbcrtc);

	/* 输出帧缓冲区信息 */
	printf("Framebuffer ID: %d\n", fb2->fb_id);
	printf("Size: %dx%d\n", fb2->width, fb2->height);
	printf("Pixel_format %x \n", fb2->pixel_format);
	printf("Handle: %d\n", fb2->handles[0]);

	i = 0;
	size[i] = fb2->pitches[i] * fb2->height;

	if (drmPrimeHandleToFD (fdcard, fb2->handles[i], DRM_CLOEXEC | DRM_RDWR,
				&dmafd[i]) < 0) {
		r_printf("drmPrimeHandleToFD err !");
	}

	fstat (dmafd[i], &st[i]);
	g_printf("%s %d ,i %d, size[i] %d,st[i].st_size %ld",__func__,__LINE__,i,size[i],st[i].st_size);

	//vaddr = mmap(0, st[0].st_size, PROT_READ, MAP_SHARED, dmafd[0], 0);
	vaddr = mmap(0, st[0].st_size, PROT_READ, MAP_PRIVATE, dmafd[0], 0);
	if (vaddr == MAP_FAILED) {
		r_printf("failed mmap to mmap buffer: %s\n", strerror(errno));
	}

#endif
	while(delay--)
	{
#if 1
		s6Us = (TEST_COMM_GetNowUs() - startUs)/1000;
		int fbcrtc_last = gst_kms_src_get_next_fb_id_sync(fdcard,crtc_id);
		frm_cnt++;
		memcpy(pVirAddr, vaddr, st[0].st_size);
		stViFrame.stVFrame.u64PTS = TEST_COMM_GetNowUs()/1000;
		//g_printf("PTS %lu",stViFrame.stVFrame.u64PTS );
		//RK_MPI_SYS_MmzFlushCache(blk, RK_FALSE);
		RK_MPI_SYS_MmzFlushCache(blk, RK_TRUE);
		s32Ret = RK_MPI_VENC_SendFrame(eContext.veChn, &stViFrame, -1);
		//printf("RK_MPI_VENC_SendFrame %d\n",s32Ret);
		s32Ret = RK_MPI_VENC_GetStream(eContext.veChn, &stFrame, -1);
		//printf("RK_MPI_VENC_GetStream %d\n",s32Ret);
		void *pData= RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
		//RK_MPI_SYS_MmzFlushCache(stFrame.pstPack->pMbBlk, RK_TRUE);
		//fwrite(pData, 1, stFrame.pstPack->u32Len, fp);
		MP4_Muxer_WriteVideo(&mux_drm, (unsigned char *)pData, (unsigned int)stFrame.pstPack->u32Len, stFrame.pstPack->u64PTS);
		//fflush(fp);
		RK_MPI_VENC_ReleaseStream(eContext.veChn, &stFrame);
		s7Us = (TEST_COMM_GetNowUs() - startUs)/1000;
		/* if((s7Us-s6Us)<=30){ */
		/* usleep((30-(s7Us-s6Us))*1000); */
		/* s7Us = (TEST_COMM_GetNowUs() - startUs)/1000; */
		/* } */
		g_printf("deal 1 frm use %lu ms",s7Us-s6Us);
		//munmap(vaddr, st[0].st_size);

		/* for (i = 0; i < 4; i++) { */
			/* if (fb2->handles[i]) */
				/* _drmCloseBufferHandle (fdcard, fb2->handles[i]); */
		/* } */
		/* drmModeFreeFB2 (fb2); */

#else

		s11Us = TEST_COMM_GetNowUs()/1000;
		int fbcrtc = gst_kms_src_get_next_fb_id_sync (fdcard,crtc_id);
		s6Us = TEST_COMM_GetNowUs()/1000;
		r_printf("fdcard %d crtc_id %d crtc %d  use %lu ms",fdcard,crtc_id,fbcrtc,s6Us-s11Us);
		fb2 = drmModeGetFB2 (fdcard,fbcrtc);

		///////////* 输出帧缓冲区信息 */
		printf("Framebuffer ID: %d\n", fb2->fb_id);
		printf("Size: %dx%d\n", fb2->width, fb2->height);
		printf("Pixel_format %x \n", fb2->pixel_format);
		printf("Handle: %d\n", fb2->handles[0]);

		i = 0;
		size[i] = fb2->pitches[i] * fb2->height;

		g_printf("fdcard %d handle %d",fdcard,fb2->handles[i]);
		if (drmPrimeHandleToFD (fdcard, fb2->handles[i], DRM_CLOEXEC | DRM_RDWR,
					&dmafd[i]) < 0) {
			r_printf("drmPrimeHandleToFD err !");
		}

		fstat (dmafd[i], &st[i]);
		g_printf("%s %d ,i %d, size[i] %d,st[i].st_size %ld , fd %d dmafd %d",__func__,__LINE__,i,size[i],st[i].st_size,fdcard,dmafd[i]);

		//vaddr = mmap(0, st[0].st_size, PROT_READ, MAP_SHARED, dmafd[0], 0);
		vaddr = mmap(0, st[0].st_size, PROT_READ, MAP_PRIVATE, dmafd[0], 0);
		if (vaddr == MAP_FAILED) {
			r_printf("failed mmap to mmap buffer: %s\n", strerror(errno));
		}

		frm_cnt++;
		s7Us = TEST_COMM_GetNowUs()/1000;
		g_printf("get line %d use %lu ms",__LINE__,s7Us-s6Us);
		memcpy(pVirAddr, vaddr, st[0].st_size);
		/* s7Us = TEST_COMM_GetNowUs()/1000; */
		/* g_printf("copy line %d use %lu ms",__LINE__,s7Us-s6Us); */

		//stViFrame.stVFrame.u64PTS = TEST_COMM_GetNowUs();
		RK_MPI_SYS_MmzFlushCache(blk, RK_TRUE);
		s32Ret = RK_MPI_VENC_SendFrame(eContext.veChn, &stViFrame, -1);
		s32Ret = RK_MPI_VENC_GetStream(eContext.veChn, &stFrame, -1);
		void *pData= RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
		/* struct iovec vecs = {0}; */
		/* vecs.iov_base = vaddr; */
		/* vecs.iov_len = st[0].st_size; */

		//memcpy(pVirAddr, vaddr, st[0].st_size);
		//RK_MPI_SYS_MmzFlushCache(blk, RK_FALSE);
		//fwrite(pVirAddr, 1, st[0].st_size, fp);
		//fwrite(vaddr, 1, st[0].st_size, fp);
		fwrite(pData, 1, stFrame.pstPack->u32Len, fp);
		fflush(fp);
		//int written = writev(file_fd, &vecs, 1);
		//r_printf("w len %d",written);
		
		RK_MPI_VENC_ReleaseStream(eContext.veChn, &stFrame);
		munmap(vaddr, st[0].st_size);

		for (i = 0; i < 4; i++) {
			if (fb2->handles[i])
				_drmCloseBufferHandle (fdcard, fb2->handles[i]);
		}
		drmModeFreeFB2 (fb2);
		i = 0;
		close(dmafd[i]);

		s12Us = TEST_COMM_GetNowUs()/1000;
		g_printf("deal 1 frm line %d use %lu ms",__LINE__,s12Us-s11Us);
#endif
	}
	RK_U64 s5Us = (TEST_COMM_GetNowUs() - startUs)/1000;
	g_printf("ok, frm_num %d  fps %lu ms %lu",frm_cnt,frm_cnt*1000/(s5Us-s4Us),s5Us-s4Us);

	//close(file_fd);
	//fclose(fp);
	MP4_Muxer_Close(&mux_drm);
	free(stFrame.pstPack);

	munmap(vaddr, st[0].st_size);
	for (i = 0; i < 4; i++) {
		if (fb2->handles[i])
			_drmCloseBufferHandle (fdcard, fb2->handles[i]);
	}
	drmModeFreeFB2 (fb2);

	drmClose(fdcard);

	s32Ret = RK_MPI_MB_ReleaseMB(blk);
	printf("RK_MPI_MB_ReleaseMB %d\n",s32Ret);
	s32Ret = RK_MPI_MB_DestroyPool(pool);
	printf("RK_MPI_MB_DestroyPool %d\n",s32Ret);

	RK_U64 endMs = (TEST_COMM_GetNowUs() - startUs)/1000;
	g_printf("drm test end time %lu ms",endMs);

	return 0;
} 

static int camera_drm_mp4_use_echo_dump(struct cam_context *msg)
{
	drm_public int fdcard;
	char *vaddr;
	//struct stat st[4];
	gint i,dmafd[4], size[4];

	//time test
	RK_U64 startUs = TEST_COMM_GetNowUs();

	// ve test
	RK_S32 s32Ret = 0;
	//VencContext *eContext = (VencContext *)&msg->camVeContext;
	struct Mpp_venc_api e_api = msg->cam_api;
	VencContext eContext;
	memset(&eContext, 0, sizeof(VencContext));
	e_api.ve_init(&eContext, VENC_DRM_H264);
	e_api.create_ve(&eContext);
	e_api.start_ve(&eContext);

	//mp4 mux
	static MUX_INFO mux_drm;
	memset(&mux_drm, 0x0 , sizeof(MUX_INFO));
	mux_drm.ai_enable = 0;
	mux_drm.w = 1350;
	mux_drm.h = 1080;
	mux_drm.fps = 22;
	MP4_Muxer_Create(&mux_drm,"/tmp/dma_data.mp4");

	//RGBA8888
	int len = eContext.width * eContext.height * 4;

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

	//vaddr = mmap(pVirAddr, st[0].st_size, PROT_READ, MAP_SHARED, dmafd[0], 0);

    VENC_STREAM_S stFrame;
	stFrame.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));

	//RK_MPI_SYS_MmzFlushCache(blk, RK_FALSE);

	VIDEO_FRAME_INFO_S stViFrame;
	memset(&stViFrame, 0x0 ,sizeof(VIDEO_FRAME_INFO_S));
	stViFrame.stVFrame.pMbBlk = blk;
	//stViFrame.stVFrame.pMbBlk = RK_MPI_MB_VirAddr2Handle(vaddr);
	//stViFrame.stVFrame.pMbBlk = vaddr;
	stViFrame.stVFrame.u32Width = eContext.width;
	stViFrame.stVFrame.u32Height = eContext.height;
	stViFrame.stVFrame.u32VirWidth = eContext.width;
	stViFrame.stVFrame.u32VirHeight = eContext.height;
	stViFrame.stVFrame.enPixelFormat = (PIXEL_FORMAT_E)eContext.enPixelFormat;
	//stViFrame.stVFrame.u32FrameFlag |= s32ReachEOS ? FRAME_FLAG_SNAP_END : 0;
	stViFrame.stVFrame.enCompressMode = COMPRESS_MODE_NONE;
	//stViFrame.stVFrame.u64PTS = TEST_COMM_GetNowUs();

	int delay = 30*15;
	int frm_cnt = 0;
	//FILE *fp = fopen("/tmp/dma_data.bin", "wb+");
	//int file_fd = open("/tmp/dma_data.bin", O_CREAT|O_WRONLY, 0644);
	RK_U64 s6Us,s7Us;
	RK_U64 s11Us,s12Us;
	while(delay--)
	{
#if 0
		s6Us = (TEST_COMM_GetNowUs() - startUs)/1000;
		system("echo dump > /sys/kernel/debug/dri/0/video_port3/vop_dump/dump");
		int fd = open("/data/dump.bin", O_RDONLY, 0644);
		truncate("/data/dump.bin",len);
		vaddr = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
		frm_cnt++;
		memcpy(pVirAddr, vaddr, len);
		stViFrame.stVFrame.u64PTS = TEST_COMM_GetNowUs()/1000;
		//g_printf("PTS %lu",stViFrame.stVFrame.u64PTS );
		//RK_MPI_SYS_MmzFlushCache(blk, RK_FALSE);
		RK_MPI_SYS_MmzFlushCache(blk, RK_TRUE);
		s32Ret = RK_MPI_VENC_SendFrame(eContext.veChn, &stViFrame, -1);
		//printf("RK_MPI_VENC_SendFrame %d\n",s32Ret);
		s32Ret = RK_MPI_VENC_GetStream(eContext.veChn, &stFrame, -1);
		//printf("RK_MPI_VENC_GetStream %d\n",s32Ret);
		void *pData= RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
		//RK_MPI_SYS_MmzFlushCache(stFrame.pstPack->pMbBlk, RK_TRUE);
		fwrite(pData, 1, stFrame.pstPack->u32Len, fp);
		fflush(fp);
		RK_MPI_VENC_ReleaseStream(eContext.veChn, &stFrame);
		s7Us = (TEST_COMM_GetNowUs() - startUs)/1000;
		/* if((s7Us-s6Us)<=30){ */
		/* usleep((30-(s7Us-s6Us))*1000); */
		/* s7Us = (TEST_COMM_GetNowUs() - startUs)/1000; */
		/* } */
		munmap(vaddr, len);
		close(fd);
		g_printf("deal 1 frm use %lu ms",s7Us-s6Us);
#else
		s6Us = TEST_COMM_GetNowUs()/1000;
		system("echo dump > /sys/kernel/debug/dri/0/video_port3/vop_dump/dump");
		int fd = open("/data/dump.bin", O_RDONLY, 0644);
		int read_len = read(fd, pVirAddr, len);
		s7Us = TEST_COMM_GetNowUs()/1000;
		g_printf("read use %lu ms",s7Us-s6Us);
		frm_cnt++;
		stViFrame.stVFrame.u64PTS = TEST_COMM_GetNowUs()/1000;
		//g_printf("PTS %lu",stViFrame.stVFrame.u64PTS );
		//RK_MPI_SYS_MmzFlushCache(blk, RK_FALSE);
		RK_MPI_SYS_MmzFlushCache(blk, RK_TRUE);
		s32Ret = RK_MPI_VENC_SendFrame(eContext.veChn, &stViFrame, -1);
		//printf("RK_MPI_VENC_SendFrame %d\n",s32Ret);
		s32Ret = RK_MPI_VENC_GetStream(eContext.veChn, &stFrame, -1);
		//printf("RK_MPI_VENC_GetStream %d\n",s32Ret);
		void *pData= RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
		MP4_Muxer_WriteVideo(&mux_drm, (unsigned char *)pData, (unsigned int)stFrame.pstPack->u32Len, stFrame.pstPack->u64PTS);
		//RK_MPI_SYS_MmzFlushCache(stFrame.pstPack->pMbBlk, RK_TRUE);
		//fwrite(pData, 1, stFrame.pstPack->u32Len, fp);
		//fflush(fp);
		RK_MPI_VENC_ReleaseStream(eContext.veChn, &stFrame);
		/* if((s7Us-s6Us)<=30){ */
		/* usleep((30-(s7Us-s6Us))*1000); */
		/* s7Us = (TEST_COMM_GetNowUs() - startUs)/1000; */
		/* } */
		close(fd);

		s11Us = TEST_COMM_GetNowUs()/1000;
		g_printf("deal frm  use %lu ms",s11Us-s6Us);
#endif
	}

	//fclose(fp);
	MP4_Muxer_Close(&mux_drm);
	free(stFrame.pstPack);

	s32Ret = RK_MPI_MB_ReleaseMB(blk);
	printf("RK_MPI_MB_ReleaseMB %d\n",s32Ret);
	s32Ret = RK_MPI_MB_DestroyPool(pool);
	printf("RK_MPI_MB_DestroyPool %d\n",s32Ret);

	e_api.stop_ve(&eContext);
	e_api.destroy_ve(&eContext);

	RK_U64 endMs = (TEST_COMM_GetNowUs() - startUs)/1000;
	g_printf("ok, frm_num %d  fps %lu ms %lu",frm_cnt,frm_cnt*1000/endMs,endMs);

	return 0;
} 
#endif