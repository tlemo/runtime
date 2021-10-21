/*
 * Copyright 2020 The TensorFlow Runtime Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Thin abstraction layer for cuFFT and rocFFT.
#ifndef TFRT_GPU_WRAPPER_FFT_WRAPPER_H_
#define TFRT_GPU_WRAPPER_FFT_WRAPPER_H_

#include <cstddef>
#include <memory>

#include "tfrt/gpu/wrapper/wrapper.h"

namespace tfrt {
namespace gpu {
namespace wrapper {

class FftHandle {
 public:
  FftHandle() = default;
  FftHandle(std::nullptr_t) {}
  FftHandle(cufftHandle handle) : platform_(Platform::CUDA) {
    union_.cuda_handle = handle;
  }
  FftHandle(hipfftHandle handle) : platform_(Platform::ROCm) {
    union_.hip_handle = handle;
  }
  // Required for std::unique_ptr<Resource>.
  FftHandle& operator=(std::nullptr_t) {
    platform_ = Platform::NONE;
    return *this;
  }
  // Required for std::unique_ptr<Resource>.
  operator bool() const { return platform() != Platform::NONE; }

  Platform platform() const { return platform_; }
  operator cufftHandle() const {
    assert(platform() == Platform::CUDA);
    return union_.cuda_handle;
  }
  operator hipfftHandle() const {
    assert(platform() == Platform::ROCm);
    return union_.hip_handle;
  }
  // For member access from std::unique_ptr.
  const FftHandle* operator->() const { return this; }

 private:
  Platform platform_;
  union {
    cufftHandle cuda_handle;
    hipfftHandle hip_handle;
  } union_;
};

enum class FftDirection : int {
  kForward,
  kInverse
};

namespace internal {
// Helper to wrap resources and memory into RAII types.
struct FftHandleDeleter {
  using pointer = FftHandle;
  void operator()(FftHandle handle) const;
};
}  // namespace internal

// RAII wrappers for resources. Instances own the underlying resource.
using OwningFftHandle = internal::OwningResource<internal::FftHandleDeleter>;

llvm::Error FftDestroy(FftHandle handle);
llvm::Error FftSetStream(FftHandle handle, Stream stream);
llvm::Expected<size_t> FftGetWorkspaceSize(FftHandle handle);
llvm::Error FftSetWorkspace(FftHandle handle, Pointer<void> workspace,
                            size_t size_bytes);

}  // namespace wrapper
}  // namespace gpu
}  // namespace tfrt

#endif  // TFRT_GPU_WRAPPER_FFT_WRAPPER_H_
