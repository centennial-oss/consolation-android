/* Android ndk-build internal config for libjpeg-turbo 3.1.4.1. */
#define BUILD  "20260327"
#define HIDDEN  __attribute__((visibility("hidden")))
#define INLINE  inline __attribute__((always_inline))
#define THREAD_LOCAL  __thread
#define PACKAGE_NAME  "libjpeg-turbo"
#define VERSION  "3.1.4.1"

#if defined(__LP64__)
#define SIZEOF_SIZE_T  8
#else
#define SIZEOF_SIZE_T  4
#endif

#if defined(__clang__) || defined(__GNUC__)
#define HAVE_BUILTIN_CTZL  1
#endif

#define FALLTHROUGH  __attribute__((fallthrough));

#ifndef BITS_IN_JSAMPLE
#define BITS_IN_JSAMPLE  8
#endif

#undef C_ARITH_CODING_SUPPORTED
#undef D_ARITH_CODING_SUPPORTED
#undef WITH_SIMD

#if BITS_IN_JSAMPLE == 8
#define C_ARITH_CODING_SUPPORTED  1
#define D_ARITH_CODING_SUPPORTED  1
#define WITH_SIMD  1
#endif
