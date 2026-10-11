// SPDX-License-Identifier: Apache-2.0
// P2 canary lowering: BufferCopyOp -> LLVM dialect memmove.
//
// Converts the synthetic pli.buffer_copy canary to LLVM dialect so the
// standard LLVM-to-LLVM-IR translation pipeline can emit llvm.memmove.

#include "PliDialect.h"

#include "mlir/Conversion/LLVMCommon/Pattern.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMTypes.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Support/LogicalResult.h"

#define GET_OP_CLASSES
#include "PliOps.h.inc"

namespace mlir::plic {

struct BufferCopyOpLowering : public ConvertOpToLLVMPattern<BufferCopyOp> {
  BufferCopyOpLowering(const LLVMTypeConverter& converter)
      : ConvertOpToLLVMPattern<BufferCopyOp>(converter) {}

  LogicalResult matchAndRewrite(BufferCopyOp op, BufferCopyOpAdaptor adaptor,
                                ConversionPatternRewriter& rewriter) const override {
    Location loc = op.getLoc();
    Value dst = adaptor.getDst();
    Value src = adaptor.getSrc();
    Value length = adaptor.getLength();
    Value isVolatile =
        LLVM::ConstantOp::create(rewriter, loc, IntegerType::get(rewriter.getContext(), 1), 0);
    LLVM::MemmoveOp::create(rewriter, loc, dst, src, length, false);
    rewriter.eraseOp(op);
    return success();
  }
};

void populatePliConversionPatterns(LLVMTypeConverter& converter, RewritePatternSet& patterns) {
  patterns.add<BufferCopyOpLowering>(converter);
}

} // namespace mlir::plic
