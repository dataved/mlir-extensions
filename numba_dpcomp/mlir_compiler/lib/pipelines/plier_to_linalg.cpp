// Copyright 2021 Intel Corporation
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

#include "pipelines/plier_to_linalg.hpp"

#include "mlir/Conversion/AffineToStandard/AffineToStandard.h"
#include <mlir/Analysis/AffineAnalysis.h>
#include <mlir/Analysis/BufferViewFlowAnalysis.h>
#include <mlir/Conversion/ReconcileUnrealizedCasts/ReconcileUnrealizedCasts.h>
#include <mlir/Conversion/SCFToStandard/SCFToStandard.h>
#include <mlir/Dialect/Affine/IR/AffineOps.h>
#include <mlir/Dialect/Arithmetic/IR/Arithmetic.h>
#include <mlir/Dialect/Bufferization/IR/Bufferization.h>
#include <mlir/Dialect/Bufferization/Transforms/Bufferize.h>
#include <mlir/Dialect/Bufferization/Transforms/Passes.h>
#include <mlir/Dialect/Linalg/IR/Linalg.h>
#include <mlir/Dialect/Linalg/Passes.h>
#include <mlir/Dialect/Linalg/Transforms/Transforms.h>
#include <mlir/Dialect/MemRef/IR/MemRef.h>
#include <mlir/Dialect/SCF/Passes.h>
#include <mlir/Dialect/SCF/SCF.h>
#include <mlir/Dialect/SCF/Transforms.h>
#include <mlir/Dialect/StandardOps/Transforms/FuncConversions.h>
#include <mlir/Dialect/StandardOps/Transforms/Passes.h>
#include <mlir/Dialect/Tensor/IR/Tensor.h>
#include <mlir/Dialect/Tensor/Transforms/Passes.h>
#include <mlir/IR/Dialect.h>
#include <mlir/Pass/Pass.h>
#include <mlir/Pass/PassManager.h>
#include <mlir/Transforms/DialectConversion.h>
#include <mlir/Transforms/GreedyPatternRewriteDriver.h>
#include <mlir/Transforms/LoopUtils.h>
#include <mlir/Transforms/Passes.h>

#include "plier/dialect/plier/dialect.hpp"
#include "plier/dialect/plier_util/dialect.hpp"

#include "pipelines/plier_to_scf.hpp"
#include "pipelines/plier_to_std.hpp"
#include "pipelines/pre_low_simplifications.hpp"

#include "plier/Conversion/SCFToAffine/SCFToAffine.h"
#include "plier/transforms/call_lowering.hpp"
#include "plier/transforms/canonicalize_reductions.hpp"
#include "plier/transforms/cast_utils.hpp"
#include "plier/transforms/common_opts.hpp"
#include "plier/transforms/const_utils.hpp"
#include "plier/transforms/cse.hpp"
#include "plier/transforms/inline_utils.hpp"
#include "plier/transforms/loop_rewrites.hpp"
#include "plier/transforms/loop_utils.hpp"
#include "plier/transforms/memory_rewrites.hpp"
#include "plier/transforms/pipeline_utils.hpp"
#include "plier/transforms/promote_bool_memref.hpp"
#include "plier/transforms/promote_to_parallel.hpp"
#include "plier/transforms/rewrite_wrapper.hpp"
#include "plier/transforms/type_conversion.hpp"
#include "plier/transforms/uplift_math_calls.hpp"

#include "base_pipeline.hpp"
#include "loop_utils.hpp"
#include "mangle.hpp"
#include "plier/compiler/pipeline_registry.hpp"
#include "py_func_resolver.hpp"
#include "py_linalg_resolver.hpp"

#include <cctype>

namespace {
int64_t getOptLevel(mlir::Operation *op) {
  assert(op);
  auto attr = op->getAttr(plier::attributes::getOptLevelName())
                  .dyn_cast_or_null<mlir::IntegerAttr>();
  if (!attr) {
    return 0;
  }
  return std::max(static_cast<int64_t>(0), attr.getInt());
}

mlir::LogicalResult applyOptimizations(
    mlir::FuncOp op, const mlir::FrozenRewritePatternSet &patterns,
    mlir::AnalysisManager am,
    llvm::function_ref<mlir::LogicalResult(mlir::FuncOp)> additionalOpts =
        nullptr) {
  bool repeat = false;
  do {
    repeat = false;
    (void)mlir::applyPatternsAndFoldGreedily(op, patterns);
    if (mlir::succeeded(plier::applyCSE(op.getRegion(), false))) {
      repeat = true;
    }

    auto memOptRes = plier::optimizeMemoryOps(am);
    if (!memOptRes) {
      op.emitError() << "Failed to build memssa analysis";
      return mlir::failure();
    }
    if (mlir::succeeded(*memOptRes)) {
      repeat = true;
    }

    if (additionalOpts && mlir::succeeded(additionalOpts(op))) {
      repeat = true;
    }
    if (repeat) {
      am.invalidate({});
    }
  } while (repeat);
  return mlir::success();
}

enum class ArrayLayout { C, F, A };

bool parse_layout(llvm::StringRef &name, ArrayLayout &layout) {
  if (name.consume_back("C")) {
    layout = ArrayLayout::C;
    return true;
  }
  if (name.consume_back("F")) {
    layout = ArrayLayout::F;
    return true;
  }
  if (name.consume_back("A")) {
    layout = ArrayLayout::A;
    return true;
  }
  return false;
}

template <typename T> bool consume_int_back(llvm::StringRef &name, T &result) {
  unsigned len = 0;
  auto tmp_name = name;
  while (!tmp_name.empty() && std::isdigit(tmp_name.back())) {
    ++len;
    tmp_name = tmp_name.drop_back();
  }
  tmp_name = name.substr(name.size() - len);
  if (!tmp_name.consumeInteger<T>(10, result)) {
    name = name.substr(0, name.size() - len);
    return true;
  }
  return false;
}

struct ArrayDesc {
  unsigned dims = 0;
  ArrayLayout layout = {};
  llvm::StringRef name;
};

llvm::Optional<ArrayDesc> parse_array_desc(llvm::StringRef &name) {
  unsigned num_dims = 0;
  ArrayLayout layout = {};
  if (name.consume_front("array(") && name.consume_back(")") &&
      parse_layout(name, layout) && name.consume_back(", ") &&
      name.consume_back("d") && consume_int_back(name, num_dims) &&
      name.consume_back(", ") && !name.empty()) {
    return ArrayDesc{num_dims, layout, name};
  }
  return {};
}

mlir::Type map_array_type(mlir::MLIRContext &ctx, mlir::TypeConverter &conveter,
                          llvm::StringRef &name) {
  if (auto desc = parse_array_desc(name)) {
    if (desc->layout == ArrayLayout::C || desc->layout == ArrayLayout::F ||
        desc->layout == ArrayLayout::A) {
      if (auto type =
              conveter.convertType(plier::PyType::get(&ctx, desc->name))) {
        llvm::SmallVector<int64_t> shape(desc->dims, -1);
        return mlir::RankedTensorType::get(shape, type);
      }
    }
  }
  return nullptr;
}

mlir::Type map_plier_type(mlir::TypeConverter &converter, mlir::Type type) {
  if (auto pyType = type.dyn_cast<plier::PyType>()) {
    auto name = pyType.getName();
    return map_array_type(*type.getContext(), converter, name);
  }
  return nullptr;
}

void rerun_scf_pipeline(mlir::Operation *op) {
  assert(nullptr != op);
  auto marker =
      mlir::StringAttr::get(op->getContext(), plierToScfPipelineName());
  auto mod = op->getParentOfType<mlir::ModuleOp>();
  assert(nullptr != mod);
  plier::add_pipeline_jump_marker(mod, marker);
}

static mlir::Value skipCasts(mlir::Value val) {
  auto getArg = [](mlir::Value arg) -> mlir::Value {
    auto cast = arg.getDefiningOp<mlir::UnrealizedConversionCastOp>();
    if (!cast)
      return {};

    auto inputs = cast.inputs();
    if (inputs.size() != 1)
      return {};

    return inputs.front();
  };
  while (auto arg = getArg(val))
    val = arg;

  return val;
};

mlir::LogicalResult
lowerPrange(plier::PyCallOp op, mlir::ValueRange operands,
            llvm::ArrayRef<std::pair<llvm::StringRef, mlir::Value>> kwargs,
            mlir::PatternRewriter &rewriter) {
  auto parent = op->getParentOp();
  auto setAttr = [](mlir::scf::ForOp op) {
    op->setAttr(plier::attributes::getParallelName(),
                mlir::UnitAttr::get(op->getContext()));
  };
  if (mlir::succeeded(lowerRange(op, operands, kwargs, rewriter, setAttr))) {
    rerun_scf_pipeline(parent);
    return mlir::success();
  }
  return mlir::failure();
}

mlir::LogicalResult
lowerLen(plier::PyCallOp op, mlir::ValueRange operands,
         llvm::ArrayRef<std::pair<llvm::StringRef, mlir::Value>> kwargs,
         mlir::PatternRewriter &rewriter) {
  if (operands.size() != 1 || !kwargs.empty())
    return mlir::failure();

  auto arg = skipCasts(operands.front());
  if (!arg.getType().isa<mlir::RankedTensorType>())
    return mlir::failure();

  rerun_scf_pipeline(op);

  auto loc = op.getLoc();
  auto dim = rewriter.createOrFold<mlir::tensor::DimOp>(loc, arg, 0);
  rewriter.replaceOpWithNewOp<plier::CastOp>(op, op.getType(), dim);
  return mlir::success();
}

using kwargs_t = llvm::ArrayRef<std::pair<llvm::StringRef, mlir::Value>>;
using func_t = mlir::LogicalResult (*)(plier::PyCallOp, mlir::ValueRange,
                                       kwargs_t, mlir::PatternRewriter &);
static const std::pair<llvm::StringRef, func_t> builtinFuncsHandlers[] = {
    // clang-format off
    {"numba.prange", lowerPrange},
    {"len", lowerLen},
    // clang-format on
};

struct NumpyCallsLowering final : public plier::CallOpLowering {
  NumpyCallsLowering(mlir::MLIRContext *context)
      : CallOpLowering(context),
        resolver("numba_dpcomp.mlir.numpy.funcs", "registry") {}

protected:
  virtual mlir::LogicalResult
  resolveCall(plier::PyCallOp op, mlir::StringRef name, mlir::Location loc,
              mlir::PatternRewriter &rewriter, mlir::ValueRange args,
              KWargs kwargs) const override {
    for (auto &handler : builtinFuncsHandlers)
      if (handler.first == name)
        return handler.second(op, args, kwargs, rewriter);

    auto res = resolver.rewriteFunc(name, loc, rewriter, args, kwargs);
    if (!res)
      return mlir::failure();

    auto results = std::move(res).getValue();
    assert(results.size() == op->getNumResults());
    for (auto it : llvm::enumerate(results)) {
      auto i = it.index();
      auto r = it.value();
      auto dstType = op->getResultTypes()[i];
      if (dstType != r.getType())
        results[i] = rewriter.create<plier::CastOp>(loc, dstType, r);
    }

    rerun_scf_pipeline(op);
    rewriter.replaceOp(op, results);
    return mlir::success();
  }

private:
  PyLinalgResolver resolver;
};

struct NumpyAttrsLowering : public mlir::OpRewritePattern<plier::GetattrOp> {
  NumpyAttrsLowering(mlir::MLIRContext *context)
      : OpRewritePattern(context),
        resolver("numba_dpcomp.mlir.numpy.funcs", "registry") {}

  mlir::LogicalResult
  matchAndRewrite(plier::GetattrOp op,
                  mlir::PatternRewriter &rewriter) const override {
    auto arg = skipCasts(op.value());

    if (!arg.getType().isa<mlir::ShapedType>())
      return mlir::failure();

    auto loc = op.getLoc();
    auto res = resolver.rewriteAttr(llvm::Twine("array.") + op.name(), loc,
                                    rewriter, arg);
    if (!res)
      return mlir::failure();

    auto results = *res;
    assert(results.size() == op->getNumResults());
    for (auto it : llvm::enumerate(results)) {
      auto i = it.index();
      auto r = it.value();
      auto dstType = op->getResultTypes()[i];
      if (dstType != r.getType())
        results[i] = rewriter.create<plier::CastOp>(loc, dstType, r);
    }

    rerun_scf_pipeline(op);
    rewriter.replaceOp(op, results);
    return mlir::success();
  }

private:
  PyLinalgResolver resolver;
};

struct NumpyBinOpLowering : public mlir::OpRewritePattern<plier::BinOp> {
  NumpyBinOpLowering(mlir::MLIRContext *context)
      : OpRewritePattern(context),
        resolver("numba_dpcomp.mlir.numpy.funcs", "registry") {}

  mlir::LogicalResult
  matchAndRewrite(plier::BinOp op,
                  mlir::PatternRewriter &rewriter) const override {
    auto lhs = skipCasts(op.lhs());
    auto rhs = skipCasts(op.rhs());

    if (!lhs.getType().isa<mlir::ShapedType>() &&
        !rhs.getType().isa<mlir::ShapedType>())
      return mlir::failure();

    auto name = op.op();

    for (auto it : plier::getOperators()) {
      if (it.op == name) {
        auto res = resolver.rewriteFunc(llvm::Twine("operator.") + it.name,
                                        op.getLoc(), rewriter, {lhs, rhs}, {});
        if (!res)
          return mlir::failure();

        rerun_scf_pipeline(op);
        rewriter.replaceOp(op, *res);
        return mlir::success();
      }
    }
    return mlir::failure();
  }

private:
  PyLinalgResolver resolver;
};

// TODO: remove
struct ExternalCallsLowering final : public plier::CallOpLowering {
  using CallOpLowering::CallOpLowering;

protected:
  virtual mlir::LogicalResult
  resolveCall(plier::PyCallOp op, mlir::StringRef name, mlir::Location loc,
              mlir::PatternRewriter &rewriter, mlir::ValueRange args,
              KWargs kwargs) const override {
    if (!kwargs.empty())
      return mlir::failure(); // TODO: kwargs support

    auto types = args.getTypes();
    auto mangledName = mangle(name, types);
    if (mangledName.empty())
      return mlir::failure();

    auto mod = op->getParentOfType<mlir::ModuleOp>();
    assert(mod);
    auto externalFunc = mod.lookupSymbol<mlir::FuncOp>(mangledName);
    if (!externalFunc) {
      externalFunc = resolver.getFunc(name, types);
      if (externalFunc) {
        externalFunc.setPrivate();
        externalFunc.setName(mangledName);
      }
    }
    if (!externalFunc)
      return mlir::failure();

    assert(externalFunc.getType().getNumResults() == op->getNumResults());

    llvm::SmallVector<mlir::Value> castedArgs(args.size());
    auto funcTypes = externalFunc.getType().getInputs();
    for (auto it : llvm::enumerate(args)) {
      auto arg = it.value();
      auto i = it.index();
      auto dstType = funcTypes[i];
      if (arg.getType() != dstType)
        castedArgs[i] = rewriter.createOrFold<plier::CastOp>(loc, dstType, arg);
      else
        castedArgs[i] = arg;
    }

    auto newFuncCall =
        rewriter.create<mlir::CallOp>(loc, externalFunc, castedArgs);

    auto results = newFuncCall.getResults();
    llvm::SmallVector<mlir::Value> castedResults(results.size());

    for (auto it : llvm::enumerate(results)) {
      auto i = static_cast<unsigned>(it.index());
      auto res = it.value();
      auto oldResType = op->getResult(i).getType();
      if (res.getType() != oldResType)
        castedResults[i] =
            rewriter.createOrFold<plier::CastOp>(loc, oldResType, res);
      else
        castedResults[i] = res;
    }

    rerun_scf_pipeline(op);
    rewriter.replaceOp(op, castedResults);
    return mlir::success();
  }

private:
  PyFuncResolver resolver;
};

struct UnrankedToElementCasts
    : public mlir::OpConversionPattern<plier::CastOp> {
  using OpConversionPattern::OpConversionPattern;

  mlir::LogicalResult
  matchAndRewrite(plier::CastOp op, plier::CastOp::Adaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    auto value = adaptor.value();
    auto srcType = value.getType();
    auto converter = *getTypeConverter();
    auto dstType = converter.convertType(op.getType());
    if (!dstType)
      return mlir::failure();

    auto isCompatible = [](mlir::Type tensor, mlir::Type element) {
      if (auto tensorType = tensor.dyn_cast<mlir::RankedTensorType>())
        return tensorType.getRank() == 0 &&
               tensorType.getElementType() == element;

      return false;
    };
    if (isCompatible(srcType, dstType)) {
      rewriter.replaceOpWithNewOp<mlir::tensor::ExtractOp>(op, value);
      return mlir::success();
    }
    if (isCompatible(dstType, srcType)) {
      auto singleElemTensor = rewriter.create<mlir::tensor::FromElementsOp>(
          op.getLoc(), op.value());
      rewriter.replaceOpWithNewOp<mlir::tensor::CollapseShapeOp>(
          op, dstType, singleElemTensor,
          llvm::ArrayRef<mlir::ReassociationExprs>{});
      return mlir::success();
    }
    return mlir::failure();
  }
};

static bool isUniTuple(mlir::TupleType type) {
  auto count = type.size();
  if (count == 0)
    return false;

  auto elemType = type.getType(0);
  for (auto i : llvm::seq<size_t>(1, count)) {
    if (type.getType(i) != elemType)
      return false;
  }
  return true;
}

static bool isUniTuple(mlir::Type type) {
  auto tupleType = type.dyn_cast<mlir::TupleType>();
  return tupleType && isUniTuple(tupleType);
}

class GetItemUniTupleConversionPattern
    : public mlir::OpConversionPattern<plier::GetItemOp> {
public:
  using OpConversionPattern<plier::GetItemOp>::OpConversionPattern;

  mlir::LogicalResult
  matchAndRewrite(plier::GetItemOp op, plier::GetItemOp::Adaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const final {
    auto container = adaptor.value();
    auto tupleType = container.getType().dyn_cast<mlir::TupleType>();
    if (!tupleType || !isUniTuple(tupleType))
      return mlir::failure();

    auto index = adaptor.index();
    if (mlir::getConstantIntValue(index))
      return mlir::failure();

    auto loc = op->getLoc();
    auto elemType = tupleType.getType(0);
    auto count = static_cast<unsigned>(tupleType.size());
    llvm::SmallVector<mlir::Value> elems(count);
    for (auto i : llvm::seq(0u, count)) {
      auto idx = rewriter.create<mlir::arith::ConstantIndexOp>(loc, i);
      elems[i] =
          rewriter.create<plier::GetItemOp>(loc, elemType, container, idx);
    }

    auto tensor = rewriter.create<mlir::tensor::FromElementsOp>(loc, elems);
    rewriter.replaceOpWithNewOp<plier::GetItemOp>(op, elemType, tensor, index);
    return mlir::success();
  }
};

struct NumpyCallsLoweringPass
    : public plier::RewriteWrapperPass<
          NumpyCallsLoweringPass, void, void, NumpyCallsLowering,
          NumpyAttrsLowering, NumpyBinOpLowering, ExternalCallsLowering> {};

static mlir::Value index_cast(mlir::Value value, mlir::Location loc,
                              mlir::OpBuilder &builder) {
  if (!value.getType().isa<mlir::IndexType>()) {
    auto indexType = builder.getIndexType();
    auto res = builder.create<plier::CastOp>(loc, indexType, value);
    rerun_scf_pipeline(res);
    return res;
  }
  return value;
}

static bool isValidGetitemIndex(mlir::Type type) {
  if (type.isa<plier::SliceType>())
    return true;

  if (auto tupleType = type.dyn_cast<mlir::TupleType>())
    return llvm::all_of(tupleType.getTypes(), &isValidGetitemIndex);

  return type.isa<mlir::IntegerType, mlir::IndexType, plier::LiteralType>();
}

static mlir::LogicalResult
computeIndices(mlir::OpBuilder &builder, mlir::Location loc, mlir::Value value,
               mlir::Value index,
               llvm::SmallVectorImpl<mlir::OpFoldResult> &offsets,
               llvm::SmallVectorImpl<mlir::OpFoldResult> &sizes,
               llvm::SmallVectorImpl<mlir::OpFoldResult> &strides,
               llvm::SmallVectorImpl<unsigned> &dimsIndices) {
  auto shapedType = value.getType().cast<mlir::ShapedType>();
  auto indexType = builder.getIndexType();
  auto getPos =
      [&](mlir::Value indexVal,
          unsigned dim) -> std::tuple<mlir::OpFoldResult, mlir::OpFoldResult,
                                      mlir::OpFoldResult, bool> {
    auto valType = indexVal.getType();
    if (auto sliceType = valType.dyn_cast<plier::SliceType>()) {
      auto getItemOrConst = [&](unsigned i) -> mlir::Value {
        assert(i < 3);
        auto createInd = [&](int64_t i) {
          return builder.create<mlir::arith::ConstantIndexOp>(loc, i);
        };
        return builder.create<plier::SliceGetItemOp>(loc, indexType, indexVal,
                                                     value, createInd(i), dim);
      };
      auto offset = getItemOrConst(0);
      auto end = getItemOrConst(1);
      auto stride = getItemOrConst(2);
      auto size =
          builder.create<mlir::arith::SubIOp>(loc, end, offset).getResult();
      return {offset, size, stride, true};
    } else if (auto literal = valType.dyn_cast<plier::LiteralType>()) {
      auto offset = literal.getValue();
      return {offset, builder.getIndexAttr(1), builder.getIndexAttr(1), false};
    } else {
      auto offset = index_cast(indexVal, loc, builder);
      return {offset, builder.getIndexAttr(1), builder.getIndexAttr(1), false};
    }
  };

  bool isMemref = shapedType.isa<mlir::MemRefType>();
  auto getDim = [&](mlir::Value val, unsigned dim) -> mlir::Value {
    if (isMemref) {
      return builder.createOrFold<mlir::memref::DimOp>(loc, val, dim);
    } else {
      return builder.createOrFold<mlir::tensor::DimOp>(loc, val, dim);
    }
  };

  auto makeFullSlice =
      [&](unsigned dim) -> std::tuple<mlir::OpFoldResult, mlir::OpFoldResult,
                                      mlir::OpFoldResult> {
    auto begin = builder.getIndexAttr(0);
    auto end = getDim(value, dim);
    auto step = builder.getIndexAttr(1);
    return {begin, end, step};
  };

  auto rank = static_cast<unsigned>(shapedType.getRank());
  offsets.resize(rank);
  sizes.resize(rank);
  strides.resize(rank);

  if (auto tupleType = index.getType().dyn_cast<mlir::TupleType>()) {
    auto count = static_cast<unsigned>(tupleType.size());
    if (count > rank)
      return mlir::failure();

    for (auto it : llvm::enumerate(tupleType)) {
      auto i = it.index();
      auto getitemInd =
          builder.create<mlir::arith::ConstantIndexOp>(loc, it.index());
      auto ind =
          builder.create<plier::GetItemOp>(loc, it.value(), index, getitemInd);
      bool isSlice = false;
      std::tie(offsets[i], sizes[i], strides[i], isSlice) =
          getPos(ind.getResult(), static_cast<unsigned>(i));
      if (isSlice)
        dimsIndices.emplace_back(i);
    }

    for (auto i : llvm::seq(count, rank)) {
      std::tie(offsets[i], sizes[i], strides[i]) = makeFullSlice(i);
      dimsIndices.emplace_back(i);
    }
  } else {
    bool isSlice = false;
    std::tie(offsets[0], sizes[0], strides[0], isSlice) = getPos(index, 0);
    if (isSlice)
      dimsIndices.emplace_back(0);

    for (auto i : llvm::seq(1u, rank)) {
      std::tie(offsets[i], sizes[i], strides[i]) = makeFullSlice(i);
      dimsIndices.emplace_back(i);
    }
  }

  mlir::Value zero;
  auto getZero = [&]() {
    if (!zero)
      zero = builder.create<mlir::arith::ConstantIndexOp>(loc, 0);
    return zero;
  };

  auto foldConst = [&](mlir::Value val) -> mlir::OpFoldResult {
    if (auto intVal = mlir::getConstantIntValue(val))
      return builder.getIndexAttr(*intVal);

    return val;
  };

  for (auto i : llvm::seq(0u, rank)) {
    auto val = offsets[i];
    mlir::Value idx;
    if (auto v = val.dyn_cast<mlir::Value>()) {
      idx = v;
    } else {
      auto attr = val.get<mlir::Attribute>();
      auto attrVal = attr.cast<mlir::IntegerAttr>().getValue().getSExtValue();
      idx = builder.create<mlir::arith::ConstantIndexOp>(loc, attrVal);
    }

    auto isNeg = builder.createOrFold<mlir::arith::CmpIOp>(
        loc, mlir::arith::CmpIPredicate::slt, idx, getZero());
    auto negIndex =
        builder.createOrFold<mlir::arith::AddIOp>(loc, getDim(value, i), idx);
    offsets[i] = foldConst(
        builder.createOrFold<mlir::SelectOp>(loc, isNeg, negIndex, idx));
  }

  return mlir::success();
}

static mlir::Value makeSubview(mlir::OpBuilder &builder, mlir::Location loc,
                               mlir::Value src,
                               llvm::ArrayRef<mlir::OpFoldResult> offsets,
                               llvm::ArrayRef<mlir::OpFoldResult> sizes,
                               llvm::ArrayRef<mlir::OpFoldResult> strides,
                               llvm::ArrayRef<unsigned> dimIndices) {
  auto srcType = src.getType().cast<mlir::ShapedType>();
  bool isTensor = srcType.isa<mlir::RankedTensorType>();
  auto srcRank = static_cast<unsigned>(srcType.getRank());
  auto dstRank = dimIndices.size();
  assert(srcRank > 0);
  assert(dstRank > 0);
  assert(dstRank <= srcRank);

  bool useReduceRank = true;

  mlir::Value view;
  if (isTensor) {
    auto resType =
        [&]() {
          auto tensorType = srcType.cast<mlir::RankedTensorType>();
          if (srcRank == dstRank || useReduceRank)
            return mlir::tensor::ExtractSliceOp::inferResultType(
                tensorType, offsets, sizes, strides);

          return mlir::tensor::ExtractSliceOp::inferRankReducedResultType(
              dstRank, tensorType, offsets, sizes, strides);
        }()
            .cast<mlir::RankedTensorType>();

    view = builder.create<mlir::tensor::ExtractSliceOp>(
        loc, resType, src, offsets, sizes, strides);
  } else {
    auto resType =
        [&]() {
          auto memrefType = srcType.cast<mlir::MemRefType>();
          if (srcRank == dstRank || useReduceRank)
            return mlir::memref::SubViewOp::inferResultType(memrefType, offsets,
                                                            sizes, strides);

          return mlir::memref::SubViewOp::inferRankReducedResultType(
              dstRank, memrefType, offsets, sizes, strides);
        }()
            .cast<mlir::MemRefType>();

    view = builder.create<mlir::memref::SubViewOp>(loc, resType, src, offsets,
                                                   sizes, strides);

    auto flatMemrefType =
        mlir::MemRefType::get(resType.getShape(), resType.getElementType());
    if (resType != flatMemrefType)
      view = builder.create<plier::ChangeLayoutOp>(loc, flatMemrefType, view);
  }

  if (srcRank != dstRank && useReduceRank) {
    llvm::SmallVector<int32_t> mapping(dimIndices.begin(), dimIndices.end());
    view = builder.createOrFold<plier::ReduceRankOp>(loc, view, mapping);
  }

  return view;
}

struct GetitemOpLowering : public mlir::OpConversionPattern<plier::GetItemOp> {
  using OpConversionPattern::OpConversionPattern;

  mlir::LogicalResult
  matchAndRewrite(plier::GetItemOp op, plier::GetItemOp::Adaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    auto value = adaptor.value();
    auto index = adaptor.index();
    auto type = value.getType();
    bool isMemref = type.isa<mlir::MemRefType>();
    bool isTensor = type.isa<mlir::TensorType>();

    if (!isMemref && !isTensor)
      return mlir::failure();

    if (!isValidGetitemIndex(index.getType()))
      return mlir::failure();

    auto loc = op.getLoc();
    auto shapedType = type.cast<mlir::ShapedType>();
    llvm::SmallVector<mlir::OpFoldResult> offsets;
    llvm::SmallVector<mlir::OpFoldResult> sizes;
    llvm::SmallVector<mlir::OpFoldResult> strides;
    llvm::SmallVector<unsigned> dimsIndices;
    if (mlir::failed(computeIndices(rewriter, loc, value, index, offsets, sizes,
                                    strides, dimsIndices)))
      return mlir::failure();

    mlir::Value res;
    auto elemType = shapedType.getElementType();
    auto elemTypeSignless = plier::makeSignlessType(elemType);
    if (elemType != elemTypeSignless) {
      if (isMemref) {
        auto memrefType = type.cast<mlir::MemRefType>();
        auto signlessType = mlir::MemRefType::get(
            memrefType.getShape(), elemTypeSignless, memrefType.getLayout());
        value = rewriter.create<plier::SignCastOp>(loc, signlessType, value);
      } else if (isTensor) {
        auto tensorType = type.cast<mlir::RankedTensorType>();
        auto signlessType = mlir::RankedTensorType::get(
            tensorType.getShape(), elemTypeSignless, tensorType.getEncoding());
        value = rewriter.create<plier::SignCastOp>(loc, signlessType, value);
      } else {
        llvm_unreachable("Invalid getitem");
      }
    }

    if (!dimsIndices.empty()) {
      // Is slice
      res = makeSubview(rewriter, loc, value, offsets, sizes, strides,
                        dimsIndices);

      mlir::ShapedType resultTypeSignless =
          res.getType().cast<mlir::ShapedType>();
      mlir::Type resultType;
      if (isMemref) {
        resultType =
            mlir::MemRefType::get(resultTypeSignless.getShape(), elemType);
      } else if (isTensor) {
        resultType = mlir::RankedTensorType::get(resultTypeSignless.getShape(),
                                                 elemType);
      } else {
        llvm_unreachable("Invalid getitem");
      }

      if (resultType != resultTypeSignless)
        res = rewriter.create<plier::SignCastOp>(loc, resultType, res);
    } else {
      // Is single element
      auto toValues = [&](auto &vals) {
        llvm::SmallVector<mlir::Value> ret(vals.size());
        for (auto it : llvm::enumerate(vals)) {
          auto i = it.index();
          auto val = it.value();
          if (auto attr = val.template dyn_cast<mlir::Attribute>()) {
            ret[i] = rewriter.create<mlir::arith::ConstantIndexOp>(
                loc, attr.template cast<mlir::IntegerAttr>()
                         .getValue()
                         .getSExtValue());
          } else {
            ret[i] = val.template get<mlir::Value>();
          }
        }

        return ret;
      };
      if (isMemref) {
        res = rewriter.create<mlir::memref::LoadOp>(loc, value,
                                                    toValues(offsets));
      } else if (isTensor) {
        res = rewriter.create<mlir::tensor::ExtractOp>(loc, value,
                                                       toValues(offsets));
      } else {
        llvm_unreachable("Invalid getitem");
      }

      if (elemType != elemTypeSignless)
        res = rewriter.create<plier::SignCastOp>(loc, elemType, res);
    }

    rerun_scf_pipeline(op);
    rewriter.replaceOp(op, res);
    return mlir::success();
  }
};

struct GetitemOpArrLowering
    : public mlir::OpConversionPattern<plier::GetItemOp> {
  using OpConversionPattern::OpConversionPattern;

  mlir::LogicalResult
  matchAndRewrite(plier::GetItemOp op, plier::GetItemOp::Adaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    auto value = adaptor.value();
    auto index = adaptor.index();
    auto type = value.getType().dyn_cast<mlir::ShapedType>();
    if (!type || !type.hasRank())
      return mlir::failure();

    auto indexType = index.getType().dyn_cast<mlir::ShapedType>();
    if (!indexType || !indexType.hasRank())
      return mlir::failure();

    auto *converter = getTypeConverter();
    assert(converter);
    auto resType = converter->convertType(op.getType())
                       .dyn_cast_or_null<mlir::RankedTensorType>();
    if (!resType)
      return mlir::failure();

    auto loc = op->getLoc();
    auto getTesnor = [&](mlir::Value val) -> mlir::Value {
      auto valType = val.getType();
      if (valType.isa<mlir::RankedTensorType>())
        return val;

      auto memrefType = valType.cast<mlir::MemRefType>();
      auto tensorType = mlir::RankedTensorType::get(
          memrefType.getShape(), memrefType.getElementType());
      return rewriter.create<mlir::bufferization::ToTensorOp>(loc, tensorType,
                                                              val);
    };

    value = getTesnor(value);
    index = getTesnor(index);

    mlir::StringRef funcName = "array.__getitem__";
    const mlir::Value args[] = {value, index};

    rewriter.replaceOpWithNewOp<plier::PyCallOp>(op, op.getType(),
                                                 mlir::Value{}, funcName, args,
                                                 mlir::Value{}, llvm::None);

    //    auto mod = op->getParentOfType<mlir::ModuleOp>();
    //    assert(mod);

    //    auto funcType = mlir::FunctionType::get(rewriter.getContext(),
    //    {value.getType(), index.getType()}, resType);

    //    auto func = plier::add_function(rewriter, mod, funcName, funcType);

    //    rewriter.replaceOpWithNewOp<mlir::CallOp>(op, func, args);
    return mlir::success();
  }
};

struct SetitemOpLowering : public mlir::OpConversionPattern<plier::SetItemOp> {
  using OpConversionPattern::OpConversionPattern;

  mlir::LogicalResult
  matchAndRewrite(plier::SetItemOp op, plier::SetItemOp::Adaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    auto target = adaptor.target();
    auto targetType = target.getType().dyn_cast<mlir::ShapedType>();
    if (!targetType)
      return mlir::failure();

    auto index = adaptor.index();
    if (!isValidGetitemIndex(index.getType()))
      return mlir::failure();

    auto elemType = targetType.getElementType();
    auto signlessElemType = plier::makeSignlessType(elemType);
    if (auto targetTensorType =
            targetType.template dyn_cast<mlir::RankedTensorType>()) {
      mlir::OpBuilder::InsertionGuard g(rewriter);
      if (auto parentOp = target.getDefiningOp()) {
        rewriter.setInsertionPointAfter(parentOp);
      } else {
        rewriter.setInsertionPointToStart(target.getParentBlock());
      }

      llvm::SmallVector<mlir::OpOperand *> uses;
      // TODO: this doesnt work properly
      //      for (auto &use : target.getUses())
      //        uses.emplace_back(&use);

      //      for (auto &use : op.target().getUses())
      //        uses.emplace_back(&use);

      auto loc = target.getLoc();
      if (elemType != signlessElemType) {
        auto tensorType = targetTensorType.clone(signlessElemType);
        target = rewriter.create<plier::SignCastOp>(loc, tensorType, target);
      }
      auto memrefType =
          mlir::MemRefType::get(targetTensorType.getShape(), signlessElemType);
      target =
          rewriter.create<plier::PseudoCopyOp>(loc, target.getType(), target);
      //      target = rewriter.create<plier::ForceCopyOp>(loc,
      //      target.getType(), target);
      auto memref = rewriter.create<mlir::bufferization::ToMemrefOp>(
          loc, memrefType, target);
      target = memref;
      for (auto *use : uses) {
        auto useOp = use->getOwner();
        if (op.target().getDefiningOp() == useOp ||
            target.getDefiningOp() == useOp)
          continue;

        assert(nullptr != useOp);
        if (useOp != memref) {
          if (mlir::isa<plier::SetItemOp>(useOp)) {
            rewriter.updateRootInPlace(useOp, [&]() { use->set(memref); });
          } else {
            mlir::OpBuilder::InsertionGuard g(rewriter);
            rewriter.setInsertionPoint(useOp);
            mlir::Value newVal =
                rewriter.create<mlir::bufferization::ToTensorOp>(
                    useOp->getLoc(), memref);
            if (elemType != signlessElemType) {
              auto tensorType = targetTensorType.clone(elemType);
              newVal =
                  rewriter.create<plier::SignCastOp>(loc, tensorType, newVal);
            }
            rewriter.updateRootInPlace(useOp, [&]() { use->set(newVal); });
          }
        }
      }
    } else if (targetType.isa<mlir::MemRefType>()) {
      // nothing
    } else {
      return mlir::failure();
    }

    auto value = adaptor.value();
    auto loc = op.getLoc();
    llvm::SmallVector<mlir::OpFoldResult> offsets;
    llvm::SmallVector<mlir::OpFoldResult> sizes;
    llvm::SmallVector<mlir::OpFoldResult> strides;
    llvm::SmallVector<unsigned> dimsIndices;
    if (mlir::failed(computeIndices(rewriter, loc, target, index, offsets,
                                    sizes, strides, dimsIndices)))
      return mlir::failure();

    auto castElem = [&](mlir::Value val) -> mlir::Value {
      if (val.getType() != elemType) {
        // TODO
        val = rewriter.createOrFold<plier::CastOp>(loc, elemType, val);
        rerun_scf_pipeline(op);
      }
      if (elemType != signlessElemType)
        val = rewriter.create<plier::SignCastOp>(loc, signlessElemType, val);
      return val;
    };

    if (!dimsIndices.empty()) {
      // Is slice
      auto dst = makeSubview(rewriter, loc, target, offsets, sizes, strides,
                             dimsIndices);

      auto castView = [&](mlir::Value val) -> mlir::Value {
        auto viewType = val.getType().cast<mlir::MemRefType>();
        if (viewType.getElementType() != signlessElemType) {
          auto signlessMemref = viewType.clone(signlessElemType);
          val = rewriter.create<plier::SignCastOp>(loc, signlessMemref, val);
        }
        return val;
      };

      auto valType = value.getType();
      if (auto tensType = valType.dyn_cast<mlir::TensorType>()) {
        auto memrefType = mlir::MemRefType::get(tensType.getShape(),
                                                tensType.getElementType());
        auto src =
            rewriter
                .create<mlir::bufferization::ToMemrefOp>(loc, memrefType, value)
                .getResult();
        rewriter.replaceOpWithNewOp<mlir::linalg::CopyOp>(op, castView(src),
                                                          dst);
      } else if (valType.isa<mlir::MemRefType>()) {
        rewriter.replaceOpWithNewOp<mlir::linalg::CopyOp>(op, castView(value),
                                                          dst);
      } else {
        rewriter.replaceOpWithNewOp<mlir::linalg::FillOp>(op, castElem(value),
                                                          dst);
      }
    } else {
      // Is single element
      auto toValues = [&](auto &vals) {
        llvm::SmallVector<mlir::Value> ret(vals.size());
        for (auto it : llvm::enumerate(vals)) {
          auto i = it.index();
          auto val = it.value();
          if (auto attr = val.template dyn_cast<mlir::Attribute>()) {
            ret[i] = rewriter.create<mlir::arith::ConstantIndexOp>(
                loc, attr.template cast<mlir::IntegerAttr>()
                         .getValue()
                         .getSExtValue());
          } else {
            ret[i] = val.template get<mlir::Value>();
          }
        }

        return ret;
      };

      rewriter.replaceOpWithNewOp<mlir::memref::StoreOp>(
          op, castElem(value), target, toValues(offsets));
    }

    return mlir::success();
  }
};

struct CleanupLoads : public mlir::OpRewritePattern<mlir::memref::LoadOp> {
  using OpRewritePattern::OpRewritePattern;

  mlir::LogicalResult
  matchAndRewrite(mlir::memref::LoadOp op,
                  mlir::PatternRewriter &rewriter) const override {
    auto block = op->getBlock();
    auto it = mlir::Block::iterator(op);
    if (it == block->begin())
      return mlir::failure();

    --it;
    auto store = mlir::dyn_cast<mlir::memref::StoreOp>(*it);
    if (!store)
      return mlir::failure();

    if (store.memref() != op.memref() || store.indices() != op.indices())
      return mlir::failure();

    rewriter.replaceOp(op, store.value());
    return mlir::success();
  }
};

struct ReshapeChangeLayout
    : public mlir::OpRewritePattern<mlir::memref::ReshapeOp> {
  using OpRewritePattern::OpRewritePattern;

  mlir::LogicalResult
  matchAndRewrite(mlir::memref::ReshapeOp op,
                  mlir::PatternRewriter &rewriter) const override {
    auto cl = op.source().getDefiningOp<plier::ChangeLayoutOp>();
    if (!cl)
      return mlir::failure();

    auto src = cl.source();
    auto dstType = op.source().getType().cast<mlir::MemRefType>();

    auto rank = static_cast<unsigned>(dstType.getRank());
    if (rank == 0)
      return mlir::failure();

    int64_t offset;
    llvm::SmallVector<int64_t> strides;
    if (mlir::failed(mlir::getStridesAndOffset(dstType, strides, offset)))
      return mlir::failure();

    auto loc = cl.getLoc();

    llvm::SmallVector<mlir::OpFoldResult> sizesVals(rank);
    for (auto i : llvm::seq(0u, rank))
      sizesVals[i] = rewriter.createOrFold<mlir::memref::DimOp>(loc, src, i);

    int64_t stride = 1;
    llvm::SmallVector<mlir::Value> expectedStrides(rank);
    mlir::Value runningStride =
        rewriter.create<mlir::arith::ConstantIndexOp>(loc, 1);
    for (auto ii = rank; ii-- > 0;) {
      auto i = static_cast<unsigned>(ii);
      expectedStrides[i] = runningStride;

      int64_t size = dstType.getShape()[i];
      if (size == 0)
        continue;
      bool useSizeAsStride = stride == 1;
      if (size == mlir::ShapedType::kDynamicSize)
        stride = mlir::ShapedType::kDynamicSize;
      if (stride != mlir::ShapedType::kDynamicSize)
        stride *= size;

      auto sizeVal = sizesVals[i].get<mlir::Value>();
      if (useSizeAsStride)
        runningStride = sizeVal;
      else if (stride == mlir::ShapedType::kDynamicSize)
        runningStride =
            rewriter.create<mlir::arith::MulIOp>(loc, runningStride, sizeVal);
      else
        runningStride = rewriter.create<mlir::arith::ConstantIndexOp>(loc, 1);
    }

    mlir::OpFoldResult offsetVal = rewriter.getIndexAttr(offset);

    llvm::SmallVector<mlir::OpFoldResult> stridesVals(rank);
    auto offsetConst =
        rewriter.create<mlir::arith::ConstantIndexOp>(loc, offset);
    auto actualOffset =
        rewriter.createOrFold<plier::ExtractMemrefMetadataOp>(loc, src);
    auto cmp = rewriter.createOrFold<mlir::arith::CmpIOp>(
        loc, mlir::arith::CmpIPredicate::eq, offsetConst, actualOffset);
    for (auto i : llvm::seq(0u, rank)) {
      if (mlir::ShapedType::isDynamicStrideOrOffset(strides[i])) {
        stridesVals[i] = expectedStrides[i];
      } else {
        stridesVals[i] = rewriter.getIndexAttr(strides[i]);
      }
      auto actualStride =
          rewriter.createOrFold<plier::ExtractMemrefMetadataOp>(loc, src, i);
      auto strideVal =
          rewriter.create<mlir::arith::ConstantIndexOp>(loc, strides[i]);
      auto cmpTemp = rewriter.createOrFold<mlir::arith::CmpIOp>(
          loc, mlir::arith::CmpIPredicate::eq, strideVal, actualStride);
      cmp = rewriter.createOrFold<mlir::arith::AndIOp>(loc, cmp, cmpTemp);
    }

    auto trueBody = [&](mlir::OpBuilder &builder, mlir::Location loc) {
      auto res = builder
                     .create<mlir::memref::ReinterpretCastOp>(
                         loc, dstType, src, offsetVal, sizesVals, stridesVals)
                     .getResult();
      builder.create<mlir::scf::YieldOp>(loc, res);
    };
    auto falseBody = [&](mlir::OpBuilder &builder, mlir::Location loc) {
      llvm::SmallVector<mlir::Value> sizes;
      sizes.reserve(rank);
      auto shape = dstType.getShape();
      for (auto i : llvm::seq(0u, rank))
        if (mlir::ShapedType::isDynamic(shape[i]))
          sizes.emplace_back(sizesVals[i].get<mlir::Value>());

      auto res = builder.create<mlir::memref::AllocOp>(loc, dstType, sizes)
                     .getResult();
      builder.create<mlir::memref::CopyOp>(loc, src, res);
      builder.create<mlir::scf::YieldOp>(loc, res);
    };

    auto res =
        rewriter.create<mlir::scf::IfOp>(loc, dstType, cmp, trueBody, falseBody)
            .getResult(0);
    rewriter.replaceOpWithNewOp<mlir::memref::ReshapeOp>(op, op.getType(), res,
                                                         op.shape());
    return mlir::success();
  }
};

struct MakeStridedLayoutPass
    : public mlir::PassWrapper<MakeStridedLayoutPass,
                               mlir::OperationPass<mlir::ModuleOp>> {
  void runOnOperation() override;
};

void MakeStridedLayoutPass::runOnOperation() {
  auto context = &getContext();
  auto mod = getOperation();

  llvm::SmallVector<mlir::Type> newArgTypes;
  llvm::SmallVector<mlir::Type> newResTypes;
  llvm::SmallVector<mlir::Value> newOperands;
  for (auto func : mod.getOps<mlir::FuncOp>()) {
    mlir::OpBuilder builder(func.body());
    auto loc = builder.getUnknownLoc();
    auto funcType = func.getType();
    auto argTypes = funcType.getInputs();
    auto resTypes = funcType.getResults();
    newArgTypes.assign(argTypes.begin(), argTypes.end());
    newResTypes.assign(resTypes.begin(), resTypes.end());
    bool hasBody = !func.getBody().empty();
    for (auto it : llvm::enumerate(argTypes)) {
      auto i = static_cast<unsigned>(it.index());
      auto type = it.value();
      auto memrefType = type.dyn_cast<mlir::MemRefType>();
      if (!memrefType || !memrefType.getLayout().isIdentity())
        continue;

      auto rank = static_cast<unsigned>(memrefType.getRank());
      auto makeShape = [&](int64_t val) {
        return llvm::SmallVector<int64_t>(rank, val);
      };
      auto strideVal = mlir::ShapedType::kDynamicStrideOrOffset;
      auto affineMap = mlir::makeStridedLinearLayoutMap(makeShape(strideVal),
                                                        strideVal, context);
      auto newMemrefType =
          mlir::MemRefType::get(makeShape(mlir::ShapedType::kDynamicSize),
                                memrefType.getElementType(), affineMap);

      if (newMemrefType != memrefType) {
        newArgTypes[i] = newMemrefType;

        if (hasBody) {
          auto arg = func.getBody().front().getArgument(i);
          arg.setType(newMemrefType);
          auto dst =
              builder.create<plier::ChangeLayoutOp>(loc, memrefType, arg);
          arg.replaceAllUsesExcept(dst, dst);
        }
      }
    }

    for (auto it : llvm::enumerate(resTypes)) {
      auto type = it.value();
      auto memrefType = type.dyn_cast<mlir::MemRefType>();
      if (!memrefType || !memrefType.getLayout().isIdentity())
        continue;

      auto rank = static_cast<unsigned>(memrefType.getRank());
      auto makeShape = [&](int64_t val) {
        return llvm::SmallVector<int64_t>(rank, val);
      };
      auto strideVal = mlir::ShapedType::kDynamicStrideOrOffset;
      auto affineMap = mlir::makeStridedLinearLayoutMap(
          makeShape(strideVal), strideVal, builder.getContext());
      auto newmemrefType =
          mlir::MemRefType::get(makeShape(mlir::ShapedType::kDynamicSize),
                                memrefType.getElementType(), affineMap);
      newResTypes[it.index()] = newmemrefType;
    }

    auto newFuncType =
        mlir::FunctionType::get(&getContext(), newArgTypes, newResTypes);
    if (newFuncType != funcType) {
      func.setType(newFuncType);
      func.walk([&](mlir::ReturnOp ret) {
        builder.setInsertionPoint(ret);
        auto count = static_cast<unsigned>(newResTypes.size());
        for (auto i : llvm::seq(0u, count)) {
          auto arg = ret.getOperand(i);
          auto newType = newResTypes[i];
          if (arg.getType() != newType) {
            assert(arg.getType().isa<mlir::MemRefType>());
            assert(newType.isa<mlir::MemRefType>());
            auto newArg = builder.createOrFold<mlir::memref::CastOp>(
                ret.getLoc(), arg, newType);
            ret.setOperand(i, newArg);
          }
        }
      });
      auto funcUses = mlir::SymbolTable::getSymbolUses(func, mod);
      if (funcUses) {
        for (auto use : *funcUses) {
          auto call = mlir::cast<mlir::CallOp>(use.getUser());
          auto loc = call.getLoc();

          builder.setInsertionPoint(call);
          assert(newArgTypes.size() == call.operands().size());
          auto argsCount = static_cast<unsigned>(newArgTypes.size());
          newOperands.resize(argsCount);
          for (auto i : llvm::seq(0u, argsCount)) {
            auto arg = call.operands()[i];
            auto oldType = arg.getType();
            auto newType = newArgTypes[i];
            if (oldType != newType) {
              assert(oldType.isa<mlir::MemRefType>());
              assert(newType.isa<mlir::MemRefType>());
              auto newArg =
                  builder.create<plier::ChangeLayoutOp>(loc, newType, arg)
                      .getResult();
              newOperands[i] = newArg;
            } else {
              newOperands[i] = arg;
            }
          }
          call.operandsMutable().assign(newOperands);

          builder.setInsertionPointAfter(call);
          assert(newResTypes.size() == call.getNumResults());
          auto numResults = call.getNumResults();
          for (auto i : llvm::seq(0u, numResults)) {
            auto res = call.getResult(i);
            auto oldType = res.getType();
            auto newType = newResTypes[i];
            if (oldType != newType) {
              assert(oldType.isa<mlir::MemRefType>());
              assert(newType.isa<mlir::MemRefType>());
              res.setType(newType);
              auto newRes =
                  builder.create<plier::ChangeLayoutOp>(loc, oldType, res);
              res.replaceAllUsesExcept(newRes, newRes);
            }
          }
        }
      }
    }
  }
}

struct FinalizeStridedLayoutPass
    : public mlir::PassWrapper<FinalizeStridedLayoutPass,
                               mlir::OperationPass<>> {
  void runOnOperation() override;
};

void FinalizeStridedLayoutPass::runOnOperation() {
  auto *context = &getContext();
  auto op = getOperation();
  mlir::OwningRewritePatternList patterns(context);

  patterns.insert<ReshapeChangeLayout, CleanupLoads>(context);

  (void)mlir::applyPatternsAndFoldGreedily(op, std::move(patterns));

  op->walk([&](plier::ChangeLayoutOp cl) {
    cl.emitError("Layout change failed");
    signalPassFailure();
  });
}

struct PlierToLinalgPass
    : public mlir::PassWrapper<PlierToLinalgPass,
                               mlir::OperationPass<mlir::ModuleOp>> {
  virtual void
  getDependentDialects(mlir::DialectRegistry &registry) const override {
    registry.insert<mlir::StandardOpsDialect>();
    registry.insert<mlir::bufferization::BufferizationDialect>();
    registry.insert<mlir::linalg::LinalgDialect>();
    registry.insert<mlir::memref::MemRefDialect>();
    registry.insert<mlir::tensor::TensorDialect>();
    registry.insert<plier::PlierDialect>();
    registry.insert<plier::PlierUtilDialect>();
  }

  void runOnOperation() override;
};

template <typename T> static bool hasCompatibleShape(T &&a1, T &&a2) {
  if (a1.getRank() != a2.getRank())
    return false;

  for (auto it : llvm::zip(a1.getShape(), a2.getShape())) {
    auto s1 = std::get<0>(it);
    auto s2 = std::get<1>(it);
    if (s1 != mlir::ShapedType::kDynamicSize &&
        s2 != mlir::ShapedType::kDynamicSize && s1 != s2)
      return false;
  }
  return true;
}

struct LowerTensorCasts : public mlir::OpConversionPattern<plier::CastOp> {
  using OpConversionPattern::OpConversionPattern;

  mlir::LogicalResult
  matchAndRewrite(plier::CastOp op, plier::CastOp::Adaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    auto converter = *getTypeConverter();

    auto value = adaptor.value();
    auto srcType = value.getType();
    auto dstType = converter.convertType(op.getType());
    if (!dstType)
      return mlir::failure();

    if (srcType == dstType) {
      rewriter.replaceOpWithNewOp<mlir::UnrealizedConversionCastOp>(op, dstType,
                                                                    value);
      return mlir::success();
    }

    if (srcType.isa<mlir::RankedTensorType>() &&
        dstType.isa<mlir::RankedTensorType>()) {
      auto src = srcType.cast<mlir::RankedTensorType>();
      auto dst = dstType.cast<mlir::RankedTensorType>();
      auto srcElem = src.getElementType();
      auto dstElem = dst.getElementType();
      if (!hasCompatibleShape(src, dst))
        return mlir::failure();

      auto signlessSrcType = mlir::RankedTensorType::get(
          src.getShape(), plier::makeSignlessType(srcElem), src.getEncoding());
      auto signlessDstType = mlir::RankedTensorType::get(
          dst.getShape(), plier::makeSignlessType(dstElem), dst.getEncoding());
      auto loc = op.getLoc();
      if (signlessSrcType != src)
        value = rewriter.createOrFold<plier::SignCastOp>(loc, signlessSrcType,
                                                         value);

      value = rewriter.createOrFold<mlir::tensor::CastOp>(loc, signlessDstType,
                                                          value);
      if (signlessDstType != dst)
        value = rewriter.createOrFold<plier::SignCastOp>(loc, dst, value);

      rewriter.replaceOp(op, value);
      return mlir::success();
    }
    return mlir::failure();
  }
};

struct SimplifyExpandDims
    : public mlir::OpRewritePattern<mlir::linalg::GenericOp> {
  using mlir::OpRewritePattern<mlir::linalg::GenericOp>::OpRewritePattern;

  mlir::LogicalResult
  matchAndRewrite(mlir::linalg::GenericOp op,
                  mlir::PatternRewriter &rewriter) const override {
    if (!op.hasTensorSemantics())
      return mlir::failure();

    if (op.getNumInputs() != 1 || op.getNumOutputs() != 1)
      return mlir::failure();

    auto context = op.getContext();
    auto parallelAttr = mlir::StringAttr::get(context, "parallel");
    if (llvm::any_of(op.iterator_types(),
                     [&](auto attr) { return attr != parallelAttr; }))
      return mlir::failure();

    auto maps = op.indexing_maps();
    assert(maps.size() == 2);
    auto outMap = maps[1].cast<mlir::AffineMapAttr>().getValue();
    if (!outMap.isIdentity())
      return mlir::failure();

    auto inMap = maps[0].cast<mlir::AffineMapAttr>().getValue();
    auto numDims = op.getNumLoops();
    if (inMap.getNumResults() != numDims)
      return mlir::failure();

    bool changed = false;
    auto outShape = op.getOutputOperand(0)
                        ->get()
                        .getType()
                        .cast<mlir::RankedTensorType>()
                        .getShape();
    llvm::SmallVector<mlir::AffineExpr> exprs(numDims);
    for (unsigned i = 0; i < numDims; ++i) {
      auto prevExpr = inMap.getResult(i);
      bool canConvert = [&]() {
        if (outShape[i] == 1) {
          auto constExpr = prevExpr.dyn_cast<mlir::AffineConstantExpr>();
          if (constExpr && constExpr.getValue() == 0)
            return true;
        }
        return false;
      }();
      if (canConvert) {
        changed = true;
        exprs[i] = mlir::getAffineDimExpr(i, context);
      } else {
        exprs[i] = prevExpr;
      }
    }

    if (changed) {
      const mlir::Attribute newMaps[] = {
          mlir::AffineMapAttr::get(
              mlir::AffineMap::get(numDims, 0, exprs, context)),
          maps[1]};
      auto newMapsAttr = mlir::ArrayAttr::get(context, newMaps);
      rewriter.updateRootInPlace(op,
                                 [&]() { op.indexing_mapsAttr(newMapsAttr); });
    }

    return mlir::success(changed);
  }
};

struct LowerEnforceShape
    : public mlir::OpRewritePattern<plier::EnforceShapeOp> {
  using mlir::OpRewritePattern<plier::EnforceShapeOp>::OpRewritePattern;

  mlir::LogicalResult
  matchAndRewrite(plier::EnforceShapeOp op,
                  mlir::PatternRewriter &rewriter) const override {
    auto type = op.getType();
    auto src = op.value();
    rewriter.replaceOpWithNewOp<mlir::tensor::CastOp>(op, type, src);
    return mlir::success();
  }
};

static bool isTensor(mlir::TypeConverter &converter, mlir::Type type) {
  return !!converter.convertType(type).dyn_cast_or_null<mlir::TensorType>();
}

void PlierToLinalgPass::runOnOperation() {
  auto &context = getContext();

  mlir::TypeConverter typeConverter;
  // Convert unknown types to itself
  typeConverter.addConversion([](mlir::Type type) { return type; });
  populateStdTypeConverter(context, typeConverter);
  populateTupleTypeConverter(context, typeConverter);
  populateArrayTypeConverter(context, typeConverter);

  auto materializeCast = [](mlir::OpBuilder &builder, mlir::Type type,
                            mlir::ValueRange inputs,
                            mlir::Location loc) -> llvm::Optional<mlir::Value> {
    if (inputs.size() == 1)
      return builder
          .create<mlir::UnrealizedConversionCastOp>(loc, type, inputs.front())
          .getResult(0);

    return llvm::None;
  };
  typeConverter.addArgumentMaterialization(materializeCast);
  typeConverter.addSourceMaterialization(materializeCast);
  typeConverter.addTargetMaterialization(materializeCast);

  mlir::RewritePatternSet patterns(&context);
  mlir::ConversionTarget target(context);

  plier::populateTupleTypeConversionRewritesAndTarget(typeConverter, patterns,
                                                      target);

  target.addDynamicallyLegalOp<plier::GetItemOp>(
      [&typeConverter](plier::GetItemOp op) -> llvm::Optional<bool> {
        auto containerType = op.value().getType();
        if (isTensor(typeConverter, containerType))
          return false;

        if (isUniTuple(containerType) && !mlir::getConstantIntValue(op.index()))
          return false;

        return llvm::None;
      });

  target.addDynamicallyLegalOp<plier::SetItemOp>(
      [&typeConverter](plier::SetItemOp op) -> bool {
        return !isTensor(typeConverter, op.target().getType());
      });

  target.addDynamicallyLegalOp<plier::CastOp>([&](plier::CastOp op) -> bool {
    auto inputType = op.value().getType();
    auto srcType = typeConverter.convertType(inputType);
    auto dstType = typeConverter.convertType(op.getType());
    if (!srcType || !dstType)
      return true;

    auto isZeroRankTensor = [](mlir::Type t) -> bool {
      auto tensor = t.dyn_cast<mlir::RankedTensorType>();
      return tensor && tensor.getRank() == 0;
    };

    if ((isZeroRankTensor(srcType) && dstType.isIntOrIndexOrFloat()) ||
        (isZeroRankTensor(dstType) && srcType.isIntOrIndexOrFloat()))
      return false;

    if (srcType == dstType && inputType != op.getType())
      return false;

    return srcType == dstType || !(srcType.isa<mlir::ShapedType>() &&
                                   dstType.isa<mlir::ShapedType>());
  });

  plier::populateControlFlowTypeConversionRewritesAndTarget(typeConverter,
                                                            patterns, target);

  patterns.insert<
      // clang-format off
      GetitemOpLowering,
      GetitemOpArrLowering,
      SetitemOpLowering,
      LowerTensorCasts,
      UnrankedToElementCasts,
      GetItemUniTupleConversionPattern
      // clang-format on
      >(typeConverter, &context);

  if (mlir::failed(mlir::applyPartialConversion(getOperation(), target,
                                                std::move(patterns))))
    signalPassFailure();
}

struct LowerLinalgPass
    : public mlir::PassWrapper<LowerLinalgPass,
                               mlir::OperationPass<mlir::ModuleOp>> {
  virtual void
  getDependentDialects(mlir::DialectRegistry &registry) const override {
    registry.insert<mlir::AffineDialect>();
    registry.insert<mlir::StandardOpsDialect>();
    registry.insert<mlir::linalg::LinalgDialect>();
    registry.insert<mlir::scf::SCFDialect>();
    registry.insert<mlir::tensor::TensorDialect>();
  }

  void runOnOperation() override;
};

void LowerLinalgPass::runOnOperation() {
  mlir::OwningRewritePatternList patterns(&getContext());

  patterns.insert<mlir::linalg::LinalgLoweringPattern<mlir::linalg::GenericOp>,
                  mlir::linalg::LinalgLoweringPattern<mlir::linalg::CopyOp>,
                  mlir::linalg::LinalgLoweringPattern<mlir::linalg::FillOp>>(
      &getContext(), mlir::linalg::LinalgLoweringType::ParallelLoops);

  (void)mlir::applyPatternsAndFoldGreedily(getOperation(), std::move(patterns));
}

struct OptimizeGlobalsConstsLoad
    : public mlir::OpRewritePattern<mlir::memref::LoadOp> {
  using mlir::OpRewritePattern<mlir::memref::LoadOp>::OpRewritePattern;

  mlir::LogicalResult
  matchAndRewrite(mlir::memref::LoadOp op,
                  mlir::PatternRewriter &rewriter) const override {
    // We access data outside function, but doesnt change it, lets hope it
    // is safe.
    auto mod = op->getParentOfType<mlir::ModuleOp>();
    if (!mod) {
      return mlir::failure();
    }
    mlir::SymbolTable symbolTable(mod);

    llvm::SmallVector<uint64_t> indices(op.indices().size());
    for (auto it : llvm::enumerate(op.indices())) {
      auto constIndex =
          it.value().getDefiningOp<mlir::arith::ConstantIndexOp>();
      if (!constIndex)
        return mlir::failure();

      auto val = constIndex.value();
      if (val < 0)
        return mlir::failure();

      indices[it.index()] = static_cast<uint64_t>(val);
    }
    auto getGlobal = op.memref().getDefiningOp<mlir::memref::GetGlobalOp>();
    if (!getGlobal)
      return mlir::failure();

    auto sym = symbolTable.lookup<mlir::memref::GlobalOp>(getGlobal.name());
    if (!sym)
      return mlir::failure();

    if (!sym.constant())
      return mlir::failure();

    auto initAttr = sym.initial_value();
    if (!initAttr)
      return mlir::failure();

    auto elements = initAttr->dyn_cast<mlir::ElementsAttr>();
    if (!elements)
      return mlir::failure();

    if (elements.getType().getElementType() != op.getType() ||
        !elements.isValidIndex(indices))
      return mlir::failure();

    auto vals = elements.tryGetValues<mlir::Attribute>();
    if (!vals)
      return mlir::failure();

    rewriter.replaceOpWithNewOp<mlir::arith::ConstantOp>(op, (*vals)[indices]);
    return mlir::success();
  }
};

struct PostPlierToLinalgPass
    : public mlir::PassWrapper<PostPlierToLinalgPass, mlir::FunctionPass> {
  void runOnFunction() override;
};

void PostPlierToLinalgPass::runOnFunction() {
  auto &context = getContext();
  mlir::OwningRewritePatternList patterns(&context);

  plier::populateCommonOptsPatterns(context, patterns);

  patterns.insert<SimplifyExpandDims>(&context);

  (void)mlir::applyPatternsAndFoldGreedily(getFunction(), std::move(patterns));
}

struct MakeTensorsSignlessPass
    : public mlir::PassWrapper<MakeTensorsSignlessPass,
                               mlir::OperationPass<mlir::ModuleOp>> {
  void runOnOperation() override;
};

void MakeTensorsSignlessPass::runOnOperation() {
  auto module = getOperation();
  auto *context = &getContext();

  mlir::TypeConverter typeConverter;
  typeConverter.addConversion([](mlir::Type type) { return type; });
  typeConverter.addConversion(
      [](mlir::RankedTensorType type) -> llvm::Optional<mlir::Type> {
        auto elemType = type.getElementType().dyn_cast<mlir::IntegerType>();
        if (elemType && !elemType.isSignless()) {
          auto signless =
              mlir::IntegerType::get(type.getContext(), elemType.getWidth());
          return mlir::RankedTensorType::get(type.getShape(), signless,
                                             type.getEncoding());
        }
        return llvm::None;
      });
  populateTupleTypeConverter(*context, typeConverter);

  auto materializeSignCast = [](mlir::OpBuilder &builder, mlir::Type type,
                                mlir::ValueRange inputs,
                                mlir::Location loc) -> mlir::Value {
    assert(inputs.size() == 1);
    return builder.create<plier::SignCastOp>(loc, type, inputs[0]);
  };
  typeConverter.addArgumentMaterialization(materializeSignCast);
  typeConverter.addSourceMaterialization(materializeSignCast);
  typeConverter.addTargetMaterialization(materializeSignCast);

  mlir::RewritePatternSet patterns(context);
  mlir::ConversionTarget target(*context);

  plier::populateControlFlowTypeConversionRewritesAndTarget(typeConverter,
                                                            patterns, target);
  plier::populateTupleTypeConversionRewritesAndTarget(typeConverter, patterns,
                                                      target);

  target.addLegalOp<mlir::ModuleOp, plier::SignCastOp>();

  if (failed(applyFullConversion(module, target, std::move(patterns))))
    signalPassFailure();
}

struct TensorFusionPass
    : public mlir::PassWrapper<TensorFusionPass,
                               mlir::OperationPass<mlir::ModuleOp>> {
  void runOnOperation() override;
};

void TensorFusionPass::runOnOperation() {
  auto &context = getContext();
  mlir::OwningRewritePatternList patterns(&context);

  plier::populateCommonOptsPatterns(context, patterns);

  patterns.insert<SimplifyExpandDims, LowerEnforceShape>(&context);

  mlir::linalg::populateElementwiseOpsFusionPatterns(patterns);

  (void)mlir::applyPatternsAndFoldGreedily(getOperation(), std::move(patterns));
}

struct LoopInvariantCodeMotion
    : public mlir::OpRewritePattern<mlir::scf::ForOp> {
  using mlir::OpRewritePattern<mlir::scf::ForOp>::OpRewritePattern;

  mlir::LogicalResult
  matchAndRewrite(mlir::scf::ForOp op,
                  mlir::PatternRewriter &rewriter) const override {
    auto parentOp = op->getParentOp();
    rewriter.startRootUpdate(parentOp);
    auto res = mlir::moveLoopInvariantCode(op);
    if (mlir::succeeded(res)) {
      rewriter.finalizeRootUpdate(parentOp);
    } else {
      rewriter.cancelRootUpdate(parentOp);
    }
    return res;
  }
};

struct BufferizeReshape
    : public mlir::OpConversionPattern<mlir::tensor::ReshapeOp> {
  using OpConversionPattern::OpConversionPattern;

  mlir::LogicalResult
  matchAndRewrite(mlir::tensor::ReshapeOp op,
                  mlir::tensor::ReshapeOp::Adaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    auto getType = [&](mlir::Type type) {
      auto shapedType = type.cast<mlir::ShapedType>();
      return mlir::MemRefType::get(shapedType.getShape(),
                                   shapedType.getElementType());
    };
    auto source = adaptor.source();
    auto shape = adaptor.shape();
    auto resType = getType(op.getType());
    rewriter.replaceOpWithNewOp<mlir::memref::ReshapeOp>(op, resType, source,
                                                         shape);
    return mlir::success();
  }
};

struct BufferizeExtractSlice
    : public mlir::OpConversionPattern<mlir::tensor::ExtractSliceOp> {
  using OpConversionPattern::OpConversionPattern;

  mlir::LogicalResult
  matchAndRewrite(mlir::tensor::ExtractSliceOp op,
                  mlir::tensor::ExtractSliceOp::Adaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    auto converter = getTypeConverter();
    assert(converter);
    auto dstType = converter->convertType(op.getType())
                       .dyn_cast_or_null<mlir::MemRefType>();
    if (!dstType)
      return mlir::failure();

    auto src = adaptor.source();
    auto srcType = src.getType().cast<mlir::MemRefType>();

    auto dstRank = dstType.getRank();
    auto offsets =
        mlir::getMixedOffsets(op, adaptor.static_offsets(), adaptor.offsets());
    auto sizes =
        mlir::getMixedSizes(op, adaptor.static_sizes(), adaptor.sizes());
    auto strides =
        mlir::getMixedStrides(op, adaptor.static_strides(), adaptor.strides());

    auto viewType = [&]() {
      if (srcType.getRank() == dstRank)
        return mlir::memref::SubViewOp::inferResultType(srcType, offsets, sizes,
                                                        strides)
            .cast<mlir::MemRefType>();

      return mlir::memref::SubViewOp::inferRankReducedResultType(
                 dstRank, srcType, offsets, sizes, strides)
          .cast<mlir::MemRefType>();
    }();
    auto loc = op->getLoc();
    mlir::Value view = rewriter.create<mlir::memref::SubViewOp>(
        loc, viewType, src, offsets, sizes, strides);

    if (viewType != dstType)
      view = rewriter.create<plier::ChangeLayoutOp>(loc, dstType, view);

    rewriter.replaceOp(op, view);
    return mlir::success();
  }
};

struct BufferizeForceCopy
    : public mlir::OpConversionPattern<plier::ForceCopyOp> {
  using OpConversionPattern::OpConversionPattern;

  mlir::LogicalResult
  matchAndRewrite(plier::ForceCopyOp op, plier::ForceCopyOp::Adaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    auto converter = getTypeConverter();
    assert(converter);
    auto dstType = converter->convertType(op.getType())
                       .dyn_cast_or_null<mlir::MemRefType>();

    if (!dstType)
      return mlir::failure();

    auto src = adaptor.source();
    auto srcType = src.getType().cast<mlir::MemRefType>();
    auto rank = static_cast<unsigned>(srcType.getRank());

    auto loc = op->getLoc();
    llvm::SmallVector<mlir::Value> sizes(rank);
    for (auto i : llvm::seq(0u, rank))
      sizes[i] = rewriter.create<mlir::memref::DimOp>(loc, src, i);

    auto dst = rewriter.create<mlir::memref::AllocOp>(loc, dstType, sizes);
    rewriter.create<mlir::memref::CopyOp>(loc, src, dst);
    rewriter.replaceOp(op, dst.getResult());
    return mlir::success();
  }
};

struct BufferizePseudoCopy
    : public mlir::OpConversionPattern<plier::PseudoCopyOp> {
  using OpConversionPattern::OpConversionPattern;

  mlir::LogicalResult
  matchAndRewrite(plier::PseudoCopyOp op, plier::PseudoCopyOp::Adaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    auto converter = getTypeConverter();
    assert(converter);
    auto dstType = converter->convertType(op.getType())
                       .dyn_cast_or_null<mlir::MemRefType>();

    if (!dstType)
      return mlir::failure();

    auto arg = adaptor.source();
    rewriter.replaceOpWithNewOp<plier::PseudoCopyOp>(op, dstType, arg);
    return mlir::success();
  }
};

struct BufferizeReduceRank
    : public mlir::OpConversionPattern<plier::ReduceRankOp> {
  using OpConversionPattern::OpConversionPattern;

  mlir::LogicalResult
  matchAndRewrite(plier::ReduceRankOp op, plier::ReduceRankOp::Adaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    if (!op.getType().isa<mlir::RankedTensorType>())
      return mlir::failure();

    rewriter.replaceOpWithNewOp<plier::ReduceRankOp>(op, adaptor.source(),
                                                     op.getMapping());
    return mlir::success();
  }
};

struct FixDeallocPlacement
    : public mlir::OpRewritePattern<mlir::memref::DeallocOp> {
  using mlir::OpRewritePattern<mlir::memref::DeallocOp>::OpRewritePattern;

  mlir::LogicalResult
  matchAndRewrite(mlir::memref::DeallocOp op,
                  mlir::PatternRewriter &rewriter) const override {
    auto block = op->getBlock();
    auto blockIt = mlir::Block::iterator(op);
    mlir::Operation *newPos = op;
    ++blockIt;
    auto memref = op.memref();
    mlir::BufferViewFlowAnalysis analysis(op->getParentOfType<mlir::FuncOp>());
    auto aliases = analysis.resolve(memref);
    auto blockEnd = block->without_terminator().end();
    for (auto &it : llvm::make_range(blockIt, blockEnd)) {
      auto visitor = [&](mlir::Operation *inner) {
        for (auto arg : inner->getOperands()) {
          if (aliases.count(arg)) {
            return mlir::WalkResult::interrupt();
          }
        }
        return mlir::WalkResult::advance();
      };
      if (it.walk(visitor).wasInterrupted()) {
        newPos = &it;
      }
    }

    if (newPos != op) {
      rewriter.setInsertionPointAfter(newPos);
      rewriter.create<mlir::memref::DeallocOp>(op.getLoc(), memref);
      rewriter.eraseOp(op);
      return mlir::success();
    }
    return mlir::failure();
  }
};

struct AdditionalBufferize
    : public mlir::PassWrapper<AdditionalBufferize, mlir::FunctionPass> {
  virtual void
  getDependentDialects(mlir::DialectRegistry &registry) const override {
    registry.insert<plier::PlierUtilDialect>();
  }

  void runOnFunction() override;
};

void AdditionalBufferize::runOnFunction() {
  auto module = getOperation();
  auto *context = &getContext();

  mlir::bufferization::BufferizeTypeConverter typeConverter;
  populateTupleTypeConverter(*context, typeConverter);

  auto materializeTupleCast =
      [](mlir::OpBuilder &builder, mlir::Type type, mlir::ValueRange inputs,
         mlir::Location loc) -> llvm::Optional<mlir::Value> {
    if (inputs.size() != 1)
      return llvm::None;

    auto input = inputs.front();
    if (input.getType().isa<mlir::TupleType>() || type.isa<mlir::TupleType>())
      return builder.createOrFold<plier::CastOp>(loc, type, input);

    return llvm::None;
  };
  typeConverter.addArgumentMaterialization(materializeTupleCast);
  typeConverter.addSourceMaterialization(materializeTupleCast);
  typeConverter.addTargetMaterialization(materializeTupleCast);

  mlir::RewritePatternSet patterns(context);
  mlir::ConversionTarget target(*context);

  plier::populateControlFlowTypeConversionRewritesAndTarget(typeConverter,
                                                            patterns, target);
  plier::populateTupleTypeConversionRewritesAndTarget(typeConverter, patterns,
                                                      target);
  target.addIllegalOp<mlir::tensor::ReshapeOp, mlir::tensor::ExtractSliceOp>();
  target.addIllegalOp<plier::ForceCopyOp>();
  target.addLegalOp<mlir::memref::ReshapeOp>();
  target.addDynamicallyLegalOp<plier::ReduceRankOp, plier::PseudoCopyOp>(
      [](mlir::Operation *op) {
        return !op->getResultTypes().front().isa<mlir::RankedTensorType>();
      });

  patterns.insert<BufferizeReshape, BufferizeExtractSlice, BufferizeForceCopy,
                  BufferizePseudoCopy, BufferizeReduceRank>(typeConverter,
                                                            context);

  if (failed(applyPartialConversion(module, target, std::move(patterns))))
    signalPassFailure();
}

struct RemovePseudoCopy : public mlir::OpRewritePattern<plier::PseudoCopyOp> {
  using OpRewritePattern::OpRewritePattern;

  mlir::LogicalResult
  matchAndRewrite(plier::PseudoCopyOp op,
                  mlir::PatternRewriter &rewriter) const override {
    auto arg = op.source();
    if (arg.getType() != op.getType())
      return mlir::failure();

    rewriter.replaceOp(op, arg);
    return mlir::success();
  }
};

struct RemovePseudoCopyPass
    : public plier::RewriteWrapperPass<RemovePseudoCopyPass, mlir::FuncOp, void,
                                       RemovePseudoCopy> {};

struct CloneArgsPass
    : public mlir::PassWrapper<CloneArgsPass, mlir::FunctionPass> {
  virtual void
  getDependentDialects(mlir::DialectRegistry &registry) const override {
    registry.insert<plier::PlierUtilDialect>();
  }

  void runOnFunction() override;
};

void CloneArgsPass::runOnFunction() {
  auto func = getFunction();
  if (func.isPrivate() || func.isDeclaration() || func.body().empty()) {
    return;
  }

  mlir::OpBuilder builder(&getContext());
  //  auto loc = builder.getUnknownLoc();
  //  auto block = &func.body().front();
  //  builder.setInsertionPointToStart(block);
  //  for (auto arg : block->getArguments()) {
  //    if (auto type = arg.getType().dyn_cast<mlir::MemRefType>()) {
  //      auto retained = builder.create<mlir::memref::CloneOp>(loc, type, arg);
  //      arg.replaceAllUsesExcept(retained, retained);
  //    }
  //  }

  for (auto &block : func.getBody()) {
    auto ret = mlir::dyn_cast_or_null<mlir::ReturnOp>(block.getTerminator());
    if (!ret)
      continue;

    auto loc = ret.getLoc();
    bool needReplace = false;
    llvm::SmallVector<mlir::Value> newArgs(ret.operands().size());
    builder.setInsertionPoint(ret);
    for (auto it : llvm::enumerate(ret.operands())) {
      auto i = it.index();
      auto arg = it.value();
      if (arg.getType().isa<mlir::MemRefType>()) {
        newArgs[i] = builder.create<mlir::bufferization::CloneOp>(loc, arg);
        needReplace = true;
      } else {
        newArgs[i] = arg;
      }
    }

    if (needReplace) {
      builder.create<mlir::ReturnOp>(loc, newArgs);
      ret.erase();
    }
  }
}

struct ReplaceClones
    : public mlir::OpRewritePattern<mlir::bufferization::CloneOp> {
  using mlir::OpRewritePattern<mlir::bufferization::CloneOp>::OpRewritePattern;

  mlir::LogicalResult
  matchAndRewrite(mlir::bufferization::CloneOp op,
                  mlir::PatternRewriter &rewriter) const override {
    rewriter.replaceOpWithNewOp<plier::RetainOp>(op, op.getType(),
                                                 op.getSource());
    return mlir::success();
  }
};

struct LowerCloneOpsPass
    : public plier::RewriteWrapperPass<LowerCloneOpsPass, mlir::FuncOp, void,
                                       ReplaceClones> {};

struct UpliftMathCallsPass
    : public plier::RewriteWrapperPass<UpliftMathCallsPass, void, void,
                                       plier::UpliftMathCalls> {};

struct PostLinalgOptPass
    : public mlir::PassWrapper<PostLinalgOptPass, mlir::FunctionPass> {
  void runOnFunction() override;
};

void PostLinalgOptPass::runOnFunction() {
  auto func = getFunction();
  auto optLevel = getOptLevel(func);
  if (0 == optLevel)
    return;

  auto &context = getContext();
  mlir::OwningRewritePatternList patterns(&context);

  plier::populateCommonOptsPatterns(context, patterns);

  patterns.insert<OptimizeGlobalsConstsLoad, plier::CanonicalizeReduction,
                  plier::PromoteToParallel, plier::MergeNestedForIntoParallel>(
      &context);

  auto additionalOpt = [](mlir::FuncOp op) {
    (void)plier::prepareForFusion(op.getRegion());
    return plier::naivelyFuseParallelOps(op.getRegion());
  };
  if (mlir::failed(applyOptimizations(func, std::move(patterns),
                                      getAnalysisManager(), additionalOpt))) {
    signalPassFailure();
  }
}

struct FixDeallocPlacementPass
    : public plier::RewriteWrapperPass<FixDeallocPlacementPass, mlir::FuncOp,
                                       void, FixDeallocPlacement> {};

static void populatePlierToLinalgGenPipeline(mlir::OpPassManager &pm) {
  pm.addPass(std::make_unique<PlierToLinalgPass>());
  pm.addPass(mlir::createCanonicalizerPass());
  pm.addPass(std::make_unique<NumpyCallsLoweringPass>());
  pm.addPass(plier::createForceInlinePass());
  pm.addPass(mlir::createSymbolDCEPass());
  pm.addNestedPass<mlir::FuncOp>(std::make_unique<PostPlierToLinalgPass>());
  pm.addNestedPass<mlir::FuncOp>(mlir::createCSEPass());
}

static void populatePlierToLinalgOptPipeline(mlir::OpPassManager &pm) {
  pm.addPass(std::make_unique<MakeTensorsSignlessPass>());

  pm.addPass(mlir::createCanonicalizerPass());
  pm.addPass(mlir::createReconcileUnrealizedCastsPass());

  pm.addPass(mlir::createCanonicalizerPass());
  pm.addPass(std::make_unique<TensorFusionPass>());

  pm.addPass(mlir::createTensorConstantBufferizePass());
  pm.addNestedPass<mlir::FuncOp>(std::make_unique<AdditionalBufferize>());
  pm.addNestedPass<mlir::FuncOp>(mlir::createSCFBufferizePass());
  pm.addNestedPass<mlir::FuncOp>(mlir::createLinalgBufferizePass());
  pm.addNestedPass<mlir::FuncOp>(mlir::createStdBufferizePass());
  pm.addNestedPass<mlir::FuncOp>(mlir::createTensorBufferizePass());
  pm.addPass(mlir::createFuncBufferizePass());
  pm.addNestedPass<mlir::FuncOp>(
      mlir::bufferization::createFinalizingBufferizePass());

  pm.addNestedPass<mlir::FuncOp>(std::make_unique<RemovePseudoCopyPass>());
  pm.addPass(mlir::createCanonicalizerPass());

  pm.addNestedPass<mlir::FuncOp>(mlir::createBufferHoistingPass());
  pm.addNestedPass<mlir::FuncOp>(mlir::createBufferLoopHoistingPass());
  pm.addNestedPass<mlir::FuncOp>(mlir::createPromoteBuffersToStackPass());

  pm.addNestedPass<mlir::FuncOp>(std::make_unique<CloneArgsPass>());
  pm.addPass(std::make_unique<MakeStridedLayoutPass>());
  pm.addNestedPass<mlir::FuncOp>(mlir::createCanonicalizerPass());
  pm.addNestedPass<mlir::FuncOp>(std::make_unique<FinalizeStridedLayoutPass>());
  pm.addNestedPass<mlir::FuncOp>(
      mlir::bufferization::createBufferDeallocationPass());
  pm.addPass(mlir::createCanonicalizerPass());

  pm.addNestedPass<mlir::FuncOp>(std::make_unique<LowerCloneOpsPass>());

  pm.addPass(std::make_unique<LowerLinalgPass>());
  pm.addPass(plier::createForceInlinePass());
  pm.addPass(mlir::createSymbolDCEPass());

  pm.addPass(plier::createPromoteBoolMemrefPass());
  pm.addNestedPass<mlir::FuncOp>(std::make_unique<UpliftMathCallsPass>());
  pm.addNestedPass<mlir::FuncOp>(mlir::createCanonicalizerPass());
  pm.addNestedPass<mlir::FuncOp>(mlir::createLoopInvariantCodeMotionPass());

  // ToDo: This pass also tries to do some simple fusion, whic should be split
  // in separate pass
  pm.addNestedPass<mlir::FuncOp>(std::make_unique<PostLinalgOptPass>());

  pm.addNestedPass<mlir::FuncOp>(std::make_unique<FixDeallocPlacementPass>());

  pm.addPass(mlir::createSymbolDCEPass());
}
} // namespace

void populateArrayTypeConverter(mlir::MLIRContext & /*context*/,
                                mlir::TypeConverter &converter) {
  converter.addConversion(
      [&](plier::PyType type) -> llvm::Optional<mlir::Type> {
        auto ret = map_plier_type(converter, type);
        if (!ret)
          return llvm::None;

        return ret;
      });
}

// ToDo: how does this sink stuff actually works?
void registerPlierToLinalgPipeline(plier::PipelineRegistry &registry) {
  registry.registerPipeline([](auto sink) {
    auto stage = getHighLoweringStage();
    sink(plierToLinalgGenPipelineName(), {plierToStdPipelineName()},
         {plierToLinalgOptPipelineName()}, {plierToScfPipelineName()},
         &populatePlierToLinalgGenPipeline);
    sink(plierToLinalgOptPipelineName(),
         {plierToLinalgGenPipelineName(), untuplePipelineName()},
         {removeSignPipelineName(), stage.end}, {},
         &populatePlierToLinalgOptPipeline);
  });
}

llvm::StringRef plierToLinalgGenPipelineName() { return "plier_to_linalg_gen"; }

llvm::StringRef plierToLinalgOptPipelineName() { return "plier_to_linalg_opt"; }
