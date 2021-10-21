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

#include <iterator>
#include <utility>

#include "llvm/ADT/STLExtras.h"
#include "mlir/Conversion/ReconcileUnrealizedCasts/ReconcileUnrealizedCasts.h"
#include "mlir/Dialect/Async/IR/Async.h"
#include "mlir/Dialect/Async/IR/AsyncTypes.h"
#include "mlir/Dialect/GPU/GPUDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/BlockAndValueMapping.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "mlir/Transforms/RegionUtils.h"
#include "tfrt/basic_kernels/opdefs/basic_kernels.h"
#include "tfrt/basic_kernels/opdefs/types.h"
#include "tfrt/gpu/kernels/gpu_ops.h"
#include "tfrt/gpu/passes/passes.h"
#include "tfrt/tensor/opdefs/dense_host_tensor.h"
#include "tfrt/test_kernels/opdefs/test_kernels.h"

namespace tfrt {
namespace gpu {

using CastOp = mlir::UnrealizedConversionCastOp;

namespace {

// Helper for 1-N conversion, similar to materializeSource/TargetConversion().
// Creates CastOps from illegal source types to legal target types and back.
class OneToAnyConversion {
 public:
  static FailureOr<OneToAnyConversion> Get(TypeConverter *converter,
                                           TypeRange source_types);

  ArrayRef<Type> GetTargetTypes() const {
    return conversion_.getConvertedTypes();
  }

  // Inserts casts of legal-typed target_values back to source_types.
  SmallVector<Value, 4> CastToSourceTypes(OpBuilder &builder, Location loc,
                                          ValueRange target_values);

  // Inserts casts of illegal-typed source_values to converted types.
  SmallVector<Value, 4> CastToTargetTypes(OpBuilder &builder, Location loc,
                                          ValueRange source_values);

 private:
  OneToAnyConversion(TypeRange source_types,
                     TypeConverter::SignatureConversion conversion)
      : source_types_(source_types), conversion_(conversion) {}

  TypeRange source_types_;
  TypeConverter::SignatureConversion conversion_;
};

// Rewrites a function to take extra !tfrt.chain and !tfrt_gpu.stream arguments
// and return a !tfrt.chain. Adds gpu.wait dependencies where there aren't any.
//
//     func @main(...) {
//       ...
//       %ti = gpu.wait async [/*no deps*/]  // At least one, may be nested.
//       ...
//       gpu.wait /*not async*/ [...]        // Exactly one.
//       return
//     }
//
// will be rewritten to
//
//     func @main(
//       %arg0 : !tfrt.chain, %arg1 : !tfrt_gpu.stream, ...
//     ) -> !tfrt.chain {
//       %t0 = unrealized_conversion_cast %arg0, %arg1
//               : !tfrt.chain, !tfrt_gpu.stream to !gpu.async.token
//       %t1 = gpu.wait async [%t0]
//       ...
//       %ti = gpu.wait async [%t1]
//       ...
//       %tn = gpu.wait async [...]
//       %ch, %stream = unrealized_conversion_cast %tn
//               : !gpu.async.token to !tfrt.chain, !tfrt_gpu.stream
//       return %ch
//     }
//
struct AddChainAndStreamToFuncPattern : public OpRewritePattern<FuncOp> {
  using OpRewritePattern::OpRewritePattern;

 private:
  LogicalResult matchAndRewrite(FuncOp func_op,
                                PatternRewriter &rewriter) const override;
};

// Two type conversion patterns for async.execute. It inserts casts to/from the
// converted types before/after as well as at the end/beginning of the region.
//
// With type X being converted to Y, applying
// ConvertAsyncExec/YieldToChainAndEventPattern to
//
//     %a1, %f1 = async.execute [%a0] (
//       %f0 as %x0: !async.value<X>
//     ) -> !async.value<X> {
//       ...
//       async.yield %x1 : X
//     }
//
// will be rewritten to
//
//     %f2 = unrealized_conversion_cast %f0 : !async.value<X> to !async.value<Y>
//     %a1, %f3 = async.execute [%a0] (
//       %f2 as %y0: !async.value<Y>
//     ) -> (!async.value<Y>) {
//       %x0 = unrealized_conversion_cast %y0 : Y to X
//       ...
//       %y1 = unrealized_conversion_cast %x1 : X to Y
//       async.yield %y1 : Y
//     }
//     %f1 = unrealized_conversion_cast %f3 : !async.value<Y> to !async.value<X>
//
struct ConvertAsyncExecToChainAndEventPattern
    : public OpConversionPattern<async::ExecuteOp> {
  using OpConversionPattern::OpConversionPattern;

 private:
  LogicalResult matchAndRewrite(
      async::ExecuteOp exec_op, OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override;
};

// A type conversion pattern for async.yield.
// See documentation of above pattern.
struct ConvertAsyncYieldToChainAndEventPattern
    : public OpConversionPattern<async::YieldOp> {
  using OpConversionPattern::OpConversionPattern;

 private:
  LogicalResult matchAndRewrite(
      async::YieldOp yield_op, OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override;
};

// Swaps the async.await and the operand-defining cast.
//
//     %fx = unrealized_conversion_cast %fy : !async.value<Y> to !async.value<X>
//     %x  = async.await %fx : X
//
// will be rewritten to
//
//     %y  = async.await %fy : Y
//     %x  = unrealized_conversion_cast %y : Y to X
//
struct SwapAsyncAwaitOfCastPattern
    : public OpConversionPattern<async::AwaitOp> {
  using OpConversionPattern::OpConversionPattern;

 private:
  LogicalResult matchAndRewrite(
      async::AwaitOp await_op, OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override;
};

// Converts mlir::gpu::MemsetOp to tfrt::gpu::MemSetOp.
struct ConvertMemsetPattern : OpConversionPattern<mlir::gpu::MemsetOp> {
  using OpConversionPattern::OpConversionPattern;

 private:
  LogicalResult matchAndRewrite(
      mlir::gpu::MemsetOp memset_op, OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override;
};

// Converts mlir::gpu::MemcpyOp to tfrt::gpu::MemCopyOp.
struct ConvertMemcpyPattern : OpConversionPattern<mlir::gpu::MemcpyOp> {
  using OpConversionPattern::OpConversionPattern;

 private:
  LogicalResult matchAndRewrite(
      mlir::gpu::MemcpyOp memcpy_op, OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override;
};

// Converts mlir::memref::GetGlobalOp to tfrt::gpu::ModuleGetGlobalOp.
struct ConvertGetGlobalPattern : OpConversionPattern<memref::GetGlobalOp> {
  using OpConversionPattern::OpConversionPattern;

 private:
  LogicalResult matchAndRewrite(
      memref::GetGlobalOp get_global_op, OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override;
};

// Converts `gpu.module` op to a function that loads the module.
//
//     gpu.module @gpu_module attributes { binary = "<cubin>" }
//
// will be rewritten to
//
//     func @gpu_module(%arg0: !tfrt_gpu.context) -> !tfrt_gpu.module {
//       %0 = tfrt_gpu.module.load %arg0 {data = "<cubin>\00"}
//       tfrt.return %0 : !tfrt_gpu.module
//     }
//
// If the `gpu.module` also has a `constants` attribute, the generated function
// initializes the given globals with the provided values and returns a chain.
struct ConvertGpuModulePattern : OpConversionPattern<mlir::gpu::GPUModuleOp> {
  using OpConversionPattern::OpConversionPattern;

 private:
  LogicalResult matchAndRewrite(
      mlir::gpu::GPUModuleOp module_op, OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override;
};

// Converts mlir::gpu::LaunchFuncOp to tfrt::gpu::FunctionLaunchOp.
struct ConvertLaunchFuncPattern : OpConversionPattern<mlir::gpu::LaunchFuncOp> {
  using OpConversionPattern::OpConversionPattern;

 private:
  LogicalResult matchAndRewrite(
      mlir::gpu::LaunchFuncOp launch_op, OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override;
};

// Folds `unrealized_conversion_cast(constant ? : index) : index to ui32`.
struct FoldConstCastPattern : OpConversionPattern<CastOp> {
  using OpConversionPattern::OpConversionPattern;

 private:
  LogicalResult matchAndRewrite(
      CastOp cast_op, OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override;
};

// Moves the body of a tfrt_gpu_conversion.async.execute op into the parent
// block and removes the op.
//
//     %t0 = unrealized_conversion_cast %ch0, %stream : !gpu.async.token
//     %t1 = tfrt_gpu_conversion.async.execute [%t0] {
//       ^bb(%0 : !tfrt.chain, %1 : !tfrt_gpu.stream)
//       ... ops using %0 and %1 ...
//       tfrt.return %n : !tfrt.chain
//     }
//
// will be rewritten to
//
//     %t0 = unrealized_conversion_cast %ch0, %stream : !gpu.async.token
//     ... ops using %ch0 and %stream ...
//     %t1 = unrealized_conversion_cast %n, %stream : !gpu.async.token
//
struct InlineConversionAsyncExecPattern
    : public OpConversionPattern<conversion::AsyncExecuteOp> {
  using OpConversionPattern::OpConversionPattern;

 private:
  LogicalResult matchAndRewrite(
      conversion::AsyncExecuteOp exec_op, OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override;
};

// A rewrite pattern to convert gpu.wait operations to streams and events.
//
//     %t0 = unrealized_conversion_cast %ch0, %stream0
//     %t1 = unrealized_converstion_cast %ch1, %event0
//
//     %t2 = gpu.wait async [%t0]
//     %t3 = gpu.wait async [%t1]
//     %t4 = gpu.wait async [%t0, %t1]
//
// will be rewritten to
//
//     %t0 = unrealized_conversion_cast %ch0, %stream0
//     %t1 = unrealized_converstion_cast %ch1, %event0
//
//     // %t2 is replaced with %t0
//     %t2      = %t0
//     // %t3 is casted from a new stream synchronized with %event0
//     %ctx     = tfrt_gpu.stream.get_context %stream0
//     %stream1 = tfrt_gpu.stream.create %ctx
//     %ch2     = tfrt_gpu.stream.wait %stream1, %event0, %ch1
//     %t3      = unrealized_conversion_cast %ch2, %stream1
//     // %t4 is casted from %stream0 synchronized with %event0
//     %ch3     = tfrt_gpu.merge_chains %ch0, %ch1
//     %ch4     = tfrt_gpu.stream.wait %stream0, %event0, %ch3
//     %t4      = unrealized_conversion_cast %ch4, %stream0
//
struct ConvertGpuWaitToChainAndStreamPattern
    : public OpConversionPattern<mlir::gpu::WaitOp> {
  using OpConversionPattern::OpConversionPattern;

 private:
  LogicalResult matchAndRewrite(
      mlir::gpu::WaitOp wait_op, OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override;
};

// A rewriter pattern to convert a nested cast from stream to event into a
// recorded event.
//
//     %t           = unrealized_conversion_cast %ch0, %stream
//     %ch1, %event = unrealized_conversion_cast %t
//
// will be rewritten to
//
//     %ctx   = tfrt_gpu.stream.get_context %stream
//     %event = tfrt_gpu.event.create
//     %ch1   = tfrt_gpu.event.record %event, %stream, %ch0
//     %t     = unlrealized_conversion_cast %ch1, %stream
//
struct ConvertCastToEventRecordPattern
    : public OpConversionPattern<mlir::UnrealizedConversionCastOp> {
  using OpConversionPattern::OpConversionPattern;

 private:
  LogicalResult matchAndRewrite(
      mlir::UnrealizedConversionCastOp cast_op, OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override;
};

// A rewrite pattern to convert async.execute operations to tfrt_test.do.async.
// The !async.token values have no meaning with non-strict execution and we
// simply drop them. This means that side-effecting ops need to by synchronized
// through one of the !async.value<> arguments.
//
//     y0 = ... : Y
//     %a1, %f1 = async.execute [%a0] (
//       %f0 as %x0: !async.value<X>
//     ) -> !async.value<X> {
//       ... %y0
//       async.yield %x0 : X
//     }
//
// will be rewritten to
//
//     y0 = ... : Y
//     %x1 = tfrt_test.do.async %x0, %y0 : (X, Y) -> (X) {
//       ... %c0
//       tfrt.return %x0 : X
//     }
//
struct ConvertAsyncExecToDoAsyncPattern
    : public OpConversionPattern<async::ExecuteOp> {
  using OpConversionPattern::OpConversionPattern;

 private:
  LogicalResult matchAndRewrite(
      async::ExecuteOp exec_op, OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override;
};

// A rewrite pattern to remove async.await operations.
struct FoldAsyncAwaitPattern : public OpConversionPattern<async::AwaitOp> {
  using OpConversionPattern::OpConversionPattern;

 private:
  LogicalResult matchAndRewrite(
      async::AwaitOp await_op, OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override;
};

// A pass which rewrites a function to take extra !tfrt.chain and
// !tfrt_gpu.stream arguments and return a !tfrt.chain.
struct AddChainAndStreamToFuncPass
    : public mlir::PassWrapper<AddChainAndStreamToFuncPass, FunctionPass> {
 private:
  void runOnFunction() override;
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<GpuDialect, compiler::TFRTDialect>();
  }
  StringRef getArgument() const override { return "func-tfrt-streamify"; }
};

// A pass which rewrites !async.execute and related ops to use !tfrt.chain and
// !tfrt_gpu.stream instead of !gpu.async.token.
struct ConvertAsyncToChainAndEventPass
    : public mlir::PassWrapper<ConvertAsyncToChainAndEventPass, FunctionPass> {
 private:
  void runOnFunction() override;
  StringRef getArgument() const override { return "async-tfrt-streamify"; }
};

// A pass which converts from gpu dialect to tfrt_gpu dialect.
struct ConvertGpuToTfrtGpuPass
    : public mlir::PassWrapper<ConvertGpuToTfrtGpuPass,
                               OperationPass<ModuleOp>> {
 private:
  void runOnOperation() override;
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<tfrt::dht::DenseHostTensorDialect>();
  }
  StringRef getArgument() const override { return "gpu-tfrt-streamify"; }
};

// A pass which converts from async dialect to tfrt dialect.
struct ConvertAsyncToTfrtPass
    : public mlir::PassWrapper<ConvertAsyncToTfrtPass, FunctionPass> {
 private:
  void runOnFunction() override;
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<test::TestDialect>();
  }
  StringRef getArgument() const override { return "async-to-tfrt"; }
};

}  // namespace

// Helper functions to unrealized_conversion_cast to statically known types.
template <typename T>
static Value CastTo(OpBuilder &builder, Location loc, ValueRange values) {
  return builder.create<CastOp>(loc, builder.getType<T>(), values).getResult(0);
}
template <typename T>
static ValueRange CastToChainAnd(OpBuilder &builder, Location loc,
                                 Value value) {
  Type types[] = {builder.getType<compiler::ChainType>(), builder.getType<T>()};
  return builder.create<CastOp>(loc, types, value).getResults();
}
const auto CastToToken = CastTo<mlir::gpu::AsyncTokenType>;
const auto CastToChainAndStream = CastToChainAnd<StreamType>;
const auto CastToChainAndEvent = CastToChainAnd<EventType>;

// Helper functions test TypeRange against a list of statically known types.
template <typename... Ts, std::size_t... Is>
static bool IsTypesImpl(TypeRange types, std::index_sequence<Is...>) {
  // TODO(csigg): Replace with fold expression once we can use C++17.
  return llvm::all_of(std::initializer_list<bool>{types[Is].isa<Ts>()...},
                      [](bool result) { return result; });
}
template <typename... Ts>
static bool IsTypes(TypeRange types) {
  if (types.size() != sizeof...(Ts)) return false;
  return IsTypesImpl<Ts...>(types, std::make_index_sequence<sizeof...(Ts)>());
}
const auto IsTokenType = IsTypes<mlir::gpu::AsyncTokenType>;
const auto IsChainAndEventType = IsTypes<compiler::ChainType, EventType>;

// Helper function to test whether cast is between !gpu.async.token and
// !tfrt.chain plus !tfrt_gpu.stream/event.
template <typename T>
static bool IsCastToChainAnd(CastOp cast_op) {
  return cast_op && IsTokenType(cast_op.getResultTypes()) &&
         IsTypes<compiler::ChainType, T>(cast_op.getOperandTypes());
}
const auto IsCastToChainAndStream = IsCastToChainAnd<StreamType>;
const auto IsCastToChainAndEvent = IsCastToChainAnd<EventType>;
template <typename T>
static bool IsCastFromChainAnd(CastOp cast_op) {
  return cast_op && IsTokenType(cast_op.getOperandTypes()) &&
         IsTypes<compiler::ChainType, T>(cast_op.getResultTypes());
}
const auto IsCastFromChainAndEvent = IsCastFromChainAnd<EventType>;

// Helper function to merge two ranges into a SmallVector.
template <typename R1, typename R2>
auto MergeRanges(R1 first, R2 second) {
  using T = typename std::iterator_traits<typename R1::iterator>::value_type;
  SmallVector<T, 8> result;
  result.reserve(first.size() + second.size());
  llvm::copy(first, std::back_inserter(result));
  llvm::copy(second, std::back_inserter(result));
  return result;
};

FailureOr<OneToAnyConversion> OneToAnyConversion::Get(TypeConverter *converter,
                                                      TypeRange source_types) {
  TypeConverter::SignatureConversion conversion(source_types.size());
  if (failed(converter->convertSignatureArgs(source_types, conversion)))
    return failure();
  return OneToAnyConversion(source_types, conversion);
}

// Inserts casts of legal-typed target_values back to source_types.
SmallVector<Value, 4> OneToAnyConversion::CastToSourceTypes(
    OpBuilder &builder, Location loc, ValueRange target_values) {
  SmallVector<Value, 4> results;
  llvm::transform(
      llvm::enumerate(source_types_), std::back_inserter(results),
      [&](const auto &pair) {
        auto mapping =
            conversion_.getInputMapping(pair.index())
                .getValueOr(TypeConverter::SignatureConversion::InputMapping{});
        if (mapping.replacementValue) return mapping.replacementValue;
        auto operands = target_values.take_front(mapping.size);
        target_values = target_values.drop_front(mapping.size);
        if (mapping.size == 1 && operands.front().getType() == pair.value())
          return operands.front();
        auto cast_op = builder.create<CastOp>(loc, pair.value(), operands);
        return cast_op.getResult(0);
      });
  return results;
}

// Inserts casts of illegal-typed source_values to converted types.
SmallVector<Value, 4> OneToAnyConversion::CastToTargetTypes(
    OpBuilder &builder, Location loc, ValueRange source_values) {
  SmallVector<Value, 4> results;
  for (auto pair : llvm::enumerate(source_values)) {
    auto mapping = conversion_.getInputMapping(pair.index());
    if (!mapping) continue;  // Argument was dropped.
    if (mapping->replacementValue) results.push_back(mapping->replacementValue);
    assert(mapping->size != 0);
    auto types = GetTargetTypes().slice(mapping->inputNo, mapping->size);
    if (types.size() == 1 && types.front() == pair.value().getType()) {
      results.push_back(pair.value());
    } else {
      auto cast_op = builder.create<CastOp>(loc, types, pair.value());
      llvm::copy(cast_op->getResults(), std::back_inserter(results));
    }
  }
  return results;
}

LogicalResult AddChainAndStreamToFuncPattern::matchAndRewrite(
    FuncOp func_op, PatternRewriter &rewriter) const {
  // Collect `gpu.wait [...]` and `gpu.wait async []` ops.
  SmallVector<mlir::gpu::WaitOp, 4> wait_ops;
  func_op.walk([&](mlir::gpu::WaitOp op) {
    if (!op.asyncToken() || op.asyncDependencies().empty())
      wait_ops.push_back(op);
  });

  if (wait_ops.size() < 2)
    return rewriter.notifyMatchFailure(func_op, "expected at least 2 gpu.wait");
  if (llvm::find_if(wait_ops, [](mlir::gpu::WaitOp op) {
        return !op.asyncToken();
      }) != wait_ops.end() - 1) {
    return rewriter.notifyMatchFailure(
        func_op, "expected all but the last gpu.wait to be async");
  }

  Type chain_type = rewriter.getType<compiler::ChainType>();
  Type stream_type = rewriter.getType<StreamType>();

  // Add !tfrt.chain, !tfrt_gpu.stream arguments and !tfrt.chain result.
  auto argument_types = MergeRanges(TypeRange{chain_type, stream_type},
                                    func_op.getArgumentTypes());
  auto result_types =
      MergeRanges(TypeRange(chain_type), func_op.getCallableResults());
  rewriter.updateRootInPlace(func_op, [&] {
    func_op.setType(
        rewriter.getType<mlir::FunctionType>(argument_types, result_types));
  });

  // Add new function arguments to entry block. This is a bit of a dance
  // so that it could be rolled back in case of conversion failure.
  Block *block = &func_op.body().front();
  Block *entry = rewriter.createBlock(block, argument_types);
  auto entry_args = entry->getArguments();

  // Cast new arguments to token and insert wait async op.
  // %t0 = unrealized_conversion_cast %arg0, %arg1 -> !gpu.async.token
  // %t1 = gpu.wait async [%t0]
  Location loc = func_op.getLoc();
  Value token = CastToToken(rewriter, loc, entry_args.take_front(2));
  auto first_wait_op =
      rewriter.create<mlir::gpu::WaitOp>(loc, token.getType(), token);
  rewriter.mergeBlocks(block, entry, entry_args.drop_front(2));

  // Add %t1 from above to all `gpu.wait async []` ops.
  for (auto op : makeArrayRef(wait_ops).drop_back())
    op.addAsyncDependency(first_wait_op.asyncToken());

  // Make `gpu.wait [...]` async, cast result and add chain to returned
  // values.
  Operation *terminator = func_op.body().back().getTerminator();
  rewriter.setInsertionPoint(terminator);
  auto last_wait_op = rewriter.create<mlir::gpu::WaitOp>(
      wait_ops.back().getLoc(), token.getType(),
      wait_ops.back().asyncDependencies());
  rewriter.eraseOp(wait_ops.back());
  auto chain_and_stream = CastToChainAndStream(rewriter, last_wait_op.getLoc(),
                                               last_wait_op.asyncToken());
  auto results =
      MergeRanges(chain_and_stream.take_front(), terminator->getOperands());
  rewriter.replaceOpWithNewOp<compiler::ReturnOp>(terminator, results);

  return success();
}

LogicalResult ConvertAsyncExecToChainAndEventPattern::matchAndRewrite(
    async::ExecuteOp exec_op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {
  Location loc = exec_op->getLoc();

  auto operand_conversion =
      OneToAnyConversion::Get(typeConverter, TypeRange(adaptor.operands()));
  auto result_conversion =
      OneToAnyConversion::Get(typeConverter, exec_op.getResultTypes());
  auto argument_conversion = OneToAnyConversion::Get(
      typeConverter, exec_op.getRegion().getArgumentTypes());
  auto terminator_conversion = OneToAnyConversion::Get(
      typeConverter,
      exec_op.getRegion().back().getTerminator()->getOperandTypes());

  if (failed(operand_conversion) || failed(result_conversion) ||
      failed(argument_conversion) || failed(terminator_conversion))
    return rewriter.notifyMatchFailure(exec_op, "failed to convert types");

  // Create new async.execute op with converted operands.
  auto new_op = rewriter.create<mlir::async::ExecuteOp>(
      loc, terminator_conversion->GetTargetTypes(), adaptor.dependencies(),
      operand_conversion->CastToTargetTypes(rewriter, loc, adaptor.operands()));

  // Convert new results back to invalid types.
  rewriter.replaceOp(exec_op, result_conversion->CastToSourceTypes(
                                  rewriter, loc, new_op.getResults()));

  OpBuilder::InsertionGuard guard(rewriter);

  // Convert region arguments back to invalid types.
  Region *region = &new_op.getRegion();
  rewriter.setInsertionPointToEnd(&region->front());
  auto arguments = argument_conversion->CastToSourceTypes(
      rewriter, loc, region->getArguments());

  // Clone original body into the new region.
  mlir::BlockAndValueMapping mapping;
  rewriter.cloneRegionBefore(exec_op.getRegion(), *region, region->end(),
                             mapping);
  rewriter.mergeBlocks(&region->back(), &region->front(), arguments);

  return success();
}

LogicalResult ConvertAsyncYieldToChainAndEventPattern::matchAndRewrite(
    async::YieldOp yield_op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {
  auto operands = adaptor.getOperands();
  auto conversion = OneToAnyConversion::Get(typeConverter, TypeRange(operands));
  if (failed(conversion))
    return rewriter.notifyMatchFailure(yield_op, "failed to convert types");
  rewriter.replaceOpWithNewOp<mlir::async::YieldOp>(
      yield_op,
      conversion->CastToTargetTypes(rewriter, yield_op->getLoc(), operands));
  return success();
}

LogicalResult SwapAsyncAwaitOfCastPattern::matchAndRewrite(
    async::AwaitOp await_op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {
  auto cast_op = adaptor.operand().getDefiningOp<CastOp>();
  if (!cast_op || !llvm::all_of(cast_op->getOperandTypes(), [](Type type) {
        return type.isa<async::ValueType>();
      }))
    return rewriter.notifyMatchFailure(await_op, "operand not def by cast");

  Location loc = await_op->getLoc();
  SmallVector<Value, 4> results;
  for (auto operand : cast_op->getOperands()) {
    results.push_back(
        rewriter.create<async::AwaitOp>(loc, operand).getResult(0));
  }
  rewriter.replaceOp(await_op, CastToToken(rewriter, loc, results));
  return success();
}

LogicalResult ConvertMemsetPattern::matchAndRewrite(
    mlir::gpu::MemsetOp memset_op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {
  if (adaptor.value().getType().getIntOrFloatBitWidth() != 32)
    return rewriter.notifyMatchFailure(memset_op, "expected 32bit value");
  if (!adaptor.dst().getType().isa<tfrt::gpu::BufferType>())
    return rewriter.notifyMatchFailure(memset_op, "expected buffer dst");
  if (adaptor.asyncDependencies().empty() || !memset_op.asyncToken())
    return rewriter.notifyMatchFailure(memset_op, "no async deps or no result");
  auto cast_op = adaptor.asyncDependencies().front().getDefiningOp<CastOp>();
  if (!IsCastToChainAndStream(cast_op))
    return rewriter.notifyMatchFailure(memset_op, "operand not def by cast");

  auto loc = memset_op->getLoc();
  auto stream = cast_op.getOperand(1);
  auto new_op = rewriter.create<tfrt::gpu::MemSetOp>(
      loc, adaptor.dst(), adaptor.value(), stream, cast_op.getOperand(0));
  auto token = CastToToken(rewriter, loc, {new_op.getResult(), stream});
  rewriter.replaceOp(memset_op, token);
  return success();
}

LogicalResult ConvertMemcpyPattern::matchAndRewrite(
    mlir::gpu::MemcpyOp memcpy_op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {
  if (!adaptor.src().getType().isa<tfrt::gpu::BufferType>() ||
      !adaptor.dst().getType().isa<tfrt::gpu::BufferType>())
    return rewriter.notifyMatchFailure(memcpy_op, "expected buffer operands");
  if (adaptor.asyncDependencies().empty() || !memcpy_op.asyncToken())
    return rewriter.notifyMatchFailure(memcpy_op, "no async deps or no result");
  auto cast_op = adaptor.asyncDependencies().front().getDefiningOp<CastOp>();
  if (!IsCastToChainAndStream(cast_op))
    return rewriter.notifyMatchFailure(memcpy_op, "operand not def by cast");

  auto loc = memcpy_op->getLoc();
  auto stream = cast_op.getOperand(1);
  auto new_op = rewriter.create<tfrt::gpu::MemCopyOp>(
      loc, adaptor.dst(), adaptor.src(), stream, cast_op.getOperand(0));
  auto token = CastToToken(rewriter, loc, {new_op.getResult(), stream});
  rewriter.replaceOp(memcpy_op, token);
  return success();
}

LogicalResult ConvertGetGlobalPattern::matchAndRewrite(
    memref::GetGlobalOp get_global_op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {
  auto module_attr =
      get_global_op->getAttrOfType<SymbolRefAttr>(getGpuModuleAttrName());
  if (!module_attr)
    return rewriter.notifyMatchFailure(get_global_op, "no gpu_module attr");
  Location loc = get_global_op->getLoc();
  Value stream = get_global_op->getParentOfType<FuncOp>().getArgument(1);
  Value context = rewriter.create<StreamGetContextOp>(loc, stream).getResult();
  auto func_op =
      SymbolTable::lookupNearestSymbolFrom<FuncOp>(get_global_op, module_attr);
  auto once_op = rewriter.create<compiler::OnceOp>(
      loc, func_op.getType().getResults(), context, func_op.getName());
  rewriter.replaceOpWithNewOp<ModuleGetGlobalOp>(
      get_global_op, once_op.getResult(0), get_global_op.nameAttr().getAttr());
  return success();
}

// Initializes the global symbols in 'module' with values in 'constants'.
static Value CreateGlobalInitialization(ConversionPatternRewriter &rewriter,
                                        Location loc, Value context,
                                        Value module,
                                        DictionaryAttr constants) {
  Value chain = rewriter.create<compiler::NewChainOp>(loc).getResult();
  Value stream = rewriter.create<StreamCreateOp>(loc, context).getResult();
  for (auto pair : constants) {
    auto name = pair.first.strref();
    auto global_op = rewriter.create<ModuleGetGlobalOp>(loc, module, name);
    auto tensor_op = rewriter.create<dht::CreateUninitializedTensorOp_ui8_1>(
        loc, rewriter.getType<t::TensorType>());
    auto attr = pair.second.cast<DenseIntElementsAttr>();
    std::vector<Attribute> values;
    values.reserve(attr.getNumElements());
    llvm::transform(attr, std::back_inserter(values), [&](APInt value) {
      return rewriter.getI8IntegerAttr(value.getZExtValue());
    });
    tensor_op->setAttr("shape", rewriter.getI64ArrayAttr(values.size()));
    Type buffer_type = rewriter.getType<ht::HostBufferType>();
    auto buffer_op = rewriter.create<dht::GetBufferOp>(
        loc, buffer_type, chain.getType(), tensor_op.getResult(), chain);
    auto set_op = rewriter.create<dht::SetTensorOp_ui8>(
        loc, chain.getType(), tensor_op.getResult(), chain);
    set_op->setAttr("values", rewriter.getArrayAttr(values));
    chain = rewriter.create<MemCopyOp>(loc, global_op.getResult(),
                                       buffer_op.getResult(0), stream, set_op);
  }
  return rewriter.create<StreamSynchronizeOp>(loc, stream, chain).getResult();
}

LogicalResult ConvertGpuModulePattern::matchAndRewrite(
    mlir::gpu::GPUModuleOp module_op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {
  auto data = module_op->getAttrOfType<StringAttr>(getGpuBinaryAttrName());
  if (!data)
    return rewriter.notifyMatchFailure(module_op, "no device code attribute");
  Location loc = module_op->getLoc();
  auto constants =
      module_op->getAttrOfType<DictionaryAttr>(getGpuConstantsAttrName());
  SmallVector<Type, 2> return_types = {rewriter.getType<ModuleType>()};
  if (constants)
    return_types.push_back(rewriter.getType<compiler::ChainType>());
  mlir::FunctionType func_type =
      rewriter.getFunctionType(rewriter.getType<ContextType>(), return_types);
  FuncOp func_op = rewriter.replaceOpWithNewOp<FuncOp>(
      module_op, module_op.getName(), func_type);
  rewriter.setInsertionPointToEnd(func_op.addEntryBlock());
  Value context = func_op.getArgument(0);
  std::string binary = data.getValue().str();  // Add trailing zero.
  Value load_op = rewriter.create<ModuleLoadOp>(
      loc, context, mlir::StringRef(binary.data(), binary.size() + 1));
  SmallVector<Value, 2> return_values = {load_op};
  if (constants) {
    return_values.push_back(
        CreateGlobalInitialization(rewriter, loc, context, load_op, constants));
  }
  rewriter.create<compiler::ReturnOp>(loc, return_values);
  return success();
}

LogicalResult ConvertLaunchFuncPattern::matchAndRewrite(
    mlir::gpu::LaunchFuncOp launch_op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {
  if (adaptor.asyncDependencies().empty() || !launch_op.asyncToken())
    return rewriter.notifyMatchFailure(launch_op, "no async deps or no result");
  auto cast_op = adaptor.asyncDependencies().front().getDefiningOp<CastOp>();
  if (!IsCastToChainAndStream(cast_op))
    return rewriter.notifyMatchFailure(launch_op, "operand not def by cast");

  Location loc = launch_op->getLoc();
  Value chain = cast_op.getOperand(0);
  Value stream = cast_op.getOperand(1);
  Value context = rewriter.create<StreamGetContextOp>(loc, stream).getResult();
  auto func_op = SymbolTable::lookupNearestSymbolFrom<FuncOp>(
      launch_op, adaptor.kernel().getRootReference());
  auto once_op = rewriter.create<compiler::OnceOp>(
      loc, func_op.getType().getResults(), context, func_op.getName());
  auto kernel_name = adaptor.kernel().getLeafReference().getValue();
  auto get_func_op = rewriter.create<ModuleGetFunctionOp>(
      loc, once_op->getResult(0), kernel_name);
  if (once_op.getNumResults() > 1) {
    chain = rewriter.create<compiler::MergeChainsOp>(
        loc, chain.getType(), ValueRange({chain, once_op->getResult(1)}));
  }
  Value shared_mem_size = adaptor.dynamicSharedMemorySize();
  if (!shared_mem_size) {
    shared_mem_size =
        rewriter.create<compiler::ConstantUI32Op>(loc, 0).getResult();
  }
  auto new_op = rewriter.create<FunctionLaunchOp>(
      loc, chain.getType(), stream, get_func_op.getResult(),
      adaptor.gridSizeX(), adaptor.gridSizeY(), adaptor.gridSizeZ(),
      adaptor.blockSizeX(), adaptor.blockSizeY(), adaptor.blockSizeZ(),
      shared_mem_size, chain, adaptor.operands());
  auto token = CastToToken(rewriter, loc, {new_op.getResult(), stream});
  rewriter.replaceOp(launch_op, token);
  return success();
}

LogicalResult FoldConstCastPattern::matchAndRewrite(
    CastOp cast_op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {
  if (!IsTypes<IndexType>(cast_op.getOperandTypes()) ||
      !IsTypes<IntegerType>(cast_op.getResultTypes()))
    return rewriter.notifyMatchFailure(cast_op, "not cast from index to int");
  auto const_op = cast_op.getOperand(0).getDefiningOp<arith::ConstantOp>();
  if (!const_op)
    return rewriter.notifyMatchFailure(cast_op, "operand not def by constant");
  auto type = cast_op.getType(0).cast<IntegerType>();
  auto rewrite = [&](auto dummy) {
    APInt value = const_op.value().cast<IntegerAttr>().getValue();
    if (type.isUnsigned())
      value = value.zextOrTrunc(type.getWidth());
    else
      value = value.sextOrTrunc(type.getWidth());
    auto attr = rewriter.getIntegerAttr(type, value);
    rewriter.replaceOpWithNewOp<decltype(dummy)>(cast_op, type, attr);
    return success();
  };
  if (type.isUnsignedInteger(32)) return rewrite(compiler::ConstantUI32Op());
  if (type.isUnsignedInteger(64)) return rewrite(compiler::ConstantUI64Op());
  if (type.isInteger(32)) return rewrite(compiler::ConstantI32Op());
  if (type.isInteger(64)) return rewrite(compiler::ConstantI64Op());
  return rewriter.notifyMatchFailure(cast_op, "Unsupported type");
}

LogicalResult InlineConversionAsyncExecPattern::matchAndRewrite(
    conversion::AsyncExecuteOp exec_op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {
  if (adaptor.asyncDependencies().empty() || !exec_op.getAsyncToken())
    return rewriter.notifyMatchFailure(exec_op, "no async deps or no result");
  auto cast_op = adaptor.asyncDependencies().front().getDefiningOp<CastOp>();
  if (!IsCastToChainAndStream(cast_op))
    return rewriter.notifyMatchFailure(exec_op, "operand not def by cast");

  // Merge !tfrt_gpu_conversion.async.execute body into parent block.
  Operation *terminator = exec_op.getBody()->getTerminator();
  rewriter.mergeBlockBefore(exec_op.getBody(), exec_op, cast_op.getOperands());
  auto chain_and_stream = {terminator->getOperand(0), cast_op.getOperand(1)};
  auto token = CastToToken(rewriter, exec_op->getLoc(), chain_and_stream);
  rewriter.replaceOp(exec_op, token);
  rewriter.eraseOp(terminator);
  return success();
}

Value GetContextFromParentFunc(Operation *op) {
  auto func_op = op->getParentOfType<FuncOp>();
  auto get_ctx_ops = func_op.getOps<StreamGetContextOp>();
  if (get_ctx_ops.empty()) return nullptr;
  return (*get_ctx_ops.begin()).getResult();
}

LogicalResult ConvertGpuWaitToChainAndStreamPattern::matchAndRewrite(
    mlir::gpu::WaitOp wait_op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {
  auto operands = adaptor.getOperands();
  if (operands.empty() || !wait_op.asyncToken())
    return rewriter.notifyMatchFailure(wait_op, "no operands or not async");
  CastOp cast_from_stream_op;
  SmallVector<CastOp, 2> cast_from_event_ops;
  for (auto operand : operands) {
    CastOp cast_op = operand.getDefiningOp<CastOp>();
    if (IsCastToChainAndEvent(cast_op)) {
      cast_from_event_ops.push_back(cast_op);
      continue;
    }
    if (IsCastToChainAndStream(cast_op)) {
      if (cast_from_stream_op)
        return rewriter.notifyMatchFailure(wait_op, "more than one stream");
      cast_from_stream_op = cast_op;
      continue;
    }
    return rewriter.notifyMatchFailure(wait_op, "operand not def by cast");
  }

  // Merge operand chains if there is more than one.
  Location loc = wait_op.getLoc();
  Value chain = [&] {
    SmallVector<Value, 4> chains;
    if (cast_from_stream_op)
      chains.push_back(cast_from_stream_op.getOperand(0));
    llvm::transform(cast_from_event_ops, std::back_inserter(chains),
                    [](auto cast_op) { return cast_op.getOperand(0); });
    if (chains.size() == 1) return chains.front();
    Type chain_type = rewriter.getType<compiler::ChainType>();
    return rewriter.create<compiler::MergeChainsOp>(loc, chain_type, chains)
        .getResult();
  }();

  // Create stream if no operand is cast from stream.
  Value stream = [&]() -> Value {
    if (cast_from_stream_op) return cast_from_stream_op.getOperand(1);
    // Use stream block argument if it exists.
    for (auto argument : wait_op->getBlock()->getArguments())
      if (argument.getType().isa<StreamType>()) return argument;
    Value context = GetContextFromParentFunc(wait_op);
    return rewriter.create<StreamCreateOp>(loc, context);
  }();

  // Synchronize stream with all event operands.
  for (auto cast_op : cast_from_event_ops) {
    auto stream_wait_op = rewriter.create<StreamWaitOp>(
        loc, stream, cast_op.getOperand(1), chain);
    chain = stream_wait_op.getResult();
  }

  // Cast back to token if stream was synchronized.
  Value token = [&]() -> Value {
    if (cast_from_event_ops.empty()) return cast_from_stream_op.getResult(0);
    return CastToToken(rewriter, wait_op.getLoc(), {chain, stream});
  }();

  // Collect uses in other blocks and terminator uses.
  auto event_uses = llvm::make_filter_range(
      wait_op.asyncToken().getUses(), [&](const OpOperand &operand) {
        Operation *owner = operand.getOwner();
        if (owner->getBlock() != wait_op->getBlock()) return true;
        return owner->mightHaveTrait<OpTrait::IsTerminator>();
      });

  // Replace event uses with cast roundtrip to chain and event.
  if (!event_uses.empty()) {
    auto chain_and_event = CastToChainAndEvent(rewriter, loc, token);
    auto cast_from_event = CastToToken(rewriter, loc, chain_and_event);
    for (auto &use : event_uses) use.set(cast_from_event);
  }

  rewriter.replaceOp(wait_op, token);

  return success();
}

LogicalResult ConvertCastToEventRecordPattern::matchAndRewrite(
    mlir::UnrealizedConversionCastOp cast_op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {
  auto operands = adaptor.getOperands();
  if (!IsTokenType(TypeRange(operands)))
    return rewriter.notifyMatchFailure(cast_op, "not cast from token");

  if (!IsChainAndEventType(cast_op->getResultTypes()))
    return rewriter.notifyMatchFailure(cast_op, "not cast to chain and event");

  auto cast_to_token_op = operands.front().getDefiningOp<CastOp>();
  if (!IsCastToChainAndStream(cast_to_token_op))
    return rewriter.notifyMatchFailure(cast_op, "operand not def by cast");

  Location loc = cast_op->getLoc();
  Value chain = cast_to_token_op.getOperand(0);
  Value stream = cast_to_token_op.getOperand(1);
  Value context = GetContextFromParentFunc(cast_op);
  if (!context) context = rewriter.create<StreamGetContextOp>(loc, stream);

  Value event = rewriter.create<EventCreateOp>(loc, context).getResult();
  chain = rewriter.create<EventRecordOp>(loc, event, stream, chain).getResult();

  rewriter.replaceOp(cast_op, {chain, event});
  Value token = CastToToken(rewriter, loc, {chain, stream});
  rewriter.replaceOp(cast_to_token_op, token);

  return success();
}

LogicalResult ConvertAsyncExecToDoAsyncPattern::matchAndRewrite(
    async::ExecuteOp exec_op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {
  // Drop !async.token operands, they are not region arguments.
  auto operands = adaptor.operands();
  SmallVector<Value, 4> arguments(operands.begin(), operands.end());
  // Make all captures explicit arguments.
  SetVector<Value> captures;
  getUsedValuesDefinedAbove(exec_op->getRegions(), captures);
  llvm::transform(captures, std::back_inserter(arguments), [&](Value value) {
    return rewriter.getRemappedValue(value);
  });

  SmallVector<Type, 4> arg_types, result_types;
  if (failed(typeConverter->convertTypes(TypeRange(ValueRange(arguments)),
                                         arg_types)) ||
      failed(typeConverter->convertTypes(
          TypeRange(exec_op.getResultTypes()).drop_front(), result_types))) {
    return rewriter.notifyMatchFailure(exec_op, "failed to convert types");
  }

  Location loc = exec_op->getLoc();
  auto do_op = rewriter.create<test::DoAsyncOp>(loc, result_types, arguments);
  Region *region = &do_op.getRegion();
  Block *block = rewriter.createBlock(region, region->end(), arg_types);
  mlir::BlockAndValueMapping mapping;
  mapping.map(arguments, block->getArguments());
  rewriter.cloneRegionBefore(exec_op.getRegion(), *region, region->end(),
                             mapping);
  rewriter.mergeBlocks(block->getNextNode(), block,
                       block->getArguments().take_front(operands.size()));

  rewriter.setInsertionPoint(exec_op);  // Restore from createBlock() above.
  SmallVector<Value, 4> results = {
      CastTo<mlir::async::TokenType>(rewriter, loc, {})};
  llvm::copy(do_op.getResults(), std::back_inserter(results));
  rewriter.replaceOp(exec_op, results);

  Operation *terminator = region->back().getTerminator();
  rewriter.setInsertionPoint(terminator);
  rewriter.replaceOpWithNewOp<compiler::ReturnOp>(terminator,
                                                  terminator->getOperands());

  return success();
}

LogicalResult FoldAsyncAwaitPattern::matchAndRewrite(
    async::AwaitOp await_op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {
  if (await_op->getNumResults() == 0)
    return rewriter.eraseOp(await_op), success();
  rewriter.replaceOp(await_op, adaptor.getOperands());
  return success();
}

void AddChainAndStreamToFuncPass::runOnFunction() {
  RewritePatternSet patterns(&getContext());
  patterns.insert<AddChainAndStreamToFuncPattern>(&getContext());
  if (failed(applyOpPatternsAndFold(getOperation(), std::move(patterns))))
    return signalPassFailure();
}

void ConvertAsyncToChainAndEventPass::runOnFunction() {
  TypeConverter converter;
  // T -> T
  converter.addConversion([](Type type) { return type; });
  // !async.value<T> -> !async.value<convert(T)>...
  converter.addConversion(
      [&](mlir::async::ValueType type, SmallVectorImpl<Type> &results) {
        if (failed(converter.convertType(type.getValueType(), results)))
          return failure();
        llvm::transform(results, results.begin(), [](Type type) {
          return mlir::async::ValueType::get(type);
        });
        return success();
      });
  // !gpu.async.token -> !tfrt.chain, !tfrt_gpu.event
  converter.addConversion(
      [](mlir::gpu::AsyncTokenType type, SmallVectorImpl<Type> &results) {
        results.push_back(compiler::ChainType::get(type.getContext()));
        results.push_back(EventType::get(type.getContext()));
        return success();
      });

  RewritePatternSet patterns(&getContext());
  patterns.add<ConvertAsyncExecToChainAndEventPattern,
               ConvertAsyncYieldToChainAndEventPattern,
               SwapAsyncAwaitOfCastPattern>(converter, &getContext());

  ConversionTarget target(getContext());
  target.addDynamicallyLegalOp<mlir::async::AwaitOp, mlir::async::ExecuteOp,
                               mlir::async::YieldOp>(
      [&](Operation *op) { return converter.isLegal(op); });
  target.markUnknownOpDynamicallyLegal([](Operation *) { return true; });

  if (failed(
          applyPartialConversion(getOperation(), target, std::move(patterns))))
    return signalPassFailure();
}

void ConvertGpuToTfrtGpuPass::runOnOperation() {
  RewritePatternSet patterns(&getContext());
  auto converter = createMemrefToTfrtGpuConverter();
  converter.addConversion([](IndexType type) {
    return IntegerType::get(type.getContext(), 32, IntegerType::Unsigned);
  });
  patterns.add<ConvertMemsetPattern, ConvertMemcpyPattern,
               ConvertLaunchFuncPattern>(converter, &getContext());
  patterns.add<ConvertGetGlobalPattern, ConvertGpuModulePattern,
               InlineConversionAsyncExecPattern,
               ConvertGpuWaitToChainAndStreamPattern,
               ConvertCastToEventRecordPattern, FoldConstCastPattern>(
      &getContext());
  ConversionTarget target(getContext());
  target.addIllegalDialect<mlir::gpu::GPUDialect>();
  target.addIllegalOp<conversion::AsyncExecuteOp>();
  target.addDynamicallyLegalOp<CastOp>([&](CastOp cast_op) {
    // Trigger ConvertCastToEventRecordPattern and FoldConstCastPattern.
    return !IsCastFromChainAndEvent(cast_op) &&
           converter.isLegal(cast_op->getOperandTypes());
  });
  target.addDynamicallyLegalOp<memref::GetGlobalOp>([&](Operation *op) {
    // Some ops (e.g. lmhlo.fusion) leave the get_global result unused, except
    // for a cast which will only be removed later. Leave those untouched.
    return !op->getAttrOfType<SymbolRefAttr>(getGpuModuleAttrName());
  });
  target.markUnknownOpDynamicallyLegal([](Operation *) { return true; });

  if (failed(
          applyPartialConversion(getOperation(), target, std::move(patterns))))
    return signalPassFailure();
}

void ConvertAsyncToTfrtPass::runOnFunction() {
  TypeConverter converter;
  // T -> T
  converter.addConversion([](Type type) { return type; });
  // !async.token -> null
  converter.addConversion([](mlir::async::TokenType type,
                             SmallVectorImpl<Type> &) { return success(); });
  // !async.value<T> -> T
  converter.addConversion([&](mlir::async::ValueType type) {
    return converter.convertType(type.getValueType());
  });

  RewritePatternSet patterns(&getContext());
  // Folds pairs of A-B-A casts before outlining async.execute regions.
  populateReconcileUnrealizedCastsPatterns(patterns);
  patterns.add<ConvertAsyncExecToDoAsyncPattern, FoldAsyncAwaitPattern>(
      converter, &getContext());

  ConversionTarget target(getContext());
  target.addIllegalOp<async::AwaitOp, async::ExecuteOp, async::YieldOp>();
  target.markUnknownOpDynamicallyLegal([&](Operation *) { return true; });

  if (failed(
          applyPartialConversion(getOperation(), target, std::move(patterns))))
    return signalPassFailure();
}

static Value MaterializeCast(OpBuilder &builder, Type type, ValueRange values,
                             Location loc) {
  return builder.create<CastOp>(loc, type, values).getResult(0);
}

mlir::StringRef getGpuBinaryAttrName() { return "binary"; }
mlir::StringRef getGpuConstantsAttrName() { return "constants"; }
mlir::StringRef getGpuModuleAttrName() { return "gpu_module"; }

TypeConverter createMemrefToTfrtGpuConverter() {
  TypeConverter converter;
  converter.addConversion([](Type type) { return type; });
  converter.addConversion([&](BaseMemRefType type) {
    return tfrt::gpu::BufferType::get(type.getContext());
  });
  converter.addArgumentMaterialization(MaterializeCast);
  converter.addSourceMaterialization(MaterializeCast);
  converter.addTargetMaterialization(MaterializeCast);
  return converter;
}

void populateGpuToTfrtGpuPasses(OpPassManager &pm) {
  pm.addPass(std::make_unique<AddChainAndStreamToFuncPass>());
  pm.addPass(std::make_unique<ConvertAsyncToChainAndEventPass>());
  pm.addPass(std::make_unique<ConvertGpuToTfrtGpuPass>());
  pm.addPass(createReconcileUnrealizedCastsPass());
  pm.addPass(std::make_unique<ConvertAsyncToTfrtPass>());
}

void registerPasses() {
  // Only register the pipeline, not the individual passes.
  // TODO(csigg): test passes individually, split and move test inputs.
  PassPipelineRegistration<>(
      "gpu-to-tfrt-gpu",
      "Pass pipeline to convert from MLIR's gpu and async dialects to TFRT.",
      [](OpPassManager &pm) { tfrt::gpu::populateGpuToTfrtGpuPasses(pm); });
}

}  // namespace gpu
}  // namespace tfrt
