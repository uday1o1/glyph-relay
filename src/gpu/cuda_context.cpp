#include "glyphrelay/cuda_context.hpp"

#include <atomic>
#include <memory>
#include <utility>

#if GLYPHRELAY_HAS_CUDA_DRIVER
#include <cuda.h>
#endif

namespace glyphrelay {

struct CudaPrimaryContext::Implementation {
  int device_ordinal = -1;
  std::string reason = "cuda_driver_not_built";
  std::atomic_size_t active_guards = 0U;
  std::uint64_t generation = 0U;
  bool retained = false;
  bool shutdown_attempted = false;
#if GLYPHRELAY_HAS_CUDA_DRIVER
  CUdevice device = 0;
  CUcontext context = nullptr;

  ~Implementation() {
    if (retained && active_guards.load() == 0U) {
      static_cast<void>(cuDevicePrimaryCtxRelease(device));
    }
  }
#endif
};

struct ScopedCudaContext::Implementation {
  std::shared_ptr<CudaPrimaryContext::Implementation> owner;
  std::string reason = "cuda_context_guard_inactive";
  bool active = false;
#if GLYPHRELAY_HAS_CUDA_DRIVER
  CUcontext expected = nullptr;
#endif
};

namespace {

#if GLYPHRELAY_HAS_CUDA_DRIVER
std::atomic_uint64_t next_context_generation = 1U;
#endif

} // namespace

CudaPrimaryContext::CudaPrimaryContext(int device_ordinal)
    : implementation_(std::make_shared<Implementation>()) {
  implementation_->device_ordinal = device_ordinal;
  if (device_ordinal < 0) {
    implementation_->reason = "cuda_device_ordinal_invalid";
    return;
  }
#if GLYPHRELAY_HAS_CUDA_DRIVER
  if (cuInit(0U) != CUDA_SUCCESS) {
    implementation_->reason = "cuda_driver_initialization_failed";
    return;
  }
  if (cuDeviceGet(&implementation_->device, device_ordinal) != CUDA_SUCCESS) {
    implementation_->reason = "cuda_device_lookup_failed";
    return;
  }
  if (cuDevicePrimaryCtxRetain(&implementation_->context, implementation_->device) !=
          CUDA_SUCCESS ||
      implementation_->context == nullptr) {
    implementation_->reason = "cuda_primary_context_retain_failed";
    return;
  }
  implementation_->retained = true;
  implementation_->generation = next_context_generation.fetch_add(1U);
  implementation_->reason = "cuda_primary_context_retained";
#endif
}

CudaPrimaryContext::~CudaPrimaryContext() {
  if (implementation_) {
    static_cast<void>(shutdown());
  }
}

CudaPrimaryContext::CudaPrimaryContext(CudaPrimaryContext &&) noexcept = default;
bool CudaPrimaryContext::available() const {
  return implementation_ && implementation_->retained && !implementation_->shutdown_attempted;
}

const std::string &CudaPrimaryContext::reason() const {
  static const std::string moved = "cuda_primary_context_moved";
  return implementation_ ? implementation_->reason : moved;
}

CudaContextIdentity CudaPrimaryContext::identity() const {
  if (!available()) {
    return {};
  }
  return {implementation_->device_ordinal, native_handle(), implementation_->generation};
}

std::uintptr_t CudaPrimaryContext::native_handle() const {
#if GLYPHRELAY_HAS_CUDA_DRIVER
  return implementation_ ? reinterpret_cast<std::uintptr_t>(implementation_->context) : 0U;
#else
  return 0U;
#endif
}

std::size_t CudaPrimaryContext::active_guard_count() const {
  return implementation_ ? implementation_->active_guards.load() : 0U;
}

bool CudaPrimaryContext::shutdown() {
  if (!implementation_) {
    return true;
  }
  if (implementation_->shutdown_attempted) {
    return !implementation_->retained;
  }
  if (implementation_->active_guards.load() != 0U) {
    implementation_->shutdown_attempted = true;
    implementation_->reason = "cuda_primary_context_has_active_guards";
    return false;
  }
  implementation_->shutdown_attempted = true;
#if GLYPHRELAY_HAS_CUDA_DRIVER
  if (implementation_->retained &&
      cuDevicePrimaryCtxRelease(implementation_->device) != CUDA_SUCCESS) {
    implementation_->reason = "cuda_primary_context_release_failed";
    implementation_->shutdown_attempted = false;
    return false;
  }
#endif
  implementation_->retained = false;
  implementation_->reason = "cuda_primary_context_released";
  return true;
}

ScopedCudaContext::ScopedCudaContext(CudaPrimaryContext &context)
    : implementation_(std::make_unique<Implementation>()) {
  if (!context.available()) {
    implementation_->reason = "cuda_context_guard_owner_unavailable";
    return;
  }
#if GLYPHRELAY_HAS_CUDA_DRIVER
  implementation_->reason = "cuda_context_guard_active";
  if (cuCtxPushCurrent(reinterpret_cast<CUcontext>(context.native_handle())) != CUDA_SUCCESS) {
    implementation_->reason = "cuda_context_push_failed";
    return;
  }
  implementation_->expected = reinterpret_cast<CUcontext>(context.native_handle());
  implementation_->owner = context.implementation_;
  ++implementation_->owner->active_guards;
  implementation_->active = true;
#endif
}

ScopedCudaContext::~ScopedCudaContext() {
#if GLYPHRELAY_HAS_CUDA_DRIVER
  if (implementation_ && implementation_->active) {
    CUcontext popped = nullptr;
    if (cuCtxPopCurrent(&popped) != CUDA_SUCCESS || popped != implementation_->expected) {
      implementation_->reason = "cuda_context_pop_identity_mismatch";
    }
    const auto remaining = --implementation_->owner->active_guards;
    if (remaining == 0U && implementation_->owner->shutdown_attempted &&
        implementation_->owner->retained) {
      if (cuDevicePrimaryCtxRelease(implementation_->owner->device) == CUDA_SUCCESS) {
        implementation_->owner->retained = false;
        implementation_->owner->reason = "cuda_primary_context_released_after_final_guard";
      } else {
        implementation_->owner->reason = "cuda_primary_context_release_failed_after_final_guard";
      }
    }
    implementation_->active = false;
  }
#endif
}

ScopedCudaContext::ScopedCudaContext(ScopedCudaContext &&) noexcept = default;
bool ScopedCudaContext::active() const { return implementation_ && implementation_->active; }

const std::string &ScopedCudaContext::reason() const {
  static const std::string moved = "cuda_context_guard_moved";
  return implementation_ ? implementation_->reason : moved;
}

} // namespace glyphrelay
