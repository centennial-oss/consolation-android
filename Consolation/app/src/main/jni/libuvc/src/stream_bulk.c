/*********************************************************************
 * Bulk streaming transfer setup and payload processing (libuvc stream path).
 *********************************************************************/

#include <assert.h>
#include <time.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

#include <libusb/libusb_prealloc.h>
#include "libuvc/stream_log.h"
#include "libuvc/libuvc.h"
#include "libuvc/libuvc_internal.h"
#include "libuvc/stream_internal.h"

/** @internal
 * @brief Process a bulk payload transfer
 *
 * @param payload Contents of the payload transfer (full bulk transfer)
 * @param payload_len Length of the payload transfer
 */
void _uvc_process_payload_bulk(uvc_stream_handle_t *strmh, const uint8_t *payload, size_t const payload_len) {
	size_t header_len;
	uint8_t header_info;
	size_t data_len;

	// ignore empty payload transfers
	if (UNLIKELY(!payload || !payload_len))
		return;
	if (UNLIKELY(!strmh->outbuf)) {
		_uvc_diag_mjpeg_drop(strmh, "slot-exhausted");
		return;
	}

	header_len = payload[0];

	if (UNLIKELY(header_len > payload_len)) {
		strmh->bfh_err |= UVC_STREAM_ERR;
		UVC_DEBUG("bogus packet: actual_len=%zd, header_len=%zd\n", payload_len, header_len);
		return;
	}

	data_len = payload_len - header_len;

	if (UNLIKELY(header_len < 2)) {
		header_info = 0;
		if (_uvc_mjpeg_eoi_pending(strmh))
			return;	/* no status bits; never append past a held EOI */
	} else {
		//  @todo we should be checking the end-of-header bit
		size_t variable_offset = 2;

		header_info = payload[1];

		if (!strmh->diag_logged_first_payload) {
			LOGI("startup-diag:libuvc bulk first-hdr HLE=0x%02x BFH=0x%02x "
				"(ERR=%d EOF=%d PTS=%d SCR=%d FID=%d) data_len=%zu",
				(unsigned)header_len,
				(unsigned)header_info,
				!!(header_info & UVC_STREAM_ERR),
				!!(header_info & UVC_STREAM_EOF),
				!!(header_info & UVC_STREAM_PTS),
				!!(header_info & UVC_STREAM_SCR),
				!!(header_info & UVC_STREAM_FID),
				data_len);
		}

		/* Tail of a frame already published on its EOI marker: ignore. */
		if (_uvc_mjpeg_payload_after_eoi(strmh, header_info,
				payload + header_len, data_len))
			return;

		if ((strmh->fid != (header_info & UVC_STREAM_FID)) && strmh->got_bytes) {
			/* The frame ID bit was flipped, but we have image data sitting
				around from prior transfers. This means the camera didn't send
				an EOF for the last transfer of the previous frame. */
			_uvc_swap_buffers(strmh, "bulk-fid");
		}

		strmh->fid = header_info & UVC_STREAM_FID;

		if (UNLIKELY(header_info & UVC_STREAM_ERR)) {
			/* A BFH ERR bit in a completed bulk payload is a device stream
			 * condition, not a USB endpoint halt. Do not issue synchronous
			 * control transfers from this hot path; true endpoint halts are
			 * reported separately as LIBUSB_TRANSFER_STALL.
			 * Per UVC 1.5 2.4.3.3 the frame is damaged: mark it so it is
			 * dropped at publish instead of decoded with a hole. */
			if (strmh->got_bytes || data_len)
				strmh->bfh_err |= UVC_STREAM_ERR;
			strmh->diag_bfh_err_packets++;
			if (strmh->diag_bfh_err_packets == 1
					|| !(strmh->diag_bfh_err_packets % 1000)) {
				LOGI("libuvc bulk ERR bit set in payload header count=%u "
					"(before_first_payload=%d got_bytes=%zu data_len=%zu)",
					strmh->diag_bfh_err_packets,
					!strmh->first_video_payload_received,
					strmh->got_bytes, data_len);
			}
		}

		if (header_info & UVC_STREAM_PTS) {
			// XXX saki some camera may send broken packet or failed to receive all data
			if (LIKELY(variable_offset + 4 <= header_len)) {
				strmh->pts = DW_TO_INT(payload + variable_offset);
				variable_offset += 4;
			} else {
				MARK("bogus packet: header info has UVC_STREAM_PTS, but no data");
				strmh->pts = 0;
			}
		}

		if (header_info & UVC_STREAM_SCR) {
			// @todo read the SOF token counter
			// XXX saki some camera may send broken packet or failed to receive all data
			if (LIKELY(variable_offset + 4 <= header_len)) {
				strmh->last_scr = DW_TO_INT(payload + variable_offset);
				variable_offset += 4;
			} else {
				MARK("bogus packet: header info has UVC_STREAM_SCR, but no data");
				strmh->last_scr = 0;
			}
		}
	}

	if (LIKELY(data_len > 0)) {
		strmh->first_video_payload_received = 1;
		_uvc_diag_first_payload(strmh, data_len, "bulk");
		/* Last chunk may fill exactly to size_buf; strict `<` falsely set ERR and dropped it. */
		if (LIKELY(strmh->got_bytes + data_len <= strmh->size_buf)) {
			memcpy(strmh->outbuf + strmh->got_bytes, payload + header_len, data_len);
			strmh->got_bytes += data_len;
			if (_uvc_mjpeg_note_payload_append(strmh)) {
				_uvc_mjpeg_publish_on_eoi(strmh, header_info, "bulk-eoi");
				return;
			}
		} else {
			strmh->bfh_err |= UVC_STREAM_ERR;
		}

		if (header_info & UVC_STREAM_EOF/*(1 << 1)*/) {
			// The EOF bit is set, so publish the complete frame
			_uvc_swap_buffers(strmh, "bulk-eof");
		}
	}
}

/* Bounds for the bulk transfer buffer when the device leaves
 * dwMaxPayloadTransferSize at 0 in its probe/commit answer. */
#define LIBUVC_BULK_FALLBACK_MIN_XFER (16u * 1024u)
#define LIBUVC_BULK_FALLBACK_MAX_XFER (1024u * 1024u)
#define LIBUVC_BULK_PAYLOAD_HEADER_ROOM 256u	/* bHeaderLength is one byte */

/** @internal
 * @brief Size of each bulk transfer buffer.
 *
 * Normally the negotiated dwMaxPayloadTransferSize: one transfer then holds
 * exactly one payload (header + data).  Some devices answer the probe with 0
 * there, which would give zero-length transfers and a stream that never
 * delivers.  For those, size the buffer for a whole frame in one payload
 * (bounded), since nothing smaller is known to be enough.
 */
static size_t _uvc_bulk_transfer_size(uvc_stream_handle_t *strmh) {
	size_t size = strmh->cur_ctrl.dwMaxPayloadTransferSize;

	if (LIKELY(size))
		return size;

	size = (size_t)strmh->cur_ctrl.dwMaxVideoFrameSize + LIBUVC_BULK_PAYLOAD_HEADER_ROOM;
	if (size < LIBUVC_BULK_FALLBACK_MIN_XFER)
		size = LIBUVC_BULK_FALLBACK_MIN_XFER;
	if (size > LIBUVC_BULK_FALLBACK_MAX_XFER)
		size = LIBUVC_BULK_FALLBACK_MAX_XFER;
	/* Both bounds are multiples of every bulk wMaxPacketSize (512, 1024);
	 * keep the result one too, so a full-size packet can never overflow. */
	size = (size + 1023u) & ~(size_t)1023u;
#ifdef __ANDROID__
	__android_log_print(ANDROID_LOG_WARN, "libUVCCamera",
		"libuvc bulk: device reports dwMaxPayloadTransferSize=0, using %zu-byte transfers",
		size);
#endif
	return size;
}

uvc_error_t _uvc_stream_setup_bulk_transfers(uvc_stream_handle_t *strmh,
		uvc_format_desc_t *format_desc) {
	struct libusb_transfer *transfer;
	int transfer_id;
	const size_t transfer_size = _uvc_bulk_transfer_size(strmh);

	MARK("bulk transfer mode");
	strmh->diag_selected_altsetting = 0;
	strmh->num_transfer_bufs = LIBUVC_NUM_TRANSFER_BUFS;
	for (transfer_id = 0; transfer_id < (int)strmh->num_transfer_bufs; ++transfer_id) {
		transfer = libusb_alloc_transfer(0);
		strmh->transfers[transfer_id] = transfer;
		strmh->transfer_bufs[transfer_id] = malloc(transfer_size);
		if (UNLIKELY(!transfer || !strmh->transfer_bufs[transfer_id])) {
			_uvc_free_transfer(strmh, transfer_id);
			return UVC_ERROR_NO_MEM;
		}
		libusb_fill_bulk_transfer(transfer, strmh->devh->usb_devh,
			format_desc->parent->bEndpointAddress,
			strmh->transfer_bufs[transfer_id],
			(int)transfer_size, _uvc_stream_callback,
			(void *)strmh, LIBUVC_STREAM_XFER_TIMEOUT_MS);
		/* See stream_iso.c: build the URBs once, resubmit without allocating. */
		if (UNLIKELY(libusb_prealloc_bulk_urbs(transfer) != LIBUSB_SUCCESS))
			UVC_DEBUG("bulk transfer %d: URB prealloc failed, using slow path", transfer_id);
	}
	return UVC_SUCCESS;
}
