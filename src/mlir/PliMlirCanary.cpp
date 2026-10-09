// SPDX-License-Identifier: Apache-2.0
// P2 canary module builder and pipeline runner.
//
// Builds a synthetic MLIR module containing a pli.buffer_copy canary, runs the
// full conversion + translation pipeline, and verifies at every checkpoint
// (design-docs/mlir-runtime-migration-plan.md §P2 steps 4-6).
//
// The canary never lowers a W1-W5 PL/I operation — it is pure scaffolding to
// prove the pipeline works end to end.

#include "PliDialect.h"

#include "mlir/Conversion/LLVMCommon/TypeConverter.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMTypes.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Support/LogicalResult.h"
#include "mlir/Transforms/DialectConversion.h"

#include "llvm/Support/raw_ostream.h"

#define GET_OP_CLASSES
#include "PliOps.h.inc"

using namespace mlir;
using namespace mlir::plic;

namespace mlir::plic {

// Build a canary module: a function that copies 4096 bytes from src to dst.
static ModuleOp buildCanaryModule(MLIRContext* ctx) {
  OpBuilder builder(ctx);
  auto loc = builder.getUnknownLoc();
  (void)ctx->loadDialect<PliDialect>();
  (void)ctx->loadDialect<LLVM::LLVMDialect>();
  (void)ctx->loadDialect<func::FuncDialect>();
  (void)ctx->loadDialect<arith::ArithDialect>();

  auto mod = builder.create<ModuleOp>(loc);
  builder.setInsertionPointToStart(&mod.getBodyRegion().front());

  auto i64Ty = builder.getI64Type();
  auto i8PtrTy = LLVM::LLVMPointerType::get(ctx);
  auto funcType = FunctionType::get(ctx, {i8PtrTy, i8PtrTy}, i64Ty);
  auto func = builder.create<func::FuncOp>(loc, "canary_copy", funcType);
  func.setPrivate();

  Block* entry = func.addEntryBlock();
  builder.setInsertionPointToStart(&func.getBody().front());

  Value length = builder.create<arith::ConstantOp>(
      loc, i64Ty, IntegerAttr::get(i64Ty, 4096));

  Value dst = entry->getArgument(0);
  Value src = entry->getArgument(1);

  builder.create<BufferCopyOp>(loc, dst, src, length);
  builder.create<func::ReturnOp>(loc, length);

  return mod;
}

// Run the conversion pipeline: Pli dialect -> LLVM dialect.
static LogicalResult runConversion(ModuleOp mod) {
  MLIRContext& ctx = *mod->getContext();

  // Stage 1: verify the original canary module.
  if (failed(verify(mod.getOperation())))
    return failure();

  // Stage 2: set up the conversion target — only LLVM dialect is legal.
  LLVMTypeConverter converter(&ctx);
  RewritePatternSet patterns(&ctx);
  populatePliConversionPatterns(converter, patterns);

  ConversionTarget target(ctx);
  target.addLegalDialect<LLVM::LLVMDialect>();
  target.addLegalOp<ModuleOp, func::FuncOp, func::ReturnOp>();

  if (failed(applyPartialConversion(mod, target, std::move(patterns))))
    return failure();

  // Stage 3: re-verify after conversion.
  if (failed(verify(mod.getOperation())))
    return failure();

  return success();
}

// Run the full canary pipeline for -emit-mlir. Returns true on success.
bool runMlirCanary(llvm::raw_ostream& os) {
  MLIRContext ctx;
  // Build the canary module.
  auto mod = buildCanaryModule(&ctx);
  if (!mod) {
    os << "plic: MLIR canary: failed to build module\n";
    return false;
  }

  // Verify original module.
  if (failed(verify(mod.getOperation()))) {
    os << "plic: MLIR canary failed verification (pre-conversion)\n";
    return false;
  }

  // Run conversion pipeline.
  if (failed(runConversion(mod))) {
    os << "plic: MLIR canary conversion failed\n";
    return false;
  }

  // Print the converted MLIR module (LLVM dialect operations).
  mod.print(os);
  os << "\n";

  return true;
}

} // namespace mlir::plic
