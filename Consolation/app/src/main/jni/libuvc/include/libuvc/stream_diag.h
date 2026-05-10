#ifndef LIBUVC_STREAM_DIAG_H
#define LIBUVC_STREAM_DIAG_H

#ifdef __ANDROID__
#include <android/log.h>
#endif

#include "libuvc/stream_log.h"
#include "libuvc/libuvc.h"
#include "libuvc/libuvc_internal.h"
#include <libusb/libusb.h>

#ifndef UVC_RUNTIME_DIAG_ENABLED
#if defined(NDEBUG)
#define UVC_RUNTIME_DIAG_ENABLED 0
#else
#define UVC_RUNTIME_DIAG_ENABLED 1
#endif
#endif

#if UVC_RUNTIME_DIAG_ENABLED
#if defined(__ANDROID__)
#define UVC_DIAG_LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#else
#define UVC_DIAG_LOGI(...) LOGI(__VA_ARGS__)
#endif
#else
#define UVC_DIAG_LOGI(...)
#endif

uint64_t uvc_diag_now_ns(void);
void _uvc_xfer_diag_arm(uvc_stream_handle_t *strmh);
void _uvc_diag_first_xfer_completed(uvc_stream_handle_t *strmh,
		struct libusb_transfer *xfer);
void _uvc_diag_first_xfer_issue(uvc_stream_handle_t *strmh,
		struct libusb_transfer *xfer);
void _uvc_diag_first_swap(uvc_stream_handle_t *strmh);
void _uvc_diag_bulk_timeout_before_payload(uvc_stream_handle_t *strmh,
		struct libusb_transfer *transfer);
void _uvc_diag_first_payload(uvc_stream_handle_t *strmh,
		size_t nbytes, const char *path);

#endif /* LIBUVC_STREAM_DIAG_H */
