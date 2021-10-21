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

// MLIR Async Runtime implemented on top of TFRT HostContext and host
// concurrency primitives.

#include "tfrt/cpu/jit/async_runtime.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

#include "llvm/Support/MathExtras.h"
#include "tfrt/host_context/async_value.h"
#include "tfrt/host_context/async_value_ref.h"
#include "tfrt/host_context/chain.h"
#include "tfrt/host_context/concurrent_work_queue.h"
#include "tfrt/host_context/diagnostic.h"
#include "tfrt/host_context/host_allocator.h"
#include "tfrt/host_context/host_buffer.h"
#include "tfrt/support/concurrent_vector.h"
#include "tfrt/support/latch.h"
#include "tfrt/support/msan.h"
#include "tfrt/support/ref_count.h"

// -------------------------------------------------------------------------- //
// Define AsyncToken and AsyncGroup in the mlir::runtime namespace to implement
// opaque structs defined in the MLIR Async Runtime API header file.
// -------------------------------------------------------------------------- //

namespace mlir {
namespace runtime {

using tfrt::AsyncValueRef;
using tfrt::HostBuffer;
using tfrt::HostContext;
using tfrt::MakeConstructedAsyncValueRef;
using tfrt::cpu::jit::AsyncRuntime;
using tfrt::cpu::jit::AsyncRuntimeObject;

class AsyncToken : public AsyncRuntimeObject {
 public:
  explicit AsyncToken(HostContext* host, unsigned ref_count = 1)
      : AsyncRuntimeObject(ref_count),
        chain_(MakeConstructedAsyncValueRef<tfrt::Chain>(host)) {}

  tfrt::AsyncValue* GetAsyncValue() const { return chain_.GetAsyncValue(); }

 private:
  AsyncValueRef<tfrt::Chain> chain_;
};

class AsyncValue : public AsyncRuntimeObject {
 public:
  explicit AsyncValue(HostContext* host, size_t size, size_t alignment,
                      unsigned ref_count = 1)
      : AsyncRuntimeObject(ref_count),
        storage_(Storage::CanStoreInline(size, alignment)
                     ? MakeConstructedAsyncValueRef<Storage>()
                     : MakeConstructedAsyncValueRef<Storage>(host->allocator(),
                                                             size, alignment)) {
    // Storage memory will be initialized by the compiled kernel.
    TFRT_MSAN_MEMORY_IS_INITIALIZED(GetStorage(), size);
  }

  void* GetStorage() const {
    assert(!GetAsyncValue()->IsError() && "unexpected error state");
    if (storage_->is_inline) return &storage_->inline_buffer;
    return storage_->host_buffer->data();
  }

  tfrt::AsyncValue* GetAsyncValue() const { return storage_.GetAsyncValue(); }

 private:
  // If the requested async value storage is small, use the inlined storage,
  // fallback on the HostBuffer if the requested storage size is large.
  struct Storage {
    static const int kSize = 128;  // enough to fit memref descriptor of rank 5
    static const int kAlign = alignof(std::max_align_t);

    Storage() : is_inline(true) {}
    Storage(tfrt::HostAllocator* allocator, size_t size, size_t alignment)
        : is_inline(false),
          host_buffer(
              HostBuffer::CreateUninitialized(size, alignment, allocator)
                  .release()) {}

    ~Storage() {
      if (!is_inline) host_buffer->DropRef();
    }

    static bool CanStoreInline(size_t size, size_t alignment) {
      assert(llvm::isPowerOf2_32(alignment));
      return size <= kSize && alignment <= kAlign;
    }

    bool is_inline;
    union {
      std::aligned_storage<kSize, kAlign>::type inline_buffer;
      tfrt::HostBuffer* host_buffer;
    };
  };

  AsyncValueRef<Storage> storage_;
};

class AsyncGroup : public AsyncRuntimeObject {
 public:
  explicit AsyncGroup(int64_t size, unsigned ref_count = 1)
      : AsyncRuntimeObject(ref_count),
        rank_(0),
        pending_tokens_(size),
        num_errors_(0),
        completed_(tfrt::MakeConstructedAsyncValueRef<tfrt::Chain>()) {
    // If group size is zero, mark completion async value ready.
    assert(size >= 0 && "size can't be negative");
    if (size == 0) completed_.SetStateConcrete();
  }

  size_t AddToken(AsyncToken* token) {
    size_t rank = rank_.fetch_add(1, std::memory_order_relaxed);

    // When token becomes available drop the number of pending tokens and maybe
    // make the group completion async value available.
    token->GetAsyncValue()->AndThen([group = this, token]() {
      // Increment the number of errors in the group.
      if (token->GetAsyncValue()->IsError()) group->num_errors_.fetch_add(1);

      // Pending tokens can't drop below zero.
      assert(group->pending_tokens_ > 0 && "wrong group size");

      // We do track group error state with the number of errors, and never
      // set completion async value state to error.
      if (group->pending_tokens_.fetch_sub(1) == 1)
        group->completed_.SetStateConcrete();
    });

    return rank;
  }

  tfrt::AsyncValue* GetCompletionAsyncValue() const {
    return completed_.GetAsyncValue();
  }

  bool IsError() const { return num_errors_.load() != 0; }

 private:
  std::atomic<int64_t> rank_;
  std::atomic<int64_t> pending_tokens_;
  std::atomic<int64_t> num_errors_;

  // Async value that keeps track the group completion, it will become available
  // when the number of pending tokens will drop to zero.
  AsyncValueRef<tfrt::Chain> completed_;
};

}  // namespace runtime
}  // namespace mlir

// -------------------------------------------------------------------------- //

namespace tfrt {
namespace cpu {
namespace jit {

/*static*/ void* AsyncRuntime::GetStorage(Value* value) {
  return value->GetStorage();
}

/*static*/ AsyncValue* AsyncRuntime::GetAsyncValue(AsyncRuntime::Value* value) {
  return value->GetAsyncValue();
}

/*static*/ AsyncValue* AsyncRuntime::GetAsyncValue(AsyncRuntime::Token* token) {
  return token->GetAsyncValue();
}

/*static*/ AsyncValue* AsyncRuntime::GetAsyncValue(AsyncRuntime::Group* group) {
  return group->GetCompletionAsyncValue();
}

void AsyncRuntime::Await(AsyncValue* awaitable) {
  // Blocking wait can't lead to a deadlock if runtime uses external thread
  // pool for launching async tasks.
  if (worker_threads_) {
    tfrt::latch latch(1);
    awaitable->AndThen([&]() { latch.count_down(); });
    latch.wait();
    return;
  }

  // If we use host context work queue to launch async tasks, then blocking
  // await can lead to a deadlock. Host context will check at runtime that
  // we are not in a thread managed by the host context itself.
  host_context_->Await(FormRef(awaitable));
}

/*static*/ void AsyncRuntime::AddRef(AsyncRuntimeObject* obj, unsigned count) {
  assert(count == 1 && "tfrt::ReferenceCounted can add just one ref");
  obj->AddRef();
}

/*static*/ void AsyncRuntime::DropRef(AsyncRuntimeObject* obj, unsigned count) {
  assert(count == 1 && "tfrt::ReferenceCounted can drop just one ref");
  obj->DropRef();
}

/*static*/ AsyncRuntimeObject* AsyncRuntime::ToAsyncRuntimeObject(
    AsyncRuntime::Token* token) {
  return static_cast<AsyncRuntimeObject*>(token);
}

/*static*/ AsyncRuntimeObject* AsyncRuntime::ToAsyncRuntimeObject(
    AsyncRuntime::Value* value) {
  return static_cast<AsyncRuntimeObject*>(value);
}

/*static*/ AsyncRuntimeObject* AsyncRuntime::ToAsyncRuntimeObject(
    AsyncRuntime::Group* group) {
  return static_cast<AsyncRuntimeObject*>(group);
}

AsyncRuntime::Token* AsyncRuntime::CreateToken() {
  // AsyncRuntime::Token created with a reference count of 2 because it will be
  // returned to the `async.execute` caller and also will be later on emplaced
  // by the asynchronously executed task. If the caller immediately will drop
  // its reference we must ensure that the token will be alive until the
  // asynchronous operation is completed.
  return new AsyncRuntime::Token(host_context_, /*ref_count=*/2);
}

void AsyncRuntime::SetAvailable(AsyncRuntime::Token* token) {
  token->GetAsyncValue()->SetStateConcrete();
  // Async tokens created with a ref count `2` to keep token alive until the
  // async task completes. Drop extra reference explicitly when token emplaced.
  DropRef(token);
}

void AsyncRuntime::SetError(AsyncRuntime::Token* token) {
  // TODO(ezhulenev): Construct a better diagnostincs when async runtime API
  // will support passing custom error messages.
  token->GetAsyncValue()->SetError(DecodedDiagnostic("<async runtime error>"));
  // Async tokens created with a ref count `2` to keep token alive until the
  // async task completes. Drop extra reference explicitly when token emplaced.
  DropRef(token);
}

bool AsyncRuntime::IsError(AsyncRuntime::Token* token) {
  return token->GetAsyncValue()->IsError();
}

void AsyncRuntime::AwaitToken(AsyncRuntime::Token* token) {
  Await(token->GetAsyncValue());
}

AsyncRuntime::Value* AsyncRuntime::CreateValue(size_t size, size_t alignment) {
  // AsyncRuntime::Value created with a reference count of 2 because it will be
  // returned to the `async.execute` caller and also will be later on emplaced
  // by the asynchronously executed task. If the caller immediately will drop
  // its reference we must ensure that the token will be alive until the
  // asynchronous operation is completed.
  return new AsyncRuntime::Value(host_context_, size, alignment,
                                 /*ref_count=*/2);
}

void AsyncRuntime::SetAvailable(AsyncRuntime::Value* value) {
  value->GetAsyncValue()->SetStateConcrete();
  // Async values created with a ref count `2` to keep token alive until the
  // async task completes. Drop extra reference explicitly when token emplaced.
  DropRef(value);
}

void AsyncRuntime::SetError(AsyncRuntime::Value* value) {
  // TODO(ezhulenev): Construct a better diagnostincs when async runtime API
  // will support passing custom error messages.
  value->GetAsyncValue()->SetError(DecodedDiagnostic("<async runtime error>"));
  // Async values created with a ref count `2` to keep token alive until the
  // async task completes. Drop extra reference explicitly when token emplaced.
  DropRef(value);
}

bool AsyncRuntime::IsError(AsyncRuntime::Value* value) {
  return value->GetAsyncValue()->IsError();
}

void AsyncRuntime::AwaitValue(AsyncRuntime::Value* value) {
  Await(value->GetAsyncValue());
}

AsyncRuntime::Group* AsyncRuntime::CreateGroup(int64_t size) {
  return new AsyncRuntime::Group(size);
}

size_t AsyncRuntime::AddTokenToGroup(AsyncRuntime::Group* group,
                                     AsyncRuntime::Token* token) {
  return group->AddToken(token);
}

bool AsyncRuntime::IsError(AsyncRuntime::Group* group) {
  return group->IsError();
}

void AsyncRuntime::AwaitGroup(AsyncRuntime::Group* group) {
  Await(group->GetCompletionAsyncValue());
}

}  // namespace jit
}  // namespace cpu
}  // namespace tfrt
