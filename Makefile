# Define cross compiler
CURR_DIR=$(shell pwd)
SDK_HOST_DIR=/home/lsc/disk6/rk3576/rk3576_lyrg/buildroot/output/rockchip_rk3576/host
SDK_SYSROOT_DIR=${SDK_HOST_DIR}/aarch64-buildroot-linux-gnu/sysroot

# 指定编译器路径
CROSS_COMPILE=${SDK_HOST_DIR}/bin/aarch64-buildroot-linux-gnu-
CC=${CROSS_COMPILE}gcc
CXX=${CROSS_COMPILE}g++
LD=${CROSS_COMPILE}ld
AR=${CROSS_COMPILE}ar
NM=${CROSS_COMPILE}nm

# gcc make flags
CFLAGS = -Wall -O3 -g -Wno-unused-variable -Wno-unused-function -Wno-deprecated-declarations -fdiagnostics-color=auto
CXXFLAGS = $(CFLAGS)

## 以下是编译需要的头文件
# linux public
INCLUDE = -I${SDK_SYSROOT_DIR}/usr/include
INCLUDE += -I${SDK_SYSROOT_DIR}/usr/include/libdrm
#INCLUDE += -I${SDK_SYSROOT_DIR}/usr/include/rockchip
# app private
INCLUDE += -I${CURR_DIR}/include
INCLUDE += -I${CURR_DIR}/src/include
INCLUDE += -I${CURR_DIR}/src/
INCLUDE += -I${CURR_DIR}/src/ai

## 以下是需要的库文件
# linux public
LIBS = -L${SDK_SYSROOT_DIR}/usr/lib
# app private
LIBS += -L${CURR_DIR}/lib

## 使用动态库
LIBS += -lavformat -lavutil -lavcodec -lrockit -lrtsp -lrknn_api -lrga
LIBS += -lgassdetec -lrga_crop_demo
LIBS += -lopencv_core -lopencv_imgproc
## 使用静态库
LIBS += -lrkdemuxer -lrkaudio

# 以下是源文件
SRCS = \
	   src/ai/camera_ai.c \
	   src/camera_mp4Muxer.c \
	   src/camera_drm.c \
	   src/camera_config.c \
	   src/camera_sem.c \
	   src/camera_mpp_api.c \
	   src/camera_mpp_cam.c \
	   src/camera_takepicture.c \
	   src/camera_recorder.c \
	   src/camera_rtsp.c \
	   src/camera_rtmp.c \
	   src/gas_quantification.c \
	   src/palette.c \
	   src/rkdrm_display.c \
	   src/display.c \
	   src/drmDsp/dev.c \
	   src/drmDsp/modeset.c \
	   src/drmDsp/bo.c \
	   src/rga_control.c \
	   src/mp3_encoder.c \
	   src/camera_main.c
CPPSRCS = \
	   src/ai/postprocess.cpp \
	   src/ai/yolo.cpp \
	   src/rga_rotate_cache.cpp

# 库名称和版本号
ifneq ($(BUILD_NAME),)
LIB_NAME=$(BUILD_NAME)
else
LIB_NAME=raincamera
endif

ifneq ($(BUILD_VERSION),)
LIB_VERSION=$(BUILD_VERSION)
else
LIB_VERSION=0.0.0
endif

# 获取GIT版本信息和路径（在代码中使用宏GIT_VERSION和GIT_PATH）
GIT_VER_INFO=$(shell git rev-parse --short HEAD)
GIT_SVR_PATH=$(shell git remote -v | awk 'NR==1' | sed 's/[()]//g' | sed 's/\t/ /g' |cut -d " " -f2)

# 定义GIT版本信息宏和路径宏，CFLAGS是编译选项变量
ifneq ($(GIT_VER_INFO),)
RCFLAGS += -DGIT_VERSION=\"$(GIT_VER_INFO)\"
else
RCFLAGS += -DGIT_VERSION=\"unknown\"
endif
RCFLAGS += -DGIT_PATH=\"$(GIT_SVR_PATH)\"
RCFLAGS += -DRAIN_CAMERA_VERSION=\"$(LIB_VERSION)\"

# 源文件和目标文件
OBJS = $(SRCS:.c=.o)
CPPOBJS = $(CPPSRCS:.cpp=.o)

# 静态库和动态库名称
STATIC_LIB=lib$(LIB_NAME).a
DYNAMIC_LIB=lib$(LIB_NAME).so.$(LIB_VERSION)
AI_CPP=ai

# 默认目标
all: $(STATIC_LIB) $(DYNAMIC_LIB)

$(AI_CPP): $(CPPOBJS)
	$(CXX) $(CPPOBJS) -o rainai $(LIBS)

# 静态库
$(STATIC_LIB): $(OBJS) $(CPPOBJS)
	$(AR) -rcs $@ $^

# 动态库
$(DYNAMIC_LIB): $(OBJS) $(CPPOBJS)
	$(CC) -shared -fPIC -o $@ $^
	$(CC) -shared -fPIC -o $@ $^ $(LIBS)
	ln -sf $(DYNAMIC_LIB) lib$(LIB_NAME).so

# 编译源文件
%.o : %.c
	$(CC) -o $@ $< -c -fPIC $(CFLAGS) $(RCFLAGS) $(INCLUDE)

%.o : %.cpp
	$(CXX) -o $@ $< -c -fPIC $(CXXFLAGS) $(RCFLAGS) $(INCLUDE)

# 清理
clean:
	rm -rf $(OBJS) $(CPPOBJS) lib$(LIB_NAME).* rainai
