# This is the Android makefile for libyuv for NDK.
LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_CPP_EXTENSION := .cc

LOCAL_SRC_FILES := \
    source/convert_argb.cc      \
    source/cpu_id.cc            \
    source/planar_functions.cc  \
    source/row_any.cc           \
    source/row_common.cc        \
    source/row_gcc.cc           \
    source/row_neon.cc          \
    source/row_neon64.cc        \
    source/scale_any.cc         \
    source/scale_common.cc      \
    source/scale_gcc.cc         \
    source/scale_neon.cc        \
    source/scale_neon64.cc      \
    source/scale_uv.cc

common_CFLAGS := -Wall -fexceptions -DLIBYUV_DISABLE_SVE -DLIBYUV_DISABLE_SME
ifeq ($(TARGET_ARCH_ABI),arm64-v8a)
common_CFLAGS += -march=armv8.2-a+dotprod+i8mm
endif

LOCAL_CFLAGS += $(common_CFLAGS)
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/include
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include
LOCAL_EXPORT_C_INCLUDE_DIRS := $(LOCAL_PATH)/include

LOCAL_MODULE := libyuv_static
LOCAL_MODULE_TAGS := optional

include $(BUILD_STATIC_LIBRARY)
