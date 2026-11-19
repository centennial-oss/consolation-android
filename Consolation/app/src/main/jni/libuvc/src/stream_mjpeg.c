/*********************************************************************
 * MJPEG-specific diagnostics and JPEG bitstream checks (libuvc stream path).
 *********************************************************************/

#include <string.h>

#include "libuvc/stream_internal.h"

static uint32_t _uvc_diag_sample_hash(const uint8_t *data, size_t len) {
	uint32_t hash = 2166136261u;
	size_t chunk;

	if (!data || !len)
		return 0;

	for (chunk = 0; chunk < 16; chunk++) {
		size_t offset = (len * chunk) / 16;
		size_t end = offset + 64;
		size_t i;
		if (end > len)
			end = len;
		for (i = offset; i < end; i++) {
			hash ^= data[i];
			hash *= 16777619u;
		}
	}
	return hash;
}

static uint32_t _uvc_diag_hash_bytes(uint32_t hash, const uint8_t *data, size_t len) {
	size_t i;
	for (i = 0; i < len; i++) {
		hash ^= data[i];
		hash *= 16777619u;
	}
	return hash;
}

static uint32_t _uvc_diag_full_hash(const uint8_t *data, size_t len) {
	if (!data || !len)
		return 0;
	return _uvc_diag_hash_bytes(2166136261u, data, len);
}

static uint16_t _uvc_diag_read_be16(const uint8_t *data) {
	return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint32_t _uvc_diag_mjpeg_header_hash(const uint8_t *data, size_t len,
		uint16_t *restart_interval) {
	uint32_t hash = 2166136261u;
	size_t pos = 2;

	if (restart_interval)
		*restart_interval = 0;

	if (!data || len < 4 || data[0] != 0xff || data[1] != 0xd8)
		return 0;

	for (; pos + 3 < len ;) {
		uint8_t marker;
		uint16_t segment_len;
		size_t segment_end;

		if (data[pos] != 0xff) {
			pos++;
			continue;
		}
		while (pos < len && data[pos] == 0xff)
			pos++;
		if (pos >= len)
			break;

		marker = data[pos++];
		if (marker == 0xda || marker == 0xd9)
			break;
		if (marker == 0x00 || (marker >= 0xd0 && marker <= 0xd7) || marker == 0x01)
			continue;
		if (pos + 2 > len)
			break;

		segment_len = _uvc_diag_read_be16(data + pos);
		if (segment_len < 2)
			break;
		segment_end = pos + segment_len;
		if (segment_end > len)
			break;

		hash ^= marker;
		hash *= 16777619u;
		hash = _uvc_diag_hash_bytes(hash, data + pos, segment_len);

		if (marker == 0xdd && restart_interval && segment_len >= 4)
			*restart_interval = _uvc_diag_read_be16(data + pos + 2);

		pos = segment_end;
	}

	return hash;
}

void _uvc_mjpeg_scan_reset(uvc_stream_handle_t *strmh) {
	if (!strmh)
		return;
	/* Note: mjpeg_eoi_skip_* deliberately survive this reset; they are
	 * cleared by the next FID flip (see _uvc_mjpeg_payload_after_eoi). */
	strmh->mjpeg_eoi_pending = 0;
	strmh->mjpeg_scan_pos = 0;
	strmh->mjpeg_scan_found_sos = 0;
	strmh->mjpeg_scan_embedded_soi = 0;
}

/* Advance the incremental marker scan over bytes appended to outbuf since the
 * last call.  Examines every (i, i+1) pair with i >= 2, carrying one byte across
 * append boundaries, so the result at publish time equals a full scan of
 * outbuf[2 .. got_bytes).  Uses memchr to skip to 0xff candidates: entropy-coded
 * JPEG data contains few 0xff bytes, so this runs near memcpy speed and the data
 * is still cache-hot from the payload memcpy that preceded it. */
static void _uvc_mjpeg_scan_advance(uvc_stream_handle_t *strmh) {
	const uint8_t *data = strmh->outbuf;
	const size_t len = strmh->got_bytes;
	size_t i;

	if (!data || len < 3)
		return;
	if (strmh->mjpeg_scan_embedded_soi)
		return;	/* already rejected; nothing further changes the verdict */

	i = strmh->mjpeg_scan_pos < 2 ? 2 : strmh->mjpeg_scan_pos;
	while (i + 1 < len) {
		const uint8_t *ff = memchr(data + i, 0xff, len - 1 - i);
		uint8_t next;
		if (!ff)
			break;
		i = (size_t)(ff - data);
		next = data[i + 1];
		if (next == 0xd8) {
			strmh->mjpeg_scan_embedded_soi = 1;
			break;
		}
		if (next == 0xda)
			strmh->mjpeg_scan_found_sos = 1;
		i++;
	}
	/* Next call resumes at the pair that starts on the current last byte, so a
	 * marker split across two appends is still seen. */
	strmh->mjpeg_scan_pos = len - 1;
}

int _uvc_mjpeg_payload_has_markers(uvc_stream_handle_t *strmh) {
	const uint8_t *data = strmh->outbuf;
	const size_t len = strmh->got_bytes;

	if (strmh->frame_format != UVC_FRAME_FORMAT_MJPEG)
		return 1;

	if (!(len >= 4
		&& data[0] == 0xff && data[1] == 0xd8
		&& data[len - 2] == 0xff && data[len - 1] == 0xd9))
		return 0;

	/* Catch up in case bytes were appended without _uvc_mjpeg_note_payload_append. */
	_uvc_mjpeg_scan_advance(strmh);
	return !strmh->mjpeg_scan_embedded_soi && strmh->mjpeg_scan_found_sos;
}

int _uvc_mjpeg_note_payload_append(uvc_stream_handle_t *strmh) {
	const uint8_t *data;
	const size_t len = strmh ? strmh->got_bytes : 0;

	if (!strmh || strmh->frame_format != UVC_FRAME_FORMAT_MJPEG || len < 2)
		return 0;

	_uvc_mjpeg_scan_advance(strmh);

	data = strmh->outbuf;
	if (data && data[len - 2] == 0xff && data[len - 1] == 0xd9) {
		strmh->frame_complete_monotonic_ns = uvc_diag_now_ns();
		_uvc_asm_report(strmh, 1);
		return 1;
	}
	_uvc_asm_report(strmh, 0);
	return 0;
}

/*
 * Assembly progress.  A listener (a decoder working ahead of the frame's
 * arrival) is told when enough new bytes are in place to be worth a wake-up,
 * and always when the end-of-image marker lands.  The step tracks the size of
 * recent frames so a frame costs the listener a handful of wake-ups whatever
 * the bitrate.
 */
#define UVC_ASM_REPORT_MIN_STEP (32u * 1024u)
#define UVC_ASM_REPORTS_PER_FRAME 8u

void _uvc_asm_report(uvc_stream_handle_t *strmh, int complete) {
	uvc_assembly_event_t ev;

	if (!strmh->assembly_cb || !strmh->outbuf || !strmh->got_bytes)
		return;

	if (!strmh->asm_open) {
		strmh->asm_open = 1;
		strmh->asm_reported_bytes = 0;
		if (!++strmh->asm_gen)
			strmh->asm_gen = 1;	/* 0 means "no generation" */
	}

	if (!complete
			&& strmh->got_bytes - strmh->asm_reported_bytes < strmh->asm_report_step)
		return;

	strmh->asm_reported_bytes = strmh->got_bytes;
	if (complete) {
		size_t step = strmh->got_bytes / UVC_ASM_REPORTS_PER_FRAME;
		strmh->asm_report_step = step > UVC_ASM_REPORT_MIN_STEP
			? step : UVC_ASM_REPORT_MIN_STEP;
	}

	ev.type = UVC_ASSEMBLY_PROGRESS;
	ev.generation = strmh->asm_gen;
	ev.data = strmh->outbuf;
	ev.bytes = strmh->got_bytes;
	ev.complete = complete ? 1 : 0;
	strmh->assembly_cb(&ev, strmh->usb_cb_user_ptr);
}

void _uvc_asm_discard(uvc_stream_handle_t *strmh) {
	uvc_assembly_event_t ev;

	if (!strmh->asm_open)
		return;
	strmh->asm_open = 0;
	if (!strmh->assembly_cb)
		return;

	ev.type = UVC_ASSEMBLY_DISCARDED;
	ev.generation = strmh->asm_gen;
	ev.data = NULL;
	ev.bytes = 0;
	ev.complete = 0;
	strmh->assembly_cb(&ev, strmh->usb_cb_user_ptr);
}

/*
 * Publish on EOI.  A JPEG is complete at its EOI marker, so waiting for the
 * UVC EOF bit (often carried by a later header-only packet, i.e. after the
 * next transfer completes) or for the FID flip (a whole frame interval later
 * on cameras that never set EOF) only adds latency.  FFD9 cannot occur inside
 * entropy-coded data (0xFF is always stuffed), so the tail check is exact.
 *
 * Complete is not the same as valid, though: a header-only trailer for this
 * frame can still carry UVC_STREAM_ERR (the frame has a transmission hole but
 * intact SOI/EOI).  So the frame is only published straight away when the
 * payload that carried the EOI also carried EOF.  Otherwise it is held until
 * a definitive boundary: a trailer with EOF, an FID flip, or the next frame's
 * SOI.  Idle ISO slots and past behaviour of the device prove nothing about
 * whether a trailer is still coming, so neither releases a held frame.
 */
static void _uvc_mjpeg_eoi_publish_now(uvc_stream_handle_t *strmh, const char *reason) {
	strmh->mjpeg_eoi_pending = 0;
	strmh->mjpeg_eoi_skip_valid = 1;
	strmh->mjpeg_eoi_skip_fid = strmh->fid;
	_uvc_swap_buffers(strmh, reason);
}

void _uvc_mjpeg_publish_on_eoi(uvc_stream_handle_t *strmh, uint8_t header_info,
		const char *reason) {
	if (!strmh || strmh->frame_format != UVC_FRAME_FORMAT_MJPEG)
		return;
	if (header_info & UVC_STREAM_EOF) {
		_uvc_mjpeg_eoi_publish_now(strmh, reason);
		return;
	}
	strmh->mjpeg_eoi_pending = 1;
	strmh->mjpeg_eoi_pending_reason = reason;
}

int _uvc_mjpeg_eoi_pending(const uvc_stream_handle_t *strmh) {
	return strmh && strmh->mjpeg_eoi_pending;
}

int _uvc_mjpeg_payload_after_eoi(uvc_stream_handle_t *strmh, uint8_t header_info,
		const uint8_t *data, size_t data_len) {
	const int new_frame =
		((header_info & UVC_STREAM_FID) != (strmh->mjpeg_eoi_pending
			? strmh->fid : strmh->mjpeg_eoi_skip_fid))
		/* New SOI under the same FID (camera does not toggle FID). */
		|| (data_len >= 2 && data[0] == 0xff && data[1] == 0xd8);

	if (strmh->mjpeg_eoi_pending) {
		if (new_frame) {
			/* No trailer was sent: the held frame stands as assembled. */
			_uvc_mjpeg_eoi_publish_now(strmh, strmh->mjpeg_eoi_pending_reason);
			strmh->mjpeg_eoi_skip_valid = 0;
			return 0;	/* this payload starts the next frame: process it */
		}
		/* Trailer (or padding) of the held frame: its ERR bit still counts. */
		if (header_info & UVC_STREAM_ERR) {
			strmh->bfh_err |= UVC_STREAM_ERR;	/* dropped at publish */
			strmh->diag_bfh_err_packets++;
		}
		if (header_info & UVC_STREAM_EOF) {
			_uvc_mjpeg_eoi_publish_now(strmh, strmh->mjpeg_eoi_pending_reason);
			strmh->mjpeg_eoi_skip_valid = 0;	/* explicit end of the frame */
		}
		return 1;
	}

	if (!strmh->mjpeg_eoi_skip_valid)
		return 0;
	if (new_frame) {
		strmh->mjpeg_eoi_skip_valid = 0;	/* next frame begins */
		return 0;
	}
	if (header_info & UVC_STREAM_EOF)
		strmh->mjpeg_eoi_skip_valid = 0;	/* explicit end of the published frame */
	return 1;
}

void _uvc_diag_mjpeg_drop(uvc_stream_handle_t *strmh, const char *reason) {
	if (strmh->frame_format != UVC_FRAME_FORMAT_MJPEG)
		return;

	strmh->diag_mjpeg_drop_count++;
	if (strmh->diag_mjpeg_drop_count <= 20 || !(strmh->diag_mjpeg_drop_count % 120)) {
		UVC_DIAG_LOGI("mjpeg-diag:drop count=%u reason=%s seq=%u bytes=%zu fid=%u pts=%u scr=%u",
			strmh->diag_mjpeg_drop_count,
			reason ? reason : "?",
			strmh->seq,
			strmh->got_bytes,
			(unsigned)strmh->fid,
			strmh->pts,
			strmh->last_scr);
	}
}

void _uvc_diag_mjpeg_publish(uvc_stream_handle_t *strmh, const char *reason) {
	size_t last_bytes;
	int size_anomaly = 0;
	int pts_anomaly = 0;
	int scr_anomaly = 0;
	uint32_t hash;
	uint32_t full_hash = 0;
	uint32_t header_hash;
	uint16_t restart_interval = 0;
	int sample_hash_repeat = 0;
	int header_change = 0;
	int restart_change = 0;
	int iso_audit_mismatch = 0;
	int iso_audit_anomaly = 0;

	if (strmh->frame_format != UVC_FRAME_FORMAT_MJPEG)
		return;
	strmh->diag_mjpeg_publish_count++;
#if !UVC_RUNTIME_DIAG_ENABLED
	(void)reason;
	return;
#endif

	hash = _uvc_diag_sample_hash(strmh->outbuf, strmh->got_bytes);
	full_hash = _uvc_diag_full_hash(strmh->outbuf, strmh->got_bytes);
	header_hash = _uvc_diag_mjpeg_header_hash(strmh->outbuf, strmh->got_bytes,
		&restart_interval);
	if (strmh->diag_selected_isochronous) {
		iso_audit_mismatch = strmh->diag_iso_payload_bytes
			&& (strmh->diag_iso_payload_bytes != strmh->got_bytes
				|| strmh->diag_iso_payload_hash != full_hash);
		iso_audit_anomaly = iso_audit_mismatch
			|| strmh->diag_iso_packet_errors
			|| strmh->diag_iso_zero_packets
			|| strmh->diag_iso_overflow_count;
	}
	sample_hash_repeat = strmh->diag_last_mjpeg_sample_hash
		&& hash == strmh->diag_last_mjpeg_sample_hash;
	header_change = strmh->diag_last_mjpeg_header_hash
		&& header_hash != strmh->diag_last_mjpeg_header_hash;
	restart_change = strmh->diag_last_mjpeg_restart_interval
		&& restart_interval != strmh->diag_last_mjpeg_restart_interval;

	last_bytes = strmh->diag_last_mjpeg_bytes;
	if (last_bytes) {
		const size_t min_expected = last_bytes - (last_bytes / 3);
		const size_t max_expected = last_bytes + (last_bytes / 3);
		size_anomaly = strmh->got_bytes < min_expected || strmh->got_bytes > max_expected;
	}
	pts_anomaly = strmh->pts && strmh->diag_last_mjpeg_pts
		&& strmh->pts <= strmh->diag_last_mjpeg_pts;
	scr_anomaly = strmh->last_scr && strmh->diag_last_mjpeg_scr
		&& strmh->last_scr <= strmh->diag_last_mjpeg_scr;

#if UVC_RUNTIME_DIAG_ENABLED
	if (strmh->diag_selected_isochronous
			|| size_anomaly || pts_anomaly || scr_anomaly || header_change || restart_change
			|| iso_audit_anomaly
			|| strmh->diag_mjpeg_publish_count <= 20
			|| !(strmh->diag_mjpeg_publish_count % 120)) {
		UVC_DIAG_LOGI("mjpeg-diag:publish count=%u reason=%s seq=%u bytes=%zu last_bytes=%zu "
			"fid=%u pts=%u last_pts=%u scr=%u last_scr=%u hash=%08x repeat=%u "
			"full=%08x hdr=%08x last_hdr=%08x dri=%u last_dri=%u "
			"iso_bytes=%zu iso_hash=%08x iso_pkts=%u iso_err=%u iso_zero=%u "
			"iso_ovf=%u iso_eof_empty=%u iso_short=%u iso_min=%u iso_max=%u "
			"iso_len_hash=%08x anomaly=%s%s%s%s%s%s",
			strmh->diag_mjpeg_publish_count,
			reason ? reason : "?",
			strmh->seq,
			strmh->got_bytes,
			last_bytes,
			(unsigned)strmh->fid,
			strmh->pts,
			strmh->diag_last_mjpeg_pts,
			strmh->last_scr,
			strmh->diag_last_mjpeg_scr,
			hash,
			(unsigned)sample_hash_repeat,
			full_hash,
			header_hash,
			strmh->diag_last_mjpeg_header_hash,
			(unsigned)restart_interval,
			(unsigned)strmh->diag_last_mjpeg_restart_interval,
			strmh->diag_iso_payload_bytes,
			strmh->diag_iso_payload_hash,
			strmh->diag_iso_payload_packets,
			strmh->diag_iso_packet_errors,
			strmh->diag_iso_zero_packets,
			strmh->diag_iso_overflow_count,
			strmh->diag_iso_eof_empty_count,
			strmh->diag_iso_short_packets,
			strmh->diag_iso_min_packet_len,
			strmh->diag_iso_max_packet_len,
			strmh->diag_iso_packet_len_hash,
			size_anomaly ? "size" : "",
			pts_anomaly ? "|pts" : "",
			scr_anomaly ? "|scr" : "",
			header_change ? "|hdr" : "",
			restart_change ? "|dri" : "",
			iso_audit_mismatch ? "|iso-audit" : "");
	}
#endif

	strmh->diag_last_mjpeg_bytes = strmh->got_bytes;
	strmh->diag_last_mjpeg_pts = strmh->pts;
	strmh->diag_last_mjpeg_scr = strmh->last_scr;
	strmh->diag_last_mjpeg_sample_hash = hash;
	strmh->diag_last_mjpeg_header_hash = header_hash;
	strmh->diag_last_mjpeg_restart_interval = restart_interval;
}

void _uvc_diag_mjpeg_log_stream_start(const uvc_stream_ctrl_t *ctrl,
		const uvc_frame_desc_t *frame_desc) {
	UVC_DIAG_LOGI("mjpeg-diag:enabled stream fmt=%u frm=%u %ux%u interval=%u diag=%d",
		(unsigned)ctrl->bFormatIndex,
		(unsigned)ctrl->bFrameIndex,
		(unsigned)frame_desc->wWidth,
		(unsigned)frame_desc->wHeight,
		(unsigned)ctrl->dwFrameInterval,
		UVC_RUNTIME_DIAG_ENABLED);
}
