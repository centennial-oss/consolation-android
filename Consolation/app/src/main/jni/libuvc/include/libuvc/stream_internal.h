#ifndef LIBUVC_STREAM_INTERNAL_H
#define LIBUVC_STREAM_INTERNAL_H

#include "libuvc/libuvc.h"
#include "libuvc/libuvc_internal.h"
#include <libusb/libusb.h>

struct libusb_interface;

void _uvc_stream_try_acquire_outbuf(uvc_stream_handle_t *strmh);
void _uvc_swap_buffers(uvc_stream_handle_t *strmh, const char *reason);

int _uvc_mjpeg_payload_has_markers(const uvc_stream_handle_t *strmh);
void _uvc_diag_mjpeg_drop(uvc_stream_handle_t *strmh, const char *reason);
void _uvc_diag_mjpeg_publish(uvc_stream_handle_t *strmh, const char *reason);
void _uvc_diag_mjpeg_log_stream_start(const uvc_stream_ctrl_t *ctrl,
		const uvc_frame_desc_t *frame_desc);
void _uvc_diag_first_payload(uvc_stream_handle_t *strmh,
		size_t nbytes, const char *path);

void _uvc_process_payload_bulk(uvc_stream_handle_t *strmh,
		const uint8_t *payload, size_t payload_len);
void _uvc_process_payload_iso(uvc_stream_handle_t *strmh,
		struct libusb_transfer *transfer);

void _uvc_stream_callback(struct libusb_transfer *transfer);

void _uvc_free_transfer(uvc_stream_handle_t *strmh, int transfer_id);

#ifndef LIBUVC_STREAM_XFER_TIMEOUT_MS
#define LIBUVC_STREAM_XFER_TIMEOUT_MS 0
#endif

uvc_error_t _uvc_stream_setup_iso_transfers(uvc_stream_handle_t *strmh,
		const struct libusb_interface *interface,
		uvc_format_desc_t *format_desc,
		uint32_t dwMaxVideoFrameSize,
		float bandwidth_factor);

uvc_error_t _uvc_stream_setup_bulk_transfers(uvc_stream_handle_t *strmh,
		uvc_format_desc_t *format_desc);

#endif /* LIBUVC_STREAM_INTERNAL_H */
