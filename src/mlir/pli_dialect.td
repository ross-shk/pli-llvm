// SPDX-License-Identifier: Apache-2.0
// P2 canary dialect: a synthetic `pli.buffer_copy` operation that exercises
// the full MLIR pipeline (parsing, verification, pass registration, dialect
// conversion, LLVM translation) without lowering any W1-W5 PL/I operation.
// It does not change PL/I output in any mode.
//
// design-docs/mlir-runtime-migration-plan.md §5.5, §P2 step 3-4.

include "mlir/IR/DialectBase.td"
include "mlir/IR/OpBase.td"
include "mlir/Interfaces/SideEffectInterfaces.td"
include "mlir/Dialect/LLVMIR/LLVMOpBase.td"

// The `Pli` dialect — minimal, containing only the canary operation.
def Pli_Dialect : Dialect {
  let name = "pli";
  let cppNamespace = "::mlir::plic";
  let summary = "Canary dialect for the plic MLIR integration spike";
  let description = [{
    The Pli dialect is a narrow, experimental canary used only by P2 to
    exercise the MLIR helper integration boundary. It contains no production
    PL/I lowering; operations are synthetic scaffolding for pipeline
    verification.
  }];
  let useDefaultAttributePrinterParser = 1;
}

// `pli.buffer_copy` — copies `length` bytes from `src` to `dst`.
// Models a memmove-style aggregate copy so it can lower to llvm.memmove.
def Pli_BufferCopyOp : Op<Pli_Dialect, "buffer_copy"> {
  let summary = "Canary buffer copy: copies length bytes from src to dst";
  let description = [{
    Copies `length` bytes from operand `src` to operand `dst`. This is a
    synthetic canary used to verify the MLIR-to-LLVM helper bridge. It has no
    production PL/I semantics.
  }];

  let arguments = (ins
    Arg<LLVM_AnyPointer, "", [MemWrite]>:$dst,
    Arg<LLVM_AnyPointer, "", [MemRead]>:$src,
    I64:$length
  );
  let assemblyFormat = "$dst `,` $src `,` $length attr-dict `:` type($dst) type($src) type($length)";
}
