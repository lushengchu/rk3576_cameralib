#ifndef __CAMERA_AI_H__
#define __CAMERA_AI_H__

#ifdef __cplusplus
extern "C" {
#endif

#define OBJ_NAME_MAX_SIZE 16
#define OBJ_NUMB_MAX_SIZE 64
#define YOLOV5_MODEL_PATH "/userdata/ai/yolov5s-640-640.rknn"
#define LABEL_NALE_TXT_PATH "/userdata/ai/model/coco_80_labels_list.txt"

//#define AI_LOG_EN
#ifdef AI_LOG_EN
#define AI_LOG(x, ...) printf("[ODET]" x "\r\n",## __VA_ARGS__)
#else
#define AI_LOG(x, ...)
#endif

typedef struct _BOX_RECT
{
    int left;
    int right;
    int top;
    int bottom;
} BOX_RECT;

typedef struct __detect_result_t
{
    int index;
    char name[OBJ_NAME_MAX_SIZE];
    BOX_RECT box;
    float prop;
} detect_result_t;

typedef struct _detect_result_group_t
{
    int id;
    int count;
    detect_result_t results[OBJ_NUMB_MAX_SIZE];
} detect_result_group_t;



int yolov5_init(const char *model_path, const char *label_path);
int yolov5_detect(unsigned char* data, int img_width, int img_height);
int yolov5_deinit(void);
void camera_ai_detect_res_deal(detect_result_group_t *res);

#ifdef __cplusplus
}
#endif

#endif
