/*********************************************************************
 * MJPEG-specific diagnostics and JPEG bitstream checks (libuvc stream path).
 *********************************************************************/

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

int _uvc_mjpeg_payload_has_markers(const uvc_stream_handle_t *strmh) {
	const uint8_t *data = strmh->outbuf;
	const size_t len = strmh->got_bytes;
	size_t i;
	int found_sos = 0;

	if (strmh->frame_format != UVC_FRAME_FORMAT_MJPEG)
		return 1;

	if (!(len >= 4
		&& data[0] == 0xff && data[1] == 0xd8
		&& data[len - 2] == 0xff && data[len - 1] == 0xd9))
		return 0;

	for (i = 2; i + 1 < len - 2; i++) {
		if (data[i] == 0xff && data[i + 1] == 0xd8)
			return 0;
		if (data[i] == 0xff && data[i + 1] == 0xda)
			found_sos = 1;
	}

	return found_sos;
}

void _uvc_mjpeg_note_payload_append(uvc_stream_handle_t *strmh) {
	const uint8_t *data;
	const size_t len = strmh ? strmh->got_bytes : 0;

	if (!strmh || strmh->frame_format != UVC_FRAME_FORMAT_MJPEG || len < 2)
		return;

	data = strmh->outbuf;
	if (data && data[len - 2] == 0xff && data[len - 1] == 0xd9)
		strmh->frame_complete_monotonic_ns = uvc_diag_now_ns();
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
