/*
 * One-time cache for IFrameCallback#onFrame — avoids repeated GetMethodID on hot path.
 */

#ifndef JNI_IFRAME_CALLBACK_CACHE_H_
#define JNI_IFRAME_CALLBACK_CACHE_H_

#include <jni.h>

/** Best-effort; safe to call multiple times. Returns 0. */
int consolation_prime_iframe_callback_cache(JNIEnv *env);

/**
 * Resolve onFrame for this receiver; uses cache when the object implements
 * org.centennialoss.consolation.uvc.IFrameCallback.
 */
jmethodID consolation_resolve_iframe_on_frame_mid(JNIEnv *env, jobject receiver);

#endif /* JNI_IFRAME_CALLBACK_CACHE_H_ */
