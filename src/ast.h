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
    Star, // '*' in a subscript list — a cross-section axis (rule 126); not a value
    ComplexLit, // imaginary constant 2i (rule 139): fval is the imaginary part
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
  ExprP locPtr;      // VarRef: the locator pointer of a P->X reference (rule 124); null = none
  Tok op = Tok::Eof; // Binary / Unary operator
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

// One FORMAT item for edit-directed I/O (rules (48)-(54)): a data format
// (A character, F fixed) that transmits the next data item, or a control format
// (X spacing, SKIP/PAGE/LINE line control) that acts without consuming data.
// F(w,d): w = field width, d = fractional digits; A(w)/X(w): w = field width.
struct FormatItem {
  enum Kind { A, F, E, X, Skip, Page, Line } kind = A;
  ExprP w; // field width
  ExprP d; // F/E: fractional digits
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
  std::string basedBase;               // BASED( <pointer-name> ) base (rule 25); empty = none
  // A dynamic (runtime-extent) array that is a structure member (rule 13): its
  // field path (the indices memberAddr walks) and its bound expressions. The
  // member's DeclItem still owns dynBounds/dynLbBounds; these reference them.
  struct DynMemberInfo {
    std::vector<unsigned> path;
    Expr* ub; // runtime upper bound expr (null = constant); non-owning
    Expr* lb; // runtime lower bound expr (null = constant); non-owning
  };
  std::vector<DynMemberInfo> dynMembers;
  Symbol* sym = nullptr;
  bool isEntry = false;          // DECLARE name ENTRY(...) (rule 38)
  bool fileAttr = false;         // DECLARE name FILE (rules 39,40): a named file
  std::vector<Type> entryParams; // ENTRY ( ... ) descriptor
  bool entryIsFunction = false;  // ENTRY ... RETURNS(...) (rule (34)): returns a value
  Type entryRetTy;               // the RETURNS(...) result type of an ENTRY declaration
  std::string extName;           // EXTERNAL('name') case-sensitive C symbol
};

struct Stmt {
  enum Kind {
    Null,     // rule (67)
    Declare,  // rule (9)
    Assign,   // rule (86)
    If,       // rule (74)
    Group,    // rule (70)  DO; ... END;
    Begin,    // rule (68)  BEGIN; ... END; — a block with its own scope
    DoWhile,  // rule (71)  DO WHILE(e);
    DoIter,   // rule (71)+(72)+(73)
    Put,      // rules (104)-(109)
    Get,      // rules (104)-(109)
    CallS,    // rule (78)
    Return,   // rule (81)
    Stop,     // rule (85)
    Goto,     // rule (77)  GO TO label — local, within a procedure (M1)
    Entry,    // rule (56)  label: ENTRY [(params)] [RETURNS(...)] —
              //            an alternate entry point into this procedure
    Allocate, // rule (87)  ALLOCATE based-allocate-item{,...}
    Free,     // rule (90)  FREE ( [reference ->] identifier ){,...}
    Open,     // rule (100) OPEN open-optionslist{,...};
    Close,    // rule (102) CLOSE close-optionslist{,...};
    Leave,    // extension (ADR-105): LEAVE [label]; exits an iterative group
    Iterate,  // extension (ADR-105): ITERATE [label]; continues one (`name` = target)
    On,       // rule (91)  ON condition [SNAP] (unit | SYSTEM)
    Revert,   // rule (92)  REVERT condition
    Signal,   // rule (93)  SIGNAL condition
    Display,  // rule (114) DISPLAY (expression) — one scalar value
    Read,     // rule (112) READ FILE ( f ) INTO ( reference ) — sequential slice
    Write,    // rule (112) WRITE FILE ( f ) FROM ( reference ) — sequential slice
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

  // PUT/GET stream statement options (rules (104),(105))
  bool skip = false, page = false;
  ExprP skipCount;
  std::vector<ExprP> items;
  // Edit-directed transmission (rule (108)): when true, the data items are
  // written/read through the paired format list instead of list-directed.
  bool edit = false;
  // Data-directed transmission (rule (106)): when true, PUT writes each item
  // as NAME=value (GET DATA stays diagnosed in this stage).
  bool data = false;
  std::vector<FormatItem> formats; // one per data item or control action
  // STRING ( reference ) option (rule 105): the character variable that the
  // list-directed output is written into (PUT) or input is read from (GET);
  // null when the stream is SYSIN/SYSPRINT.
  ExprP stringTarget;
  // FILE ( f ) option (rule 105) and OPEN/CLOSE FILE ( f ): the name of the
  // FILE variable being named; resolved to `fileSym` by sema. For OPEN, the
  // TITLE ('name') string and the INPUT/OUTPUT mode are also carried.
  std::string fileIdent;
  Symbol* fileSym = nullptr;
  std::string openTitle;
  bool openInput = false;
  bool openRecord = false; // OPEN ... RECORD SEQUENTIAL (rules (101),(112)): a record
                           // file; READ/WRITE transfer fixed-size binary records

  std::vector<ExprP> args; // CALL arguments

  // ON statement (rule 91): the established condition, whether SNAP was
  // given, whether the unit is SYSTEM, and the unit body (null for SYSTEM).
  // REVERT/SIGNAL (rules 92,93) use condName only.
  // Condition keys: 0 = ERROR, kSizeCondKey = SIZE (rule 94, QR1.4),
  // kSubscriptrangeCondKey = SUBSCRIPTRANGE and kZerodivideCondKey =
  // ZERODIVIDE (rule 94), else index + 1 into Program::condNames for a
  // rule (99) name.
  static constexpr int kSizeCondKey = -1;
  static constexpr int kSubscriptrangeCondKey = -2;
  static constexpr int kZerodivideCondKey = -3;
  std::string condName; // e.g. "ERROR", "SIZE", "SUBSCRIPTRANGE", "ZERODIVIDE",
                        // or a rule (99) condition name
  int condKey = 0;      // 0 = ERROR, negative = fixed key above, else condNames index + 1
  bool snap = false;
  bool isSystem = false;
  StmtP unit;
  int onIndex = -1; // dense handler id assigned during lowering (rule 91)
  // ALLOCATE (rule 87): per based-allocate-item, the based variable reference
  // (a VarRef to a based structure) and its SET(...) pointer target (rule 88;
  // allocSet[i] is a VarRef to a POINTER).
  std::vector<ExprP> allocBase;
  std::vector<ExprP> allocSet;
  // FREE (rule 90): per item, the based variable reference. An explicit locator
  // is carried on freeBase[i]->locPtr; null means the BASED base pointer.
  std::vector<ExprP> freeBase;
};

struct Proc {
  std::string name;
  SourceLoc loc{};
  bool isMain = false;
  bool isExternal = false;             // rule (42): a top-level non-MAIN procedure is
                                       // externally linked under its upper-cased name
  bool isRecursive = false;            // RECURSIVE option (rule (5))
  bool isFunction = false;             // has a RETURNS attribute (rules (5),(34))
  Type retTy{};                        // function return type (RETURNS)
  Type commonRetTy{};                  // rule (56): the single result type shared by all
                                       // function-valued entry points (void if none)
  // A structure-valued function's result type (rule 127): the name of an
  // enclosing structure variable whose shape the function returns. The parser
  // records the name; sema resolves it to a deep copy of that type into retTy.
  std::string returnsStructName;
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
  // Programmer-named conditions in first-use order (rule 99); a use-site key
  // is its index + 1 (key 0 means ERROR, negative keys are the fixed
  // conditions above).
  std::vector<std::string> condNames;
};
