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

} // namespace

#define	LOCAL_DEBUG 0
#define PREVIEW_PIXEL_BYTES 4	// RGBA/RGBX
/** Enough headroom for queue + MJPEG decompress + RGBX convert + capture */
#define FRAME_POOL_SZ (PREVIEW_QUEUE_MAX + 8)
#define REQUEST_MODE_YUYV 0
#define REQUEST_MODE_MJPEG 1
#define REQUEST_MODE_H264 2
#define REQUEST_MODE_NV12 3
#define REQUEST_MODE_P010 4

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
	previewFormat(WINDOW_FORMAT_RGBX_8888),
	previewBytes(DEFAULT_PREVIEW_WIDTH * DEFAULT_PREVIEW_HEIGHT * PREVIEW_PIXEL_BYTES),
	mIsRunning(false),
	mIsCapturing(false),
	capture_thread_joinable(false),
	captureQueu(NULL),
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
	streamingStartMonotonicNs(0),
	firstFrameLogged(false) {

	ENTER();
	pthread_cond_init(&preview_sync, NULL);
	pthread_mutex_init(&preview_mutex, NULL);
//
	pthread_cond_init(&capture_sync, NULL);
	pthread_mutex_init(&capture_mutex, NULL);
	pthread_mutex_init(&processing_stats_mutex, NULL);
//	
	pthread_mutex_init(&pool_mutex, NULL);
	iframecallback_fields.onFrame = nullptr;
	preview_iframecallback_fields.onFrame = nullptr;
	EXIT();
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
	clearPreviewFrame();
	clearCaptureFrame();
	clear_pool();
	pthread_mutex_destroy(&preview_mutex);
	pthread_cond_destroy(&preview_sync);
	pthread_mutex_destroy(&capture_mutex);
	pthread_cond_destroy(&capture_sync);
	pthread_mutex_destroy(&processing_stats_mutex);
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
	pthread_mutex_lock(&processing_stats_mutex);
	processingPreviewConvertCount++;
	processingPreviewConvertTotalNs += duration_ns;
	if (duration_ns > processingPreviewConvertMaxNs)
		processingPreviewConvertMaxNs = duration_ns;
	pthread_mutex_unlock(&processing_stats_mutex);
}

void UVCPreview::recordCallbackConversionTiming(uint64_t duration_ns) {
	pthread_mutex_lock(&processing_stats_mutex);
	processingCallbackConvertCount++;
	processingCallbackConvertTotalNs += duration_ns;
	if (duration_ns > processingCallbackConvertMaxNs)
		processingCallbackConvertMaxNs = duration_ns;
	pthread_mutex_unlock(&processing_stats_mutex);
}

void UVCPreview::recordSurfaceCopyTiming(uint64_t duration_ns) {
	pthread_mutex_lock(&processing_stats_mutex);
	processingCopyCount++;
	processingCopyTotalNs += duration_ns;
	if (duration_ns > processingCopyMaxNs)
		processingCopyMaxNs = duration_ns;
	pthread_mutex_unlock(&processing_stats_mutex);
}

void UVCPreview::recordEndToEndLatencyTiming(uint64_t start_ns, uint64_t end_ns) {
	if (!start_ns || end_ns <= start_ns)
		return;
	const uint64_t duration_ns = end_ns - start_ns;
	pthread_mutex_lock(&processing_stats_mutex);
	processingEndToEndLatencyCount++;
	processingEndToEndLatencyTotalNs += duration_ns;
	if (duration_ns > processingEndToEndLatencyMaxNs)
		processingEndToEndLatencyMaxNs = duration_ns;
	pthread_mutex_unlock(&processing_stats_mutex);
}

void UVCPreview::recordPayloadBytes(size_t bytes) {
	pthread_mutex_lock(&processing_stats_mutex);
	processingPayloadCount++;
	processingPayloadTotalBytes += bytes;
	if (bytes > processingPayloadMaxBytes)
		processingPayloadMaxBytes = bytes;
	pthread_mutex_unlock(&processing_stats_mutex);
}

void UVCPreview::recordPreviewQueueDepthSample(uint64_t depth_frames) {
	pthread_mutex_lock(&processing_stats_mutex);
	processingPreviewQueueDepthSampleCount++;
	processingPreviewQueueDepthTotalMilli += depth_frames * 1000ULL;
	pthread_mutex_unlock(&processing_stats_mutex);
}

void UVCPreview::getAndResetProcessingStats(uint64_t stats[14]) {
	pthread_mutex_lock(&processing_stats_mutex);
	stats[0] = processingPreviewConvertCount;
	stats[1] = processingEndToEndLatencyCount
		? processingEndToEndLatencyTotalNs / processingEndToEndLatencyCount : 0;
	stats[2] = processingPreviewConvertMaxNs;
	stats[3] = processingCallbackConvertCount;
	stats[4] = processingCallbackConvertCount
		? processingCallbackConvertTotalNs / processingCallbackConvertCount : 0;
	stats[5] = processingCallbackConvertMaxNs;
	stats[6] = processingCopyCount;
	stats[7] = processingCopyCount
		? processingCopyTotalNs / processingCopyCount : 0;
	stats[8] = processingCopyMaxNs;
	stats[9] = processingPayloadCount;
	stats[10] = processingPayloadCount
		? processingPayloadTotalBytes / processingPayloadCount : 0;
	stats[11] = processingPayloadMaxBytes;
	stats[12] = processingPreviewQueueDropCount;
	stats[13] = processingPreviewQueueDepthSampleCount
		? processingPreviewQueueDepthTotalMilli / processingPreviewQueueDepthSampleCount : 0;
	processingPreviewConvertCount = 0;
	processingPreviewConvertTotalNs = 0;
	processingPreviewConvertMaxNs = 0;
	processingCallbackConvertCount = 0;
	processingCallbackConvertTotalNs = 0;
	processingCallbackConvertMaxNs = 0;
	processingCopyCount = 0;
	processingCopyTotalNs = 0;
	processingCopyMaxNs = 0;
	processingEndToEndLatencyCount = 0;
	processingEndToEndLatencyTotalNs = 0;
	processingEndToEndLatencyMaxNs = 0;
	processingPayloadCount = 0;
	processingPayloadTotalBytes = 0;
	processingPayloadMaxBytes = 0;
	processingPreviewQueueDropCount = 0;
	processingPreviewQueueDepthSampleCount = 0;
	processingPreviewQueueDepthTotalMilli = 0;
	pthread_mutex_unlock(&processing_stats_mutex);
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
			mPreviewWindow = preview_window;
			if (LIKELY(mPreviewWindow)) {
				ANativeWindow_setBuffersGeometry(mPreviewWindow,
					frameWidth, frameHeight, previewFormat);
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
			pthread_mutex_lock(&preview_mutex);
			{
				pthread_cond_signal(&preview_sync);
			}
			pthread_mutex_unlock(&preview_mutex);
		}
	}
	RETURN(result, int);
}

int UVCPreview::stopPreview() {
	ENTER();
	bool b = isRunning();
	if (LIKELY(b)) {
		mIsRunning = false;
		pthread_cond_signal(&preview_sync);
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
int copyToSurface(uvc_frame_t *frame, ANativeWindow **window, uint64_t *after_post_ns);

void UVCPreview::uvc_preview_frame_callback(uvc_frame_t *frame, void *vptr_args) {
	UVCPreview *preview = reinterpret_cast<UVCPreview *>(vptr_args);
	if UNLIKELY(!preview->isRunning() || !frame || !frame->frame_format || !frame->data || !frame->data_bytes) return;
	if (!preview->firstFrameLogged) {
		preview->firstFrameLogged = true;
		const uint64_t t0 = preview->streamingStartMonotonicNs;
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
		((frame->frame_format != UVC_FRAME_FORMAT_MJPEG) && (frame->actual_bytes < preview->frameBytes))
		|| (frame->width != preview->frameWidth) || (frame->height != preview->frameHeight) )) {

#if LOCAL_DEBUG
		LOGD("broken frame!:format=%d,actual_bytes=%d/%d(%d,%d/%d,%d)",
			frame->frame_format, frame->actual_bytes, preview->frameBytes,
			frame->width, frame->height, preview->frameWidth, preview->frameHeight);
#endif
		return;
	}
	if (LIKELY(preview->isRunning())) {
		preview->recordPayloadBytes(frame->actual_bytes);

		if (preview->frameMode != REQUEST_MODE_H264) {
			const bool has_preview_callback = preview->hasPreviewFrameCallback();
			const bool has_capture_consumers = preview->hasCaptureConsumers();
			uint64_t preview_post_ns = 0;
			bool preview_rendered = false;

			if (UNLIKELY(preview->capture_thread_joinable)) {
				uvc_frame_t *rgbx = preview->convertPreviewFrameToRgbx(frame);
				if (LIKELY(rgbx)) {
					pthread_mutex_lock(&preview->preview_mutex);
					const uint64_t t_copy = processing_now_ns();
					if (copyToSurface(rgbx, &preview->mPreviewWindow, &preview_post_ns) == 0) {
						const uint64_t t_end = preview_post_ns ? preview_post_ns : processing_now_ns();
						preview->recordSurfaceCopyTiming(t_end - t_copy);
						preview->recordEndToEndLatencyTiming(frame->arrival_monotonic_ns, t_end);
					}
					pthread_mutex_unlock(&preview->preview_mutex);

					if (preview->capture_thread_joinable) {
						preview->addCaptureFrame(rgbx);
					} else {
						preview->addPreviewFrame(rgbx);
					}
					return;
				}
			}

			preview_rendered = preview->renderFrameDirectToSurface(frame,
				&preview->mPreviewWindow, &preview->preview_mutex, &preview_post_ns);
			if (preview_rendered && preview_post_ns)
				preview->recordEndToEndLatencyTiming(frame->arrival_monotonic_ns, preview_post_ns);

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

bool UVCPreview::renderFrameDirectToSurface(uvc_frame_t *frame,
	ANativeWindow **window, pthread_mutex_t *window_mutex,
	uint64_t *after_post_ns) {
	bool rendered = false;

	pthread_mutex_lock(window_mutex);
	ANativeWindow *target = *window;
	if (LIKELY(target)) {
		ANativeWindow_Buffer buffer;
		if (LIKELY(ANativeWindow_lock(target, &buffer, NULL) == 0)) {
			if (LIKELY(buffer.bits && buffer.width >= (int32_t) frame->width &&
					buffer.height >= (int32_t) frame->height)) {
				uvc_frame_t surface = {};
				surface.data = buffer.bits;
				surface.data_bytes = (size_t) buffer.stride * (size_t) buffer.height
					* PREVIEW_PIXEL_BYTES;
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
			}
			ANativeWindow_unlockAndPost(target);
			if (rendered && after_post_ns)
				*after_post_ns = processing_now_ns();
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
		recycle_frame(rgbx);
		return NULL;
	}
	rgbx->arrival_monotonic_ns = frame->arrival_monotonic_ns;
	return rgbx;
}

void UVCPreview::addPreviewFrame(uvc_frame_t *frame) {

	pthread_mutex_lock(&preview_mutex);
	if (isRunning()) {
		uvc_frame_t *drop = preview_frame_ring.enqueue_drop_oldest_if_full(frame);
		if UNLIKELY(drop) {
			pthread_mutex_lock(&processing_stats_mutex);
			processingPreviewQueueDropCount++;
			pthread_mutex_unlock(&processing_stats_mutex);
			recycle_frame(drop);
		}
		frame = nullptr;
		pthread_cond_signal(&preview_sync);
	}
	pthread_mutex_unlock(&preview_mutex);
	if (frame)
		recycle_frame(frame);
}

uvc_frame_t *UVCPreview::waitPreviewFrame() {
	uvc_frame_t *frame = nullptr;
	pthread_mutex_lock(&preview_mutex);
	{
		while (isRunning() && preview_frame_ring.empty())
			pthread_cond_wait(&preview_sync, &preview_mutex);
		if (LIKELY(isRunning() && !preview_frame_ring.empty())) {
			frame = preview_frame_ring.dequeue();
			recordPreviewQueueDepthSample(static_cast<uint64_t>(preview_frame_ring.size()));
		}
	}
	pthread_mutex_unlock(&preview_mutex);
	return frame;
}

void UVCPreview::clearPreviewFrame() {
	pthread_mutex_lock(&preview_mutex);
	{
		while (!preview_frame_ring.empty())
			recycle_frame(preview_frame_ring.dequeue());
		preview_frame_ring.reset_storage();
	}
	pthread_mutex_unlock(&preview_mutex);
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
			if (requestMode == REQUEST_MODE_NV12)
				frameBytes = (wpx * hpx * 3) / 2;
			else if (requestMode == REQUEST_MODE_P010)
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
	uint64_t *after_post_ns = NULL) {
	// ENTER();
	int result = 0;
	if (LIKELY(*window)) {
		ANativeWindow_Buffer buffer;
		if (LIKELY(ANativeWindow_lock(*window, &buffer, NULL) == 0)) {
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
			ANativeWindow_unlockAndPost(*window);
			if (after_post_ns)
				*after_post_ns = processing_now_ns();
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
					uint64_t after_post_ns = 0;
					if (copyToSurface(converted, window, &after_post_ns) == 0) {
						const uint64_t t_end = after_post_ns ? after_post_ns : processing_now_ns();
						recordSurfaceCopyTiming(t_end - t_copy);
						recordEndToEndLatencyTiming(frame->arrival_monotonic_ns, t_end);
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
			uint64_t after_post_ns = 0;
			if (copyToSurface(frame, window, &after_post_ns) == 0) {
				const uint64_t t_end = after_post_ns ? after_post_ns : processing_now_ns();
				recordSurfaceCopyTiming(t_end - t_copy);
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
