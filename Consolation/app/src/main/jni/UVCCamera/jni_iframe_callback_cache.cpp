/*
 * IFrameCallback JNI method ID cache (primed from JNI_OnLoad path).
 */

#include "jni_iframe_callback_cache.h"
#include "utilbase.h"

static jclass g_iface_iframe_callback;
static jmethodID g_mid_iframe_on_frame;

int consolation_prime_iframe_callback_cache(JNIEnv *env) {
	if (g_iface_iframe_callback)
		return 0;

	jclass local = env->FindClass("org/centennialoss/consolation/uvc/IFrameCallback");
	if UNLIKELY(!local) {
		env->ExceptionClear();
		return 0;
	}
	g_mid_iframe_on_frame = env->GetMethodID(local, "onFrame",
		"(Ljava/nio/ByteBuffer;)V");
	if UNLIKELY(!g_mid_iframe_on_frame) {
		env->ExceptionClear();
		env->DeleteLocalRef(local);
		return 0;
	}
	g_iface_iframe_callback = reinterpret_cast<jclass>(env->NewGlobalRef(local));
	env->DeleteLocalRef(local);
	return 0;
}

int consolation_cleanup_iframe_callback_cache(JNIEnv *env) {
	if (g_iface_iframe_callback) {
		env->DeleteGlobalRef(g_iface_iframe_callback);
		g_iface_iframe_callback = NULL;
	}
	g_mid_iframe_on_frame = NULL;
	return 0;
}

jmethodID consolation_resolve_iframe_on_frame_mid(JNIEnv *env, jobject receiver) {
	if UNLIKELY(!receiver)
		return NULL;

	jclass object_class = env->GetObjectClass(receiver);
	if UNLIKELY(!object_class)
		return NULL;

	jmethodID mid = NULL;
	if (g_iface_iframe_callback && g_mid_iframe_on_frame) {
		if (env->IsAssignableFrom(object_class, g_iface_iframe_callback))
			mid = g_mid_iframe_on_frame;
	}
	if UNLIKELY(!mid)
		mid = env->GetMethodID(object_class, "onFrame",
			"(Ljava/nio/ByteBuffer;)V");

	env->DeleteLocalRef(object_class);
	return mid;
}

