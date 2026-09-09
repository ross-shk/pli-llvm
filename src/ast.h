// ast.h — abstract syntax tree.
//
// WIREFRAME NOTE: M0 uses two "wide" node structs (Expr, Stmt) with a kind tag
// instead of a class hierarchy. This keeps the bootstrap small; M1 replaces
// them with a proper node hierarchy + visitor, and inserts the HIR layer
// described in docs/ARCHITECTURE.md.
#pragma once
#include "token.h"
#include "types.h"
#include <memory>
#include <string>
#include <vector>

struct Symbol;
struct Proc;

struct Expr;
using ExprP = std::unique_ptr<Expr>;

struct Expr {
  enum Kind {
    IntLit,
    FltLit,
    CharLit,
    BitLit,
    VarRef,
    Binary,
    Unary,
    Call,
    Subscript
  } kind = IntLit;
  SourceLoc loc{};
  Type ty{}; // assigned by sema

  long long ival = 0;
  double fval = 0;
  std::string sval; // CharLit / BitLit payload
  std::string name; // VarRef / Call target
  std::vector<std::string>
      path; // VarRef: member qualifiers after the base name (S.A.B -> {"A","B"})
  std::vector<unsigned> memberPath; // VarRef: resolved LLVM struct field indices (set by sema)
  Symbol* sym = nullptr;            // resolved by sema
  Tok op = Tok::Eof;                // Binary / Unary operator
  ExprP a, b;
  std::vector<ExprP> args; // Call
};

struct Stmt;
using StmtP = std::unique_ptr<Stmt>;

struct DeclItem {
  std::string name;
  Type ty{};
  SourceLoc loc{};
  int level = 0; // rule (11) level number; 0 when absent (no structure)
  ExprP init;    // INITIAL(...) — scalar constant only in M0
  Symbol* sym = nullptr;
  bool isEntry = false;          // DECLARE name ENTRY(...) (rule 38)
  std::vector<Type> entryParams; // ENTRY ( ... ) descriptor
  std::string extName;           // EXTERNAL('name') case-sensitive C symbol
};

struct Stmt {
  enum Kind {
    Null,    // rule (67)
    Declare, // rule (9)
    Assign,  // rule (86)
    If,      // rule (74)
    Group,   // rule (70)  DO; ... END;
    Begin,   // rule (68)  BEGIN; ... END; — a block with its own scope
    DoWhile, // rule (71)  DO WHILE(e);
    DoIter,  // rule (71)+(72)+(73)
    Put,     // rules (104)-(109)
    CallS,   // rule (78)
    Return,  // rule (81)
    Stop,    // rule (85)
    Goto,    // rule (77)  GO TO label — local, within a procedure (M1)
    Entry,   // rule (56)  label: ENTRY [(params)] [RETURNS(...)] —
             //            an alternate entry point into this procedure
    Leave,   // (not in TR 25.084; modern LEAVE, rejected in M0)
  } kind = Null;

  SourceLoc loc{};
  std::vector<std::string> labels; // rule (64) label prefixes

  std::vector<DeclItem> decls;

  ExprP target, value, cond, from, to, by;
  std::vector<ExprP> extraTargets; // rule (86) multiple assignment a, b, c = e
  StmtP thenS, elseS;
  std::vector<StmtP> body;

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
  ExprP skipCount;
  std::vector<ExprP> items;

  std::vector<ExprP> args; // CALL arguments
};

struct Proc {
  std::string name;
  SourceLoc loc{};
  bool isMain = false;
  bool isFunction = false;             // has a RETURNS attribute (rules (5),(34))
  Type retTy{};                        // function return type (RETURNS)
  std::vector<std::string> params;     // rule (4) parameterlist
  std::vector<std::string> entryNames; // rule (3) entry-namelist extra names
  std::vector<StmtP> body;
  Proc* parent = nullptr; // lexical nesting (rule (8) sentence)
  std::vector<Symbol*> paramSyms;
  std::vector<Symbol*> localSyms;  // AUTOMATIC variables needing an alloca
  std::vector<Symbol*> directUses; // enclosing vars referenced by this body
  std::vector<Symbol*> env;        // static-link targets for this procedure
  std::string irName;              // mangled LLVM symbol
};

struct Program {
  // All procedures, flattened; `parent` preserves lexical nesting.
  std::vector<std::unique_ptr<Proc>> procs;
  Proc* mainProc = nullptr;
};
