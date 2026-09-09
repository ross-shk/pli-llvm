// irgen.h — LLVM IR generation.
//
// M1 generates IR with llvm::IRBuilder<> (ADR-002); `run` prints the built
// llvm::Module to textual IR so the driver and the Makefile/clang pipeline are
// unchanged. Everything the rest of the compiler sees is the IRGen/Val
// interface below; parser and sema are untouched.
#pragma once
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include "ast.h"
#include "diag.h"
#include "hir.h"
#include "sema.h"

// A materialised PL/I value.
//   scalars : `reg` holds the LLVM value (i32/i64/double, or i1 for BIT)
//   strings : `ptr` holds a pointer to the first character and `len` an i64
struct Val {
  Type ty{};
  llvm::Value* reg = nullptr;
  llvm::Value* ptr = nullptr;
  llvm::Value* len = nullptr;
};

class IRGen {
public:
  IRGen(Diags& d, Sema& s, std::string triple)
      : d_(d), sema_(s), triple_(std::move(triple)), mod_("plic", ctx_), b_(ctx_) {}

  std::string run(HProgram& prog);

private:
  // --- emission primitives -------------------------------------------
  llvm::Type* llvmTy(const Type& t);
  llvm::Value* i32(int v);
  llvm::Value* i64(long long v);
  llvm::Value* flt(double d);
  llvm::AllocaInst* entryAlloca(llvm::Type* ty, const llvm::Twine& name);
  void startBlock(llvm::BasicBlock* bb); // branch into bb unless terminated, then insert there
  void newBlock();                       // a fresh dead block for unreachable code
  void branch(llvm::BasicBlock* target);
  llvm::GlobalVariable* globalString(const std::string& s);

  // --- lvalues / storage ---------------------------------------------
  llvm::Value* addressOf(Symbol* sym);
  // Every storage-owning symbol this frame can address directly (global,
  // local alloca, parameter, or a static link) to its address value.
  std::unordered_map<Symbol*, llvm::Value*> symAddr_;
  void emitGlobals();
  void declareProc(HProc* p); // pre-create a proc's functions/aliases so calls resolve
  void emitProc(HProc* p);
  void emitPlainProc(HProc* p, llvm::Type* retLLVM);
  void emitMultiEntryProc(HProc* p, const std::vector<HStmt*>& entries, llvm::Type* retLLVM);
  void allocaLocals(HProc* p);
  void emitInitials(HProc* p);      // INITIAL stores on AUTOMATIC vars (rule 26)
  void collectGotoBlocks(HStmt* s); // assign an LLVM block to each labelled stmt
  // rule (56): LLVM function name for an ENTRY statement's alternate entry point.
  static std::string entryIrName(const std::string& proc, const std::string& parent,
                                 const std::string& entry);

  // --- statements & expressions --------------------------------------
  void emitStmt(HStmt* s);
  void emitAssign(HStmt* s);
  void emitIf(HStmt* s);
  void emitDoWhile(HStmt* s);
  void emitDoIter(HStmt* s);
  void emitPut(HStmt* s);
  void emitCall(HStmt* s);

  // Address of one call argument for a by-reference parameter (rule 4): a
  // direct variable of the same type passes its own address; anything else is
  // copied into a fresh dummy argument. Shared by emitCall and emitExpr so the
  // marshalling logic has a single home.
  llvm::Value* argAddr(HExpr* a, const Type& pty);
  // Append the callee's static-link arguments (its enclosing automatic
  // variables, rule 8). Shared by emitCall and emitExpr.
  void appendStaticLinks(Proc* callee, std::vector<llvm::Value*>& args);

  Val emitExpr(HExpr* e);
  // Number of elements across all axes: the product of (ub - lb + 1) (rule (12)).
  long long arrayExtent(const Type& arr);
  // Address of one array element A(i,j,...) (rule 126), after a runtime bounds
  // check on each axis. `arr` is the array type (bounds + element), `base` the
  // address of the array storage; `idxs` holds one index per axis.
  llvm::Value* arrayElementAddr(const Type& arr, llvm::Value* base, const std::vector<HExprP>& idxs,
                                SourceLoc loc);
  // Address of a qualified member S.A.B (rule 124): a GEP off the structure
  // base through the recorded LLVM field indices (relative to each nested
  // struct), loading the leaf member's scalar value.
  llvm::Value* memberAddr(Symbol* base, const std::vector<unsigned>& path, SourceLoc loc);
  // The resolved type of a qualified member S.A.B (rule 124): walk the recorded
  // field indices to recover the leaf member's type (an array member's array
  // type), mirroring memberAddr without emitting GEPs.
  const Type& memberType(Symbol* base, const std::vector<unsigned>& path);
  // BY NAME assignment (rule 86): copy each same-named member of `dst` from
  // `src` at their struct bases, recursing into minor structures; names absent
  // from either side are skipped.
  void emitByNameCopy(llvm::Value* dstBase, llvm::Value* srcBase, const Type& dst, const Type& src,
                      SourceLoc loc);
  // Emit a built-in function call (SUBSTR, INDEX, ABS, …). Returns true if
  // `e` was a recognised built-in; false otherwise, so emitExpr can fall
  // through to the general function-call path.
  bool emitBuiltin(HExpr* e, Val& out);
  Val loadSym(Symbol* sym, const Type& ty);
  void storeTo(Symbol* sym, const Val& v, SourceLoc loc);
  void storeScalarTo(llvm::Value* addr, const Type& ty, const Val& v);
  // Load / store one array element (rule 126), with the SUBSCRIPTRANGE check.
  Val loadArrayElement(Symbol* sym, const std::vector<HExprP>& idxs, SourceLoc loc);
  void storeArrayElement(Symbol* sym, const std::vector<HExprP>& idxs, const Val& src,
                         SourceLoc loc);

  Val convert(const Val& v, const Type& dst, SourceLoc loc);
  llvm::Value* toI1(const Val& v, SourceLoc loc);
  llvm::Value* toI64(const Val& v);
  Val charTemp(int len); // alloca [len x i8]
  Val charOf(HExpr* e);  // materialise a character value

  // Runtime callee lookup: get-or-create the declaration for a pli_* symbol.
  // The signature is taken from runtime/pli_rt_abi.def (the single source of
  // truth for the runtime ABI), not re-specified by the caller.
  llvm::Function* runtimeFn(const std::string& name);
  // Get-or-create an LLVM intrinsic with an explicit signature (used only for
  // non-ABI LLVM builtins such as llvm.pow.f64 / llvm.fabs.f64).
  llvm::Function* intrinsicFn(const std::string& name, llvm::Type* ret,
                              std::vector<llvm::Type*> args);
  // Resolve a call target's LLVM function; for an external C entry (rule 38),
  // get-or-create its external declaration (no PL/I body).
  llvm::Function* calleeFn(Symbol* sym);

  Diags& d_;
  Sema& sema_;
  std::string triple_;
  llvm::LLVMContext ctx_;
  llvm::Module mod_;
  llvm::IRBuilder<> b_;
  llvm::Function* curFn_ = nullptr; // the function we are currently filling
  std::vector<llvm::GlobalVariable*> strLits_;
  int n_ = 0;
  HProc* curProc_ = nullptr;
  std::map<std::string, llvm::BasicBlock*> labelBlocks_; // label -> block (rule 77)
};
