/*********************************************************************
 * Isochronous streaming transfer setup and payload processing (libuvc stream path).
 *********************************************************************/

#include <assert.h>
#include <time.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

#include "libuvc/stream_log.h"
#include "libuvc/libuvc.h"
#include "libuvc/libuvc_internal.h"
#include "libuvc/stream_internal.h"

#define USE_EOF

void _uvc_process_payload_iso(uvc_stream_handle_t *strmh, struct libusb_transfer *transfer) {
	/* per packet */
	uint8_t *pktbuf;
	uint8_t check_header;
	size_t header_len;
	uint8_t header_info;
	struct libusb_iso_packet_descriptor *pkt;

	/* magic numbers for identifying header packets from some iSight cameras */
	static const uint8_t isight_tag[] = {
		0x11, 0x22, 0x33, 0x44, 0xde, 0xad,
		0xbe, 0xef, 0xde, 0xad, 0xfa, 0xce };
	int packet_id;
	uvc_vs_error_code_control_t vs_error_code;

	for (packet_id = 0; packet_id < transfer->num_iso_packets; ++packet_id) {
		check_header = 1;
		if (UNLIKELY(!strmh->outbuf)) {
			_uvc_diag_mjpeg_drop(strmh, "slot-exhausted");
			continue;
		}

		pkt = transfer->iso_packet_desc + packet_id;

		if (UNLIKELY(pkt->status != 0)) {
			MARK("bad packet:status=%d,actual_length=%d", pkt->status, pkt->actual_length);
			strmh->bfh_err |= UVC_STREAM_ERR;
			libusb_clear_halt(strmh->devh->usb_devh, strmh->stream_if->bEndpointAddress);
			continue;
		}

		if (UNLIKELY(!pkt->actual_length)) {
			continue;
		}
		pktbuf = libusb_get_iso_packet_buffer_simple(transfer, packet_id);
		if (LIKELY(pktbuf)) {
#ifdef __ANDROID__
			if (UNLIKELY(strmh->devh->is_isight))
#else
			if (strmh->devh->is_isight)
#endif
			{
				if (pkt->actual_length < 30
					|| (memcmp(isight_tag, pktbuf + 2, sizeof(isight_tag))
						&& memcmp(isight_tag, pktbuf + 3, sizeof(isight_tag)))) {
					check_header = 0;
					header_len = 0;
				} else {
					header_len = pktbuf[0];
				}
			} else {
				header_len = pktbuf[0];	// Header length field of Stream Header
			}

			if (LIKELY(check_header)) {
				header_info = pktbuf[1];
				if (UNLIKELY(header_info & UVC_STREAM_ERR)) {
					MARK("bad packet:status=0x%2x", header_info);
					libusb_clear_halt(strmh->devh->usb_devh, strmh->stream_if->bEndpointAddress);
					uvc_vs_get_error_code(strmh->devh, &vs_error_code, UVC_GET_CUR);
					continue;
				}
#ifdef USE_EOF
				if ((strmh->fid != (header_info & UVC_STREAM_FID)) && strmh->got_bytes) {
				/* The frame ID bit was flipped, but we have image data sitting
	             around from prior transfers. This means the camera didn't send
    		     an EOF for the last transfer of the previous frame or some frames losted. */
					_uvc_swap_buffers(strmh, "iso-fid");
				}
				strmh->fid = header_info & UVC_STREAM_FID;
#else
				if (strmh->fid != (header_info & UVC_STREAM_FID)) {
					_uvc_swap_buffers(strmh, "iso-fid-noeof");
					strmh->fid = header_info & UVC_STREAM_FID;
				}
#endif
				if (header_info & UVC_STREAM_PTS) {
					if (LIKELY(header_len >= 6)) {
						strmh->pts = DW_TO_INT(pktbuf + 2);
					} else {
						MARK("bogus packet: header info has UVC_STREAM_PTS, but no data");
						strmh->pts = 0;
					}
				}

				if (header_info & UVC_STREAM_SCR) {
					if (LIKELY(header_len >= 10)) {
						strmh->last_scr = DW_TO_INT(pktbuf + 6);
					} else {
						MARK("bogus packet: header info has UVC_STREAM_SCR, but no data");
						strmh->last_scr = 0;
					}
				}

#ifdef __ANDROID__
				if (UNLIKELY(strmh->devh->is_isight))
					continue; // don't look for data after an iSight header
#else
				if (strmh->devh->is_isight) {
					MARK("is_isight");
					continue; // don't look for data after an iSight header
				}
#endif
			}

			if (UNLIKELY(pkt->actual_length < header_len)) {
				strmh->bfh_err |= UVC_STREAM_ERR;
				MARK("bogus packet: actual_len=%d, header_len=%zd", pkt->actual_length, header_len);
				continue;
			}

			if (LIKELY(pkt->actual_length > header_len)) {
				const size_t odd_bytes = pkt->actual_length - header_len;
				_uvc_diag_first_payload(strmh, odd_bytes, "iso");
				if (UNLIKELY(strmh->got_bytes + odd_bytes > strmh->size_buf)) {
					strmh->bfh_err |= UVC_STREAM_ERR;
					UVC_DEBUG("iso bulk would overflow got=%zu odd=%zu cap=%zu",
					    strmh->got_bytes, odd_bytes, strmh->size_buf);
					continue;
				}
				memcpy(strmh->outbuf + strmh->got_bytes, pktbuf + header_len,
				    odd_bytes);
				strmh->got_bytes += odd_bytes;
			}
#ifdef USE_EOF
			if ((pktbuf[1] & UVC_STREAM_EOF) && strmh->got_bytes != 0) {
				_uvc_swap_buffers(strmh, "iso-eof");
			}
#endif
		} else {
			strmh->bfh_err |= UVC_STREAM_ERR;
			MARK("libusb_get_iso_packet_buffer_simple returned null");
			continue;
		}
	}
}

uvc_error_t _uvc_stream_setup_iso_transfers(uvc_stream_handle_t *strmh,
		const struct libusb_interface *interface,
		uvc_format_desc_t *format_desc,
		uint32_t dwMaxVideoFrameSize,
		float bandwidth_factor) {
	const struct libusb_interface_descriptor *altsetting;
	const struct libusb_endpoint_descriptor *endpoint;
	size_t config_bytes_per_packet;
	size_t packets_per_transfer = 0;
	size_t total_transfer_size = 0;
	size_t endpoint_bytes_per_packet;
	int alt_idx, ep_idx;
	struct libusb_transfer *transfer;
	int transfer_id;
	int usb_ret;
	uvc_error_t ret;

	MARK("isochronous transfer mode:num_altsetting=%d", interface->num_altsetting);

	if ((bandwidth_factor > 0) && (bandwidth_factor < 1.0f)) {
		config_bytes_per_packet = (size_t)(strmh->cur_ctrl.dwMaxPayloadTransferSize * bandwidth_factor);
		if (!config_bytes_per_packet) {
			config_bytes_per_packet = strmh->cur_ctrl.dwMaxPayloadTransferSize;
		}
	} else {
		config_bytes_per_packet = strmh->cur_ctrl.dwMaxPayloadTransferSize;
	}

	if (UNLIKELY(!config_bytes_per_packet)) {
		LOGE("config_bytes_per_packet is zero");
		return UVC_ERROR_IO;
	}

	endpoint_bytes_per_packet = 0;
	const int num_alt = interface->num_altsetting - 1;
	for (alt_idx = 0; alt_idx <= num_alt ; alt_idx++) {
		altsetting = interface->altsetting + alt_idx;
		endpoint_bytes_per_packet = 0;

		for (ep_idx = 0; ep_idx < altsetting->bNumEndpoints; ep_idx++) {
			endpoint = altsetting->endpoint + ep_idx;
			if (endpoint->bEndpointAddress == format_desc->parent->bEndpointAddress) {
				endpoint_bytes_per_packet = endpoint->wMaxPacketSize;
				endpoint_bytes_per_packet
					= (endpoint_bytes_per_packet & 0x07ff)
						* (((endpoint_bytes_per_packet >> 11) & 3) + 1);
				break;
			}
		}
		if (LIKELY(endpoint_bytes_per_packet)) {
			if ( (endpoint_bytes_per_packet >= config_bytes_per_packet)
				|| (alt_idx == num_alt) ) {
				packets_per_transfer = (dwMaxVideoFrameSize
						+ endpoint_bytes_per_packet - 1)
						/ endpoint_bytes_per_packet;

				if (packets_per_transfer > 32)
					packets_per_transfer = 32;

				total_transfer_size = packets_per_transfer * endpoint_bytes_per_packet;
				break;
			}
		}
	}
	if (UNLIKELY(!endpoint_bytes_per_packet)) {
		LOGE("endpoint_bytes_per_packet is zero");
		return UVC_ERROR_INVALID_MODE;
	}
	if (UNLIKELY(!total_transfer_size)) {
		LOGE("total_transfer_size is zero");
		return UVC_ERROR_INVALID_MODE;
	}

	MARK("Select the altsetting");
	ret = libusb_set_interface_alt_setting(strmh->devh->usb_devh,
			altsetting->bInterfaceNumber, altsetting->bAlternateSetting);
	if (UNLIKELY(ret != UVC_SUCCESS)) {
		UVC_DEBUG("libusb_set_interface_alt_setting failed");
		return ret;
	}
	strmh->diag_selected_altsetting = altsetting->bAlternateSetting;

	MARK("Set up the transfers");
	for (transfer_id = 0; transfer_id < LIBUVC_NUM_TRANSFER_BUFS; ++transfer_id) {
		transfer = libusb_alloc_transfer((int)packets_per_transfer);
		strmh->transfers[transfer_id] = transfer;
		strmh->transfer_bufs[transfer_id] = malloc(total_transfer_size);
		if (UNLIKELY(!transfer || !strmh->transfer_bufs[transfer_id])) {
			ret = UVC_ERROR_NO_MEM;
			_uvc_free_transfer(strmh, transfer_id);
			return ret;
		}

		libusb_fill_iso_transfer(transfer, strmh->devh->usb_devh,
			format_desc->parent->bEndpointAddress,
			strmh->transfer_bufs[transfer_id], (int)total_transfer_size,
			(int)packets_per_transfer, _uvc_stream_callback,
			(void*) strmh, LIBUVC_STREAM_XFER_TIMEOUT_MS);

		libusb_set_iso_packet_lengths(transfer, endpoint_bytes_per_packet);

		usb_ret = libusb_prealloc_iso_urbs(transfer);
		if (UNLIKELY(usb_ret != LIBUSB_SUCCESS)) {
			UVC_DEBUG("libusb_prealloc_iso_urbs failed: %d", usb_ret);
			_uvc_free_transfer(strmh, transfer_id);
			return UVC_ERROR_NO_MEM;
		}
	}
	return UVC_SUCCESS;
}
