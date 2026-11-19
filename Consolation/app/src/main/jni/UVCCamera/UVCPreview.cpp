/*
 * UVCCamera
 * library and sample to access to UVC web camera on non-rooted Android device
 *
 * Copyright (c) 2014-2017 saki t_saki@serenegiant.com
 *
 * File name: UVCPreview.cpp
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 * All files in the folder are under this Apache License, Version 2.0.
 * Files in the jni/libjpeg, jni/libusb, jin/libuvc, jni/rapidjson folder may have a different license, see the respective files.
*/

#include <algorithm>
#include <stdlib.h>
#include <math.h>
#include <linux/time.h>
#include <time.h>
#include <unistd.h>

#if 1	// set 1 if you don't need debug log
	#ifndef LOG_NDEBUG
		#define	LOG_NDEBUG		// w/o LOGV/LOGD/MARK
	#endif
	#undef USE_LOGALL
#else
	#define USE_LOGALL
	#undef LOG_NDEBUG
//	#undef NDEBUG
#endif

#include "utilbase.h"
#include "UVCPreview.h"
#include "jni_iframe_callback_cache.h"
#include "libuvc_internal.h"

#include <sys/resource.h>
#include <dlfcn.h>
#include <android/hardware_buffer.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <stdio.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

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

namespace {

static void consolation_tune_thread_latency(const char *pthread_name_not_null)
{
#if defined(__ANDROID__)
	pthread_setname_np(pthread_self(), pthread_name_not_null);
	const pid_t tid = gettid();
	if (tid >= 1)
		(void)setpriority(PRIO_PROCESS, static_cast<id_t>(tid), -4);
#else
	(void)pthread_name_not_null;
#endif
}

static uint64_t processing_now_ns()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL
		+ static_cast<uint64_t>(ts.tv_nsec);
}

/** Relaxed atomic max: store `value` into `target` if it exceeds the current value. */
static inline void atomic_store_max(std::atomic<uint64_t> &target, uint64_t value)
{
	uint64_t prev = target.load(std::memory_order_relaxed);
	while (value > prev
		&& !target.compare_exchange_weak(prev, value,
			std::memory_order_relaxed, std::memory_order_relaxed)) {
		// prev is reloaded by compare_exchange_weak on failure
	}
}

} // namespace

/* Frame-buffer integrity probe: each hop re-samples the bytes it is about to
 * read and compares with the fingerprint taken when they were produced.  A
 * mismatch means something wrote into the buffer while it was in flight, and
 * the stage name says between which two threads.  Reads ~1 KiB per check. */
/* UVC_FRAME_INTEGRITY_CHECK defaults to the runtime-diagnostics switch; see libuvc.h. */

namespace {
static bool frame_integrity_ok(const uvc_frame_t *frame, const char *stage,
	std::atomic<uint32_t> &mismatches)
{
#if UVC_FRAME_INTEGRITY_CHECK
	if (!frame || !frame->data || !frame->integrity_sample_hash)
		return true;
	const size_t len = frame->actual_bytes ? frame->actual_bytes : frame->data_bytes;
	const uint32_t now = uvc_frame_sample_hash(frame->data, len);
	if (LIKELY(now == frame->integrity_sample_hash))
		return true;
	const uint32_t n = mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
	if (n <= 10 || !(n % 100))
		LOGW("frame-integrity: buffer changed in flight stage=%s count=%u seq=%u bytes=%zu "
			"expected=%08x now=%08x fmt=%d slot=%u",
			stage, n, frame->sequence, len, frame->integrity_sample_hash, now,
			frame->frame_format, frame->library_frame_slot);
	return false;
#else
	(void)frame; (void)stage; (void)mismatches;
	return true;
#endif
}
/* Debug-only raw MJPEG dump, driven by a system property so it costs nothing
 * unless armed:  adb shell setprop debug.consolation.mjpeg_dump <N>
 * dumps the next N consecutive frames handed to the async decoder into
 * /data/data/<pkg>/cache/mjpeg_dump/<seq>.jpg (pull with `adb shell run-as`).
 * Setting a different value re-arms it. The property is polled once per
 * 60 frames so the hot path pays one property read per second at most. */
static void debug_mjpeg_dump_maybe(const uvc_frame_t *frame)
{
	static char s_armed_value[PROP_VALUE_MAX] = {};
	static int s_remaining = 0;
	static uint32_t s_poll = 0;
	static const char *s_dir = "/data/data/org.centennialoss.consolation/cache/mjpeg_dump";

	if (!frame || !frame->data || !frame->actual_bytes)
		return;
	if (s_remaining <= 0) {
		if ((s_poll++ % 60) != 0)
			return;
		char value[PROP_VALUE_MAX] = {};
		if (__system_property_get("debug.consolation.mjpeg_dump", value) <= 0
				|| !value[0] || !strcmp(value, s_armed_value))
			return;
		strncpy(s_armed_value, value, sizeof(s_armed_value) - 1);
		s_remaining = atoi(value);
		if (s_remaining <= 0)
			return;
		mkdir(s_dir, 0755);
		LOGW("mjpeg-dump: armed for %d frames -> %s", s_remaining, s_dir);
	}
	char path[256];
	snprintf(path, sizeof(path), "%s/%06u.jpg", s_dir, frame->sequence);
	FILE *f = fopen(path, "wb");
	if (!f) {
		LOGW("mjpeg-dump: cannot open %s", path);
		s_remaining = 0;
		return;
	}
	fwrite(frame->data, 1, frame->actual_bytes, f);
	fclose(f);
	/* Sidecar: one line per USB packet "len flags cumulative_image_bytes". */
	snprintf(path, sizeof(path), "%s/%06u.pkts", s_dir, frame->sequence);
	f = fopen(path, "w");
	if (f) {
		size_t cum = 0;
		for (unsigned i = 0; i < frame->iso_trace_count; i++) {
			const unsigned len = frame->iso_trace_len[i];
			const unsigned fl = frame->iso_trace_flags[i];
			if (!(fl & 8u) && len > 12)
				cum += len - 12;
			fprintf(f, "%u %u %zu\n", len, fl, cum);
		}
		fclose(f);
	}
	if (--s_remaining == 0)
		LOGW("mjpeg-dump: done");
}

static std::atomic<uint32_t> g_integrity_mismatch_cb{0};
static std::atomic<uint32_t> g_integrity_mismatch_decode{0};
static std::atomic<uint32_t> g_integrity_mismatch_render{0};
} // namespace

/** processingUvcSeqState bit63 marks the last-sequence value as valid. */
#define UVC_SEQ_STATE_VALID_BIT (1ULL << 63)

#define	LOCAL_DEBUG 0
#define PREVIEW_PIXEL_BYTES 4	// RGBA/RGBX
/** Enough headroom for queue + MJPEG decompress + RGBX convert + capture */
#define FRAME_POOL_SZ (PREVIEW_QUEUE_MAX + 8)
#define REQUEST_MODE_YUYV 0
#define REQUEST_MODE_MJPEG 1
#define REQUEST_MODE_H264 2
#define REQUEST_MODE_NV12 3
#define REQUEST_MODE_P010 4
#define REQUEST_MODE_YU12 5
#define REQUEST_MODE_BGR3 6

static inline enum uvc_frame_format request_mode_to_frame_format(const int requestMode) {
	switch (requestMode) {
	case REQUEST_MODE_YUYV:
		return UVC_FRAME_FORMAT_YUYV;
	case REQUEST_MODE_H264:
		return UVC_FRAME_FORMAT_H264;
	case REQUEST_MODE_NV12:
		return UVC_FRAME_FORMAT_NV12;
	case REQUEST_MODE_P010:
		return UVC_FRAME_FORMAT_P010;
	case REQUEST_MODE_YU12:
		return UVC_FRAME_FORMAT_YU12;
	case REQUEST_MODE_BGR3:
		return UVC_FRAME_FORMAT_BGR;
	case REQUEST_MODE_MJPEG:
	default:
		return UVC_FRAME_FORMAT_MJPEG;
	}
}

static inline const char *request_mode_name(const int requestMode) {
	switch (requestMode) {
	case REQUEST_MODE_YUYV:
		return "YUYV";
	case REQUEST_MODE_H264:
		return "H264";
	case REQUEST_MODE_NV12:
		return "NV12";
	case REQUEST_MODE_P010:
		return "P010";
	case REQUEST_MODE_YU12:
		return "YU12";
	case REQUEST_MODE_BGR3:
		return "BGR3";
	case REQUEST_MODE_MJPEG:
	default:
		return "MJPEG";
	}
}

UVCPreview::UVCPreview(uvc_device_handle_t *devh)
:	mPreviewWindow(NULL),
	mCaptureWindow(NULL),
	mDeviceHandle(devh),
	requestWidth(DEFAULT_PREVIEW_WIDTH),
	requestHeight(DEFAULT_PREVIEW_HEIGHT),
	requestMinFps(DEFAULT_PREVIEW_FPS_MIN),
	requestMaxFps(DEFAULT_PREVIEW_FPS_MAX),
	requestMode(DEFAULT_PREVIEW_MODE),
	requestBandwidth(DEFAULT_BANDWIDTH),
	frameWidth(DEFAULT_PREVIEW_WIDTH),
	frameHeight(DEFAULT_PREVIEW_HEIGHT),
	frameBytes(DEFAULT_PREVIEW_WIDTH * DEFAULT_PREVIEW_HEIGHT * 2),	// YUYV
	frameMode(0),
	preview_frame_ring(PREVIEW_QUEUE_MAX),
	mjpeg_decode_thread_joinable(false),
	mjpeg_decode_frame_ring(MJPEG_DECODE_QUEUE_MAX),
	previewFormat(WINDOW_FORMAT_RGBX_8888),
	previewBytes(DEFAULT_PREVIEW_WIDTH * DEFAULT_PREVIEW_HEIGHT * PREVIEW_PIXEL_BYTES),
	mIsRunning(false),
	mIsCapturing(false),
	capture_thread_joinable(false),
	captureQueu(NULL),
	mGpuPreviewRenderer(new UVCGpuPreviewRenderer()),
	mMjpegPreviewYuvFrame(NULL),
	mFrameCallbackObj(NULL),
	mFrameCallbackFunc(NULL),
	callbackPixelBytes(2),
	mPreviewFrameCallbackObj(NULL),
	mPreviewPixelFormat(PIXEL_FORMAT_RAW),
	previewCallbackPixelBytes(1),
	preview_frame_callback_enabled(false),
	capture_frame_callback_enabled(false),
	processingPreviewConvertCount(0),
	processingPreviewConvertTotalNs(0),
	processingPreviewConvertMaxNs(0),
	processingCallbackConvertCount(0),
	processingCallbackConvertTotalNs(0),
	processingCallbackConvertMaxNs(0),
	processingCopyCount(0),
	processingCopyTotalNs(0),
	processingCopyMaxNs(0),
	processingEndToEndLatencyCount(0),
	processingEndToEndLatencyTotalNs(0),
	processingEndToEndLatencyMaxNs(0),
	processingPayloadCount(0),
	processingPayloadTotalBytes(0),
	processingPayloadMaxBytes(0),
	processingPreviewQueueDropCount(0),
	processingPreviewQueueDepthSampleCount(0),
	processingPreviewQueueDepthTotalMilli(0),
	processingPreviewQueueDepthMaxMilli(0),
	processingPreviewEnqueueDepthSampleCount(0),
	processingPreviewEnqueueDepthTotalMilli(0),
	processingPreviewEnqueueDepthMaxMilli(0),
	processingUvcCallbackCount(0),
	processingUvcCallbackTotalNs(0),
	processingUvcCallbackMaxNs(0),
	processingCallbackLagCount(0),
	processingCallbackLagTotalNs(0),
	processingCallbackLagMaxNs(0),
	processingPreCallbackSkippedFrames(0),
	processingUvcSeqState(0),
	diagMjpegDecodedCount(0),
	diagMjpegLastLuma(0),
	streamingStartMonotonicNs(0),
	firstFrameLogged(false) {

	ENTER();
	pthread_cond_init(&preview_sync, NULL);
	pthread_mutex_init(&preview_mutex, NULL);
	pthread_mutex_init(&preview_queue_mutex, NULL);
	pthread_cond_init(&mjpeg_decode_sync, NULL);
	pthread_mutex_init(&mjpeg_decode_mutex, NULL);
//
	pthread_cond_init(&capture_sync, NULL);
	pthread_mutex_init(&capture_mutex, NULL);
//
	pthread_mutex_init(&pool_mutex, NULL);
	iframecallback_fields.onFrame = nullptr;
	preview_iframecallback_fields.onFrame = nullptr;
	memset(mjpeg_header_slots, 0, sizeof(mjpeg_header_slots));
	memset(mjpeg_header_used, 0, sizeof(mjpeg_header_used));
	{
		static const float identity[9] = { 1, 0, 0,  0, 1, 0,  0, 0, 1 };
		memcpy(mPreviewXform, identity, sizeof(mPreviewXform));
	}
	mPreviewFitX = 1.0f;
	mPreviewFitY = 1.0f;
	mPreviewGeomWidth = 0;
	mPreviewGeomHeight = 0;
	mPreviewSurfaceSizeDirty = false;
	memset(mGpuPlanar, 0, sizeof(mGpuPlanar));
	for (int i = 0; i < GPU_PLANAR_POOL_SZ; i++)
		mGpuPlanar[i].fence_fd = -1;
	mGpuPlanarNext = 0;
	mGpuPlanarRenderFailures = 0;
	mMergedRender = true;
	mUsbThreadTid.store(0, std::memory_order_relaxed);
	mStreamDecodeEnabled = false;
	memset(&mLive, 0, sizeof(mLive));
	memset(&mStream, 0, sizeof(mStream));
	mPerfHintSession = NULL;
	mPerfHintTried = false;
	{
		/* Pick a GPU-sampleable buffer format for the zero-copy planar path.
		 * R8 is the natural fit but many gralloc implementations reject it;
		 * RGBA8 a quarter as wide (4 plane bytes per texel) is universal. */
		/* CPU_READ_OFTEN is requested first on purpose: gralloc then gives a
		 * cached CPU mapping (write-combined memory is very slow for libjpeg's
		 * 8x8-block write pattern) and cleans the cache on unlock for the GPU. */
		struct { uint32_t format; uint64_t usage; uint32_t bpt; const char *name; } cands[] = {
			{ AHARDWAREBUFFER_FORMAT_R8_UNORM,
			  AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN
				| AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 1, "R8 (cached)" },
			{ AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
			  AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN
				| AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 4, "packed RGBA8 (cached)" },
			{ AHARDWAREBUFFER_FORMAT_R8_UNORM,
			  AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_CPU_READ_RARELY
				| AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 1, "R8" },
			{ AHARDWAREBUFFER_FORMAT_R8_UNORM,
			  AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
			  1, "R8 (write-only)" },
			{ AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
			  AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_CPU_READ_RARELY
				| AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 4, "packed RGBA8" },
			{ AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
			  AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
			  4, "packed RGBA8 (write-only)" },
		};
		mGpuPlanarEnabled = false;
		mGpuPlanarFormat = 0;
		mGpuPlanarUsage = 0;
		mGpuPlanarBytesPerTexel = 1;
		for (size_t c = 0; c < sizeof(cands) / sizeof(cands[0]); c++) {
			AHardwareBuffer_Desc probe;
			memset(&probe, 0, sizeof(probe));
			probe.width = 64;
			probe.height = 64;
			probe.layers = 1;
			probe.format = cands[c].format;
			probe.usage = cands[c].usage;
			/* AHardwareBuffer_isSupported() is API 29+; actually allocating a
			 * small buffer is the portable equivalent and answers the same
			 * question more honestly. This runs once at preview setup. */
			AHardwareBuffer *probe_buf = NULL;
			if (AHardwareBuffer_allocate(&probe, &probe_buf) == 0 && probe_buf) {
				AHardwareBuffer_release(probe_buf);
				mGpuPlanarEnabled = true;
				mGpuPlanarFormat = cands[c].format;
				mGpuPlanarUsage = cands[c].usage;
				mGpuPlanarBytesPerTexel = cands[c].bpt;
				LOGI("gpu-planar: zero-copy decode targets enabled using %s AHardwareBuffers",
					cands[c].name);
				break;
			}
		}
		if (!mGpuPlanarEnabled)
			LOGI("gpu-planar: no GPU-sampleable CPU-writable buffer format; using texture upload");
	}
	EXIT();
}

/* ---- zero-copy planar MJPEG targets ------------------------------------ */

void UVCPreview::gpu_planar_free_slot(GpuPlanarFrame *g)
{
	for (int i = 0; i < 3; i++) {
		if (g->ahb[i]) {
			AHardwareBuffer_release((AHardwareBuffer *)g->ahb[i]);
			g->ahb[i] = NULL;
		}
		g->ids[i] = 0;
		g->w[i] = g->h[i] = g->stride[i] = 0;
	}
	if (g->fence_fd >= 0) {
		close(g->fence_fd);
		g->fence_fd = -1;
	}
}

bool UVCPreview::gpu_planar_alloc_plane(GpuPlanarFrame *g, int i,
	uint32_t width, uint32_t height)
{
	static std::atomic<uint64_t> s_next_id{1};
	AHardwareBuffer_Desc desc;
	AHardwareBuffer *buf = NULL;
	const uint32_t bpt = mGpuPlanarBytesPerTexel;
	memset(&desc, 0, sizeof(desc));
	desc.width = (width + bpt - 1) / bpt;	/* texels; 4 plane bytes per RGBA texel */
	desc.height = height;
	desc.layers = 1;
	desc.format = mGpuPlanarFormat;
	desc.usage = mGpuPlanarUsage;
	if (AHardwareBuffer_allocate(&desc, &buf) != 0 || !buf)
		return false;
	AHardwareBuffer_describe(buf, &desc);
	g->ahb[i] = buf;
	g->ids[i] = s_next_id.fetch_add(1, std::memory_order_relaxed);
	g->w[i] = width;
	g->h[i] = height;
	g->stride[i] = desc.stride * bpt;	/* row pitch in bytes */
	return true;
}

uvc_frame_t *UVCPreview::gpu_planar_get(const uint32_t widths[3], const uint32_t heights[3])
{
	GpuPlanarFrame *g = NULL;
	pthread_mutex_lock(&pool_mutex);
	/* Round-robin so the slot handed out is the one the GPU finished with
	 * longest ago; its read fence is then almost always already signalled. */
	for (int n = 0; n < GPU_PLANAR_POOL_SZ; n++) {
		GpuPlanarFrame *c = &mGpuPlanar[(mGpuPlanarNext + n) % GPU_PLANAR_POOL_SZ];
		if (!c->in_use) {
			c->in_use = true;
			mGpuPlanarNext = (unsigned)((c - mGpuPlanar) + 1) % GPU_PLANAR_POOL_SZ;
			g = c;
			break;
		}
	}
	pthread_mutex_unlock(&pool_mutex);
	if (!g)
		return NULL;

	for (int i = 0; i < 3; i++) {
		if (!widths[i]) {
			if (g->ahb[i]) {
				AHardwareBuffer_release((AHardwareBuffer *)g->ahb[i]);
				g->ahb[i] = NULL;
				g->ids[i] = 0;
			}
			g->w[i] = g->h[i] = g->stride[i] = 0;
			continue;
		}
		if (g->ahb[i] && g->w[i] == widths[i] && g->h[i] == heights[i])
			continue;
		if (g->ahb[i]) {
			AHardwareBuffer_release((AHardwareBuffer *)g->ahb[i]);
			g->ahb[i] = NULL;
		}
		if (!gpu_planar_alloc_plane(g, i, widths[i], heights[i])) {
			LOGW("gpu-planar: AHardwareBuffer_allocate %ux%u R8 failed", widths[i], heights[i]);
			gpu_planar_put(&g->frame, -1);
			return NULL;
		}
	}
	return &g->frame;
}

void UVCPreview::gpu_planar_put(uvc_frame_t *frame, int fence_fd)
{
	if (!frame)
		return;
	GpuPlanarFrame *g = reinterpret_cast<GpuPlanarFrame *>(frame);	/* frame is first member */
	pthread_mutex_lock(&pool_mutex);
	if (g->fence_fd >= 0)
		close(g->fence_fd);
	g->fence_fd = fence_fd;
	g->in_use = false;
	pthread_mutex_unlock(&pool_mutex);
}

void UVCPreview::gpu_planar_release_all()
{
	pthread_mutex_lock(&pool_mutex);
	for (int i = 0; i < GPU_PLANAR_POOL_SZ; i++) {
		gpu_planar_free_slot(&mGpuPlanar[i]);
		mGpuPlanar[i].in_use = false;
	}
	pthread_mutex_unlock(&pool_mutex);
}

/* Decode `frame` straight into a GPU planar target and queue it for the
 * preview thread.  Returns true when the frame was handled (queued or
 * dropped as corrupt); false means the zero-copy path is unavailable and the
 * caller should fall back to the contiguous decode + texture upload. */
bool UVCPreview::decode_mjpeg_to_gpu_planar(uvc_frame_t *frame)
{
	uint32_t widths[3], heights[3];
	int subsamp = 0;
	if (uvc_mjpeg_planar_layout(frame, widths, heights, &subsamp) != UVC_SUCCESS)
		return false;	/* header problem: let the normal path report it */

	uvc_frame_t *out = gpu_planar_get(widths, heights);
	if (UNLIKELY(!out)) {
		if (!mGpuPlanarEnabled)
			return false;
		/* Pool exhausted (should not happen with latest-wins): drop this one. */
		processingPreviewQueueDropCount.fetch_add(1, std::memory_order_relaxed);
		return true;
	}
	GpuPlanarFrame *g = reinterpret_cast<GpuPlanarFrame *>(out);
	const int nplanes = widths[1] ? 3 : 1;
	unsigned char *planes[3] = { NULL, NULL, NULL };
	int strides[3] = { 0, 0, 0 };
	int locked = 0;
	bool ok = true;

	/* The first lock waits on the fence from this slot's last GPU read; the
	 * fence fd is consumed by the lock, so hand it over exactly once. */
	int fence = g->fence_fd;
	g->fence_fd = -1;
	for (int i = 0; i < nplanes; i++) {
		void *ptr = NULL;
		if (AHardwareBuffer_lock((AHardwareBuffer *)g->ahb[i],
				AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, i == 0 ? fence : -1,
				NULL, &ptr) != 0 || !ptr) {
			if (i == 0 && fence >= 0)
				close(fence);
			ok = false;
			break;
		}
		planes[i] = (unsigned char *)ptr;
		strides[i] = (int)g->stride[i];
		locked = i + 1;
	}

	uvc_error_t result = UVC_ERROR_OTHER;
	const uint64_t t_convert = processing_now_ns();
	if (ok)
		result = uvc_mjpeg2yuv_planes(frame, planes, strides);
	for (int i = 0; i < locked; i++)
		AHardwareBuffer_unlock((AHardwareBuffer *)g->ahb[i], NULL);
	if (ok)
		recordPreviewConversionTiming(processing_now_ns() - t_convert);

	if (!ok) {
		LOGW("gpu-planar: AHardwareBuffer_lock failed; falling back to texture upload");
		mGpuPlanarEnabled = false;
		gpu_planar_put(out, -1);
		return false;
	}
	if (result != UVC_SUCCESS) {
		/* Corrupt or undecodable: same policy as the upload path, frame dropped. */
		processingPreviewQueueDropCount.fetch_add(1, std::memory_order_relaxed);
		gpu_planar_put(out, -1);
		return true;
	}

	emitGpuPlanarFrame(out, frame, widths, heights, subsamp);
	return true;
}

void UVCPreview::emitGpuPlanarFrame(uvc_frame_t *out, const uvc_frame_t *frame,
	const uint32_t widths[3], const uint32_t heights[3], int subsamp)
{
	GpuPlanarFrame *g = reinterpret_cast<GpuPlanarFrame *>(out);
	out->width = frame->width;
	out->height = frame->height;
	out->frame_format = UVC_FRAME_FORMAT_MJPEG_YUV_PLANAR;
	out->data = NULL;
	out->data_bytes = 0;
	out->actual_bytes = 0;
	out->library_owns_data = 0;
	out->step = g->stride[0];
	out->sequence = frame->sequence;
	out->capture_time = frame->capture_time;
	out->arrival_monotonic_ns = frame->arrival_monotonic_ns;
	out->source = frame->source;
	out->yuv_subsampling = subsamp;
	out->integrity_sample_hash = 0;
	out->yuv_hardware_buffer_bytes_per_texel = mGpuPlanarBytesPerTexel;
	for (int i = 0; i < 3; i++) {
		out->yuv_plane_widths[i] = widths[i];
		out->yuv_plane_heights[i] = heights[i];
		out->yuv_plane_strides[i] = g->stride[i];
		out->yuv_plane_offsets[i] = 0;
		out->yuv_hardware_buffers[i] = widths[i] ? g->ahb[i] : NULL;
		out->yuv_hardware_buffer_ids[i] = widths[i] ? g->ids[i] : 0;
	}
	if (mMergedRender)
		presentPlanarFrame(out);
	else
		addPreviewFrame(out);
}

uvc_frame_t *UVCPreview::mjpeg_header_get() {
	uvc_frame_t *header = NULL;
	pthread_mutex_lock(&mjpeg_decode_mutex);
	for (int i = 0; i < MJPEG_HEADER_POOL_SZ; i++) {
		if (!mjpeg_header_used[i]) {
			mjpeg_header_used[i] = true;
			header = &mjpeg_header_slots[i];
			break;
		}
	}
	pthread_mutex_unlock(&mjpeg_decode_mutex);
	return header;
}

void UVCPreview::mjpeg_header_put_locked(uvc_frame_t *header) {
	if (!header)
		return;
	const ptrdiff_t i = header - mjpeg_header_slots;
	if (LIKELY(i >= 0 && i < MJPEG_HEADER_POOL_SZ)) {
		mjpeg_header_used[i] = false;
	} else {
		LOGW("mjpeg_header_put: foreign frame header %p", header);
	}
}

void UVCPreview::mjpeg_header_put(uvc_frame_t *header) {
	pthread_mutex_lock(&mjpeg_decode_mutex);
	mjpeg_header_put_locked(header);
	pthread_mutex_unlock(&mjpeg_decode_mutex);
}

UVCPreview::~UVCPreview() {

	ENTER();
	JavaVM *jvm = getVM();
	JNIEnv *jni_env = nullptr;
	bool attached_for_cleanup = false;
	if LIKELY(jvm) {
		const jint gotEnv = jvm->GetEnv(reinterpret_cast<void **>(&jni_env),
			JNI_VERSION_1_6);
		if UNLIKELY(gotEnv == JNI_EDETACHED) {
			if (jvm->AttachCurrentThread(&jni_env, nullptr) == 0)
				attached_for_cleanup = true;
			else
				jni_env = nullptr;
		} else if UNLIKELY(gotEnv != JNI_OK)
			jni_env = nullptr;
	}
	if LIKELY(jni_env) {
		pthread_mutex_lock(&capture_mutex);
		if LIKELY(mFrameCallbackObj) {
			jni_env->DeleteGlobalRef(mFrameCallbackObj);
			mFrameCallbackObj = nullptr;
			iframecallback_fields.onFrame = nullptr;
		}
		if LIKELY(mPreviewFrameCallbackObj) {
			jni_env->DeleteGlobalRef(mPreviewFrameCallbackObj);
			mPreviewFrameCallbackObj = nullptr;
			preview_iframecallback_fields.onFrame = nullptr;
		}
		pthread_mutex_unlock(&capture_mutex);
	} else if UNLIKELY(mFrameCallbackObj || mPreviewFrameCallbackObj)
		LOGW("UVCPreview::~UVCPreview: no JNIEnv for GlobalRef cleanup");
	if (attached_for_cleanup && jvm)
		jvm->DetachCurrentThread();

	if (mPreviewWindow)
		ANativeWindow_release(mPreviewWindow);
	mPreviewWindow = NULL;
	if (mCaptureWindow)
		ANativeWindow_release(mCaptureWindow);
	mCaptureWindow = NULL;
	if (mGpuPreviewRenderer) {
		mGpuPreviewRenderer->shutdown();
		delete mGpuPreviewRenderer;
		mGpuPreviewRenderer = NULL;
	}
	gpu_planar_release_all();
	if (mMjpegPreviewYuvFrame) {
		uvc_free_frame(mMjpegPreviewYuvFrame);
		mMjpegPreviewYuvFrame = NULL;
	}
	clearPreviewFrame();
	clearMjpegDecodeFrame();
	clearCaptureFrame();
	clear_pool();
	pthread_mutex_destroy(&preview_mutex);
	pthread_mutex_destroy(&preview_queue_mutex);
	pthread_cond_destroy(&preview_sync);
	pthread_mutex_destroy(&mjpeg_decode_mutex);
	pthread_cond_destroy(&mjpeg_decode_sync);
	pthread_mutex_destroy(&capture_mutex);
	pthread_cond_destroy(&capture_sync);
	pthread_mutex_destroy(&pool_mutex);
	EXIT();
}

/**
 * get uvc_frame_t from frame pool
 * if pool is empty, create new frame
 * this function does not confirm the frame size
 * and you may need to confirm the size
 */
uvc_frame_t *UVCPreview::get_frame(size_t data_bytes) {
	uvc_frame_t *frame = NULL;
	pthread_mutex_lock(&pool_mutex);
	{
		if (!mFramePool.isEmpty()) {
			frame = mFramePool.last();
		}
	}
	pthread_mutex_unlock(&pool_mutex);
	if UNLIKELY(!frame) {
		LOGW("allocate new frame");
		frame = uvc_allocate_frame(data_bytes);
	}
	return frame;
}

uvc_frame_t *UVCPreview::get_notification_frame() {
	uvc_frame_t *frame = NULL;
	pthread_mutex_lock(&pool_mutex);
	{
		if (!mNotificationFramePool.isEmpty()) {
			frame = mNotificationFramePool.last();
		}
	}
	pthread_mutex_unlock(&pool_mutex);
	if UNLIKELY(!frame) {
		frame = uvc_allocate_frame(1);
	}
	return frame;
}

void UVCPreview::recycle_frame(uvc_frame_t *frame) {
	if (UNLIKELY(gpu_planar_is(frame))) {
		gpu_planar_put(frame, -1);
		return;
	}
	if (UNLIKELY(frame && frame->data_bytes <= 1 && frame->library_owns_data)) {
		pthread_mutex_lock(&pool_mutex);
		if (LIKELY(mNotificationFramePool.size() < FRAME_POOL_SZ)) {
			mNotificationFramePool.put(frame);
			frame = NULL;
		}
		pthread_mutex_unlock(&pool_mutex);
		if (UNLIKELY(frame)) {
			uvc_free_frame(frame);
		}
		return;
	}
	pthread_mutex_lock(&pool_mutex);
	if (LIKELY(mFramePool.size() < FRAME_POOL_SZ)) {
		mFramePool.put(frame);
		frame = NULL;
	}
	pthread_mutex_unlock(&pool_mutex);
	if (UNLIKELY(frame)) {
		uvc_free_frame(frame);
	}
}


void UVCPreview::init_pool(size_t data_bytes) {
	ENTER();

	clear_pool();
	pthread_mutex_lock(&pool_mutex);
	{
		for (int i = 0; i < FRAME_POOL_SZ; i++) {
			mFramePool.put(uvc_allocate_frame(data_bytes));
		}
	}
	pthread_mutex_unlock(&pool_mutex);

	EXIT();
}

void UVCPreview::clear_pool() {
	ENTER();

	pthread_mutex_lock(&pool_mutex);
	{
		const int n = mFramePool.size();
		for (int i = 0; i < n; i++) {
			uvc_free_frame(mFramePool[i]);
		}
		mFramePool.clear();
		const int notification_n = mNotificationFramePool.size();
		for (int i = 0; i < notification_n; i++) {
			uvc_free_frame(mNotificationFramePool[i]);
		}
		mNotificationFramePool.clear();
	}
	pthread_mutex_unlock(&pool_mutex);
	EXIT();
}

inline const bool UVCPreview::isRunning() const {return mIsRunning; }

void UVCPreview::recordPreviewConversionTiming(uint64_t duration_ns) {
	processingPreviewConvertCount.fetch_add(1, std::memory_order_relaxed);
	processingPreviewConvertTotalNs.fetch_add(duration_ns, std::memory_order_relaxed);
	atomic_store_max(processingPreviewConvertMaxNs, duration_ns);
}

void UVCPreview::recordCallbackConversionTiming(uint64_t duration_ns) {
	processingCallbackConvertCount.fetch_add(1, std::memory_order_relaxed);
	processingCallbackConvertTotalNs.fetch_add(duration_ns, std::memory_order_relaxed);
	atomic_store_max(processingCallbackConvertMaxNs, duration_ns);
}

void UVCPreview::recordSurfaceCopyTiming(uint64_t duration_ns) {
	processingCopyCount.fetch_add(1, std::memory_order_relaxed);
	processingCopyTotalNs.fetch_add(duration_ns, std::memory_order_relaxed);
	atomic_store_max(processingCopyMaxNs, duration_ns);
}

void UVCPreview::recordEndToEndLatencyDuration(uint64_t duration_ns) {
	if (!duration_ns)
		return;
	processingEndToEndLatencyCount.fetch_add(1, std::memory_order_relaxed);
	processingEndToEndLatencyTotalNs.fetch_add(duration_ns, std::memory_order_relaxed);
	atomic_store_max(processingEndToEndLatencyMaxNs, duration_ns);
}

void UVCPreview::recordEndToEndLatencyTiming(uint64_t start_ns, uint64_t end_ns) {
	if (!start_ns || end_ns <= start_ns)
		return;
	recordEndToEndLatencyDuration(end_ns - start_ns);
}

void UVCPreview::recordPayloadBytes(size_t bytes) {
	processingPayloadCount.fetch_add(1, std::memory_order_relaxed);
	processingPayloadTotalBytes.fetch_add(bytes, std::memory_order_relaxed);
	atomic_store_max(processingPayloadMaxBytes, bytes);
}

void UVCPreview::recordMjpegDecodedVisualSample(uint32_t sequence, size_t bytes,
		const uint8_t *rgbx, size_t stride_bytes, uint32_t width, uint32_t height) {
#if !UVC_RUNTIME_DIAG_ENABLED
	(void)sequence;
	(void)bytes;
	(void)rgbx;
	(void)stride_bytes;
	(void)width;
	(void)height;
	return;
#else
	/* Sample 8 rows × 8 columns for the whole-frame average luma, and
	 * additionally record per-row luma at 8 evenly-spaced horizontal bands
	 * (10%, 20%, …, 90% of frame height).  The per-row log fires whenever
	 * the whole-frame luma delta is large (≥30) so we can pinpoint which
	 * bands are flickering without drowning logcat in normal operation. */
	uint64_t luma_total = 0;
	uint32_t samples = 0;
	uint32_t luma_avg;
	uint32_t prior;
	uint32_t delta;
	uint32_t y;
	/* per-row band lumas; 0 = not computed */
	uint32_t band_luma[8] = {};

	if (UNLIKELY(!rgbx || !width || !height || stride_bytes < (size_t)width * PREVIEW_PIXEL_BYTES))
		return;

	for (y = 0; y < 8; y++) {
		const uint32_t py = (height * (y * 2 + 1)) / 16;
		uint32_t x;
		const uint8_t *row = rgbx + (size_t)py * stride_bytes;
		uint64_t band_total = 0;
		for (x = 0; x < 8; x++) {
			const uint32_t px = (width * (x * 2 + 1)) / 16;
			const uint8_t *p = row + (size_t)px * PREVIEW_PIXEL_BYTES;
			const uint32_t pix_luma = (uint32_t)p[0] * 77u + (uint32_t)p[1] * 150u
				+ (uint32_t)p[2] * 29u;
			luma_total += pix_luma;
			band_total += pix_luma;
			samples++;
		}
		band_luma[y] = (uint32_t)(band_total / (8u * 256u));
	}
	if (!samples)
		return;

	luma_avg = (uint32_t)(luma_total / (samples * 256u));
	const uint32_t decoded =
		diagMjpegDecodedCount.fetch_add(1, std::memory_order_relaxed) + 1;
	prior = diagMjpegLastLuma.load(std::memory_order_relaxed);
	delta = luma_avg > prior ? luma_avg - prior : prior - luma_avg;
	if (delta >= 10 || decoded <= 20 || !(decoded % 120)) {
		UVC_DIAG_LOGI("mjpeg-diag:decoded count=%u seq=%u bytes=%zu luma=%u last_luma=%u delta=%u frame=%ux%u",
			decoded,
			sequence,
			bytes,
			luma_avg,
			prior,
			delta,
			width,
			height);
	}
	/* Per-band breakdown when luma swings are large enough to indicate banding. */
	if (delta >= 30) {
		UVC_DIAG_LOGI("mjpeg-diag:bands count=%u seq=%u "
			"b0=%u b1=%u b2=%u b3=%u b4=%u b5=%u b6=%u b7=%u",
			decoded,
			sequence,
			band_luma[0], band_luma[1], band_luma[2], band_luma[3],
			band_luma[4], band_luma[5], band_luma[6], band_luma[7]);
	}
	if ((band_luma[5] >= 240 && band_luma[6] >= 240 && band_luma[7] >= 240)
			|| (band_luma[5] + 80 < band_luma[0] && band_luma[6] + 80 < band_luma[0]
				&& band_luma[7] + 80 < band_luma[0])) {
		UVC_DIAG_LOGI("mjpeg-diag:band-anomaly count=%u seq=%u bytes=%zu rgbx=%p "
			"stride=%zu bands=%u,%u,%u,%u,%u,%u,%u,%u",
			decoded,
			sequence,
			bytes,
			rgbx,
			stride_bytes,
			band_luma[0], band_luma[1], band_luma[2], band_luma[3],
			band_luma[4], band_luma[5], band_luma[6], band_luma[7]);
	}
	diagMjpegLastLuma.store(luma_avg, std::memory_order_relaxed);
#endif
}

void UVCPreview::recordPreviewQueueDepthSample(uint64_t depth_frames) {
	const uint64_t depth_milli = depth_frames * 1000ULL;
	processingPreviewQueueDepthSampleCount.fetch_add(1, std::memory_order_relaxed);
	processingPreviewQueueDepthTotalMilli.fetch_add(depth_milli, std::memory_order_relaxed);
	atomic_store_max(processingPreviewQueueDepthMaxMilli, depth_milli);
}

static inline void recordPreviewEnqueueDepthSample(
	std::atomic<uint64_t> *sample_count,
	std::atomic<uint64_t> *total_milli,
	std::atomic<uint64_t> *max_milli,
	uint64_t depth_frames) {
	const uint64_t depth_milli = depth_frames * 1000ULL;
	sample_count->fetch_add(1, std::memory_order_relaxed);
	total_milli->fetch_add(depth_milli, std::memory_order_relaxed);
	atomic_store_max(*max_milli, depth_milli);
}

void UVCPreview::getAndResetProcessingStats(uint64_t stats[UVC_PROCESSING_STATS_COUNT]) {
	uint32_t stream_interval_100ns = 0;
	int stream_altsetting = -1;
	uint8_t stream_is_isochronous = 0;
	uint32_t stream_published_count = 0;
	uint32_t stream_dropped_before_cb_count = 0;
	(void)uvc_get_stream_runtime_diag(
		mDeviceHandle,
		&stream_interval_100ns,
		&stream_altsetting,
		&stream_is_isochronous,
		&stream_published_count,
		&stream_dropped_before_cb_count);
	/* Atomically snapshot+reset each field (exchange to 0). The window is
	 * eventually consistent across fields rather than locked-coherent, which is
	 * acceptable for periodic diagnostics. */
	const auto take = [](std::atomic<uint64_t> &field) {
		return field.exchange(0, std::memory_order_relaxed);
	};
	const uint64_t previewConvertCount = take(processingPreviewConvertCount);
	const uint64_t previewConvertTotalNs = take(processingPreviewConvertTotalNs);
	const uint64_t previewConvertMaxNs = take(processingPreviewConvertMaxNs);
	const uint64_t callbackConvertCount = take(processingCallbackConvertCount);
	const uint64_t callbackConvertTotalNs = take(processingCallbackConvertTotalNs);
	const uint64_t callbackConvertMaxNs = take(processingCallbackConvertMaxNs);
	const uint64_t copyCount = take(processingCopyCount);
	const uint64_t copyTotalNs = take(processingCopyTotalNs);
	const uint64_t copyMaxNs = take(processingCopyMaxNs);
	const uint64_t endToEndCount = take(processingEndToEndLatencyCount);
	const uint64_t endToEndTotalNs = take(processingEndToEndLatencyTotalNs);
	const uint64_t endToEndMaxNs = take(processingEndToEndLatencyMaxNs);
	const uint64_t payloadCount = take(processingPayloadCount);
	const uint64_t payloadTotalBytes = take(processingPayloadTotalBytes);
	const uint64_t payloadMaxBytes = take(processingPayloadMaxBytes);
	const uint64_t previewQueueDropCount = take(processingPreviewQueueDropCount);
	const uint64_t previewQueueDepthSampleCount = take(processingPreviewQueueDepthSampleCount);
	const uint64_t previewQueueDepthTotalMilli = take(processingPreviewQueueDepthTotalMilli);
	const uint64_t previewQueueDepthMaxMilli = take(processingPreviewQueueDepthMaxMilli);
	const uint64_t previewEnqueueDepthSampleCount = take(processingPreviewEnqueueDepthSampleCount);
	const uint64_t previewEnqueueDepthTotalMilli = take(processingPreviewEnqueueDepthTotalMilli);
	const uint64_t previewEnqueueDepthMaxMilli = take(processingPreviewEnqueueDepthMaxMilli);
	const uint64_t uvcCallbackCount = take(processingUvcCallbackCount);
	const uint64_t uvcCallbackTotalNs = take(processingUvcCallbackTotalNs);
	const uint64_t uvcCallbackMaxNs = take(processingUvcCallbackMaxNs);
	const uint64_t callbackLagCount = take(processingCallbackLagCount);
	const uint64_t callbackLagTotalNs = take(processingCallbackLagTotalNs);
	const uint64_t callbackLagMaxNs = take(processingCallbackLagMaxNs);
	const uint64_t preCallbackSkippedFrames = take(processingPreCallbackSkippedFrames);
	processingUvcSeqState.store(0, std::memory_order_relaxed);

	stats[0] = previewConvertCount;
	stats[1] = endToEndCount ? endToEndTotalNs / endToEndCount : 0;
	stats[2] = previewConvertMaxNs;
	stats[3] = callbackConvertCount;
	stats[4] = callbackConvertCount ? callbackConvertTotalNs / callbackConvertCount : 0;
	stats[5] = callbackConvertMaxNs;
	stats[6] = copyCount;
	stats[7] = copyCount ? copyTotalNs / copyCount : 0;
	stats[8] = copyMaxNs;
	stats[9] = payloadCount;
	stats[10] = payloadCount ? payloadTotalBytes / payloadCount : 0;
	stats[11] = payloadMaxBytes;
	stats[12] = previewQueueDropCount;
	stats[13] = previewQueueDepthSampleCount
		? previewQueueDepthTotalMilli / previewQueueDepthSampleCount : 0;
	stats[14] = previewConvertCount ? previewConvertTotalNs / previewConvertCount : 0;
	stats[15] = endToEndMaxNs;
	stats[16] = previewQueueDepthMaxMilli;
	stats[17] = previewEnqueueDepthSampleCount
		? previewEnqueueDepthTotalMilli / previewEnqueueDepthSampleCount : 0;
	stats[18] = previewEnqueueDepthMaxMilli;
	stats[19] = uvcCallbackCount ? uvcCallbackTotalNs / uvcCallbackCount : 0;
	stats[20] = uvcCallbackMaxNs;
	stats[21] = callbackLagCount ? callbackLagTotalNs / callbackLagCount : 0;
	stats[22] = callbackLagMaxNs;
	stats[23] = callbackLagCount;
	stats[24] = preCallbackSkippedFrames;
	stats[25] = stream_interval_100ns;
	stats[26] = stream_altsetting >= 0 ? (uint64_t)stream_altsetting : 0;
	stats[27] = stream_published_count;
	stats[28] = stream_dropped_before_cb_count;
	stats[29] = stream_is_isochronous ? 1 : 0;
}

int UVCPreview::setPreviewSize(int width, int height, int min_fps, int max_fps, int mode, float bandwidth) {
	ENTER();
	
	int result = 0;
	/* Serenegiant originally only compared wxh+mode. Frame-rate-only changes (same resolution,
	 * switch 60↔30 fps) must still run uvc_get_stream_ctrl_format_size_fps + probe/commit,
	 * otherwise libuvc keeps the previous interval → black preview until USB power-cycle. */
	if ((requestWidth != width) || (requestHeight != height) || (requestMode != mode) ||
		(requestMinFps != min_fps) || (requestMaxFps != max_fps) ||
		(requestBandwidth != bandwidth)) {
		requestWidth = width;
		requestHeight = height;
		requestMinFps = min_fps;
		requestMaxFps = max_fps;
		requestMode = mode;
		requestBandwidth = bandwidth;

		uvc_stream_ctrl_t ctrl;
		result = uvc_get_stream_ctrl_format_size_fps(mDeviceHandle, &ctrl,
			request_mode_to_frame_format(requestMode),
			requestWidth, requestHeight, requestMinFps, requestMaxFps);
	}
	
	RETURN(result, int);
}

int UVCPreview::setPreviewDisplay(ANativeWindow *preview_window) {
	ENTER();
	pthread_mutex_lock(&preview_mutex);
	{
		if (mPreviewWindow != preview_window) {
			if (mPreviewWindow)
				ANativeWindow_release(mPreviewWindow);
			if (mGpuPreviewRenderer)
				mGpuPreviewRenderer->resetSurface();
			mPreviewWindow = preview_window;
			if (LIKELY(mPreviewWindow)) {
				ANativeWindow_setBuffersGeometry(mPreviewWindow,
					frameWidth, frameHeight, previewFormat);
				mPreviewGeomWidth = frameWidth;
				mPreviewGeomHeight = frameHeight;
			}
		} else if (preview_window) {
			/* JNI calls ANativeWindow_fromSurface each time; if the pointer matches the
			 * existing window we must release this duplicate ref or BufferQueue state drifts. */
			ANativeWindow_release(preview_window);
		}
	}
	pthread_mutex_unlock(&preview_mutex);
	RETURN(0, int);
}

int UVCPreview::setPreviewTransform(int rotation_degrees, bool flip_h, bool flip_v,
	float scale, float pan_x_ndc, float pan_y_ndc, float fit_x, float fit_y) {
	/* scale (incl. mirror) -> rotate -> fit -> translate.  Rotation happens in the
	 * NDC of the fit-to-screen content box (which has the rotated aspect), the fit
	 * step maps that box into the full surface, and the pan is in surface NDC.
	 * NDC has y up while the screen has y down, so a clockwise on-screen rotation
	 * by r is (x, y) -> (x cos r + y sin r, -x sin r + y cos r). */
	if (!(fit_x > 0.0f) || fit_x > 1.0f)
		fit_x = 1.0f;
	if (!(fit_y > 0.0f) || fit_y > 1.0f)
		fit_y = 1.0f;
	const double r = rotation_degrees * M_PI / 180.0;
	const float c = (float)cos(r);
	const float sn = (float)sin(r);
	const float sx = scale * (flip_h ? -1.0f : 1.0f);
	const float sy = scale * (flip_v ? -1.0f : 1.0f);
	/* M = T * F * R * S, column-major for glUniformMatrix3fv (F scales rows). */
	float m[9];
	m[0] = fit_x * c * sx;   m[1] = -fit_y * sn * sx;  m[2] = 0.0f;	/* column 0 */
	m[3] = fit_x * sn * sy;  m[4] = fit_y * c * sy;    m[5] = 0.0f;	/* column 1 */
	m[6] = pan_x_ndc;        m[7] = pan_y_ndc;         m[8] = 1.0f;	/* column 2 */
	pthread_mutex_lock(&preview_mutex);
	memcpy(mPreviewXform, m, sizeof(mPreviewXform));
	mPreviewFitX = fit_x;
	mPreviewFitY = fit_y;
	/* Kotlin re-sends the transform whenever the view's size changes. */
	mPreviewSurfaceSizeDirty = true;
	pthread_mutex_unlock(&preview_mutex);
	return 0;
}

int UVCPreview::setFrameCallback(JNIEnv *env, jobject frame_callback_obj, int pixel_format) {
	
	ENTER();
	pthread_mutex_lock(&capture_mutex);
	{
		if (isRunning() && isCapturing()) {
			mIsCapturing = false;
			if (mFrameCallbackObj) {
				pthread_cond_signal(&capture_sync);
				pthread_cond_wait(&capture_sync, &capture_mutex);	// wait finishing capturing
			}
		}
		if (!env->IsSameObject(mFrameCallbackObj, frame_callback_obj))	{
			iframecallback_fields.onFrame = NULL;
			if (mFrameCallbackObj) {
				env->DeleteGlobalRef(mFrameCallbackObj);
			}
			mFrameCallbackObj = frame_callback_obj;
			if (frame_callback_obj) {
				iframecallback_fields.onFrame =
					consolation_resolve_iframe_on_frame_mid(env, frame_callback_obj);
				env->ExceptionClear();
				if (!iframecallback_fields.onFrame) {
					LOGE("Can't find IFrameCallback#onFrame");
					env->DeleteGlobalRef(frame_callback_obj);
					mFrameCallbackObj = frame_callback_obj = NULL;
				}
			}
		}
		if (frame_callback_obj) {
			mPixelFormat = pixel_format;
			callbackPixelFormatChanged();
		}
		capture_frame_callback_enabled = mFrameCallbackObj != NULL;
	}
	pthread_mutex_unlock(&capture_mutex);
	RETURN(0, int);
}

int UVCPreview::setPreviewFrameCallback(JNIEnv *env, jobject frame_callback_obj, int pixel_format) {
	ENTER();
	pthread_mutex_lock(&capture_mutex);
	{
		if (!env->IsSameObject(mPreviewFrameCallbackObj, frame_callback_obj)) {
			preview_iframecallback_fields.onFrame = NULL;
			if (mPreviewFrameCallbackObj) {
				env->DeleteGlobalRef(mPreviewFrameCallbackObj);
			}
			mPreviewFrameCallbackObj = frame_callback_obj;
			if (frame_callback_obj) {
				preview_iframecallback_fields.onFrame =
					consolation_resolve_iframe_on_frame_mid(env, frame_callback_obj);
				env->ExceptionClear();
				if (!preview_iframecallback_fields.onFrame) {
					LOGE("Can't find preview IFrameCallback#onFrame");
					env->DeleteGlobalRef(frame_callback_obj);
					mPreviewFrameCallbackObj = frame_callback_obj = NULL;
				}
			}
		}
		mPreviewPixelFormat = pixel_format;
		const size_t sz = requestWidth * requestHeight;
		switch (mPreviewPixelFormat) {
		case PIXEL_FORMAT_RGBX:
			previewCallbackPixelBytes = sz * 4;
			break;
		case PIXEL_FORMAT_YUV20SP:
		case PIXEL_FORMAT_NV21:
			previewCallbackPixelBytes = (sz * 3) / 2;
			break;
		case PIXEL_FORMAT_RAW:
			previewCallbackPixelBytes = 0;
			break;
		case PIXEL_FORMAT_YUV:
		case PIXEL_FORMAT_RGB565:
		default:
			previewCallbackPixelBytes = sz * 2;
			break;
		}
		preview_frame_callback_enabled = mPreviewFrameCallbackObj != NULL;
	}
	pthread_mutex_unlock(&capture_mutex);
	RETURN(0, int);
}

void UVCPreview::callbackPixelFormatChanged() {
	mFrameCallbackFunc = NULL;
	const size_t sz = requestWidth * requestHeight;
	switch (mPixelFormat) {
	  case PIXEL_FORMAT_RAW:
		LOGI("PIXEL_FORMAT_RAW:");
		callbackPixelBytes = sz * 2;
		break;
	  case PIXEL_FORMAT_YUV:
		LOGI("PIXEL_FORMAT_YUV:");
		callbackPixelBytes = sz * 2;
		break;
	  case PIXEL_FORMAT_RGB565:
		LOGI("PIXEL_FORMAT_RGB565:");
		mFrameCallbackFunc = uvc_any2rgb565;
		callbackPixelBytes = sz * 2;
		break;
	  case PIXEL_FORMAT_RGBX:
		LOGI("PIXEL_FORMAT_RGBX:");
		mFrameCallbackFunc = uvc_any2rgbx;
		callbackPixelBytes = sz * 4;
		break;
	  case PIXEL_FORMAT_YUV20SP:
		LOGI("PIXEL_FORMAT_YUV20SP:");
		mFrameCallbackFunc = uvc_yuyv2iyuv420SP;
		callbackPixelBytes = (sz * 3) / 2;
		break;
	  case PIXEL_FORMAT_NV21:
		LOGI("PIXEL_FORMAT_NV21:");
		mFrameCallbackFunc = uvc_yuyv2yuv420SP;
		callbackPixelBytes = (sz * 3) / 2;
		break;
	}
}

bool UVCPreview::hasCaptureConsumers() const {
	return capture_thread_joinable || capture_frame_callback_enabled;
}

bool UVCPreview::hasPreviewFrameCallback() const {
	return preview_frame_callback_enabled;
}

void UVCPreview::clearDisplay() {
	ENTER();

	ANativeWindow_Buffer buffer;
	pthread_mutex_lock(&capture_mutex);
	{
		if (LIKELY(mCaptureWindow)) {
			if (LIKELY(ANativeWindow_lock(mCaptureWindow, &buffer, NULL) == 0)) {
				uint8_t *dest = (uint8_t *)buffer.bits;
				const size_t bytes = buffer.width * PREVIEW_PIXEL_BYTES;
				const int stride = buffer.stride * PREVIEW_PIXEL_BYTES;
				for (int i = 0; i < buffer.height; i++) {
					memset(dest, 0, bytes);
					dest += stride;
				}
				ANativeWindow_unlockAndPost(mCaptureWindow);
			}
		}
	}
	pthread_mutex_unlock(&capture_mutex);
	pthread_mutex_lock(&preview_mutex);
	{
		if (LIKELY(mPreviewWindow)) {
			if (LIKELY(ANativeWindow_lock(mPreviewWindow, &buffer, NULL) == 0)) {
				uint8_t *dest = (uint8_t *)buffer.bits;
				const size_t bytes = buffer.width * PREVIEW_PIXEL_BYTES;
				const int stride = buffer.stride * PREVIEW_PIXEL_BYTES;
				for (int i = 0; i < buffer.height; i++) {
					memset(dest, 0, bytes);
					dest += stride;
				}
				ANativeWindow_unlockAndPost(mPreviewWindow);
			}
		}
	}
	pthread_mutex_unlock(&preview_mutex);

	EXIT();
}

int UVCPreview::startPreview() {
	ENTER();

	int result = EXIT_FAILURE;
	if (!isRunning()) {
		mIsRunning = true;
		pthread_mutex_lock(&preview_mutex);
		{
			if (LIKELY(mPreviewWindow)) {
				result = pthread_create(&preview_thread, NULL, preview_thread_func, (void *)this);
			}
		}
		pthread_mutex_unlock(&preview_mutex);
		if (UNLIKELY(result != EXIT_SUCCESS)) {
			LOGW("UVCCamera::window does not exist/already running/could not create thread etc.");
			mIsRunning = false;
			pthread_mutex_lock(&preview_queue_mutex);
			{
				pthread_cond_signal(&preview_sync);
			}
			pthread_mutex_unlock(&preview_queue_mutex);
		}
	}
	RETURN(result, int);
}

int UVCPreview::stopPreview() {
	ENTER();
	bool b = isRunning();
	if (LIKELY(b)) {
		mIsRunning = false;
		/* Signal under the queue lock so a waiter that has checked isRunning()
		 * but not yet entered cond_wait cannot miss this wakeup. */
		pthread_mutex_lock(&preview_queue_mutex);
		pthread_cond_signal(&preview_sync);
		pthread_mutex_unlock(&preview_queue_mutex);
		pthread_mutex_lock(&mjpeg_decode_mutex);
		pthread_cond_signal(&mjpeg_decode_sync);
		pthread_mutex_unlock(&mjpeg_decode_mutex);
		if (capture_thread_joinable)
			pthread_cond_signal(&capture_sync);
		if (capture_thread_joinable) {
			if (pthread_join(capture_thread, NULL) != EXIT_SUCCESS) {
				LOGW("UVCPreview::terminate capture thread: pthread_join failed");
			}
			capture_thread_joinable = false;
		}
		if (pthread_join(preview_thread, NULL) != EXIT_SUCCESS) {
			LOGW("UVCPreview::terminate preview thread: pthread_join failed");
		}
		clearDisplay();
	}
	clearPreviewFrame();
	clearCaptureFrame();
	pthread_mutex_lock(&preview_mutex);
	if (mPreviewWindow) {
		if (mGpuPreviewRenderer)
			mGpuPreviewRenderer->resetSurface();
		ANativeWindow_release(mPreviewWindow);
		mPreviewWindow = NULL;
	}
	pthread_mutex_unlock(&preview_mutex);
	pthread_mutex_lock(&capture_mutex);
	if (mCaptureWindow) {
		ANativeWindow_release(mCaptureWindow);
		mCaptureWindow = NULL;
	}
	pthread_mutex_unlock(&capture_mutex);
	RETURN(0, int);
}

//**********************************************************************
//
//**********************************************************************
int copyToSurface(uvc_frame_t *frame, ANativeWindow **window,
	uint64_t *frame_ready_ns = NULL, uint64_t *surface_wait_ns = NULL);

bool UVCPreview::admitFrame(uvc_frame_t *frame) {
	{
		const uint64_t seq_state =
			processingUvcSeqState.load(std::memory_order_relaxed);
		if (seq_state & UVC_SEQ_STATE_VALID_BIT) {
			const uint32_t last_seq = (uint32_t)(seq_state & 0xffffffffULL);
			if (frame->sequence > last_seq + 1U) {
				processingPreCallbackSkippedFrames.fetch_add(
					(uint64_t)(frame->sequence - last_seq - 1U),
					std::memory_order_relaxed);
			}
		}
		processingUvcSeqState.store(
			UVC_SEQ_STATE_VALID_BIT | (uint64_t)frame->sequence,
			std::memory_order_relaxed);
	}
	if (LIKELY(frame->arrival_monotonic_ns)) {
		const uint64_t now_ns = processing_now_ns();
		if (LIKELY(now_ns > frame->arrival_monotonic_ns)) {
			const uint64_t lag_ns = now_ns - frame->arrival_monotonic_ns;
			processingCallbackLagCount.fetch_add(1, std::memory_order_relaxed);
			processingCallbackLagTotalNs.fetch_add(lag_ns, std::memory_order_relaxed);
			atomic_store_max(processingCallbackLagMaxNs, lag_ns);
		}
	}
	if (!firstFrameLogged) {
		firstFrameLogged = true;
		const uint64_t t0 = streamingStartMonotonicNs;
		if (t0 > 0) {
			const uint64_t elapsed_ms = (processing_now_ns() - t0) / 1000000ULL;
			LOGI("startup-diag:first frame received after %llu ms format=%d size=%ux%u bytes=%zu",
				(unsigned long long)elapsed_ms,
				frame->frame_format,
				frame->width,
				frame->height,
				frame->actual_bytes);
		}
	}
	if (UNLIKELY(
		((frame->frame_format != UVC_FRAME_FORMAT_MJPEG) && (frame->actual_bytes < frameBytes))
		|| (frame->width != frameWidth) || (frame->height != frameHeight) )) {

#if LOCAL_DEBUG
		LOGD("broken frame!:format=%d,actual_bytes=%d/%d(%d,%d/%d,%d)",
			frame->frame_format, frame->actual_bytes, frameBytes,
			frame->width, frame->height, frameWidth, frameHeight);
#endif
		return false;
	}
	return true;
}

bool UVCPreview::mjpegFastPathActive() const {
	return frameMode != REQUEST_MODE_H264
		&& mjpeg_decode_thread_joinable
		&& !hasPreviewFrameCallback()
		&& !hasCaptureConsumers();
}

/* Runs on the USB event thread, at the instant libuvc publishes a frame.  For
 * the MJPEG fast path the regular callback only queues the frame for the
 * decoder, so doing that here saves the wake-up of libuvc's callback thread.
 * Anything else is declined (0) and takes the regular callback. */
int UVCPreview::uvc_preview_publish_hook(uvc_frame_t *frame, void *vptr_args) {
	UVCPreview *preview = reinterpret_cast<UVCPreview *>(vptr_args);
	if (UNLIKELY(!preview || !frame))
		return 0;
	if (UNLIKELY(!preview->mUsbThreadTid.load(std::memory_order_relaxed)))
		preview->mUsbThreadTid.store(gettid(), std::memory_order_relaxed);
	if (UNLIKELY(!preview->isRunning() || frame->frame_format != UVC_FRAME_FORMAT_MJPEG
			|| !frame->data || !frame->data_bytes || !preview->mjpegFastPathActive()))
		return 0;

	const uint64_t start_ns = processing_now_ns();
	if (LIKELY(preview->admitFrame(frame))) {
		preview->recordPayloadBytes(frame->actual_bytes);
		preview->addMjpegDecodeFrame(frame);
	}
	const uint64_t elapsed_ns = processing_now_ns() - start_ns;
	preview->processingUvcCallbackCount.fetch_add(1, std::memory_order_relaxed);
	preview->processingUvcCallbackTotalNs.fetch_add(elapsed_ns, std::memory_order_relaxed);
	atomic_store_max(preview->processingUvcCallbackMaxNs, elapsed_ns);
	return 1;
}

/* Runs on the USB event thread: note how far the frame under assembly has got
 * and let the decode thread follow. */
void UVCPreview::uvc_preview_assembly_hook(const uvc_assembly_event_t *event, void *vptr_args) {
	UVCPreview *preview = reinterpret_cast<UVCPreview *>(vptr_args);
	if (UNLIKELY(!preview || !event || !preview->mStreamDecodeEnabled
			|| !preview->isRunning() || !preview->mjpegFastPathActive()))
		return;

	pthread_mutex_lock(&preview->mjpeg_decode_mutex);
	if (event->type == UVC_ASSEMBLY_PROGRESS) {
		preview->mLive.gen = event->generation;
		preview->mLive.data = event->data;
		preview->mLive.bytes = event->bytes;
		preview->mLive.complete = event->complete != 0;
	} else {
		preview->mLive.discarded_gen = event->generation;
	}
	preview->mLive.dirty = true;
	pthread_cond_signal(&preview->mjpeg_decode_sync);
	pthread_mutex_unlock(&preview->mjpeg_decode_mutex);
}

void UVCPreview::uvc_preview_frame_callback(uvc_frame_t *frame, void *vptr_args) {
	UVCPreview *preview = reinterpret_cast<UVCPreview *>(vptr_args);
	struct UvcCallbackTimingScope final {
		UVCPreview *preview;
		uint64_t start_ns;
		~UvcCallbackTimingScope() {
			if (!preview || !start_ns)
				return;
			const uint64_t elapsed_ns = processing_now_ns() - start_ns;
			preview->processingUvcCallbackCount.fetch_add(1, std::memory_order_relaxed);
			preview->processingUvcCallbackTotalNs.fetch_add(elapsed_ns, std::memory_order_relaxed);
			atomic_store_max(preview->processingUvcCallbackMaxNs, elapsed_ns);
		}
	} callback_timing_scope = { preview, processing_now_ns() };
	if UNLIKELY(!preview->isRunning() || !frame || !frame->frame_format || !frame->data || !frame->data_bytes) return;
	if (UNLIKELY(!preview->admitFrame(frame)))
		return;
	if (LIKELY(preview->isRunning())) {
		preview->recordPayloadBytes(frame->actual_bytes);

		if (preview->frameMode != REQUEST_MODE_H264) {
			const bool has_preview_callback = preview->hasPreviewFrameCallback();
			const bool has_capture_consumers = preview->hasCaptureConsumers();
			uint64_t preview_ready_ns = 0;
			uint64_t surface_wait_ns = 0;
			bool preview_rendered = false;

			if (frame->frame_format == UVC_FRAME_FORMAT_MJPEG
					&& !has_preview_callback
					&& !has_capture_consumers
					&& preview->mjpeg_decode_thread_joinable) {
				preview->addMjpegDecodeFrame(frame);
				return;
			}

			if (UNLIKELY(preview->capture_thread_joinable)) {
				preview_rendered = preview->renderFrameDirectToSurface(frame,
					&preview->mPreviewWindow, &preview->preview_mutex,
					&preview_ready_ns, &surface_wait_ns);
				if (preview_rendered && preview_ready_ns
						&& frame->arrival_monotonic_ns)
					preview->recordEndToEndLatencyTiming(frame->arrival_monotonic_ns,
						preview_ready_ns);

				uvc_frame_t *rgbx = preview->convertPreviewFrameToRgbx(frame);
				if (LIKELY(rgbx)) {
					if (!preview_rendered) {
						pthread_mutex_lock(&preview->preview_mutex);
						const uint64_t t_copy = processing_now_ns();
						if (copyToSurface(rgbx, &preview->mPreviewWindow,
								&preview_ready_ns, &surface_wait_ns) == 0) {
							const uint64_t t_end = preview_ready_ns ? preview_ready_ns : processing_now_ns();
							preview->recordSurfaceCopyTiming(t_end - t_copy);
							if (frame->arrival_monotonic_ns)
								preview->recordEndToEndLatencyTiming(
									frame->arrival_monotonic_ns, t_end);
						}
						pthread_mutex_unlock(&preview->preview_mutex);
					}

					if (preview->capture_thread_joinable) {
						preview->addCaptureFrame(rgbx);
					} else {
						preview->addPreviewFrame(rgbx);
					}
					return;
				}
			}

			preview_rendered = preview->renderFrameDirectToSurface(frame,
				&preview->mPreviewWindow, &preview->preview_mutex,
				&preview_ready_ns, &surface_wait_ns);
			if (preview_rendered && preview_ready_ns
					&& frame->arrival_monotonic_ns)
				preview->recordEndToEndLatencyTiming(frame->arrival_monotonic_ns,
					preview_ready_ns);

			if (preview_rendered) {
				if (!has_preview_callback && !has_capture_consumers)
					return;
				uvc_frame_t *notification = preview->createFrameNotification(frame);
				if (LIKELY(notification)) {
					preview->addPreviewFrame(notification);
					return;
				}
			}

			uvc_frame_t *rgbx = preview->convertPreviewFrameToRgbx(frame);
			if (LIKELY(rgbx))
				preview->addPreviewFrame(rgbx);
			return;
		}

		if (!preview->hasPreviewFrameCallback() && !preview->hasCaptureConsumers())
			return;

		uvc_frame_t *copy = preview->get_frame(frame->actual_bytes);
		if (UNLIKELY(!copy)) {
#if LOCAL_DEBUG
			LOGE("uvc_callback:unable to allocate duplicate frame!");
#endif
			return;
		}
		uvc_error_t ret = uvc_duplicate_frame(frame, copy);
		if (UNLIKELY(ret)) {
			preview->recycle_frame(copy);
			return;
		}
		preview->addPreviewFrame(copy);
	}
}

uvc_frame_t *UVCPreview::createFrameNotification(uvc_frame_t *frame) {
	uvc_frame_t *notification = get_notification_frame();
	if (UNLIKELY(!notification))
		return NULL;

	notification->width = frame->width;
	notification->height = frame->height;
	notification->frame_format = UVC_FRAME_FORMAT_UNKNOWN;
	notification->step = 0;
	notification->sequence = frame->sequence;
	notification->capture_time = frame->capture_time;
	notification->arrival_monotonic_ns = frame->arrival_monotonic_ns;
	notification->source = frame->source;
	notification->actual_bytes = notification->data ? 1 : 0;
	return notification;
}

bool UVCPreview::startMjpegDecodeWorker() {
	if (mjpeg_decode_thread_joinable || frameMode != REQUEST_MODE_MJPEG)
		return true;
	mjpeg_decode_thread_joinable = true;
	const int result = pthread_create(&mjpeg_decode_thread, NULL,
		mjpeg_decode_thread_func, (void *)this);
	if (UNLIKELY(result != 0)) {
		mjpeg_decode_thread_joinable = false;
		LOGW("UVCPreview::startMjpegDecodeWorker pthread_create failed");
		return false;
	}
	return true;
}

void UVCPreview::stopMjpegDecodeWorker() {
	const bool should_join = mjpeg_decode_thread_joinable;
	pthread_mutex_lock(&mjpeg_decode_mutex);
	mjpeg_decode_thread_joinable = false;
	pthread_cond_signal(&mjpeg_decode_sync);
	pthread_mutex_unlock(&mjpeg_decode_mutex);
	if (should_join) {
		if (pthread_join(mjpeg_decode_thread, NULL) != EXIT_SUCCESS)
			LOGW("UVCPreview::stopMjpegDecodeWorker pthread_join failed");
	}
	clearMjpegDecodeFrame();
}

void UVCPreview::addMjpegDecodeFrame(uvc_frame_t *frame) {
	if (UNLIKELY(!frame))
		return;

	/* Stage 1: USB-thread publish -> libuvc callback thread. */
	if (UNLIKELY(!frame_integrity_ok(frame, "publish->callback", g_integrity_mismatch_cb))) {
		processingPreviewQueueDropCount.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	uvc_frame_t *queued = mjpeg_header_get();
	if (UNLIKELY(!queued)) {
		/* Pool exhausted: ring full plus both in-flight slots busy.  The ring's
		 * drop-oldest would have evicted anyway; count it as a queue drop. */
		processingPreviewQueueDropCount.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	*queued = *frame;
	queued->library_owns_data = 0;
	if (UNLIKELY(!uvc_frame_retain(queued))) {
		/* No slot reference: the bytes could be overwritten by the USB thread
		 * while the decoder reads them. Never hand such a frame to the async
		 * decoder (sequential overwrite shows as a shredded lower half). */
		mjpeg_header_put(queued);
		return;
	}
	pthread_mutex_lock(&mjpeg_decode_mutex);
	if (isRunning() && mjpeg_decode_thread_joinable) {
		uvc_frame_t *drop = mjpeg_decode_frame_ring.enqueue_drop_oldest_if_full(queued);
		if (drop) {
			processingPreviewQueueDropCount.fetch_add(1, std::memory_order_relaxed);
			uvc_frame_release(drop);
			mjpeg_header_put_locked(drop);
		}
		queued = NULL;
		pthread_cond_signal(&mjpeg_decode_sync);
	}
	pthread_mutex_unlock(&mjpeg_decode_mutex);
	if (queued) {
		uvc_frame_release(queued);
		mjpeg_header_put(queued);
	}
}

uvc_frame_t *UVCPreview::waitMjpegDecodeFrame() {
	uvc_frame_t *frame = NULL;
	pthread_mutex_lock(&mjpeg_decode_mutex);
	while (isRunning() && mjpeg_decode_thread_joinable
			&& mjpeg_decode_frame_ring.empty())
		pthread_cond_wait(&mjpeg_decode_sync, &mjpeg_decode_mutex);
	if (LIKELY(isRunning() && mjpeg_decode_thread_joinable
			&& !mjpeg_decode_frame_ring.empty()))
		frame = mjpeg_decode_frame_ring.dequeue();
	pthread_mutex_unlock(&mjpeg_decode_mutex);
	return frame;
}

void UVCPreview::clearMjpegDecodeFrame() {
	pthread_mutex_lock(&mjpeg_decode_mutex);
	while (!mjpeg_decode_frame_ring.empty()) {
		uvc_frame_t *frame = mjpeg_decode_frame_ring.dequeue();
		uvc_frame_release(frame);
		mjpeg_header_put_locked(frame);
	}
	mjpeg_decode_frame_ring.reset_storage();
	pthread_mutex_unlock(&mjpeg_decode_mutex);
}

void *UVCPreview::mjpeg_decode_thread_func(void *vptr_args) {
#if defined(__ANDROID__)
	consolation_tune_thread_latency("UVC-mjpg");
#endif
	UVCPreview *preview = reinterpret_cast<UVCPreview *>(vptr_args);
	if (LIKELY(preview))
		preview->do_mjpeg_decode();
	pthread_exit(NULL);
}

void UVCPreview::do_mjpeg_decode() {
	{
		/* Merged decode+render is chosen once per stream: the EGL context can be
		 * current on only one thread, so it cannot move between the decode and
		 * preview threads while running.  A/B: setprop debug.consolation.merged_render 0
		 * then restart the preview. */
		char mv[PROP_VALUE_MAX] = {};
		mMergedRender = true;
		if (__system_property_get("debug.consolation.merged_render", mv) > 0 && mv[0] == '0')
			mMergedRender = false;
		LOGI("merged-render: %s (decode thread %s the planar frame)",
			mMergedRender ? "on" : "off", mMergedRender ? "renders" : "hands off");
	}
	mStream.dec = mStreamDecodeEnabled ? uvc_mjpeg_stream_decoder_create() : NULL;
	if (mStreamDecodeEnabled && !mStream.dec) {
		LOGW("stream-decode: decoder allocation failed; decoding whole frames");
		mStreamDecodeEnabled = false;
	}
	for (; LIKELY(isRunning() && mjpeg_decode_thread_joinable); ) {
		uvc_frame_t *frame = NULL;
		LiveAssembly live;
		if (!waitMjpegWork(&frame, &live))
			break;
		if (!frame) {
			/* No finished frame waiting: work ahead on the one arriving. */
			streamDecodeFollow(live);
			continue;
		}

		const uint64_t t_frame = processing_now_ns();
		/* Stage 2: callback thread enqueue -> decode thread dequeue (retained slot). */
		if (UNLIKELY(!frame_integrity_ok(frame, "callback->decode", g_integrity_mismatch_decode))) {
			processingPreviewQueueDropCount.fetch_add(1, std::memory_order_relaxed);
			uvc_frame_release(frame);
			mjpeg_header_put(frame);
			continue;
		}
		debug_mjpeg_dump_maybe(frame);
		{
			/* A/B switch: adb shell setprop debug.consolation.gpu_planar 0|1
			 * (polled every 60 frames; only honoured if the format probe passed). */
			static uint32_t s_poll;
			static int s_forced = -1;	/* -1 unset, 0 off, 1 on */
			if ((s_poll++ % 60) == 0) {
				char value[PROP_VALUE_MAX] = {};
				if (__system_property_get("debug.consolation.gpu_planar", value) > 0 && value[0]) {
					const int want = value[0] != '0';
					if (want != s_forced) {
						s_forced = want;
						if (mGpuPlanarFormat) {
							mGpuPlanarEnabled = want != 0;
							mGpuPlanarRenderFailures = 0;
							LOGW("gpu-planar: %s by property", want ? "enabled" : "disabled");
						}
					}
				}
			}
		}
		if (!streamDecodeComplete(frame))
			decodeWholeMjpegFrame(frame);
		perfHintReport(mStream.work_ns + (processing_now_ns() - t_frame));
		mStream.work_ns = 0;
		uvc_frame_release(frame);
		mjpeg_header_put(frame);
	}
	streamDecodeAbort();
	if (mStream.dec) {
		uvc_mjpeg_stream_decoder_destroy(mStream.dec);
		mStream.dec = NULL;
	}
	memset(&mStream, 0, sizeof(mStream));
	perfHintClose();
}

/* Decode a published frame in one go (the path used before decode-ahead, and
 * still the fallback for anything decode-ahead declines or gets wrong). */
void UVCPreview::decodeWholeMjpegFrame(uvc_frame_t *frame) {
	if (mGpuPlanarEnabled && decode_mjpeg_to_gpu_planar(frame)) {
		return;
	}
	uvc_frame_t *decoded = get_frame(0);
	if (LIKELY(decoded)) {
		const uint64_t t_convert = processing_now_ns();
		const uvc_error_t result = uvc_mjpeg2yuv_planar(frame, decoded);
		recordPreviewConversionTiming(processing_now_ns() - t_convert);
		if (LIKELY(result == UVC_SUCCESS)) {
			/* Stage 3 check happens on the preview thread: was the JPEG
			 * still intact once decoding finished (writer overtook reader)? */
			if (UNLIKELY(!frame_integrity_ok(frame, "during-decode", g_integrity_mismatch_decode))) {
				processingPreviewQueueDropCount.fetch_add(1, std::memory_order_relaxed);
				recycle_frame(decoded);
				decoded = NULL;
				return;
			}
#if UVC_FRAME_INTEGRITY_CHECK
			decoded->integrity_sample_hash =
				uvc_frame_sample_hash(decoded->data, decoded->actual_bytes);
#endif
			if (mMergedRender)
				presentPlanarFrame(decoded);
			else
				addPreviewFrame(decoded);
			decoded = NULL;
		} else {
			/* Corrupt or undecodable frame: not shown, last good frame stays.
			 * Surface it in the dropped-frames telemetry counter. */
			processingPreviewQueueDropCount.fetch_add(1, std::memory_order_relaxed);
			UVC_DIAG_LOGI("mjpeg-diag:async-planar-decode-fail seq=%u bytes=%zu result=%d frame=%ux%u",
				frame->sequence,
				frame->actual_bytes,
				result,
				frame->width,
				frame->height);
		}
	}
	if (decoded)
		recycle_frame(decoded);
}

bool UVCPreview::waitMjpegWork(uvc_frame_t **final_frame, LiveAssembly *live) {
	bool have = false;
	pthread_mutex_lock(&mjpeg_decode_mutex);
	while (isRunning() && mjpeg_decode_thread_joinable
			&& mjpeg_decode_frame_ring.empty() && !mLive.dirty)
		pthread_cond_wait(&mjpeg_decode_sync, &mjpeg_decode_mutex);
	if (LIKELY(isRunning() && mjpeg_decode_thread_joinable)) {
		have = true;
		if (!mjpeg_decode_frame_ring.empty()) {
			/* A published frame always goes first: it is older than whatever
			 * is arriving now.  Progress stays flagged for the next round. */
			*final_frame = mjpeg_decode_frame_ring.dequeue();
		} else {
			*live = mLive;
			mLive.dirty = false;
		}
	}
	pthread_mutex_unlock(&mjpeg_decode_mutex);
	return have;
}

void UVCPreview::streamDecodeReleaseOutput(bool unlock_only) {
	if (!mStream.out)
		return;
	GpuPlanarFrame *g = reinterpret_cast<GpuPlanarFrame *>(mStream.out);
	for (int i = 0; i < mStream.locked; i++)
		AHardwareBuffer_unlock((AHardwareBuffer *)g->ahb[i], NULL);
	mStream.locked = 0;
	if (!unlock_only) {
		gpu_planar_put(mStream.out, -1);
		mStream.out = NULL;
	}
}

/* Drop whatever is in progress.  The generation stays recorded as dead so
 * late progress reports for it do not restart the decode. */
void UVCPreview::streamDecodeAbort() {
	if (mStream.dec && mStream.active)
		uvc_mjpeg_stream_abort(mStream.dec);
	streamDecodeReleaseOutput(false);
	mStream.active = false;
	mStream.done = false;
	mStream.dead = true;
}

/* The JPEG header is in: get a GPU planar target of the right shape and
 * point the decoder at it. */
bool UVCPreview::streamDecodeAcquireOutput() {
	uint32_t min_strides[3];
	if (uvc_mjpeg_stream_layout(mStream.dec, mStream.widths, mStream.heights,
			&mStream.subsamp) != UVC_SUCCESS
			|| uvc_mjpeg_stream_min_strides(mStream.dec, min_strides) != UVC_SUCCESS)
		return false;

	uvc_frame_t *out = gpu_planar_get(mStream.widths, mStream.heights);
	if (UNLIKELY(!out))
		return false;
	GpuPlanarFrame *g = reinterpret_cast<GpuPlanarFrame *>(out);
	mStream.out = out;
	mStream.locked = 0;

	const int nplanes = mStream.widths[1] ? 3 : 1;
	unsigned char *planes[3] = { NULL, NULL, NULL };
	int strides[3] = { 0, 0, 0 };
	bool ok = true;
	for (int i = 0; i < nplanes && ok; i++)
		ok = g->stride[i] >= min_strides[i];

	/* As in decode_mjpeg_to_gpu_planar: the first lock consumes the fence
	 * from this slot's last GPU read. */
	int fence = g->fence_fd;
	g->fence_fd = -1;
	for (int i = 0; i < nplanes && ok; i++) {
		void *ptr = NULL;
		const int lock_fence = i == 0 ? fence : -1;
		if (AHardwareBuffer_lock((AHardwareBuffer *)g->ahb[i],
				AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, lock_fence,
				NULL, &ptr) != 0 || !ptr) {
			ok = false;
			break;
		}
		if (i == 0)
			fence = -1;	/* handed to the lock */
		planes[i] = (unsigned char *)ptr;
		strides[i] = (int)g->stride[i];
		mStream.locked = i + 1;
	}
	if (fence >= 0)
		close(fence);
	if (ok)
		ok = uvc_mjpeg_stream_set_output(mStream.dec, planes, strides) == UVC_SUCCESS;
	if (!ok)
		streamDecodeReleaseOutput(false);
	return ok;
}

void UVCPreview::streamDecodeAdvance(size_t bytes, bool no_more_data) {
	const uint64_t t0 = processing_now_ns();
	int st = uvc_mjpeg_stream_advance(mStream.dec, bytes, no_more_data ? 1 : 0);
	if (st == UVC_MJPEG_STREAM_NEED_OUTPUT) {
		if (streamDecodeAcquireOutput())
			st = uvc_mjpeg_stream_advance(mStream.dec, bytes, no_more_data ? 1 : 0);
		else
			st = UVC_MJPEG_STREAM_UNSUPPORTED;
	}
	mStream.work_ns += processing_now_ns() - t0;

	if (st == UVC_MJPEG_STREAM_DONE) {
		/* Fully decoded.  The planes are unlocked now so the GPU can sample
		 * them the moment the frame's verdict arrives. */
		streamDecodeReleaseOutput(true);
		mStream.done = true;
	} else if (st != UVC_MJPEG_STREAM_NEED_DATA) {
		streamDecodeAbort();	/* failed or declined: whole-frame path decides */
	}
}

/* Follow the frame the USB thread is assembling. */
void UVCPreview::streamDecodeFollow(const LiveAssembly &live) {
	if (!mStream.dec || !mStreamDecodeEnabled || !mGpuPlanarEnabled) {
		if (mStream.active || mStream.out)
			streamDecodeAbort();
		return;
	}
	if (live.discarded_gen && live.discarded_gen == mStream.gen) {
		streamDecodeAbort();
		return;
	}
	if (!live.gen || live.gen == live.discarded_gen || !live.data)
		return;

	if (live.gen != mStream.gen) {
		/* A new frame started.  Anything still held belongs to a frame that
		 * was neither published nor reported discarded: let it go. */
		streamDecodeAbort();
		mStream.gen = live.gen;
		mStream.data = live.data;
		mStream.dead = false;
		mStream.work_ns = 0;
		uvc_mjpeg_stream_begin(mStream.dec, live.data, (uint32_t)frameWidth,
			(uint32_t)frameHeight);
		mStream.active = true;
	}
	if (mStream.dead || mStream.done)
		return;
	streamDecodeAdvance(live.bytes, live.complete);
}

/* `frame` was published.  If it is the one decode-ahead has been working on,
 * finish it (often there is nothing left to do) and present it. */
bool UVCPreview::streamDecodeComplete(uvc_frame_t *frame) {
	if (!mStream.dec || !frame->assembly_generation
			|| mStream.gen != frame->assembly_generation) {
		/* Not ours: it never reported progress, or the decode thread has not
		 * looked at it yet.  Make sure nothing stale is still held. */
		if (mStream.active || mStream.out)
			streamDecodeAbort();
		if (frame->assembly_generation) {
			mStream.gen = frame->assembly_generation;
			mStream.dead = true;
		}
		return false;
	}
	if (mStream.dead)
		return false;
	if (UNLIKELY(mStream.data != (const uint8_t *)frame->data)) {
		/* Same generation number but different memory: not the bytes that
		 * were decoded.  Never present that. */
		streamDecodeAbort();
		return false;
	}

	if (!mStream.done)
		streamDecodeAdvance(frame->actual_bytes, true);
	if (!mStream.done) {
		/* The incremental decode did not get there.  The whole-frame path
		 * redoes it; if that keeps happening the fault is on this side. */
		if (++mStream.failures == 30 && mStreamDecodeEnabled) {
			LOGW("stream-decode: 30 frames in a row needed the whole-frame path; disabling");
			mStreamDecodeEnabled = false;
		}
		return false;
	}
	mStream.failures = 0;

	uvc_frame_t *out = mStream.out;
	const uvc_error_t result = uvc_mjpeg_stream_finish(mStream.dec, frame);
	mStream.out = NULL;
	mStream.active = false;
	mStream.done = false;
	mStream.dead = true;	/* this generation is finished with */
	recordPreviewConversionTiming(mStream.work_ns);
	if (UNLIKELY(result != UVC_SUCCESS)) {
		/* Damaged bitstream: same policy as the whole-frame path, not shown. */
		processingPreviewQueueDropCount.fetch_add(1, std::memory_order_relaxed);
		gpu_planar_put(out, -1);
		return true;
	}
	emitGpuPlanarFrame(out, frame, mStream.widths, mStream.heights, mStream.subsamp);
	return true;
}

/*
 * CPU performance hints (Android 13+, android/performance_hint.h).  The decode
 * thread works in a short burst per frame and then sleeps, which frequency
 * governors read as a light load and answer with a low clock.  A hint session
 * tells the system how long each burst took against how long it should take.
 * The target is deliberately a fraction of the frame interval: the aim is the
 * shortest decode, not merely one that fits the interval.  The USB event
 * thread joins the session when its id is known.
 * A/B: setprop debug.consolation.perf_hint 0  (read when the stream starts).
 * The symbols are looked up at run time; the app's minSdk predates them.
 */
namespace {
struct PerfHintApi {
	void *(*getManager)();
	void *(*createSession)(void *manager, const int32_t *tids, size_t count, int64_t target_ns);
	int (*reportActual)(void *session, int64_t actual_ns);
	void (*closeSession)(void *session);
	bool ok;
};
static const PerfHintApi &perf_hint_api() {
	static PerfHintApi api = [] {
		PerfHintApi a = {};
		void *lib = dlopen("libandroid.so", RTLD_NOW);
		if (lib) {
			a.getManager = reinterpret_cast<void *(*)()>(
				dlsym(lib, "APerformanceHint_getManager"));
			a.createSession = reinterpret_cast<void *(*)(void *, const int32_t *, size_t, int64_t)>(
				dlsym(lib, "APerformanceHint_createSession"));
			a.reportActual = reinterpret_cast<int (*)(void *, int64_t)>(
				dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
			a.closeSession = reinterpret_cast<void (*)(void *)>(
				dlsym(lib, "APerformanceHint_closeSession"));
		}
		a.ok = a.getManager && a.createSession && a.reportActual && a.closeSession;
		return a;
	}();
	return api;
}
} // namespace

void UVCPreview::perfHintReport(uint64_t work_ns) {
	if (UNLIKELY(!mPerfHintTried)) {
		mPerfHintTried = true;
		char pv[PROP_VALUE_MAX] = {};
		if (__system_property_get("debug.consolation.perf_hint", pv) > 0 && pv[0] == '0') {
			LOGI("perf-hint: disabled by property");
			return;
		}
		const PerfHintApi &api = perf_hint_api();
		void *manager = api.ok ? api.getManager() : NULL;
		if (!manager) {
			LOGI("perf-hint: not available on this device");
			return;
		}
		const int fps = requestMaxFps > 0 ? requestMaxFps : 60;
		int64_t target_ns = (1000000000LL / fps) / 8;
		if (target_ns < 1000000LL)
			target_ns = 1000000LL;
		int32_t tids[2] = { (int32_t)gettid(), 0 };
		size_t count = 1;
		const int usb_tid = mUsbThreadTid.load(std::memory_order_relaxed);
		if (usb_tid > 0 && usb_tid != tids[0])
			tids[count++] = (int32_t)usb_tid;
		mPerfHintSession = api.createSession(manager, tids, count, target_ns);
		if (!mPerfHintSession && count > 1) {
			count = 1;
			mPerfHintSession = api.createSession(manager, tids, count, target_ns);
		}
		LOGI("perf-hint: session %s (threads=%zu target=%lld us)",
			mPerfHintSession ? "created" : "refused", count,
			(long long)(target_ns / 1000));
	}
	if (mPerfHintSession && work_ns)
		perf_hint_api().reportActual(mPerfHintSession, (int64_t)work_ns);
}

void UVCPreview::perfHintClose() {
	if (mPerfHintSession) {
		perf_hint_api().closeSession(mPerfHintSession);
		mPerfHintSession = NULL;
	}
	mPerfHintTried = false;
}

/* Blacks out the buffer area around a frame decoded at (off_x, off_y); RGBX 0 is black.
 * Only the letterbox strips are touched, never the frame itself. */
static void clearLetterbox(uint8_t *bits, size_t row_bytes, int32_t buf_w, int32_t buf_h,
	uint32_t frame_w, uint32_t frame_h, size_t off_x, size_t off_y) {
	const size_t px = PREVIEW_PIXEL_BYTES;
	const size_t right_x = off_x + frame_w;
	const size_t right_bytes = ((size_t) buf_w - right_x) * px;
	if (off_y)
		memset(bits, 0, off_y * row_bytes);
	const size_t bottom_y = off_y + frame_h;
	if ((size_t) buf_h > bottom_y)
		memset(bits + bottom_y * row_bytes, 0, ((size_t) buf_h - bottom_y) * row_bytes);
	if (off_x || right_bytes) {
		for (size_t y = off_y; y < bottom_y; ++y) {
			uint8_t *row = bits + y * row_bytes;
			if (off_x)
				memset(row, 0, off_x * px);
			if (right_bytes)
				memset(row + right_x * px, 0, right_bytes);
		}
	}
}

bool UVCPreview::renderFrameDirectToSurface(uvc_frame_t *frame,
	ANativeWindow **window, pthread_mutex_t *window_mutex,
	uint64_t *frame_ready_ns, uint64_t *surface_wait_ns) {
	bool rendered = false;

	pthread_mutex_lock(window_mutex);
	ANativeWindow *target = *window;
	if (LIKELY(target)) {
		if (mGpuPreviewRenderer) {
			/* The GPU path renders at the view's own size (1:1 with the screen);
			 * only the CPU fallback below needs stream-sized buffers. */
			if (mPreviewGeomWidth || mPreviewGeomHeight) {
				ANativeWindow_setBuffersGeometry(target, 0, 0, previewFormat);
				mPreviewGeomWidth = 0;
				mPreviewGeomHeight = 0;
				mPreviewSurfaceSizeDirty = true;
			}
			if (mPreviewSurfaceSizeDirty) {
				mGpuPreviewRenderer->invalidateSurfaceSize();
				mPreviewSurfaceSizeDirty = false;
			}
			mGpuPreviewRenderer->setTransform(mPreviewXform);
			if (frame->frame_format != UVC_FRAME_FORMAT_MJPEG
					&& mGpuPreviewRenderer->render(frame, target, frame_ready_ns)) {
				rendered = true;
				pthread_mutex_unlock(window_mutex);
				return rendered;
			}
			if (gpu_planar_is(frame)) {
				/* Planes live only in GPU memory: nothing for the CPU path. */
				pthread_mutex_unlock(window_mutex);
				return false;
			}
			if (frame->frame_format == UVC_FRAME_FORMAT_MJPEG) {
				if (UNLIKELY(!mMjpegPreviewYuvFrame))
					mMjpegPreviewYuvFrame = uvc_allocate_frame(0);
				if (LIKELY(mMjpegPreviewYuvFrame)) {
					const uint64_t t_convert = processing_now_ns();
					const uvc_error_t result =
						uvc_mjpeg2yuv_planar(frame, mMjpegPreviewYuvFrame);
					recordPreviewConversionTiming(processing_now_ns() - t_convert);
					if (LIKELY(result == UVC_SUCCESS
							&& mGpuPreviewRenderer->render(mMjpegPreviewYuvFrame,
								target, frame_ready_ns))) {
						rendered = true;
						pthread_mutex_unlock(window_mutex);
						return rendered;
					}
					if (UNLIKELY(result != UVC_SUCCESS)) {
						UVC_DIAG_LOGI("mjpeg-diag:planar-preview-fail seq=%u bytes=%zu result=%d frame=%ux%u",
							frame->sequence,
							frame->actual_bytes,
							result,
							frame->width,
							frame->height);
					}
				}
			}
		}
		/* The view fills the screen, so pad the buffer to the screen's shape (the
		 * compositor then scales it uniformly) and decode the frame into its center. */
		{
			int32_t geom_w = (int32_t) frame->width;
			int32_t geom_h = (int32_t) frame->height;
			if (mPreviewFitX < 1.0f)
				geom_w = std::max(geom_w, (int32_t) lroundf(frame->width / mPreviewFitX) & ~1);
			if (mPreviewFitY < 1.0f)
				geom_h = std::max(geom_h, (int32_t) lroundf(frame->height / mPreviewFitY) & ~1);
			if (geom_w != mPreviewGeomWidth || geom_h != mPreviewGeomHeight) {
				ANativeWindow_setBuffersGeometry(target, geom_w, geom_h, previewFormat);
				mPreviewGeomWidth = geom_w;
				mPreviewGeomHeight = geom_h;
			}
		}
		ANativeWindow_Buffer buffer;
		const uint64_t lock_start_ns = processing_now_ns();
		if (LIKELY(ANativeWindow_lock(target, &buffer, NULL) == 0)) {
			if (surface_wait_ns)
				*surface_wait_ns += processing_now_ns() - lock_start_ns;
			if (LIKELY(buffer.bits && buffer.width >= (int32_t) frame->width &&
					buffer.height >= (int32_t) frame->height)) {
				const size_t row_bytes = (size_t) buffer.stride * PREVIEW_PIXEL_BYTES;
				const size_t off_x = (size_t) (buffer.width - (int32_t) frame->width) / 2;
				const size_t off_y = (size_t) (buffer.height - (int32_t) frame->height) / 2;
				uint8_t *bits = (uint8_t *) buffer.bits;
				if (off_x || off_y)
					clearLetterbox(bits, row_bytes, buffer.width, buffer.height,
						frame->width, frame->height, off_x, off_y);
				uvc_frame_t surface = {};
				surface.data = bits + off_y * row_bytes + off_x * PREVIEW_PIXEL_BYTES;
				surface.data_bytes = row_bytes * ((size_t) buffer.height - off_y)
					- off_x * PREVIEW_PIXEL_BYTES;
				surface.width = frame->width;
				surface.height = frame->height;
				surface.frame_format = UVC_FRAME_FORMAT_RGBX;
				surface.step = (size_t) buffer.stride * PREVIEW_PIXEL_BYTES;
				surface.sequence = frame->sequence;
				surface.capture_time = frame->capture_time;
				surface.source = frame->source;
				surface.library_owns_data = 0;

				const uint64_t t_convert = processing_now_ns();
				const uvc_error_t result = frame->frame_format == UVC_FRAME_FORMAT_MJPEG
					? uvc_mjpeg2rgbx(frame, &surface) : uvc_any2rgbx(frame, &surface);
				recordPreviewConversionTiming(processing_now_ns() - t_convert);
				rendered = result == UVC_SUCCESS;
				if (LIKELY(rendered && frame->frame_format == UVC_FRAME_FORMAT_MJPEG)) {
					recordMjpegDecodedVisualSample(frame->sequence, frame->actual_bytes,
						(const uint8_t *)surface.data, surface.step, frame->width, frame->height);
				}
				if (UNLIKELY(result && frame->frame_format == UVC_FRAME_FORMAT_MJPEG)) {
					UVC_DIAG_LOGI("mjpeg-diag:decode-fail direct seq=%u bytes=%zu result=%d surface=%dx%d stride=%d frame=%ux%u",
						frame->sequence,
						frame->actual_bytes,
						result,
						buffer.width,
						buffer.height,
						buffer.stride,
						frame->width,
						frame->height);
				}
				if (rendered && frame_ready_ns)
					*frame_ready_ns = processing_now_ns();
			}
			ANativeWindow_unlockAndPost(target);
		}
	}
	pthread_mutex_unlock(window_mutex);

	return rendered;
}

uvc_frame_t *UVCPreview::convertPreviewFrameToRgbx(uvc_frame_t *frame) {
	uvc_frame_t *rgbx = get_frame(frame->width * frame->height * PREVIEW_PIXEL_BYTES);
	if (UNLIKELY(!rgbx))
		return NULL;

	uint64_t t_convert = processing_now_ns();
	uvc_error_t result = frame->frame_format == UVC_FRAME_FORMAT_MJPEG
		? uvc_mjpeg2rgbx(frame, rgbx) : uvc_any2rgbx(frame, rgbx);
	recordPreviewConversionTiming(processing_now_ns() - t_convert);

	if (UNLIKELY(result && frame->frame_format == UVC_FRAME_FORMAT_MJPEG)) {
		UVC_DIAG_LOGI("mjpeg-diag:decode-fail scratch-primary seq=%u bytes=%zu result=%d frame=%ux%u",
			frame->sequence,
			frame->actual_bytes,
			result,
			frame->width,
			frame->height);
		uvc_frame_t *tmp = get_frame(frame->width * frame->height * 2);
		if (LIKELY(tmp)) {
			t_convert = processing_now_ns();
			result = uvc_mjpeg2yuyv(frame, tmp);
			if (LIKELY(!result))
				result = uvc_any2rgbx(tmp, rgbx);
			recordPreviewConversionTiming(processing_now_ns() - t_convert);
			recycle_frame(tmp);
		} else {
			result = UVC_ERROR_NO_MEM;
		}
	}

	if (UNLIKELY(result)) {
		if (frame->frame_format == UVC_FRAME_FORMAT_MJPEG) {
			UVC_DIAG_LOGI("mjpeg-diag:decode-fail scratch-final seq=%u bytes=%zu result=%d frame=%ux%u",
				frame->sequence,
				frame->actual_bytes,
				result,
				frame->width,
				frame->height);
		}
		recycle_frame(rgbx);
		return NULL;
	}
	rgbx->arrival_monotonic_ns = frame->arrival_monotonic_ns;
	if (frame->frame_format == UVC_FRAME_FORMAT_MJPEG) {
		recordMjpegDecodedVisualSample(frame->sequence, frame->actual_bytes,
			(const uint8_t *)rgbx->data, rgbx->step, rgbx->width, rgbx->height);
	}
	return rgbx;
}

void UVCPreview::addPreviewFrame(uvc_frame_t *frame) {

	pthread_mutex_lock(&preview_queue_mutex);
	if (isRunning()) {
		if (frameMode != REQUEST_MODE_H264) {
			/* Latest-wins for live video: anything still queued is older than
			 * this frame and would only be shown late.  Recycle it instead. */
			while (!preview_frame_ring.empty()) {
				uvc_frame_t *stale = preview_frame_ring.dequeue();
				processingPreviewQueueDropCount.fetch_add(1, std::memory_order_relaxed);
				recycle_frame(stale);
			}
		}
		uvc_frame_t *drop = preview_frame_ring.enqueue_drop_oldest_if_full(frame);
		const uint64_t queued_backlog =
			preview_frame_ring.size() > 0 ? preview_frame_ring.size() - 1 : 0;
		recordPreviewEnqueueDepthSample(
			&processingPreviewEnqueueDepthSampleCount,
			&processingPreviewEnqueueDepthTotalMilli,
			&processingPreviewEnqueueDepthMaxMilli,
			queued_backlog);
		if UNLIKELY(drop) {
			processingPreviewQueueDropCount.fetch_add(1, std::memory_order_relaxed);
			recycle_frame(drop);
		}
		frame = nullptr;
		pthread_cond_signal(&preview_sync);
	}
	pthread_mutex_unlock(&preview_queue_mutex);
	if (frame)
		recycle_frame(frame);
}

uvc_frame_t *UVCPreview::waitPreviewFrame() {
	uvc_frame_t *frame = nullptr;
	pthread_mutex_lock(&preview_queue_mutex);
	{
		while (isRunning() && preview_frame_ring.empty())
			pthread_cond_wait(&preview_sync, &preview_queue_mutex);
		if (LIKELY(isRunning() && !preview_frame_ring.empty())) {
			frame = preview_frame_ring.dequeue();
			recordPreviewQueueDepthSample(static_cast<uint64_t>(preview_frame_ring.size()));
		}
	}
	pthread_mutex_unlock(&preview_queue_mutex);
	return frame;
}

void UVCPreview::clearPreviewFrame() {
	pthread_mutex_lock(&preview_queue_mutex);
	{
		while (!preview_frame_ring.empty())
			recycle_frame(preview_frame_ring.dequeue());
		preview_frame_ring.reset_storage();
	}
	pthread_mutex_unlock(&preview_queue_mutex);
}

void *UVCPreview::preview_thread_func(void *vptr_args) {
	int result;

	ENTER();
#if defined(__ANDROID__)
	consolation_tune_thread_latency("UVC-prev");
#endif
	UVCPreview *preview = reinterpret_cast<UVCPreview *>(vptr_args);
	if (LIKELY(preview)) {
		uvc_stream_ctrl_t ctrl;
		result = preview->prepare_preview(&ctrl);
		if (LIKELY(!result)) {
			preview->do_preview(&ctrl);
		}
	}
	PRE_EXIT();
	pthread_exit(NULL);
}

int UVCPreview::prepare_preview(uvc_stream_ctrl_t *ctrl) {
	uvc_error_t result;

	ENTER();
	uvc_set_rgbx_converter_backend(UVC_RGBX_CONVERTER_BACKEND_INTERNAL);
	result = uvc_get_stream_ctrl_format_size_fps(mDeviceHandle, ctrl,
		request_mode_to_frame_format(requestMode),
		requestWidth, requestHeight, requestMinFps, requestMaxFps
	);
	if (LIKELY(!result)) {
#if LOCAL_DEBUG
		uvc_print_stream_ctrl(ctrl, stderr);
#endif
		uvc_frame_desc_t *frame_desc;
		result = uvc_get_frame_desc(mDeviceHandle, ctrl, &frame_desc);
		if (LIKELY(!result)) {
			frameWidth = frame_desc->wWidth;
			frameHeight = frame_desc->wHeight;
			LOGI("frameSize=(%d,%d)@%s", frameWidth, frameHeight, request_mode_name(requestMode));
			pthread_mutex_lock(&preview_mutex);
			if (LIKELY(mPreviewWindow)) {
				ANativeWindow_setBuffersGeometry(mPreviewWindow,
					frameWidth, frameHeight, previewFormat);
				mPreviewGeomWidth = frameWidth;
				mPreviewGeomHeight = frameHeight;
			}
			pthread_mutex_unlock(&preview_mutex);
		} else {
			frameWidth = requestWidth;
			frameHeight = requestHeight;
		}
		frameMode = requestMode;
		{
			const size_t wpx = static_cast<size_t>(frameWidth);
			const size_t hpx = static_cast<size_t>(frameHeight);
			if (requestMode == REQUEST_MODE_NV12 || requestMode == REQUEST_MODE_YU12)
				frameBytes = (wpx * hpx * 3) / 2;
			else if (requestMode == REQUEST_MODE_P010)
				frameBytes = wpx * hpx * 3;
			else if (requestMode == REQUEST_MODE_BGR3)
				frameBytes = wpx * hpx * 3;
			else
				frameBytes = wpx * hpx * static_cast<size_t>(requestMode == REQUEST_MODE_YUYV ? 2 : 4);
			previewBytes = wpx * hpx * static_cast<size_t>(PREVIEW_PIXEL_BYTES);
		}
		if (LIKELY(previewBytes > 0))
			init_pool(previewBytes);
	} else {
		LOGE("could not negotiate with camera:err=%d", result);
	}
	RETURN(result, int);
}

void UVCPreview::presentPlanarFrame(uvc_frame_t *frame) {
	uint64_t frame_ready_ns = 0;
	uint64_t surface_wait_ns = 0;
	/* Stage 4: decode thread -> preview thread (pool frame). */
	if (UNLIKELY(!frame_integrity_ok(frame, "decode->render", g_integrity_mismatch_render))) {
		processingPreviewQueueDropCount.fetch_add(1, std::memory_order_relaxed);
		recycle_frame(frame);
		return;
	}
	const uint64_t t_render = processing_now_ns();
	const bool rendered = renderFrameDirectToSurface(frame,
		&mPreviewWindow, &preview_mutex, &frame_ready_ns,
		&surface_wait_ns);
	{
		/* Per-path render cost (upload+draw+swap), logged every
		 * 600 frames so the zero-copy path can be A/B'd on device. */
		static uint64_t s_sum[2], s_max[2];
		static uint32_t s_n[2];
		const int path = gpu_planar_is(frame) ? 1 : 0;
		const uint64_t dt = processing_now_ns() - t_render;
		s_sum[path] += dt;
		if (dt > s_max[path]) s_max[path] = dt;
		if (++s_n[path] == 600) {
			LOGI("planar-render: path=%s avg_us=%llu max_us=%llu over 600 frames",
				path ? "zero-copy" : "upload",
				(unsigned long long)(s_sum[path] / 600 / 1000),
				(unsigned long long)(s_max[path] / 1000));
			s_sum[path] = 0; s_max[path] = 0; s_n[path] = 0;
		}
	}
	if (rendered && frame_ready_ns && frame->arrival_monotonic_ns) {
		recordEndToEndLatencyTiming(frame->arrival_monotonic_ns,
			frame_ready_ns);
	}
	if (gpu_planar_is(frame)) {
		if (rendered) {
			mGpuPlanarRenderFailures = 0;
			gpu_planar_put(frame, mGpuPreviewRenderer
				? mGpuPreviewRenderer->takeRenderFenceFd() : -1);
		} else {
			gpu_planar_put(frame, -1);
			if (++mGpuPlanarRenderFailures >= 3 && mGpuPlanarEnabled) {
				LOGW("gpu-planar: render failed %u times; falling back to texture upload",
					mGpuPlanarRenderFailures);
				mGpuPlanarEnabled = false;
			}
		}
	} else {
		recycle_frame(frame);
	}
}

void UVCPreview::do_preview(uvc_stream_ctrl_t *ctrl) {
	ENTER();

	uvc_frame_t *frame = NULL;
	JavaVM *vm = getVM();
	JNIEnv *env = nullptr;
	bool preview_thread_attached = false;
	if (vm) {
		const jint gotEnv = vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
		if (gotEnv == JNI_EDETACHED) {
			if (vm->AttachCurrentThread(&env, NULL) == 0)
				preview_thread_attached = true;
			else
				env = nullptr;
		} else if (gotEnv != JNI_OK) {
			env = nullptr;
		}
	}
	{
		/* USB-thread hooks, MJPEG only.  A/B switches (read once per stream):
		 *   setprop debug.consolation.usb_publish 0    frames take the callback thread
		 *   setprop debug.consolation.stream_decode 0  decode starts at end of frame */
		char pv[PROP_VALUE_MAX] = {};
		const bool mjpeg = frameMode == REQUEST_MODE_MJPEG;
		bool usb_publish = mjpeg, stream_decode = mjpeg;
		if (__system_property_get("debug.consolation.usb_publish", pv) > 0 && pv[0] == '0')
			usb_publish = false;
		pv[0] = 0;
		if (__system_property_get("debug.consolation.stream_decode", pv) > 0 && pv[0] == '0')
			stream_decode = false;
		/* Decode-ahead ends in a frame published on the USB thread; without
		 * that hook its result would never be claimed. */
		if (!usb_publish)
			stream_decode = false;
		pthread_mutex_lock(&mjpeg_decode_mutex);
		memset(&mLive, 0, sizeof(mLive));
		pthread_mutex_unlock(&mjpeg_decode_mutex);
		mStreamDecodeEnabled = stream_decode;
		uvc_set_usb_thread_callbacks(mDeviceHandle,
			usb_publish ? uvc_preview_publish_hook : NULL,
			stream_decode ? uvc_preview_assembly_hook : NULL, (void *)this);
		LOGI("usb-thread hooks: publish=%s stream_decode=%s",
			usb_publish ? "on" : "off", stream_decode ? "on" : "off");
	}
	const uint64_t t_start_streaming = processing_now_ns();
	uvc_error_t result = uvc_start_streaming_bandwidth(
		mDeviceHandle, ctrl, uvc_preview_frame_callback, (void *)this, requestBandwidth, 0);
	const uint64_t start_streaming_elapsed_ms = (processing_now_ns() - t_start_streaming) / 1000000ULL;
	LOGI("startup-diag:uvc_start_streaming_bandwidth done in %llu ms result=%d",
		(unsigned long long)start_streaming_elapsed_ms, result);

	if (LIKELY(!result)) {
		streamingStartMonotonicNs = processing_now_ns();
		firstFrameLogged = false;
		clearPreviewFrame();
		if (frameMode == REQUEST_MODE_MJPEG)
			startMjpegDecodeWorker();
#if LOCAL_DEBUG
		LOGI("Streaming...");
#endif
		if (frameMode == REQUEST_MODE_H264) {
			for ( ; LIKELY(isRunning()) ; ) {
				frame = waitPreviewFrame();
				if (LIKELY(frame)) {
					if (preview_frame_callback_enabled && env) {
						do_preview_frame_callback(env, frame);
						frame = NULL;
					} else if (capture_thread_joinable) {
						addCaptureFrame(frame);
						frame = NULL;
					} else if (capture_frame_callback_enabled && env) {
						do_capture_callback(env, frame);
						frame = NULL;
					} else {
						recycle_frame(frame);
						frame = NULL;
					}
				}
			}
		} else {
			// Non-H264 frames are converted to owned RGBX frames in the UVC callback,
			// so the preview thread only performs the surface copy.
			for ( ; LIKELY(isRunning()) ; ) {
				frame = waitPreviewFrame();
				if (LIKELY(frame)) {
					if (frame->frame_format == UVC_FRAME_FORMAT_UNKNOWN) {
						if (preview_frame_callback_enabled && env) {
							do_preview_frame_callback(env, frame);
							frame = NULL;
						} else if (capture_thread_joinable) {
							addCaptureFrame(frame);
							frame = NULL;
						} else if (capture_frame_callback_enabled && env) {
							do_capture_callback(env, frame);
							frame = NULL;
						} else {
							recycle_frame(frame);
							frame = NULL;
						}
						continue;
					}
					if (frame->frame_format == UVC_FRAME_FORMAT_MJPEG_YUV_PLANAR) {
						presentPlanarFrame(frame);
						frame = NULL;
						continue;
					}
					if (frame->frame_format != UVC_FRAME_FORMAT_UNKNOWN)
						frame = draw_preview_one(frame, &mPreviewWindow, nullptr,
							PREVIEW_PIXEL_BYTES);
					if (capture_thread_joinable) {
						addCaptureFrame(frame);
					} else if (capture_frame_callback_enabled && env) {
						do_capture_callback(env, frame);
					} else if (preview_frame_callback_enabled && env) {
						do_preview_frame_callback(env, frame);
					} else if (frame) {
						recycle_frame(frame);
					}
				}
			}
		}
		if (capture_thread_joinable)
			pthread_cond_signal(&capture_sync);
#if LOCAL_DEBUG
		LOGI("preview_thread_func:wait for all callbacks complete");
#endif
		stopMjpegDecodeWorker();
		uvc_stop_streaming(mDeviceHandle);
		LOGI("startup-diag:uvc_stop_streaming called");
#if LOCAL_DEBUG
		LOGI("Streaming finished");
#endif
	} else {
		uvc_perror(result, "failed start_streaming");
	}
	if (preview_thread_attached && vm)
		vm->DetachCurrentThread();

	EXIT();
}

/** Row-by-row; fixed-height safe (prior 8× unroll skipped tail rows → corruption) */
static void copyFrame(const uint8_t *src, uint8_t *dest, const int row_bytes,
	const int height, const int stride_src, const int stride_dest) {
	for (int row_ix = 0; row_ix < height; row_ix++) {
		memcpy(dest, src, row_bytes);
		dest += stride_dest;
		src += stride_src;
	}
}


// transfer specific frame data to the Surface(ANativeWindow)
int copyToSurface(uvc_frame_t *frame, ANativeWindow **window,
	uint64_t *frame_ready_ns, uint64_t *surface_wait_ns) {
	// ENTER();
	int result = 0;
	if (LIKELY(*window)) {
		ANativeWindow_Buffer buffer;
		const uint64_t lock_start_ns = processing_now_ns();
		if (LIKELY(ANativeWindow_lock(*window, &buffer, NULL) == 0)) {
			if (surface_wait_ns)
				*surface_wait_ns += processing_now_ns() - lock_start_ns;
			// source = frame data
			const uint8_t *src = (uint8_t *)frame->data;
			const uint32_t expected_step = frame->width * PREVIEW_PIXEL_BYTES;
			const int src_step = (frame->step > 0 &&
				(size_t) frame->step >= expected_step)
				? (int) frame->step
				: (int) expected_step;
			const int row_bytes = frame->width * PREVIEW_PIXEL_BYTES;
			// destination = Surface(ANativeWindow)
			uint8_t *dest = (uint8_t *)buffer.bits;
			const int dest_w = buffer.width * PREVIEW_PIXEL_BYTES;
			const int dest_step = buffer.stride * PREVIEW_PIXEL_BYTES;
			// use lower transfer bytes
			const int w = std::min(row_bytes, dest_w);
			const int transfer_h =
				std::min((int) frame->height, buffer.height);
			copyFrame(src, dest, w, transfer_h, src_step, dest_step);
			if (frame_ready_ns)
				*frame_ready_ns = processing_now_ns();
			ANativeWindow_unlockAndPost(*window);
		} else {
			result = -1;
		}
	} else {
		result = -1;
	}
	return result; //RETURN(result, int);
}

// changed to return original frame instead of returning converted frame even if convert_func is not null.
uvc_frame_t *UVCPreview::draw_preview_one(uvc_frame_t *frame, ANativeWindow **window, convFunc_t convert_func, int pixcelBytes) {
	// ENTER();

	int b = 0;
	pthread_mutex_lock(&preview_mutex);
	{
		b = *window != NULL;
	}
	pthread_mutex_unlock(&preview_mutex);
	if (LIKELY(b)) {
		uvc_frame_t *converted;
		if (convert_func) {
			converted = get_frame(frame->width * frame->height * pixcelBytes);
			if LIKELY(converted) {
				const uint64_t t_convert = processing_now_ns();
				b = convert_func(frame, converted);
				recordPreviewConversionTiming(processing_now_ns() - t_convert);
				if (!b) {
					pthread_mutex_lock(&preview_mutex);
					const uint64_t t_copy = processing_now_ns();
					uint64_t frame_ready_ns = 0;
					uint64_t surface_wait_ns = 0;
					if (copyToSurface(converted, window, &frame_ready_ns, &surface_wait_ns) == 0) {
						const uint64_t t_end = frame_ready_ns ? frame_ready_ns : processing_now_ns();
						recordSurfaceCopyTiming(t_end - t_copy);
						if (frame->arrival_monotonic_ns)
							recordEndToEndLatencyTiming(frame->arrival_monotonic_ns,
								t_end);
					}
					pthread_mutex_unlock(&preview_mutex);
				} else {
					LOGE("failed converting");
				}
				recycle_frame(converted);
			}
		} else {
			pthread_mutex_lock(&preview_mutex);
			const uint64_t t_copy = processing_now_ns();
			uint64_t frame_ready_ns = 0;
			uint64_t surface_wait_ns = 0;
			if (copyToSurface(frame, window, &frame_ready_ns, &surface_wait_ns) == 0) {
				const uint64_t t_end = frame_ready_ns ? frame_ready_ns : processing_now_ns();
				recordSurfaceCopyTiming(t_end - t_copy);
				if (frame->arrival_monotonic_ns)
					recordEndToEndLatencyTiming(frame->arrival_monotonic_ns, t_end);
			}
			pthread_mutex_unlock(&preview_mutex);
		}
	}
	return frame; //RETURN(frame, uvc_frame_t *);
}

//======================================================================
//
//======================================================================
inline const bool UVCPreview::isCapturing() const { return mIsCapturing; }

int UVCPreview::setCaptureDisplay(ANativeWindow *capture_window) {
	ENTER();
	pthread_mutex_lock(&capture_mutex);
	{
		if (isRunning() && isCapturing()) {
			mIsCapturing = false;
			if (mCaptureWindow) {
				pthread_cond_signal(&capture_sync);
				pthread_cond_wait(&capture_sync, &capture_mutex);	// wait finishing capturing
			}
		}
		if (mCaptureWindow != capture_window) {
			// release current Surface if already assigned.
			if (UNLIKELY(mCaptureWindow))
				ANativeWindow_release(mCaptureWindow);
			mCaptureWindow = capture_window;
			// if you use Surface came from MediaCodec#createInputSurface
			// you could not change window format at least when you use
			// ANativeWindow_lock / ANativeWindow_unlockAndPost
			// to write frame data to the Surface...
			// So we need check here.
			if (mCaptureWindow) {
				ANativeWindow_setBuffersGeometry(mCaptureWindow,
					frameWidth, frameHeight, previewFormat);
				int32_t window_format = ANativeWindow_getFormat(mCaptureWindow);
				if ((window_format != WINDOW_FORMAT_RGB_565)
					&& (previewFormat == WINDOW_FORMAT_RGB_565)) {
					LOGE("window format mismatch, cancelled movie capturing.");
					ANativeWindow_release(mCaptureWindow);
					mCaptureWindow = NULL;
				}
			}
		}
		if (mCaptureWindow && isRunning() && !capture_thread_joinable) {
			if (pthread_create(&capture_thread, NULL, capture_thread_func, (void *)this) != 0) {
				LOGW("UVCPreview::setCaptureDisplay pthread_create capture_thread failed");
			} else {
				capture_thread_joinable = true;
			}
		}
	}
	pthread_mutex_unlock(&capture_mutex);
	RETURN(0, int);
}

void UVCPreview::addCaptureFrame(uvc_frame_t *frame) {
	pthread_mutex_lock(&capture_mutex);
	if (LIKELY(isRunning())) {
		// keep only latest one
		if (captureQueu) {
			recycle_frame(captureQueu);
		}
		captureQueu = frame;
		pthread_cond_broadcast(&capture_sync);
	}
	pthread_mutex_unlock(&capture_mutex);
}

/**
 * get frame data for capturing, if not exist, block and wait
 */
uvc_frame_t *UVCPreview::waitCaptureFrame() {
	uvc_frame_t *frame = NULL;
	pthread_mutex_lock(&capture_mutex);
	{
		while (LIKELY(isRunning()) && !captureQueu)
			pthread_cond_wait(&capture_sync, &capture_mutex);
		if (LIKELY(isRunning() && captureQueu)) {
			frame = captureQueu;
			captureQueu = NULL;
		}
	}
	pthread_mutex_unlock(&capture_mutex);
	return frame;
}

/**
 * clear drame data for capturing
 */
void UVCPreview::clearCaptureFrame() {
	pthread_mutex_lock(&capture_mutex);
	{
		if (captureQueu)
			recycle_frame(captureQueu);
		captureQueu = NULL;
	}
	pthread_mutex_unlock(&capture_mutex);
}

//======================================================================
/*
 * thread function
 * @param vptr_args pointer to UVCPreview instance
 */
// static
void *UVCPreview::capture_thread_func(void *vptr_args) {
	int result;

	ENTER();
#if defined(__ANDROID__)
	consolation_tune_thread_latency("UVC-cap");
#endif
	UVCPreview *preview = reinterpret_cast<UVCPreview *>(vptr_args);
	if (LIKELY(preview)) {
		JavaVM *vm = getVM();
		JNIEnv *env;
		// attach to JavaVM
		vm->AttachCurrentThread(&env, NULL);
		preview->do_capture(env);	// never return until finish previewing
		// detach from JavaVM
		vm->DetachCurrentThread();
		MARK("DetachCurrentThread");
	}
	PRE_EXIT();
	pthread_exit(NULL);
}

/**
 * the actual function for capturing
 */
void UVCPreview::do_capture(JNIEnv *env) {

	ENTER();

	clearCaptureFrame();
	callbackPixelFormatChanged();
	for (; isRunning() ;) {
		mIsCapturing = true;
		if (mCaptureWindow) {
			do_capture_surface(env);
		} else {
			do_capture_idle_loop(env);
		}
		pthread_cond_broadcast(&capture_sync);
	}	// end of for (; isRunning() ;)
	EXIT();
}

void UVCPreview::do_capture_idle_loop(JNIEnv *env) {
	ENTER();
	
	for (; isRunning() && isCapturing() ;) {
		do_capture_callback(env, waitCaptureFrame());
	}
	
	EXIT();
}

/**
 * write frame data to Surface for capturing
 */
void UVCPreview::do_capture_surface(JNIEnv *env) {
	ENTER();

	uvc_frame_t *frame = NULL;
	uvc_frame_t *converted = NULL;

	for (; isRunning() && isCapturing() ;) {
		frame = waitCaptureFrame();
		if (LIKELY(frame)) {
			if (frame->frame_format == UVC_FRAME_FORMAT_UNKNOWN) {
				do_capture_callback(env, frame);
				continue;
			}
			bool fused_into_callback = false;
			if LIKELY(isCapturing()) {
				bool conv_ok = false;
				uvc_frame_t *rgbx_for_callback = NULL;
				if (frame->frame_format == UVC_FRAME_FORMAT_RGBX) {
					conv_ok = true;
					rgbx_for_callback = frame;
					if (LIKELY(mCaptureWindow)) {
						const uint64_t t_copy = processing_now_ns();
						copyToSurface(frame, &mCaptureWindow);
						recordSurfaceCopyTiming(processing_now_ns() - t_copy);
					}
				} else {
					if (UNLIKELY(!converted))
						converted = get_frame(previewBytes);
					rgbx_for_callback = converted;
				}
				if (!conv_ok && LIKELY(converted)) {
					const uint64_t t_convert = processing_now_ns();
					int b_conv = uvc_any2rgbx(frame, converted);
					recordPreviewConversionTiming(processing_now_ns() - t_convert);
					if (LIKELY(!b_conv)) {
						conv_ok = true;
						if (LIKELY(mCaptureWindow)) {
							const uint64_t t_copy = processing_now_ns();
							copyToSurface(converted, &mCaptureWindow);
							recordSurfaceCopyTiming(processing_now_ns() - t_copy);
						}
					}
				}
				if (conv_ok && mFrameCallbackObj && mPixelFormat == PIXEL_FORMAT_RGBX &&
					mFrameCallbackFunc != NULL) {
					do_capture_callback(env, frame, true, rgbx_for_callback);
					fused_into_callback = true;
				}
			}
			if (!fused_into_callback)
				do_capture_callback(env, frame);
		}
	}
	if (converted) {
		recycle_frame(converted);
	}
	if (mCaptureWindow) {
		ANativeWindow_release(mCaptureWindow);
		mCaptureWindow = NULL;
	}

	EXIT();
}

/**
 * call preview-frame callback if one is registered. This is intentionally separate from
 * capture callbacks so preview-only apps can observe frames without enabling capture work.
 */
void UVCPreview::do_preview_frame_callback(JNIEnv *env, uvc_frame_t *frame) {
	ENTER();

	if UNLIKELY(!frame) {
		EXIT();
		return;
	}

	jobject local_cb_obj = nullptr;
	jmethodID on_frame_mid = nullptr;
	size_t callback_bytes = 0;

	pthread_mutex_lock(&capture_mutex);
	if LIKELY(mPreviewFrameCallbackObj) {
		local_cb_obj = env->NewLocalRef(mPreviewFrameCallbackObj);
		on_frame_mid = preview_iframecallback_fields.onFrame;
		callback_bytes = previewCallbackPixelBytes;
	}
	pthread_mutex_unlock(&capture_mutex);

	if UNLIKELY(env->ExceptionCheck()) {
		env->ExceptionClear();
		recycle_frame(frame);
		if (local_cb_obj)
			env->DeleteLocalRef(local_cb_obj);
		EXIT();
		return;
	}

	if UNLIKELY(local_cb_obj == nullptr || on_frame_mid == nullptr) {
		recycle_frame(frame);
		if (local_cb_obj)
			env->DeleteLocalRef(local_cb_obj);
		EXIT();
		return;
	}

	const size_t frame_bytes = frame->actual_bytes > 0 ? frame->actual_bytes : frame->data_bytes;
	const size_t direct_bytes = callback_bytes > 0
		? std::min(callback_bytes, frame_bytes) : frame_bytes;
	jobject buf = env->NewDirectByteBuffer(frame->data, direct_bytes);
	env->CallVoidMethod(local_cb_obj, on_frame_mid, buf);
	env->ExceptionClear();
	env->DeleteLocalRef(buf);
	env->DeleteLocalRef(local_cb_obj);
	recycle_frame(frame);
	EXIT();
}

/**
 * call IFrameCallback#onFrame if needs
 */
void UVCPreview::do_capture_callback(JNIEnv *env, uvc_frame_t *frame,
	bool fused_rgbx, uvc_frame_t *rgbx_ready) {
	ENTER();

	if UNLIKELY(!frame) {
		EXIT();
		return;
	}

	jobject local_cb_obj = nullptr;
	jmethodID on_frame_mid = nullptr;
	convFunc_t conv_fun = nullptr;
	int pix_fmt = 0;
	size_t pix_callback_bytes = 0;

	pthread_mutex_lock(&capture_mutex);
	if LIKELY(mFrameCallbackObj) {
		local_cb_obj = env->NewLocalRef(mFrameCallbackObj);
		on_frame_mid = iframecallback_fields.onFrame;
		conv_fun = mFrameCallbackFunc;
		pix_fmt = mPixelFormat;
		pix_callback_bytes = callbackPixelBytes;
	}
	pthread_mutex_unlock(&capture_mutex);

	if UNLIKELY(env->ExceptionCheck()) {
		env->ExceptionClear();
		recycle_frame(frame);
		if (local_cb_obj)
			env->DeleteLocalRef(local_cb_obj);
		EXIT();
		return;
	}

	if UNLIKELY(local_cb_obj == nullptr) {
		recycle_frame(frame);
		EXIT();
		return;
	}

	if UNLIKELY(on_frame_mid == nullptr) {
		recycle_frame(frame);
		env->DeleteLocalRef(local_cb_obj);
		EXIT();
		return;
	}

	uvc_frame_t *callback_frame = frame;
	bool skip_recycle_cb_frame = false;

	if (fused_rgbx && rgbx_ready && (pix_fmt == PIXEL_FORMAT_RGBX)) {
		callback_frame = rgbx_ready;
		if (rgbx_ready != frame) {
			recycle_frame(frame);
			frame = nullptr;
			skip_recycle_cb_frame = true;
		}
		jobject buf = env->NewDirectByteBuffer(callback_frame->data,
			pix_callback_bytes);
		env->CallVoidMethod(local_cb_obj, on_frame_mid, buf);
		env->ExceptionClear();
		env->DeleteLocalRef(buf);
	} else if (conv_fun) {
		callback_frame = get_frame(pix_callback_bytes);
		if LIKELY(callback_frame) {
			const uint64_t t_convert = processing_now_ns();
			int convert_err = conv_fun(frame, callback_frame);
			recordCallbackConversionTiming(processing_now_ns() - t_convert);
			recycle_frame(frame);
			frame = nullptr;
			if UNLIKELY(convert_err) {
				LOGW("failed to convert for callback frame");
				recycle_frame(callback_frame);
				callback_frame = nullptr;
			}
		} else {
			LOGW("failed to allocate for callback frame");
			recycle_frame(frame);
			callback_frame = nullptr;
		}
		if (LIKELY(callback_frame)) {
			jobject buf = env->NewDirectByteBuffer(callback_frame->data,
				pix_callback_bytes);
			env->CallVoidMethod(local_cb_obj, on_frame_mid, buf);
			env->ExceptionClear();
			env->DeleteLocalRef(buf);
		}
	} else {
		jobject buf = env->NewDirectByteBuffer(callback_frame->data,
			callback_frame->actual_bytes);
		env->CallVoidMethod(local_cb_obj, on_frame_mid, buf);
		env->ExceptionClear();
		env->DeleteLocalRef(buf);
	}

	if (!skip_recycle_cb_frame && callback_frame && callback_frame != rgbx_ready)
		recycle_frame(callback_frame);
	env->DeleteLocalRef(local_cb_obj);
	EXIT();
}
