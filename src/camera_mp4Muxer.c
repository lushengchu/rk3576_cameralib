#include <pthread.h>
#include <string.h>
#include <sys/stat.h>
#include "mp4_muxer_ffmpeg.h"
#include "camera_main.h"

#define MUX_LOG_ENABLE
#ifdef MUX_LOG_ENABLE
#define mux_log(x, ...)  printf("[FFMPEG_MUX] " x "\n",## __VA_ARGS__)
#define mux_err(x, ...)  printf("[FFMPEG_MUX] {ERROR} " x "\n",## __VA_ARGS__)
#endif

/* A方案：硬件时间戳跳变钳制阈值(us)。
 * 视频/音频每帧 pts 由 (硬件时间戳 - shared_base_ts) 算出，而硬件时间戳(VI/VENC u64PTS、
 * AENC u64TimeStamp)与 CLOCK_MONOTONIC 基准不保证同源；一旦某帧时间戳跳变(残留/错值)，
 * time_diff 会瞬间变成十几秒，再被"强制单调递增"锁存，把整条轨道拉长——表现为
 * "只录了6秒、文件却是20秒、播放到6秒后画面静止"。
 * 相邻两帧原始时间戳间隔超过该阈值即判为 glitch，用标称帧率重新推算本帧时间戳。
 * 1秒远高于正常丢帧造成的间隔，又能拦掉异常大跳变。 */
#define MUX_MAX_VIDEO_GAP_US  1000000ULL
#define MUX_MAX_AUDIO_GAP_US  1000000ULL

/* Add an output stream */
static int MP4_Muxer_Add_Stream(MUX_INFO *info)
{
    AVOutputFormat *pOutFmt = NULL;
    AVStream *pAVStream = NULL;
    AVStream *aAVStream = NULL;
    AVCodec *pcodec = NULL;
    AVCodec *acodec = NULL;


    pOutFmt = info->g_OutFmt_Ctx->oformat;

    /* find the encoder */
    pcodec = avcodec_find_encoder(pOutFmt->video_codec);
    if (NULL == pcodec)
    {
        mux_err("could not find encoder for '%s' \n", avcodec_get_name(pOutFmt->video_codec));
        return -1;
    }

    pAVStream = avformat_new_stream(info->g_OutFmt_Ctx, pcodec);
    if (NULL == pAVStream)
    {
       mux_err("could not allocate stream \n");
       return -1;
    }

    pAVStream->id = info->g_OutFmt_Ctx->nb_streams-1;

	mux_log("max_interleave_delta %ld",info->g_OutFmt_Ctx->max_interleave_delta);

	if(info->ai_enable){
		acodec = avcodec_find_encoder(pOutFmt->audio_codec);
		aAVStream = avformat_new_stream(info->g_OutFmt_Ctx, acodec);
		if (NULL == aAVStream)
		{
			mux_err("Muxing:could not allocate acodec stream \n");
			return -1;
		}
		aAVStream->id = info->g_OutFmt_Ctx->nb_streams-1;
		//获取音频流的索引号
		info->ai = aAVStream->index;
	}
    info->vi = pAVStream->index;

    if(pcodec->type == AVMEDIA_TYPE_VIDEO)//编码器编码的数据类型
    {
        mux_log("AVMEDIA_TYPE_VIDEO\n");
        pAVStream->codecpar->codec_id = AV_CODEC_ID_H264;
        pAVStream->codecpar->bit_rate =2000;//2000 (值越大，MP4时长越长)好像不影响(1280 * 2)+ (1024*30/30)=3584
        /* 1280x516 配下去，编码器实际 SPS 是 640x512。容器若写 1280x516，播放器按这个尺寸协商失败。 */
        if(info->w == 1280 && info->h == 516){
            info->w = 640;
            info->h = 512;
        }
        pAVStream->codecpar->width = info->w;
        pAVStream->codecpar->height = info->h;
        pAVStream->codecpar->format = AV_PIX_FMT_YUV420P;
        pAVStream->codec->width = info->w;
        pAVStream->codec->height = info->h;
		//pAVStream->codec->time_base = (AVRational){1, 30}; //fps 30
		pAVStream->codec->time_base = (AVRational){1, info->fps};
		pAVStream->codec->codec_id = AV_CODEC_ID_H264;
        pAVStream->time_base = (AVRational){1, 90000};
    }

	if(info->ai_enable){
		if(acodec && acodec->type == AVMEDIA_TYPE_AUDIO){
			int sample_rate = info->audio_sample_rate > 0 ? info->audio_sample_rate : 48000;
			int frame_samples = info->audio_frame_samples > 0 ? info->audio_frame_samples : AUDIO_LEN;
			int channels = info->audio_channels == 1 ? 1 : 2;
			uint64_t layout = (channels == 1) ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
			mux_log("AVMEDIA_TYPE_AUDIO rate=%d frame=%d ch=%d\n", sample_rate, frame_samples, channels);
			/* 参数写在 codecpar 和 codec 上。这份 ffmpeg 写包时读 AVStream.codec。 */
			aAVStream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
			aAVStream->codecpar->codec_id = AV_CODEC_ID_MP3;
			aAVStream->codecpar->bit_rate = 160000;
			aAVStream->codecpar->sample_rate = sample_rate;
			aAVStream->codecpar->channel_layout = layout;
			aAVStream->codecpar->channels = channels;
			aAVStream->codecpar->frame_size = frame_samples;
			if(aAVStream->codec){
				aAVStream->codec->codec_type = AVMEDIA_TYPE_AUDIO;
				aAVStream->codec->codec_id = AV_CODEC_ID_MP3;
				aAVStream->codec->bit_rate = 160000;
				aAVStream->codec->sample_rate = sample_rate;
				aAVStream->codec->channel_layout = layout;
				aAVStream->codec->channels = channels;
				aAVStream->codec->frame_size = frame_samples;
				aAVStream->codec->time_base = (AVRational){1, sample_rate};
			}
			aAVStream->time_base = (AVRational){1, sample_rate};
		}
	}

    if (info->g_OutFmt_Ctx->oformat->flags & AVFMT_GLOBALHEADER)
    {
        info->g_OutFmt_Ctx->oformat->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }


    return MUX_SUCCESS;
}

int MP4_Muxer_Create(MUX_INFO *info ,char *mp4filename)
{
    int ret = 0;
    char pszFileName[256] = {0};
    AVOutputFormat *pOutFmt = NULL;
	sprintf(pszFileName,"%s",mp4filename);

	info->vi = -1;
	info->ai = -1;
    //av_register_all();

    avformat_alloc_output_context2(&info->g_OutFmt_Ctx, NULL, NULL, pszFileName);
	mux_log("create %s",pszFileName);

    if (NULL == info->g_OutFmt_Ctx)
    {	//try default
      mux_err("Could not deduce output format from file extension: using mp4. \n");
      avformat_alloc_output_context2(&info->g_OutFmt_Ctx, NULL, "mp4", pszFileName);
      if (NULL == info->g_OutFmt_Ctx)
      {
          mux_err("avformat_alloc_output_context2 failed  \n");
          return -1;
      }
    }

    pOutFmt = info->g_OutFmt_Ctx->oformat;
    if (pOutFmt->video_codec == AV_CODEC_ID_NONE)
    {
      mux_err("add_vstream ID =%d \n",pOutFmt->video_codec);
      goto exit_outFmt_failed;
    }
    if (pOutFmt->audio_codec == AV_CODEC_ID_NONE)
    {
      mux_err("add_astream ID =%d \n",pOutFmt->audio_codec);
      goto exit_outFmt_failed;
    }

    //ret = MP4_Muxer_Add_Stream(info->g_OutFmt_Ctx);
    ret = MP4_Muxer_Add_Stream(info);
    if(ret <0)
    {
       mux_err(" HI_PDT_Add_Stream Failed \n");
      goto exit_outFmt_failed;
    }

    mux_log("==========Output Information==========\n");
    av_dump_format(info->g_OutFmt_Ctx, 0, pszFileName, 1);

    /* open the output file, if needed */
    if (!(pOutFmt->flags & AVFMT_NOFILE))
    {
      ret = avio_open(&info->g_OutFmt_Ctx->pb, pszFileName, AVIO_FLAG_WRITE);
      if (ret < 0)
      {
          mux_err("could not open %s\n", pszFileName);
          goto exit_avio_open_failed;
      }
    }

    /* Write the stream header, if any */
    ret = avformat_write_header(info->g_OutFmt_Ctx, NULL);
    if (ret < 0)
    {
      mux_err("Error occurred when opening output file \n");
      goto exit_writeheader_failed;
    }

    info->b_First_IDR_Find = MUX_FALSE;
    info->write_frame_cnt = 0;
    info->first_write_pts = 0;
    info->last_write_pts = 0;
    pthread_mutex_init(&info->write_lock, NULL);
    //mux_log("[MUX-DEBUG] info=%p Create: write_frame_cnt=%llu, first_write_pts=%llu, last_write_pts=%llu",
    //    info,
    //    (unsigned long long)info->write_frame_cnt,
    //    (unsigned long long)info->first_write_pts,
    //    (unsigned long long)info->last_write_pts);
    if (info->vi >= 0 && info->g_OutFmt_Ctx && info->vi < (int)info->g_OutFmt_Ctx->nb_streams) {
        AVStream *vst = info->g_OutFmt_Ctx->streams[info->vi];
        //mux_log("[MUX-DEBUG] info=%p Create: video_stream time_base=%d/%d, codec_time_base=%d/%d",
        //    info, vst->time_base.num, vst->time_base.den,
        //    vst->codec->time_base.num, vst->codec->time_base.den);
    }

    return MUX_SUCCESS;

exit_writeheader_failed:
exit_avio_open_failed:
    if (info->g_OutFmt_Ctx && !(info->g_OutFmt_Ctx->flags & AVFMT_NOFILE))
      avio_close(info->g_OutFmt_Ctx->pb);
exit_outFmt_failed:
    if(NULL != info->g_OutFmt_Ctx)
    {
        mux_err("avformat_free_context(g_OutFmt_Ctx);247 exection\n");
        avformat_free_context(info->g_OutFmt_Ctx);
        info->g_OutFmt_Ctx = NULL;
    }
    return -1;
}

static int write_one_audio_frame(MUX_INFO *info, AVStream *Apst, const unsigned char *data, int len, MUX_U64 a_ts)
{
	AVPacket apkt;
	int ret;
	int64_t time_diff;
	int frame_samples;

	memset(&apkt, 0, sizeof(apkt));
	/* 拷进带填充的 AVPacket。直接挂 AENC 指针时，这份 ffmpeg 不认这个包，音轨不会进文件。 */
	if(av_new_packet(&apkt, len) < 0){
		mux_err("Muxing:av_new_packet %d failed\n", len);
		info->audio_pkt_fail++;
		return MUX_FAILURE;
	}
	memcpy(apkt.data, data, len);

	time_diff = (int64_t)(a_ts - info->Audio_PTS);
	if(time_diff < 0)
		time_diff = 0;
	apkt.pts = av_rescale_q(time_diff, (AVRational){1, 1000000}, Apst->time_base);

	frame_samples = info->audio_frame_samples > 0 ? info->audio_frame_samples : AUDIO_LEN;
	if(Apst->codecpar == NULL || Apst->codecpar->sample_rate <= 0 || Apst->time_base.den <= 0){
		mux_err("Muxing:audio stream not ready, rate=%d tb=%d/%d\n",
			Apst->codecpar ? Apst->codecpar->sample_rate : 0,
			Apst->time_base.num, Apst->time_base.den);
		info->audio_pkt_fail++;
		av_packet_unref(&apkt);
		return MUX_FAILURE;
	}
	apkt.duration = av_rescale_q(frame_samples, (AVRational){1, Apst->codecpar->sample_rate}, Apst->time_base);
	if(apkt.duration <= 0)
		apkt.duration = 1;
	if(info->last_audio_pts > 0 && apkt.pts > (int64_t)info->last_audio_pts + apkt.duration * 8)
		apkt.pts = info->last_audio_pts + apkt.duration;
	if(info->last_audio_pts == 0 && apkt.pts > apkt.duration * 48000)
		apkt.pts = 0;
	if(apkt.pts <= (int64_t)info->last_audio_pts)
		apkt.pts = info->last_audio_pts + 1;
	info->last_audio_pts = apkt.pts;
	apkt.dts = apkt.pts;
	apkt.pos = -1;
	apkt.stream_index = Apst->index;

	pthread_mutex_lock(&info->write_lock);
	ret = av_write_frame(info->g_OutFmt_Ctx, &apkt);
	pthread_mutex_unlock(&info->write_lock);
	av_packet_unref(&apkt);
	if(ret < 0)
	{
		info->audio_pkt_fail++;
		mux_err("Muxing:cannot write audio frame, ret=%d len=%d\n", ret, len);
		return MUX_FAILURE;
	}
	info->audio_pkt_ok++;
	return MUX_SUCCESS;
}

int MP4_Muxer_WriteAudio(MUX_INFO *info, MUX_AUDIO_STREAM_S *pstStream)
{
	unsigned char* pPackVirtAddr = NULL;	//码流数据首地址
    unsigned int u32PackLen = 0;	//码流数据长度
    AVStream *Apst = NULL;	//用于指向音频流

	if((NULL == pstStream)||(info->ai_enable == 0))
    {
        return MUX_SUCCESS;
    }
    if(info->g_OutFmt_Ctx == NULL)
    {
        return MUX_FAILURE;
    }
    if(info->ai<0 || info->ai >= (int)info->g_OutFmt_Ctx->nb_streams)
    {
        mux_err("ai %d out of range\n", info->ai);
        return MUX_SUCCESS;
    }
    //获取音频码流信息
    pPackVirtAddr = pstStream->pStream;	//带７字节ADTS头
    u32PackLen = pstStream->u32Len;
    if(pPackVirtAddr == NULL || u32PackLen == 0)
    {
        return MUX_SUCCESS;
    }
    /* AENC 输出缓冲按 u32MaxFrmLen=2048 申请。长度异常时只取前 2048，避免把缓存后面的未映射页读进来。 */
    if(u32PackLen > 2048)
        u32PackLen = 2048;
    Apst = info->g_OutFmt_Ctx->streams[info->ai];	//ai代表音频流索引号
    MUX_U64 a_ts = pstStream->u64TimeStamp;
    if(info->Afirst==0)	//如果是第一个音频帧
    {
        info->Afirst=1;
        /* 视频基准是 CLOCK_MONOTONIC。本机麦的 AENC 时间戳也是这套钟，直接相减就能和视频对齐。
         * 蓝牙 SCO 的时间戳是墙上时钟，和单调时钟相差几十年。若仍用 shared_base_ts 做减法，
         * 音轨时长会变成当前 Unix 时间，播放器打不开视频。这时改用音频自己的第一帧做原点。 */
        int same_clock = 0;
        if(info->shared_base_ts != 0){
            int64_t delta = (int64_t)a_ts - (int64_t)info->shared_base_ts;
            if(delta < 0)
                delta = -delta;
            if(delta <= 3600LL * 1000000LL)
                same_clock = 1;
        }
        if(same_clock){
            info->Audio_PTS = info->shared_base_ts;
        }else{
            int64_t wall_off = 0;
            if(info->shared_base_ts != 0){
                wall_off = (int64_t)TEST_COMM_GetNowUs() - (int64_t)info->shared_base_ts;
                if(wall_off < 0 || wall_off > 3600LL * 1000000LL)
                    wall_off = 0;
            }
            info->Audio_PTS = a_ts - (MUX_U64)wall_off;
            mux_log("[MUX-AUDIO] rebase timestamp, a_ts=%llu video_base=%llu wall_off=%lld us",
                (unsigned long long)a_ts, (unsigned long long)info->shared_base_ts, (long long)wall_off);
        }
	}

	/* 按 AENC 原始包整包写入。切成从 0xFFFB 开始的帧后，封装器会按帧长去读，第一包就段错误。 */
	if(info->Afirst == 1 && info->last_audio_pts == 0){
		mux_log("[MUX-AUDIO] first pkt len=%u head=%02x%02x%02x%02x",
			u32PackLen,
			pPackVirtAddr[0],
			u32PackLen > 1 ? pPackVirtAddr[1] : 0,
			u32PackLen > 2 ? pPackVirtAddr[2] : 0,
			u32PackLen > 3 ? pPackVirtAddr[3] : 0);
	}
	if(write_one_audio_frame(info, Apst, pPackVirtAddr, (int)u32PackLen, a_ts) != MUX_SUCCESS)
		return MUX_FAILURE;
	return MUX_SUCCESS;
}

static int br_u(const unsigned char *d, int len, int *bit, int n)
{
	int v = 0;
	int i;
	for(i = 0; i < n; i++){
		int bi = (*bit)++;
		if(bi / 8 >= len)
			return 0;
		v = (v << 1) | ((d[bi / 8] >> (7 - (bi % 8))) & 1);
	}
	return v;
}

static int br_ue(const unsigned char *d, int len, int *bit)
{
	int z = 0;
	while(br_u(d, len, bit, 1) == 0 && z < 31)
		z++;
	return ((1 << z) - 1) + (z ? br_u(d, len, bit, z) : 0);
}

static int br_se(const unsigned char *d, int len, int *bit)
{
	int v = br_ue(d, len, bit);
	return (v & 1) ? (v + 1) / 2 : -(v / 2);
}

static void skip_scaling_list(const unsigned char *d, int len, int *bit, int size)
{
	int last = 8, next = 8, j;
	for(j = 0; j < size; j++){
		int delta = br_se(d, len, bit);
		next = (last + delta + 256) % 256;
		if(next == 0)
			break;
		last = next;
	}
}

/* 从 Annex-B 关键帧里取出 SPS 宽高。失败返回 -1。 */
static int h264_sps_size(const unsigned char *buf, int len, int *w, int *h)
{
	int i;
	const unsigned char *nal = NULL;
	int nal_len = 0;
	unsigned char rb[128];
	int rb_len = 0;
	int bit = 0;
	int profile, chroma, frame_mbs_only, crop;
	int pic_w, pic_h;
	int k;

	for(i = 0; i + 5 < len; i++){
		int sc = 0;
		if(buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)
			sc = 3;
		else if(i + 4 < len && buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 0 && buf[i + 3] == 1)
			sc = 4;
		if(!sc)
			continue;
		if((buf[i + sc] & 0x1f) != 7)
			continue;
		nal = buf + i + sc + 1;
		nal_len = len - (i + sc + 1);
		if(nal_len > 80)
			nal_len = 80;
		break;
	}
	if(!nal)
		return -1;

	for(k = 0; k < nal_len && rb_len < (int)sizeof(rb); k++){
		if(k + 2 < nal_len && nal[k] == 0 && nal[k + 1] == 0 && nal[k + 2] == 3){
			rb[rb_len++] = 0;
			rb[rb_len++] = 0;
			k += 2;
		}else{
			rb[rb_len++] = nal[k];
		}
	}
	if(rb_len < 4)
		return -1;

	profile = br_u(rb, rb_len, &bit, 8);
	br_u(rb, rb_len, &bit, 8);
	br_u(rb, rb_len, &bit, 8);
	br_ue(rb, rb_len, &bit);
	chroma = 1;
	if(profile == 100 || profile == 110 || profile == 122 || profile == 244){
		chroma = br_ue(rb, rb_len, &bit);
		if(chroma == 3)
			br_u(rb, rb_len, &bit, 1);
		br_ue(rb, rb_len, &bit);
		br_ue(rb, rb_len, &bit);
		br_u(rb, rb_len, &bit, 1);
		if(br_u(rb, rb_len, &bit, 1)){
			int n = (chroma != 3) ? 8 : 12;
			int si;
			for(si = 0; si < n; si++){
				if(br_u(rb, rb_len, &bit, 1))
					skip_scaling_list(rb, rb_len, &bit, si < 6 ? 16 : 64);
			}
		}
	}
	br_ue(rb, rb_len, &bit);
	{
		int poc = br_ue(rb, rb_len, &bit);
		if(poc == 0){
			br_ue(rb, rb_len, &bit);
		}else if(poc == 1){
			int n, t;
			br_u(rb, rb_len, &bit, 1);
			br_se(rb, rb_len, &bit);
			br_se(rb, rb_len, &bit);
			n = br_ue(rb, rb_len, &bit);
			for(t = 0; t < n; t++)
				br_se(rb, rb_len, &bit);
		}
	}
	br_ue(rb, rb_len, &bit);
	br_u(rb, rb_len, &bit, 1);
	pic_w = br_ue(rb, rb_len, &bit);
	pic_h = br_ue(rb, rb_len, &bit);
	frame_mbs_only = br_u(rb, rb_len, &bit, 1);
	if(!frame_mbs_only)
		br_u(rb, rb_len, &bit, 1);
	br_u(rb, rb_len, &bit, 1);
	crop = br_u(rb, rb_len, &bit, 1);
	*w = (pic_w + 1) * 16;
	*h = (pic_h + 1) * 16 * (2 - frame_mbs_only);
	if(crop){
		int cl = br_ue(rb, rb_len, &bit);
		int cr = br_ue(rb, rb_len, &bit);
		int ct = br_ue(rb, rb_len, &bit);
		int cb = br_ue(rb, rb_len, &bit);
		int unit = (chroma == 1) ? 2 : 1;
		*w -= (cl + cr) * unit;
		*h -= (ct + cb) * unit * (2 - frame_mbs_only);
	}
	if(*w <= 0 || *h <= 0 || *w > 8192 || *h > 8192)
		return -1;
	return 0;
}

static int getVopType( const void *p, int len )
{
    if ( !p || 6 >= len )
        return -1;
    unsigned char *b = (unsigned char*)p;
    // Verify NAL marker
    if ( b[ 0 ] || b[ 1 ] || 0x01 != b[ 2 ] )
    {   b++;
        if ( b[ 0 ] || b[ 1 ] || 0x01 != b[ 2 ] )
            return -1;
    } // end if
    b += 3;
    // Verify VOP id
    if ( 0xb6 == *b )
    {   
        b++;
        return ( *b & 0xc0 ) >> 6;
    } // end if
    switch( *b )
    {  
        case 0x65 : return 0;
        case 0x61 : return 1;
        case 0x01 : return 2;
    } // end switch
    return -1;
}

void MP4_Muxer_WriteVideo(MUX_INFO *info, unsigned char *ptr , unsigned int len, MUX_U64 ts_us)
{
    unsigned int i=0;
    unsigned char *pPackVirtAddr = ptr;
    unsigned int u32PackLen = len;
    int ret = 0;
    AVStream *pst = NULL;
    AVPacket vpkt;
    memset(&vpkt, 0, sizeof(AVPacket));

    if(info->g_OutFmt_Ctx == NULL || info->vi<0)
	{
		return;
	}
	if(info->vi >= (int)info->g_OutFmt_Ctx->nb_streams)
	{
		mux_err("vi %d >= nb_streams %u \n", info->vi, info->g_OutFmt_Ctx->nb_streams);
		return;
	}

	pst = info->g_OutFmt_Ctx->streams[info->vi];	//根据索引号获取视频流地址

	vpkt.flags |= ( 0 >= getVopType( pPackVirtAddr, u32PackLen ) ) ? AV_PKT_FLAG_KEY : 0;

	if(info->b_First_IDR_Find ==0)
	{
		if (0 == ( vpkt.flags & AV_PKT_FLAG_KEY )){
			av_packet_unref(&vpkt);
			return;
		}
		info->b_First_IDR_Find = MUX_TRUE;
	}

	vpkt.stream_index = pst->index;	//视频流的索引号 赋给 包里面的流索引号，表示这个包属于视频流

	/* 封装宽高按码流里的 SPS，不按配置的 1280x516。实际编码是 640x512，
	 * 容器写成 1280x516 时播放器按这个尺寸协商，视频解不出来。 */
	if(!info->video_size_fixed && (vpkt.flags & AV_PKT_FLAG_KEY))
	{
		int vw = 0, vh = 0;
		if(h264_sps_size(pPackVirtAddr, u32PackLen, &vw, &vh) == 0 && vw > 0 && vh > 0)
		{
			pst->codecpar->width = vw;
			pst->codecpar->height = vh;
			if(pst->codec)
			{
				pst->codec->width = vw;
				pst->codec->height = vh;
			}
			info->w = vw;
			info->h = vh;
			info->video_size_fixed = 1;
			mux_log("[MUX-VIDEO] sps size %dx%d", vw, vh);
		}
	}

	//用真实时间戳计算相对基准的pts，丢帧时时长依然正确，三路视频时间一致（时间戳单位us）
	//优先使用shared_base_ts（全局录像开始时间），保证三路视频时长一致
	if(info->base_ts_set == MUX_FALSE)
	{
		info->base_ts_set = MUX_TRUE;
		info->Video_Base_TS = (info->shared_base_ts != 0) ? info->shared_base_ts : ts_us;
	}
	int64_t time_diff = (int64_t)(ts_us - info->Video_Base_TS);
	if(time_diff < 0)
		time_diff = 0;
	if (pst->time_base.den <= 0 || pst->time_base.num <= 0) {
		mux_err("[MUX-ERR] invalid time_base: %d/%d, vi=%d, nb_streams=%u",
			pst->time_base.num, pst->time_base.den, info->vi, info->g_OutFmt_Ctx->nb_streams);
		vpkt.pts = info->last_video_pts;
	} else {
		vpkt.pts = av_rescale_q(time_diff, (AVRational){1, 1000000}, pst->time_base);
	}
	//mux_log("[MUX-DEBUG] info=%p time_diff=%lld, time_base=%d/%d, vpkt.pts=%lld, last_video_pts=%llu",
	//	info, (long long)time_diff, pst->time_base.num, pst->time_base.den,
	//	(long long)vpkt.pts, (unsigned long long)info->last_video_pts);

	//修复DTS重复问题：av_rescale_q精度约11us，两帧时间差小于此值会产生相同PTS
	//强制PTS单调递增，避免FFmpeg报错"non monotonically increasing dts"
	if(vpkt.pts <= (int64_t)info->last_video_pts)
	{
		vpkt.pts = info->last_video_pts + 1;
	}
	info->last_video_pts = vpkt.pts;

	vpkt.dts = vpkt.pts;	//只有I、P帧，相等就行了
	//duration按帧率换算，不再硬编码40（硬编码与drm的22fps不匹配）
	vpkt.duration = av_rescale_q(1, pst->codec->time_base, pst->time_base);
	//mux_log("%d : %ld %ld %ld",info->ai_enable,vpkt.pts,vpkt.dts,vpkt.duration);
	vpkt.pos = -1;

	//接受视频数据NAUL
	vpkt.data = (unsigned char*)(pPackVirtAddr);
	//视频数据长度
	vpkt.size = u32PackLen;


	pthread_mutex_lock(&info->write_lock);
	int64_t pts_before_write = vpkt.pts;
	int64_t dts_before_write = vpkt.dts;
	int64_t duration_before_write = vpkt.duration;
	/* 使用 av_write_frame 代替 av_interleaved_write_frame
	 * av_interleaved_write_frame 会重新排序帧并修改 PTS，导致 MP4 duration 错误
	 * 我们的帧已经按时间顺序排列，不需要 interleaving */
	ret = av_write_frame(info->g_OutFmt_Ctx, &vpkt);
	if (ret < 0)
	{
		mux_err("cannot write video frame, ret=%d\n", ret);
	}
	else
	{
		/* 更新统计信息 */
		if (info->write_frame_cnt == 0) {
			info->first_write_pts = (MUX_U64)pts_before_write;
			mux_log("[MUX-DEBUG] frame=0: pts=%lld, dts=%lld, duration=%lld, pts_after=%lld",
				(long long)pts_before_write, (long long)dts_before_write, (long long)duration_before_write, (long long)vpkt.pts);
		}
		if (info->write_frame_cnt == 1 || info->write_frame_cnt == 10 || info->write_frame_cnt % 100 == 0) {
			mux_log("[MUX-DEBUG] frame=%d: pts=%lld, dts=%lld, duration=%lld, pts_after=%lld",
				info->write_frame_cnt, (long long)pts_before_write, (long long)dts_before_write, (long long)duration_before_write, (long long)vpkt.pts);
		}
		info->last_write_pts = (MUX_U64)pts_before_write;
		info->write_frame_cnt++;
	}
	pthread_mutex_unlock(&info->write_lock);

	av_packet_unref(&vpkt);
    return;
}

/* 定位"录20s播放60s"专用自检日志：文件关闭完成后，重新打开刚落盘的 mp4，
 * 按播放器解析同一份数据的方式读出容器记录的真实时长，逐轨打印。判读方法：
 * - [MUX-VERIFY] 的时长就已经虚高(如60s) → 文件本身写错，与QT打开时机无关；
 *   看 stream#N 哪条轨时长虚高(若 audio 轨≈60s 而 video≈20s，即音轨pts在拉伸)。
 * - [MUX-VERIFY] 时长正常(≈20s) → 关闭瞬间文件是好的，应查QT播放侧
 *   (是否打开了未写trailer的半截文件/用了旧的缓存)，now=时间戳可与QT打开时刻对齐。 */
static void mp4_verify_written_file(const char *path)
{
	if (path[0] == 0x0)
		return;

	RK_U64 now_us = TEST_COMM_GetNowUs();
	struct stat st;
	if (stat(path, &st) == 0) {
		mux_log("[MUX-VERIFY] now=%llu us, %s size=%lld bytes",
			(unsigned long long)now_us, path, (long long)st.st_size);
	} else {
		mux_err("[MUX-VERIFY] stat(%s) failed", path);
		return;
	}

	AVFormatContext *fmt = NULL;
	int ret = avformat_open_input(&fmt, path, NULL, NULL);
	if (ret < 0 || fmt == NULL) {
		mux_err("[MUX-VERIFY] reopen failed ret=%d path=%s", ret, path);
		return;
	}
	if (avformat_find_stream_info(fmt, NULL) < 0) {
		mux_err("[MUX-VERIFY] find_stream_info failed path=%s", path);
		avformat_close_input(&fmt);
		return;
	}

	mux_log("[MUX-VERIFY] container: duration=%.2f sec, nb_streams=%u",
		fmt->duration > 0 ? (double)fmt->duration / (double)AV_TIME_BASE : -1.0,
		fmt->nb_streams);
	for (unsigned int i = 0; i < fmt->nb_streams; i++) {
		AVStream *s = fmt->streams[i];
		double dur = -1.0;
		if (s->duration != AV_NOPTS_VALUE && s->time_base.den > 0)
			dur = (double)s->duration * s->time_base.num / s->time_base.den;
		mux_log("[MUX-VERIFY]   stream#%u %s: dur=%.2f sec (raw=%lld tb=%d/%d) nb_frames=%d",
			i,
			s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ? "video" :
			(s->codecpar->codec_type == AVMEDIA_TYPE_AUDIO ? "audio" : "other"),
			dur, (long long)s->duration, s->time_base.num, s->time_base.den,
			(int)s->nb_frames);
	}
	avformat_close_input(&fmt);
}

void MP4_Muxer_Close(MUX_INFO *info)
{
	RK_U64 close_start_us = TEST_COMM_GetNowUs();
	char file_path[256] = {0x0};
	const char *url = "NULL";
	if (info->g_OutFmt_Ctx && info->g_OutFmt_Ctx->url)
		url = info->g_OutFmt_Ctx->url;
	mux_log("[MUX-CLOSE] START file=%s (w=%d) audio_ok=%llu audio_fail=%llu", url, info->w,
		(unsigned long long)info->audio_pkt_ok, (unsigned long long)info->audio_pkt_fail);
	fflush(stdout);
	if (url[0] != '\0' && strcmp(url, "NULL") != 0)
		snprintf(file_path, sizeof(file_path), "%s", url);

	/* 打印 Muxer 统计信息 */
	//mux_log("[MUX-DEBUG] info=%p Close: write_frame_cnt=%llu, first_write_pts=%llu, last_write_pts=%llu",
	//	info,
	//	(unsigned long long)info->write_frame_cnt,
	//	(unsigned long long)info->first_write_pts,
	//	(unsigned long long)info->last_write_pts);
	if (info->write_frame_cnt > 0) {
		int64_t pts_span = (int64_t)info->last_write_pts - (int64_t)info->first_write_pts;
		/* time_base 通常是 1/90000 对于 H.264 */
		double duration_sec = 0;
		if (info->g_OutFmt_Ctx && info->vi >= 0 && info->vi < (int)info->g_OutFmt_Ctx->nb_streams) {
			AVStream *pst = info->g_OutFmt_Ctx->streams[info->vi];
			duration_sec = (double)pts_span * (double)pst->time_base.num / (double)pst->time_base.den;
		}
		mux_log("[MUX-STAT] file written: %llu frames, first_pts=%lld, last_pts=%lld, pts_span=%lld (%.2f sec), expect %.1f sec at %d fps",
			(unsigned long long)info->write_frame_cnt,
			(long long)info->first_write_pts, (long long)info->last_write_pts, (long long)pts_span,
			duration_sec,
			(double)info->write_frame_cnt / info->fps, info->fps);
	} else {
		mux_log("[MUX-STAT] file written: 0 frames");
	}

    if (info->g_OutFmt_Ctx){
        mux_log("[MUX-CLOSE] ID %d av_write_trailer START", info->w);
        int ret_trailer = av_write_trailer(info->g_OutFmt_Ctx);
        if (ret_trailer < 0) {
            mux_err("ID %d av_write_trailer FAILED! ret=%d, file may be incomplete", info->w, ret_trailer);
        } else {
            mux_log("[MUX-CLOSE] ID %d av_write_trailer DONE (ret=%d)", info->w, ret_trailer);
        }
	}

    if (info->g_OutFmt_Ctx && !(info->g_OutFmt_Ctx->oformat->flags & AVFMT_NOFILE)){
        /* 先 flush 缓冲区，确保所有数据写入磁盘 */
        if (info->g_OutFmt_Ctx->pb) {
            mux_log("[MUX-CLOSE] ID %d avio_flush START", info->w);
            avio_flush(info->g_OutFmt_Ctx->pb);
            mux_log("[MUX-CLOSE] ID %d avio_flush DONE", info->w);
        }
        mux_log("[MUX-CLOSE] ID %d avio_close START", info->w);
        int ret_close = avio_close(info->g_OutFmt_Ctx->pb);
        if (ret_close < 0) {
            mux_err("ID %d avio_close FAILED! ret=%d, data may be lost", info->w, ret_close);
        } else {
            mux_log("[MUX-CLOSE] ID %d avio_close DONE (ret=%d)", info->w, ret_close);
        }
	}

    if (info->g_OutFmt_Ctx)
    {
        mux_log("[MUX-CLOSE] ID %d avformat_free_context START", info->w);
        avformat_free_context(info->g_OutFmt_Ctx);
        info->g_OutFmt_Ctx = NULL;
        mux_log("[MUX-CLOSE] ID %d avformat_free_context DONE", info->w);
    }
    
    RK_U64 close_end_us = TEST_COMM_GetNowUs();
    mux_log("[MUX-CLOSE] COMPLETE file closed in %lu ms", (close_end_us - close_start_us)/1000);

    /* 定位"录20s播放60s"：关闭后立即重开文件读实际时长，区分"文件写错"还是"读早了" */
    mp4_verify_written_file(file_path);
    
    info->vi = -1;
    info->ai = -1;
    info->ptsInc = 0;
    info->Afirst = 0;
    info->base_ts_set = MUX_FALSE;
    info->Video_Base_TS = 0;
    info->b_First_IDR_Find = MUX_FALSE;
    info->last_video_pts = 0;
    info->last_audio_pts = 0;
    info->last_video_ts_us = 0;
    info->last_audio_ts_us = 0;
    info->video_ts_offset = 0;
    info->audio_ts_offset = 0;
    info->write_frame_cnt = 0;
    info->first_write_pts = 0;
    info->last_write_pts = 0;
    pthread_mutex_destroy(&info->write_lock);
}

#if 0
/*******以已生成的文件合成封装MP4**************/
#define USE_H264BSF 1
#define USE_AACBSF 0
#define CODEC_FLAG_GLOBAL_HEADER  AV_CODEC_FLAG_GLOBAL_HEADER
int file_to_mp4_test(void)
{
    AVOutputFormat *ofmt = NULL;
    //Input AVFormatContext and Output AVFormatContext
    AVFormatContext *ifmt_ctx_v = NULL, *ifmt_ctx_a = NULL,*ofmt_ctx = NULL;
    AVPacket pkt;
    int ret, i;
    int videoindex_v=-1,videoindex_out=-1;
    int audioindex_a=-1,audioindex_out=-1;
    int frame_index=0;
    int64_t cur_pts_v=0,cur_pts_a=0;

	const char *in_filename_v = "/tmp/test.h264";
	const char *in_filename_a = "/tmp/out.mp2";
    const char *out_filename = "/tmp/off_out.mp4";//Output file URL

    av_register_all();
    //Input
    if ((ret = avformat_open_input(&ifmt_ctx_v, in_filename_v, 0, 0)) < 0) {
        printf( "Could not open input file.");
        goto end;
    }
    if ((ret = avformat_find_stream_info(ifmt_ctx_v, 0)) < 0) {
        printf( "Failed to retrieve input stream information");
        goto end;
    }

    if ((ret = avformat_open_input(&ifmt_ctx_a, in_filename_a, 0, 0)) < 0) {
        printf( "Could not open input file.");
        goto end;
    }
    if ((ret = avformat_find_stream_info(ifmt_ctx_a, 0)) < 0) {
        printf( "Failed to retrieve input stream information");
        goto end;
    }
    printf("===========Input Information==========\n");
    av_dump_format(ifmt_ctx_v, 0, in_filename_v, 0);
    av_dump_format(ifmt_ctx_a, 0, in_filename_a, 0);
    printf("======================================\n");
    //Output
    avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, out_filename);
    if (!ofmt_ctx) {
        printf( "Could not create output context\n");
        ret = AVERROR_UNKNOWN;
        goto end;
    }
    ofmt = ofmt_ctx->oformat;

    for (i = 0; i < ifmt_ctx_v->nb_streams; i++) {
        //Create output AVStream according to input AVStream
        if(ifmt_ctx_v->streams[i]->codec->codec_type==AVMEDIA_TYPE_VIDEO){
        AVStream *in_stream = ifmt_ctx_v->streams[i];
        AVStream *out_stream = avformat_new_stream(ofmt_ctx, in_stream->codec->codec);
        videoindex_v=i;
        if (!out_stream) {
            printf( "Failed allocating output stream\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }
        videoindex_out=out_stream->index;
        //Copy the settings of AVCodecContext
        if (avcodec_copy_context(out_stream->codec, in_stream->codec) < 0) {
            printf( "Failed to copy context from input to output stream codec context\n");
            goto end;
        }
        out_stream->codec->codec_tag = 0;
        if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
            out_stream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
        break;
        }
    }

    for (i = 0; i < ifmt_ctx_a->nb_streams; i++) {
        //Create output AVStream according to input AVStream
        if(ifmt_ctx_a->streams[i]->codec->codec_type==AVMEDIA_TYPE_AUDIO){
            AVStream *in_stream = ifmt_ctx_a->streams[i];
            AVStream *out_stream = avformat_new_stream(ofmt_ctx, in_stream->codec->codec);
            audioindex_a=i;
            if (!out_stream) {
                printf( "Failed allocating output stream\n");
                ret = AVERROR_UNKNOWN;
                goto end;
            }
            audioindex_out=out_stream->index;
            //Copy the settings of AVCodecContext
            if (avcodec_copy_context(out_stream->codec, in_stream->codec) < 0) {
                printf( "Failed to copy context from input to output stream codec context\n");
                goto end;
            }
            out_stream->codec->codec_tag = 0;
            if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
                out_stream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;

            break;
        }
    }

    printf("==========Output Information==========\n");
    av_dump_format(ofmt_ctx, 0, out_filename, 1);
    printf("======================================\n");
    //Open output file
    if (!(ofmt->flags & AVFMT_NOFILE)) {
        if (avio_open(&ofmt_ctx->pb, out_filename, AVIO_FLAG_WRITE) < 0) {
            printf( "Could not open output file '%s'", out_filename);
            goto end;
        }
    }
    //Write file header
    if (avformat_write_header(ofmt_ctx, NULL) < 0) {
        printf( "Error occurred when opening output file\n");
        goto end;
    }


    //FIX
#if USE_H264BSF
    AVBitStreamFilterContext* h264bsfc =  av_bitstream_filter_init("h264_mp4toannexb");
#endif
#if USE_AACBSF
    AVBitStreamFilterContext* aacbsfc =  av_bitstream_filter_init("aac_adtstoasc");
#endif

    while (1) {
        AVFormatContext *ifmt_ctx;
        int stream_index=0;
        AVStream *in_stream, *out_stream;

        //Get an AVPacket
        if(av_compare_ts(cur_pts_v,ifmt_ctx_v->streams[videoindex_v]->time_base,cur_pts_a,ifmt_ctx_a->streams[audioindex_a]->time_base) <= 0){
            ifmt_ctx=ifmt_ctx_v;
            stream_index=videoindex_out;

            if(av_read_frame(ifmt_ctx, &pkt) >= 0){
                do{
                    in_stream  = ifmt_ctx->streams[pkt.stream_index];
                    out_stream = ofmt_ctx->streams[stream_index];

                    if(pkt.stream_index==videoindex_v){
                        //FIX：No PTS (Example: Raw H.264)
                        //Simple Write PTS
                        if(pkt.pts==AV_NOPTS_VALUE){
                            //Write PTS
                            AVRational time_base1=in_stream->time_base;
                            //Duration between 2 frames (us)
                            int64_t calc_duration=(double)AV_TIME_BASE/av_q2d(in_stream->r_frame_rate);
                            //Parameters
                            pkt.pts=(double)(frame_index*calc_duration)/(double)(av_q2d(time_base1)*AV_TIME_BASE);
                            pkt.dts=pkt.pts;
                            pkt.duration=(double)calc_duration/(double)(av_q2d(time_base1)*AV_TIME_BASE);
                            frame_index++;
                        }

                        cur_pts_v=pkt.pts;
                        break;
                    }
                }while(av_read_frame(ifmt_ctx, &pkt) >= 0);
            }else{
                break;
            }
        }else{
            ifmt_ctx=ifmt_ctx_a;
            stream_index=audioindex_out;
            if(av_read_frame(ifmt_ctx, &pkt) >= 0){
                do{
                    in_stream  = ifmt_ctx->streams[pkt.stream_index];
                    out_stream = ofmt_ctx->streams[stream_index];

                    if(pkt.stream_index==audioindex_a){

                        //FIX：No PTS
                        //Simple Write PTS
                        if(pkt.pts==AV_NOPTS_VALUE){
                            //Write PTS
                            AVRational time_base1=in_stream->time_base;
                            //Duration between 2 frames (us)
                            int64_t calc_duration=(double)AV_TIME_BASE/av_q2d(in_stream->r_frame_rate);
                            //Parameters
                            pkt.pts=(double)(frame_index*calc_duration)/(double)(av_q2d(time_base1)*AV_TIME_BASE);
                            pkt.dts=pkt.pts;
                            pkt.duration=(double)calc_duration/(double)(av_q2d(time_base1)*AV_TIME_BASE);
                            frame_index++;
                        }
                        cur_pts_a=pkt.pts;

                        break;
                    }
                }while(av_read_frame(ifmt_ctx, &pkt) >= 0);
            }else{
                break;
            }

        }

        //FIX:Bitstream Filter
#if USE_H264BSF
        av_bitstream_filter_filter(h264bsfc, in_stream->codec, NULL, &pkt.data, &pkt.size, pkt.data, pkt.size, 0);
#endif
#if USE_AACBSF
        av_bitstream_filter_filter(aacbsfc, out_stream->codec, NULL, &pkt.data, &pkt.size, pkt.data, pkt.size, 0);
#endif


        //Convert PTS/DTS
        pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, out_stream->time_base, (enum AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
        pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, out_stream->time_base, (enum AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
        pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
        pkt.pos = -1;
        pkt.stream_index=stream_index;

        //printf("Write 1 Packet. size:%5d\tpts:%ld\n",pkt.size,pkt.pts);
        //Write
        if (av_interleaved_write_frame(ofmt_ctx, &pkt) < 0) {
            printf( "Error muxing packet\n");
            break;
        }
        //printf("av_free_packet(&pkt); execution\n");
        av_free_packet(&pkt);

    }
    //Write file trailer
    av_write_trailer(ofmt_ctx);

#if USE_H264BSF
    av_bitstream_filter_close(h264bsfc);
#endif
#if USE_AACBSF
    av_bitstream_filter_close(aacbsfc);
#endif

end:
    avformat_close_input(&ifmt_ctx_v);
    avformat_close_input(&ifmt_ctx_a);
    /* close output */
    if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE))
        avio_close(ofmt_ctx->pb);
    printf("avformat_free_context(ofmt_ctx); execution\n");
    avformat_free_context(ofmt_ctx);
    if (ret < 0 && ret != AVERROR_EOF) {
        printf( "Error occurred.\n");
        return -1;
    }
    return 0;
}
#endif