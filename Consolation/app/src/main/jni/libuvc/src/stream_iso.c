/*********************************************************************
 * Isochronous streaming transfer setup and payload processing (libuvc stream path).
 *********************************************************************/

 #ifdef __ANDROID__
 #include <android/log.h>
 #endif
 
 #include <stdlib.h>
 #include <string.h>
 
 #include "libuvc/stream_log.h"
 #include "libuvc/libuvc.h"
 #include "libuvc/libuvc_internal.h"
 #include "libuvc/stream_internal.h"
 
#ifndef LIBUVC_NUM_ISO_PACKETS_PER_XFER
#define LIBUVC_NUM_ISO_PACKETS_PER_XFER 32
#endif
/*
 * This is the number of libusb transfers submitted for ISO streaming, not the
 * storage capacity for transfer pointers. uvc_stream_handle stores transfers in
 * arrays sized by LIBUVC_MAX_TRANSFER_BUFS in libuvc_internal.h. If this value
 * needs to exceed LIBUVC_MAX_TRANSFER_BUFS, resize those arrays at the same
 * time; otherwise ISO setup will write past the end of the stream handle.
 */
#ifndef LIBUVC_NUM_ISO_TRANSFER_BUFS
#define LIBUVC_NUM_ISO_TRANSFER_BUFS 32
#endif
#if LIBUVC_NUM_ISO_TRANSFER_BUFS > LIBUVC_MAX_TRANSFER_BUFS
#error "LIBUVC_NUM_ISO_TRANSFER_BUFS cannot exceed LIBUVC_MAX_TRANSFER_BUFS array capacity"
#endif
#ifndef LIBUVC_ISO_PREFER_MAX_PACKET_SIZE
#define LIBUVC_ISO_PREFER_MAX_PACKET_SIZE 1
#endif
 
 static unsigned int _uvc_iso_endpoint_bytes_per_interval(
		 const struct libusb_endpoint_descriptor *endpoint) {
	 const unsigned char *extra;
	 int extra_left;
	 unsigned int bytes;
	 unsigned int transactions;
 
	 extra = endpoint->extra;
	 extra_left = endpoint->extra_length;
	 while (extra && extra_left >= 2) {
		 uint8_t desc_len = extra[0];
		 uint8_t desc_type = extra[1];
 
		 if (desc_len < 2 || desc_len > extra_left)
			 break;
		 if (desc_type == LIBUSB_DT_SS_ENDPOINT_COMPANION
				 && desc_len >= LIBUSB_DT_SS_ENDPOINT_COMPANION_SIZE) {
			 unsigned int bytes_per_interval = extra[4] | ((unsigned int)extra[5] << 8);
			 if (bytes_per_interval)
				 return bytes_per_interval;
		 }
		 extra += desc_len;
		 extra_left -= desc_len;
	 }
 
	 bytes = endpoint->wMaxPacketSize & 0x07ff;
	 transactions = ((endpoint->wMaxPacketSize >> 11) & 0x03) + 1;
	 return bytes * transactions;
 }
 
 static int _uvc_iso_endpoint_matches(const struct libusb_endpoint_descriptor *endpoint,
		 uint8_t endpoint_address) {
	 if (UNLIKELY(!endpoint))
		 return 0;
	 if ((endpoint->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK)
			 != LIBUSB_TRANSFER_TYPE_ISOCHRONOUS)
		 return 0;
	 if ((endpoint->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) != LIBUSB_ENDPOINT_IN)
		 return 0;
	 if (endpoint_address
			 && (endpoint->bEndpointAddress & LIBUSB_ENDPOINT_ADDRESS_MASK)
				 != (endpoint_address & LIBUSB_ENDPOINT_ADDRESS_MASK))
		 return 0;
	 return 1;
 }
 
 static const struct libusb_endpoint_descriptor *_uvc_find_iso_endpoint(
		 const struct libusb_interface_descriptor *altsetting,
		 uint8_t endpoint_address) {
	 int endpoint_id;
 
	 for (endpoint_id = 0; endpoint_id < altsetting->bNumEndpoints; ++endpoint_id) {
		 const struct libusb_endpoint_descriptor *endpoint =
			 altsetting->endpoint + endpoint_id;
		 if (_uvc_iso_endpoint_matches(endpoint, endpoint_address))
			 return endpoint;
	 }
	 return NULL;
 }
 
static uint32_t _uvc_iso_required_payload_size(uvc_stream_handle_t *strmh,
		 float bandwidth_factor) {
	 uint32_t required = strmh->cur_ctrl.dwMaxPayloadTransferSize;
 
	 if (bandwidth_factor > 0.0f && bandwidth_factor < 1.0f) {
		 required = (uint32_t)(required * bandwidth_factor);
		 if (!required)
			 required = 1;
	 }
	 return required;
 }

void _uvc_diag_iso_frame_reset(uvc_stream_handle_t *strmh) {
	if (!strmh)
		return;
	strmh->diag_iso_payload_bytes = 0;
	strmh->diag_iso_payload_hash = 2166136261u;
	strmh->diag_iso_payload_packets = 0;
	strmh->diag_iso_packet_errors = 0;
	strmh->diag_iso_zero_packets = 0;
	strmh->diag_iso_overflow_count = 0;
	strmh->diag_iso_eof_empty_count = 0;
	strmh->diag_iso_short_packets = 0;
	strmh->diag_iso_min_packet_len = 0;
	strmh->diag_iso_max_packet_len = 0;
	strmh->diag_iso_packet_len_hash = 2166136261u;
}

static void _uvc_diag_iso_payload_bytes(uvc_stream_handle_t *strmh,
		const uint8_t *data, size_t len) {
	size_t i;

	if (strmh->frame_format != UVC_FRAME_FORMAT_MJPEG || !data || !len)
		return;
	if (!strmh->diag_iso_payload_hash)
		strmh->diag_iso_payload_hash = 2166136261u;
	for (i = 0; i < len; i++) {
		strmh->diag_iso_payload_hash ^= data[i];
		strmh->diag_iso_payload_hash *= 16777619u;
	}
	strmh->diag_iso_payload_bytes += len;
	strmh->diag_iso_payload_packets++;
}

static void _uvc_diag_iso_packet_shape(uvc_stream_handle_t *strmh,
		int actual_length, int expected_length) {
	uint32_t len;

	if (strmh->frame_format != UVC_FRAME_FORMAT_MJPEG || actual_length <= 0)
		return;

	len = (uint32_t)actual_length;
	if (!strmh->diag_iso_min_packet_len || len < strmh->diag_iso_min_packet_len)
		strmh->diag_iso_min_packet_len = len;
	if (len > strmh->diag_iso_max_packet_len)
		strmh->diag_iso_max_packet_len = len;
	if (expected_length > 0 && actual_length < expected_length)
		strmh->diag_iso_short_packets++;
	if (!strmh->diag_iso_packet_len_hash)
		strmh->diag_iso_packet_len_hash = 2166136261u;
	strmh->diag_iso_packet_len_hash ^= len;
	strmh->diag_iso_packet_len_hash *= 16777619u;
	strmh->diag_iso_packet_len_hash ^= (uint32_t)(len >> 16);
	strmh->diag_iso_packet_len_hash *= 16777619u;
}
 
 static void _uvc_process_payload_iso_packet(uvc_stream_handle_t *strmh,
		 const uint8_t *payload, size_t payload_len) {
	 size_t header_len;
	 uint8_t header_info = 0;
	 size_t data_len;
 
	 if (UNLIKELY(!payload || !payload_len))
		 return;
	 if (UNLIKELY(!strmh->outbuf)) {
		 _uvc_diag_mjpeg_drop(strmh, "slot-exhausted");
		 return;
	 }
 
	 header_len = payload[0];
	 if (UNLIKELY(header_len > payload_len)) {
		 strmh->bfh_err |= UVC_STREAM_ERR;
		 UVC_DEBUG("bogus iso packet: actual_len=%zd, header_len=%zd\n",
			 payload_len, header_len);
		 return;
	 }
 
	 data_len = payload_len - header_len;
 
	 if (UNLIKELY(header_len < 2)) {
		 header_info = 0;
	 } else {
		 size_t variable_offset = 2;
 
		 header_info = payload[1];
 
		 if (!strmh->diag_logged_first_payload) {
			 LOGI("startup-diag:libuvc iso first-hdr HLE=0x%02x BFH=0x%02x "
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
 
		 if (UNLIKELY(header_info & UVC_STREAM_ERR)) {
			 LOGI("startup-diag:libuvc iso ERR bit set in header "
				 "(before_first_payload=%d)",
				 !strmh->first_video_payload_received);
		 }
 
		 if ((strmh->fid != (header_info & UVC_STREAM_FID)) && strmh->got_bytes) {
			 _uvc_swap_buffers(strmh, "iso-fid");
		 }
 
		 strmh->fid = header_info & UVC_STREAM_FID;
 
		 if (header_info & UVC_STREAM_PTS) {
			 if (LIKELY(variable_offset + 4 <= header_len)) {
				 strmh->pts = DW_TO_INT(payload + variable_offset);
				 variable_offset += 4;
			 } else {
				 MARK("bogus packet: header info has UVC_STREAM_PTS, but no data");
				 strmh->pts = 0;
			 }
		 }
 
		 if (header_info & UVC_STREAM_SCR) {
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
		 _uvc_diag_first_payload(strmh, data_len, "iso");
		 if (LIKELY(strmh->got_bytes + data_len <= strmh->size_buf)) {
			 _uvc_diag_iso_payload_bytes(strmh, payload + header_len, data_len);
			 memcpy(strmh->outbuf + strmh->got_bytes, payload + header_len, data_len);
			 strmh->got_bytes += data_len;
			 _uvc_mjpeg_note_payload_append(strmh);
		 } else {
			 strmh->diag_iso_overflow_count++;
			 strmh->bfh_err |= UVC_STREAM_ERR;
		 }
	 }

	 if (UNLIKELY((header_info & UVC_STREAM_EOF) && strmh->got_bytes)) {
		 if (!data_len)
			 strmh->diag_iso_eof_empty_count++;
		 _uvc_swap_buffers(strmh, data_len ? "iso-eof" : "iso-eof-empty");
	 }
 }
 
 void _uvc_process_payload_iso(uvc_stream_handle_t *strmh, struct libusb_transfer *transfer) {
	 int packet_id;
 
	 if (UNLIKELY(!transfer))
		 return;
 
	 for (packet_id = 0; packet_id < transfer->num_iso_packets; ++packet_id) {
		 struct libusb_iso_packet_descriptor *packet =
			 transfer->iso_packet_desc + packet_id;
		 uint8_t *payload;

		 if (UNLIKELY(packet->status != LIBUSB_TRANSFER_COMPLETED)) {
			 strmh->diag_iso_packet_errors++;
			 strmh->bfh_err |= UVC_STREAM_ERR;
			 continue;
		 }
		 if (UNLIKELY(packet->actual_length <= 0)) {
			 strmh->diag_iso_zero_packets++;
			 if (strmh->got_bytes)
				 strmh->bfh_err |= UVC_STREAM_ERR;
			 continue;
		 }

		 _uvc_diag_iso_packet_shape(strmh, packet->actual_length, packet->length);
		 payload = libusb_get_iso_packet_buffer_simple(transfer, packet_id);
		 _uvc_process_payload_iso_packet(strmh, payload, packet->actual_length);
	 }
 }
 
 uvc_error_t _uvc_stream_setup_iso_transfers(uvc_stream_handle_t *strmh,
		 const struct libusb_interface *interface,
		 uvc_format_desc_t *format_desc,
		 uint32_t dwMaxVideoFrameSize,
		 float bandwidth_factor) {
	 const struct libusb_interface_descriptor *selected_altsetting = NULL;
	 const struct libusb_endpoint_descriptor *selected_endpoint = NULL;
	 unsigned int selected_packet_size = 0;
	 uint32_t required_payload_size;
	 int selected_satisfies_required = 0;
	 int altsetting_id;
	 int usb_ret;
	 int transfer_id;
 
	 (void)dwMaxVideoFrameSize;
 
	 if (UNLIKELY(!strmh || !interface || !format_desc))
		 return UVC_ERROR_INVALID_PARAM;
 
	 required_payload_size = _uvc_iso_required_payload_size(strmh, bandwidth_factor);
 
	 for (altsetting_id = 0; altsetting_id < interface->num_altsetting; ++altsetting_id) {
		 const struct libusb_interface_descriptor *altsetting =
			 interface->altsetting + altsetting_id;
		 const struct libusb_endpoint_descriptor *endpoint =
			 _uvc_find_iso_endpoint(altsetting, format_desc->parent->bEndpointAddress);
		 unsigned int packet_size;
 
		 if (!endpoint)
			 continue;
 
		 packet_size = _uvc_iso_endpoint_bytes_per_interval(endpoint);
		 if (!packet_size)
			 continue;

		 UVC_DIAG_LOGI("mjpeg-diag:iso-alt alt=%u ep=0x%02x packet_size=%u "
			 "wMaxPacketSize=0x%04x interval=%u required=%u",
			 (unsigned)altsetting->bAlternateSetting,
			 (unsigned)endpoint->bEndpointAddress,
			 packet_size,
			 (unsigned)endpoint->wMaxPacketSize,
			 (unsigned)endpoint->bInterval,
			 (unsigned)required_payload_size);
 
#if LIBUVC_ISO_PREFER_MAX_PACKET_SIZE
		 if (!selected_altsetting || packet_size > selected_packet_size) {
			 selected_altsetting = altsetting;
			 selected_endpoint = endpoint;
			 selected_packet_size = packet_size;
			 selected_satisfies_required = packet_size >= required_payload_size;
		 }
#else
		 if (!selected_altsetting
				 || (!selected_satisfies_required && packet_size > selected_packet_size)
				 || (selected_satisfies_required && packet_size >= required_payload_size
					 && packet_size < selected_packet_size)) {
			 selected_altsetting = altsetting;
			 selected_endpoint = endpoint;
			 selected_packet_size = packet_size;
			 selected_satisfies_required = packet_size >= required_payload_size;
		 }
#endif
 
	 }
 
	 if (UNLIKELY(!selected_altsetting || !selected_endpoint)) {
		 UVC_DEBUG("no usable isochronous IN endpoint found");
		 return UVC_ERROR_NOT_SUPPORTED;
	 }
 
	 if (UNLIKELY(selected_packet_size < required_payload_size)) {
		 UVC_DEBUG("using largest ISO endpoint packet size %u below requested payload size %u",
			 selected_packet_size, (unsigned)required_payload_size);
	 }
 
	 usb_ret = libusb_set_interface_alt_setting(strmh->devh->usb_devh,
		 strmh->stream_if->bInterfaceNumber, selected_altsetting->bAlternateSetting);
	 if (UNLIKELY(usb_ret != LIBUSB_SUCCESS)) {
		 UVC_DEBUG("libusb_set_interface_alt_setting(%u) failed: %d",
			 (unsigned)selected_altsetting->bAlternateSetting, usb_ret);
		 return UVC_ERROR_IO;
	 }
 
	 strmh->diag_selected_altsetting = selected_altsetting->bAlternateSetting;
	 strmh->num_transfer_bufs = LIBUVC_NUM_ISO_TRANSFER_BUFS;
	 UVC_DIAG_LOGI("mjpeg-diag:iso-selected alt=%u ep=0x%02x packet_size=%u "
		 "required=%u prefer_max=%u",
		 (unsigned)selected_altsetting->bAlternateSetting,
		 (unsigned)selected_endpoint->bEndpointAddress,
		 selected_packet_size,
		 (unsigned)required_payload_size,
		 (unsigned)LIBUVC_ISO_PREFER_MAX_PACKET_SIZE);
 
	 for (transfer_id = 0; transfer_id < (int)strmh->num_transfer_bufs; ++transfer_id) {
		 struct libusb_transfer *transfer =
			 libusb_alloc_transfer(LIBUVC_NUM_ISO_PACKETS_PER_XFER);
		 size_t transfer_buf_size =
			 (size_t)selected_packet_size * LIBUVC_NUM_ISO_PACKETS_PER_XFER;
 
		 strmh->transfers[transfer_id] = transfer;
		 strmh->transfer_bufs[transfer_id] = malloc(transfer_buf_size);
		 if (UNLIKELY(!transfer || !strmh->transfer_bufs[transfer_id])) {
			 _uvc_free_transfer(strmh, transfer_id);
			 return UVC_ERROR_NO_MEM;
		 }
 
		 libusb_fill_iso_transfer(transfer, strmh->devh->usb_devh,
			 selected_endpoint->bEndpointAddress,
			 strmh->transfer_bufs[transfer_id],
			 (int)transfer_buf_size,
			 LIBUVC_NUM_ISO_PACKETS_PER_XFER,
			 _uvc_stream_callback,
			 (void *)strmh,
			 LIBUVC_STREAM_XFER_TIMEOUT_MS);
		 libusb_set_iso_packet_lengths(transfer, selected_packet_size);
 
	 }

	 return UVC_SUCCESS;
 }
