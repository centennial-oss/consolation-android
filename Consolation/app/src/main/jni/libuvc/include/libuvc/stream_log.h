#ifndef LIBUVC_STREAM_LOG_H
#define LIBUVC_STREAM_LOG_H

#define LOCAL_DEBUG 0

#ifndef LOG_TAG
#define LOG_TAG "libuvc/stream"
#endif
#if 1	// デバッグ情報を出さない時1
	#ifndef LOG_NDEBUG
		#define	LOG_NDEBUG		// LOGV/LOGD/MARKを出力しない時
		#endif
	#undef USE_LOGALL			// 指定したLOGxだけを出力
#else
	#define USE_LOGALL
	#undef LOG_NDEBUG
	#undef NDEBUG
	#define GET_RAW_DESCRIPTOR
#endif

#endif /* LIBUVC_STREAM_LOG_H */
