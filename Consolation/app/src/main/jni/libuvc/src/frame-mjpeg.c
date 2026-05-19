/*********************************************************************
 * add and modified some function to avoid crash
 * Copyright (C) 2014-2015 saki@serenegiant All rights reserved.
 *********************************************************************/
/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (C) 2014 Robert Xiao
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
 */
#include "libuvc/libuvc.h"
#include "libuvc/libuvc_internal.h"
#include <jpeglib.h>
#include <pthread.h>
#include <setjmp.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

extern uvc_error_t uvc_ensure_frame_size(uvc_frame_t *frame, size_t need_bytes);

#ifndef UVC_RUNTIME_DIAG_ENABLED
#define UVC_RUNTIME_DIAG_ENABLED 0
#endif

#if UVC_RUNTIME_DIAG_ENABLED
#if defined(__ANDROID__)
#define UVC_DIAG_LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#else
#define UVC_DIAG_LOGI(...) LOGI(__VA_ARGS__)
#endif
#else
#define UVC_DIAG_LOGI(...)
#endif

static pthread_once_t uvc_mjpeg_rgbx_diag_once = PTHREAD_ONCE_INIT;

static void uvc_mjpeg_rgbx_diag_enabled_once(void) {
	UVC_DIAG_LOGI("mjpeg-diag:decoder-active rgbx diag=%d", UVC_RUNTIME_DIAG_ENABLED);
}

struct error_mgr {
	struct jpeg_error_mgr super;
	void (*original_emit_message)(j_common_ptr cinfo, int msg_level);
	jmp_buf jmp;
	int warning_count;
};

static void _emit_message(j_common_ptr dinfo, int msg_level) {
	struct error_mgr *myerr = (struct error_mgr *) dinfo->err;
	if (msg_level < 0)
		myerr->warning_count++;
#ifndef NDEBUG
	if (myerr->original_emit_message)
		(*myerr->original_emit_message)(dinfo, msg_level);
#else
	(void)dinfo;
#endif
}

static void _error_exit(j_common_ptr dinfo) {
	struct error_mgr *myerr = (struct error_mgr *) dinfo->err;
#ifndef NDEBUG
#if (defined(ANDROID) || defined(__ANDROID__))
	char err_msg[1024];
	(*dinfo->err->format_message)(dinfo, err_msg);
	err_msg[1023] = 0;
	LOGW("err=%s", err_msg);
#else
	(*dinfo->err->output_message)(dinfo);
#endif
#endif
	longjmp(myerr->jmp, 1);
}

struct mjpeg_decoder_ctx {
	struct jpeg_decompress_struct dinfo;
	int initialized;
};

static pthread_key_t mjpeg_decoder_key;
static pthread_once_t mjpeg_decoder_key_once = PTHREAD_ONCE_INIT;
static int mjpeg_decoder_key_result;

static void _mjpeg_decoder_destroy(void *ptr) {
	struct mjpeg_decoder_ctx *ctx = (struct mjpeg_decoder_ctx *) ptr;
	if (!ctx)
		return;
	if (ctx->initialized)
		jpeg_destroy_decompress(&ctx->dinfo);
	free(ctx);
}

static void _mjpeg_decoder_make_key(void) {
	mjpeg_decoder_key_result = pthread_key_create(&mjpeg_decoder_key,
		_mjpeg_decoder_destroy);
}

static struct mjpeg_decoder_ctx *_mjpeg_decoder_get(void) {
	struct mjpeg_decoder_ctx *ctx;

	pthread_once(&mjpeg_decoder_key_once, _mjpeg_decoder_make_key);
	if (UNLIKELY(mjpeg_decoder_key_result))
		return NULL;

	ctx = (struct mjpeg_decoder_ctx *) pthread_getspecific(mjpeg_decoder_key);
	if (ctx)
		return ctx;

	ctx = (struct mjpeg_decoder_ctx *) calloc(1, sizeof(*ctx));
	if (UNLIKELY(!ctx))
		return NULL;

	if (UNLIKELY(pthread_setspecific(mjpeg_decoder_key, ctx))) {
		free(ctx);
		return NULL;
	}

	return ctx;
}

static void _mjpeg_decoder_reset(struct mjpeg_decoder_ctx *ctx) {
	if (ctx && ctx->initialized) {
		jpeg_destroy_decompress(&ctx->dinfo);
		memset(&ctx->dinfo, 0, sizeof(ctx->dinfo));
		ctx->initialized = 0;
	}
}

/* ISO/IEC 10918-1:1993(E) K.3.3. Default Huffman tables used by MJPEG UVC devices
 which don't specify a Huffman table in the JPEG stream. */
static const unsigned char dc_lumi_len[] = {
	0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const unsigned char dc_lumi_val[] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };

static const unsigned char dc_chromi_len[] = {
	0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
static const unsigned char dc_chromi_val[] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };

static const unsigned char ac_lumi_len[] = {
	0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const unsigned char ac_lumi_val[] = {
	0x01, 0x02, 0x03, 0x00, 0x04, 0x11,	0x05, 0x12,
	0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
	0x22, 0x71,	0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
	0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
	0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16,
	0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
	0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
	0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
	0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
	0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
	0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
	0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
	0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
	0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
	0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
	0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
	0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4,
	0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
	0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
	0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
	0xf9, 0xfa
};
static const unsigned char ac_chromi_len[] = {
	0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
static const unsigned char ac_chromi_val[] = {
	0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
	0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
	0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
	0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
	0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34,
	0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
	0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38,
	0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
	0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
	0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
	0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
	0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
	0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96,
	0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
	0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
	0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
	0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2,
	0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
	0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
	0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
	0xf9, 0xfa
};

#define COPY_HUFF_TABLE(dinfo,tbl,name) do { \
	if (dinfo->tbl == NULL) dinfo->tbl = jpeg_alloc_huff_table((j_common_ptr)dinfo); \
		memcpy(dinfo->tbl->bits, name##_len, sizeof(name##_len)); \
		memset(dinfo->tbl->huffval, 0, sizeof(dinfo->tbl->huffval)); \
		memcpy(dinfo->tbl->huffval, name##_val, sizeof(name##_val)); \
	} while(0)

static inline void insert_huff_tables(j_decompress_ptr dinfo) {
	COPY_HUFF_TABLE(dinfo, dc_huff_tbl_ptrs[0], dc_lumi);
	COPY_HUFF_TABLE(dinfo, dc_huff_tbl_ptrs[1], dc_chromi);
	COPY_HUFF_TABLE(dinfo, ac_huff_tbl_ptrs[0], ac_lumi);
	COPY_HUFF_TABLE(dinfo, ac_huff_tbl_ptrs[1], ac_chromi);
}

// XXX added to improve the performance of decoding
// maximun reading lines for each call of jpeg_read_scanlines
// when defined this macro, it's value should be common factor
// of all available frame height.
// (1, 2, 4, 5, 6, 8, 10, 12, 20, 40...for 720p&1080p)
#define MAX_READLINE 8

#ifndef MAX_READLINE
#define MAX_READLINE 1
#endif
#if MAX_READLINE < 1
#undef MAX_READLINE
#define MAX_READLINE 1
#endif

#ifndef MJPEG_RGBX_READLINE
#define MJPEG_RGBX_READLINE 64
#endif

/* Diagnostic/compat path: avoid carrying libjpeg decompressor state across
 * MJPEG frames.  If this fixes one card's flicker, make it device-scoped. */
#ifndef UVC_MJPEG_RGBX_REUSE_DECODER
#define UVC_MJPEG_RGBX_REUSE_DECODER 1
#endif

/** True if full output height was decoded (avoids size_t vs int compare on return paths). */
static inline int uvc_mjpeg_lines_match_height(size_t lines_decoded, int height_px) {
	if UNLIKELY(height_px < 0)
		return 0;
	return lines_decoded == (size_t) height_px;
}

#if UVC_RUNTIME_DIAG_ENABLED
static uint32_t uvc_mjpeg_diag_hash_range(uint32_t hash, const uint8_t *data,
	size_t len) {
	size_t i;
	for (i = 0; i < len; i++) {
		hash ^= data[i];
		hash *= 16777619u;
	}
	return hash;
}

static uint32_t uvc_mjpeg_diag_source_hash(const uint8_t *data, size_t len) {
	uint32_t hash = 2166136261u;
	const uint64_t len64 = (uint64_t)len;

	if (!data || !len)
		return 0;

	hash ^= (uint32_t)len64;
	hash *= 16777619u;
	hash ^= (uint32_t)(len64 >> 32);
	hash *= 16777619u;
	return uvc_mjpeg_diag_hash_range(hash, data, len);
}

static uint32_t uvc_mjpeg_diag_full_hash(const uint8_t *data, size_t len) {
	if (!data || !len)
		return 0;
	return uvc_mjpeg_diag_hash_range(2166136261u, data, len);
}
#endif

/** @brief Convert an MJPEG frame to RGB
 * @ingroup frame
 *
 * @param in MJPEG frame
 * @param out RGB frame
 */
uvc_error_t uvc_mjpeg2rgb(uvc_frame_t *in, uvc_frame_t *out) {
	struct jpeg_decompress_struct dinfo;
	struct error_mgr jerr;
	size_t lines_read;

	int num_scanlines, i;
	lines_read = 0;
	unsigned char *buffer[MAX_READLINE];

	out->actual_bytes = 0;	// XXX
	if (UNLIKELY(in->frame_format != UVC_FRAME_FORMAT_MJPEG))
		return UVC_ERROR_INVALID_PARAM;

	if (uvc_ensure_frame_size(out, in->width * in->height * 3) < 0)
		return UVC_ERROR_NO_MEM;

	out->width = in->width;
	out->height = in->height;
	out->frame_format = UVC_FRAME_FORMAT_RGB;
	out->step = in->width * 3;
	// local copy — MUST be after ensure_frame_size (may realloc data) and after step is set
	uint8_t *data = out->data;
	const int out_step = out->step;
	out->sequence = in->sequence;
	out->capture_time = in->capture_time;
	out->source = in->source;

	dinfo.err = jpeg_std_error(&jerr.super);
	jerr.super.error_exit = _error_exit;

	if (setjmp(jerr.jmp)) {
		goto fail;
	}

	jpeg_create_decompress(&dinfo);
	jpeg_mem_src(&dinfo, in->data, in->actual_bytes/*in->data_bytes*/);
	jpeg_read_header(&dinfo, TRUE);

	if (dinfo.dc_huff_tbl_ptrs[0] == NULL) {
		/* This frame is missing the Huffman tables: fill in the standard ones */
		insert_huff_tables(&dinfo);
	}

	dinfo.out_color_space = JCS_RGB;
	dinfo.dct_method = JDCT_IFAST;

	jpeg_start_decompress(&dinfo);

	if (LIKELY(out->height >= 0 &&
			dinfo.output_height == (JDIMENSION) out->height)) {
		for (; dinfo.output_scanline < dinfo.output_height ;) {
			buffer[0] = data + (lines_read) * out_step;
			for (i = 1; i < MAX_READLINE; i++)
				buffer[i] = buffer[i-1] + out_step;
			num_scanlines = jpeg_read_scanlines(&dinfo, buffer, MAX_READLINE);
			lines_read += num_scanlines;
		}
		out->actual_bytes = in->width * in->height * 3;	// XXX
	}
	jpeg_finish_decompress(&dinfo);
	jpeg_destroy_decompress(&dinfo);
	return uvc_mjpeg_lines_match_height(lines_read, out->height)
		? UVC_SUCCESS : UVC_ERROR_OTHER;

fail:
	jpeg_destroy_decompress(&dinfo);
	return UVC_ERROR_OTHER+1;
}

/** @brief Convert an MJPEG frame to BGR
 * @ingroup frame
 *
 * @param in MJPEG frame
 * @param out BGR frame
 */
uvc_error_t uvc_mjpeg2bgr(uvc_frame_t *in, uvc_frame_t *out) {
	struct jpeg_decompress_struct dinfo;
	struct error_mgr jerr;
	size_t lines_read;

	int num_scanlines, i;
	lines_read = 0;
	unsigned char *buffer[MAX_READLINE];

	out->actual_bytes = 0;	// XXX
	if (UNLIKELY(in->frame_format != UVC_FRAME_FORMAT_MJPEG))
		return UVC_ERROR_INVALID_PARAM;

	if (uvc_ensure_frame_size(out, in->width * in->height * 3) < 0)
		return UVC_ERROR_NO_MEM;

	out->width = in->width;
	out->height = in->height;
	out->frame_format = UVC_FRAME_FORMAT_BGR;
	out->step = in->width * 3;
	out->sequence = in->sequence;
	out->capture_time = in->capture_time;
	out->source = in->source;

	dinfo.err = jpeg_std_error(&jerr.super);
	jerr.super.error_exit = _error_exit;

	if (setjmp(jerr.jmp)) {
		goto fail;
	}

	jpeg_create_decompress(&dinfo);
	jpeg_mem_src(&dinfo, in->data, in->actual_bytes/*in->data_bytes*/);
	jpeg_read_header(&dinfo, TRUE);

	if (dinfo.dc_huff_tbl_ptrs[0] == NULL) {
		/* This frame is missing the Huffman tables: fill in the standard ones */
		insert_huff_tables(&dinfo);
	}

	dinfo.out_color_space = JCS_EXT_BGR;
	dinfo.dct_method = JDCT_IFAST;

	jpeg_start_decompress(&dinfo);

	// local copy
	uint8_t *data = out->data;
	const int out_step = out->step;

	if (LIKELY(out->height >= 0 &&
			dinfo.output_height == (JDIMENSION) out->height)) {
		for (; dinfo.output_scanline < dinfo.output_height ;) {
			buffer[0] = data + (lines_read) * out_step;
			for (i = 1; i < MAX_READLINE; i++)
				buffer[i] = buffer[i-1] + out_step;
			num_scanlines = jpeg_read_scanlines(&dinfo, buffer, MAX_READLINE);
			lines_read += num_scanlines;
		}
		out->actual_bytes = in->width * in->height * 3;	// XXX
	}
	jpeg_finish_decompress(&dinfo);
	jpeg_destroy_decompress(&dinfo);
	return uvc_mjpeg_lines_match_height(lines_read, out->height)
		? UVC_SUCCESS : UVC_ERROR_OTHER;

fail:
	jpeg_destroy_decompress(&dinfo);
	return UVC_ERROR_OTHER+1;
}

/** @brief Convert an MJPEG frame to RGB565
 * @ingroup frame
 *
 * @param in MJPEG frame
 * @param out RGB frame
 */
uvc_error_t uvc_mjpeg2rgb565(uvc_frame_t *in, uvc_frame_t *out) {
	struct jpeg_decompress_struct dinfo;
	struct error_mgr jerr;
	size_t lines_read;

	int num_scanlines, i;
	lines_read = 0;
	unsigned char *buffer[MAX_READLINE];

	out->actual_bytes = 0;	// XXX
	if (UNLIKELY(in->frame_format != UVC_FRAME_FORMAT_MJPEG))
		return UVC_ERROR_INVALID_PARAM;

	if (uvc_ensure_frame_size(out, in->width * in->height * 2) < 0)
		return UVC_ERROR_NO_MEM;

	out->width = in->width;
	out->height = in->height;
	out->frame_format = UVC_FRAME_FORMAT_RGB565;
	out->step = in->width * 2;
	out->sequence = in->sequence;
	out->capture_time = in->capture_time;
	out->source = in->source;

	dinfo.err = jpeg_std_error(&jerr.super);
	jerr.super.error_exit = _error_exit;

	if (setjmp(jerr.jmp)) {
		goto fail;
	}

	jpeg_create_decompress(&dinfo);
	jpeg_mem_src(&dinfo, in->data, in->actual_bytes/*in->data_bytes*/);
	jpeg_read_header(&dinfo, TRUE);

	if (dinfo.dc_huff_tbl_ptrs[0] == NULL) {
		/* This frame is missing the Huffman tables: fill in the standard ones */
		insert_huff_tables(&dinfo);
	}

	dinfo.out_color_space = JCS_RGB565;
	dinfo.dct_method = JDCT_IFAST;

	jpeg_start_decompress(&dinfo);

	// local copy
	uint8_t *data = out->data;
	const int out_step = out->step;

	if (LIKELY(out->height >= 0 &&
			dinfo.output_height == (JDIMENSION) out->height)) {
		for (; dinfo.output_scanline < dinfo.output_height ;) {
			buffer[0] = data + (lines_read) * out_step;
			for (i = 1; i < MAX_READLINE; i++)
				buffer[i] = buffer[i-1] + out_step;
			num_scanlines = jpeg_read_scanlines(&dinfo, buffer, MAX_READLINE);
			lines_read += num_scanlines;
		}
		out->actual_bytes = in->width * in->height * 2;	// XXX
	}
	jpeg_finish_decompress(&dinfo);
	jpeg_destroy_decompress(&dinfo);
	return uvc_mjpeg_lines_match_height(lines_read, out->height)
		? UVC_SUCCESS : UVC_ERROR_OTHER;

fail:
	jpeg_destroy_decompress(&dinfo);
	return UVC_ERROR_OTHER+1;
}

/** @brief Convert an MJPEG frame to RGBX
 * @ingroup frame
 *
 * @param in MJPEG frame
 * @param out RGBX frame
 */
uvc_error_t uvc_mjpeg2rgbx(uvc_frame_t *in, uvc_frame_t *out) {
#if UVC_MJPEG_RGBX_REUSE_DECODER
	struct mjpeg_decoder_ctx *decoder;
	struct jpeg_decompress_struct *dinfo;
#else
	struct jpeg_decompress_struct dinfo_stack;
	struct jpeg_decompress_struct *dinfo = &dinfo_stack;
#endif
	struct error_mgr jerr;
	size_t lines_read;
#if UVC_RUNTIME_DIAG_ENABLED
	const uint8_t *diag_src = (const uint8_t *)in->data;
	const size_t diag_src_len = in->actual_bytes;
	const uint32_t diag_hash_before = uvc_mjpeg_diag_source_hash(diag_src, diag_src_len);
	const uint32_t diag_full_hash_before = uvc_mjpeg_diag_full_hash(diag_src, diag_src_len);
#endif

	int num_scanlines, i;
	lines_read = 0;
	unsigned char *buffer[MJPEG_RGBX_READLINE];

	out->actual_bytes = 0;	// XXX
	if (UNLIKELY(in->frame_format != UVC_FRAME_FORMAT_MJPEG))
		return UVC_ERROR_INVALID_PARAM;
	pthread_once(&uvc_mjpeg_rgbx_diag_once, uvc_mjpeg_rgbx_diag_enabled_once);

	if (uvc_ensure_frame_size(out, in->width * in->height * 4) < 0)
		return UVC_ERROR_NO_MEM;

	out->width = in->width;
	out->height = in->height;
	out->frame_format = UVC_FRAME_FORMAT_RGBX;	// XXX
	if (out->library_owns_data || out->step < (size_t) in->width * 4)
		out->step = in->width * 4;
	// local copy — MUST be after ensure_frame_size (may realloc data) and after step is set
	uint8_t *data = out->data;
	const int out_step = out->step;
	out->sequence = in->sequence;
	out->capture_time = in->capture_time;
	out->source = in->source;

#if UVC_MJPEG_RGBX_REUSE_DECODER
	decoder = _mjpeg_decoder_get();
	if (UNLIKELY(!decoder))
		return UVC_ERROR_NO_MEM;

	dinfo = &decoder->dinfo;
#else
	memset(dinfo, 0, sizeof(*dinfo));
#endif
	dinfo->err = jpeg_std_error(&jerr.super);
	jerr.original_emit_message = jerr.super.emit_message;
	jerr.super.error_exit = _error_exit;
	jerr.super.emit_message = _emit_message;
	jerr.warning_count = 0;

	if (setjmp(jerr.jmp)) {
		goto fail;
	}

#if UVC_MJPEG_RGBX_REUSE_DECODER
	if (!decoder->initialized) {
		jpeg_create_decompress(dinfo);
		decoder->initialized = 1;
	}
#else
	jpeg_create_decompress(dinfo);
#endif

	jpeg_mem_src(dinfo, in->data, in->actual_bytes/*in->data_bytes*/);	// XXX
	jpeg_read_header(dinfo, TRUE);

	if (dinfo->dc_huff_tbl_ptrs[0] == NULL) {
		/* This frame is missing the Huffman tables: fill in the standard ones */
		insert_huff_tables(dinfo);
	}

	dinfo->out_color_space = JCS_EXT_RGBX;
	dinfo->dct_method = JDCT_IFAST;
	dinfo->do_fancy_upsampling = FALSE;

	jpeg_start_decompress(dinfo);

	if (LIKELY(out->height >= 0 &&
			dinfo->output_height == (JDIMENSION) out->height &&
			dinfo->output_width == (JDIMENSION) out->width)) {
		for (; dinfo->output_scanline < dinfo->output_height ;) {
			const JDIMENSION rows_remaining = dinfo->output_height - dinfo->output_scanline;
			const JDIMENSION rows_to_read = rows_remaining < MJPEG_RGBX_READLINE
				? rows_remaining : MJPEG_RGBX_READLINE;
			buffer[0] = data + (lines_read) * out_step;
			for (i = 1; i < (int) rows_to_read; i++)
				buffer[i] = buffer[i-1] + out_step;
			num_scanlines = jpeg_read_scanlines(dinfo, buffer, rows_to_read);
			lines_read += num_scanlines;
		}
		out->actual_bytes = in->width * in->height * 4;	// XXX
	}
	jpeg_finish_decompress(dinfo);
#if !UVC_MJPEG_RGBX_REUSE_DECODER
	jpeg_destroy_decompress(dinfo);
#endif
#if UVC_RUNTIME_DIAG_ENABLED
	{
		const uint32_t diag_hash_after = uvc_mjpeg_diag_source_hash(diag_src, diag_src_len);
		if (UNLIKELY(diag_hash_before && diag_hash_after
				&& diag_hash_before != diag_hash_after)) {
			UVC_DIAG_LOGI("mjpeg-diag:source-mutated rgbx seq=%u bytes=%zu "
				"before=%08x after=%08x lines=%zu/%d",
				in->sequence,
				in->actual_bytes,
				diag_hash_before,
				diag_hash_after,
				lines_read,
				out->height);
		}
	}
#endif
	if (UNLIKELY(jerr.warning_count)) {
		UVC_DIAG_LOGI("mjpeg-diag:decode-warning rgbx seq=%u bytes=%zu src=%08x full=%08x warnings=%d lines=%zu/%d",
			in->sequence,
			in->actual_bytes,
			diag_hash_before,
			diag_full_hash_before,
			jerr.warning_count,
			lines_read,
			out->height);
	}
	return !jerr.warning_count && uvc_mjpeg_lines_match_height(lines_read, out->height)
		? UVC_SUCCESS : UVC_ERROR_OTHER;

fail:
#if UVC_RUNTIME_DIAG_ENABLED
	{
		const uint32_t diag_hash_after = uvc_mjpeg_diag_source_hash(diag_src, diag_src_len);
		if (UNLIKELY(diag_hash_before && diag_hash_after
				&& diag_hash_before != diag_hash_after)) {
			UVC_DIAG_LOGI("mjpeg-diag:source-mutated rgbx-fail seq=%u bytes=%zu "
				"before=%08x after=%08x lines=%zu/%d",
				in->sequence,
				in->actual_bytes,
				diag_hash_before,
				diag_hash_after,
				lines_read,
				out->height);
		}
	}
#endif
#if UVC_MJPEG_RGBX_REUSE_DECODER
	_mjpeg_decoder_reset(decoder);
#else
	jpeg_destroy_decompress(dinfo);
#endif
	return UVC_ERROR_OTHER+1;
}

static inline unsigned char sat(int i) {
	return (unsigned char) (i >= 255 ? 255 : (i < 0 ? 0 : i));
}

#define YCbCr_YUYV_2(YCbCr, yuyv) \
	{ \
		*(yuyv++) = *(YCbCr+0); \
		*(yuyv++) = (*(YCbCr+1) + *(YCbCr+4)) >> 1; \
		*(yuyv++) = *(YCbCr+3); \
		*(yuyv++) = (*(YCbCr+2) + *(YCbCr+5)) >> 1; \
	}

uvc_error_t uvc_mjpeg2yuyv(uvc_frame_t *in, uvc_frame_t *out) {
	struct mjpeg_decoder_ctx *decoder;
	struct jpeg_decompress_struct *dinfo;
	struct error_mgr jerr;

	out->actual_bytes = 0;	// XXX
	if (UNLIKELY(in->frame_format != UVC_FRAME_FORMAT_MJPEG))
		return UVC_ERROR_INVALID_PARAM;

	if (uvc_ensure_frame_size(out, in->width * in->height * 2) < 0)
		return UVC_ERROR_NO_MEM;

	size_t lines_read = 0;
	int i, j;
	int num_scanlines;
	register uint8_t *yuyv, *ycbcr;

	out->width = in->width;
	out->height = in->height;
	out->frame_format = UVC_FRAME_FORMAT_YUYV;
	out->step = in->width * 2;
	out->sequence = in->sequence;
	out->capture_time = in->capture_time;
	out->source = in->source;

	decoder = _mjpeg_decoder_get();
	if (UNLIKELY(!decoder))
		return UVC_ERROR_NO_MEM;

	dinfo = &decoder->dinfo;
	dinfo->err = jpeg_std_error(&jerr.super);
	jerr.super.error_exit = _error_exit;

	if (setjmp(jerr.jmp)) {
		goto fail;
	}

	if (!decoder->initialized) {
		jpeg_create_decompress(dinfo);
		decoder->initialized = 1;
	}

	jpeg_mem_src(dinfo, in->data, in->actual_bytes/*in->data_bytes*/);	// XXX
	jpeg_read_header(dinfo, TRUE);

	if (dinfo->dc_huff_tbl_ptrs[0] == NULL) {
		/* This frame is missing the Huffman tables: fill in the standard ones */
		insert_huff_tables(dinfo);
	}

	dinfo->out_color_space = JCS_YCbCr;
	dinfo->dct_method = JDCT_IFAST;

	// start decompressor
	jpeg_start_decompress(dinfo);

	// these dinfo.xxx valiables are only valid after jpeg_start_decompress
	const int row_stride = dinfo->output_width * dinfo->output_components;

	// allocate buffer
	register JSAMPARRAY buffer = (*dinfo->mem->alloc_sarray)
		((j_common_ptr) dinfo, JPOOL_IMAGE, row_stride, MAX_READLINE);

	// local copy
	uint8_t *data = out->data;
	const int out_step = out->step;

	if (LIKELY(out->height >= 0 &&
			dinfo->output_height == (JDIMENSION) out->height)) {
		for (; dinfo->output_scanline < dinfo->output_height ;) {
			// convert lines of mjpeg data to YCbCr
			num_scanlines = jpeg_read_scanlines(dinfo, buffer, MAX_READLINE);
			// convert YCbCr to yuyv(YUV422)
			for (j = 0; j < num_scanlines; j++) {
				yuyv = data + (lines_read + j) * out_step;
				ycbcr = buffer[j];
				/*
				 * 24-byte blocks = four 6-pixel YCbCr macropairs → 16 B YUYV.
				 * `i < row_stride; i+=24` over-reads trailing bytes when row_stride % 24 != 0
				 * and left tail rows partially unwritten. Handle vectorised block then pairs.
				 */
				for (i = 0; i + 24 <= row_stride; i += 24) {
					YCbCr_YUYV_2(ycbcr + i, yuyv);
					YCbCr_YUYV_2(ycbcr + i + 6, yuyv);
					YCbCr_YUYV_2(ycbcr + i + 12, yuyv);
					YCbCr_YUYV_2(ycbcr + i + 18, yuyv);
				}
				while (i + 6 <= row_stride) {
					YCbCr_YUYV_2(ycbcr + i, yuyv);
					i += 6;
				}
			}
			lines_read += num_scanlines;
		}
		out->actual_bytes = in->width * in->height * 2;	// XXX
	}

	jpeg_finish_decompress(dinfo);
	return uvc_mjpeg_lines_match_height(lines_read, out->height)
		? UVC_SUCCESS : UVC_ERROR_OTHER;

fail:
	_mjpeg_decoder_reset(decoder);
	return uvc_mjpeg_lines_match_height(lines_read, out->height)
		? UVC_SUCCESS : UVC_ERROR_OTHER+1;
}
