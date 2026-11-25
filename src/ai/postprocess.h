#ifndef _RKNN_ZERO_COPY_DEMO_POSTPROCESS_H_
#define _RKNN_ZERO_COPY_DEMO_POSTPROCESS_H_

#include <stdint.h>
#include <vector>

#include "rknn/rknn_api.h"
#include "camera_ai.h"
// #include "opencv2/opencv.hpp"
// #include "opencv2/core/core.hpp"
// #include "opencv2/imgproc.hpp"
// #include "opencv2/imgcodecs.hpp"

#define OBJ_CLASS_NUM     1
#define NMS_THRESH        0.45
#define BOX_THRESH        0.25
#define PROP_BOX_SIZE     (5+OBJ_CLASS_NUM)

//#define YOLOV5_MODEL_PATH "/userdata/ai/yolov5s-640-640.rknn"
//#define LABEL_NALE_TXT_PATH "/userdata/ai/model/coco_80_labels_list.txt"

// int yolov5_init(const char *model_path, const char *label_path);
// int yolov5_detect(cv::Mat &src, cv::Mat &res);
// int yolov5_deinit();

int loadLabelName(const char* locationFilename);
int post_process(int8_t *input0, int8_t *input1, int8_t *input2, int model_in_h, int model_in_w,
                 float conf_threshold, float nms_threshold, float scale_w, float scale_h,
                 std::vector<int32_t> &qnt_zps, std::vector<float> &qnt_scales,
                 detect_result_group_t *group);

void deinitPostProcess();
#endif //_RKNN_ZERO_COPY_DEMO_POSTPROCESS_H_
