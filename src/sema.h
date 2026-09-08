// sema.h — declaration processing, name resolution and expression typing.
#pragma once
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include "ast.h"
#include "diag.h"

struct Symbol {
  std::string name;
  Type ty{};
  SourceLoc loc{};
  enum Kind { Var, Param, ProcName } kind = Var;
  bool isStatic = false;   // STATIC storage: an LLVM global
  bool implicit = false;   // created by the implicit-declaration rule
  bool isEntry = false;    // external C entry (DECLARE ... ENTRY): no body
  std::vector<Type> entryParams;  // ENTRY(...) descriptor, for codegen
  std::string irName;      // "@pli_g_X" / "%X.addr" / "%X.ptr"
  Proc *proc = nullptr;    // for ProcName
  Proc *owner = nullptr;   // the procedure that declares this variable (static link)
  Stmt *entry = nullptr;   // if this ProcName is an ENTRY statement (rule 56)
  Expr *initExpr = nullptr;  // folded INITIAL constant, rule (26)
};

struct Scope {
  Scope *parent = nullptr;
  std::unordered_map<std::string, Symbol *> tab;
  std::vector<Symbol *> order;  // declaration order, for deterministic output
};

class Sema {
public:
  explicit Sema(Diags &d) : d_(d) {}
  bool run(Program &prog, bool compileOnly = false);

  // Symbols that require storage, in declaration order.
  const std::vector<Symbol *> &storage() const { return storage_; }

  // External entries (DECLARE ... ENTRY) to forward-declare, in decl order.
  const std::vector<Symbol *> &entries() const { return entries_; }

private:
  Scope *scopeFor(Proc *p);
  Symbol *lookup(Scope *sc, const std::string &n);
  Symbol *declare(Scope *sc, const std::string &n, Type t, SourceLoc l,
                  Symbol::Kind k, bool isStatic);
  Symbol *implicitDeclare(Scope *sc, const std::string &n, SourceLoc l, bool isStatic);

  void processProc(Proc *p);
  // Bottom-up: fill each Proc::env with the enclosing variables its subtree
  // accesses, so codegen can thread a static link (M1, removes ADR-010 dev).
  void computeEnv(Proc *p);
  void addEnv(std::vector<Symbol *> &env, Symbol *s);
  // Is `p` nested (directly or transitively) inside `anc`?
  bool isDescendantOf(Proc *p, Proc *anc);
  // Turn a parameter name list into by-reference Param symbols in `sc`, so the
  // same storage is shared when a name repeats (proc param vs ENTRY param).
  void resolveParams(Scope *sc, Proc *p, const std::vector<std::string> &names,
                     std::vector<Symbol *> &out);
  void collectDecls(std::vector<StmtP> &body, Scope *sc, Proc *p, bool isStatic);
  void collectLabels(Stmt *s);  // gather GO TO targets defined in this proc
  void checkStmt(Stmt *s, Scope *sc, Proc *p);
  void typeExpr(Expr *e, Scope *sc, Proc *p);
  bool checkAssignable(const Type &dst, const Type &src, SourceLoc loc, const char *what);

  Diags &d_;
  Program *prog_ = nullptr;
  std::vector<std::unique_ptr<Symbol>> owned_;
  std::vector<std::unique_ptr<Scope>> scopes_;
  std::unordered_map<Proc *, Scope *> procScopes_;
  Scope *rootScope_ = nullptr;  // program scope: all external procedure names
  std::vector<Symbol *> storage_;
  std::vector<Symbol *> entries_;   // external C entries, in declaration order
  std::set<std::string> procLabels_;  // GO TO targets in the current proc (rule 77)
  std::set<std::string> irNames_;   // irNames in use, to disambiguate shadowing
  std::unordered_map<Stmt *, Scope *> beginScopes_;  // BEGIN block -> its scope
};

// Arithmetic result type per the conversion rules (M0 approximation).
Type arithResultType(const Type &a, const Type &b);
