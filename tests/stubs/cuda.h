#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int CUresult;
typedef uint64_t CUdeviceptr;

enum {
  CUDA_SUCCESS = 0,
  CU_MEMORYTYPE_HOST = 1,
  CU_MEMORYTYPE_DEVICE = 2,
};

typedef struct CUDA_MEMCPY2D_st {
  size_t srcXInBytes;
  size_t srcY;
  unsigned int srcMemoryType;
  const void *srcHost;
  CUdeviceptr srcDevice;
  void *srcArray;
  size_t srcPitch;
  size_t dstXInBytes;
  size_t dstY;
  unsigned int dstMemoryType;
  void *dstHost;
  CUdeviceptr dstDevice;
  void *dstArray;
  size_t dstPitch;
  size_t WidthInBytes;
  size_t Height;
} CUDA_MEMCPY2D;

CUresult cuGetErrorName(CUresult error, const char **name);
CUresult cuGetErrorString(CUresult error, const char **description);
CUresult cuMemAllocPitch(CUdeviceptr *device_pointer, size_t *pitch, size_t width_in_bytes,
                         size_t height, unsigned int element_size_bytes);
CUresult cuMemHostAlloc(void **host_pointer, size_t byte_count, unsigned int flags);
CUresult cuMemcpy2D(const CUDA_MEMCPY2D *copy);
CUresult cuMemFree(CUdeviceptr device_pointer);
CUresult cuMemFreeHost(void *host_pointer);

#ifdef __cplusplus
}
#endif
