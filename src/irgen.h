// irgen.h — LLVM IR generation.
//
// M0 emits textual LLVM IR and lets `clang` assemble/optimize/link it.
// Rationale and migration path: docs/DESIGN-DECISIONS.md ADR-002.
//
// Everything the rest of the compiler sees is the IRGen/Val interface below;
// M1 replaces the bodies with llvm::IRBuilder<> calls (Val::reg becomes
// llvm::Value*) without touching the parser or sema.
#pragma once
#include <map>
#include <string>
#include <vector>
#include "ast.h"
#include "diag.h"
#include "sema.h"

// A materialised PL/I value.
//   scalars : `reg` holds the LLVM value (i32/i64/double, or i1 for BIT)
//   strings : `ptr` holds a pointer to the first character and `len` an i64
struct Val {
  Type ty{};
  std::string reg;
  std::string ptr;
  std::string len;
};

class IRGen {
public:
  IRGen(Diags &d, Sema &s, std::string triple)
      : d_(d), sema_(s), triple_(std::move(triple)) {}

  std::string run(Program &prog);

private:
  // --- emission primitives -------------------------------------------
  std::string fresh(const char *prefix);
  void emit(const std::string &line) { body_ += "  " + line + "\n"; }
  void emitLabel(const std::string &name);
  void branch(const std::string &target);
  std::string globalString(const std::string &s);

  // --- lvalues / storage ---------------------------------------------
  std::string addressOf(Symbol *sym);
  void emitGlobals();
  void emitProc(Proc *p);
  void allocaLocals(Proc *p);

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
  void storeScalarTo(const std::string &addr, const Type &ty, const Val &v);

  Val convert(const Val &v, const Type &dst, SourceLoc loc);
  std::string toI1(const Val &v, SourceLoc loc);
  std::string toI64(const Val &v);
  Val charTemp(int len);          // alloca [len x i8]
  Val charOf(Expr *e);            // materialise a character value

  Diags &d_;
  Sema &sema_;
  std::string triple_;
  std::string module_;   // globals + declares
  std::string body_;     // current function body
  std::string funcs_;    // completed functions
  int n_ = 0;
  bool terminated_ = false;
  Proc *curProc_ = nullptr;
  std::map<std::string, std::string> strLits_;
  std::vector<std::string> allocas_;
};
