# Android build config for libusb-1.0.30
# Adapted from the legacy libusb/android/jni/libusb.mk for stock libusb-1.0.30.
#
# Key differences from the upstream Android port:
#   - Uses linux_usbfs.c / linux_netlink.c / events_posix.c instead of the
#     Android-specific android_usbfs.c / android_netlink.c / poll_posix.c.
#   - No LIBUSB_OPTION_NO_DEVICE_DISCOVERY flag needed at build time;
#     callers must pass it to libusb_init_context() at runtime.
#   - Module names (libusb1_static / libusb1) are used consistently across
#     the build system and Java loadLibrary() calls.

######################################################################
# libusb1_static.a
######################################################################
LOCAL_PATH := $(call my-dir)/../..
include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
	libusb/core.c \
	libusb/descriptor.c \
	libusb/hotplug.c \
	libusb/io.c \
	libusb/sync.c \
	libusb/strerror.c \
	libusb/os/linux_usbfs.c \
	libusb/os/events_posix.c \
	libusb/os/threads_posix.c \
	libusb/os/linux_netlink.c

LOCAL_C_INCLUDES += \
	$(LOCAL_PATH)/libusb \
	$(LOCAL_PATH)/libusb/os \
	$(LOCAL_PATH)/android

LOCAL_EXPORT_C_INCLUDES := \
	$(LOCAL_PATH)/

LOCAL_CFLAGS := $(LOCAL_C_INCLUDES:%=-I%)
LOCAL_CFLAGS += -DANDROID_NDK
LOCAL_CFLAGS += -DLOG_NDEBUG
LOCAL_CFLAGS += -O3 -fstrict-aliasing -fprefetch-loop-arrays
LOCAL_EXPORT_LDLIBS += -llog
LOCAL_ARM_MODE := arm

LOCAL_MODULE := libusb1_static
include $(BUILD_STATIC_LIBRARY)

######################################################################
# libusb1.so
######################################################################
include $(CLEAR_VARS)
LOCAL_MODULE_TAGS := optional
LOCAL_EXPORT_LDLIBS += -llog

LOCAL_WHOLE_STATIC_LIBRARIES = libusb1_static

LOCAL_MODULE := libusb1
include $(BUILD_SHARED_LIBRARY)
