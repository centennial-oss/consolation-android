# Android build config for libusb-1.0.29
# Adapted from the legacy libusb/android/jni/libusb.mk for stock libusb-1.0.29.
#
# Key differences from the legacy (1.0.18) Android port:
#   - Uses linux_usbfs.c / linux_netlink.c / events_posix.c instead of the
#     Android-specific android_usbfs.c / android_netlink.c / poll_posix.c.
#   - No LIBUSB_OPTION_NO_DEVICE_DISCOVERY flag needed at build time;
#     callers must pass it to libusb_init_context() at runtime.
#   - Module names (libusb100_static / libusb100) are preserved so that the
#     rest of the build system needs no module-name changes.

######################################################################
# libusb100_static.a
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

LOCAL_MODULE := libusb100_static
include $(BUILD_STATIC_LIBRARY)

######################################################################
# libusb100.so
######################################################################
include $(CLEAR_VARS)
LOCAL_MODULE_TAGS := optional
LOCAL_EXPORT_LDLIBS += -llog

LOCAL_WHOLE_STATIC_LIBRARIES = libusb100_static

LOCAL_MODULE := libusb100
include $(BUILD_SHARED_LIBRARY)
