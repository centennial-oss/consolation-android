/*
 * libusb_prealloc.h — public declarations for the URB pre-allocation extension.
 *
 * Dropped into libusb-1.0.29/libusb/ by `make patch-libusb`.
 * Include this alongside <libusb.h> when calling the prealloc API.
 *
 * See patches/libusb-1.0.29-prealloc/libusb_prealloc_ext.c for the
 * implementation and full usage notes.
 */

#ifndef LIBUSB_PREALLOC_H
#define LIBUSB_PREALLOC_H

#include <libusb.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Pre-allocate kernel URBs for a bulk or interrupt transfer.
 *
 * Call once before the first libusb_submit_transfer().  Subsequent submits
 * skip calloc/free and issue SUBMITURB ioctls directly (zero heap allocation
 * on the hot resubmit path).
 *
 * Idempotent — safe to call multiple times on the same transfer.
 * The URBs are freed automatically by libusb_free_transfer().
 *
 * @param transfer  A fully configured bulk or interrupt transfer.
 * @returns         LIBUSB_SUCCESS or a LIBUSB_ERROR_* code.
 */
int libusb_prealloc_bulk_urbs(struct libusb_transfer *transfer);

/**
 * Pre-allocate kernel URBs for an isochronous transfer.
 *
 * Call once after iso_packet_desc[] is populated and before the first
 * libusb_submit_transfer().  Subsequent submits reset per-packet
 * status/actual_length fields in-place and issue SUBMITURB ioctls directly
 * (zero heap allocation on the hot resubmit path).
 *
 * Idempotent — safe to call multiple times on the same transfer.
 * The URBs are freed automatically by libusb_free_transfer().
 *
 * @param transfer  A fully configured isochronous transfer.
 * @returns         LIBUSB_SUCCESS or a LIBUSB_ERROR_* code.
 */
int libusb_prealloc_iso_urbs(struct libusb_transfer *transfer);

#ifdef __cplusplus
}
#endif

#endif /* LIBUSB_PREALLOC_H */
