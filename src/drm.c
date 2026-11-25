#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libdrm/drm_fourcc.h>

// 内存对齐宏
#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))

// 缓冲区对象结构
typedef struct buffer_object {
    uint32_t fb_id;
    uint32_t handle;
    uint32_t pitch;
    uint32_t size;
    uint8_t *map;
    uint32_t width;
    uint32_t height;
} buffer_object;

// DRM显示结构
typedef struct DrmDisplay {
    int drm_fd;
    uint32_t connector_id;
    uint32_t encoder_id;
    uint32_t crtc_id;
    uint32_t plane_id;
    uint32_t width;
    uint32_t height;
    
    drmModeCrtcPtr saved_crtc;
    
    buffer_object *buffers;
    int buffer_count;
    int buffer_capacity;
} DrmDisplay;

// 初始化DRM显示
DrmDisplay* drm_display_create() {
    DrmDisplay *display = (DrmDisplay*)malloc(sizeof(DrmDisplay));
    if (!display) {
        fprintf(stderr, "Failed to allocate DrmDisplay structure\n");
        return NULL;
    }
    
    display->drm_fd = -1;
    display->connector_id = 0;
    display->encoder_id = 0;
    display->crtc_id = 0;
    display->plane_id = 0;
    display->width = 0;
    display->height = 0;
    display->saved_crtc = NULL;
    display->buffers = NULL;
    display->buffer_count = 0;
    display->buffer_capacity = 0;
    
    return display;
}

// 清理DRM显示资源
void drm_display_destroy(DrmDisplay *display) {
    if (!display) return;
    
    // 恢复原始CRTC设置
    if (display->saved_crtc) {
        drmModeSetCrtc(display->drm_fd, display->saved_crtc->crtc_id, 
                      display->saved_crtc->buffer_id,
                      display->saved_crtc->x, display->saved_crtc->y, 
                      &display->connector_id, 1,
                      &display->saved_crtc->mode);
        drmModeFreeCrtc(display->saved_crtc);
    }
    
    // 清理缓冲区
    for (int i = 0; i < display->buffer_count; i++) {
        buffer_object *bo = &display->buffers[i];
        if (bo->fb_id) {
            drmModeRmFB(display->drm_fd, bo->fb_id);
        }
        if (bo->map && bo->map != MAP_FAILED) {
            munmap(bo->map, bo->size);
        }
        if (bo->handle) {
            struct drm_mode_destroy_dumb destroy_dumb = {};
            destroy_dumb.handle = bo->handle;
            drmIoctl(display->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);
        }
    }
    
    if (display->buffers) {
        free(display->buffers);
    }
    
    if (display->drm_fd >= 0) {
        close(display->drm_fd);
    }
    
    free(display);
}

// 获取CRTC索引
static int get_crtc_index(DrmDisplay *display, uint32_t crtc_id) {
    drmModeRes* resources = drmModeGetResources(display->drm_fd);
    if (!resources) {
        return -1;
    }
    
    for (int i = 0; i < resources->count_crtcs; i++) {
        if (resources->crtcs[i] == crtc_id) {
            drmModeFreeResources(resources);
            return i;
        }
    }
    
    drmModeFreeResources(resources);
    return -1;
}

// 获取DRM资源
static bool get_resources(DrmDisplay *display) {
    drmModeRes* resources = drmModeGetResources(display->drm_fd);
    if (!resources) {
        fprintf(stderr, "Failed to get DRM resources: %s\n", strerror(errno));
        return false;
    }
    
    // 查找connector
    for (int i = 0; i < resources->count_connectors; i++) {
        drmModeConnector* connector = drmModeGetConnector(display->drm_fd, resources->connectors[i]);
        if (connector && connector->connection == DRM_MODE_CONNECTED && 
            connector->count_modes > 0) {
            
            // 查找3840x2160模式，如果没有则使用第一个可用模式
            drmModeModeInfo* preferred_mode = NULL;
            for (int m = 0; m < connector->count_modes; m++) {
                drmModeModeInfo* mode = &connector->modes[m];
                if (mode->hdisplay == 3840 && mode->vdisplay == 2160) {
                    preferred_mode = mode;
                    break;
                }
            }
            
            // 如果没有找到4K模式，使用第一个模式
            if (!preferred_mode) {
                preferred_mode = &connector->modes[0];
            }
            
            display->connector_id = connector->connector_id;
            display->width = preferred_mode->hdisplay;
            display->height = preferred_mode->vdisplay;
            
            // 查找可用的encoder
            for (int j = 0; j < connector->count_encoders; j++) {
                drmModeEncoder* encoder = drmModeGetEncoder(display->drm_fd, connector->encoders[j]);
                if (encoder) {
                    // 查找可用的CRTC
                    for (int k = 0; k < resources->count_crtcs; k++) {
                        // 检查CRTC是否支持该encoder
                        if (encoder->possible_crtcs & (1 << k)) {
                            display->crtc_id = resources->crtcs[k];
                            display->encoder_id = encoder->encoder_id;
                            
                            // 保存原始CRTC状态
                            display->saved_crtc = drmModeGetCrtc(display->drm_fd, display->crtc_id);
                            
                            printf("Found connector: %u, encoder: %u, crtc: %u, mode: %ux%u\n", 
                                   display->connector_id, display->encoder_id, 
                                   display->crtc_id, display->width, display->height);
                            
                            drmModeFreeEncoder(encoder);
                            drmModeFreeConnector(connector);
                            drmModeFreeResources(resources);
                            return true;
                        }
                    }
                    drmModeFreeEncoder(encoder);
                }
            }
            drmModeFreeConnector(connector);
        }
    }
    
    drmModeFreeResources(resources);
    return false;
}

// 获取plane
static bool get_plane(DrmDisplay *display) {
    drmModePlaneRes* plane_resources = drmModeGetPlaneResources(display->drm_fd);
    if (!plane_resources) {
        fprintf(stderr, "Failed to get plane resources: %s\n", strerror(errno));
        return false;
    }
    
    // 查找支持NV12格式的overlay plane
    for (uint32_t i = 0; i < plane_resources->count_planes; i++) {
        drmModePlane* plane = drmModeGetPlane(display->drm_fd, plane_resources->planes[i]);
        if (plane) {
            // 检查plane是否支持NV12格式和当前CRTC
            bool supports_nv12 = false;
            for (uint32_t j = 0; j < plane->count_formats; j++) {
                if (plane->formats[j] == DRM_FORMAT_NV12) {
                    supports_nv12 = true;
                    break;
                }
            }
            
            // 检查plane是否支持当前CRTC
            bool supports_crtc = (plane->possible_crtcs & (1 << get_crtc_index(display, display->crtc_id)));
            
            if (supports_nv12 && supports_crtc) {
                display->plane_id = plane->plane_id;
                printf("Found NV12 plane: %u (supports current CRTC)\n", display->plane_id);
                drmModeFreePlane(plane);
                drmModeFreePlaneResources(plane_resources);
                return true;
            }
            drmModeFreePlane(plane);
        }
    }
    
    fprintf(stderr, "No plane supporting NV12 format found for current CRTC\n");
    drmModeFreePlaneResources(plane_resources);
    return false;
}

// 设置模式
static bool set_mode(DrmDisplay *display) {
    // 获取connector以获取mode信息
    drmModeConnector* connector = drmModeGetConnector(display->drm_fd, display->connector_id);
    if (!connector || connector->count_modes == 0) {
        fprintf(stderr, "Failed to get connector for mode setting\n");
        return false;
    }
    
    // 查找3840x2160模式
    drmModeModeInfo* mode_ptr = NULL;
    for (int i = 0; i < connector->count_modes; i++) {
        if (connector->modes[i].hdisplay == 1080 && connector->modes[i].vdisplay == 1920) {
            mode_ptr = &connector->modes[i];
            break;
        }
    }
    
    // 如果没有找到4K模式，使用第一个模式
    if (!mode_ptr) {
        mode_ptr = &connector->modes[0];
    }
    
    // 设置CRTC
    int ret = drmModeSetCrtc(display->drm_fd, display->crtc_id, 0, 0, 0, 
                            &display->connector_id, 1, mode_ptr);
    
    if (ret != 0) {
        fprintf(stderr, "Failed to set CRTC mode: %s (crtc_id=%u, connector_id=%u)\n", 
                strerror(errno), display->crtc_id, display->connector_id);
        
        // 如果设置失败，尝试恢复原始CRTC设置
        if (display->saved_crtc) {
            printf("Trying to restore saved CRTC...\n");
            ret = drmModeSetCrtc(display->drm_fd, display->saved_crtc->crtc_id, 
                                display->saved_crtc->buffer_id,
                                display->saved_crtc->x, display->saved_crtc->y, 
                                &display->connector_id, 1,
                                &display->saved_crtc->mode);
            if (ret == 0) {
                display->crtc_id = display->saved_crtc->crtc_id;
                display->width = display->saved_crtc->mode.hdisplay;
                display->height = display->saved_crtc->mode.vdisplay;
                printf("Successfully restored saved CRTC: %ux%u\n", 
                       display->width, display->height);
                drmModeFreeConnector(connector);
                return true;
            }
        }
        drmModeFreeConnector(connector);
        return false;
    }
    
    printf("Successfully set CRTC mode: %ux%u\n", display->width, display->height);
    drmModeFreeConnector(connector);
    return true;
}

// 确保CRTC已设置
static bool ensure_crtc_set(DrmDisplay *display) {
    // 检查CRTC是否已设置
    drmModeCrtc* current_crtc = drmModeGetCrtc(display->drm_fd, display->crtc_id);
    if (current_crtc) {
        bool is_set = (current_crtc->mode_valid != 0);
        drmModeFreeCrtc(current_crtc);
        if (is_set) {
            return true;
        }
    }
    
    // 如果CRTC未设置，尝试设置
    return set_mode(display);
}

// 初始化DRM显示
bool drm_display_init(DrmDisplay *display, const char* device) {
    if (!display) {
        fprintf(stderr, "Invalid display pointer\n");
        return false;
    }
    
    display->drm_fd = open(device, O_RDWR | O_CLOEXEC);
    if (display->drm_fd < 0) {
        fprintf(stderr, "Failed to open DRM device: %s\n", strerror(errno));
        return false;
    }
    
    if (!get_resources(display)) {
        fprintf(stderr, "Failed to get DRM resources\n");
        close(display->drm_fd);
        display->drm_fd = -1;
        return false;
    }
    
    if (!get_plane(display)) {
        fprintf(stderr, "Failed to get plane\n");
        close(display->drm_fd);
        display->drm_fd = -1;
        return false;
    }
    
    if (!set_mode(display)) {
        fprintf(stderr, "Failed to set mode\n");
        close(display->drm_fd);
        display->drm_fd = -1;
        return false;
    }
    
    printf("DRM display initialized: %ux%u\n", display->width, display->height);
    return true;
}

// 扩容缓冲区数组
static bool resize_buffers(DrmDisplay *display) {
    int new_capacity = display->buffer_capacity == 0 ? 4 : display->buffer_capacity * 2;
    buffer_object *new_buffers = (buffer_object*)realloc(display->buffers, 
                                                       new_capacity * sizeof(buffer_object));
    if (!new_buffers) {
        fprintf(stderr, "Failed to resize buffers\n");
        return false;
    }
    
    display->buffers = new_buffers;
    display->buffer_capacity = new_capacity;
    return true;
}

// 创建NV12缓冲区
int drm_display_create_nv12_buffer(DrmDisplay *display, uint32_t width, uint32_t height, 
                                  uint32_t aligned_width, uint32_t aligned_height) {
    if (!display || display->drm_fd < 0) {
        fprintf(stderr, "DRM not initialized\n");
        return -1;
    }
    
    if (aligned_width == 0) {
        aligned_width = ALIGN(width, 32);  // 32字节对齐
    }
    if (aligned_height == 0) {
        aligned_height = ALIGN(height, 16);  // 16行对齐
    }
    
    // 确保缓冲区数组有足够空间
    if (display->buffer_count >= display->buffer_capacity) {
        if (!resize_buffers(display)) {
            return -1;
        }
    }
    
    buffer_object bo = {0};
    bo.width = width;
    bo.height = height;
    
    // 创建gem buffer
    struct drm_mode_create_dumb create_dumb = {};
    create_dumb.width = aligned_width;
    create_dumb.height = aligned_height * 3 / 2;  // NV12包含Y和UV平面
    create_dumb.bpp = 8;  // 每个像素8bit
    
    if (drmIoctl(display->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_dumb) < 0) {
        fprintf(stderr, "Failed to create dumb buffer: %s\n", strerror(errno));
        return -1;
    }
    
    bo.handle = create_dumb.handle;
    bo.pitch = create_dumb.pitch;
    bo.size = create_dumb.size;
    
    // 映射buffer到用户空间
    struct drm_mode_map_dumb map_dumb = {};
    map_dumb.handle = bo.handle;
    if (drmIoctl(display->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_dumb) < 0) {
        fprintf(stderr, "Failed to map dumb buffer: %s\n", strerror(errno));
        struct drm_mode_destroy_dumb destroy_dumb = {};
        destroy_dumb.handle = bo.handle;
        drmIoctl(display->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);
        return -1;
    }
    
    bo.map = (uint8_t*)mmap(0, bo.size, PROT_READ | PROT_WRITE, 
                           MAP_SHARED, display->drm_fd, map_dumb.offset);
    if (bo.map == MAP_FAILED) {
        fprintf(stderr, "Failed to mmap buffer: %s\n", strerror(errno));
        struct drm_mode_destroy_dumb destroy_dumb = {};
        destroy_dumb.handle = bo.handle;
        drmIoctl(display->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);
        return -1;
    }
    
    // 创建framebuffer
    uint32_t handles[4] = {bo.handle, bo.handle, 0, 0};
    uint32_t pitches[4] = {bo.pitch, bo.pitch, 0, 0};
    uint32_t offsets[4] = {0, aligned_width * aligned_height, 0, 0};  // Y在0偏移，UV在y_size偏移
    
    if (drmModeAddFB2(display->drm_fd, aligned_width, aligned_height, 
                     DRM_FORMAT_NV12, handles, pitches, offsets, &bo.fb_id, 0)) {
        fprintf(stderr, "Failed to add framebuffer: %s\n", strerror(errno));
        munmap(bo.map, bo.size);
        struct drm_mode_destroy_dumb destroy_dumb = {};
        destroy_dumb.handle = bo.handle;
        drmIoctl(display->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);
        return -1;
    }
    
    display->buffers[display->buffer_count] = bo;
    int buffer_index = display->buffer_count;
    display->buffer_count++;
    
    printf("Created NV12 buffer: %ux%u (aligned: %ux%u, pitch: %u)\n", 
           width, height, aligned_width, aligned_height, bo.pitch);
    return buffer_index;
}

// 写入NV12数据到缓冲区
bool drm_display_write_nv12_data(DrmDisplay *display, int buffer_index, 
                                const uint8_t* nv12_data, 
                                uint32_t data_width, uint32_t data_height 
                                ) {
    if (!display || buffer_index < 0 || buffer_index >= display->buffer_count) {
        fprintf(stderr, "Invalid buffer index\n");
        return false;
    }
    
    
    int     data_stride = data_width;
    
    
    buffer_object *bo = &display->buffers[buffer_index];
    uint32_t aligned_width = bo->pitch;
    uint32_t aligned_height = bo->size * 2 / (3 * bo->pitch);  // 计算实际的高度
    
    // 复制Y平面
    const uint8_t* y_plane = nv12_data;
    uint8_t* dst_y = bo->map;
    for (uint32_t i = 0; i < data_height; i++) {
        memcpy(dst_y + i * aligned_width, y_plane + i * data_stride, data_width);
    }
    
    // 复制UV平面
    const uint8_t* uv_plane = nv12_data + data_height * data_stride;
    uint8_t* dst_uv = bo->map + aligned_width * aligned_height;
    for (uint32_t i = 0; i < data_height / 2; i++) {
        memcpy(dst_uv + i * aligned_width, uv_plane + i * data_stride, data_width);
    }
    
    return true;
}

// 从文件读取NV12数据并写入缓冲区
bool drm_display_write_nv12_from_file(DrmDisplay *display, int buffer_index, 
                                     const uint8_t* nv12_data, 
                                     uint32_t width, uint32_t height) {
#if 0

    // 计算NV12文件大小
    size_t file_size = width * height * 3 / 2;
    
    // 打开文件
    FILE *file = fopen(filename, "rb");
    if (!file) {
        fprintf(stderr, "Failed to open file: %s\n", filename);
        return false;
    }
    
    // 获取文件大小
    fseek(file, 0, SEEK_END);
    size_t actual_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    if (actual_size < file_size) {
        fprintf(stderr, "File size too small: %zu < %zu\n", actual_size, file_size);
        fclose(file);
        return false;
    }
    
    // 读取数据
    uint8_t *nv12_data = (uint8_t*)malloc(file_size);
    if (!nv12_data) {
        fprintf(stderr, "Failed to allocate memory for NV12 data\n");
        fclose(file);
        return false;
    }
    
    size_t read_size = fread(nv12_data, 1, file_size, file);
    if (read_size != file_size) {
        fprintf(stderr, "Failed to read file data\n");
        free(nv12_data);
        fclose(file);
        return false;
    }
    
    fclose(file);
    
    printf("Successfully read NV12 file: %s (%zu bytes)\n", filename, file_size);
#endif
    // 写入到缓冲区
    bool result = drm_display_write_nv12_data(display, buffer_index, 
                                           nv12_data, width, height);
    
   // free(nv12_data);
    return result;
}




// 全屏显示缓冲区数据
bool drm_display_display_buffer_fullscreen(DrmDisplay *display, int buffer_index, 
                                         bool scale_to_fit) {
    if (!display || buffer_index < 0 || buffer_index >= display->buffer_count) {
        fprintf(stderr, "Invalid buffer index\n");
        return false;
    }
    
    buffer_object *bo = &display->buffers[buffer_index];
    
    // 确保CRTC已设置
    if (!ensure_crtc_set(display)) {
        fprintf(stderr, "Failed to ensure CRTC is set\n");
        return false;
    }
    
    uint32_t src_x = 0, src_y = 0;
    uint32_t src_w = bo->width, src_h = bo->height;
    uint32_t dst_x = 0, dst_y = 0;
    uint32_t dst_w = display->width, dst_h = display->height;
    
    if (!scale_to_fit) {
        // 保持原始大小显示
        if (bo->width <= display->width && bo->height <= display->height) {
            dst_x = (display->width - bo->width) / 2;
            dst_y = (display->height - bo->height) / 2;
            dst_w = bo->width;
            dst_h = bo->height;
        } else {
            // 如果图像大于屏幕，按比例缩放到屏幕
            float scale_x = (float)display->width / bo->width;
            float scale_y = (float)display->height / bo->height;
            float scale = scale_x < scale_y ? scale_x : scale_y;
            
            dst_w = (uint32_t)(bo->width * scale);
            dst_h = (uint32_t)(bo->height * scale);
            dst_x = (display->width - dst_w) / 2;
            dst_y = (display->height - dst_h) / 2;
        }
    }
    
    // 使用plane全屏显示
    int ret = drmModeSetPlane(display->drm_fd, display->plane_id, display->crtc_id, 
                             bo->fb_id, 0,
                             dst_x, dst_y, dst_w, dst_h,
                             src_x << 16, src_y << 16, src_w << 16, src_h << 16);
    if (ret != 0) {
        fprintf(stderr, "Failed to set plane: %s (plane_id=%u, crtc_id=%u, fb_id=%u)\n", 
                strerror(-ret), display->plane_id, display->crtc_id, bo->fb_id);
        return false;
    }
    
    printf("Displaying fullscreen: %ux%u -> %ux%u at (%u,%u)\n", 
           bo->width, bo->height, dst_w, dst_h, dst_x, dst_y);
    return true;
}

// 获取屏幕分辨率
void drm_display_get_resolution(DrmDisplay *display, uint32_t* width, uint32_t* height) {
    if (display && width && height) {
        *width = display->width;
        *height = display->height;
    }
}

// 获取缓冲区信息
bool drm_display_get_buffer_info(DrmDisplay *display, int buffer_index, 
                               uint32_t* width, uint32_t* height, uint32_t* pitch) {
    if (!display || buffer_index < 0 || buffer_index >= display->buffer_count) {
        return false;
    }
    
    buffer_object *bo = &display->buffers[buffer_index];
    if (width) *width = bo->width;
    if (height) *height = bo->height;
    if (pitch) *pitch = bo->pitch;
    return true;
}

// 获取DRM文件描述符
int drm_display_get_drm_fd(DrmDisplay *display) {
    return display ? display->drm_fd : -1;
}

static DrmDisplay *display = NULL;
static int buffer_index;
    static int drm_init_flags = 0;
// 使用示例
int drm_init(void) {
    DrmDisplay *display = drm_display_create();
    if (!display) {
        fprintf(stderr, "Failed to create display\n");
        return -1;
    }
    
    if (!drm_display_init(display, "/dev/dri/card0")) {
        fprintf(stderr, "Failed to initialize display\n");
        drm_display_destroy(display);
        return -1;
    }
    
    uint32_t screen_width, screen_height;
    drm_display_get_resolution(display, &screen_width, &screen_height);
    printf("Screen resolution: %ux%u\n", screen_width, screen_height);
    
       
    // 获取缓冲区信息
  //  uint32_t buf_width, buf_height, pitch;
    //if (drm_display_get_buffer_info(display, buffer_index, &buf_width, &buf_height, &pitch)) {
      //  printf("Buffer info: %ux%u (pitch: %u)\n", buf_width, buf_height, pitch);
   // }
    
    // 从文件读取NV12数据并全屏显示
   // const char *nv12_file = "test_1920x1080.nv12";
   
#if 0
    else {
        fprintf(stderr, "Failed to read NV12 file\n");
        
        // 如果文件读取失败，使用灰色背景作为fallback
        printf("Using gray background as fallback\n");
        size_t data_size = 1920 * 1080 * 3 / 2;
        uint8_t *nv12_data = (uint8_t*)malloc(data_size);
        if (nv12_data) {
            memset(nv12_data, 0x80, data_size);
            if (drm_display_write_nv12_data(display, buffer_index, nv12_data, 1920, 1080)) {
                drm_display_display_buffer_fullscreen(display, buffer_index, true);
            }
            free(nv12_data);
        }
    }
    
    // 等待用户输入
    printf("Press Enter to exit...\n");
    getchar();
#endif
    drm_init_flags = 1;
    return 0;
}


void drm_display(uint8_t *nv12_data, int w, int h) {

    if (drm_init_flags == 0)
        drm_init();
 // 创建NV12缓冲区用于1920x1080视频
    buffer_index = drm_display_create_nv12_buffer(display, 1920, 1080, 0, 0);
    if (buffer_index < 0) {
        fprintf(stderr, "Failed to create buffer\n");
        drm_display_destroy(display);
        return -1;
    }


    if (drm_display_write_nv12_from_file(display, buffer_index, nv12_data, w, h)) {
        // 全屏显示并自动适应屏幕大小
        if (drm_display_display_buffer_fullscreen(display, buffer_index, true)) {
        } else {
            fprintf(stderr, "Failed to display buffer fullscreen\n");
        }
    }
}

void drm_destory(void) {
    if (display) {
        drm_display_destroy(display);
        display = NULL;
    }
}
