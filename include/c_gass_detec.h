#ifdef __cplusplus
extern "C" {
#endif

// 不透明指针，隐藏C++类的实现细节
typedef struct ImageProcessorHandle ImageProcessorHandle;

// C语言接口函数
ImageProcessorHandle* image_processor_create();
void image_processor_destroy(ImageProcessorHandle* handle);
unsigned char *dealNewFrame(ImageProcessorHandle* handle,unsigned char *data,double *ppm);
void setRectangle(ImageProcessorHandle* handle,int x,int y,int w,int h);

#ifdef __cplusplus
}
#endif