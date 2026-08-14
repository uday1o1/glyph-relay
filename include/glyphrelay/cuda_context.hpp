#pragma once

#include "glyphrelay/gpu_contracts.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace glyphrelay {

class ScopedCudaContext;

class CudaPrimaryContext {
public:
  explicit CudaPrimaryContext(int device_ordinal);
  ~CudaPrimaryContext();

  CudaPrimaryContext(CudaPrimaryContext &&) noexcept;
  CudaPrimaryContext &operator=(CudaPrimaryContext &&) noexcept = delete;
  CudaPrimaryContext(const CudaPrimaryContext &) = delete;
  CudaPrimaryContext &operator=(const CudaPrimaryContext &) = delete;

  bool available() const;
  const std::string &reason() const;
  CudaContextIdentity identity() const;
  std::uintptr_t native_handle() const;
  std::size_t active_guard_count() const;
  bool shutdown();

private:
  friend class ScopedCudaContext;
  struct Implementation;
  std::shared_ptr<Implementation> implementation_;
};

class ScopedCudaContext {
public:
  explicit ScopedCudaContext(CudaPrimaryContext &context);
  ~ScopedCudaContext();

  ScopedCudaContext(ScopedCudaContext &&) noexcept;
  ScopedCudaContext &operator=(ScopedCudaContext &&) noexcept = delete;
  ScopedCudaContext(const ScopedCudaContext &) = delete;
  ScopedCudaContext &operator=(const ScopedCudaContext &) = delete;

  bool active() const;
  const std::string &reason() const;

private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

} // namespace glyphrelay
