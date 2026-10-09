// SPDX-License-Identifier: Apache-2.0
// P2 canary dialect implementation.
//
// The canary exercises the full MLIR pipeline without lowering any W1-W5
// PL/I operation. All MLIR sources live under src/mlir/ and compile only when
// PLIC_ENABLE_MLIR=ON (design-docs/mlir-runtime-migration-plan.md §P2 step 2).

#include "PliDialect.h"

#include "mlir/Dialect/LLVMIR/LLVMTypes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Support/LogicalResult.h"

#define GET_DIALECT_IMPL
#include "PliDialect.cpp.inc"
#define GET_OP_CLASSES
#include "PliOps.h.inc"
#define GET_OP_CLASSES
#include "PliOps.cpp.inc"

namespace mlir::plic {

void PliDialect::initialize() {
  addOperations<BufferCopyOp>();
}

Attribute PliDialect::parseAttribute(DialectAsmParser &parser,
                                      Type type) const {
  return Attribute();
}

void PliDialect::printAttribute(Attribute attr,
                                 DialectAsmPrinter &os) const {}

} // namespace mlir::plic
