#ifndef MP4_MUXER_FFMPEG_H
#define MP4_MUXER_FFMPEG_H

#include <unistd.h>
#include "libavformat/avformat.h"

typedef unsigned char           MUX_U8;
typedef unsigned int            MUX_U32;
typedef unsigned long long      MUX_U64;
typedef long long               MUX_S64;

#define MUX_SUCCESS              0
#define MUX_FAILURE              (-1)

typedef enum {
    MUX_FALSE = 0,
    MUX_TRUE  = 1,
} MUX_BOOL;

#define MUX_ALIGN_NUM                      4
#define MUX_ATTRIBUTE              __attribute__((aligned(MUX_ALIGN_NUM)))

typedef struct muxAUDIO_STREAM_S {
    MUX_U8 MUX_ATTRIBUTE *pStream;         /* the virtual address of stream */
    MUX_U64 MUX_ATTRIBUTE u64PhyAddr;      /* the physics address of stream */
    MUX_U32 u32Len;          /* stream lenth, by bytes */
    MUX_U64 u64TimeStamp;    /* frame time stamp */
    MUX_U32 u32Seq;          /* frame seq, if stream is not a valid frame,u32Seq is 0 */
} MUX_AUDIO_STREAM_S;

/* Defines a stream packet */
typedef struct muxVENC_PACK_S {
    MUX_U8                MUX_ATTRIBUTE* pu8Addr;         /* R; the virtual address of stream */
    MUX_U32               MUX_ATTRIBUTE u32Len;           /* R; the length of stream */
    MUX_U32               u32Offset;                  /* R; the offset between the Valid data and the start address */
} MUX_VENC_PACK_S;

/* Defines the features of an stream */
typedef struct muxVENC_STREAM_S {
    MUX_VENC_PACK_S MUX_ATTRIBUTE* pstPack;            /* R; stream pack attribute */
    MUX_U32      MUX_ATTRIBUTE u32PackCount;        /* R; the pack number of one frame stream */
} MUX_VENC_STREAM_S;

typedef struct muxInfo{
	AVFormatContext *g_OutFmt_Ctx;
	int vi;
	int ai;
	int ai_enable;
	int ptsInc;
	int Afirst;
	int w;
	int h;
	int fps;
	pthread_mutex_t write_lock;  /* 真锁：保护 av_interleaved_write_frame 并发写 */
	MUX_BOOL b_First_IDR_Find;
	MUX_U64 Audio_PTS;
	MUX_U64 Video_Base_TS;   /* first video timestamp (us), for relative pts */
	MUX_BOOL base_ts_set;
	MUX_U64 last_video_pts;  /* last written video pts, for monotonic dts check */
	MUX_U64 last_audio_pts;  /* last written audio pts, for monotonic dts check */
	MUX_U64 last_video_ts_us; /* last accepted raw video timestamp (us), for glitch clamp */
	MUX_U64 last_audio_ts_us; /* last accepted raw audio timestamp (us), for glitch clamp */
	MUX_S64 video_ts_offset;  /* accumulated video timestamp offset (us), for glitch compensation */
	MUX_S64 audio_ts_offset;  /* accumulated audio timestamp offset (us), for glitch compensation */
	MUX_U64 shared_base_ts;  /* shared recording start time (us), 0 = use own base */
	/* 统计信息 */
	MUX_U64 write_frame_cnt;    /* 写入的视频帧数 */
	MUX_U64 first_write_pts;    /* 第一帧写入时的 PTS */
	MUX_U64 last_write_pts;     /* 最后一帧写入时的 PTS */
} MUX_INFO;

//extern fun
extern int MP4_Muxer_Create(MUX_INFO *info ,char *mp4filename);
extern int MP4_Muxer_WriteAudio(MUX_INFO *info, MUX_AUDIO_STREAM_S *pstStream);
extern void MP4_Muxer_WriteVideo(MUX_INFO *info, unsigned char *ptr , unsigned int len, MUX_U64 ts_us);
extern void MP4_Muxer_Close(MUX_INFO *info);

#endif // MP4_MUXER_FFMPEG_H