// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM
// irgen.h — LLVM IR generation.
//
// M1 generates IR with llvm::IRBuilder<> (ADR-002); `run` prints the built
// llvm::Module to textual IR so the driver and the Makefile/clang pipeline are
// unchanged. Everything the rest of the compiler sees is the IRGen/Val
// interface below; parser and sema are untouched.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
  IRGen(Diags& d, Sema& s, std::string triple, bool noSizeChecks = false,
        std::string runtimeBc = "", bool linkBitcode = false)
      : d_(d), sema_(s), triple_(std::move(triple)), mod_("plic", ctx_), b_(ctx_),
        noSizeChecks_(noSizeChecks), runtimeBc_(std::move(runtimeBc)), linkBitcode_(linkBitcode) {}

  std::string run(HProgram& prog);

  // Module ownership for the in-process backend (review §4): reuses run()
  // (so `-emit-llvm` stays byte-identical) then re-parses the IR into a fresh
  // context the caller owns. Null on any diagnostic failure.
  struct OwnedModule {
    std::unique_ptr<llvm::LLVMContext> ctx;
    std::unique_ptr<llvm::Module> mod;
  };
  std::unique_ptr<OwnedModule> takeModule(HProgram& prog);

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
  // For an adjustable-length `CHAR(*)` parameter (rule (18)): the live length,
  // read from the caller-supplied hidden i64 length argument once at entry.
  // The mirror of `dynUb_` for character parameters.
  std::unordered_map<Symbol*, llvm::Value*> dynLen_;
  // For a CONTROLLED `CHAR(*)` dummy (rules (15),(18)): the effective
  // generation-stack key, selected from the hidden i64 slot-key argument at
  // entry (-1 = fresh stack, else the actual's key). The mirror of `dynLen_`
  // for CONTROLLED parameters; every pli_ctl_* call uses it via ctlKeyOf.
  std::unordered_map<Symbol*, llvm::Value*> ctlKey_;
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
  // Folded constant INITIAL/VALUE on a Fixed/Float scalar (rules (26), ADR-108):
  // true + `out` when `s` qualifies. Stored at alloca time so later entry code
  // (dynamic bounds) loads the value even without optimization.
  bool constScalarInit(Symbol* s, Val& out);
  // Record the runtime upper bound of each dynamic (runtime-extent) array
  // parameter at entry (rules (12),(13),(34)-(38)): a parameter like `x(k)` is
  // a by-reference pointer with no own storage, so its extent must be read from
  // the bound argument (itself by-ref) once, mirroring allocaLocals' locals.
  void recordDynParamUbs(const std::vector<Symbol*>& params);
  void collectGotoBlocks(HStmt* s,
                         std::vector<int>& chain); // assign an LLVM block to each labelled stmt
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
  // Data-directed output (rule (106)): each item as NAME=value, ", "-separated
  // and ";"-terminated; names are compile-time globals, values reuse the
  // list-directed printers.
  void emitPutDataItems(HStmt* s);
  // Data-directed input (rule (106)): read NAME=value pairs in any order,
  // storing each into the matching item and skipping unknown names.
  void emitGetDataItems(HStmt* s);
  // Store a list-directed input value into a data-list reference (the same
  // target-addressing as an assignment's left-hand side).
  void storeGetTarget(HExpr* t, const Val& v, SourceLoc loc);
  void emitCall(HStmt* s);
  // Asynchronous CALL (rule (79), QR2.8): pack the callee arguments into a
  // heap context, spawn a detached thread running a per-site wrapper, and
  // return at once. Shared by emitCall when a task option is present.
  void emitAsyncCall(HStmt* s);
  void emitWait(HStmt* s); // WAIT (rule (82), QR2.8)
  // Address of a task/event operand (rule (78),(81)): accepts a scalar,
  // array-element (ev(i)) or member reference, covering CONTROLLED/dynamic
  // storage for the latter two. Returns a pointer to the underlying flag.
  llvm::Value* taskEventAddr(HExpr* e, SourceLoc loc);
  void emitDelay(HStmt* s);       // DELAY (rule (83), QR2.8)
  void emitAllocate(HStmt* s);    // ALLOCATE (rule 87)
  void emitFree(HStmt* s);        // FREE (rule 90)
  void emitOpen(HStmt* s);        // OPEN (rules 100,101)
  void emitClose(HStmt* s);       // CLOSE (rules 102,103)
  void emitRecordRead(HStmt* s);  // READ (rules (112),(113)): one binary record in
  void emitRecordWrite(HStmt* s); // WRITE (rules (112),(113)): one binary record out

  // Address of one call argument for a by-reference parameter (rule 4): a
  // direct variable of the same type passes its own address; anything else is
  // copied into a fresh dummy argument. Shared by emitCall and emitExpr so the
  // marshalling logic has a single home.
  llvm::Value* argAddr(HExpr* a, const Type& pty);
  // A CHARACTER VARYING argument of a different declared length than the
  // parameter is marshalled into a parameter-sized dummy (see argAddr); after
  // the call the callee's result is copied back into the caller's variable,
  // clamped to the caller's own capacity. Each entry records what to copy
  // back; flushVarWrites emits the copies right after a synchronous call.
  struct PendingVarWrite {
    llvm::Value* dummy = nullptr; // the parameter-typed dummy alloca
    Symbol* sym = nullptr;        // the caller's varying-char variable
    Type pty;                     // the parameter (dummy) type, for layout
  };
  std::vector<PendingVarWrite> pendingVarWrites_;
  void flushVarWrites();
  // Marshal one argument for a by-value C entry (rules (34),(38)): FIXED and
  // FLOAT scalars convert to the parameter type and ride as values, POINTERs
  // ride as the pointer itself; anything else keeps the argAddr form. A
  // POINTER parameter given a non-pointer is diagnosed (sema pins the common
  // case; this is the backstop).
  llvm::Value* marshalArg(HExpr* a, const Type& pty, SourceLoc loc);
  // VARYINGZ argument for a by-value C entry (ADR-168): materialise the
  // actual's current value into a fresh NUL-terminated buffer and return its
  // address, so the C callee sees a `char *` with no explicit length.
  llvm::Value* cstrArg(HExpr* a, const Type& pty, SourceLoc loc);
  // Append the callee's static-link arguments (its enclosing automatic
  // variables, rule 8). Shared by emitCall and emitExpr.
  void appendStaticLinks(Proc* callee, std::vector<llvm::Value*>& args);
  // True when a parameter is a `*`-adjustable-extent array (rule (13)) or an
  // adjustable-length `CHAR(*)` (rule (18)); such a parameter carries a hidden
  // i64 extent/length argument from the caller.
  bool isAdjustable(Symbol* s) {
    return (s->ty.isArray() && !s->ty.dims.empty() && s->ty.dims[0].adj) ||
           (s->ty.isChar() && s->ty.starLen);
  }
  // True when a parameter is an adjustable-length `CHAR(*)` (rule (18)); its
  // hidden argument is a character length, not an array extent.
  static bool isStarLen(const Type& t) { return t.isChar() && t.starLen; }
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
  // Buffer capacity of a call argument passed to an adjustable-length `CHAR(*)`
  // parameter (rule (18)): a fixed char variable's declared length, a forwarded
  // `CHAR(*)` parameter's live length, or the emitted value's length.
  llvm::Value* argLen(HExpr* a, bool ptyVarying);
  // Live capacity of an adjustable-length `CHAR(*)` value (rule (18)): the
  // hidden length for a parameter, the current generation size for a
  // CONTROLLED variable (rule (15)). Returns nullptr when the type is not an
  // adjustable character (fixed/VARYING keep their static lengths).
  llvm::Value* adjustLen(Symbol* sym, const Type& ty);
  // Effective generation-stack key of a CONTROLLED variable (rules (15),(87)):
  // the key bound at entry for a controlled `CHAR(*)` dummy (an alias of the
  // actual's stack), else the symbol's own deterministic slot.
  llvm::Value* ctlKeyOf(Symbol* sym);
  // Hidden i64 for a controlled `CHAR(*)` dummy (rules (15),(18)): the
  // actual's generation-stack key, or -1 when the actual is not controlled
  // (the dummy then gets a stack of its own).
  llvm::Value* ctlKeyArg(HExpr* a);
  // The hidden i64 one adjustable parameter receives at a call site (rules
  // (13),(15),(18)): the actual's slot key for a CONTROLLED `CHAR(*)` dummy,
  // zero for an omitted `*`/trailing OPTIONAL, else the actual's array extent
  // or character buffer length. `a` is null for an omitted argument.
  llvm::Value* hiddenAdjustArg(HExpr* a, const Type& ty, bool ctl);

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
                                SourceLoc, llvm::Value* dynUb = nullptr,
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
  // from either side are skipped. The symbol/root-path pairs address dynamic
  // member buffers (rule (13), ADR-093); empty when the base is not a plain
  // variable (then dynamic members are diagnosed, never pointer-copied).
  void emitByNameCopy(llvm::Value* dstBase, llvm::Value* srcBase, const Type& dst, const Type& src,
                      SourceLoc loc, Symbol* dstSym, const std::vector<unsigned>& dstPrefix,
                      Symbol* srcSym, const std::vector<unsigned>& srcPrefix);
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
  // `packed` marks a destination inside an UNALIGNED structure: the store then
  // uses align 1 rather than the value type's natural alignment (ADR-169).
  void storeScalarTo(llvm::Value* addr, const Type& ty, const Val& v, bool packed = false);
  // Copy a character value into a buffer (rules (34),(37),(86)): blank-pad or
  // truncate to the destination length, setting the live length for VARYING.
  // `packed` as for storeScalarTo (the VARYING length word is the aligned part).
  void storeCharTo(llvm::Value* addr, const Type& dst, const Val& v, SourceLoc loc,
                   bool packed = false);
  // An LLVM scalar constant for an INITIAL element value (rule 26), or null for
  // types without a constant form (e.g. STRUCT).
  llvm::Constant* scalarInitConstant(const Type& ty, const Expr* ini);
  // A materialised scalar value for an INITIAL element (rule 26), for storing
  // into an AUTOMATIC array element.
  Val initValue(const Type& ty, const Expr* e);
  // Load / store one array element (rule 126), with the SUBSCRIPTRANGE check.
  // A locator-qualified subscript P->X(i) (rules 124,126) addresses off the
  // locator value instead of the declared BASED pointer; null means the own
  // base. CONTROLLED dynamic arrays never carry a locator (rule 124 rejects
  // non-BASED targets in sema).
  Val loadArrayElement(Symbol* sym, const std::vector<HExprP>& idxs, SourceLoc loc,
                       HExpr* locPtr = nullptr);
  void storeArrayElement(Symbol* sym, const std::vector<HExprP>& idxs, const Val& src,
                         SourceLoc loc, HExpr* locPtr = nullptr);
  // A CONTROLLED dynamic numeric array (rules (13),(89)): a scalar
  // `DCL FDS FIXED CONTROLLED` or a 1-D `(*)` array whose extent is the top
  // generation size. True for the scalar form and for a dynamic array form.
  static bool isCtlDynArray(Symbol* sym);
  // Live upper bound of a CONTROLLED dynamic array (rule (126)): lb is 1, ub
  // is `pli_ctl_len(key) / elemSize` so LIFO generations report their own size.
  llvm::Value* ctlDynBound(Symbol* sym);
  // Live lower bound: the DECLARE lb for a fixed 1-D array, else 1.
  static long long ctlDynLb(Symbol* sym);
  // Address of one CONTROLLED dynamic array element (rule 126) with the
  // SUBSCRIPTRANGE check against the live bound.
  llvm::Value* ctlDynElementAddr(Symbol* sym, const std::vector<HExprP>& idxs, SourceLoc loc);
  // An N-D CONTROLLED numeric array (rules (13),(89), IBM asterisk notation):
  // rank >= 2 with numeric/BIT elements. Every generation carries per-axis
  // live extents (pli_ctl_rank/pli_ctl_extent); lower bounds are static (the
  // DECLARE lb, else 1). True for fixed N-D and `(*)`/mixed N-D forms alike,
  // so bare and overriding ALLOCATEs share one live-bounds path.
  static bool isCtlNDynArray(Symbol* sym);
  // Static lower bound of one N-D axis: the DECLARE lb for a fixed axis,
  // else 1 for a `(*)` axis.
  static long long ctlNDLb(Symbol* sym, size_t axis);
  // Live extent of one N-D axis (rule (126)): pli_ctl_extent(key, axis).
  llvm::Value* ctlNDExtent(Symbol* sym, size_t axis);
  // Address of one N-D CONTROLLED element (rule 126): per-axis live
  // SUBSCRIPTRANGE checks, then a row-major GEP off the top generation base.
  llvm::Value* ctlNDElementAddr(Symbol* sym, const std::vector<HExprP>& idxs, SourceLoc loc);
  // Push the DECLARE extents of a fixed N-D CONTROLLED array as the new top
  // generation's live extents (bare ALLOCATE and implicit allocation).
  void emitCtlNDDescDims(Symbol* sym);
  // Copy a slice (rule 126) between two array views; either side may be a
  // cross-section A(i, *, ...), a whole array being the all-'*' case. Walks the
  // reduced rank's linear index and maps each star-axis coordinate to both
  // views; a scalar value broadcasts over the target slice.
  void emitCrossSectionAssign(HExpr* target, HExpr* value, SourceLoc loc);
  // Address of a DEFINED scalar element overlay (rule 24): the base element at
  // the constant subscripts, computed once (sema bounds-checked them).
  llvm::Value* definedConstAddr(Symbol* sym);
  // Address of a subscripted iSUB-DEFINED array element Y(k) (rules 134,126):
  // the base X element at the fixed subscripts with the iSUB slot set to k.
  llvm::Value* definedSubElementAddr(Symbol* y, const std::vector<HExprP>& idxs, SourceLoc);

  Val convert(const Val& v, const Type& dst, SourceLoc loc);
  llvm::Value* toI1(const Val& v, SourceLoc loc);
  llvm::Value* toI64(const Val& v, SourceLoc loc = SourceLoc{});
  // Packed BIT(n) layout (rule (18), QR2.2): big-endian bytes, the first
  // character in the top bit of byte 0, unused low bits of the last byte
  // zero. Bytes of a BIT(1); [ceil(n/8) x i8] above.
  static int bitBytes(int nbits) { return (nbits + 7) / 8; }
  // Pack a [B x i8] aggregate (nbits <= 64) big-endian into an i64; anything
  // wider is diagnosed, never silently truncated.
  llvm::Value* packBitValue(llvm::Value* agg, int nbits, SourceLoc loc);
  // Unpack an integer's low nbits big-endian into a [B x i8] aggregate.
  llvm::Value* unpackBitValue(llvm::Value* intval, int nbits, SourceLoc loc);
  // Pack a bit-string literal (characters '0'/'1') into bytes, padding and
  // truncating on the right to nbits.
  static std::vector<unsigned char> packBitLiteral(const std::string& sval, int nbits);
  // Bytes for an INITIAL value: a bit literal packs directly, anything else
  // contributes its integer value low-bits-first big-endian (zero when absent).
  static std::vector<unsigned char> packBitInit(const Expr* ini, int nbits);
  Val charTemp(int len); // alloca [len x i8]
  Val charOf(HExpr* e);  // materialise a character value
  // Hidden result buffer for a character function (rules (34),(37)): a
  // `CHAR(*) VARYING` result (rule (18)) gets a caller-sized max
  // (kStarRetMax) so the callee never writes past the buffer; other results
  // use their static descriptor size.
  static constexpr long long kStarRetMax = 32767;
  llvm::Type* sretBufTy(const Type& rty);
  llvm::Value* sretAlloc(const Type& rty, const char* name = "sret");
  // FIXED BINARY checked +,-,* (QR1.2): overflow traps to the SIZE path
  // (hard ERROR when no SIZE handler is established, QR1.4).
  llvm::Value* checkedArith(Tok op, llvm::Value* a, llvm::Value* b);
  // FIXED DECIMAL precision trap (QR1.2): an i64 magnitude at or beyond
  // `limit` (10^prec digits, or a binary width) traps to the SIZE path.
  void magTrap(llvm::Value* v, long long limit);
  // FLOAT -> FIXED range trap (QR1.2): FPToSI outside the destination range
  // is UB, so the float is checked first (ordered compares, so NaN traps).
  // `loIncl` selects the closed lower bound (exact INT_MIN stays storable).
  void floatRangeTrap(llvm::Value* f, double lo, bool loIncl, double hi);
  // SIZE dispatch (QR1.4, rules (91)-(94)): emit the body of an overflow
  // trap block. With no ON SIZE in the module this is the unconditional
  // hard-ERROR call; otherwise check the SIZE stack — empty resumes the
  // abort path, established runs the top handler then branches to okBB.
  // Resumes with the wrapped value; ONCODE stays ERROR-only in this stage.
  void emitSizeTrap(llvm::BasicBlock* okBB);
  // Computational-condition trap (rules (91)-(94)): the shared shape behind
  // emitSizeTrap. Without an ON unit for `key` in the module emit the
  // unconditional `abortFn` call; otherwise consult the stack — empty takes
  // the abort path, established runs the matching handler then branches to
  // okBB, where the caller resumes with its own recovery value.
  void emitCondTrap(int key, const std::string& abortFn, const std::string& tag,
                    llvm::BasicBlock* okBB);
  // Clamp an index into [lb, ub] (SUBSCRIPTRANGE resume value, rule 94).
  llvm::Value* clampIndex(llvm::Value* i, llvm::Value* lb, llvm::Value* ub);
  // ZERODIVIDE value trap (rule 94): branch on `isZero`; the trap block
  // routes through the ZERODIVIDE dispatch (abort when unhandled) and
  // rejoins, resuming with `zero` instead of `computed`.
  llvm::Value* zerodivideResume(llvm::Value* isZero, llvm::Value* computed, llvm::Value* zero);
  int ovSeq_ = 0; // disambiguates overflow trap blocks within a function

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
  // LLVM function type of a call through an entry variable/parameter (IBM
  // ENTRY VARIABLE extension, ADR-171), built from its declared descriptor.
  // No static-link arguments: a captured procedure is diagnosed at assignment.
  llvm::FunctionType* entryFnType(Symbol* sym);
  // An indirect call through an entry variable (ADR-171): load the stored
  // function pointer and call it with the declared signature. Shared by the
  // CALL statement (result discarded) and a function reference; `sretPtr`
  // receives the hidden result buffer when the entry returns a structure or
  // character (diagnosed in sema in this slice).
  llvm::CallInst* emitEntryCall(Symbol* sym, std::vector<HExprP>& args, SourceLoc loc,
                                llvm::Value** sretPtr);
  // Implicit ALLOCATE for CONTROLLED variables (IBM Enterprise PL/I): push a
  // generation sized to the compile-time descriptor if none exists yet. Skips
  // CHAR(*) entirely — those require an explicit ALLOCATE with a known size.
  void ensureCtlAlloc(Symbol* sym, SourceLoc loc = SourceLoc{});
  // Push a CONTROLLED generation sized from the DECLARE runtime bounds (rules
  // (13),(89)): a non-`*` runtime upper bound `A(n)` is evaluated at the point
  // of allocation and recorded as the generation's live per-axis extent.
  void ctlAllocDeclared(Symbol* sym, SourceLoc loc);
  // True when a CONTROLLED array has a non-`*` runtime DECLARE upper bound
  // (sized per generation) rather than an ALLOCATE-supplied `*` axis.
  static bool ctlDeclaredRuntimeBound(Symbol* sym);
  // INITIAL on a CONTROLLED generation (rules (15),(26), SC26-3114): assign
  // the declared initial value into the just-allocated generation. Assumes
  // the generation exists (just pushed); no-INITIAL symbols are a no-op.
  void emitCtlInit(Symbol* sym, SourceLoc loc = SourceLoc{});
  // Package CONTROLLED generations (extension, ADR-109): program-lifetime
  // storage shared by member procedures. Ensure one generation exists (alloc
  // + INITIAL on first entry, reuse after) without ever popping per proc.
  void emitPackageCtlEnsure();
  // Implicit FREE for CONTROLLED variables at procedure exit: pop exactly one
  // generation for each symbol this proc implicitly allocated, leaving any
  // explicitly-ALLOCATEd generations above/below untouched.
  void emitCtlEpilogue();

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
  // The hidden result pointer of a structure- or character-valued function
  // (rules 127 and (34),(37)): the caller-supplied buffer that a RETURN
  // copies into before returning void.
  llvm::Value* structRetPtr_ = nullptr;
  std::map<std::string, llvm::BasicBlock*> labelBlocks_; // label -> block (rule 77)
  // ON-unit scope on block termination (rule (91); Y33 "Activation and
  // Termination of Blocks"): a GO TO that leaves one or more BEGIN blocks
  // reverts the handlers those blocks established. Each BEGIN carries a unique
  // scope id; `onScopes_` is the emission-time chain of active blocks with the
  // handler depth saved at their entry, and `labelBeginChain_` records the
  // chain enclosing each label. `onExitDepth` returns the depth to restore at
  // a GO TO (the outermost exited block's entry depth), or null when no block
  // is left.
  struct OnScope {
    int id = 0;
    llvm::Value* entryDepth = nullptr;
  };
  std::vector<OnScope> onScopes_;
  std::map<std::string, std::vector<int>> labelBeginChain_;
  int nextOnScope_ = 0;
  llvm::Value* onExitDepth(const std::string& label);
  // Enclosing iterative groups for LEAVE/ITERATE (extension, ADR-105):
  // labels with the break (end) and continue (re-entry) blocks, innermost last.
  struct LoopTargets {
    std::vector<std::string> labels;
    llvm::BasicBlock* breakBB = nullptr;
    llvm::BasicBlock* contBB = nullptr;
  };
  std::vector<LoopTargets> loopStack_;
  // Condition enable-state (rules (60)-(63), ADR-110/112): effective
  // disables per statement, OR-inherited through compound statements so a
  // prefixed group covers its body. The trap sites read the top.
  struct CheckState {
    bool noSize = false;
    bool noSub = false;
    bool noZdiv = false;
  };
  std::vector<CheckState> checkStack_;
  // Global --no-size-checks (ADR-111) disables every SIZE trap, including
  // ones an ON SIZE handler would otherwise route.
  bool noSizeChecks_ = false;
  bool sizeChecks() const {
    return !noSizeChecks_ && (checkStack_.empty() || !checkStack_.back().noSize);
  }
  bool subChecks() const { return checkStack_.empty() || !checkStack_.back().noSub; }
  bool zdivChecks() const { return checkStack_.empty() || !checkStack_.back().noZdiv; }
  // ON state (rules (91)-(94),(99)): condition key (0 = ERROR,
  // Stmt::kSizeCondKey = SIZE, else a rule (99) name) to handler
  // functions, ids dense from 1 within a key.
  std::map<int, std::vector<llvm::Function*>> onHandlers_;
  // Procedure-entry ERROR depth slot for exit restore; null when the module
  // establishes no handlers (the common path stays free) or inside a handler.
  llvm::AllocaInst* curOnDepth_ = nullptr;
  // True while filling an ON-unit handler: calls needing static links are
  // diagnosed (the handler has no establishing frame).
  bool inHandler_ = false;
  // Per-procedure tracking for implicit ALLOCATE of CONTROLLED variables
  // (IBM Enterprise PL/I): one bit per symbol prevents duplicate pushes.
  std::unordered_set<Symbol*> ctlImplicitAlloc_;
  // AUTOMATIC AREA variables of the procedure being emitted (rule (20)): the
  // runtime region is created at entry and destroyed on every exit path.
  std::vector<Symbol*> areaLocals_;
  // Bitcode runtime (OPTIMIZATION.md §10, P3): runtimeBc_ is the path to the
  // build-tree runtime.bc; linkBitcode_ requests linking its pli_* definitions
  // into the module. Only the MAIN unit embeds (runtime globals must stay
  // shared across units), so the archive stays on the final link line where
  // its members are simply never pulled once the symbols are defined.
  std::string runtimeBc_;
  bool linkBitcode_ = false;
  // Parse runtime.bc, check its LLVM stamp and target triple, merge the whole
  // runtime into the module, and re-apply the P0 side-table facts to the real
  // bodies. The embedded definitions keep their external linkage so every
  // unit's pli_* references resolve to this one shared copy (runtime globals
  // stay shared); the archive stays on the link line but is never pulled.
  // Returns false after diagnosing.
  bool linkRuntimeBitcode();
};
