#ifndef _LIBS_CUTILS_COMPILER_H
#define _LIBS_CUTILS_COMPILER_H

/*
 * Android "cutils/compiler.h" mock.
 * Defines standard compiler attributes used by AOSP headers.
 */

#ifndef CC_LIKELY
#define CC_LIKELY(exp) (__builtin_expect(!!(exp), 1))
#define CC_UNLIKELY(exp) (__builtin_expect(!!(exp), 0))
#endif

#ifndef ANDROID_API
#define ANDROID_API __attribute__((visibility("default")))
#endif

#endif // _LIBS_CUTILS_COMPILER_H
