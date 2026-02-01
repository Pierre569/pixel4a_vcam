#ifndef _LIBS_CUTILS_NATIVE_HANDLE_H
#define _LIBS_CUTILS_NATIVE_HANDLE_H

#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>


/*
 * Android "cutils/native_handle.h" mock.
 * Minimal definition of native_handle_t for compilation.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct native_handle {
  int version; /* sizeof(native_handle_t) */
  int numFds;  /* number of file-descriptors at &data[0] */
  int numInts; /* number of ints at &data[numFds] */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wzero-length-array"
#endif
  int data[0]; /* numFds + numInts */
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
} native_handle_t;

typedef const native_handle_t *buffer_handle_t;

#ifdef __cplusplus
}
#endif

#endif // _LIBS_CUTILS_NATIVE_HANDLE_H
