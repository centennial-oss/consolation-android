/*
 * UVCCamera
 * library and sample to access to UVC web camera on non-rooted Android device
 *
 * Copyright (c) 2014-2017 saki t_saki@serenegiant.com
 *
 * File name: UVCPreview.h
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

#ifndef UVCPREVIEW_H_
#define UVCPREVIEW_H_

#include "libUVCCamera.h"
#include <stdint.h>
#include <pthread.h>
#include <android/native_window.h>
#include "bounded_pointer_ring.h"
#include "objectarray.h"

#pragma interface

/** Preview FIFO depth; drop-oldest on overflow (see BoundedPointerRing). */
#define PREVIEW_QUEUE_MAX 4

#define DEFAULT_PREVIEW_WIDTH 640
#define DEFAULT_PREVIEW_HEIGHT 480
#define DEFAULT_PREVIEW_FPS_MIN 1
#define DEFAULT_PREVIEW_FPS_MAX 30
#define DEFAULT_PREVIEW_MODE 0
#define DEFAULT_BANDWIDTH 1.0f
#define UVC_PROCESSING_STATS_COUNT 30

typedef uvc_error_t (*convFunc_t)(uvc_frame_t *in, uvc_frame_t *out);

#define PIXEL_FORMAT_RAW 0		// same as PIXEL_FORMAT_YUV
#define PIXEL_FORMAT_YUV 1
#define PIXEL_FORMAT_RGB565 2
#define PIXEL_FORMAT_RGBX 3
#define PIXEL_FORMAT_YUV20SP 4
#define PIXEL_FORMAT_NV21 5		// YVU420SemiPlanar

// for callback to Java object
typedef struct {
	jmethodID onFrame;
} Fields_iframecallback;

class UVCPreview {
private:
	uvc_device_handle_t *mDeviceHandle;
	ANativeWindow *mPreviewWindow;
	volatile bool mIsRunning;
	int requestWidth, requestHeight, requestMode;
	int requestMinFps, requestMaxFps;
	float requestBandwidth;
	int frameWidth, frameHeight;
	int frameMode;
	size_t frameBytes;
	pthread_t preview_thread;
	pthread_mutex_t preview_mutex;
	pthread_cond_t preview_sync;
	/** Incoming frames; fixed ring, O(1) enqueue with drop-oldest on overflow */
	BoundedPointerRing<uvc_frame_t *> preview_frame_ring;
	int previewFormat;
	size_t previewBytes;
//
	volatile bool mIsCapturing;
	ANativeWindow *mCaptureWindow;
	pthread_t capture_thread;
	/** Set only after successful pthread_create for capture_thread (safe pthread_join). */
	volatile bool capture_thread_joinable;
	pthread_mutex_t capture_mutex;
	pthread_cond_t capture_sync;
	uvc_frame_t *captureQueu;			// keep latest frame
	jobject mFrameCallbackObj;
	convFunc_t mFrameCallbackFunc;
	Fields_iframecallback iframecallback_fields;
	int mPixelFormat;
	size_t callbackPixelBytes;
	jobject mPreviewFrameCallbackObj;
	Fields_iframecallback preview_iframecallback_fields;
	int mPreviewPixelFormat;
	size_t previewCallbackPixelBytes;
	volatile bool preview_frame_callback_enabled;
	volatile bool capture_frame_callback_enabled;
	pthread_mutex_t processing_stats_mutex;
	uint64_t processingPreviewConvertCount;
	uint64_t processingPreviewConvertTotalNs;
	uint64_t processingPreviewConvertMaxNs;
	uint64_t processingCallbackConvertCount;
	uint64_t processingCallbackConvertTotalNs;
	uint64_t processingCallbackConvertMaxNs;
	uint64_t processingCopyCount;
	uint64_t processingCopyTotalNs;
	uint64_t processingCopyMaxNs;
	uint64_t processingEndToEndLatencyCount;
	uint64_t processingEndToEndLatencyTotalNs;
	uint64_t processingEndToEndLatencyMaxNs;
	uint64_t processingPayloadCount;
	uint64_t processingPayloadTotalBytes;
	uint64_t processingPayloadMaxBytes;
	uint64_t processingPreviewQueueDropCount;
	uint64_t processingPreviewQueueDepthSampleCount;
	uint64_t processingPreviewQueueDepthTotalMilli;
	uint64_t processingPreviewQueueDepthMaxMilli;
	uint64_t processingPreviewEnqueueDepthSampleCount;
	uint64_t processingPreviewEnqueueDepthTotalMilli;
	uint64_t processingPreviewEnqueueDepthMaxMilli;
	uint64_t processingUvcCallbackCount;
	uint64_t processingUvcCallbackTotalNs;
	uint64_t processingUvcCallbackMaxNs;
	uint64_t processingCallbackLagCount;
	uint64_t processingCallbackLagTotalNs;
	uint64_t processingCallbackLagMaxNs;
	uint64_t processingPreCallbackSkippedFrames;
	uint32_t processingLastUvcSequence;
	bool processingLastUvcSequenceValid;
	uint32_t diagMjpegDecodedCount;
	uint32_t diagMjpegLastLuma;
	volatile uint64_t streamingStartMonotonicNs;
	volatile bool firstFrameLogged;
// improve performance by reducing memory allocation
	pthread_mutex_t pool_mutex;
	ObjectArray<uvc_frame_t *> mFramePool;
	ObjectArray<uvc_frame_t *> mNotificationFramePool;
	uvc_frame_t *get_frame(size_t data_bytes);
	uvc_frame_t *get_notification_frame();
	void recycle_frame(uvc_frame_t *frame);
	void init_pool(size_t data_bytes);
	void clear_pool();
//
	void clearDisplay();
	static void uvc_preview_frame_callback(uvc_frame_t *frame, void *vptr_args);
	uvc_frame_t *convertPreviewFrameToRgbx(uvc_frame_t *frame);
	bool renderFrameDirectToSurface(uvc_frame_t *frame, ANativeWindow **window,
		pthread_mutex_t *window_mutex, uint64_t *frame_ready_ns = NULL,
		uint64_t *surface_wait_ns = NULL);
	uvc_frame_t *createFrameNotification(uvc_frame_t *frame);
	void addPreviewFrame(uvc_frame_t *frame);
	uvc_frame_t *waitPreviewFrame();
	void clearPreviewFrame();
	static void *preview_thread_func(void *vptr_args);
	int prepare_preview(uvc_stream_ctrl_t *ctrl);
	void do_preview(uvc_stream_ctrl_t *ctrl);
	uvc_frame_t *draw_preview_one(uvc_frame_t *frame, ANativeWindow **window, convFunc_t func, int pixelBytes);
//
	void addCaptureFrame(uvc_frame_t *frame);
	uvc_frame_t *waitCaptureFrame();
	void clearCaptureFrame();
	static void *capture_thread_func(void *vptr_args);
	void do_capture(JNIEnv *env);
	void do_capture_surface(JNIEnv *env);
	void do_capture_idle_loop(JNIEnv *env);
	void do_capture_callback(JNIEnv *env, uvc_frame_t *frame,
		bool fused_rgbx = false, uvc_frame_t *rgbx_ready = NULL);
	void do_preview_frame_callback(JNIEnv *env, uvc_frame_t *frame);
	void callbackPixelFormatChanged();
	bool hasCaptureConsumers() const;
	bool hasPreviewFrameCallback() const;
	void recordPreviewConversionTiming(uint64_t duration_ns);
	void recordCallbackConversionTiming(uint64_t duration_ns);
	void recordSurfaceCopyTiming(uint64_t duration_ns);
	void recordEndToEndLatencyDuration(uint64_t duration_ns);
	void recordEndToEndLatencyTiming(uint64_t start_ns, uint64_t end_ns);
	void recordPayloadBytes(size_t bytes);
	void recordPreviewQueueDepthSample(uint64_t depth_frames);
	void recordMjpegDecodedVisualSample(uint32_t sequence, size_t bytes,
		const uint8_t *rgbx, size_t stride_bytes, uint32_t width, uint32_t height);
public:
	UVCPreview(uvc_device_handle_t *devh);
	~UVCPreview();

	inline const bool isRunning() const;
	int setPreviewSize(int width, int height, int min_fps, int max_fps, int mode, float bandwidth = 1.0f);
	int setPreviewDisplay(ANativeWindow *preview_window);
	int setPreviewFrameCallback(JNIEnv *env, jobject frame_callback_obj, int pixel_format);
	int setFrameCallback(JNIEnv *env, jobject frame_callback_obj, int pixel_format);
	int startPreview();
	int stopPreview();
	inline const bool isCapturing() const;
	int setCaptureDisplay(ANativeWindow *capture_window);
	void getAndResetProcessingStats(uint64_t stats[UVC_PROCESSING_STATS_COUNT]);
};

#endif /* UVCPREVIEW_H_ */
