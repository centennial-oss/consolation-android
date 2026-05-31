/*
 * libusb_prealloc_ext.c — URB pre-allocation for hot-path bulk and ISO streaming.
 *
 * This file is #include'd at the end of linux_usbfs.c (before usbi_backend) by
 * the patch in libusb-1.0.30-prealloc.patch.  It is intentionally NOT a standalone
 * compilation unit: it relies on static types, macros, and helper functions
 * defined earlier in linux_usbfs.c (linux_transfer_priv, usbfs_urb,
 * IOCTL_USBFS_SUBMITURB, max_iso_packet_len, free_iso_urbs, etc.).
 *
 * Usage from libuvc (or any caller):
 *   #include "libusb_prealloc.h"
 *
 *   // Before first submit:
 *   libusb_prealloc_bulk_urbs(transfer);  // idempotent
 *
 *   // On each resubmit the fast path in submit_bulk_transfer fires
 *   // automatically — zero heap allocation per cycle.
 *
 *   // URBs are freed by libusb_free_transfer() via op_clear_transfer_priv().
 *
 * To re-apply against a new libusb version:
 *   patch -p1 -d libusb-X.Y.Z < patches/libusb-1.0.30-prealloc/libusb-1.0.30-prealloc.patch
 *   cp patches/libusb-1.0.30-prealloc/libusb_prealloc_ext.c libusb-X.Y.Z/libusb/os/
 *   cp patches/libusb-1.0.30-prealloc/libusb_prealloc.h     libusb-X.Y.Z/libusb/
 */

/**
 * Pre-allocate kernel URBs for a bulk (or interrupt) transfer.
 *
 * Call once before the first submit.  On every subsequent
 * libusb_submit_transfer() the fast path in submit_bulk_transfer() fires:
 * it resets three status fields per URB and calls SUBMITURB ioctl directly,
 * with zero heap allocation.
 *
 * The URBs are freed automatically when libusb_free_transfer() is called.
 *
 * Returns LIBUSB_SUCCESS or a LIBUSB_ERROR_* code.
 */
int API_EXPORTED libusb_prealloc_bulk_urbs(struct libusb_transfer *transfer)
{
	struct usbi_transfer *itransfer = LIBUSB_TRANSFER_TO_USBI_TRANSFER(transfer);
	struct linux_transfer_priv *tpriv = usbi_get_transfer_priv(itransfer);
	struct linux_device_handle_priv *hpriv =
		usbi_get_device_handle_priv(transfer->dev_handle);
	struct usbfs_urb *urbs;
	int is_out = IS_XFEROUT(transfer);
	int bulk_buffer_len, use_bulk_continuation;
	int num_urbs;
	int last_urb_partial = 0;
	int i;

	if (tpriv->bulk_urbs_preallocated)
		return LIBUSB_SUCCESS;

	if (transfer->type != LIBUSB_TRANSFER_TYPE_BULK &&
	    transfer->type != LIBUSB_TRANSFER_TYPE_BULK_STREAM &&
	    transfer->type != LIBUSB_TRANSFER_TYPE_INTERRUPT)
		return LIBUSB_ERROR_INVALID_PARAM;

	/* Mirror submit_bulk_transfer()'s buffer-length selection exactly so
	 * the pre-allocated URBs match what the submit path would have built. */
	if (hpriv->caps & USBFS_CAP_BULK_SCATTER_GATHER) {
		bulk_buffer_len = transfer->length ? transfer->length : 1;
		use_bulk_continuation = 0;
	} else if (hpriv->caps & USBFS_CAP_BULK_CONTINUATION) {
		bulk_buffer_len = MAX_BULK_BUFFER_LENGTH;
		use_bulk_continuation = 1;
	} else if (hpriv->caps & USBFS_CAP_NO_PACKET_SIZE_LIM) {
		bulk_buffer_len = transfer->length ? transfer->length : 1;
		use_bulk_continuation = 0;
	} else {
		bulk_buffer_len = MAX_BULK_BUFFER_LENGTH;
		use_bulk_continuation = 0;
	}

	num_urbs = transfer->length / bulk_buffer_len;
	if (transfer->length == 0) {
		num_urbs = 1;
	} else if ((transfer->length % bulk_buffer_len) > 0) {
		last_urb_partial = 1;
		num_urbs++;
	}

	urbs = calloc(num_urbs, sizeof(*urbs));
	if (!urbs)
		return LIBUSB_ERROR_NO_MEM;

	for (i = 0; i < num_urbs; i++) {
		struct usbfs_urb *urb = &urbs[i];
		urb->usercontext = itransfer;
		switch (transfer->type) {
		case LIBUSB_TRANSFER_TYPE_BULK:
			urb->type = USBFS_URB_TYPE_BULK;
			urb->stream_id = 0;
			break;
		case LIBUSB_TRANSFER_TYPE_BULK_STREAM:
			urb->type = USBFS_URB_TYPE_BULK;
			urb->stream_id = itransfer->stream_id;
			break;
		case LIBUSB_TRANSFER_TYPE_INTERRUPT:
			urb->type = USBFS_URB_TYPE_INTERRUPT;
			break;
		default:
			break;
		}
		urb->endpoint = transfer->endpoint;
		urb->buffer = transfer->buffer + (i * bulk_buffer_len);
		if (use_bulk_continuation && !is_out && (i < num_urbs - 1))
			urb->flags = USBFS_URB_SHORT_NOT_OK;
		if (i == num_urbs - 1 && last_urb_partial)
			urb->buffer_length = transfer->length % bulk_buffer_len;
		else if (transfer->length == 0)
			urb->buffer_length = 0;
		else
			urb->buffer_length = bulk_buffer_len;
		if (i > 0 && use_bulk_continuation)
			urb->flags |= USBFS_URB_BULK_CONTINUATION;
		if (is_out && i == num_urbs - 1 &&
		    (transfer->flags & LIBUSB_TRANSFER_ADD_ZERO_PACKET))
			urb->flags |= USBFS_URB_ZERO_PACKET;
	}

	tpriv->urbs = urbs;
	tpriv->num_urbs = num_urbs;
	tpriv->bulk_urbs_preallocated = 1;
	usbi_dbg(TRANSFER_CTX(transfer), "preallocated %d bulk URBs for transfer", num_urbs);
	return LIBUSB_SUCCESS;
}

/**
 * Pre-allocate kernel URBs for an isochronous transfer.
 *
 * Call once after the transfer's iso_packet_desc[] array is fully populated
 * and before the first submit.  On every subsequent libusb_submit_transfer()
 * the fast path in submit_iso_transfer() fires: it resets bookkeeping and
 * per-packet status/actual_length fields in-place and calls SUBMITURB ioctl
 * directly, with zero heap allocation.
 *
 * The URBs are freed automatically when libusb_free_transfer() is called.
 *
 * Returns LIBUSB_SUCCESS or a LIBUSB_ERROR_* code.
 */
int API_EXPORTED libusb_prealloc_iso_urbs(struct libusb_transfer *transfer)
{
	struct usbi_transfer *itransfer = LIBUSB_TRANSFER_TO_USBI_TRANSFER(transfer);
	struct linux_transfer_priv *tpriv = usbi_get_transfer_priv(itransfer);
	struct usbfs_urb **urbs;
	int num_packets = transfer->num_iso_packets;
	int num_packets_remaining;
	int i, j;
	int num_urbs;
	unsigned int packet_len;
	unsigned int total_len = 0;
	unsigned char *urb_buffer = transfer->buffer;

	if (tpriv->iso_urbs_preallocated)
		return LIBUSB_SUCCESS;

	if (num_packets < 1)
		return LIBUSB_ERROR_INVALID_PARAM;

	/* Mirror submit_iso_transfer()'s validation exactly. */
	for (i = 0; i < num_packets; i++) {
		packet_len = transfer->iso_packet_desc[i].length;
		if (packet_len > max_iso_packet_len)
			return LIBUSB_ERROR_INVALID_PARAM;
		total_len += packet_len;
	}

	if (transfer->length < (int)total_len)
		return LIBUSB_ERROR_INVALID_PARAM;

	num_urbs = (num_packets + (MAX_ISO_PACKETS_PER_URB - 1)) / MAX_ISO_PACKETS_PER_URB;

	urbs = calloc(num_urbs, sizeof(*urbs));
	if (!urbs)
		return LIBUSB_ERROR_NO_MEM;

	num_packets_remaining = num_packets;
	for (i = 0, j = 0; i < num_urbs; i++) {
		int num_packets_in_urb = MIN(num_packets_remaining, MAX_ISO_PACKETS_PER_URB);
		struct usbfs_urb *urb;
		size_t alloc_size;
		int k;

		alloc_size = sizeof(*urb)
			+ (num_packets_in_urb * sizeof(struct usbfs_iso_packet_desc));
		urb = calloc(1, alloc_size);
		if (!urb) {
			int m;
			for (m = 0; m < i; m++)
				free(urbs[m]);
			free(urbs);
			return LIBUSB_ERROR_NO_MEM;
		}
		urbs[i] = urb;

		for (k = 0; k < num_packets_in_urb; j++, k++) {
			packet_len = transfer->iso_packet_desc[j].length;
			urb->buffer_length += packet_len;
			urb->iso_frame_desc[k].length = packet_len;
		}

		urb->usercontext = itransfer;
		urb->type = USBFS_URB_TYPE_ISO;
		urb->flags = USBFS_URB_ISO_ASAP;
		urb->endpoint = transfer->endpoint;
		urb->number_of_packets = num_packets_in_urb;
		urb->buffer = urb_buffer;

		urb_buffer += urb->buffer_length;
		num_packets_remaining -= num_packets_in_urb;
	}

	tpriv->iso_urbs = urbs;
	tpriv->num_urbs = num_urbs;
	tpriv->iso_urbs_preallocated = 1;
	usbi_dbg(TRANSFER_CTX(transfer), "preallocated %d ISO URBs for transfer", num_urbs);
	return LIBUSB_SUCCESS;
}
