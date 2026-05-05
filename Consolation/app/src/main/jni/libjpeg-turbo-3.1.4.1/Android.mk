LOCAL_PATH := $(call my-dir)

LIBJPEG_TURBO_3141_INCLUDES := \
	$(LOCAL_PATH)/android/include \
	$(LOCAL_PATH)/android/simd/arm \
	$(LOCAL_PATH)/src \
	$(LOCAL_PATH)/simd \
	$(LOCAL_PATH)/simd/arm

LIBJPEG_TURBO_3141_SOURCES := \
	src/jcapimin.c \
	src/wrapper/jcapistd-8.c \
	src/wrapper/jcapistd-12.c \
	src/wrapper/jcapistd-16.c \
	src/wrapper/jccoefct-8.c \
	src/wrapper/jccoefct-12.c \
	src/wrapper/jccolor-8.c \
	src/wrapper/jccolor-12.c \
	src/wrapper/jccolor-16.c \
	src/wrapper/jcdctmgr-8.c \
	src/wrapper/jcdctmgr-12.c \
	src/wrapper/jcdiffct-8.c \
	src/wrapper/jcdiffct-12.c \
	src/wrapper/jcdiffct-16.c \
	src/jchuff.c \
	src/jcicc.c \
	src/jcinit.c \
	src/jclhuff.c \
	src/wrapper/jclossls-8.c \
	src/wrapper/jclossls-12.c \
	src/wrapper/jclossls-16.c \
	src/wrapper/jcmainct-8.c \
	src/wrapper/jcmainct-12.c \
	src/wrapper/jcmainct-16.c \
	src/jcmarker.c \
	src/jcmaster.c \
	src/jcomapi.c \
	src/jcparam.c \
	src/jcphuff.c \
	src/wrapper/jcprepct-8.c \
	src/wrapper/jcprepct-12.c \
	src/wrapper/jcprepct-16.c \
	src/wrapper/jcsample-8.c \
	src/wrapper/jcsample-12.c \
	src/wrapper/jcsample-16.c \
	src/jctrans.c \
	src/jdapimin.c \
	src/wrapper/jdapistd-8.c \
	src/wrapper/jdapistd-12.c \
	src/wrapper/jdapistd-16.c \
	src/jdatadst.c \
	src/jdatasrc.c \
	src/wrapper/jdcoefct-8.c \
	src/wrapper/jdcoefct-12.c \
	src/wrapper/jdcolor-8.c \
	src/wrapper/jdcolor-12.c \
	src/wrapper/jdcolor-16.c \
	src/wrapper/jddctmgr-8.c \
	src/wrapper/jddctmgr-12.c \
	src/wrapper/jddiffct-8.c \
	src/wrapper/jddiffct-12.c \
	src/wrapper/jddiffct-16.c \
	src/jdhuff.c \
	src/jdicc.c \
	src/jdinput.c \
	src/jdlhuff.c \
	src/wrapper/jdlossls-8.c \
	src/wrapper/jdlossls-12.c \
	src/wrapper/jdlossls-16.c \
	src/wrapper/jdmainct-8.c \
	src/wrapper/jdmainct-12.c \
	src/wrapper/jdmainct-16.c \
	src/jdmarker.c \
	src/jdmaster.c \
	src/wrapper/jdmerge-8.c \
	src/wrapper/jdmerge-12.c \
	src/jdphuff.c \
	src/wrapper/jdpostct-8.c \
	src/wrapper/jdpostct-12.c \
	src/wrapper/jdpostct-16.c \
	src/wrapper/jdsample-8.c \
	src/wrapper/jdsample-12.c \
	src/wrapper/jdsample-16.c \
	src/jdtrans.c \
	src/jerror.c \
	src/jfdctflt.c \
	src/wrapper/jfdctfst-8.c \
	src/wrapper/jfdctfst-12.c \
	src/wrapper/jfdctint-8.c \
	src/wrapper/jfdctint-12.c \
	src/wrapper/jidctflt-8.c \
	src/wrapper/jidctflt-12.c \
	src/wrapper/jidctfst-8.c \
	src/wrapper/jidctfst-12.c \
	src/wrapper/jidctint-8.c \
	src/wrapper/jidctint-12.c \
	src/wrapper/jidctred-8.c \
	src/wrapper/jidctred-12.c \
	src/jmemmgr.c \
	src/jmemnobs.c \
	src/jpeg_nbits.c \
	src/wrapper/jquant1-8.c \
	src/wrapper/jquant1-12.c \
	src/wrapper/jquant2-8.c \
	src/wrapper/jquant2-12.c \
	src/wrapper/jutils-8.c \
	src/wrapper/jutils-12.c \
	src/wrapper/jutils-16.c \
	src/jaricom.c \
	src/jcarith.c \
	src/jdarith.c

LIBJPEG_TURBO_3141_ARM_NEON_SOURCES := \
	simd/arm/jcgray-neon.c \
	simd/arm/jcphuff-neon.c \
	simd/arm/jcsample-neon.c \
	simd/arm/jdmerge-neon.c \
	simd/arm/jdsample-neon.c \
	simd/arm/jfdctfst-neon.c \
	simd/arm/jidctred-neon.c \
	simd/arm/jquanti-neon.c \
	simd/arm/jccolor-neon.c \
	simd/arm/jidctint-neon.c \
	simd/arm/jidctfst-neon.c \
	simd/arm/jdcolor-neon.c \
	simd/arm/jfdctint-neon.c

include $(CLEAR_VARS)

LOCAL_MODULE := jpeg-turbo3141_static
LOCAL_C_INCLUDES := $(LIBJPEG_TURBO_3141_INCLUDES)
LOCAL_EXPORT_C_INCLUDES := $(LIBJPEG_TURBO_3141_INCLUDES)
LOCAL_CFLAGS := $(LOCAL_C_INCLUDES:%=-I%)
LOCAL_CFLAGS += -DANDROID_NDK -DANDROID -DNEON_INTRINSICS
LOCAL_ARM_MODE := arm
LOCAL_SRC_FILES := $(LIBJPEG_TURBO_3141_SOURCES)

ifeq ($(TARGET_ARCH_ABI),armeabi-v7a)
LOCAL_ARM_NEON := true
LOCAL_SRC_FILES += \
	$(LIBJPEG_TURBO_3141_ARM_NEON_SOURCES) \
	simd/arm/aarch32/jchuff-neon.c \
	simd/arm/aarch32/jsimd.c
else ifeq ($(TARGET_ARCH_ABI),arm64-v8a)
LOCAL_SRC_FILES += \
	$(LIBJPEG_TURBO_3141_ARM_NEON_SOURCES) \
	simd/arm/aarch64/jchuff-neon.c \
	simd/arm/aarch64/jsimd.c
else
LOCAL_SRC_FILES += src/jsimd_none.c
endif

LOCAL_DISABLE_FATAL_LINKER_WARNINGS := true

include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)

LOCAL_MODULE := jpeg-turbo3141
LOCAL_EXPORT_C_INCLUDES := $(LIBJPEG_TURBO_3141_INCLUDES)
LOCAL_LDLIBS := -L$(SYSROOT)/usr/lib -ldl
LOCAL_WHOLE_STATIC_LIBRARIES := jpeg-turbo3141_static
LOCAL_DISABLE_FATAL_LINKER_WARNINGS := true

include $(BUILD_SHARED_LIBRARY)
