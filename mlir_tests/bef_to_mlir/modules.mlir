// Copyright 2020 The TensorFlow Runtime Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// RUN: tfrt_translate -bef-to-mlir %s.bef | FileCheck %s

// CHECK: @kernel attributes {tfrt.compiled}
module @kernel attributes {tfrt.compiled} {
  func @main(%input: memref<?x?xf32>, %output: memref<?x?xf32>) {
    return
  }
}

// CHECK-LABEL: func @trivial
func @trivial() {
  // CHECK: simple.kernel
  // CHECK-SAME: func = @kernel::@main
  "simple.kernel"() { func = @kernel::@main } : () -> ()
  tfrt.return
}
