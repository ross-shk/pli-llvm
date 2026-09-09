// hir.h — high-level IR (ADR-005).
//
// HIR is PL/I with everything implicit made explicit, still structured. It
// mirrors the typed AST node-for-node so codegen can walk it uniformly, but
// adds the nodes the AST omits — most importantly an explicit `Convert` node
// marking every implicit arithmetic/bit conversion. The plan (IMPLEMENTATION-
// PLAN.md M1) introduces it as a thin mirror first; later milestones hang
// aggregate loops, ON-units and DO semantics off it.
//
// Symbols are shared with sema and still reference the (kept-alive) AST
// procedures/entries; because HIR mirrors their names exactly, codegen's callee
// resolution through Symbol works unchanged.
#pragma once
#include "ast.h"
#include "token.h"
#include "types.h"
#include <memory>
#include <ostream>
#include <string>
#include <vector>

struct Symbol;

struct HExpr;
using HExprP = std::unique_ptr<HExpr>;

struct HExpr {
  enum Kind {
    IntLit,
    FltLit,
    CharLit,
    BitLit, // literals
    VarRef,
    Binary,
    Unary,
    Call,      // compound
    Subscript, // array element A(i) — rules (126)
    Convert,   // explicit conversion (HIR only)
  } kind = IntLit;
  SourceLoc loc{};
  Type ty{}; // the value's type after sema typing

  long long ival = 0;
  double fval = 0;
  std::string sval;                 // CharLit / BitLit payload
  std::string name;                 // VarRef / Call target
  std::vector<std::string> path;    // VarRef: member qualifiers (S.A.B -> {"A","B"})
  std::vector<unsigned> memberPath; // VarRef: resolved LLVM struct field indices (sema)
  Symbol* sym = nullptr;            // resolved by sema
  Tok op = Tok::Eof;                // Binary / Unary operator
  HExprP a, b;
  std::vector<HExprP> args; // Call

  Type convTo{}; // Convert: the target type of the conversion
};

struct HStmt;
using HStmtP = std::unique_ptr<HStmt>;

struct HDeclItem {
  std::string name;
  Type ty{};
  SourceLoc loc{};
  int level = 0; // rule (11) level number; 0 when absent (no structure)
  HExprP init;   // INITIAL(...) — scalar constant only in M0
  Symbol* sym = nullptr;
  bool isEntry = false;          // DECLARE name ENTRY(...) (rule 38)
  std::vector<Type> entryParams; // ENTRY ( ... ) descriptor
  std::string extName;           // EXTERNAL('name') case-sensitive C symbol
};

struct HStmt {
  enum Kind {
    Null,
    Declare,
    Assign,
    If,
    Group,
    Begin,
    DoWhile,
    DoIter,
    Put,
    CallS,
    Return,
    Stop,
    Goto,
    Entry,
    Leave,
  } kind = Null;

  SourceLoc loc{};
  std::vector<std::string> labels; // rule (64) label prefixes

  std::vector<HDeclItem> decls;

  HExprP target, value, cond, from, to, by;
  std::vector<HExprP> extraTargets; // rule (86) multiple assignment a, b, c = e
  HStmtP thenS, elseS;
  std::vector<HStmtP> body;

  std::string name;      // DO control variable, CALL target, ENTRY name
  Symbol* sym = nullptr; // resolved control variable / callee

  // ENTRY statement (rule 56): an alternate entry point, with its own params
  // and optional RETURNS type.
  std::vector<std::string> params; // ENTRY parameter names
  bool entryIsFunction = false;
  Type entryRetTy{};
  std::vector<Symbol*> entryParamSyms; // resolved by sema

  // PUT statement options
  bool skip = false, page = false;
  HExprP skipCount;
  std::vector<HExprP> items;

  std::vector<HExprP> args; // CALL arguments
};

struct HProc {
  std::string name;
  SourceLoc loc{};
  bool isMain = false;
  bool isFunction = false;             // has a RETURNS attribute (rules (5),(34))
  Type retTy{};                        // function return type (RETURNS)
  std::vector<std::string> params;     // rule (4) parameterlist
  std::vector<std::string> entryNames; // rule (3) entry-namelist extra names
  std::vector<HStmtP> body;
  HProc* parent = nullptr; // lexical nesting (rule (8) sentence)
  std::vector<Symbol*> paramSyms;
  std::vector<Symbol*> localSyms;  // AUTOMATIC variables needing an alloca
  std::vector<Symbol*> directUses; // enclosing vars referenced by this body
  std::vector<Symbol*> env;        // static-link targets for this procedure
  std::string irName;              // mangled LLVM symbol
  Proc* src = nullptr;             // the AST procedure this mirrors (Symbol back-references)
};

struct HProgram {
  // All procedures, flattened; `parent` preserves lexical nesting.
  std::vector<std::unique_ptr<HProc>> procs;
  HProc* mainProc = nullptr;
};

// Lower a typed AST into HIR, inserting explicit `Convert` nodes at every
// implicit-conversion site. Does not mutate the AST.
HProgram lower(const Program& prog);

// Print the HIR tree (for `plic --print-hir`).
void printHIR(const HProgram& p, std::ostream& os);
