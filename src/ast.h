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
    DecLit, // FIXED DECIMAL constant (rule 135): exact, value scaled by 10^decScale
    CharLit,
    BitLit,
    VarRef,
    Binary,
    Unary,
    Call,
    Subscript,
    Star // '*' in a subscript list — a cross-section axis (rule 126); not a value
  } kind = IntLit;
  SourceLoc loc{};
  Type ty{}; // assigned by sema

  long long ival = 0;
  double fval = 0;
  int decScale = 0; // DecLit: fraction digits q (the 10^q scaling of ival)
  int decPrec = 0;  // DecLit: total significant digits p
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

// One INITIAL item (rules (26)-(31)): a constant value, an iteration factor
// (n) that repeats its sublist n times, a bare '*' that repeats the last value,
// or a Group — a parenthesised sublist. Sema expands a list of these into a flat
// sequence of element values.
struct InitItem {
  enum Kind { Value, Iter, Repeat, Group } kind = Value;
  ExprP value;                 // Value: the constant; Iter: the factor count
  long long factor = 0;        // Iter: iteration count
  std::vector<InitItem> items; // Iter/Group: the repeated sublist
};

// One DEFINED base subscript (rules 24,126): either an iSUB dummy variable
// (rule 134, uses the DEFINED array's own index) or a fixed index expression.
// An iSUB subscript may be affine `m * 1SUB + c` (rule 134 index arithmetic):
// isub=true carries the coefficients mult (=m) and add (=c); a bare 1SUB is
// mult=1, add=0.
struct DefinedSub {
  bool isub = false;  // true: this subscript contains the iSUB dummy
  ExprP expr;         // fixed index expression (constant in this stage)
  long long mult = 1; // affine iSUB multiplier (m in m*1SUB + c)
  long long add = 0;  // affine iSUB offset (c)
};

struct DeclItem {
  std::string name;
  Type ty{};
  SourceLoc loc{};
  int level = 0; // rule (11) level number; 0 when absent (no structure)
  // Runtime upper-bound expressions for dynamic array axes (rule (13)); empty
  // for a fully constant array. Parallel to ty.dims: an entry is non-null when
  // the corresponding axis's upper bound is a runtime value.
  std::vector<ExprP> dynBounds;
  // Runtime lower-bound expressions for dynamic array axes (rule (13)); empty
  // when every lower bound is constant. Mirrors `dynBounds` for the lower bound.
  std::vector<ExprP> dynLbBounds;
  ExprP init;     // INITIAL(...) — a single simple scalar constant (M0 scalar path)
  ExprP initCall; // INITIAL(CALL f(...)) — a function call initializer (rule 27)
  std::vector<InitItem> initItems;     // INITIAL(...) itemlist (arrays, rule 26-31)
  std::string like;                    // LIKE <unsubscripted-reference> template (rule 43)
  std::string definedBase;             // DEFINED <reference> base name (rule 24); empty = none
  std::vector<DefinedSub> definedSubs; // base subscript list; empty = whole base
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
  bool byName = false;             // rule (86) trailing ", BY NAME" on assignment
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
