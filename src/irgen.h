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
//   complex : `cpx` holds an {double,double} struct of the real/imaginary parts
struct Val {
  Type ty{};
  llvm::Value* reg = nullptr;
  llvm::Value* ptr = nullptr;
  llvm::Value* len = nullptr;
  llvm::Value* cpx = nullptr;
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
  // For a dynamic (runtime-extent, rule (13)) array symbol: the runtime upper
  // bound value of its dynamic axis, loaded once at block entry.
  std::unordered_map<Symbol*, llvm::Value*> dynUb_;
  // The runtime lower bound value of a dynamic lower bound (rule (13)), the
  // mirror of `dynUb_`, loaded once at block entry.
  std::unordered_map<Symbol*, llvm::Value*> dynLb_;
  // Dynamic array member (rule 13) runtime bound values, keyed by (structure
  // symbol, member field path), recorded once at block entry.
  struct MemberDyn {
    Symbol* sym;
    std::vector<unsigned> path;
    bool operator==(const MemberDyn& o) const { return sym == o.sym && path == o.path; }
    struct Hash {
      size_t operator()(const MemberDyn& k) const {
        size_t h = std::hash<Symbol*>{}(k.sym);
        for (unsigned u : k.path)
          h = h * 31 + u;
        return h;
      }
    };
  };
  struct MemberBounds {
    llvm::Value* ub = nullptr;
    llvm::Value* lb = nullptr;
  };
  std::unordered_map<MemberDyn, MemberBounds, MemberDyn::Hash> memberDyn_;
  void emitGlobals();
  void declareProc(HProc* p); // pre-create a proc's functions/aliases so calls resolve
  void emitProc(HProc* p);
  void emitPlainProc(HProc* p, llvm::Type* retLLVM);
  void emitMultiEntryProc(HProc* p, const std::vector<HStmt*>& entries, llvm::Type* retLLVM);
  void allocaLocals(HProc* p);
  void emitInitials(HProc* p); // INITIAL stores on AUTOMATIC vars (rule 26)
  // Record the runtime upper bound of each dynamic (runtime-extent) array
  // parameter at entry (rules (12),(13),(34)-(38)): a parameter like `x(k)` is
  // a by-reference pointer with no own storage, so its extent must be read from
  // the bound argument (itself by-ref) once, mirroring allocaLocals' locals.
  void recordDynParamUbs(const std::vector<Symbol*>& params);
  void collectGotoBlocks(HStmt* s); // assign an LLVM block to each labelled stmt
  // rule (56): LLVM function name for an ENTRY statement's alternate entry point.
  static std::string entryIrName(const std::string& proc, const std::string& parent,
                                 const std::string& entry);
  // ON ERROR handlers (rules (91)-(94)): assign dense handler ids (1-based;
  // 0 means SYSTEM), pre-create the handler functions, and fill their bodies.
  void assignOnIds(HProgram& prog);
  void declareOnHandlers(HProgram& prog);
  void emitOnHandlers(HProgram& prog);
  void emitOn(HStmt* s);     // establish a handler (or SYSTEM)
  void emitSignal(HStmt* s); // raise ERROR: dispatch or take the system action

  // --- statements & expressions --------------------------------------
  void emitStmt(HStmt* s);
  void emitAssign(HStmt* s);
  void emitIf(HStmt* s);
  void emitDoWhile(HStmt* s);
  void emitDoIter(HStmt* s);
  void emitPut(HStmt* s);
  void emitGet(HStmt* s); // GET (rules 104-109), list-directed input
  // Edit-directed transmission (rule (108)): walk the format list, pairing each
  // data item with its A/F format and emitting the control (X/SKIP/PAGE/LINE)
  // items in order.
  void emitPutEditItems(HStmt* s);
  void emitGetEditItems(HStmt* s);
  // Store a list-directed input value into a data-list reference (the same
  // target-addressing as an assignment's left-hand side).
  void storeGetTarget(HExpr* t, const Val& v, SourceLoc loc);
  void emitCall(HStmt* s);
  void emitAllocate(HStmt* s); // ALLOCATE (rule 87)
  void emitFree(HStmt* s);     // FREE (rule 90)
  void emitOpen(HStmt* s);     // OPEN (rules 100,101)
  void emitClose(HStmt* s);    // CLOSE (rules 102,103)

  // Address of one call argument for a by-reference parameter (rule 4): a
  // direct variable of the same type passes its own address; anything else is
  // copied into a fresh dummy argument. Shared by emitCall and emitExpr so the
  // marshalling logic has a single home.
  llvm::Value* argAddr(HExpr* a, const Type& pty);
  // Append the callee's static-link arguments (its enclosing automatic
  // variables, rule 8). Shared by emitCall and emitExpr.
  void appendStaticLinks(Proc* callee, std::vector<llvm::Value*>& args);
  // True when a parameter is a `*`-adjustable-extent array (rule (13)); such a
  // parameter carries a hidden i64 extent argument from the caller.
  bool isAdjustable(Symbol* s) {
    return s->ty.isArray() && !s->ty.dims.empty() && s->ty.dims[0].adj;
  }
  // Number of `*`-adjustable-extent parameters in `params` (each contributes a
  // hidden i64 extent argument after the by-reference pointers).
  size_t nAdjustable(const std::vector<Symbol*>& params) {
    size_t n = 0;
    for (Symbol* s : params)
      if (isAdjustable(s))
        ++n;
    return n;
  }
  // Element count of a call argument passed to a `*`-extent parameter: constant
  // for a fixed array, the recorded live bound for a dynamic-bound array.
  llvm::Value* argExtent(HExpr* a);

  Val emitExpr(HExpr* e);
  // Number of elements across all axes: the product of (ub - lb + 1) (rule (12)).
  long long arrayExtent(const Type& arr);
  // Address of one array element A(i,j,...) (rule 126), after a runtime bounds
  // check on each axis. `arr` is the array type (bounds + element), `base` the
  // address of the array storage; `idxs` holds one index per axis. For a
  // dynamic (runtime-extent) array, `dynUb` supplies the runtime upper bound of
  // the (single, 1-D) dynamic axis, `dynLb` the runtime lower bound, and `base`
  // is a bare element pointer.
  llvm::Value* arrayElementAddr(const Type& arr, llvm::Value* base, const std::vector<HExprP>& idxs,
                                SourceLoc loc, llvm::Value* dynUb = nullptr,
                                llvm::Value* dynLb = nullptr);
  // Address of a qualified member S.A.B (rule 124): a GEP off the structure
  // base through the recorded LLVM field indices (relative to each nested
  // struct), loading the leaf member's scalar value.
  llvm::Value* memberAddr(Symbol* base, const std::vector<unsigned>& path, SourceLoc loc);
  // Address of a member of one structure element of an array of structures
  // (rule 124): like memberAddr, but GEPs from a caller-supplied element address
  // through the recorded field indices against the element structure type.
  llvm::Value* elementMemberAddr(Symbol* base, const std::vector<unsigned>& path,
                                 llvm::Value* elemAddr);
  // Address of a member of a locator-qualified reference P->X.FIELD (rule 124):
  // like memberAddr, but GEPs from a caller-supplied base address (the loaded
  // pointer value) through the field indices against the based structure type.
  llvm::Value* locatorMemberAddr(Symbol* base, const std::vector<unsigned>& path,
                                 llvm::Value* baseAddr);
  // The resolved type of a qualified member S.A.B (rule 124): walk the recorded
  // field indices to recover the leaf member's type (an array member's array
  // type), mirroring memberAddr without emitting GEPs.
  const Type& memberType(Symbol* base, const std::vector<unsigned>& path);
  // Buffer pointer and live bounds of a dynamic-array structure member (rule
  // 13): load the runtime-sized buffer pointer held in the struct field, and
  // return the bounds recorded at entry. Caller passes a dynamic-array member
  // path; the returned base is a bare element pointer.
  llvm::Value* dynamicMemberBase(Symbol* base, const std::vector<unsigned>& path, SourceLoc loc,
                                 llvm::Value*& ub, llvm::Value*& lb);
  // BY NAME assignment (rule 86): copy each same-named member of `dst` from
  // `src` at their struct bases, recursing into minor structures; names absent
  // from either side are skipped.
  void emitByNameCopy(llvm::Value* dstBase, llvm::Value* srcBase, const Type& dst, const Type& src,
                      SourceLoc loc);
  // Store a structure's INITIAL element list (rule (26)) into its storage,
  // walking members in declaration order and storing each value to the matching
  // scalar leaf (recursing into nested structures and array members). `idx` is
  // the position in `vals` and advances as leaves are consumed.
  void emitStructInitValues(llvm::Value* base, const Type& ty, const std::vector<Expr*>& vals,
                            size_t& idx, SourceLoc loc);
  // Emit a built-in function call (SUBSTR, INDEX, ABS, …). Returns true if
  // `e` was a recognised built-in; false otherwise, so emitExpr can fall
  // through to the general function-call path.
  bool emitBuiltin(HExpr* e, Val& out);
  Val loadSym(Symbol* sym, const Type& ty);
  void storeTo(Symbol* sym, const Val& v, SourceLoc loc);
  void storeScalarTo(llvm::Value* addr, const Type& ty, const Val& v);
  // An LLVM scalar constant for an INITIAL element value (rule 26), or null for
  // types without a constant form (e.g. STRUCT).
  llvm::Constant* scalarInitConstant(const Type& ty, const Expr* ini);
  // A materialised scalar value for an INITIAL element (rule 26), for storing
  // into an AUTOMATIC array element.
  Val initValue(const Type& ty, const Expr* e);
  // Load / store one array element (rule 126), with the SUBSCRIPTRANGE check.
  Val loadArrayElement(Symbol* sym, const std::vector<HExprP>& idxs, SourceLoc loc);
  void storeArrayElement(Symbol* sym, const std::vector<HExprP>& idxs, const Val& src,
                         SourceLoc loc);
  // Copy an N-star cross-section A(i, *, ...) (rule 126) into a whole array
  // target: iterate the target's linear index, mapping each star-axis
  // coordinate back to a source element and gathering it into the target.
  void emitCrossSectionAssign(HExpr* target, HExpr* cross, SourceLoc loc);
  // Address of a DEFINED scalar element overlay (rule 24): the base element at
  // the constant subscripts, computed once (sema bounds-checked them).
  llvm::Value* definedConstAddr(Symbol* sym);
  // Address of a subscripted iSUB-DEFINED array element Y(k) (rules 134,126):
  // the base X element at the fixed subscripts with the iSUB slot set to k.
  llvm::Value* definedSubElementAddr(Symbol* y, const std::vector<HExprP>& idxs, SourceLoc loc);

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
  Type curRetTy_; // result type of the function currently being emitted (the impl's
                  // common entry type for a multi-entry procedure, rule (56))
  // The hidden result pointer of a structure-valued function (rule 127): the
  // caller-supplied buffer that a RETURN(struct) copies into before returning.
  llvm::Value* structRetPtr_ = nullptr;
  std::map<std::string, llvm::BasicBlock*> labelBlocks_; // label -> block (rule 77)
  // ON ERROR state (rules (91)-(94)): handler id -> function (id 1-based).
  std::vector<llvm::Function*> onHandlers_;
  // Procedure-entry ERROR depth slot for exit restore; null when the module
  // establishes no handlers (the common path stays free) or inside a handler.
  llvm::AllocaInst* curOnDepth_ = nullptr;
  // True while filling an ON-unit handler: calls needing static links are
  // diagnosed (the handler has no establishing frame).
  bool inHandler_ = false;
};
