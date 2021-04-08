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

// Thin abstraction layer for cuBLAS and MIOpen.
#ifndef TFRT_GPU_STREAM_BLAS_WRAPPER_H_
#define TFRT_GPU_STREAM_BLAS_WRAPPER_H_

#include <cstddef>
#include <memory>

#include "tfrt/gpu/stream/cuda_forwards.h"
#include "tfrt/gpu/stream/stream_wrapper.h"

namespace tfrt {
namespace gpu {
namespace stream {

enum class BlasOperation {
  kNone = 0,
  kTranspose = 1,
  kConjugateTranspose = 2,
};

// Non-owning handles of GPU resources.
using BlasHandle = Resource<cublasHandle_t, rocblas_handle>;

namespace internal {
// Helper to wrap resources and memory into RAII types.
struct BlasHandleDeleter {
  using pointer = BlasHandle;
  void operator()(BlasHandle handle) const;
};
}  // namespace internal

// RAII wrappers for resources. Instances own the underlying resource.
//
// They are implemented as std::unique_ptrs with custom deleters.
//
// Use get() and release() to access the non-owning handle, please use with
// appropriate care.
using OwningBlasHandle = internal::OwningResource<internal::BlasHandleDeleter>;

llvm::Expected<OwningBlasHandle> BlasCreate(CurrentContext current);
llvm::Error BlasDestroy(BlasHandle handle);
llvm::Error BlasSetStream(BlasHandle handle, Stream stream);
llvm::Expected<Stream> BlasGetStream(BlasHandle handle);
llvm::Error BlasSaxpy(CurrentContext current, BlasHandle handle, int n,
                      Pointer<const float> alpha, Pointer<const float> x,
                      int incx, Pointer<float> y, int incy);
llvm::Error BlasSgemm(CurrentContext current, BlasHandle handle,
                      BlasOperation transa, BlasOperation transb, int m, int n,
                      int k, Pointer<const float> alpha, Pointer<const float> A,
                      int lda, Pointer<const float> B, int ldb,
                      Pointer<const float> beta, Pointer<float> C, int ldc);

}  // namespace stream
}  // namespace gpu
}  // namespace tfrt

#endif  // TFRT_GPU_STREAM_BLAS_WRAPPER_H_
