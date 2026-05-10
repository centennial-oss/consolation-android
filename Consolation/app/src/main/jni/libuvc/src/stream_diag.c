/*********************************************************************
 * Stream path diagnostics (startup timing, first payload, MJPEG runtime, etc.)
 *********************************************************************/

#include <time.h>

#include "libuvc/stream_diag.h"
#include "libuvc/stream_internal.h"

uint64_t uvc_diag_now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static const char *stream_libusb_xfer_status_str(int status) {
	switch (status) {
	case 0: return "COMPLETED";
	case 1: return "ERROR";
	case 2: return "TIMED_OUT";
	case 3: return "CANCELLED";
	case 4: return "STALL";
	case 5: return "NO_DEVICE";
	case 6: return "OVERFLOW";
	default: return "UNKNOWN";
	}
}

void _uvc_xfer_diag_arm(uvc_stream_handle_t *strmh) {
	strmh->diag_xfer_epoch_ns = uvc_diag_now_ns();
	strmh->diag_logged_first_xfer_done = 0;
	strmh->diag_logged_first_xfer_issue = 0;
	strmh->diag_logged_first_payload = 0;
	strmh->diag_logged_first_swap = 0;
	strmh->diag_bulk_timeout_count_before_payload = 0;
	LOGI(
		"startup-diag:libuvc xfer submit arm ep=0x%02x xfer_timeout_ms=%u "
		"(no log lines until a usb callback: complete, timeout, or error)",
		strmh->stream_if ? strmh->stream_if->bEndpointAddress : 0,
		(unsigned)LIBUVC_STREAM_XFER_TIMEOUT_MS);
}

void _uvc_diag_first_xfer_completed(uvc_stream_handle_t *strmh,
		struct libusb_transfer *xfer) {
	if (!strmh->diag_xfer_epoch_ns || strmh->diag_logged_first_xfer_done)
		return;
	strmh->diag_logged_first_xfer_done = 1;
	const uint64_t ms =
		(uvc_diag_now_ns() - strmh->diag_xfer_epoch_ns) / 1000000ULL;
	if (!xfer->num_iso_packets) {
		LOGI(
			"startup-diag:libuvc first xfer COMPLETED bulk actual_length=%u after %llu ms%s",
			(unsigned)xfer->actual_length,
			(unsigned long long)ms,
			xfer->actual_length < (unsigned)(strmh->cur_ctrl.dwMaxPayloadTransferSize / 4)
				? " (!=full maxPacket; bulk IN waits up to xfer_timeout_ms for data)"
				: "");
	} else {
		unsigned nonempty = 0;
		unsigned total_raw = 0;
		int pk;
		for (pk = 0; pk < xfer->num_iso_packets; ++pk) {
			const struct libusb_iso_packet_descriptor *pkt =
				xfer->iso_packet_desc + pk;
			total_raw += (unsigned)pkt->actual_length;
			if (pkt->actual_length)
				nonempty++;
		}
		LOGI(
			"startup-diag:libuvc first xfer COMPLETED iso pkts=%u nonempty=%u "
			"total_raw_bytes=%u after %llu ms",
			(unsigned)xfer->num_iso_packets,
			nonempty,
			total_raw,
			(unsigned long long)ms);
	}
}

void _uvc_diag_first_xfer_issue(uvc_stream_handle_t *strmh,
		struct libusb_transfer *xfer) {
	if (!strmh->diag_xfer_epoch_ns || strmh->diag_logged_first_xfer_issue)
		return;
	strmh->diag_logged_first_xfer_issue = 1;
	const uint64_t ms =
		(uvc_diag_now_ns() - strmh->diag_xfer_epoch_ns) / 1000000ULL;
	LOGI(
		"startup-diag:libuvc first xfer status=%d %s after %llu ms (bulk xfer_timeout_ms=%u)",
		(int)xfer->status,
		stream_libusb_xfer_status_str((int)xfer->status),
		(unsigned long long)ms,
		(unsigned)LIBUVC_STREAM_XFER_TIMEOUT_MS);
}

void _uvc_diag_first_payload(uvc_stream_handle_t *strmh,
		size_t nbytes, const char *path) {
	if (!strmh->diag_xfer_epoch_ns || !nbytes || strmh->diag_logged_first_payload)
		return;
	strmh->diag_logged_first_payload = 1;
	const uint64_t ms =
		(uvc_diag_now_ns() - strmh->diag_xfer_epoch_ns) / 1000000ULL;
	LOGI(
		"startup-diag:libuvc first video payload (%s) nbytes=%zu after %llu ms",
		path,
		nbytes,
		(unsigned long long)ms);
}

void _uvc_diag_first_swap(uvc_stream_handle_t *strmh) {
	if (!strmh->diag_xfer_epoch_ns || strmh->diag_logged_first_swap)
		return;
	strmh->diag_logged_first_swap = 1;
	const uint64_t ms =
		(uvc_diag_now_ns() - strmh->diag_xfer_epoch_ns) / 1000000ULL;
	LOGI(
		"startup-diag:libuvc first frame ready (_uvc_swap_buffers) got_bytes=%zu after %llu ms",
		strmh->got_bytes,
		(unsigned long long)ms);
}

void _uvc_diag_bulk_timeout_before_payload(uvc_stream_handle_t *strmh,
		struct libusb_transfer *transfer) {
	if (!transfer->num_iso_packets && !strmh->diag_logged_first_payload) {
		strmh->diag_bulk_timeout_count_before_payload++;
		LOGI(
			"startup-diag:libuvc bulk xfer %s #%u after %llu ms since arm "
			"(device still not filling IN pipe; xfer_timeout_ms=%u)",
			stream_libusb_xfer_status_str((int)transfer->status),
			(unsigned)strmh->diag_bulk_timeout_count_before_payload,
			(unsigned long long)((uvc_diag_now_ns()
				- strmh->diag_xfer_epoch_ns)
				/ 1000000ULL),
			(unsigned)LIBUVC_STREAM_XFER_TIMEOUT_MS);
	}
}

uvc_error_t uvc_get_stream_runtime_diag(uvc_device_handle_t *devh,
		uint32_t *frame_interval_100ns,
		int *altsetting,
		uint8_t *is_isochronous,
		uint32_t *published_count,
		uint32_t *dropped_before_cb_count) {
	uvc_stream_handle_t *strmh;

	if (UNLIKELY(!devh))
		return UVC_ERROR_INVALID_PARAM;

	strmh = devh->streams;
	if (UNLIKELY(!strmh))
		return UVC_ERROR_NOT_FOUND;

	if (frame_interval_100ns)
		*frame_interval_100ns = strmh->diag_selected_frame_interval_100ns;
	if (altsetting)
		*altsetting = strmh->diag_selected_altsetting;
	if (is_isochronous)
		*is_isochronous = strmh->diag_selected_isochronous;
	if (published_count)
		*published_count = strmh->diag_mjpeg_publish_count;
	if (dropped_before_cb_count)
		*dropped_before_cb_count = strmh->diag_mjpeg_drop_count;
	return UVC_SUCCESS;
}
