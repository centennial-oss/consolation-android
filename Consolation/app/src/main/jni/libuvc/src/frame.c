/*********************************************************************
 *********************************************************************/
/*********************************************************************
 * added and modified some function for support and help for Android
 * Copyright (C) 2014 saki@serenegiant All rights reserved.
 * add:
 * 	added some helper functions for supporting rgb565 and rgbx8888
 * modified:
 * 	modified for optimization with gcc
 * 	modified macros that convert pixel format to reduce cpu cycles
 * 	added boundary check of pointer in the converting function to avoid crash
 *********************************************************************/

/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (C) 2010-2012 Ken Tossell
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the author nor other contributors may be
 *     used to endorse or promote products derived from this software
 *     without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/
/**
 * @defgroup frame Frame processing
 * @brief Tools for managing frame buffers and converting between image formats
 */
#include "libuvc/libuvc.h"
#include "libuvc/libuvc_internal.h"
#include "libyuv/convert_argb.h"
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define LIBUVC_HAS_ARM_NEON 1
#else
#define LIBUVC_HAS_ARM_NEON 0
#endif

#define USE_STRIDE 1

static inline uvc_error_t libyuv_result_to_uvc_error(int result) {
	return result == 0 ? UVC_SUCCESS : UVC_ERROR_INVALID_PARAM;
}

typedef uvc_error_t (*uvc_rgbx_converter_func_t)(uvc_frame_t *in, uvc_frame_t *out);
static uvc_error_t internal_yuyv2rgbx(uvc_frame_t *in, uvc_frame_t *out);
static uvc_error_t internal_nv122rgbx(uvc_frame_t *in, uvc_frame_t *out);
static uvc_error_t internal_yu122rgbx(uvc_frame_t *in, uvc_frame_t *out);
static uvc_error_t internal_bgr2rgbx(uvc_frame_t *in, uvc_frame_t *out);
static uvc_error_t internal_p0102rgbx(uvc_frame_t *in, uvc_frame_t *out);
static uvc_error_t libyuv_yuyv2rgbx(uvc_frame_t *in, uvc_frame_t *out);
static uvc_error_t libyuv_nv122rgbx(uvc_frame_t *in, uvc_frame_t *out);
static uvc_error_t libyuv_yu122rgbx(uvc_frame_t *in, uvc_frame_t *out);
static uvc_error_t libyuv_bgr2rgbx(uvc_frame_t *in, uvc_frame_t *out);
static uvc_error_t libyuv_p0102rgbx(uvc_frame_t *in, uvc_frame_t *out);

static uvc_rgbx_converter_func_t s_yuyv2rgbx_func = internal_yuyv2rgbx;
static uvc_rgbx_converter_func_t s_nv122rgbx_func = internal_nv122rgbx;
static uvc_rgbx_converter_func_t s_yu122rgbx_func = internal_yu122rgbx;
static uvc_rgbx_converter_func_t s_bgr2rgbx_func = internal_bgr2rgbx;
static uvc_rgbx_converter_func_t s_p0102rgbx_func = internal_p0102rgbx;

void uvc_set_rgbx_converter_backend(uvc_rgbx_converter_backend_t backend) {
	switch (backend) {
	case UVC_RGBX_CONVERTER_BACKEND_LIBYUV:
		s_yuyv2rgbx_func = libyuv_yuyv2rgbx;
		s_nv122rgbx_func = libyuv_nv122rgbx;
		s_yu122rgbx_func = libyuv_yu122rgbx;
		s_bgr2rgbx_func = libyuv_bgr2rgbx;
		s_p0102rgbx_func = libyuv_p0102rgbx;
		break;
	case UVC_RGBX_CONVERTER_BACKEND_INTERNAL:
	default:
		s_yuyv2rgbx_func = internal_yuyv2rgbx;
		s_nv122rgbx_func = internal_nv122rgbx;
		s_yu122rgbx_func = internal_yu122rgbx;
		s_bgr2rgbx_func = internal_bgr2rgbx;
		s_p0102rgbx_func = internal_p0102rgbx;
		break;
	}
}

/** @internal */
uvc_error_t uvc_ensure_frame_size(uvc_frame_t *frame, size_t need_bytes) {
	if (UNLIKELY(!need_bytes))
		return UVC_ERROR_NO_MEM;

	if LIKELY(frame->library_owns_data) {
		if UNLIKELY(!frame->data || frame->data_bytes < need_bytes) {
			void *resized = realloc(frame->data, need_bytes);
			if (UNLIKELY(!resized))
				return UVC_ERROR_NO_MEM;
			frame->data = resized;
			frame->data_bytes = need_bytes;
		}
		if (UNLIKELY(!frame->data))
			return UVC_ERROR_NO_MEM;
		frame->actual_bytes = need_bytes;
		return UVC_SUCCESS;
	} else {
		if (UNLIKELY(!frame->data || frame->data_bytes < need_bytes))
			return UVC_ERROR_NO_MEM;
		frame->actual_bytes = need_bytes;
		return UVC_SUCCESS;
	}
}

/** @brief Allocate a frame structure
 * @ingroup frame
 *
 * @param data_bytes Number of bytes to allocate, or zero
 * @return New frame, or NULL on error
 */
uvc_frame_t *uvc_allocate_frame(size_t data_bytes) {
	uvc_frame_t *frame = malloc(sizeof(*frame));	// FIXME using buffer pool is better performance(5-30%) than directory use malloc everytime.

	if (UNLIKELY(!frame))
		return NULL;

	/* Initialise struct on all targets (was skipped on Android for perf); uninitialized
	 * scalar fields confuse downstream logic and UB. Pool-style reuse still sets payloads. */
	memset(frame, 0, sizeof(*frame));
//	frame->library_owns_data = 1;	// XXX moved to lower

	if (LIKELY(data_bytes > 0)) {
		frame->library_owns_data = 1;
		frame->actual_bytes = frame->data_bytes = data_bytes;	// XXX
		frame->data = malloc(data_bytes);

		if (UNLIKELY(!frame->data)) {
			free(frame);
			return NULL ;
		}
	}

	return frame;
}

/** @brief Free a frame structure
 * @ingroup frame
 *
 * @param frame Frame to destroy
 */
void uvc_free_frame(uvc_frame_t *frame) {
	if ((frame->data_bytes > 0) && frame->library_owns_data)
		free(frame->data);

	free(frame);
}

static inline unsigned char sat(int i) {
	return (unsigned char) (i >= 255 ? 255 : (i < 0 ? 0 : i));
}

static inline void store_rgb565(uint8_t *dst, int r, int g, int b) {
	const uint8_t rr = sat(r);
	const uint8_t gg = sat(g);
	const uint8_t bb = sat(b);
	dst[0] = ((gg << 3) & 0xe0) | ((bb >> 3) & 0x1f);
	dst[1] = (rr & 0xf8) | ((gg >> 5) & 0x07);
}

/** @brief Duplicate a frame, preserving color format
 * @ingroup frame
 *
 * @param in Original frame
 * @param out Duplicate frame
 */
uvc_error_t uvc_duplicate_frame(uvc_frame_t *in, uvc_frame_t *out) {
	const size_t copy_bytes = (in->actual_bytes > 0 && in->actual_bytes <= in->data_bytes)
		? in->actual_bytes : in->data_bytes;

	if (UNLIKELY(uvc_ensure_frame_size(out, copy_bytes) < 0))
		return UVC_ERROR_NO_MEM;

	out->width = in->width;
	out->height = in->height;
	out->frame_format = in->frame_format;
	if (out->library_owns_data)
		out->step = in->step;
	out->sequence = in->sequence;
	out->capture_time = in->capture_time;
	out->arrival_monotonic_ns = in->arrival_monotonic_ns;
	out->source = in->source;
	out->actual_bytes = copy_bytes;
	out->library_frame_owner = NULL;
	out->library_frame_slot = 0;

#if USE_STRIDE	 // XXX
	if (in->step && out->step) {
		const int istep = in->step;
		const int ostep = out->step;
		const int hh = in->height < out->height ? in->height : out->height;
		const int rowbytes = istep < ostep ? istep : ostep;
		const size_t input_span = (hh > 0 && istep > 0 && rowbytes > 0)
			? ((size_t)(hh - 1) * (size_t)istep) + (size_t)rowbytes : 0;
		const size_t output_span = (hh > 0 && ostep > 0 && rowbytes > 0)
			? ((size_t)(hh - 1) * (size_t)ostep) + (size_t)rowbytes : 0;
		register void *ip = in->data;
		register void *op = out->data;
		int h;
		/* One row per iteration -- 4x unroll advanced h by 4 always, reading past the
		 * last row when hh % 4 != 0 (buffer overrun). */
		if (hh > 0 && istep > 0 && ostep > 0 && rowbytes > 0 &&
				input_span <= copy_bytes && output_span <= out->data_bytes) {
			for (h = 0; h < hh; h++) {
				memcpy(op, ip, rowbytes);
				ip += istep;
				op += ostep;
			}
		} else {
			memcpy(out->data, in->data, copy_bytes);
		}
	} else {
		// compressed format? XXX if only one of the frame in / out has step, this may lead to crash...
		memcpy(out->data, in->data, copy_bytes);
	}
#else
	memcpy(out->data, in->data, copy_bytes); // XXX
#endif
	return UVC_SUCCESS;
}

#define PIXEL_RGB565		2
#define PIXEL_UYVY			2
#define PIXEL_YUYV			2
#define PIXEL_RGB			3
#define PIXEL_BGR			3
#define PIXEL_RGBX			4

#define PIXEL2_RGB565		PIXEL_RGB565 * 2
#define PIXEL2_UYVY			PIXEL_UYVY * 2
#define PIXEL2_YUYV			PIXEL_YUYV * 2
#define PIXEL2_RGB			PIXEL_RGB * 2
#define PIXEL2_BGR			PIXEL_BGR * 2
#define PIXEL2_RGBX			PIXEL_RGBX * 2

#define PIXEL4_RGB565		PIXEL_RGB565 * 4
#define PIXEL4_UYVY			PIXEL_UYVY * 4
#define PIXEL4_YUYV			PIXEL_YUYV * 4
#define PIXEL4_RGB			PIXEL_RGB * 4
#define PIXEL4_BGR			PIXEL_BGR * 4
#define PIXEL4_RGBX			PIXEL_RGBX * 4

#define PIXEL8_RGB565		PIXEL_RGB565 * 8
#define PIXEL8_UYVY			PIXEL_UYVY * 8
#define PIXEL8_YUYV			PIXEL_YUYV * 8
#define PIXEL8_RGB			PIXEL_RGB * 8
#define PIXEL8_BGR			PIXEL_BGR * 8
#define PIXEL8_RGBX			PIXEL_RGBX * 8

#define PIXEL16_RGB565		PIXEL_RGB565 * 16
#define PIXEL16_UYVY		PIXEL_UYVY * 16
#define PIXEL16_YUYV		PIXEL_YUYV * 16
#define PIXEL16_RGB			PIXEL_RGB * 16
#define PIXEL16_BGR			PIXEL_BGR * 16
#define PIXEL16_RGBX		PIXEL_RGBX * 16

#define RGB2RGBX_2(prgb, prgbx, ax, bx) { \
		(prgbx)[bx+0] = (prgb)[ax+0]; \
		(prgbx)[bx+1] = (prgb)[ax+1]; \
		(prgbx)[bx+2] = (prgb)[ax+2]; \
		(prgbx)[bx+3] = 0xff; \
		(prgbx)[bx+4] = (prgb)[ax+3]; \
		(prgbx)[bx+5] = (prgb)[ax+4]; \
		(prgbx)[bx+6] = (prgb)[ax+5]; \
		(prgbx)[bx+7] = 0xff; \
	}
#define RGB2RGBX_16(prgb, prgbx, ax, bx) \
	RGB2RGBX_8(prgb, prgbx, ax, bx) \
	RGB2RGBX_8(prgb, prgbx, ax + PIXEL8_RGB, bx +PIXEL8_RGBX);
#define RGB2RGBX_8(prgb, prgbx, ax, bx) \
	RGB2RGBX_4(prgb, prgbx, ax, bx) \
	RGB2RGBX_4(prgb, prgbx, ax + PIXEL4_RGB, bx + PIXEL4_RGBX);
#define RGB2RGBX_4(prgb, prgbx, ax, bx) \
	RGB2RGBX_2(prgb, prgbx, ax, bx) \
	RGB2RGBX_2(prgb, prgbx, ax + PIXEL2_RGB, bx + PIXEL2_RGBX);

/** @brief Convert a frame from RGB888 to RGBX8888
 * @ingroup frame
 * @param ini RGB888 frame
 * @param out RGBX8888 frame
 */
uvc_error_t uvc_rgb2rgbx(uvc_frame_t *in, uvc_frame_t *out) {
	if (UNLIKELY(in->frame_format != UVC_FRAME_FORMAT_RGB))
		return UVC_ERROR_INVALID_PARAM;

	if (UNLIKELY(uvc_ensure_frame_size(out, in->width * in->height * PIXEL_RGBX) < 0))
		return UVC_ERROR_NO_MEM;

	out->width = in->width;
	out->height = in->height;
	out->frame_format = UVC_FRAME_FORMAT_RGBX;
	if (out->library_owns_data)
		out->step = in->width * PIXEL_RGBX;
	out->sequence = in->sequence;
	out->capture_time = in->capture_time;
	out->source = in->source;

	uint8_t *prgb = in->data;
	const uint8_t *prgb_end = prgb + in->data_bytes - PIXEL8_RGB;
	uint8_t *prgbx = out->data;
	const uint8_t *prgbx_end = prgbx + out->data_bytes - PIXEL8_RGBX;

	// RGB888 to RGBX8888
#if USE_STRIDE
	if (in->step && out->step && (in->step != out->step)) {
		const int hh = in->height < out->height ? in->height : out->height;
		const int ww = in->width < out->width ? in->width : out->width;
		int h, w;
		for (h = 0; h < hh; h++) {
			w = 0;
			prgb = in->data + in->step * h;
			prgbx = out->data + out->step * h;
			for (; (prgbx <= prgbx_end) && (prgb <= prgb_end) && (w < ww) ;) {
				RGB2RGBX_8(prgb, prgbx, 0, 0);

				prgb += PIXEL8_RGB;
				prgbx += PIXEL8_RGBX;
				w += 8;
			}
		}
	} else {
		// compressed format? XXX if only one of the frame in / out has step, this may lead to crash...
		for (; (prgbx <= prgbx_end) && (prgb <= prgb_end) ;) {
			RGB2RGBX_8(prgb, prgbx, 0, 0);

			prgb += PIXEL8_RGB;
			prgbx += PIXEL8_RGBX;
		}
	}
#else
	for (; (prgbx <= prgbx_end) && (prgb <= prgb_end) ;) {
		RGB2RGBX_8(prgb, prgbx, 0, 0);

		prgb += PIXEL8_RGB;
		prgbx += PIXEL8_RGBX;
	}
#endif
	return UVC_SUCCESS;
}

// prgb565[0] = ((g << 3) & 0b11100000) | ((b >> 3) & 0b00011111);	// low byte
// prgb565[1] = ((r & 0b11111000) | ((g >> 5) & 0b00000111)); 		// high byte
#define RGB2RGB565_2(prgb, prgb565, ax, bx) { \
		(prgb565)[bx+0] = (((prgb)[ax+1] << 3) & 0b11100000) | (((prgb)[ax+2] >> 3) & 0b00011111); \
		(prgb565)[bx+1] = (((prgb)[ax+0] & 0b11111000) | (((prgb)[ax+1] >> 5) & 0b00000111)); \
		(prgb565)[bx+2] = (((prgb)[ax+4] << 3) & 0b11100000) | (((prgb)[ax+5] >> 3) & 0b00011111); \
		(prgb565)[bx+3] = (((prgb)[ax+3] & 0b11111000) | (((prgb)[ax+4] >> 5) & 0b00000111)); \
    }
#define RGB2RGB565_16(prgb, prgb565, ax, bx) \
	RGB2RGB565_8(prgb, prgb565, ax, bx) \
	RGB2RGB565_8(prgb, prgb565, ax + PIXEL8_RGB, bx + PIXEL8_RGB565);
#define RGB2RGB565_8(prgb, prgb565, ax, bx) \
	RGB2RGB565_4(prgb, prgb565, ax, bx) \
	RGB2RGB565_4(prgb, prgb565, ax + PIXEL4_RGB, bx + PIXEL4_RGB565);
#define RGB2RGB565_4(prgb, prgb565, ax, bx) \
	RGB2RGB565_2(prgb, prgb565, ax, bx) \
	RGB2RGB565_2(prgb, prgb565, ax + PIXEL2_RGB, bx + PIXEL2_RGB565);

/** @brief Convert a frame from RGB888 to RGB565
 * @ingroup frame
 * @param ini RGB888 frame
 * @param out RGB565 frame
 */
uvc_error_t uvc_rgb2rgb565(uvc_frame_t *in, uvc_frame_t *out) {
	if (UNLIKELY(in->frame_format != UVC_FRAME_FORMAT_RGB))
		return UVC_ERROR_INVALID_PARAM;

	if (UNLIKELY(uvc_ensure_frame_size(out, in->width * in->height * PIXEL_RGB565) < 0))
		return UVC_ERROR_NO_MEM;

	out->width = in->width;
	out->height = in->height;
	out->frame_format = UVC_FRAME_FORMAT_RGB565;
	if (out->library_owns_data)
		out->step = in->width * PIXEL_RGB565;
	out->sequence = in->sequence;
	out->capture_time = in->capture_time;
	out->source = in->source;

	uint8_t *prgb = in->data;
	const uint8_t *prgb_end = prgb + in->data_bytes - PIXEL8_RGB;
	uint8_t *prgb565 = out->data;
	const uint8_t *prgb565_end = prgb565 + out->data_bytes - PIXEL8_RGB565;

	// RGB888 to RGB565
#if USE_STRIDE
	if (in->step && out->step && (in->step != out->step)) {
		const int hh = in->height < out->height ? in->height : out->height;
		const int ww = in->width < out->width ? in->width : out->width;
		int h, w;
		for (h = 0; h < hh; h++) {
			w = 0;
			prgb = in->data + in->step * h;
			prgb565 = out->data + out->step * h;
			for (; (prgb565 <= prgb565_end) && (prgb <= prgb_end) && (w < ww) ;) {
				RGB2RGB565_8(prgb, prgb565, 0, 0);

				prgb += PIXEL8_RGB;
				prgb565 += PIXEL8_RGB565;
				w += 8;
			}
		}
	} else {
		// compressed format? XXX if only one of the frame in / out has step, this may lead to crash...
		for (; (prgb565 <= prgb565_end) && (prgb <= prgb_end) ;) {
			RGB2RGB565_8(prgb, prgb565, 0, 0);

			prgb += PIXEL8_RGB;
			prgb565 += PIXEL8_RGB565;
		}
	}
#else
	for (; (prgb565 <= prgb565_end) && (prgb <= prgb_end) ;) {
		RGB2RGB565_8(prgb, prgb565, 0, 0);

		prgb += PIXEL8_RGB;
		prgb565 += PIXEL8_RGB565;
	}
#endif
	return UVC_SUCCESS;
}
/*
 #define YUYV2RGB_2(pyuv, prgb) { \
    float r = 1.402f * ((pyuv)[3]-128); \
    float g = -0.34414f * ((pyuv)[1]-128) - 0.71414f * ((pyuv)[3]-128); \
    float b = 1.772f * ((pyuv)[1]-128); \
    (prgb)[0] = sat(pyuv[0] + r); \
    (prgb)[1] = sat(pyuv[0] + g); \
    (prgb)[2] = sat(pyuv[0] + b); \
    (prgb)[3] = sat(pyuv[2] + r); \
    (prgb)[4] = sat(pyuv[2] + g); \
    (prgb)[5] = sat(pyuv[2] + b); \
    }
*/

#define IYUYV2RGB_2(pyuv, prgb, ax, bx) { \
		const int d1 = (pyuv)[ax+1]; \
		const int d3 = (pyuv)[ax+3]; \
		const int r = (22987 * (d3/*(pyuv)[ax+3]*/ - 128)) >> 14; \
		const int g = (-5636 * (d1/*(pyuv)[ax+1]*/ - 128) - 11698 * (d3/*(pyuv)[ax+3]*/ - 128)) >> 14; \
		const int b = (29049 * (d1/*(pyuv)[ax+1]*/ - 128)) >> 14; \
		const int y0 = (pyuv)[ax+0]; \
		(prgb)[bx+0] = sat(y0 + r); \
		(prgb)[bx+1] = sat(y0 + g); \
		(prgb)[bx+2] = sat(y0 + b); \
		const int y2 = (pyuv)[ax+2]; \
		(prgb)[bx+3] = sat(y2 + r); \
		(prgb)[bx+4] = sat(y2 + g); \
		(prgb)[bx+5] = sat(y2 + b); \
    }
#define IYUYV2RGB_16(pyuv, prgb, ax, bx) \
	IYUYV2RGB_8(pyuv, prgb, ax, bx) \
	IYUYV2RGB_8(pyuv, prgb, ax + PIXEL8_YUYV, bx + PIXEL8_RGB)
#define IYUYV2RGB_8(pyuv, prgb, ax, bx) \
	IYUYV2RGB_4(pyuv, prgb, ax, bx) \
	IYUYV2RGB_4(pyuv, prgb, ax + PIXEL4_YUYV, bx + PIXEL4_RGB)
#define IYUYV2RGB_4(pyuv, prgb, ax, bx) \
	IYUYV2RGB_2(pyuv, prgb, ax, bx) \
	IYUYV2RGB_2(pyuv, prgb, ax + PIXEL2_YUYV, bx + PIXEL2_RGB)

#define IYUYV2RGB565_2(pyuv, prgb565, ax, bx) { \
		const int d1 = (pyuv)[ax+1]; \
		const int d3 = (pyuv)[ax+3]; \
		const int r = (22987 * (d3 - 128)) >> 14; \
		const int g = (-5636 * (d1 - 128) - 11698 * (d3 - 128)) >> 14; \
		const int b = (29049 * (d1 - 128)) >> 14; \
		const int y0 = (pyuv)[ax+0]; \
		store_rgb565((prgb565) + bx, y0 + r, y0 + g, y0 + b); \
		const int y2 = (pyuv)[ax+2]; \
		store_rgb565((prgb565) + bx + PIXEL_RGB565, y2 + r, y2 + g, y2 + b); \
    }
#define IYUYV2RGB565_16(pyuv, prgb565, ax, bx) \
	IYUYV2RGB565_8(pyuv, prgb565, ax, bx) \
	IYUYV2RGB565_8(pyuv, prgb565, ax + PIXEL8_YUYV, bx + PIXEL8_RGB565)
#define IYUYV2RGB565_8(pyuv, prgb565, ax, bx) \
	IYUYV2RGB565_4(pyuv, prgb565, ax, bx) \
	IYUYV2RGB565_4(pyuv, prgb565, ax + PIXEL4_YUYV, bx + PIXEL4_RGB565)
#define IYUYV2RGB565_4(pyuv, prgb565, ax, bx) \
	IYUYV2RGB565_2(pyuv, prgb565, ax, bx) \
	IYUYV2RGB565_2(pyuv, prgb565, ax + PIXEL2_YUYV, bx + PIXEL2_RGB565)

/** @brief Convert a frame from YUYV to RGB888
 * @ingroup frame
 *
 * @param in YUYV frame
 * @param out RGB888 frame
 */
uvc_error_t uvc_yuyv2rgb(uvc_frame_t *in, uvc_frame_t *out) {
	if (UNLIKELY(in->frame_format != UVC_FRAME_FORMAT_YUYV))
		return UVC_ERROR_INVALID_PARAM;

	if (UNLIKELY(uvc_ensure_frame_size(out, in->width * in->height * PIXEL_RGB) < 0))
		return UVC_ERROR_NO_MEM;

	out->width = in->width;
	out->height = in->height;
	out->frame_format = UVC_FRAME_FORMAT_RGB;
	if (out->library_owns_data)
		out->step = in->width * PIXEL_RGB;
	out->sequence = in->sequence;
	out->capture_time = in->capture_time;
	out->source = in->source;

	uint8_t *pyuv = in->data;
	const uint8_t *pyuv_end = pyuv + in->data_bytes - PIXEL8_YUYV;
	uint8_t *prgb = out->data;
	const uint8_t *prgb_end = prgb + out->data_bytes - PIXEL8_RGB;

#if USE_STRIDE
	if (in->step && out->step && (in->step != out->step)) {
		const int hh = in->height < out->height ? in->height : out->height;
		const int ww = in->width < out->width ? in->width : out->width;
		int h, w;
		for (h = 0; h < hh; h++) {
			w = 0;
			pyuv = in->data + in->step * h;
			prgb = out->data + out->step * h;
			for (; (prgb <= prgb_end) && (pyuv <= pyuv_end) && (w < ww) ;) {
				IYUYV2RGB_8(pyuv, prgb, 0, 0);

				prgb += PIXEL8_RGB;
				pyuv += PIXEL8_YUYV;
				w += 8;
			}
		}
	} else {
		// compressed format? XXX if only one of the frame in / out has step, this may lead to crash...
		for (; (prgb <= prgb_end) && (pyuv <= pyuv_end) ;) {
			IYUYV2RGB_8(pyuv, prgb, 0, 0);

			prgb += PIXEL8_RGB;
			pyuv += PIXEL8_YUYV;
		}
	}
#else
	// YUYV => RGB888
	for (; (prgb <= prgb_end) && (pyuv <= pyuv_end) ;) {
		IYUYV2RGB_8(pyuv, prgb, 0, 0);

		prgb += PIXEL8_RGB;
		pyuv += PIXEL8_YUYV;
	}
#endif
	return UVC_SUCCESS;
}

/** @brief Convert a frame from YUYV to RGB565
 * @ingroup frame
 * @param ini YUYV frame
 * @param out RGB565 frame
 */
uvc_error_t uvc_yuyv2rgb565(uvc_frame_t *in, uvc_frame_t *out) {
	if (UNLIKELY(in->frame_format != UVC_FRAME_FORMAT_YUYV))
		return UVC_ERROR_INVALID_PARAM;

	if (UNLIKELY(uvc_ensure_frame_size(out, in->width * in->height * PIXEL_RGB565) < 0))
		return UVC_ERROR_NO_MEM;

	out->width = in->width;
	out->height = in->height;
	out->frame_format = UVC_FRAME_FORMAT_RGB565;
	if (out->library_owns_data)
		out->step = in->width * PIXEL_RGB565;
	out->sequence = in->sequence;
	out->capture_time = in->capture_time;
	out->source = in->source;

	uint8_t *pyuv = in->data;
	const uint8_t *pyuv_end = pyuv + in->data_bytes - PIXEL8_YUYV;
	uint8_t *prgb565 = out->data;
	const uint8_t *prgb565_end = prgb565 + out->data_bytes - PIXEL8_RGB565;

#if USE_STRIDE
	if (in->step && out->step && (in->step != out->step)) {
		const int hh = in->height < out->height ? in->height : out->height;
		const int ww = in->width < out->width ? in->width : out->width;
		int h, w;
		for (h = 0; h < hh; h++) {
			w = 0;
			pyuv = in->data + in->step * h;
			prgb565 = out->data + out->step * h;
			for (; (prgb565 <= prgb565_end) && (pyuv <= pyuv_end) && (w < ww) ;) {
				IYUYV2RGB565_8(pyuv, prgb565, 0, 0);

				prgb565 += PIXEL8_RGB565;
				pyuv += PIXEL8_YUYV;
				w += 8;
			}
		}
	} else {
		// compressed format? XXX if only one of the frame in / out has step, this may lead to crash...
		for (; (prgb565 <= prgb565_end) && (pyuv <= pyuv_end) ;) {
			IYUYV2RGB565_8(pyuv, prgb565, 0, 0);

			prgb565 += PIXEL8_RGB565;
			pyuv += PIXEL8_YUYV;
		}
	}
#else
	// YUYV => RGB565
	for (; (prgb565 <= prgb565_end) && (pyuv <= pyuv_end) ;) {
		IYUYV2RGB565_8(pyuv, prgb565, 0, 0);

		prgb565 += PIXEL8_RGB565;
		pyuv += PIXEL8_YUYV;
	}
#endif
	return UVC_SUCCESS;
}

#define IYUYV2RGBX_2(pyuv, prgbx, ax, bx) { \
		const int d1 = (pyuv)[ax+1]; \
		const int d3 = (pyuv)[ax+3]; \
		const int r = (22987 * (d3/*(pyuv)[ax+3]*/ - 128)) >> 14; \
		const int g = (-5636 * (d1/*(pyuv)[ax+1]*/ - 128) - 11698 * (d3/*(pyuv)[ax+3]*/ - 128)) >> 14; \
		const int b = (29049 * (d1/*(pyuv)[ax+1]*/ - 128)) >> 14; \
		const int y0 = (pyuv)[ax+0]; \
		(prgbx)[bx+0] = sat(y0 + r); \
		(prgbx)[bx+1] = sat(y0 + g); \
		(prgbx)[bx+2] = sat(y0 + b); \
		(prgbx)[bx+3] = 0xff; \
		const int y2 = (pyuv)[ax+2]; \
		(prgbx)[bx+4] = sat(y2 + r); \
		(prgbx)[bx+5] = sat(y2 + g); \
		(prgbx)[bx+6] = sat(y2 + b); \
		(prgbx)[bx+7] = 0xff; \
    }
#define IYUYV2RGBX_16(pyuv, prgbx, ax, bx) \
	IYUYV2RGBX_8(pyuv, prgbx, ax, bx) \
	IYUYV2RGBX_8(pyuv, prgbx, ax + PIXEL8_YUYV, bx + PIXEL8_RGBX);
#define IYUYV2RGBX_8(pyuv, prgbx, ax, bx) \
	IYUYV2RGBX_4(pyuv, prgbx, ax, bx) \
	IYUYV2RGBX_4(pyuv, prgbx, ax + PIXEL4_YUYV, bx + PIXEL4_RGBX);
#define IYUYV2RGBX_4(pyuv, prgbx, ax, bx) \
	IYUYV2RGBX_2(pyuv, prgbx, ax, bx) \
	IYUYV2RGBX_2(pyuv, prgbx, ax + PIXEL2_YUYV, bx + PIXEL2_RGBX);

#if LIBUVC_HAS_ARM_NEON
static inline int16x8_t yuv_mul_shift_s16(int16x8_t value, int coeff) {
	const int32x4_t lo = vmull_n_s16(vget_low_s16(value), coeff);
	const int32x4_t hi = vmull_n_s16(vget_high_s16(value), coeff);
	return vcombine_s16(vshrn_n_s32(lo, 14), vshrn_n_s32(hi, 14));
}

static inline uint8x8_t yuv_add_sat_u8(uint8x8_t y, int16x8_t addend) {
	return vqmovun_s16(vaddq_s16(vreinterpretq_s16_u16(vmovl_u8(y)), addend));
}

static inline void yuv422_to_rgbx_neon_8pairs(uint8_t *dst, uint8x8_t y0,
		uint8x8_t chroma_u, uint8x8_t y1, uint8x8_t chroma_v) {
	const int16x8_t u = vreinterpretq_s16_u16(vsubl_u8(chroma_u, vdup_n_u8(128)));
	const int16x8_t v = vreinterpretq_s16_u16(vsubl_u8(chroma_v, vdup_n_u8(128)));
	const int16x8_t r_add = yuv_mul_shift_s16(v, 22987);
	const int16x8_t b_add = yuv_mul_shift_s16(u, 29049);
	const int32x4_t g_lo = vaddq_s32(
		vmull_n_s16(vget_low_s16(u), -5636),
		vmull_n_s16(vget_low_s16(v), -11698));
	const int32x4_t g_hi = vaddq_s32(
		vmull_n_s16(vget_high_s16(u), -5636),
		vmull_n_s16(vget_high_s16(v), -11698));
	const int16x8_t g_add = vcombine_s16(vshrn_n_s32(g_lo, 14), vshrn_n_s32(g_hi, 14));

	const uint8x8_t r0 = yuv_add_sat_u8(y0, r_add);
	const uint8x8_t g0 = yuv_add_sat_u8(y0, g_add);
	const uint8x8_t b0 = yuv_add_sat_u8(y0, b_add);
	const uint8x8_t r1 = yuv_add_sat_u8(y1, r_add);
	const uint8x8_t g1 = yuv_add_sat_u8(y1, g_add);
	const uint8x8_t b1 = yuv_add_sat_u8(y1, b_add);

	uint8x8x2_t rz = vzip_u8(r0, r1);
	uint8x8x2_t gz = vzip_u8(g0, g1);
	uint8x8x2_t bz = vzip_u8(b0, b1);
	uint8x16x4_t rgba;
	rgba.val[0] = vcombine_u8(rz.val[0], rz.val[1]);
	rgba.val[1] = vcombine_u8(gz.val[0], gz.val[1]);
	rgba.val[2] = vcombine_u8(bz.val[0], bz.val[1]);
	rgba.val[3] = vdupq_n_u8(0xff);
	vst4q_u8(dst, rgba);
}

static void yuyv2rgbx_neon_line(const uint8_t *src, uint8_t *dst, int width) {
	int x = 0;
	for (; x + 16 <= width; x += 16) {
		const uint8x8x4_t yuyv = vld4_u8(src);
		yuv422_to_rgbx_neon_8pairs(dst, yuyv.val[0], yuyv.val[1], yuyv.val[2], yuyv.val[3]);
		src += 32;
		dst += 64;
	}
	for (; x + 2 <= width; x += 2) {
		IYUYV2RGBX_2(src, dst, 0, 0);
		src += PIXEL2_YUYV;
		dst += PIXEL2_RGBX;
	}
}

static void nv12_to_rgbx_neon_two_rows(const uint8_t *y0, const uint8_t *y1,
		const uint8_t *uv, uint8_t *dst0, uint8_t *dst1, int width) {
	int x = 0;
	for (; x + 16 <= width; x += 16) {
		const uint8x8x2_t y0_pairs = vld2_u8(y0);
		const uint8x8x2_t y1_pairs = vld2_u8(y1);
		const uint8x8x2_t uv_pairs = vld2_u8(uv);

		yuv422_to_rgbx_neon_8pairs(dst0, y0_pairs.val[0], uv_pairs.val[0],
			y0_pairs.val[1], uv_pairs.val[1]);
		yuv422_to_rgbx_neon_8pairs(dst1, y1_pairs.val[0], uv_pairs.val[0],
			y1_pairs.val[1], uv_pairs.val[1]);

		y0 += 16;
		y1 += 16;
		uv += 16;
		dst0 += 64;
		dst1 += 64;
	}
	for (; x + 2 <= width; x += 2) {
		const int u = uv[0] - 128;
		const int v = uv[1] - 128;
		const int r = (22987 * v) >> 14;
		const int g = (-5636 * u - 11698 * v) >> 14;
		const int b = (29049 * u) >> 14;

		dst0[0] = sat(y0[0] + r);
		dst0[1] = sat(y0[0] + g);
		dst0[2] = sat(y0[0] + b);
		dst0[3] = 0xff;
		dst0[4] = sat(y0[1] + r);
		dst0[5] = sat(y0[1] + g);
		dst0[6] = sat(y0[1] + b);
		dst0[7] = 0xff;

		dst1[0] = sat(y1[0] + r);
		dst1[1] = sat(y1[0] + g);
		dst1[2] = sat(y1[0] + b);
		dst1[3] = 0xff;
		dst1[4] = sat(y1[1] + r);
		dst1[5] = sat(y1[1] + g);
		dst1[6] = sat(y1[1] + b);
		dst1[7] = 0xff;

		y0 += 2;
		y1 += 2;
		uv += 2;
		dst0 += 8;
		dst1 += 8;
	}
}

static void nv12_to_rgbx_neon(const uint8_t *y_plane, const uint8_t *uv_plane,
		uint8_t *rgba, size_t width, size_t height, size_t out_step) {
	for (size_t y = 0; y + 1 < height; y += 2) {
		const uint8_t *y0 = y_plane + y * width;
		const uint8_t *y1 = y0 + width;
		const uint8_t *uv = uv_plane + (y / 2) * width;
		uint8_t *dst0 = rgba + y * out_step;
		uint8_t *dst1 = dst0 + out_step;
		nv12_to_rgbx_neon_two_rows(y0, y1, uv, dst0, dst1, (int) width);
	}
}

static void yu12_to_rgbx_neon_two_rows(const uint8_t *y0, const uint8_t *y1,
		const uint8_t *u, const uint8_t *v, uint8_t *dst0, uint8_t *dst1,
		int width) {
	int x = 0;
	for (; x + 16 <= width; x += 16) {
		const uint8x8x2_t y0_pairs = vld2_u8(y0);
		const uint8x8x2_t y1_pairs = vld2_u8(y1);
		const uint8x8_t u8 = vld1_u8(u);
		const uint8x8_t v8 = vld1_u8(v);

		yuv422_to_rgbx_neon_8pairs(dst0, y0_pairs.val[0], u8,
			y0_pairs.val[1], v8);
		yuv422_to_rgbx_neon_8pairs(dst1, y1_pairs.val[0], u8,
			y1_pairs.val[1], v8);

		y0 += 16;
		y1 += 16;
		u += 8;
		v += 8;
		dst0 += 64;
		dst1 += 64;
	}
	for (; x + 2 <= width; x += 2) {
		const int u0 = u[0] - 128;
		const int v0 = v[0] - 128;
		const int r = (22987 * v0) >> 14;
		const int g = (-5636 * u0 - 11698 * v0) >> 14;
		const int b = (29049 * u0) >> 14;

		dst0[0] = sat(y0[0] + r);
		dst0[1] = sat(y0[0] + g);
		dst0[2] = sat(y0[0] + b);
		dst0[3] = 0xff;
		dst0[4] = sat(y0[1] + r);
		dst0[5] = sat(y0[1] + g);
		dst0[6] = sat(y0[1] + b);
		dst0[7] = 0xff;

		dst1[0] = sat(y1[0] + r);
		dst1[1] = sat(y1[0] + g);
		dst1[2] = sat(y1[0] + b);
		dst1[3] = 0xff;
		dst1[4] = sat(y1[1] + r);
		dst1[5] = sat(y1[1] + g);
		dst1[6] = sat(y1[1] + b);
		dst1[7] = 0xff;

		y0 += 2;
		y1 += 2;
		u += 1;
		v += 1;
		dst0 += 8;
		dst1 += 8;
	}
}

static void yu12_to_rgbx_neon(const uint8_t *y_plane, const uint8_t *u_plane,
		const uint8_t *v_plane, uint8_t *rgba, size_t width, size_t height,
		size_t chroma_width, size_t out_step) {
	for (size_t y = 0; y + 1 < height; y += 2) {
		const uint8_t *y0 = y_plane + y * width;
		const uint8_t *y1 = y0 + width;
		const uint8_t *u = u_plane + (y / 2) * chroma_width;
		const uint8_t *v = v_plane + (y / 2) * chroma_width;
		uint8_t *dst0 = rgba + y * out_step;
		uint8_t *dst1 = dst0 + out_step;
		yu12_to_rgbx_neon_two_rows(y0, y1, u, v, dst0, dst1, (int) width);
	}
}

#endif

static uvc_error_t internal_yuyv2rgbx(uvc_frame_t *in, uvc_frame_t *out) {
	if (UNLIKELY(in->frame_format != UVC_FRAME_FORMAT_YUYV))
		return UVC_ERROR_INVALID_PARAM;

	if (UNLIKELY(uvc_ensure_frame_size(out, in->width * in->height * PIXEL_RGBX) < 0))
		return UVC_ERROR_NO_MEM;

	out->width = in->width;
	out->height = in->height;
	out->frame_format = UVC_FRAME_FORMAT_RGBX;
	if (out->library_owns_data)
		out->step = in->width * PIXEL_RGBX;
	out->sequence = in->sequence;
	out->capture_time = in->capture_time;
	out->source = in->source;

	uint8_t *pyuv = in->data;
	const uint8_t *pyuv_end = pyuv + in->data_bytes - PIXEL8_YUYV;
	uint8_t *prgbx = out->data;
	const uint8_t *prgbx_end = prgbx + out->data_bytes - PIXEL8_RGBX;

#if LIBUVC_HAS_ARM_NEON
	if (LIKELY(in->width >= 16 && !(in->width & 1))) {
		if (in->step && out->step && (in->step != out->step)) {
			const int hh = in->height < out->height ? in->height : out->height;
			const int ww = (in->width < out->width ? in->width : out->width) & ~1;
			int h;
			for (h = 0; h < hh; h++) {
				yuyv2rgbx_neon_line(in->data + in->step * h,
					out->data + out->step * h, ww);
			}
		} else {
			yuyv2rgbx_neon_line(in->data, out->data, in->width * in->height);
		}
		return UVC_SUCCESS;
	}
#endif
#if USE_STRIDE
	if (in->step && out->step && (in->step != out->step)) {
		const int hh = in->height < out->height ? in->height : out->height;
		const int ww = in->width < out->width ? in->width : out->width;
		int h, w;
		for (h = 0; h < hh; h++) {
			w = 0;
			pyuv = in->data + in->step * h;
			prgbx = out->data + out->step * h;
			for (; (prgbx <= prgbx_end) && (pyuv <= pyuv_end) && (w < ww) ;) {
				IYUYV2RGBX_8(pyuv, prgbx, 0, 0);

				prgbx += PIXEL8_RGBX;
				pyuv += PIXEL8_YUYV;
				w += 8;
			}
		}
	} else {
		for (; (prgbx <= prgbx_end) && (pyuv <= pyuv_end) ;) {
			IYUYV2RGBX_8(pyuv, prgbx, 0, 0);

			prgbx += PIXEL8_RGBX;
			pyuv += PIXEL8_YUYV;
		}
	}
#else
	for (; (prgbx <= prgbx_end) && (pyuv <= pyuv_end) ;) {
		IYUYV2RGBX_8(pyuv, prgbx, 0, 0);

		prgbx += PIXEL8_RGBX;
		pyuv += PIXEL8_YUYV;
	}
#endif
	return UVC_SUCCESS;
}

static uvc_error_t libyuv_yuyv2rgbx(uvc_frame_t *in, uvc_frame_t *out) {
	if (UNLIKELY(in->frame_format != UVC_FRAME_FORMAT_YUYV))
		return UVC_ERROR_INVALID_PARAM;

	if (UNLIKELY(uvc_ensure_frame_size(out, in->width * in->height * PIXEL_RGBX) < 0))
		return UVC_ERROR_NO_MEM;

	out->width = in->width;
	out->height = in->height;
	out->frame_format = UVC_FRAME_FORMAT_RGBX;
	if (out->library_owns_data)
		out->step = in->width * PIXEL_RGBX;
	out->sequence = in->sequence;
	out->capture_time = in->capture_time;
	out->source = in->source;

	const int src_stride_yuy2 = in->step ? (int) in->step : (int) in->width * PIXEL_YUYV;
	const int dst_stride_rgbx = out->step ? (int) out->step : (int) in->width * PIXEL_RGBX;
	const int result = YUY2ToARGBMatrix(in->data, src_stride_yuy2, out->data,
		dst_stride_rgbx, &kYvuI601Constants, (int) in->width, (int) in->height);
	return libyuv_result_to_uvc_error(result);
}

static uvc_error_t internal_nv122rgbx(uvc_frame_t *in, uvc_frame_t *out) {
	if (UNLIKELY(in->frame_format != UVC_FRAME_FORMAT_NV12))
		return UVC_ERROR_INVALID_PARAM;

	const size_t width = in->width;
	const size_t height = in->height;
	const size_t min_bytes = (width * height * 3) / 2;
	if (UNLIKELY(in->actual_bytes < min_bytes && in->data_bytes < min_bytes))
		return UVC_ERROR_INVALID_PARAM;

	if (UNLIKELY(uvc_ensure_frame_size(out, width * height * PIXEL_RGBX) < 0))
		return UVC_ERROR_NO_MEM;

	out->width = in->width;
	out->height = in->height;
	out->frame_format = UVC_FRAME_FORMAT_RGBX;
	if (out->library_owns_data || out->step < width * PIXEL_RGBX)
		out->step = in->width * PIXEL_RGBX;
	out->sequence = in->sequence;
	out->capture_time = in->capture_time;
	out->source = in->source;

	const uint8_t * restrict y_plane = in->data;
	const uint8_t * restrict uv_plane = y_plane + width * height;
	uint8_t * restrict rgba = out->data;
	const size_t out_step = out->step;

#if LIBUVC_HAS_ARM_NEON
	if (LIKELY(width >= 16 && !(width & 1) && !(height & 1))) {
		nv12_to_rgbx_neon(y_plane, uv_plane, rgba, width, height, out_step);
		return UVC_SUCCESS;
	}
#endif

	for (size_t y = 0; y < height; ++y) {
		const uint8_t *y_row = y_plane + y * width;
		const uint8_t *uv_row = uv_plane + (y / 2) * width;
		uint8_t *out_row = rgba + y * out_step;
		for (size_t x = 0; x < width; x += 2) {
			const int u = uv_row[0] - 128;
			const int v = uv_row[1] - 128;
			const int r = (22987 * v) >> 14;
			const int g = (-5636 * u - 11698 * v) >> 14;
			const int b = (29049 * u) >> 14;

			const int yy0 = y_row[0];
			out_row[0] = sat(yy0 + r);
			out_row[1] = sat(yy0 + g);
			out_row[2] = sat(yy0 + b);
			out_row[3] = 0xff;

			const int yy1 = y_row[1];
			out_row[4] = sat(yy1 + r);
			out_row[5] = sat(yy1 + g);
			out_row[6] = sat(yy1 + b);
			out_row[7] = 0xff;

			y_row += 2;
			uv_row += 2;
			out_row += 8;
		}
	}

	return UVC_SUCCESS;
}

static uvc_error_t libyuv_nv122rgbx(uvc_frame_t *in, uvc_frame_t *out) {
	if (UNLIKELY(in->frame_format != UVC_FRAME_FORMAT_NV12))
		return UVC_ERROR_INVALID_PARAM;

	const size_t width = in->width;
	const size_t height = in->height;
	const size_t min_bytes = (width * height * 3) / 2;
	if (UNLIKELY(in->actual_bytes < min_bytes && in->data_bytes < min_bytes))
		return UVC_ERROR_INVALID_PARAM;

	if (UNLIKELY(uvc_ensure_frame_size(out, width * height * PIXEL_RGBX) < 0))
		return UVC_ERROR_NO_MEM;

	out->width = in->width;
	out->height = in->height;
	out->frame_format = UVC_FRAME_FORMAT_RGBX;
	if (out->library_owns_data || out->step < width * PIXEL_RGBX)
		out->step = in->width * PIXEL_RGBX;
	out->sequence = in->sequence;
	out->capture_time = in->capture_time;
	out->source = in->source;

	const uint8_t * restrict y_plane = in->data;
	const uint8_t * restrict uv_plane = y_plane + width * height;
	uint8_t * restrict rgba = out->data;
	const size_t out_step = out->step;
	const int result = NV12ToABGR(y_plane, (int) width, uv_plane, (int) width,
		rgba, (int) out_step, (int) width, (int) height);
	return libyuv_result_to_uvc_error(result);
}

static uvc_error_t internal_yu122rgbx(uvc_frame_t *in, uvc_frame_t *out) {
	if (UNLIKELY(in->frame_format != UVC_FRAME_FORMAT_YU12))
		return UVC_ERROR_INVALID_PARAM;

	const size_t width = in->width;
	const size_t height = in->height;
	const size_t chroma_width = width / 2;
	const size_t chroma_height = height / 2;
	const size_t min_bytes = (width * height * 3) / 2;
	if (UNLIKELY(in->actual_bytes < min_bytes && in->data_bytes < min_bytes))
		return UVC_ERROR_INVALID_PARAM;

	if (UNLIKELY(uvc_ensure_frame_size(out, width * height * PIXEL_RGBX) < 0))
		return UVC_ERROR_NO_MEM;

	out->width = in->width;
	out->height = in->height;
	out->frame_format = UVC_FRAME_FORMAT_RGBX;
	if (out->library_owns_data || out->step < width * PIXEL_RGBX)
		out->step = in->width * PIXEL_RGBX;
	out->sequence = in->sequence;
	out->capture_time = in->capture_time;
	out->source = in->source;

	const uint8_t * restrict y_plane = in->data;
	const uint8_t * restrict u_plane = y_plane + width * height;
	const uint8_t * restrict v_plane = u_plane + chroma_width * chroma_height;
	uint8_t * restrict rgba = out->data;
	const size_t out_step = out->step;

#if LIBUVC_HAS_ARM_NEON
	if (LIKELY(width >= 16 && !(width & 1) && !(height & 1))) {
		yu12_to_rgbx_neon(y_plane, u_plane, v_plane, rgba, width, height,
			chroma_width, out_step);
		return UVC_SUCCESS;
	}
#endif

	for (size_t y = 0; y < height; ++y) {
		const uint8_t *y_row = y_plane + y * width;
		const uint8_t *u_row = u_plane + (y / 2) * chroma_width;
		const uint8_t *v_row = v_plane + (y / 2) * chroma_width;
		uint8_t *out_row = rgba + y * out_step;
		for (size_t x = 0; x < width; x += 2) {
			const int u = u_row[x / 2] - 128;
			const int v = v_row[x / 2] - 128;
			const int r = (22987 * v) >> 14;
			const int g = (-5636 * u - 11698 * v) >> 14;
			const int b = (29049 * u) >> 14;

			const int yy0 = y_row[x];
			out_row[0] = sat(yy0 + r);
			out_row[1] = sat(yy0 + g);
			out_row[2] = sat(yy0 + b);
			out_row[3] = 0xff;

			const int yy1 = y_row[x + 1];
			out_row[4] = sat(yy1 + r);
			out_row[5] = sat(yy1 + g);
			out_row[6] = sat(yy1 + b);
			out_row[7] = 0xff;

			out_row += 8;
		}
	}

	return UVC_SUCCESS;
}

static uvc_error_t libyuv_yu122rgbx(uvc_frame_t *in, uvc_frame_t *out) {
	if (UNLIKELY(in->frame_format != UVC_FRAME_FORMAT_YU12))
		return UVC_ERROR_INVALID_PARAM;

	const size_t width = in->width;
	const size_t height = in->height;
	const size_t chroma_width = width / 2;
	const size_t chroma_height = height / 2;
	const size_t min_bytes = (width * height * 3) / 2;
	if (UNLIKELY(in->actual_bytes < min_bytes && in->data_bytes < min_bytes))
		return UVC_ERROR_INVALID_PARAM;

	if (UNLIKELY(uvc_ensure_frame_size(out, width * height * PIXEL_RGBX) < 0))
		return UVC_ERROR_NO_MEM;

	out->width = in->width;
	out->height = in->height;
	out->frame_format = UVC_FRAME_FORMAT_RGBX;
	if (out->library_owns_data || out->step < width * PIXEL_RGBX)
		out->step = in->width * PIXEL_RGBX;
	out->sequence = in->sequence;
	out->capture_time = in->capture_time;
	out->source = in->source;

	const uint8_t * restrict y_plane = in->data;
	const uint8_t * restrict u_plane = y_plane + width * height;
	const uint8_t * restrict v_plane = u_plane + chroma_width * chroma_height;
	uint8_t * restrict rgba = out->data;
	const size_t out_step = out->step;
	const int result = I420ToABGR(y_plane, (int) width, u_plane, (int) chroma_width,
		v_plane, (int) chroma_width, rgba, (int) out_step, (int) width, (int) height);
	return libyuv_result_to_uvc_error(result);
}

static uvc_error_t internal_bgr2rgbx(uvc_frame_t *in, uvc_frame_t *out) {
	if (UNLIKELY(in->frame_format != UVC_FRAME_FORMAT_BGR))
		return UVC_ERROR_INVALID_PARAM;

	const size_t width = in->width;
	const size_t height = in->height;
	const size_t min_bytes = width * height * PIXEL_BGR;
	if (UNLIKELY(in->actual_bytes < min_bytes && in->data_bytes < min_bytes))
		return UVC_ERROR_INVALID_PARAM;

	if (UNLIKELY(uvc_ensure_frame_size(out, width * height * PIXEL_RGBX) < 0))
		return UVC_ERROR_NO_MEM;

	out->width = in->width;
	out->height = in->height;
	out->frame_format = UVC_FRAME_FORMAT_RGBX;
	if (out->library_owns_data || out->step < width * PIXEL_RGBX)
		out->step = in->width * PIXEL_RGBX;
	out->sequence = in->sequence;
	out->capture_time = in->capture_time;
	out->source = in->source;

	const uint8_t * restrict src = in->data;
	const size_t src_step = in->step ? in->step : width * PIXEL_BGR;
	uint8_t * restrict dst = out->data;
	const size_t dst_step = out->step;

	for (size_t y = 0; y < height; ++y) {
		const uint8_t *src_row = src + y * src_step;
		uint8_t *dst_row = dst + y * dst_step;
		for (size_t x = 0; x < width; ++x) {
			dst_row[0] = src_row[2];
			dst_row[1] = src_row[1];
			dst_row[2] = src_row[0];
			dst_row[3] = 0xff;
			src_row += PIXEL_BGR;
			dst_row += PIXEL_RGBX;
		}
	}

	return UVC_SUCCESS;
}

static uvc_error_t libyuv_bgr2rgbx(uvc_frame_t *in, uvc_frame_t *out) {
	if (UNLIKELY(in->frame_format != UVC_FRAME_FORMAT_BGR))
		return UVC_ERROR_INVALID_PARAM;

	const size_t width = in->width;
	const size_t height = in->height;
	const size_t min_bytes = width * height * PIXEL_BGR;
	if (UNLIKELY(in->actual_bytes < min_bytes && in->data_bytes < min_bytes))
		return UVC_ERROR_INVALID_PARAM;

	if (UNLIKELY(uvc_ensure_frame_size(out, width * height * PIXEL_RGBX) < 0))
		return UVC_ERROR_NO_MEM;

	out->width = in->width;
	out->height = in->height;
	out->frame_format = UVC_FRAME_FORMAT_RGBX;
	if (out->library_owns_data || out->step < width * PIXEL_RGBX)
		out->step = in->width * PIXEL_RGBX;
	out->sequence = in->sequence;
	out->capture_time = in->capture_time;
	out->source = in->source;

	const int src_stride = in->step ? (int) in->step : (int) width * PIXEL_BGR;
	const int dst_stride = out->step ? (int) out->step : (int) width * PIXEL_RGBX;
	const int result = RGB24ToARGB(in->data, src_stride, out->data, dst_stride,
		(int) width, (int) height);
	return libyuv_result_to_uvc_error(result);
}

static uvc_error_t internal_p0102rgbx(uvc_frame_t *in, uvc_frame_t *out) {
	if (UNLIKELY(in->frame_format != UVC_FRAME_FORMAT_P010))
		return UVC_ERROR_INVALID_PARAM;

	const size_t width = in->width;
	const size_t height = in->height;
	const size_t min_bytes = width * height * 3;
	if (UNLIKELY(in->actual_bytes < min_bytes && in->data_bytes < min_bytes))
		return UVC_ERROR_INVALID_PARAM;

	if (UNLIKELY(uvc_ensure_frame_size(out, width * height * PIXEL_RGBX) < 0))
		return UVC_ERROR_NO_MEM;

	out->width = in->width;
	out->height = in->height;
	out->frame_format = UVC_FRAME_FORMAT_RGBX;
	if (out->library_owns_data)
		out->step = in->width * PIXEL_RGBX;
	out->sequence = in->sequence;
	out->capture_time = in->capture_time;
	out->source = in->source;

	const uint8_t * restrict y_plane = in->data;
	const uint8_t * restrict uv_plane = y_plane + width * height * 2;
	uint8_t * restrict rgba = out->data;
	const size_t out_step = out->step;

	for (size_t y = 0; y < height; ++y) {
		const uint8_t *y_row = y_plane + y * width * 2;
		const uint8_t *uv_row = uv_plane + (y / 2) * width * 2;
		uint8_t *out_row = rgba + y * out_step;
		for (size_t x = 0; x < width; x += 2) {
			const int u = (((int)uv_row[0] | ((int)uv_row[1] << 8)) >> 8) - 128;
			const int v = (((int)uv_row[2] | ((int)uv_row[3] << 8)) >> 8) - 128;
			const int r = (22987 * v) >> 14;
			const int g = (-5636 * u - 11698 * v) >> 14;
			const int b = (29049 * u) >> 14;

			const int yy0 = ((int)y_row[0] | ((int)y_row[1] << 8)) >> 8;
			out_row[0] = sat(yy0 + r);
			out_row[1] = sat(yy0 + g);
			out_row[2] = sat(yy0 + b);
			out_row[3] = 0xff;

			const int yy1 = ((int)y_row[2] | ((int)y_row[3] << 8)) >> 8;
			out_row[4] = sat(yy1 + r);
			out_row[5] = sat(yy1 + g);
			out_row[6] = sat(yy1 + b);
			out_row[7] = 0xff;

			y_row += 4;
			uv_row += 4;
			out_row += 8;
		}
	}

	return UVC_SUCCESS;
}

static uvc_error_t libyuv_p0102rgbx(uvc_frame_t *in, uvc_frame_t *out) {
	if (UNLIKELY(in->frame_format != UVC_FRAME_FORMAT_P010))
		return UVC_ERROR_INVALID_PARAM;

	const size_t width = in->width;
	const size_t height = in->height;
	const size_t min_bytes = width * height * 3;
	if (UNLIKELY(in->actual_bytes < min_bytes && in->data_bytes < min_bytes))
		return UVC_ERROR_INVALID_PARAM;

	if (UNLIKELY(uvc_ensure_frame_size(out, width * height * PIXEL_RGBX) < 0))
		return UVC_ERROR_NO_MEM;

	out->width = in->width;
	out->height = in->height;
	out->frame_format = UVC_FRAME_FORMAT_RGBX;
	if (out->library_owns_data)
		out->step = in->width * PIXEL_RGBX;
	out->sequence = in->sequence;
	out->capture_time = in->capture_time;
	out->source = in->source;

	const uint8_t * restrict y_plane = in->data;
	const uint8_t * restrict uv_plane = y_plane + width * height * 2;
	uint8_t * restrict rgba = out->data;
	const size_t out_step = out->step;
	const int result = P010ToARGBMatrix((const uint16_t *) y_plane, (int) width,
		(const uint16_t *) uv_plane, (int) width, rgba, (int) out_step,
		&kYvuI601Constants, (int) width, (int) height);
	return libyuv_result_to_uvc_error(result);
}

/** @brief Convert a frame from YUYV to RGBX8888
 * @ingroup frame
 * @param ini YUYV frame
 * @param out RGBX8888 frame
 */
uvc_error_t uvc_yuyv2rgbx(uvc_frame_t *in, uvc_frame_t *out) {
	return s_yuyv2rgbx_func(in, out);
}

/** @brief Convert a frame from NV12 to RGBX8888 */
uvc_error_t uvc_nv122rgbx(uvc_frame_t *in, uvc_frame_t *out) {
	return s_nv122rgbx_func(in, out);
}

/** @brief Convert a frame from YU12 (I420) to RGBX8888 */
uvc_error_t uvc_yu122rgbx(uvc_frame_t *in, uvc_frame_t *out) {
	return s_yu122rgbx_func(in, out);
}

/** @brief Convert a frame from BGR888 to RGBX8888 */
uvc_error_t uvc_bgr2rgbx(uvc_frame_t *in, uvc_frame_t *out) {
	return s_bgr2rgbx_func(in, out);
}

/** @brief Convert a frame from P010 to RGBX8888 */
uvc_error_t uvc_p0102rgbx(uvc_frame_t *in, uvc_frame_t *out) {
	return s_p0102rgbx_func(in, out);
}

#define IYUYV2BGR_2(pyuv, pbgr, ax, bx) { \
		const int d1 = (pyuv)[ax+1]; \
		const int d3 = (pyuv)[ax+3]; \
	    const int r = (22987 * (d3/*(pyuv)[3]*/ - 128)) >> 14; \
	    const int g = (-5636 * (d1/*(pyuv)[1]*/ - 128) - 11698 * (d3/*(pyuv)[3]*/ - 128)) >> 14; \
	    const int b = (29049 * (d1/*(pyuv)[1]*/ - 128)) >> 14; \
		const int y0 = (pyuv)[ax+0]; \
		(pbgr)[bx+0] = sat(y0 + b); \
		(pbgr)[bx+1] = sat(y0 + g); \
		(pbgr)[bx+2] = sat(y0 + r); \
		const int y2 = (pyuv)[ax+2]; \
		(pbgr)[bx+3] = sat(y2 + b); \
		(pbgr)[bx+4] = sat(y2 + g); \
		(pbgr)[bx+5] = sat(y2 + r); \
    }
#define IYUYV2BGR_16(pyuv, pbgr, ax, bx) \
	IYUYV2BGR_8(pyuv, pbgr, ax, bx) \
	IYUYV2BGR_8(pyuv, pbgr, ax + PIXEL8_YUYV, bx + PIXEL8_BGR)
#define IYUYV2BGR_8(pyuv, pbgr, ax, bx) \
	IYUYV2BGR_4(pyuv, pbgr, ax, bx) \
	IYUYV2BGR_4(pyuv, pbgr, ax + PIXEL4_YUYV, bx + PIXEL4_BGR)
#define IYUYV2BGR_4(pyuv, pbgr, ax, bx) \
	IYUYV2BGR_2(pyuv, pbgr, ax, bx) \
	IYUYV2BGR_2(pyuv, pbgr, ax + PIXEL2_YUYV, bx + PIXEL2_BGR)

/** @brief Convert a frame from YUYV to BGR888
 * @ingroup frame
 *
 * @param in YUYV frame
 * @param out BGR888 frame
 */
uvc_error_t uvc_yuyv2bgr(uvc_frame_t *in, uvc_frame_t *out) {
	if (UNLIKELY(in->frame_format != UVC_FRAME_FORMAT_YUYV))
		return UVC_ERROR_INVALID_PARAM;

	if (UNLIKELY(uvc_ensure_frame_size(out, in->width * in->height * PIXEL_BGR) < 0))
		return UVC_ERROR_NO_MEM;

	out->width = in->width;
	out->height = in->height;
	out->frame_format = UVC_FRAME_FORMAT_BGR;
	if (out->library_owns_data)
		out->step = in->width * PIXEL_BGR;
	out->sequence = in->sequence;
	out->capture_time = in->capture_time;
	out->source = in->source;

	uint8_t *pyuv = in->data;
	uint8_t *pyuv_end = pyuv + in->data_bytes - PIXEL8_YUYV;
	uint8_t *pbgr = out->data;
	uint8_t *pbgr_end = pbgr + out->data_bytes - PIXEL8_BGR;

	// YUYV => BGR888
#if USE_STRIDE
	if (in->step && out->step && (in->step != out->step)) {
		const int hh = in->height < out->height ? in->height : out->height;
		const int ww = in->width < out->width ? in->width : out->width;
		int h, w;
		for (h = 0; h < hh; h++) {
			w = 0;
			pyuv = in->data + in->step * h;
			pbgr = out->data + out->step * h;
			for (; (pbgr <= pbgr_end) && (pyuv <= pyuv_end) && (w < ww) ;) {
				IYUYV2BGR_8(pyuv, pbgr, 0, 0);

				pbgr += PIXEL8_BGR;
				pyuv += PIXEL8_YUYV;
				w += 8;
			}
		}
	} else {
		// compressed format? XXX if only one of the frame in / out has step, this may lead to crash...
		for (; (pbgr <= pbgr_end) && (pyuv <= pyuv_end) ;) {
			IYUYV2BGR_8(pyuv, pbgr, 0, 0);

			pbgr += PIXEL8_BGR;
			pyuv += PIXEL8_YUYV;
		}
	}
#else
	for (; (pbgr <= pbgr_end) && (pyuv <= pyuv_end) ;) {
		IYUYV2BGR_8(pyuv, pbgr, 0, 0);

		pbgr += PIXEL8_BGR;
		pyuv += PIXEL8_YUYV;
	}
#endif
	return UVC_SUCCESS;
}

#define IUYVY2RGB_2(pyuv, prgb, ax, bx) { \
		const int d0 = (pyuv)[ax+0]; \
		const int d2 = (pyuv)[ax+2]; \
	    const int r = (22987 * (d2/*(pyuv)[ax+2]*/ - 128)) >> 14; \
	    const int g = (-5636 * (d0/*(pyuv)[ax+0]*/ - 128) - 11698 * (d2/*(pyuv)[ax+2]*/ - 128)) >> 14; \
	    const int b = (29049 * (d0/*(pyuv)[ax+0]*/ - 128)) >> 14; \
		const int y1 = (pyuv)[ax+1]; \
		(prgb)[bx+0] = sat(y1 + r); \
		(prgb)[bx+1] = sat(y1 + g); \
		(prgb)[bx+2] = sat(y1 + b); \
		const int y3 = (pyuv)[ax+3]; \
		(prgb)[bx+3] = sat(y3 + r); \
		(prgb)[bx+4] = sat(y3 + g); \
		(prgb)[bx+5] = sat(y3 + b); \
    }
#define IUYVY2RGB_16(pyuv, prgb, ax, bx) \
	IUYVY2RGB_8(pyuv, prgb, ax, bx) \
	IUYVY2RGB_8(pyuv, prgb, ax + 16, bx + 24)
#define IUYVY2RGB_8(pyuv, prgb, ax, bx) \
	IUYVY2RGB_4(pyuv, prgb, ax, bx) \
	IUYVY2RGB_4(pyuv, prgb, ax + 8, bx + 12)
#define IUYVY2RGB_4(pyuv, prgb, ax, bx) \
	IUYVY2RGB_2(pyuv, prgb, ax, bx) \
	IUYVY2RGB_2(pyuv, prgb, ax + 4, bx + 6)

#define IUYVY2RGB565_2(pyuv, prgb565, ax, bx) { \
		const int d0 = (pyuv)[ax+0]; \
		const int d2 = (pyuv)[ax+2]; \
		const int r = (22987 * (d2 - 128)) >> 14; \
		const int g = (-5636 * (d0 - 128) - 11698 * (d2 - 128)) >> 14; \
		const int b = (29049 * (d0 - 128)) >> 14; \
		const int y1 = (pyuv)[ax+1]; \
		store_rgb565((prgb565) + bx, y1 + r, y1 + g, y1 + b); \
		const int y3 = (pyuv)[ax+3]; \
		store_rgb565((prgb565) + bx + PIXEL_RGB565, y3 + r, y3 + g, y3 + b); \
    }
#define IUYVY2RGB565_16(pyuv, prgb565, ax, bx) \
	IUYVY2RGB565_8(pyuv, prgb565, ax, bx) \
	IUYVY2RGB565_8(pyuv, prgb565, ax + PIXEL8_UYVY, bx + PIXEL8_RGB565)
#define IUYVY2RGB565_8(pyuv, prgb565, ax, bx) \
	IUYVY2RGB565_4(pyuv, prgb565, ax, bx) \
	IUYVY2RGB565_4(pyuv, prgb565, ax + PIXEL4_UYVY, bx + PIXEL4_RGB565)
#define IUYVY2RGB565_4(pyuv, prgb565, ax, bx) \
	IUYVY2RGB565_2(pyuv, prgb565, ax, bx) \
	IUYVY2RGB565_2(pyuv, prgb565, ax + PIXEL2_UYVY, bx + PIXEL2_RGB565)

/** @brief Convert a frame from UYVY to RGB888
 * @ingroup frame
 * @param ini UYVY frame
 * @param out RGB888 frame
 */
uvc_error_t uvc_uyvy2rgb(uvc_frame_t *in, uvc_frame_t *out) {
	if (UNLIKELY(in->frame_format != UVC_FRAME_FORMAT_UYVY))
		return UVC_ERROR_INVALID_PARAM;

	if (UNLIKELY(uvc_ensure_frame_size(out, in->width * in->height * PIXEL_RGB) < 0))
		return UVC_ERROR_NO_MEM;

	out->width = in->width;
	out->height = in->height;
	out->frame_format = UVC_FRAME_FORMAT_RGB;
	if (out->library_owns_data)
		out->step = in->width * PIXEL_RGB;
	out->sequence = in->sequence;
	out->capture_time = in->capture_time;
	out->source = in->source;

	uint8_t *pyuv = in->data;
	const uint8_t *pyuv_end = pyuv + in->data_bytes - PIXEL8_UYVY;
	uint8_t *prgb = out->data;
	const uint8_t *prgb_end = prgb + out->data_bytes - PIXEL8_RGB;

	// UYVY => RGB888
#if USE_STRIDE
	if (in->step && out->step && (in->step != out->step)) {
		const int hh = in->height < out->height ? in->height : out->height;
		const int ww = in->width < out->width ? in->width : out->width;
		int h, w;
		for (h = 0; h < hh; h++) {
			w = 0;
			pyuv = in->data + in->step * h;
			prgb = out->data + out->step * h;
			for (; (prgb <= prgb_end) && (pyuv <= pyuv_end) && (w < ww) ;) {
				IUYVY2RGB_8(pyuv, prgb, 0, 0);

				prgb += PIXEL8_RGB;
				pyuv += PIXEL8_UYVY;
				w += 8;
			}
		}
	} else {
		// compressed format? XXX if only one of the frame in / out has step, this may lead to crash...
		for (; (prgb <= prgb_end) && (pyuv <= pyuv_end) ;) {
			IUYVY2RGB_8(pyuv, prgb, 0, 0);

			prgb += PIXEL8_RGB;
			pyuv += PIXEL8_UYVY;
		}
	}
#else
	for (; ((prgb <= prgb_end) && (pyuv <= pyuv_end) ;) {
		IUYVY2RGB_8(pyuv, prgb, 0, 0);

		prgb += PIXEL8_RGB;
		pyuv += PIXEL8_UYVY;
	}
#endif
	return UVC_SUCCESS;
}

/** @brief Convert a frame from UYVY to RGB565
 * @ingroup frame
 * @param ini UYVY frame
 * @param out RGB565 frame
 */
uvc_error_t uvc_uyvy2rgb565(uvc_frame_t *in, uvc_frame_t *out) {
	if (UNLIKELY(in->frame_format != UVC_FRAME_FORMAT_UYVY))
		return UVC_ERROR_INVALID_PARAM;

	if (UNLIKELY(uvc_ensure_frame_size(out, in->width * in->height * PIXEL_RGB565) < 0))
		return UVC_ERROR_NO_MEM;

	out->width = in->width;
	out->height = in->height;
	out->frame_format = UVC_FRAME_FORMAT_RGB565;
	if (out->library_owns_data)
		out->step = in->width * PIXEL_RGB565;
	out->sequence = in->sequence;
	out->capture_time = in->capture_time;
	out->source = in->source;

	uint8_t *pyuv = in->data;
	const uint8_t *pyuv_end = pyuv + in->data_bytes - PIXEL8_UYVY;
	uint8_t *prgb565 = out->data;
	const uint8_t *prgb565_end = prgb565 + out->data_bytes - PIXEL8_RGB565;

	// UYVY => RGB565
#if USE_STRIDE
	if (in->step && out->step && (in->step != out->step)) {
		const int hh = in->height < out->height ? in->height : out->height;
		const int ww = in->width < out->width ? in->width : out->width;
		int h, w;
		for (h = 0; h < hh; h++) {
			w = 0;
			pyuv = in->data + in->step * h;
			prgb565 = out->data + out->step * h;
			for (; (prgb565 <= prgb565_end) && (pyuv <= pyuv_end) && (w < ww) ;) {
				IUYVY2RGB565_8(pyuv, prgb565, 0, 0);

				prgb565 += PIXEL8_RGB565;
				pyuv += PIXEL8_UYVY;
				w += 8;
			}
		}
	} else {
		// compressed format? XXX if only one of the frame in / out has step, this may lead to crash...
		for (; (prgb565 <= prgb565_end) && (pyuv <= pyuv_end) ;) {
			IUYVY2RGB565_8(pyuv, prgb565, 0, 0);

			prgb565 += PIXEL8_RGB565;
			pyuv += PIXEL8_UYVY;
		}
	}
#else
	for (; (prgb565 <= prgb565_end) && (pyuv <= pyuv_end) ;) {
		IUYVY2RGB565_8(pyuv, prgb565, 0, 0);

		prgb565 += PIXEL8_RGB565;
		pyuv += PIXEL8_UYVY;
	}
#endif
	return UVC_SUCCESS;
}

#define IUYVY2RGBX_2(pyuv, prgbx, ax, bx) { \
		const int d0 = (pyuv)[ax+0]; \
		const int d2 = (pyuv)[ax+2]; \
	    const int r = (22987 * (d2/*(pyuv)[ax+2]*/ - 128)) >> 14; \
	    const int g = (-5636 * (d0/*(pyuv)[ax+0]*/ - 128) - 11698 * (d2/*(pyuv)[ax+2]*/ - 128)) >> 14; \
	    const int b = (29049 * (d0/*(pyuv)[ax+0]*/ - 128)) >> 14; \
		const int y1 = (pyuv)[ax+1]; \
		(prgbx)[bx+0] = sat(y1 + r); \
		(prgbx)[bx+1] = sat(y1 + g); \
		(prgbx)[bx+2] = sat(y1 + b); \
		(prgbx)[bx+3] = 0xff; \
		const int y3 = (pyuv)[ax+3]; \
		(prgbx)[bx+4] = sat(y3 + r); \
		(prgbx)[bx+5] = sat(y3 + g); \
		(prgbx)[bx+6] = sat(y3 + b); \
		(prgbx)[bx+7] = 0xff; \
    }
#define IUYVY2RGBX_16(pyuv, prgbx, ax, bx) \
	IUYVY2RGBX_8(pyuv, prgbx, ax, bx) \
	IUYVY2RGBX_8(pyuv, prgbx, ax + PIXEL8_UYVY, bx + PIXEL8_RGBX)
#define IUYVY2RGBX_8(pyuv, prgbx, ax, bx) \
	IUYVY2RGBX_4(pyuv, prgbx, ax, bx) \
	IUYVY2RGBX_4(pyuv, prgbx, ax + PIXEL4_UYVY, bx + PIXEL4_RGBX)
#define IUYVY2RGBX_4(pyuv, prgbx, ax, bx) \
	IUYVY2RGBX_2(pyuv, prgbx, ax, bx) \
	IUYVY2RGBX_2(pyuv, prgbx, ax + PIXEL2_UYVY, bx + PIXEL2_RGBX)

#if LIBUVC_HAS_ARM_NEON
static void uyvy2rgbx_neon_line(const uint8_t *src, uint8_t *dst, int width) {
	int x = 0;
	for (; x + 16 <= width; x += 16) {
		const uint8x8x4_t uyvy = vld4_u8(src);
		yuv422_to_rgbx_neon_8pairs(dst, uyvy.val[1], uyvy.val[0], uyvy.val[3], uyvy.val[2]);
		src += 32;
		dst += 64;
	}
	for (; x + 2 <= width; x += 2) {
		IUYVY2RGBX_2(src, dst, 0, 0);
		src += PIXEL2_UYVY;
		dst += PIXEL2_RGBX;
	}
}
#endif

/** @brief Convert a frame from UYVY to RGBX8888
 * @ingroup frame
 * @param ini UYVY frame
 * @param out RGBX8888 frame
 */
uvc_error_t uvc_uyvy2rgbx(uvc_frame_t *in, uvc_frame_t *out) {
	if (UNLIKELY(in->frame_format != UVC_FRAME_FORMAT_UYVY))
		return UVC_ERROR_INVALID_PARAM;

	if (UNLIKELY(uvc_ensure_frame_size(out, in->width * in->height * PIXEL_RGBX) < 0))
		return UVC_ERROR_NO_MEM;

	out->width = in->width;
	out->height = in->height;
	out->frame_format = UVC_FRAME_FORMAT_RGBX;
	if (out->library_owns_data)
		out->step = in->width * PIXEL_RGBX;
	out->sequence = in->sequence;
	out->capture_time = in->capture_time;
	out->source = in->source;

	uint8_t *pyuv = in->data;
	const uint8_t *pyuv_end = pyuv + in->data_bytes - PIXEL8_UYVY;
	uint8_t *prgbx = out->data;
	const uint8_t *prgbx_end = prgbx + out->data_bytes - PIXEL8_RGBX;

	// UYVY => RGBX8888
#if LIBUVC_HAS_ARM_NEON
	if (LIKELY(in->width >= 16 && !(in->width & 1))) {
		if (in->step && out->step && (in->step != out->step)) {
			const int hh = in->height < out->height ? in->height : out->height;
			const int ww = (in->width < out->width ? in->width : out->width) & ~1;
			int h;
			for (h = 0; h < hh; h++) {
				uyvy2rgbx_neon_line(in->data + in->step * h,
					out->data + out->step * h, ww);
			}
		} else {
			uyvy2rgbx_neon_line(in->data, out->data, in->width * in->height);
		}
		return UVC_SUCCESS;
	}
#endif
#if USE_STRIDE
	if (in->step && out->step && (in->step != out->step)) {
		const int hh = in->height < out->height ? in->height : out->height;
		const int ww = in->width < out->width ? in->width : out->width;
		int h, w;
		for (h = 0; h < hh; h++) {
			w = 0;
			pyuv = in->data + in->step * h;
			prgbx = out->data + out->step * h;
			for (; (prgbx <= prgbx_end) && (pyuv <= pyuv_end) && (w < ww) ;) {
				IUYVY2RGBX_8(pyuv, prgbx, 0, 0);

				prgbx += PIXEL8_RGBX;
				pyuv += PIXEL8_UYVY;
				w += 8;
			}
		}
	} else {
		// compressed format? XXX if only one of the frame in / out has step, this may lead to crash...
		for (; (prgbx <= prgbx_end) && (pyuv <= pyuv_end) ;) {
			IUYVY2RGBX_8(pyuv, prgbx, 0, 0);

			prgbx += PIXEL8_RGBX;
			pyuv += PIXEL8_UYVY;
		}
	}
#else
	for (; (prgbx <= prgbx_end) && (pyuv <= pyuv_end) ;) {
		IUYVY2RGBX_8(pyuv, prgbx, 0, 0);

		prgbx += PIXEL8_RGBX;
		pyuv += PIXEL8_UYVY;
	}
#endif
	return UVC_SUCCESS;
}

#define IUYVY2BGR_2(pyuv, pbgr, ax, bx) { \
		const int d0 = (pyuv)[ax+0]; \
		const int d2 = (pyuv)[ax+2]; \
	    const int r = (22987 * (d2/*(pyuv)[ax+2]*/ - 128)) >> 14; \
	    const int g = (-5636 * (d0/*(pyuv)[ax+0]*/ - 128) - 11698 * (d2/*(pyuv)[ax+2]*/ - 128)) >> 14; \
	    const int b = (29049 * (d0/*(pyuv)[ax+0]*/ - 128)) >> 14; \
		const int y1 = (pyuv)[ax+1]; \
		(pbgr)[bx+0] = sat(y1 + b); \
		(pbgr)[bx+1] = sat(y1 + g); \
		(pbgr)[bx+2] = sat(y1 + r); \
		const int y3 = (pyuv)[ax+3]; \
		(pbgr)[bx+3] = sat(y3 + b); \
		(pbgr)[bx+4] = sat(y3 + g); \
		(pbgr)[bx+5] = sat(y3 + r); \
    }
#define IUYVY2BGR_16(pyuv, pbgr, ax, bx) \
	IUYVY2BGR_8(pyuv, pbgr, ax, bx) \
	IUYVY2BGR_8(pyuv, pbgr, ax + PIXEL8_UYVY, bx + PIXEL8_BGR)
#define IUYVY2BGR_8(pyuv, pbgr, ax, bx) \
	IUYVY2BGR_4(pyuv, pbgr, ax, bx) \
	IUYVY2BGR_4(pyuv, pbgr, ax + PIXEL4_UYVY, bx + PIXEL4_BGR)
#define IUYVY2BGR_4(pyuv, pbgr, ax, bx) \
	IUYVY2BGR_2(pyuv, pbgr, ax, bx) \
	IUYVY2BGR_2(pyuv, pbgr, ax + PIXEL2_UYVY, bx + PIXEL2_BGR)

/** @brief Convert a frame from UYVY to BGR888
 * @ingroup frame
 * @param ini UYVY frame
 * @param out BGR888 frame
 */
uvc_error_t uvc_uyvy2bgr(uvc_frame_t *in, uvc_frame_t *out) {
	if (UNLIKELY(in->frame_format != UVC_FRAME_FORMAT_UYVY))
		return UVC_ERROR_INVALID_PARAM;

	if (UNLIKELY(uvc_ensure_frame_size(out, in->width * in->height * PIXEL_BGR) < 0))
		return UVC_ERROR_NO_MEM;

	out->width = in->width;
	out->height = in->height;
	out->frame_format = UVC_FRAME_FORMAT_BGR;
	if (out->library_owns_data)
		out->step = in->width * PIXEL_BGR;
	out->sequence = in->sequence;
	out->capture_time = in->capture_time;
	out->source = in->source;

	uint8_t *pyuv = in->data;
	const uint8_t *pyuv_end = pyuv + in->data_bytes - PIXEL8_UYVY;
	uint8_t *pbgr = out->data;
	const uint8_t *pbgr_end = pbgr + out->data_bytes - PIXEL8_BGR;

	// UYVY => BGR888
#if USE_STRIDE
	if (in->step && out->step && (in->step != out->step)) {
		const int hh = in->height < out->height ? in->height : out->height;
		const int ww = in->width < out->width ? in->width : out->width;
		int h, w;
		for (h = 0; h < hh; h++) {
			w = 0;
			pyuv = in->data + in->step * h;
			pbgr = out->data + out->step * h;
			for (; (pbgr <= pbgr_end) && (pyuv <= pyuv_end) && (w < ww) ;) {
				IUYVY2BGR_8(pyuv, pbgr, 0, 0);

				pbgr += PIXEL8_BGR;
				pyuv += PIXEL8_UYVY;
				w += 8;
			}
		}
	} else {
		// compressed format? XXX if only one of the frame in / out has step, this may lead to crash...
		for (; (pbgr <= pbgr_end) && (pyuv <= pyuv_end) ;) {
			IUYVY2BGR_8(pyuv, pbgr, 0, 0);

			pbgr += PIXEL8_BGR;
			pyuv += PIXEL8_UYVY;
		}
	}
#else
	for (; (pbgr <= pbgr_end) && (pyuv <= pyuv_end) ;) {
		IUYVY2BGR_8(pyuv, pbgr, 0, 0);

		pbgr += PIXEL8_BGR;
		pyuv += PIXEL8_UYVY;
	}
#endif
	return UVC_SUCCESS;
}

int uvc_yuyv2yuv420P(uvc_frame_t *in, uvc_frame_t *out) {

	ENTER();
	
	if (UNLIKELY(in->frame_format != UVC_FRAME_FORMAT_YUYV))
		RETURN(UVC_ERROR_INVALID_PARAM, uvc_error_t);

	if (UNLIKELY(uvc_ensure_frame_size(out, (in->width * in->height * 3) / 2) < 0))
		RETURN(UVC_ERROR_NO_MEM, uvc_error_t);

	const uint8_t *src = in->data;
	uint8_t *dest = out->data;
	const int32_t width = in->width;
	const int32_t height = in->height;
	const int32_t src_width = in->step;
	const int32_t src_height = in->height;
	const int32_t dest_width = out->width = out->step = in->width;
	const int32_t dest_height = out->height = in->height;
	const uint32_t hh = src_height < dest_height ? src_height : dest_height;
	uint8_t *y = dest;
	uint8_t *v = dest + dest_width * dest_height;
	uint8_t *u = dest + dest_width * dest_height * 5 / 4;
	int h, w;
	for (h = 0; h < hh; h++) {
		const uint8_t *yuv = src + src_width * h;
		if ((h & 1) == 0) {
			for (w = 0; w < width; w += 4) {
				*(y++) = yuv[0];
				*(y++) = yuv[2];
				*(y++) = yuv[4];
				*(y++) = yuv[6];
				*(v++) = yuv[1];
				*(v++) = yuv[5];
				yuv += 8;
			}
		} else {
			for (w = 0; w < width; w += 4) {
				*(y++) = yuv[0];
				*(y++) = yuv[2];
				*(y++) = yuv[4];
				*(y++) = yuv[6];
				*(u++) = yuv[3];
				*(u++) = yuv[7];
				yuv += 8;
			}
		}
	}
	RETURN(0, int);
}

//--------------------------------------------------------------------------------
int uvc_yuyv2iyuv420P(uvc_frame_t *in, uvc_frame_t *out) {

	ENTER();
	
	if (UNLIKELY(in->frame_format != UVC_FRAME_FORMAT_YUYV))
		RETURN(UVC_ERROR_INVALID_PARAM, uvc_error_t);

	if (UNLIKELY(uvc_ensure_frame_size(out, (in->width * in->height * 3) / 2) < 0))
		RETURN(UVC_ERROR_NO_MEM, uvc_error_t);

	const uint8_t *src = in->data;
	uint8_t *dest = out->data;
	const int32_t width = in->width;
	const int32_t height = in->height;
	const int32_t src_width = in->step;
	const int32_t src_height = in->height;
	const int32_t dest_width = out->width = out->step = in->width;
	const int32_t dest_height = out->height = in->height;
	const uint32_t hh = src_height < dest_height ? src_height : dest_height;
	uint8_t *y = dest;
	uint8_t *u = dest + dest_width * dest_height;
	uint8_t *v = dest + dest_width * dest_height * 5 / 4;
	int h, w;
	for (h = 0; h < hh; h++) {
		const uint8_t *yuv = src + src_width * h;
		if ((h & 1) == 0) {
			for (w = 0; w < width; w += 4) {
				*(y++) = yuv[0];
				*(y++) = yuv[2];
				*(y++) = yuv[4];
				*(y++) = yuv[6];
				*(v++) = yuv[1];
				*(v++) = yuv[5];
				yuv += 8;
			}
		} else {
			for (w = 0; w < width; w += 4) {
				*(y++) = yuv[0];
				*(y++) = yuv[2];
				*(y++) = yuv[4];
				*(y++) = yuv[6];
				*(u++) = yuv[3];
				*(u++) = yuv[7];
				yuv += 8;
			}
		}
	}
	RETURN(0, int);
}

uvc_error_t uvc_yuyv2yuv420SP(uvc_frame_t *in, uvc_frame_t *out) {
	ENTER();
	
	if (UNLIKELY(in->frame_format != UVC_FRAME_FORMAT_YUYV))
		RETURN(UVC_ERROR_INVALID_PARAM, uvc_error_t);

	if (UNLIKELY(uvc_ensure_frame_size(out, (in->width * in->height * 3) / 2) < 0))
		RETURN(UVC_ERROR_NO_MEM, uvc_error_t);

	const uint8_t *src = in->data;
	uint8_t *dest = out->data;
	const int32_t width = in->width;
	const int32_t height = in->height;
	const int32_t src_width = in->step;
	const int32_t src_height = in->height;
	const int32_t dest_width = out->width = out->step = in->width;
	const int32_t dest_height = out->height = in->height;

	const uint32_t hh = src_height < dest_height ? src_height : dest_height;
	uint8_t *uv = dest + dest_width * dest_height;
	int h, w;
	for (h = 0; h < hh - 1; h += 2) {
		uint8_t *y0 = dest + width * h;
		uint8_t *y1 = y0 + width;
		const uint8_t *yuv = src + src_width * h;
		for (w = 0; w < width; w += 4) {
			*(y0++) = yuv[0];	// y
			*(y0++) = yuv[2];	// y'
			*(y0++) = yuv[4];	// y''
			*(y0++) = yuv[6];	// y'''
			*(uv++) = yuv[1];	// u
			*(uv++) = yuv[3];	// v
			*(uv++) = yuv[5];	// u
			*(uv++) = yuv[7];	// v
			*(y1++) = yuv[src_width+0];	// y on next low
			*(y1++) = yuv[src_width+2];	// y' on next low
			*(y1++) = yuv[src_width+4];	// y''  on next low
			*(y1++) = yuv[src_width+6];	// y'''  on next low
			yuv += 8;	// (1pixel=2bytes)x4pixels=8bytes
		}
	}
	
	RETURN(UVC_SUCCESS, uvc_error_t);
}

uvc_error_t uvc_yuyv2iyuv420SP(uvc_frame_t *in, uvc_frame_t *out) {
	ENTER();
	
	if (UNLIKELY(in->frame_format != UVC_FRAME_FORMAT_YUYV))
		RETURN(UVC_ERROR_INVALID_PARAM, uvc_error_t);

	if (UNLIKELY(uvc_ensure_frame_size(out, (in->width * in->height * 3) / 2) < 0))
		return UVC_ERROR_NO_MEM;

	const uint8_t *src = in->data;
	uint8_t *dest =out->data;
	const int32_t width = in->width;
	const int32_t height = in->height;
	const int32_t src_width = in->step;
	const int32_t src_height = in->height;
	const int32_t dest_width = out->width = out->step = in->width;
	const int32_t dest_height = out->height = in->height;

	const uint32_t hh = src_height < dest_height ? src_height : dest_height;
	uint8_t *uv = dest + dest_width * dest_height;
	int h, w;
	for (h = 0; h < hh - 1; h += 2) {
		uint8_t *y0 = dest + width * h;
		uint8_t *y1 = y0 + width;
		const uint8_t *yuv = src + src_width * h;
		for (w = 0; w < width; w += 4) {
			*(y0++) = yuv[0];	// y
			*(y0++) = yuv[2];	// y'
			*(y0++) = yuv[4];	// y''
			*(y0++) = yuv[6];	// y'''
			*(uv++) = yuv[3];	// v
			*(uv++) = yuv[1];	// u
			*(uv++) = yuv[7];	// v
			*(uv++) = yuv[5];	// u
			*(y1++) = yuv[src_width+0];	// y on next low
			*(y1++) = yuv[src_width+2];	// y' on next low
			*(y1++) = yuv[src_width+4];	// y''  on next low
			*(y1++) = yuv[src_width+6];	// y'''  on next low
			yuv += 8;	// (1pixel=2bytes)x4pixels=8bytes
		}
	}
	
	RETURN(UVC_SUCCESS, uvc_error_t);
}

/** @brief Convert a frame to RGB565
 * @ingroup frame
 *
 * @param in non-RGB565 frame
 * @param out RGB565 frame
 */
uvc_error_t uvc_any2rgb565(uvc_frame_t *in, uvc_frame_t *out) {

	switch (in->frame_format) {
#ifdef LIBUVC_HAS_JPEG
	case UVC_FRAME_FORMAT_MJPEG:
		return uvc_mjpeg2rgb565(in, out);
#endif
	case UVC_FRAME_FORMAT_YUYV:
		return uvc_yuyv2rgb565(in, out);
	case UVC_FRAME_FORMAT_UYVY:
		return uvc_uyvy2rgb565(in, out);
	case UVC_FRAME_FORMAT_RGB565:
		return uvc_duplicate_frame(in, out);
	case UVC_FRAME_FORMAT_RGB:
		return uvc_rgb2rgb565(in, out);
	default:
		return UVC_ERROR_NOT_SUPPORTED;
	}
}

/** @brief Convert a frame to RGB888
 * @ingroup frame
 *
 * @param in non-RGB888 frame
 * @param out RGB888 frame
 */
uvc_error_t uvc_any2rgb(uvc_frame_t *in, uvc_frame_t *out) {

	switch (in->frame_format) {
#ifdef LIBUVC_HAS_JPEG
	case UVC_FRAME_FORMAT_MJPEG:
		return uvc_mjpeg2rgb(in, out);
#endif
	case UVC_FRAME_FORMAT_YUYV:
		return uvc_yuyv2rgb(in, out);
	case UVC_FRAME_FORMAT_UYVY:
		return uvc_uyvy2rgb(in, out);
	case UVC_FRAME_FORMAT_RGB:
		return uvc_duplicate_frame(in, out);
	default:
		return UVC_ERROR_NOT_SUPPORTED;
	}
}

/** @brief Convert a frame to BGR888
 * @ingroup frame
 *
 * @param in non-BGR888 frame
 * @param out BGR888 frame
 */
uvc_error_t uvc_any2bgr(uvc_frame_t *in, uvc_frame_t *out) {

	switch (in->frame_format) {
#ifdef LIBUVC_HAS_JPEG
	case UVC_FRAME_FORMAT_MJPEG:
		return uvc_mjpeg2bgr(in, out);
#endif
	case UVC_FRAME_FORMAT_YUYV:
		return uvc_yuyv2bgr(in, out);
	case UVC_FRAME_FORMAT_UYVY:
		return uvc_uyvy2bgr(in, out);
	case UVC_FRAME_FORMAT_BGR:
		return uvc_duplicate_frame(in, out);
	default:
		return UVC_ERROR_NOT_SUPPORTED;
	}
}

/** @brief Convert a frame to RGBX8888
 * @ingroup frame
 *
 * @param in non-rgbx frame
 * @param out rgbx frame
 */
uvc_error_t uvc_any2rgbx(uvc_frame_t *in, uvc_frame_t *out) {

	switch (in->frame_format) {
#ifdef LIBUVC_HAS_JPEG
	case UVC_FRAME_FORMAT_MJPEG:
		return uvc_mjpeg2rgbx(in, out);
#endif
	case UVC_FRAME_FORMAT_YUYV:
		return uvc_yuyv2rgbx(in, out);
	case UVC_FRAME_FORMAT_UYVY:
		return uvc_uyvy2rgbx(in, out);
	case UVC_FRAME_FORMAT_RGBX:
		return uvc_duplicate_frame(in, out);
	case UVC_FRAME_FORMAT_RGB:
		return uvc_rgb2rgbx(in, out);
	case UVC_FRAME_FORMAT_NV12:
		return uvc_nv122rgbx(in, out);
	case UVC_FRAME_FORMAT_YU12:
		return uvc_yu122rgbx(in, out);
	case UVC_FRAME_FORMAT_BGR:
		return uvc_bgr2rgbx(in, out);
	case UVC_FRAME_FORMAT_P010:
		return uvc_p0102rgbx(in, out);
	default:
		return UVC_ERROR_NOT_SUPPORTED;
	}
}

/** @brief Convert a frame to yuyv
 * @ingroup frame
 *
 * @param in non-yuyv frame
 * @param out yuyv frame
 */
uvc_error_t uvc_any2yuyv(uvc_frame_t *in, uvc_frame_t *out) {

	switch (in->frame_format) {
#ifdef LIBUVC_HAS_JPEG
	case UVC_FRAME_FORMAT_MJPEG:
		return uvc_mjpeg2yuyv(in, out);
#endif
	case UVC_FRAME_FORMAT_YUYV:
		return uvc_duplicate_frame(in, out);
	default:
		return UVC_ERROR_NOT_SUPPORTED;
	}
}

/* Scratch frame reused across calls to avoid per-frame malloc in the any2yuv420SP
   family. Sized up on demand. Use a bounded cache and transient allocations for
   unusually large frames so retained memory does not grow without bound. */
static uvc_frame_t *s_yuv420sp_scratch;
/* Keep persistent cache modest; larger frames use temporary scratch. */
#define LIBUVC_YUV420SP_SCRATCH_CAP_BYTES (32 * 1024 * 1024)

static uvc_frame_t *get_yuv_scratch(size_t need_bytes, int *must_free) {
	if (must_free) {
		*must_free = 0;
	}

	if (UNLIKELY(need_bytes > LIBUVC_YUV420SP_SCRATCH_CAP_BYTES)) {
		if (must_free) {
			*must_free = 1;
		}
		return uvc_allocate_frame(need_bytes);
	}

	if (UNLIKELY(!s_yuv420sp_scratch)) {
		s_yuv420sp_scratch = uvc_allocate_frame(need_bytes);
	} else if (UNLIKELY(s_yuv420sp_scratch->data_bytes < need_bytes)) {
		uvc_free_frame(s_yuv420sp_scratch);
		s_yuv420sp_scratch = uvc_allocate_frame(need_bytes);
	}
	return s_yuv420sp_scratch;
}

void uvc_cleanup_frame_caches(void) {
	if (s_yuv420sp_scratch) {
		uvc_free_frame(s_yuv420sp_scratch);
		s_yuv420sp_scratch = NULL;
	}
}

/** @brief Convert a frame to yuv420sp
 * @ingroup frame
 *
 * @param in non-yuv420sp frame
 * @param out yuv420sp frame
 */
uvc_error_t uvc_any2yuv420SP(uvc_frame_t *in, uvc_frame_t *out) {
	int free_scratch = 0;
	uvc_frame_t *yuv = get_yuv_scratch((in->width * in->height * 3) / 2, &free_scratch);
	if (UNLIKELY(!yuv))
		return UVC_ERROR_NO_MEM;
	uvc_error_t result = uvc_any2yuyv(in, yuv);
	if (LIKELY(!result))
		result = uvc_yuyv2yuv420SP(yuv, out);
	if (free_scratch)
		uvc_free_frame(yuv);
	return result;
}

/** @brief Convert a frame to iyuv420sp(NV21)
 * @ingroup frame
 *
 * @param in non-iyuv420SP(NV21) frame
 * @param out iyuv420SP(NV21) frame
 */
uvc_error_t uvc_any2iyuv420SP(uvc_frame_t *in, uvc_frame_t *out) {
	int free_scratch = 0;
	uvc_frame_t *yuv = get_yuv_scratch((in->width * in->height * 3) / 2, &free_scratch);
	if (UNLIKELY(!yuv))
		return UVC_ERROR_NO_MEM;
	uvc_error_t result = uvc_any2yuyv(in, yuv);
	if (LIKELY(!result))
		result = uvc_yuyv2iyuv420SP(yuv, out);
	if (free_scratch)
		uvc_free_frame(yuv);
	return result;
}
