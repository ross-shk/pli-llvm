// sema.h — declaration processing, name resolution and expression typing.
#pragma once
#include "ast.h"
#include "diag.h"
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

struct HExpr; // forward decl; dynamic array bounds are lowered to HIR in hir.cpp

struct Symbol {
  std::string name;
  Type ty{};
  SourceLoc loc{};
  enum Kind { Var, Param, ProcName } kind = Var;
  bool isStatic = false;   // STATIC storage: an LLVM global
  bool implicit = false;   // created by the implicit-declaration rule
  bool isEntry = false;    // external C entry (DECLARE ... ENTRY): no body
  bool isValue = false;    // VALUE named constant (extension, ADR-108): reassignment diagnosed
  bool isOptional = false; // OPTIONAL parameter (extension): may be omitted at a call
  bool fileAttr = false;   // FILE variable (rules 39,40): a named file
  int fileSlot = -1;       // runtime slot index for a FILE variable (100-103)
  std::vector<Type> entryParams; // ENTRY(...) descriptor, for codegen
  bool entryIsFunction = false;  // ENTRY ... RETURNS(...): an external function entry
  Type entryRetTy;               // the RETURNS(...) result type of an ENTRY declaration
  bool entryByValue = false;     // ENTRY OPTIONS(LINKAGE(SYSTEM)/BYVALUE) (rule (34))
  std::string irName;            // "@pli_g_X" / "%X.addr" / "%X.ptr"
  Proc* proc = nullptr;          // for ProcName
  Proc* owner = nullptr;         // the procedure that declares this variable (static link)
  Stmt* entry = nullptr;         // if this ProcName is an ENTRY statement (rule 56)
  Expr* initExpr = nullptr;      // folded INITIAL constant, rule (26)
  std::vector<Expr*> initElems;  // folded INITIAL element list for arrays, rule (26)
  Expr* initCall = nullptr;      // INITIAL(CALL f(...)) call expr (rule 27), lowered to HIR
  HExpr* initCallH = nullptr;    // the lowered INITIAL CALL expression, for codegen
  Symbol* definedBase = nullptr; // DEFINED on this variable's storage (rule 24); null = none
  // BASED (rule 25): the POINTER variable that addresses this based structure's
  // storage; a based symbol has no own storage, its address is the pointer value.
  Symbol* basedBase = nullptr;
  bool controlled = false; // CONTROLLED (rule (15), ADR-140): generation-stack storage
  int ctlSlot = -1;        // runtime generation-stack slot index (87-90)
  // For a DEFINED base that is a subscripted reference (rule 126):
  //   definedIsubAxis = -1  -> a whole base, or a scalar overlay (no iSUB)
  //   definedIsubAxis >= 0  -> this X axis holds the iSUB dummy (rule 134)
  // definedConst holds one fixed constant per X axis (0 at the iSUB axis).
  int definedIsubAxis = -1;
  std::vector<long long> definedConst;
  // Affine iSUB base index (rule 134 index arithmetic): the iSUB axis base
  // subscript is `definedIsubMult * overlay_index + definedIsubAdd`. Bare iSUB
  // is mult=1, add=0.
  long long definedIsubMult = 1;
  long long definedIsubAdd = 0;
  // Runtime upper-bound expression of a dynamic array's first axis (rule (13)),
  // lowered to HIR during AST->HIR lowering; null for a constant array. Used by
  // irgen to size and bounds-check the dynamic array.
  HExpr* dynUb = nullptr;
  // The lowered runtime lower-bound expression of a dynamic array (rule (13)),
  // the mirror of `dynUb`; null when the lower bound is constant.
  HExpr* dynLb = nullptr;
  // A dynamic (runtime-extent) array that is a structure member (rule 13), with
  // its field path (the indices memberAddr walks) and its lowered bound exprs.
  // Lowered from DeclItem::dynMembers during AST->HIR lowering.
  struct DynMemberH {
    std::vector<unsigned> path;
    HExpr* ub = nullptr;
    HExpr* lb = nullptr;
  };
  std::vector<DynMemberH> dynMembers;
};

struct Scope {
  Scope* parent = nullptr;
  std::unordered_map<std::string, Symbol*> tab;
  std::vector<Symbol*> order; // declaration order, for deterministic output
  // DEFINE ALIAS types (extension, ADR-114): a separate namespace from
  // variables, so an alias never collides with a variable of any type.
  std::unordered_map<std::string, Type> aliases;
};

class Sema {
public:
  explicit Sema(Diags& d) : d_(d) {}
  bool run(Program& prog, bool compileOnly = false);

  // Symbols that require storage, in declaration order.
  const std::vector<Symbol*>& storage() const { return storage_; }

  // External entries (DECLARE ... ENTRY) to forward-declare, in decl order.
  const std::vector<Symbol*>& entries() const { return entries_; }

private:
  Scope* scopeFor(Proc* p);
  Symbol* lookup(Scope* sc, const std::string& n);
  Symbol* declare(Scope* sc, const std::string& n, Type t, SourceLoc l, Symbol::Kind k,
                  bool isStatic);
  Symbol* implicitDeclare(Scope* sc, const std::string& n, SourceLoc l, bool isStatic);

  void processProc(Proc* p);
  // Resolve a structure-valued function's RETURNS name (rule 127): deep-copy the
  // referenced structure variable's type into p->retTy. Runs after all
  // declarations are collected, so the template is visible in the proc's scope.
  void resolveStructReturn(Proc* p);
  // Bottom-up: fill each Proc::env with the enclosing variables its subtree
  // accesses, so codegen can thread a static link (M1, removes ADR-010 dev).
  void computeEnv(Proc* p);
  void addEnv(std::vector<Symbol*>& env, Symbol* s);
  // Rule (8): record a static-link use of `s` and its BASED/DEFINED bases.
  void noteStaticUse(Proc* p, Symbol* s);
  // Rule (5): every procedure in a static call cycle must carry RECURSIVE.
  // Runs after all bodies are typed so CALL and function-reference callees
  // are resolved; reports one error at each non-recursive cycle member.
  void checkRecursion();
  void collectCallees(const std::vector<StmtP>& body, std::vector<Proc*>& out);
  void collectStmtCallees(const Stmt* s, std::vector<Proc*>& out);
  void collectExprCallees(const Expr* e, std::vector<Proc*>& out);
  void collectInitItemCallees(const InitItem& item, std::vector<Proc*>& out);
  // Is `p` nested (directly or transitively) inside `anc`?
  bool isDescendantOf(Proc* p, Proc* anc);
  // Turn a parameter name list into by-reference Param symbols in `sc`, so the
  // same storage is shared when a name repeats (proc param vs ENTRY param).
  void resolveParams(Scope* sc, Proc* p, const std::vector<std::string>& names,
                     std::vector<Symbol*>& out);
  // Resolve a procedure's own parameters and its ENTRY statements' parameters
  // (rules (34),(56)); safe to call twice, the second call is a no-op.
  void resolveProcParams(Proc* p);
  void collectDecls(std::vector<StmtP>& body, Scope* sc, Proc* p, bool isStatic);
  void collectLabels(Stmt* s); // gather GO TO targets defined in this proc
  void checkStmt(Stmt* s, Scope* sc, Proc* p);
  // Rule (91): reject ON-unit constructs that need the establishing frame
  // (automatic-variable access, RETURN, nested ON, DECLARE, ENTRY). The unit
  // was already type-checked by checkStmt, so symbol references are resolved.
  void checkOnUnit(Stmt* u, Proc* p);
  // Extension (ADR-109): resolve PACKAGE EXPORTS against hoisted members,
  // marking listed member procedures external. Runs before pass 1.
  void resolvePackageExports();
  // Extension (ADR-109): a package contributes scope and linkage only —
  // diagnose package-level data and non-member statements in its body.
  void processPackage(Proc* p);
  // Rules (94),(99): resolve a condition to its dispatch key
  // (0 means ERROR, negative Stmt::k*CondKey are the fixed conditions,
  // else first-use order in prog_->condNames). A programmer-named name
  // already declared as a variable, parameter, or procedure is diagnosed.
  int resolveCondKey(Stmt* s, Scope* sc);
  // Validate the STRING ( reference ) stream option (rule 105): the target must
  // be a NONVARYING CHARACTER variable, and PAGE/SKIP are stream-only. A PUT
  // STRING formats into its target, so a VALUE constant is diagnosed there.
  void checkStringTarget(Stmt* s, Scope* sc, Proc* p, bool isGet);
  // Extension (ADR-108): diagnose a write to a VALUE named constant.
  void checkValueTarget(Expr* t);
  // Extension (ADR-114): register a DEFINE ALIAS type in scope.
  void defineAlias(Scope* sc, const std::string& name, const Type& ty, SourceLoc loc);
  // Resolve and validate the FILE ( f ) stream option (rule 105) and the
  // OPEN/CLOSE FILE ( f ): `f` must be a declared FILE variable. Resolves
  // s->fileIdent to s->fileSym.
  void checkFileTarget(Stmt* s, Scope* sc);
  // Validate edit-directed transmission (rule (108)): type the format widths,
  // and check that the data items pair one-to-one with the data (A/F) formats,
  // and for GET that each item is an assignable reference of a matching type.
  void checkEditFormats(Stmt* s, Scope* sc, Proc* p, bool isGet);
  void typeExpr(Expr* e, Scope* sc, Proc* p);
  // Compile-time SUBSCRIPTRANGE check for a constant subscript (rule 126).
  void checkSubscriptBounds(Expr* e, Symbol* arr);
  // Compile-time SUBSCRIPTRANGE check for a constant subscript against an
  // explicit bound list (rule 126); `name` is the array's diagnostic name.
  void checkSubscriptBoundsDims(Expr* e, const std::vector<Dim>& dims, const std::string& name);
  // Resolve a qualified reference S.A.B (rule 124) against a structure type,
  // recording the LLVM field index of each step in e->memberPath. Returns the
  // leaf member's type, or nullptr after reporting a diagnostic.
  const Type* resolveMemberPath(Expr* e, const Type& base);
  // Type a built-in function call (SUBSTR, INDEX, ABS, …). Returns true if
  // `e` is a known built-in (result typed or diagnosed); false otherwise, so
  // typeExpr can fall through to the general function-call path. Reductions
  // over array expressions defer to Assign expansion via pendingReduces_.
  bool typeBuiltin(Expr* e, Proc* p);
  // A SUM/PROD/ANY/ALL call over an array expression awaiting expansion in
  // direct assignment (rule (123)); anything left at processProc end was
  // never expanded and is diagnosed there.
  struct PendingReduce {
    Expr* call = nullptr;
    Proc* owner = nullptr;
  };
  std::vector<PendingReduce> pendingReduces_;
  // BASED base deferred to after all DECLAREs are collected (rule 25): a
  // BASED(P) may name a POINTER declared later in the same procedure, or a
  // procedure POINTER parameter with no DECLARE yet (which becomes POINTER).
  struct PendingBased {
    DeclItem* item = nullptr;
    Scope* sc = nullptr;
    Proc* proc = nullptr;
  };
  std::vector<PendingBased> pendingBased_;
  // Try to resolve one deferred BASED base; create a POINTER Var for a
  // parameter base with no DECLARE yet. Returns true when resolved.
  bool tryResolveBased(PendingBased& pb);
  // Resolve deferred BASED bases for one procedure (before its params
  // resolve, so a bare param base becomes POINTER, not implicit FLOAT).
  void resolvePendingBased(Proc* p);
  // Final retry for all procedures after every DECLARE is collected; errors
  // on anything still unresolvable.
  void flushPendingBased();
  // Drop a consumed or diagnosed reduction from the pending list.
  void dropPendingReduce(Expr* call);
  // Validate a reduction argument as an element-wise expression: every whole
  // reference outside calls has an identical static plain-storage shape
  // (set in shapeOut). Silent: false simply keeps the existing diagnostic.
  bool reduceArgShapeOk(Expr* e, Type& shape);
  // Expand pending reductions in an assignment value into a static temp per
  // call, then check the statement as a Group of the fills plus the original
  // (whose calls now take the bare temps). Returns true when expanded.
  bool expandReductionTemps(Stmt* s, Scope* sc, Proc* p);
  // Backstop (rule (123)): reductions over array expressions only expand in
  // direct assignment; diagnose anything left pending for this procedure.
  void drainPendingReduces(Proc* p);
  // By-value C entries (rule (34)): a POINTER parameter takes a pointer
  // argument, structure parameters stay diagnosed, and anything else rides
  // checkAssignable. No-op unless the entry carries
  // OPTIONS(LINKAGE(SYSTEM)/BYVALUE).
  void checkByValueArgs(Symbol* sym, const std::vector<ExprP>& args);
  // Expand whole-array and cross-section PUT/GET items into element subscript
  // calls in row-major order (rules (104)-(110)). Static plain-storage
  // shapes only; anything else is diagnosed with the position's rule.
  // New nodes are typed here; the caller re-validates. Returns false after
  // a diagnostic.
  bool expandAggregateItems(Stmt* s, Scope* sc, Proc* p, bool isGet);
  bool checkAssignable(const Type& dst, const Type& src, SourceLoc loc, const char* what);
  // Struct leaf type of a whole-structure reference (VarRef, top-level or
  // qualified path to a minor structure), or nullptr if it is not a structure.
  const Type* structLeafType(Expr* e);
  // BY NAME assignment (rule 86): validate that each member of `dst` has a
  // same-named, assignable member in `src` (recursively); names absent from
  // either side are skipped, so the layouts need not match.
  void checkByNameMatch(const Type& dst, const Type& src, SourceLoc loc);
  // Check an iterative DO group (rule 71): resolve the control variable,
  // type the bounds, then check the body with loop bookkeeping.
  void checkDoIter(Stmt* s, Scope* sc, Proc* p);
  // Plain whole-array storage a DO desugar can address (rule 86): a variable
  // with its own storage (static or dynamic bounds); parameters, DEFINED
  // overlays, BASED storage, and dynamic members are excluded.
  bool wholeArrayStorageOk(Expr* e);
  // True when an expression tree contains a whole-array reference that
  // element-wise rewriting would expand (references under a Call keep their
  // whole form and do not count); cross-section values always count.
  bool valueHasWholeArrayRef(Expr* e, bool underCall);
  // Rewrite whole-array VarRef uses to subscript calls over `idx` (all must
  // match `shape`); references under a Call keep their whole form, and a
  // nested cross-section is diagnosed. Returns false after a diagnostic.
  bool rewriteWholeArrayRefs(ExprP& e, const std::vector<std::string>& idx, const Type& shape,
                             bool underCall);
  // Whole-array expressions (rules 86, 127; QR2.1): rewrite `T = <array
  // expression>` into explicit DO loops over the target's axes. Returns
  // true when rewritten (the caller then checks it as a DO group), false
  // after diagnosing an unsupported shape or storage combination.
  bool expandWholeArrayAssign(Stmt* s, Scope* sc, Proc* p);
  // Fold an INITIAL constant expression (rule 26) to a literal, negating a
  // leading unary minus; returns the folded literal, or nullptr after a
  // diagnostic when it is not a literal or not assignable to `ty`.
  Expr* foldInitialConstant(Expr* e, const Type& ty, SourceLoc loc);
  // Expand an INITIAL itemlist (rules (28)-(31)) into a flat sequence of folded
  // element values, expanding iteration factors and resolving '*'.
  void expandInitItems(const std::vector<InitItem>& items, const Type& elemTy, SourceLoc loc,
                       std::vector<Expr*>& out);
  // Total element count of an array type: the product of (ub - lb + 1) (rule 12).
  long long elementCount(const Type& ty);
  // Number of scalar leaf values a structure's INITIAL list must supply (rule
  // (26)): sum over members — a scalar counts 1, a nested structure recurses,
  // an array member counts its element count (an array of structures counts the
  // leaves of each element).
  long long structureLeafCount(const Type& ty);
  // Flatten an INITIAL itemlist into raw (unfolded) values, expanding iteration
  // factors and '*' but not folding, so a heterogeneous structure can fold each
  // value against its own member type later.
  void flattenInitItems(const std::vector<InitItem>& items, SourceLoc loc, std::vector<Expr*>& out);
  // Fold raw INITIAL values against a structure's scalar leaves in order into
  // `out` (an index into `vals` advanced as leaves are consumed).
  void foldStructInit(const Type& ty, const std::vector<Expr*>& vals, size_t& idx, SourceLoc loc,
                      std::vector<Expr*>& out);

  Diags& d_;
  Program* prog_ = nullptr;
  std::vector<std::unique_ptr<Symbol>> owned_;
  std::vector<std::unique_ptr<Scope>> scopes_;
  std::unordered_map<Proc*, Scope*> procScopes_;
  Scope* rootScope_ = nullptr; // program scope: all external procedure names
  std::vector<Symbol*> storage_;
  std::vector<Symbol*> entries_;     // external C entries, in declaration order
  int nextFileSlot_ = 0;             // next FILE variable slot index (100-103)
  int nextCtlSlot_ = 0;              // next CONTROLLED variable slot index (87-90)
  std::set<std::string> procLabels_; // GO TO targets in the current proc (rule 77)
  Stmt* curEntry_ = nullptr;         // the ENTRY segment currently being checked
                                     // (rule 56): enables RETURN(value) in its body
  bool inUnit_ = false;              // true while checking an ON-unit body (rule 91):
                                     // a bare RETURN there ends the unit, never the function
  std::vector<std::vector<std::string>> loopStack_; // labels of enclosing iterative
                                                    // DO-groups, innermost last (ADR-105)
  std::set<std::string> irNames_;                   // irNames in use, to disambiguate shadowing
  std::unordered_map<Stmt*, Scope*> beginScopes_;   // BEGIN block -> its scope
  bool declsCollected_ = false; // true once the pass-0 collectDecls pre-pass ran
};

// Arithmetic result type per the conversion rules (M0 approximation).
Type arithResultType(const Type& a, const Type& b);
