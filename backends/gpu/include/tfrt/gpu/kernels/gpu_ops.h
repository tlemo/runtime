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

// MLIR op definitions for gpu_ops library
//
// This file declares the 'gpu' dialect as well as the operators that make up
// the gpu_ops library.

#ifndef TFRT_GPU_KERNELS_CUDA_OPDEFS_GPU_OPS_H_
#define TFRT_GPU_KERNELS_CUDA_OPDEFS_GPU_OPS_H_

#include <type_traits>

#include "mlir/Dialect/GPU/GPUDialect.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/Interfaces/InferTypeOpInterface.h"
#include "tfrt/basic_kernels/opdefs/basic_kernels.h"
#include "tfrt/gpu/wrapper/blas_wrapper.h"
#include "tfrt/gpu/wrapper/ccl_wrapper.h"
#include "tfrt/gpu/wrapper/dnn_wrapper.h"
#include "tfrt/gpu/wrapper/wrapper.h"
#include "tfrt/tensor/opdefs/host_tensor.h"
#include "tfrt/tensor/opdefs/tensor.h"
#include "tfrt/tensor/opdefs/tensor_shape.h"

using namespace mlir;

namespace tfrt {
namespace gpu {

// Dialect for cuda operations.
class GpuDialect : public Dialect {
 public:
  static StringRef getDialectNamespace() { return "tfrt_gpu"; }
  explicit GpuDialect(MLIRContext* context);

 private:
  Type parseType(DialectAsmParser& parser) const override;
  void printType(Type, DialectAsmPrinter&) const override;
};

// An attribute that wrapps an I32Attr holding an enum or wrapper::Enum value.
template <typename T>
class EnumAttr : public mlir::Attribute {
 public:
  using mlir::Attribute::Attribute;
  static EnumAttr get(MLIRContext* context, T value) {
    return IntegerAttr::get(IntegerType::get(context, 32),
                            APInt(32, ToOpaqueValue(value)))
        .cast<EnumAttr>();
  }
  T getValue() const {
    return FromOpaqueValue(IntegerAttr(impl).getValue().getZExtValue());
  }
  static bool classof(mlir::Attribute attr) {
    IntegerAttr int_attr = attr.dyn_cast<IntegerAttr>();
    return int_attr && int_attr.getType().isSignlessInteger(32);
  }

 private:
  template <typename X = T>
  static decltype(&X::ToOpaqueValue, int()) ToOpaqueValue(T value) {
    return value.ToOpaqueValue();
  }
  template <typename X = T>
  static decltype(&X::FromOpaqueValue, T()) FromOpaqueValue(int opaque) {
    return T::FromOpaqueValue(opaque);
  }

  template <typename X = T>
  static typename std::enable_if<std::is_enum<X>::value, int>::type
  ToOpaqueValue(T value) {
    return static_cast<int>(value);
  }
  template <typename X = T>
  static typename std::enable_if<std::is_enum<X>::value, T>::type
  FromOpaqueValue(int opaque) {
    return static_cast<T>(opaque);
  }
};

using PlatformAttr = EnumAttr<wrapper::Platform>;
using DnnDataTypeAttr = EnumAttr<wrapper::DnnDataType>;
using DnnConvolutionModeAttr = EnumAttr<wrapper::DnnConvolutionMode>;
using BlasDataTypeAttr = EnumAttr<wrapper::BlasDataType>;
using BlasDiagTypeAttr = EnumAttr<wrapper::BlasDiagType>;
using BlasComputeTypeAttr = EnumAttr<wrapper::BlasComputeType>;
using BlasOperationAttr = EnumAttr<wrapper::BlasOperation>;
using BlasGemmAlgoAttr = EnumAttr<wrapper::BlasGemmAlgo>;
using BlasFillModeAttr = EnumAttr<wrapper::BlasFillMode>;
using BlasSideModeAttr = EnumAttr<wrapper::BlasSideMode>;
using CclDataTypeAttr = EnumAttr<ncclDataType_t>;
using CclReductionOpAttr = EnumAttr<ncclRedOp_t>;

namespace conversion {

// Dialect for cuda conversion helper operations.
class GpuConversionDialect : public Dialect {
 public:
  static StringRef getDialectNamespace() { return "tfrt_gpu_conversion"; }
  explicit GpuConversionDialect(MLIRContext* context);
};

}  // namespace conversion

}  // namespace gpu
}  // namespace tfrt

// TableGen'd declarations
#define GET_TYPEDEF_CLASSES
#include "tfrt/gpu/kernels/gpu_typedefs.h.inc"
#define GET_OP_CLASSES
#include "tfrt/gpu/kernels/gpu_opdefs.h.inc"
#define GET_OP_CLASSES
#include "tfrt/gpu/kernels/gpu_conversion_helper_opdefs.h.inc"

#endif  // TFRT_GPU_KERNELS_CUDA_OPDEFS_GPU_OPS_H_
