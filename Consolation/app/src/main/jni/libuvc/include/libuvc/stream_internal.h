#ifndef LIBUVC_STREAM_INTERNAL_H
#define LIBUVC_STREAM_INTERNAL_H

#include "libuvc/libuvc.h"
#include "libuvc/libuvc_internal.h"
#include <libusb/libusb.h>
#include "libuvc/stream_diag.h"

struct libusb_interface;

void _uvc_stream_try_acquire_outbuf(uvc_stream_handle_t *strmh);
void _uvc_swap_buffers(uvc_stream_handle_t *strmh, const char *reason);

int _uvc_mjpeg_payload_has_markers(uvc_stream_handle_t *strmh);
/* Returns 1 when the MJPEG frame under assembly now ends with EOI (FFD9). */
int _uvc_mjpeg_note_payload_append(uvc_stream_handle_t *strmh);
/* EOI seen: publish the assembled MJPEG frame now when its UVC status is
 * final (EOF in this payload's header); otherwise hold it until EOF, an FID
 * flip or the next SOI.  Call right after a successful append that returned 1. */
void _uvc_mjpeg_publish_on_eoi(uvc_stream_handle_t *strmh, uint8_t header_info,
		const char *reason);
/* 1 while a complete frame is held awaiting its trailing status. */
int _uvc_mjpeg_eoi_pending(const uvc_stream_handle_t *strmh);
/* 1 if this payload belongs to a frame already completed on EOI: drop it.
 * Also resolves a frame held on EOI (applying a trailing ERR bit). */
int _uvc_mjpeg_payload_after_eoi(uvc_stream_handle_t *strmh, uint8_t header_info,
		const uint8_t *data, size_t data_len);
void _uvc_mjpeg_scan_reset(uvc_stream_handle_t *strmh);
/* Report bytes appended to the frame under assembly (USB thread). */
void _uvc_asm_report(uvc_stream_handle_t *strmh, int complete);
/* The frame under assembly is being dropped: tell the listener, close it. */
void _uvc_asm_discard(uvc_stream_handle_t *strmh);
void _uvc_diag_mjpeg_drop(uvc_stream_handle_t *strmh, const char *reason);
void _uvc_diag_mjpeg_publish(uvc_stream_handle_t *strmh, const char *reason);
void _uvc_diag_mjpeg_log_stream_start(const uvc_stream_ctrl_t *ctrl,
		const uvc_frame_desc_t *frame_desc);
void _uvc_diag_iso_frame_reset(uvc_stream_handle_t *strmh);

void _uvc_process_payload_bulk(uvc_stream_handle_t *strmh,
		const uint8_t *payload, size_t payload_len);
void _uvc_process_payload_iso(uvc_stream_handle_t *strmh,
		struct libusb_transfer *transfer);

void _uvc_stream_callback(struct libusb_transfer *transfer);

void _uvc_free_transfer(uvc_stream_handle_t *strmh, int transfer_id);

/** Transfer timeout for libusb_fill_{bulk,iso}_transfer (see stream_diag.c). Default 0 (infinite). */
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
