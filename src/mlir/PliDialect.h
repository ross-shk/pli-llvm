// SPDX-License-Identifier: Apache-2.0
// P2 canary dialect header.

#pragma once

#include "mlir/Conversion/LLVMCommon/TypeConverter.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "mlir/Transforms/DialectConversion.h"

// Generated dialect and op declarations.
#define GET_DIALECT_DECL
#include "PliDialect.h.inc"
#define GET_OP_DECL
#include "PliOps.h.inc"

namespace mlir::plic {

void populatePliConversionPatterns(LLVMTypeConverter& converter, RewritePatternSet& patterns);

bool runMlirCanary(llvm::raw_ostream& os);

} // namespace mlir::plic
