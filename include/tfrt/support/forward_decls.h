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

// This file forward declares tfrt/support's template types. This
// file should not include any files from tfrt/support.

#ifndef TFRT_SUPPORT_FORWARD_DECLS_H_
#define TFRT_SUPPORT_FORWARD_DECLS_H_

#include <cstddef>
#include <memory>

#include "llvm/Support/Casting.h"

// We don't forward declare:
//   DenseMap, StringMap, StringSet
// Because they use default template parameters.
namespace llvm {

class raw_ostream;

template <typename T>
class ArrayRef;
template <typename T>
class Expected;
class Error;
template <typename T>
class MutableArrayRef;
template <typename T>
class Optional;
template <typename T, unsigned N>
class SmallVector;
template <typename T>
class SmallVectorImpl;
class StringRef;

template <typename FunctionT>
class unique_function;
template <typename FunctionT>
class function_ref;
}  // namespace llvm

namespace tfrt {

// Common TFRT types.
template <typename T>
class AsyncValueRef;
template <typename T>
class ConcurrentVector;
template <typename SubClass>
class ReferenceCounted;
template <typename T>
class RCReference;

template <typename T>
using Expected = llvm::Expected<T>;

using Error = llvm::Error;

// Commonly used types imported from LLVM.
using raw_ostream = llvm::raw_ostream;

template <typename T>
using ArrayRef = llvm::ArrayRef<T>;
template <typename T>
using MutableArrayRef = llvm::MutableArrayRef<T>;
template <typename T>
using Optional = llvm::Optional<T>;
template <typename T, unsigned N>
using SmallVector = llvm::SmallVector<T, N>;
template <typename T>
using SmallVectorImpl = llvm::SmallVectorImpl<T>;
using string_view = llvm::StringRef;

// Casting operators.
using llvm::cast;
using llvm::cast_or_null;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;
using llvm::isa_and_nonnull;

// TensorShape dimension type alias. It is currently fixed at 64 bits,
//  but might change to machine word in the future.
using Index = int64_t;

}  // namespace tfrt

#endif  // TFRT_SUPPORT_FORWARD_DECLS_H_
