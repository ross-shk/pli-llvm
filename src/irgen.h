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
#include "sema.h"

// A materialised PL/I value.
//   scalars : `reg` holds the LLVM value (i32/i64/double, or i1 for BIT)
//   strings : `ptr` holds a pointer to the first character and `len` an i64
struct Val {
  Type ty{};
  llvm::Value *reg = nullptr;
  llvm::Value *ptr = nullptr;
  llvm::Value *len = nullptr;
};

class IRGen {
public:
  IRGen(Diags &d, Sema &s, std::string triple)
      : d_(d), sema_(s), triple_(std::move(triple)),
        mod_("plic", ctx_), b_(ctx_) {}

  std::string run(Program &prog);

private:
  // --- emission primitives -------------------------------------------
  llvm::Type *llvmTy(const Type &t);
  llvm::Value *i32(int v);
  llvm::Value *i64(long long v);
  llvm::Value *flt(double d);
  void startBlock(llvm::BasicBlock *bb);   // branch into bb unless terminated, then insert there
  void newBlock();                          // a fresh dead block for unreachable code
  void branch(llvm::BasicBlock *target);
  llvm::GlobalVariable *globalString(const std::string &s);

  // --- lvalues / storage ---------------------------------------------
  llvm::Value *addressOf(Symbol *sym);
  // Every storage-owning symbol this frame can address directly (global,
  // local alloca, parameter, or a static link) to its address value.
  std::unordered_map<Symbol *, llvm::Value *> symAddr_;
  void emitGlobals();
  void declareProc(Proc *p);  // pre-create a proc's functions/aliases so calls resolve
  void emitProc(Proc *p);
  void emitPlainProc(Proc *p, llvm::Type *retLLVM);
  void emitMultiEntryProc(Proc *p, const std::vector<Stmt *> &entries, llvm::Type *retLLVM);
  void allocaLocals(Proc *p);
  void emitInitials(Proc *p);  // INITIAL stores on AUTOMATIC vars (rule 26)
  void closeBlocks();          // give any unterminated block an unreachable terminator
  void collectGotoBlocks(Stmt *s);  // assign an LLVM block to each labelled stmt
  // rule (56): LLVM function name for an ENTRY statement's alternate entry point.
  std::string entryIrName(Proc *p, Stmt *e);

  // --- statements & expressions --------------------------------------
  void emitStmt(Stmt *s);
  void emitAssign(Stmt *s);
  void emitIf(Stmt *s);
  void emitDoWhile(Stmt *s);
  void emitDoIter(Stmt *s);
  void emitPut(Stmt *s);
  void emitCall(Stmt *s);

  Val emitExpr(Expr *e);
  Val loadSym(Symbol *sym, const Type &ty);
  void storeTo(Symbol *sym, const Val &v, SourceLoc loc);
  void storeScalarTo(llvm::Value *addr, const Type &ty, const Val &v);

  Val convert(const Val &v, const Type &dst, SourceLoc loc);
  llvm::Value *toI1(const Val &v, SourceLoc loc);
  llvm::Value *toI64(const Val &v);
  Val charTemp(int len);          // alloca [len x i8]
  Val charOf(Expr *e);            // materialise a character value

  // Runtime callee lookup: get-or-create the declaration for a pli_* symbol.
  llvm::Function *runtimeFn(const std::string &name, llvm::Type *ret,
                            std::vector<llvm::Type *> args, bool vararg = false);

  Diags &d_;
  Sema &sema_;
  std::string triple_;
  llvm::LLVMContext ctx_;
  llvm::Module mod_;
  llvm::IRBuilder<> b_;
  llvm::Function *curFn_ = nullptr;  // the function we are currently filling
  std::vector<llvm::GlobalVariable *> strLits_;
  int n_ = 0;
  bool terminated_ = false;
  Proc *curProc_ = nullptr;
  std::map<std::string, llvm::BasicBlock *> labelBlocks_;  // label -> block (rule 77)
};
