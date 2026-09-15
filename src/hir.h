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
    DecLit, // FIXED DECIMAL constant (rule 135), exact
    CharLit,
    BitLit, // literals
    VarRef,
    Binary,
    Unary,
    Call,      // compound
    Subscript, // array element A(i) — rules (126)
    Star,      // '*' in a subscript list — a cross-section axis (rule 126)
    ComplexLit, // imaginary constant (rule 139): fval is the imaginary part
    Convert,   // explicit conversion (HIR only)
  } kind = IntLit;
  SourceLoc loc{};
  Type ty{}; // the value's type after sema typing

  long long ival = 0;
  double fval = 0;
  int decScale = 0;                 // DecLit: fraction digits q (the 10^q scaling of ival)
  int decPrec = 0;                  // DecLit: total significant digits p
  std::string sval;                 // CharLit / BitLit payload
  std::string name;                 // VarRef / Call target
  std::vector<std::string> path;    // VarRef: member qualifiers (S.A.B -> {"A","B"})
  std::vector<unsigned> memberPath; // VarRef: resolved LLVM struct field indices (sema)
  Symbol* sym = nullptr;            // resolved by sema
  HExprP locPtr;     // VarRef: the locator pointer of a P->X reference (rule 124); null = none
  Tok op = Tok::Eof; // Binary / Unary operator
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
  // Runtime upper-bound expressions for dynamic array axes (rule (13)); empty
  // for a fully constant array. Parallel to ty.dims.
  std::vector<HExprP> dynBounds;
  // Runtime lower-bound expressions for dynamic array axes (rule (13)); empty
  // when every lower bound is constant. Mirrors `dynBounds`.
  std::vector<HExprP> dynLbBounds;
  // Owns the lowered bound exprs of dynamic-array structure members (rule 13),
  // which Symbol::DynMemberH.ub/lb point into; kept in HIR so IRGen can size the
  // member buffers at entry. Mirrors dynBounds ownership.
  std::vector<HExprP> dynMemberBounds;
  HExprP init;     // INITIAL(...) — scalar constant only in M0
  HExprP initCall; // INITIAL(CALL f(...)) — a function-call initializer (rule 27)
  Symbol* sym = nullptr;
  bool isEntry = false;          // DECLARE name ENTRY(...) (rule 38)
  std::vector<Type> entryParams; // ENTRY ( ... ) descriptor
  bool entryIsFunction = false;  // ENTRY ... RETURNS(...) (rule (34)): returns a value
  Type entryRetTy;               // the RETURNS(...) result type of an ENTRY declaration
  std::string extName;           // EXTERNAL('name') case-sensitive C symbol
};

// One FORMAT item for edit-directed I/O (rules (48)-(54)); mirrors the AST
// FormatItem with lowered width/decimals expressions.
struct HFormatItem {
  enum Kind { A, F, E, X, Skip, Page, Line } kind = A;
  HExprP w; // field width
  HExprP d; // F/E: fractional digits
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
    Get,
    CallS,
    Return,
    Stop,
    Goto,
    Entry,
    Allocate,
    Free,
    Open,
    Close,
    Leave,
    On,     // rule (91)
    Revert, // rule (92)
    Signal, // rule (93)
    Display, // rule (114)
  } kind = Null;

  SourceLoc loc{};
  std::vector<std::string> labels; // rule (64) label prefixes

  std::vector<HDeclItem> decls;

  HExprP target, value, cond, from, to, by;
  std::vector<HExprP> extraTargets; // rule (86) multiple assignment a, b, c = e
  bool byName = false;              // rule (86) trailing ", BY NAME" on assignment
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

  // GET/PUT stream statement options (rules (104),(105))
  bool skip = false, page = false;
  HExprP skipCount;
  std::vector<HExprP> items;
  // Edit-directed transmission (rule (108)); mirrors the AST flag and format
  // list, with lowered width/decimals expressions.
  bool edit = false;
  // Data-directed transmission (rule (106)); mirrors the AST flag.
  bool data = false;
  std::vector<HFormatItem> formats;
  // STRING ( reference ) option (rule 105): the character variable the list-
  // directed output is written into (PUT) or input read from (GET); null =
  // SYSIN/SYSPRINT.
  HExprP stringTarget;

  // FILE ( f ) option (rule 105) and OPEN/CLOSE FILE ( f ): the resolved FILE
  // variable and its runtime slot; OPEN carries the TITLE string and mode.
  Symbol* fileSym = nullptr;
  std::string openTitle;
  bool openInput = false;

  std::vector<HExprP> args; // CALL arguments

  // ON statement (rule 91); REVERT/SIGNAL use condName/condKey only.
  std::string condName;
  int condKey = 0; // 0 = ERROR, Stmt::kSizeCondKey = SIZE, else sema key
  bool snap = false;
  bool isSystem = false;
  HStmtP unit;
  int onIndex = -1; // dense handler id assigned by irgen before emission
  // ALLOCATE (rule 87): per based-allocate-item, the based variable reference
  // and its SET(...) pointer target (rule 88).
  std::vector<HExprP> allocBase;
  std::vector<HExprP> allocSet;
  // FREE (rule 90): per item, the based variable reference (locPtr carries an
  // explicit locator, null = the BASED base).
  std::vector<HExprP> freeBase;
};

struct HProc {
  std::string name;
  SourceLoc loc{};
  bool isMain = false;
  bool isFunction = false;             // has a RETURNS attribute (rules (5),(34))
  Type retTy{};                        // function return type (RETURNS)
  Type commonRetTy{};                  // rule (56): the single result type shared by all
                                       // function-valued entry points (void if none)
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
