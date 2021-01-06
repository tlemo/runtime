/*
 * Copyright 2021 The TensorFlow Runtime Authors
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

//===- cpurt_corert_kernels_registration.cc -------------------------------===//
//
// This file uses a static constructor to automatically register CpuRT kernels
// that could handle CoreRt TensorHandle inputs. These kernels depend on the
// CoreRT library.
//
//===----------------------------------------------------------------------===//

#include "tfrt/host_context/kernel_registry.h"

namespace tfrt {
namespace cpu {
namespace jit {

void RegisterCpuRuntimeCoreRtKernels(KernelRegistry* registry);

namespace kernels {
TFRT_STATIC_KERNEL_REGISTRATION(RegisterCpuRuntimeCoreRtKernels);
}  // namespace kernels

}  // namespace jit
}  // namespace cpu
}  // namespace tfrt
