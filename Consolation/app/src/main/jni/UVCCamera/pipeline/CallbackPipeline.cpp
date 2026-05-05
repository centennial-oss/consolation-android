//
// Created by saki on 15/11/07.
//

#if 1	// set 1 if you don't need debug message
	#ifndef LOG_NDEBUG
		#define	LOG_NDEBUG		// ignore LOGV/LOGD/MARK
	#endif
	#undef USE_LOGALL
#else
	#define USE_LOGALL
	#undef LOG_NDEBUG
	#undef NDEBUG		// depends on definition in Android.mk and Application.mk
#endif

#include "utilbase.h"
#include "common_utils.h"
#include "libUVCCamera.h"
#include "jni_iframe_callback_cache.h"

#include "pipeline_helper.h"
#include "IPipeline.h"
#include "CallbackPipeline.h"

#define INIT_FRAME_POOL_SZ 2
#define MAX_FRAME_NUM 2

CallbackPipeline::CallbackPipeline(const size_t &_data_bytes)
:	CaptureBasePipeline(MAX_FRAME_NUM, INIT_FRAME_POOL_SZ, _data_bytes),
	mFrameCallbackFunc(NULL),
	callbackPixelBytes(0)
{
	ENTER();

	setState(PIPELINE_STATE_INITIALIZED);

	EXIT();
}

CallbackPipeline::~CallbackPipeline() {
	ENTER();

	EXIT();
}

int CallbackPipeline::setFrameCallback(JNIEnv *env, jobject frame_callback_obj, int pixel_format) {

	ENTER();
	Mutex::Autolock lock(capture_mutex);

	if (isRunning() && isCapturing()) {
		mIsCapturing = false;
		if (mFrameCallbackObj) {
			capture_sync.signal();
			capture_sync.wait(capture_mutex);	// wait finishing capturing
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
	}
	RETURN(0, int);
}

void CallbackPipeline::callbackPixelFormatChanged(const uint32_t &width, const uint32_t &height) {
	mFrameCallbackFunc = NULL;
	const size_t sz = width * height;
	switch (mPixelFormat) {
	  case PIXEL_FORMAT_RAW:
		LOGI("PIXEL_FORMAT_RAW:");
		callbackPixelBytes = sz * 2;
		break;
	  case PIXEL_FORMAT_YUV:
		LOGI("PIXEL_FORMAT_YUV:");
		mFrameCallbackFunc = uvc_any2yuyv;
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
		mFrameCallbackFunc = uvc_any2yuv420SP;
		callbackPixelBytes = (sz * 3) / 2;
		break;
	  case PIXEL_FORMAT_NV21:
		LOGI("PIXEL_FORMAT_NV21:");
		mFrameCallbackFunc = uvc_any2iyuv420SP;
		callbackPixelBytes = (sz * 3) / 2;
		break;
	}
}

void CallbackPipeline::do_capture(JNIEnv *env) {
	ENTER();

	uvc_frame_t *frame;
	uvc_frame_t *temp = get_frame(default_frame_size);
	uvc_frame_t *callback_frame;
	uint32_t width = 0, height = 0;
	size_t sz = default_frame_size;

	if (LIKELY(temp)) {
		for (; isRunning() && isCapturing();) {
			frame = waitCaptureFrame();
			if ((LIKELY(frame))) {
				if (UNLIKELY((width != frame->width) || (height != frame->height))) {
					width = frame->width;
					height = frame->height;
					callbackPixelFormatChanged(width, height);
					uvc_ensure_frame_size(temp, callbackPixelBytes);
					sz = callbackPixelBytes;
				}
				if (mFrameCallbackObj) {
					callback_frame = frame;
					sz = frame->actual_bytes;
					if (mFrameCallbackFunc) {
						callback_frame = temp;
						sz = callbackPixelBytes;
						int b = mFrameCallbackFunc(frame, temp);
						if (UNLIKELY(b)) {
							LOGW("failed to convert to callback frame");
							goto SKIP;
						}
					}
					if (callback_frame->data_bytes > 0 &&
						sz > callback_frame->data_bytes)
						sz = callback_frame->data_bytes;
					jobject buf = env->NewDirectByteBuffer(callback_frame->data, sz);
					env->CallVoidMethod(mFrameCallbackObj, iframecallback_fields.onFrame, buf);
					env->ExceptionClear();
					env->DeleteLocalRef(buf);
				}
SKIP:
				recycle_frame(frame);
			}
		}
		recycle_frame(temp);
	}

	EXIT();
}

//**********************************************************************
//
//**********************************************************************
static ID_TYPE nativeCreate(JNIEnv *env, jobject thiz) {

	ENTER();
	CallbackPipeline *pipeline = new CallbackPipeline();
	setField_long(env, thiz, "mNativePtr", reinterpret_cast<ID_TYPE>(pipeline));
	RETURN(reinterpret_cast<ID_TYPE>(pipeline), ID_TYPE);
}

static void nativeDestroy(JNIEnv *env, jobject thiz,
	ID_TYPE id_pipeline) {

	ENTER();
	setField_long(env, thiz, "mNativePtr", 0);
	CallbackPipeline *pipeline = reinterpret_cast<CallbackPipeline *>(id_pipeline);
	if (LIKELY(pipeline)) {
		pipeline->release();
		SAFE_DELETE(pipeline);
	}
	EXIT();
}

static jint nativeGetState(JNIEnv *env, jobject thiz,
	ID_TYPE id_pipeline) {

	ENTER();
	jint result = 0;
	CallbackPipeline *pipeline = reinterpret_cast<CallbackPipeline *>(id_pipeline);
	if (pipeline) {
		result = pipeline->getState();
	}
	RETURN(result, jint);
}

static jint nativeSetPipeline(JNIEnv *env, jobject thiz,
	ID_TYPE id_pipeline, jobject pipeline_obj) {

	ENTER();
	jint result = JNI_ERR;
	CallbackPipeline *callback_pl = reinterpret_cast<CallbackPipeline *>(id_pipeline);
	if (LIKELY(callback_pl && pipeline_obj)) {
		IPipeline *next_pl = getPipeline(env, pipeline_obj);
		if (LIKELY(next_pl))
			result = callback_pl->setPipeline(next_pl);
	}

	RETURN(result, jint);
}

static jint nativeStart(JNIEnv *env, jobject thiz,
	ID_TYPE id_pipeline) {

	ENTER();

	int result = JNI_ERR;
	CallbackPipeline *pipeline = reinterpret_cast<CallbackPipeline *>(id_pipeline);
	if (LIKELY(pipeline)) {
		result = pipeline->start();
	}
	RETURN(result, jint);
}

static jint nativeStop(JNIEnv *env, jobject thiz,
	ID_TYPE id_pipeline) {

	jint result = JNI_ERR;
	ENTER();
	CallbackPipeline *pipeline = reinterpret_cast<CallbackPipeline *>(id_pipeline);
	if (LIKELY(pipeline)) {
		result = pipeline->stop();
	}
	RETURN(result, jint);
}

static jint nativeSetFrameCallback(JNIEnv *env, jobject thiz,
	ID_TYPE id_pipeline, jobject jIFrameCallback, jint pixel_format) {

	jint result = JNI_ERR;
	ENTER();
	CallbackPipeline *pipeline = reinterpret_cast<CallbackPipeline *>(id_pipeline);
	if (LIKELY(pipeline)) {
		jobject frame_callback_obj = env->NewGlobalRef(jIFrameCallback);
		result = pipeline->setFrameCallback(env, frame_callback_obj, pixel_format);
	}
	RETURN(result, jint);
}

//**********************************************************************
//
//**********************************************************************
static JNINativeMethod methods[] = {
	{ "nativeCreate",					"()J", (void *) nativeCreate },
	{ "nativeDestroy",					"(J)V", (void *) nativeDestroy },

	{ "nativeGetState",					"(J)I", (void *) nativeGetState },
	{ "nativeSetPipeline",				"(JLorg/centennialoss/consolation/uvc/IPipeline;)I", (void *) nativeSetPipeline },

	{ "nativeStart",					"(J)I", (void *) nativeStart },
	{ "nativeStop",						"(J)I", (void *) nativeStop },

	{ "nativeSetFrameCallback",			"(JLorg/centennialoss/consolation/uvc/IFrameCallback;I)I", (void *) nativeSetFrameCallback },
};

int register_callback_pipeline(JNIEnv *env) {
	LOGV("register_callback_pipeline:");
	if (registerNativeMethods(env,
		"org/centennialoss/consolation/uvc/FrameCallbackPipeline",
		methods, NUM_ARRAY_ELEMENTS(methods)) < 0) {
		return -1;
	}
    return 0;
}
