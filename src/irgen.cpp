#include "irgen.h"
#include <algorithm>

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
// 10^k as a compile-time integer (k >= 0); used to rescale FIXED DECIMAL
// values whose stored integer is scaled by 10^q (ADR-006).
static long long pliPow10(int k) {
  long long p = 1;
  for (int i = 0; i < k; ++i)
    p *= 10;
  return p;
}

// Compile-time scale reduction of a FIXED DECIMAL stored integer v by 10^k,
// rounding half away from zero (matches the runtime rescale in convert).
static long long pliRescaleDown(long long v, int k) {
  long long p = pliPow10(k);
  long long sign = v < 0 ? -1 : 1;
  return (v + pliPow10(k - 1) * 5 * sign) / p;
}

// Numeric value of a constant INITIAL expression (rule 26). A DECIMAL literal
// holds its value scaled by 10^q, so divide back to the true value.
static double iniNumeric(const Expr* e) {
  if (!e)
    return 0;
  if (e->kind == Expr::FltLit)
    return e->fval;
  if (e->kind == Expr::DecLit)
    return (double)e->ival / (double)pliPow10(e->decScale);
  return (double)e->ival;
}

// Field paths of every dynamic-array member in a structure type (rule (13)),
// relative to the structure root; nested structures recurse (ADR-091).
static void collectDynMemberPaths(const Type& ty, std::vector<unsigned>& prefix,
                                  std::vector<std::vector<unsigned>>& out) {
  for (unsigned i = 0; i < ty.members.size(); ++i) {
    const Type& m = ty.members[i]->ty;
    prefix.push_back(i);
    if (m.isArray() && m.isDynamic())
      out.push_back(prefix);
    else if (m.isStruct())
      collectDynMemberPaths(m, prefix, out);
    prefix.pop_back();
  }
}

// The array type at a field path inside a structure type (rule 124): walk the
// recorded field indices like memberType does, but from a type root so it
// also serves minor-structure bases (ADR-091).
static const Type& structPathType(const Type& root, const std::vector<unsigned>& path) {
  const Type* cur = &root;
  for (unsigned f : path)
    cur = &cur->members[f]->ty;
  return *cur;
}

llvm::Type* IRGen::llvmTy(const Type& t) {
  // An array is a [N x elemTy] aggregate (rules (12),(13)); handled here so a
  // struct member that is itself an array (2 A(10) ...) lays out correctly.
  if (t.isArray())
    return llvm::ArrayType::get(llvmTy(t.elementType()), (unsigned)arrayExtent(t));
  switch (t.k) {
  case TK::FixedBin:
  case TK::FixedDec:
    return b_.getIntNTy(t.intBits());
  case TK::Float:
    return b_.getDoubleTy();
  case TK::Bit:
    return b_.getInt8Ty();
  case TK::Char: {
    if (t.varying) {
      llvm::Type* data = llvm::ArrayType::get(b_.getInt8Ty(), t.len);
      return llvm::StructType::get(b_.getInt32Ty(), data);
    }
    return llvm::ArrayType::get(b_.getInt8Ty(), t.len);
  }
  case TK::Struct: {
    // A level-numbered structure (rule 11): an LLVM literal struct of its
    // members, recursively laid out in declaration order. A dynamic (runtime
    // extent, rule 13) array member is a bare runtime-sized buffer, so its
    // field is a pointer to the element type (allocated and stored at entry).
    std::vector<llvm::Type*> mts;
    for (const auto& m : t.members)
      if (m->ty.isArray() && m->ty.isDynamic())
        mts.push_back(llvm::PointerType::get(ctx_, 0));
      else
        mts.push_back(llvmTy(m->ty));
    return llvm::StructType::get(ctx_, mts);
  }
  case TK::Pointer:
    return b_.getPtrTy();
  case TK::Complex: {
    // A complex value (QR2.2/CM5): a pair of FLOAT real/imaginary parts.
    llvm::Type* d = b_.getDoubleTy();
    return llvm::StructType::get(ctx_, {d, d});
  }
  case TK::Void:
    return b_.getVoidTy();
  }
  return b_.getInt32Ty();
}

llvm::Value* IRGen::i32(int v) { return b_.getInt32(v); }
llvm::Value* IRGen::i64(long long v) { return b_.getInt64(v); }
llvm::Value* IRGen::flt(double d) { return llvm::ConstantFP::get(b_.getDoubleTy(), d); }

llvm::AllocaInst* IRGen::entryAlloca(llvm::Type* ty, const llvm::Twine& name) {
  llvm::IRBuilder<> ab(&curFn_->getEntryBlock(), curFn_->getEntryBlock().begin());
  return ab.CreateAlloca(ty, nullptr, name);
}

static bool blockTerminated(llvm::BasicBlock* bb) {
  return bb && !bb->empty() && bb->back().isTerminator();
}

void IRGen::startBlock(llvm::BasicBlock* bb) {
  if (b_.GetInsertBlock() && !blockTerminated(b_.GetInsertBlock())) {
    // The current block is open (not terminated): fall through into bb.
    b_.CreateBr(bb);
  }
  b_.SetInsertPoint(bb);
}

void IRGen::newBlock() {
  llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx_, "", curFn_);
  b_.SetInsertPoint(bb);
}

void IRGen::branch(llvm::BasicBlock* target) {
  if (!blockTerminated(b_.GetInsertBlock())) {
    b_.CreateBr(target);
  }
}

llvm::GlobalVariable* IRGen::globalString(const std::string& s) {
  std::string want = s.empty() ? " " : s;
  auto* init = llvm::ConstantDataArray::getString(ctx_, want, false);
  for (auto* g : strLits_)
    if (g->getInitializer() == init)
      return g;
  auto* g = new llvm::GlobalVariable(mod_, init->getType(), true, llvm::GlobalValue::PrivateLinkage,
                                     init, "str." + std::to_string(strLits_.size()));
  strLits_.push_back(g);
  return g;
}

// ABI type tokens, used to expand runtime/pli_rt_abi.def into LLVM
// signatures. RtVoid is also the "no arguments" marker (a function with no
// parameters is written with a single VOID in the .def).
enum RtTok { RtVoid, RtI64, RtI32, RtI8, RtDouble, RtPtr };
struct RtSig {
  RtTok ret;
  std::vector<RtTok> args;
};

// The pli_* ABI table, expanded from the single source of truth
// runtime/pli_rt_abi.def. Tokens resolve to LLVM types inside runtimeFn,
// where the builder's context is available.
static const std::map<std::string, RtSig>& kRuntimeSigs() {
  static const std::map<std::string, RtSig> table = {
#define VOID RtVoid
#define I64 RtI64
#define I32 RtI32
#define I8 RtI8
#define DOUBLE RtDouble
#define PTR RtPtr
#define CPTR RtPtr
#define PLI_STRIP(...) __VA_ARGS__ // turn the .def's (a, b, c) into a braced list
#define PLI_FN(name, ret, args) {#name, {ret, {PLI_STRIP args}}},
#include "../runtime/pli_rt_abi.def"
#undef PLI_FN
#undef PLI_STRIP
#undef CPTR
#undef PTR
#undef DOUBLE
#undef I8
#undef I32
#undef I64
#undef VOID
  };
  return table;
}

// Get (or create) a declaration for a runtime `pli_*` function. The signature
// comes from runtime/pli_rt_abi.def, not from the caller, so the emitted IR
// cannot drift from the C ABI.
llvm::Function* IRGen::runtimeFn(const std::string& name) {
  auto& sigs = kRuntimeSigs();
  auto it = sigs.find(name);
  if (it == sigs.end())
    return nullptr; // not a pli_* ABI function
  auto tokTy = [this](RtTok k) -> llvm::Type* {
    switch (k) {
    case RtVoid:
      return b_.getVoidTy();
    case RtI64:
      return b_.getInt64Ty();
    case RtI32:
      return b_.getInt32Ty();
    case RtI8:
      return b_.getInt8Ty();
    case RtDouble:
      return b_.getDoubleTy();
    case RtPtr:
      return b_.getPtrTy();
    }
    return b_.getVoidTy();
  };
  const RtSig& s = it->second;
  std::vector<llvm::Type*> args;
  for (RtTok a : s.args)
    if (a != RtVoid)
      args.push_back(tokTy(a)); // RtVoid here means "no args"
  llvm::FunctionType* ft = llvm::FunctionType::get(tokTy(s.ret), args, false);
  llvm::Function* f = mod_.getFunction(name);
  if (f)
    return f;
  return llvm::Function::Create(ft, llvm::Function::ExternalLinkage, name, &mod_);
}

// Get (or create) an LLVM intrinsic with an explicit signature. Only used for
// non-ABI LLVM builtins (llvm.pow.f64, llvm.fabs.f64).
llvm::Function* IRGen::intrinsicFn(const std::string& name, llvm::Type* ret,
                                   std::vector<llvm::Type*> args) {
  llvm::FunctionType* ft = llvm::FunctionType::get(ret, args, false);
  llvm::Function* f = mod_.getFunction(name);
  if (f)
    return f;
  return llvm::Function::Create(ft, llvm::Function::ExternalLinkage, name, &mod_);
}

// Computational-condition trap (rules (91)-(94)): the shared shape behind
// the SIZE dispatch. Without an ON unit for `key` in the module keep the
// unconditional abort call so existing code pays nothing; otherwise consult
// the stack — empty takes the abort path, established runs the matching
// handler then branches to okBB, where the caller resumes with its own
// recovery value.
void IRGen::emitCondTrap(int key, const std::string& abortFn, const std::string& tag,
                         llvm::BasicBlock* okBB) {
  auto it = onHandlers_.find(key);
  if (it == onHandlers_.end()) {
    b_.CreateCall(runtimeFn(abortFn), {});
    b_.CreateUnreachable();
    return;
  }
  const std::vector<llvm::Function*>& handlers = it->second;
  llvm::Value* top =
      b_.CreateCall(runtimeFn("pli_on_top_cond"), {i64((long long)key)}, tag + "top");
  llvm::Value* none = b_.CreateICmpEQ(top, i64(0), tag + "none");
  llvm::BasicBlock* abortBB = llvm::BasicBlock::Create(ctx_, tag + ".abort", curFn_);
  llvm::BasicBlock* dspBB = llvm::BasicBlock::Create(ctx_, tag + ".dispatch", curFn_);
  b_.CreateCondBr(none, abortBB, dspBB);
  b_.SetInsertPoint(abortBB);
  b_.CreateCall(runtimeFn(abortFn), {});
  b_.CreateUnreachable();
  b_.SetInsertPoint(dspBB);
  llvm::SwitchInst* sw = b_.CreateSwitch(top, okBB, handlers.size());
  for (size_t i = 0; i < handlers.size(); ++i) {
    llvm::BasicBlock* hbb =
        llvm::BasicBlock::Create(ctx_, tag + ".handle." + std::to_string(i + 1), curFn_);
    sw->addCase(llvm::ConstantInt::get(b_.getInt64Ty(), (long long)i + 1), hbb);
    b_.SetInsertPoint(hbb);
    b_.CreateCall(handlers[i], {});
    b_.CreateBr(okBB);
  }
}

// SIZE dispatch (QR1.4, rules (91)-(94)): body of a fixed-overflow trap
// block; the computational-trap shape with the SIZE key and abort call.
void IRGen::emitSizeTrap(llvm::BasicBlock* okBB) {
  emitCondTrap(Stmt::kSizeCondKey, "pli_fixed_overflow", "size", okBB);
}

// Clamp an index into [lb, ub] (SUBSCRIPTRANGE resume value, rule 94): the
// guarded address computation uses the clamped index, so a handled slip
// touches the nearest edge element instead of out-of-bounds storage.
llvm::Value* IRGen::clampIndex(llvm::Value* i, llvm::Value* lb, llvm::Value* ub) {
  llvm::Value* lo = b_.CreateSelect(b_.CreateICmpSLT(i, lb, "clo"), lb, i, "cidx.lo");
  return b_.CreateSelect(b_.CreateICmpSGT(lo, ub, "chi"), ub, lo, "cidx");
}

// ZERODIVIDE value trap (rule 94): branch on `isZero`; the trap block routes
// through the ZERODIVIDE dispatch (abort when unhandled) and rejoins,
// resuming with `zero` instead of `computed`.
llvm::Value* IRGen::zerodivideResume(llvm::Value* isZero, llvm::Value* computed,
                                     llvm::Value* zero) {
  std::string zid = std::to_string(n_++);
  llvm::BasicBlock* trapBB = llvm::BasicBlock::Create(ctx_, "zd.trap." + zid, curFn_);
  llvm::BasicBlock* okBB = llvm::BasicBlock::Create(ctx_, "zd.ok." + zid, curFn_);
  b_.CreateCondBr(isZero, trapBB, okBB);
  b_.SetInsertPoint(trapBB);
  emitCondTrap(Stmt::kZerodivideCondKey, "pli_zerodivide", "zd", okBB);
  b_.SetInsertPoint(okBB);
  return b_.CreateSelect(isZero, zero, computed, "zdiv.r");
}

// FIXED BINARY checked +,-,* (QR1.2): the overflow intrinsic yields the
// value plus a flag; a set flag traps through the SIZE path (hard ERROR
// when no SIZE handler is established, QR1.4).
llvm::Value* IRGen::checkedArith(Tok op, llvm::Value* a, llvm::Value* b) {
  llvm::Type* ty = a->getType();
  unsigned bits = ty->getIntegerBitWidth();
  std::string base = op == Tok::Plus ? "sadd" : op == Tok::Minus ? "ssub" : "smul";
  std::string iname = "llvm." + base + ".with.overflow.i" + std::to_string(bits);
  llvm::Type* st = llvm::StructType::get(ctx_, {ty, b_.getInt1Ty()});
  llvm::Value* ov = b_.CreateCall(intrinsicFn(iname, st, {ty, ty}), {a, b}, "ov");
  llvm::Value* r = b_.CreateExtractValue(ov, 0, "bin");
  if (!sizeChecks())
    return r; // (NOSIZE): the wrapped value stands (rules (60)-(63), ADR-110)
  llvm::Value* of = b_.CreateExtractValue(ov, 1, "ovf");
  int seq = ovSeq_++;
  llvm::BasicBlock* trapBB =
      llvm::BasicBlock::Create(ctx_, "ov.trap." + std::to_string(seq), curFn_);
  llvm::BasicBlock* okBB = llvm::BasicBlock::Create(ctx_, "ov.ok." + std::to_string(seq), curFn_);
  b_.CreateCondBr(of, trapBB, okBB);
  b_.SetInsertPoint(trapBB);
  emitSizeTrap(okBB);
  b_.SetInsertPoint(okBB);
  return r;
}

// FIXED DECIMAL precision trap (QR1.2): |v| >= limit holds more digits than
// the target (SIZE path, hard ERROR when unhandled). Two-sided so
// INT64_MIN is caught without negating it.
void IRGen::magTrap(llvm::Value* v, long long limit) {
  if (!sizeChecks())
    return; // (NOSIZE): the wrapped value stands (rules (60)-(63), ADR-110)
  llvm::Value* hi = b_.CreateICmpSGE(v, i64(limit), "dov.hi");
  llvm::Value* lo = b_.CreateICmpSLE(v, i64(-limit), "dov.lo");
  llvm::Value* of = b_.CreateOr(hi, lo, "dov");
  int seq = ovSeq_++;
  llvm::BasicBlock* trapBB =
      llvm::BasicBlock::Create(ctx_, "ov.trap." + std::to_string(seq), curFn_);
  llvm::BasicBlock* okBB =
      llvm::BasicBlock::Create(ctx_, "ov.ok." + std::to_string(seq), curFn_);
  b_.CreateCondBr(of, trapBB, okBB);
  b_.SetInsertPoint(trapBB);
  emitSizeTrap(okBB);
  b_.SetInsertPoint(okBB);
}

// FLOAT -> FIXED range trap (QR1.2): FPToSI outside [lo, hi) is UB, so trap
// first through the SIZE path (hard ERROR when unhandled). Ordered compares
// fail on NaN, which therefore traps as well.
void IRGen::floatRangeTrap(llvm::Value* f, double lo, bool loIncl, double hi) {
  if (!sizeChecks())
    return; // (NOSIZE): the wrapped value stands (rules (60)-(63), ADR-110)
  llvm::Value* okLo = loIncl ? b_.CreateFCmpOGE(f, flt(lo), "frt.lo")
                             : b_.CreateFCmpOGT(f, flt(lo), "frt.lo");
  llvm::Value* okHi = b_.CreateFCmpOLT(f, flt(hi), "frt.hi");
  llvm::Value* ok = b_.CreateAnd(okLo, okHi, "frt.ok");
  int seq = ovSeq_++;
  llvm::BasicBlock* trapBB =
      llvm::BasicBlock::Create(ctx_, "ov.trap." + std::to_string(seq), curFn_);
  llvm::BasicBlock* okBB =
      llvm::BasicBlock::Create(ctx_, "ov.ok." + std::to_string(seq), curFn_);
  b_.CreateCondBr(ok, okBB, trapBB);
  b_.SetInsertPoint(trapBB);
  emitSizeTrap(okBB);
  b_.SetInsertPoint(okBB);
}

// Resolve the LLVM function a call targets. External C entries (rule (38)) have
// no PL/I body, so no function is pre-declared; declare it on demand. Every
// PL/I argument is passed by reference, so all parameters are pointers.
llvm::Function* IRGen::calleeFn(Symbol* sym) {
  Proc* callee = sym->proc;
  Stmt* en = sym->entry;
  std::string name;
  if (en)
    name =
        entryIrName(callee->name, callee->parent ? callee->parent->name : "", en->name).substr(1);
  else if (callee)
    name = callee->irName.substr(1);
  else if (sym->isEntry)
    name = sym->irName.substr(1);
  else
    return nullptr;

  llvm::Function* f = mod_.getFunction(name);
  if (f)
    return f;
  if (!sym->isEntry)
    return f; // internal callee missing a declaration is a bug

  std::vector<llvm::Type*> pt;
  for (size_t i = 0; i < sym->entryParams.size(); ++i)
    pt.push_back(b_.getPtrTy());
  for (const Type& t : sym->entryParams)
    if (t.isArray() && !t.dims.empty() && t.dims[0].adj)
      pt.push_back(b_.getInt64Ty()); // hidden `*` extent args
  llvm::Type* rty =
      sym->entryIsFunction ? llvmTy(sym->entryRetTy) : b_.getVoidTy(); // rule (34) RETURNS
  llvm::FunctionType* ft = llvm::FunctionType::get(rty, pt, false);
  return llvm::Function::Create(ft, llvm::Function::ExternalLinkage, name, &mod_);
}

// ---------------------------------------------------------------------------
// module
// ---------------------------------------------------------------------------
std::string IRGen::run(HProgram& prog) {
  // Reject unsupported signatures before constructing a partial module.
  for (auto& p : prog.procs) {
    if (p->isFunction && p->retTy.isChar())
      d_.error(p->loc, "character-valued functions are not implemented in this stage", "(34)");
    if (p->isFunction && p->retTy.isStruct()) {
      for (auto& st : p->body)
        if (st && st->kind == HStmt::Entry)
          d_.error(st->loc,
                   "a structure-returning procedure with ENTRY statements is not implemented in "
                   "this stage",
                   "(56)");
    }
  }
  if (prog.mainProc && !prog.mainProc->params.empty())
    d_.error(prog.mainProc->loc,
             "parameters on the MAIN procedure are not implemented in this stage", "(2)");
  if (!d_.ok())
    return "";

  // The module needs a data layout so aggregate store sizes (used by
  // whole-structure assignment's memcpy, rule 127) match the target. Pick by
  // pointer/aggregate width from the target triple; the OS mangling does not
  // affect type sizes.
  if (!triple_.empty()) {
    if (llvm::Triple(triple_).isArch64Bit())
      mod_.setDataLayout("e-m:o-i64:64-i128:128-n32:64-S128");
    else
      mod_.setDataLayout("e-m:o-p:32:32-i64:64-i128:128-n32:64-S128");
  }

  emitGlobals();

  // ON ERROR handlers (rules (91)-(94)): number the established units, then
  // pre-create their functions so SIGNAL dispatch resolves regardless of
  // emission order (mirroring declareProc for procedures).
  assignOnIds(prog);
  declareOnHandlers(prog);

  // Pre-declare every procedure's functions/aliases so a call site resolves
  // regardless of the order the (flattened) procedures are emitted in.
  for (auto& p : prog.procs)
    declareProc(p.get());
  for (auto& p : prog.procs)
    emitProc(p.get());
  emitOnHandlers(prog);

  // C entry point: initialise the runtime, invoke the MAIN procedure,
  // terminate normally (this is where FINISH would be raised, see M5).
  if (prog.mainProc) {
    llvm::Function* main = llvm::Function::Create(llvm::FunctionType::get(b_.getInt32Ty(), false),
                                                  llvm::Function::ExternalLinkage, "main", &mod_);
    llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx_, "entry", main);
    b_.SetInsertPoint(bb);
    b_.CreateCall(runtimeFn("pli_rt_init"), {});
    llvm::Function* mfn = mod_.getFunction(prog.mainProc->irName.substr(1));
    if (!mfn) {
      mfn = llvm::Function::Create(llvm::FunctionType::get(b_.getVoidTy(), false),
                                   llvm::Function::InternalLinkage, prog.mainProc->irName.substr(1),
                                   &mod_);
      llvm::BasicBlock* mb = llvm::BasicBlock::Create(ctx_, "entry", mfn);
      b_.SetInsertPoint(mb);
      b_.CreateRetVoid();
    }
    b_.SetInsertPoint(bb);
    b_.CreateCall(mfn, {});
    b_.CreateCall(runtimeFn("pli_rt_fini"), {});
    b_.CreateRet(i32(0));
  }

  // Verify the module before serializing; a malformed IR will trip an
  // assertion here rather than causing an opaque clang assembler crash.
  if (llvm::verifyModule(mod_, &llvm::errs())) {
    d_.error({}, "internal error: LLVM module verification failed", "");
    return "";
  }

  // Serialize to a string for the driver / clang pipeline.
  std::string ir;
  llvm::raw_string_ostream os(ir);
  os << "; Generated by plic (PL/I -> LLVM)\n";
  if (!triple_.empty())
    mod_.setTargetTriple(llvm::Triple(triple_));
  mod_.print(os, nullptr);
  return ir;
}

// An LLVM scalar constant for an INITIAL element value (rule 26). Null is
// returned only for types without a constant form (e.g. STRUCT), which callers
// fall back to zero-initialising.
llvm::Constant* IRGen::scalarInitConstant(const Type& t, const Expr* ini) {
  switch (t.k) {
  case TK::FixedBin:
  case TK::FixedDec: {
    long long v = 0;
    if (ini) {
      if (ini->kind == Expr::DecLit) {
        v = ini->ival;
        int dq = t.scale - ini->decScale; // rescale to the target type's scale
        v = dq > 0 ? v * pliPow10(dq) : dq < 0 ? pliRescaleDown(v, -dq) : v;
      } else {
        v = ini->kind == Expr::FltLit   ? (long long)ini->fval
            : ini->kind == Expr::BitLit ? (!ini->sval.empty() && ini->sval[0] == '1')
                                        : ini->ival;
      }
    }
    return llvm::ConstantInt::get(llvmTy(t), v, true);
  }
  case TK::Float: {
    return llvm::ConstantFP::get(b_.getDoubleTy(), iniNumeric(ini));
  }
  case TK::Bit: {
    int v = 0;
    if (ini)
      v = ini->kind == Expr::BitLit ? (!ini->sval.empty() && ini->sval[0] == '1')
                                    : (ini->ival != 0 || ini->fval != 0);
    return llvm::ConstantInt::get(b_.getInt8Ty(), v);
  }
  case TK::Char: {
    std::string text(t.len, ' ');
    if (ini) {
      for (int i = 0; i < t.len && i < (int)ini->sval.size(); ++i)
        text[i] = ini->sval[i];
    }
    llvm::Constant* data = llvm::ConstantDataArray::getString(ctx_, text, false);
    if (t.varying) {
      size_t cur = ini ? std::min<size_t>(ini->sval.size(), (size_t)t.len) : 0;
      return llvm::ConstantStruct::get(llvm::cast<llvm::StructType>(llvmTy(t)),
                                       llvm::ConstantInt::get(b_.getInt32Ty(), cur), data);
    }
    return data;
  }
  default:
    return nullptr; // no scalar constant form (e.g. STRUCT)
  }
}

// A materialised scalar value for an INITIAL element (rule 26), for storing
// into an AUTOMATIC array element.
Val IRGen::initValue(const Type& t, const Expr* e) {
  Val v;
  v.ty = t;
  switch (t.k) {
  case TK::Float:
    v.reg = flt(iniNumeric(e));
    break;
  case TK::Bit:
    v.reg = b_.getInt1(e->kind == Expr::BitLit ? (!e->sval.empty() && e->sval[0] == '1')
                                               : (e->ival != 0 || e->fval != 0));
    break;
  default: { // Fixed
    long long iv = e->kind == Expr::FltLit ? (long long)e->fval : e->ival;
    if (e->kind == Expr::DecLit) {
      int dq = t.scale - e->decScale; // rescale to the target type's scale
      iv = dq > 0 ? iv * pliPow10(dq) : dq < 0 ? pliRescaleDown(iv, -dq) : iv;
    }
    v.reg = llvm::ConstantInt::get(llvmTy(t), iv, true);
    break;
  }
  }
  return v;
}

void IRGen::emitGlobals() {
  for (Symbol* s : sema_.storage()) {
    if (!s->isStatic || s->kind != Symbol::Var)
      continue;
    const Type& t = s->ty;
    if (t.k == TK::Void)
      continue;
    llvm::Type* gt = llvmTy(t);
    llvm::Constant* ginit = nullptr;
    if (t.isArray()) {
      // STATIC array: a [N x elemTy] global (rules (12),(13)), zero-initialised
      // unless an INITIAL element list (rule 26) supplies a constant per element.
      gt = llvm::ArrayType::get(llvmTy(t.elementType()), (unsigned)arrayExtent(t));
      if (!s->initElems.empty()) {
        std::vector<llvm::Constant*> els;
        for (Expr* e : s->initElems)
          els.push_back(scalarInitConstant(t.elementType(), e));
        ginit = llvm::ConstantArray::get(llvm::cast<llvm::ArrayType>(gt), els);
      } else {
        ginit = llvm::ConstantAggregateZero::get(gt);
      }
    } else {
      ginit = scalarInitConstant(t, s->initExpr);
      if (!ginit)
        ginit = llvm::ConstantAggregateZero::get(gt); // STRUCT (rule 11)
    }
    auto* g = new llvm::GlobalVariable(mod_, gt, false, llvm::GlobalValue::InternalLinkage, ginit,
                                       s->irName.substr(1));
    symAddr_[s] = g;
  }
}

llvm::Value* IRGen::addressOf(Symbol* sym) {
  // A BASED variable (rule 25) has no storage of its own: its address is the
  // value held in the based POINTER variable, loaded at each reference.
  if (sym->basedBase)
    return b_.CreateLoad(b_.getPtrTy(), addressOf(sym->basedBase), "basep");
  // A DEFINED variable (rule 24) has no storage of its own. A whole-base or
  // iSUB overlay resolves to the base's address; a scalar element overlay
  // (all-constant subscripts, no iSUB) is a stable GEP into the base.
  if (sym->definedBase) {
    if (!sym->definedConst.empty() && sym->definedIsubAxis < 0)
      return definedConstAddr(sym);
    return addressOf(sym->definedBase);
  }
  // rule (8): an enclosing variable is reached through this frame's static
  // link; otherwise it is this frame's own storage (globals / allocas /
  // parameters). Both are recorded in symAddr_.
  return symAddr_.count(sym) ? symAddr_[sym] : nullptr;
}

void IRGen::allocaLocals(HProc* p) {
  // Pass 1: fixed-size storage (scalars and constant-bounds arrays). Dynamic
  // arrays are deferred so that any variables their bounds reference are
  // already addressable.
  for (Symbol* s : p->localSyms) {
    if (s->kind != Symbol::Var || s->ty.isDynamic())
      continue;
    llvm::Value* a;
    if (s->ty.isArray()) {
      // A fixed-size array is a [N x elemTy] alloca (rules (12),(13)).
      const Type& el = s->ty.elementType();
      a = entryAlloca(llvm::ArrayType::get(llvmTy(el), (unsigned)arrayExtent(s->ty)),
                      s->irName.substr(1));
    } else {
      a = entryAlloca(llvmTy(s->ty), s->irName.substr(1));
    }
    symAddr_[s] = a;
    if (s->ty.isChar() && !s->ty.isArray()) { // blank fill (scalar char only)
      std::string blanks(s->ty.len, ' ');
      llvm::Value* g = globalString(blanks);
      if (s->ty.varying) {
        llvm::Value* lenp = b_.CreateStructGEP(llvmTy(s->ty), a, 0, "lenp");
        b_.CreateStore(i32(0), lenp);
      } else {
        b_.CreateCall(runtimeFn("pli_assign_char"), {a, i64(s->ty.len), g, i64(0)});
      }
    }
  }
  // Pass 2: dynamic (runtime-extent) arrays (rules (12),(13)). Evaluate the
  // upper bound at entry, allocate a runtime-sized element buffer, and record
  // the buffer pointer and the bound value for subscript addressing.
  for (Symbol* s : p->localSyms) {
    if (s->kind != Symbol::Var || !s->ty.isDynamic())
      continue;
    const Type& el = s->ty.elementType();
    const Dim& d = s->ty.dims[0];
    // The upper bound is a runtime expression, or a constant when only the lower
    // bound is dynamic; a dynamic lower bound is evaluated at entry too.
    llvm::Value* ub = s->dynUb ? toI64(emitExpr(s->dynUb)) : i64(d.ub);
    llvm::Value* lb = s->dynLb ? toI64(emitExpr(s->dynLb)) : i64(d.lb);
    llvm::Value* extent = b_.CreateAdd(b_.CreateSub(ub, lb, "e1"), i64(1), "ext");
    // A dynamic array may have fixed later axes (only the first axis is dynamic):
    // the buffer holds every element of every axis, so scale by their product.
    long long rest = 1;
    for (size_t k = 1; k < s->ty.dims.size(); ++k)
      rest *= (s->ty.dims[k].ub - s->ty.dims[k].lb + 1);
    if (rest != 1)
      extent = b_.CreateMul(extent, i64(rest), "extall");
    // An INITIAL itemlist (rule 26) is stored at block entry; a list longer than
    // the runtime extent cannot be diagnosed at compile time, so size the buffer
    // to hold it too (the logical extent used for bounds checks is unchanged).
    long long ninit = (long long)s->initElems.size();
    if (ninit > 0)
      extent =
          b_.CreateSelect(b_.CreateICmpUGT(extent, i64(ninit), "maxc"), extent, i64(ninit), "max");
    llvm::Value* buf = b_.CreateAlloca(llvmTy(el), extent, s->irName.substr(1) + ".dyn");
    symAddr_[s] = buf;
    if (s->dynUb)
      dynUb_[s] = ub;
    if (s->dynLb)
      dynLb_[s] = lb;
  }
  // Pass 3: dynamic array structure members (rule 13). Each member is a bare
  // runtime-sized element buffer; evaluate its bounds at entry, allocate it, and
  // store the buffer pointer into the struct field (the struct itself was
  // allocated in pass 1). Record the bounds for subscript addressing.
  for (Symbol* s : p->localSyms) {
    if (s->kind != Symbol::Var || s->dynMembers.empty())
      continue;
    for (const auto& mh : s->dynMembers) {
      const Type& arr = memberType(s, mh.path);
      const Type& el = arr.elementType();
      const Dim& d = arr.dims[0];
      llvm::Value* ub = mh.ub ? toI64(emitExpr(mh.ub)) : i64(d.ub);
      llvm::Value* lb = mh.lb ? toI64(emitExpr(mh.lb)) : i64(d.lb);
      llvm::Value* extent = b_.CreateAdd(b_.CreateSub(ub, lb, "me1"), i64(1), "mext");
      long long rest = 1;
      for (size_t k = 1; k < arr.dims.size(); ++k)
        rest *= (arr.dims[k].ub - arr.dims[k].lb + 1);
      if (rest != 1)
        extent = b_.CreateMul(extent, i64(rest), "mextall");
      // An INITIAL itemlist (rule (26), ADR-092) may exceed the live extent;
      // the emission stores every supplied value straight-line, so grow the
      // buffer by the whole itemlist length (an over-approximation — only the
      // trailing values target this member — mirroring the top-level dynamic
      // INITIAL pre-size).
      if (!s->initElems.empty())
        extent = b_.CreateAdd(extent, i64((long long)s->initElems.size()), "mextinit");
      llvm::Value* buf = b_.CreateAlloca(llvmTy(el), extent, s->irName.substr(1) + ".mdyn");
      b_.CreateStore(buf, memberAddr(s, mh.path, s->loc));
      memberDyn_[MemberDyn{s, mh.path}] = MemberBounds{ub, lb};
    }
  }
}

// A dynamic (runtime-extent) array parameter is a by-reference pointer to the
// caller's data with no own storage, so its extent must be read once from the
// bound argument (itself by-ref) at entry. Record it so subscripting and the
// array built-ins bounds-check against the live extent (rules (12),(13),(34)).
void IRGen::recordDynParamUbs(const std::vector<Symbol*>& params) {
  for (Symbol* s : params) {
    if (s->ty.isDynamic() && s->dynUb && !dynUb_.count(s))
      dynUb_[s] = toI64(emitExpr(s->dynUb));
    if (s->ty.isDynamic() && s->dynLb && !dynLb_.count(s))
      dynLb_[s] = toI64(emitExpr(s->dynLb));
  }
}

// INITIAL attribute on AUTOMATIC variables (rule 26): runs on every
// activation.
static void collectDeclStmts(HStmt* s, std::vector<HStmt*>& out) {
  if (!s)
    return;
  if (s->kind == HStmt::Declare) {
    out.push_back(s);
    return;
  }
  if (s->kind == HStmt::Begin || s->kind == HStmt::Group)
    for (auto& b : s->body)
      collectDeclStmts(b.get(), out);
  else {
    for (auto& b : s->body)
      collectDeclStmts(b.get(), out);
    collectDeclStmts(s->thenS.get(), out);
    collectDeclStmts(s->elseS.get(), out);
  }
}

void IRGen::emitInitials(HProc* p) {
  std::vector<HStmt*> decls;
  for (auto& st : p->body)
    collectDeclStmts(st.get(), decls);
  for (HStmt* st : decls) {
    for (auto& item : st->decls) {
      Symbol* sym = item.sym;
      // INITIAL on a dynamic array (rule 26): the element buffer is a bare
      // runtime-sized alloca (allocaLocals pass 2), pre-sized to hold the whole
      // itemlist, so store each value into its slot straight-line.
      if (sym && sym->ty.isArray() && !sym->initElems.empty() && sym->ty.isDynamic()) {
        const Type& et = sym->ty.elementType();
        int i = 0;
        for (Expr* e : sym->initElems) {
          llvm::Value* p = b_.CreateGEP(llvmTy(et), symAddr_[sym], {i64(i++)}, "init.el");
          storeScalarTo(p, et, initValue(et, e));
        }
        continue;
      }
      // INITIAL on an AUTOMATIC array (rule 26): store each element constant
      // into its slot; the array is a [N x elemTy] alloca.
      if (sym && sym->ty.isArray() && !sym->initElems.empty()) {
        const Type& et = sym->ty.elementType();
        llvm::Type* aty = llvm::ArrayType::get(llvmTy(et), (unsigned)arrayExtent(sym->ty));
        llvm::Value* base = addressOf(sym);
        int i = 0;
        for (Expr* e : sym->initElems) {
          llvm::Value* idx = i32(i++);
          llvm::Value* p = b_.CreateGEP(aty, base, {i32(0), idx}, "init.el");
          storeScalarTo(p, et, initValue(et, e));
        }
        continue;
      }
      // INITIAL on an AUTOMATIC structure (rule (26)): store each leaf value
      // into its member slot, recursing through nested structures and array
      // members (emitStructInitValues).
      if (sym && sym->ty.isStruct() && !sym->initElems.empty()) {
        size_t idx = 0;
        emitStructInitValues(addressOf(sym), sym->ty, sym->initElems, idx, item.loc);
        continue;
      }
      // INITIAL(CALL f(...)) (rule 27): evaluate the call at block entry and
      // store its return value into the variable. Runs on every entry (AUTOMATIC).
      if (sym && sym->initCallH) {
        Val v = emitExpr(sym->initCallH);
        storeTo(sym, v, item.loc);
        continue;
      }
      Expr* e = item.sym ? item.sym->initExpr : nullptr;
      if (!e)
        continue;
      Val v;
      v.ty = item.sym->ty;
      switch (item.sym->ty.k) {
      case TK::Float: {
        v.reg = flt(iniNumeric(e));
        break;
      }
      case TK::Bit: {
        bool one = e->kind == Expr::BitLit ? (!e->sval.empty() && e->sval[0] == '1')
                                           : (e->ival != 0 || e->fval != 0);
        v.reg = b_.getInt1(one);
        break;
      }
      case TK::Char: {
        Val cv;
        cv.ty = item.sym->ty;
        cv.ptr = globalString(e->sval);
        cv.len = i64(e->sval.size());
        storeTo(item.sym, cv, item.loc);
        continue;
      }
      default: { // Fixed
        long long val = e->kind == Expr::FltLit ? (long long)e->fval : e->ival;
        if (e->kind == Expr::DecLit) {
          int dq = item.sym->ty.scale - e->decScale; // rescale to the target scale
          val = dq > 0 ? val * pliPow10(dq) : dq < 0 ? pliRescaleDown(val, -dq) : val;
        }
        v.reg = llvm::ConstantInt::get(llvmTy(item.sym->ty), val, true);
        break;
      }
      }
      storeTo(item.sym, v, item.loc);
    }
  }
}

// Assign an LLVM block to every labelled statement (rule (64)) so a GO TO
// (rule (77)) can branch to it. Multiple labels on one statement alias one block.
void IRGen::collectGotoBlocks(HStmt* s) {
  if (!s)
    return;
  if (!s->labels.empty() && s->kind != HStmt::Entry) {
    std::string blk = "L" + std::to_string(n_++);
    llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx_, blk, curFn_);
    for (const std::string& l : s->labels)
      labelBlocks_[l] = bb;
  }
  if (s->thenS)
    collectGotoBlocks(s->thenS.get());
  if (s->elseS)
    collectGotoBlocks(s->elseS.get());
  for (auto& b : s->body)
    collectGotoBlocks(b.get());
}

// Pre-create a procedure's functions and aliases so a call site resolves
// regardless of emission order. Plain procedures get one function (filled by
// emitPlainProc); multi-entry procedures get the shared impl (filled by
// emitMultiEntryProc) plus a fully-built tail-calling thunk per entry name.
void IRGen::declareProc(HProc* p) {
  if (p->isPackage)
    return; // packages emit no function (extension, ADR-109)
  bool sret = p->isFunction && p->retTy.isStruct();
  // A structure-valued function (rule 127) returns through a hidden result
  // pointer and returns void: the caller allocates the storage and passes its
  // address as the first argument.
  // The shared implementation returns the single result type shared by every
  // function-valued entry point (rule 56): the procedure's own RETURNS type
  // when it is a function, else the common RETURNS type of its function ENTRYs
  // (a non-function primary entry may coexist with function segments).
  llvm::Type* implRet =
      (sret || p->commonRetTy.isVoid()) ? b_.getVoidTy() : llvmTy(p->commonRetTy);
  std::vector<HStmt*> entries;
  for (auto& st : p->body)
    if (st && st->kind == HStmt::Entry)
      entries.push_back(st.get());
  auto aliasFor = [&](const std::string& en, llvm::Function* target) {
    std::string alias = "PLI_" + (p->parent ? p->parent->name + "$" : std::string()) + en;
    llvm::GlobalAlias::create(llvm::GlobalValue::InternalLinkage, alias, target);
  };

  if (entries.empty()) {
    std::vector<llvm::Type*> pt;
    if (sret)
      pt.push_back(b_.getPtrTy()); // hidden result pointer (rule 127)
    for (size_t i = 0; i < p->paramSyms.size(); ++i)
      pt.push_back(b_.getPtrTy());
    for (Symbol* s : p->paramSyms)
      if (isAdjustable(s))
        pt.push_back(b_.getInt64Ty()); // hidden `*` extent args
    for (size_t i = 0; i < p->env.size(); ++i)
      pt.push_back(b_.getPtrTy()); // links
    llvm::FunctionType* ft = llvm::FunctionType::get(implRet, pt, false);
    // Rule (42): an external procedure is visible to the linker under its
    // upper-cased name; every other procedure stays module-private.
    auto linkage = p->isExternal ? llvm::Function::ExternalLinkage
                                 : llvm::Function::InternalLinkage;
    llvm::Function* fn = llvm::Function::Create(ft, linkage, p->irName.substr(1), &mod_);
    for (const auto& en : p->entryNames)
      aliasFor(en, fn);
    return;
  }

  // Multi-entry: the union of every entry point's parameters.
  std::vector<Symbol*> uni;
  auto push = [&](Symbol* s) {
    if (std::find(uni.begin(), uni.end(), s) == uni.end())
      uni.push_back(s);
  };
  for (Symbol* s : p->paramSyms)
    push(s);
  for (HStmt* e : entries)
    for (Symbol* s : e->entryParamSyms)
      push(s);

  // Shared implementation (body filled by emitMultiEntryProc).
  std::vector<llvm::Type*> pt;
  for (size_t i = 0; i < uni.size(); ++i)
    pt.push_back(b_.getPtrTy());
  for (Symbol* s : uni)
    if (isAdjustable(s))
      pt.push_back(b_.getInt64Ty()); // hidden `*` extent args
  for (size_t i = 0; i < p->env.size(); ++i)
    pt.push_back(b_.getPtrTy());
  pt.push_back(b_.getInt64Ty()); // the entry selector
  llvm::FunctionType* ift = llvm::FunctionType::get(implRet, pt, false);
  llvm::Function* impl = llvm::Function::Create(ift, llvm::Function::InternalLinkage,
                                                p->irName.substr(1) + ".impl", &mod_);

  // A thunk marshals one entry's arguments and tail-calls the shared impl.
  // The impl carries one segment per entry point in body order; control must
  // never fall from one segment into the next, so each segment ends with an
  // explicit terminator when its source statements do not supply one: a
  // function-valued segment returns the common result type (reached only by
  // falling off its RETURNs), and a void segment returns void. The thunk
  // returns its own entry point's result type (void for a non-function entry;
  // a value produced by the impl is then discarded).
  int thunkN = 0;
  auto thunk = [&](const std::vector<Symbol*>& mine, llvm::Value* selv, llvm::Type* trty) {
    std::vector<llvm::Type*> sig;
    for (size_t i = 0; i < mine.size(); ++i)
      sig.push_back(b_.getPtrTy());
    for (Symbol* s : mine)
      if (isAdjustable(s))
        sig.push_back(b_.getInt64Ty()); // hidden `*` extent args
    for (size_t i = 0; i < p->env.size(); ++i)
      sig.push_back(b_.getPtrTy()); // links
    llvm::FunctionType* tft = llvm::FunctionType::get(trty, sig, false);
    llvm::Function* tf = llvm::Function::Create(tft, llvm::Function::InternalLinkage,
                                                "entry.thunk." + std::to_string(thunkN++), &mod_);
    llvm::BasicBlock* tb = llvm::BasicBlock::Create(ctx_, "entry", tf);
    b_.SetInsertPoint(tb);
    std::vector<llvm::Value*> args;
    size_t targ = 0;
    std::unordered_map<Symbol*, llvm::Value*> mineAddr;
    for (Symbol* s : mine)
      mineAddr[s] = tf->getArg(targ++);
    std::unordered_map<Symbol*, llvm::Value*> mineExt;
    for (Symbol* s : mine)
      if (isAdjustable(s))
        mineExt[s] = tf->getArg(targ++);
    std::vector<llvm::Value*> links;
    for (size_t i = 0; i < p->env.size(); ++i)
      links.push_back(tf->getArg(targ++));
    for (Symbol* u : uni)
      args.push_back(mineAddr.count(u) ? mineAddr[u] : llvm::UndefValue::get(b_.getPtrTy()));
    for (Symbol* u : uni)
      if (isAdjustable(u))
        args.push_back(mineExt.count(u) ? mineExt[u] : llvm::UndefValue::get(b_.getInt64Ty()));
    for (auto* l : links)
      args.push_back(l);
    args.push_back(selv);
    llvm::CallInst* call = b_.CreateCall(ift, impl, args);
    call->setTailCall(true);
    if (trty->isVoidTy())
      b_.CreateRetVoid();
    else
      b_.CreateRet(call);
    return tf;
  };

  llvm::Function* t0 = thunk(p->paramSyms, i64(0), p->isFunction ? llvmTy(p->retTy) : b_.getVoidTy());
  t0->setName(p->irName.substr(1));
  // Rule (42): the primary entry thunk of an external procedure is the
  // link-visible symbol; the shared impl stays module-private.
  if (p->isExternal)
    t0->setLinkage(llvm::Function::ExternalLinkage);
  for (const auto& en : p->entryNames)
    aliasFor(en, t0);
  for (size_t i = 0; i < entries.size(); ++i) {
    llvm::Function* tf = thunk(entries[i]->entryParamSyms, i64(i + 1),
                               entries[i]->entryIsFunction ? llvmTy(entries[i]->entryRetTy)
                                                           : b_.getVoidTy());
    tf->setName(entryIrName(p->name, p->parent ? p->parent->name : "", entries[i]->name).substr(1));
  }
}

void IRGen::emitProc(HProc* p) {
  if (p->isPackage)
    return; // packages emit no function (extension, ADR-109)
  curProc_ = p;
  labelBlocks_.clear();
  symAddr_.clear();
  structRetPtr_ = nullptr;
  // Re-seed globals: static storage resolves the same in every procedure.
  for (Symbol* s : sema_.storage())
    if (s->isStatic && s->kind == Symbol::Var)
      symAddr_[s] = mod_.getGlobalVariable(s->irName.substr(1), true);

  llvm::Type* retLLVM = (p->isFunction && p->retTy.isStruct())
                            ? b_.getVoidTy()
                            : (p->isFunction ? llvmTy(p->retTy) : b_.getVoidTy());

  // rule (56): ENTRY statements declare alternate entry points.
  std::vector<HStmt*> entries;
  for (auto& st : p->body)
    if (st && st->kind == HStmt::Entry)
      entries.push_back(st.get());

  if (entries.empty())
    emitPlainProc(p, retLLVM);
  else
    emitMultiEntryProc(p, entries,
                       p->commonRetTy.isVoid() ? b_.getVoidTy() : llvmTy(p->commonRetTy));
  curProc_ = nullptr;
}

// One procedure, one LLVM function: the ordinary path (no ENTRY statements).
// The function and its entry-namelist aliases were pre-created by declareProc.
void IRGen::emitPlainProc(HProc* p, llvm::Type* retLLVM) {
  llvm::Function* fn = mod_.getFunction(p->irName.substr(1));
  curFn_ = fn;
  curRetTy_ = p->isFunction ? p->retTy : Type::voidTy();
  // The entry block must be the function's first block (so it is the real
  // entry, and the allocas it holds dominate every reachable block).
  llvm::BasicBlock* entry = llvm::BasicBlock::Create(ctx_, "entry", fn);
  for (auto& st : p->body)
    collectGotoBlocks(st.get());

  // Parameter arguments become their symbols' addresses (PL/I by reference);
  // each `*`-extent parameter (rule 13) then reads its hidden i64 extent into a
  // dope slot; the trailing args are the static links (rule (8)).
  size_t ai = 0;
  if (p->isFunction && p->retTy.isStruct())
    structRetPtr_ = fn->getArg(ai++); // hidden result pointer (rule 127)
  for (Symbol* s : p->paramSyms)
    symAddr_[s] = fn->getArg(ai++);
  for (Symbol* s : p->paramSyms)
    if (isAdjustable(s))
      dynUb_[s] = fn->getArg(ai++);
  for (size_t i = 0; i < p->env.size(); ++i)
    symAddr_[p->env[i]] = fn->getArg(ai++);

  b_.SetInsertPoint(entry);

  allocaLocals(p);
  emitInitials(p); // INITIAL attribute on AUTOMATIC variables (rule 26)
  recordDynParamUbs(p->paramSyms);

  // Rule (91): save the ERROR handler depth so procedure exit restores the
  // caller's establishment state. Skipped when nothing establishes handlers.
  curOnDepth_ = nullptr;
  if (!onHandlers_.empty()) {
    curOnDepth_ = entryAlloca(b_.getInt64Ty(), "ondepth");
    b_.CreateStore(b_.CreateCall(runtimeFn("pli_on_depth_error"), {}), curOnDepth_);
  }

  for (auto& st : p->body)
    emitStmt(st.get());

  if (!blockTerminated(b_.GetInsertBlock())) {
    if (curOnDepth_)
      b_.CreateCall(runtimeFn("pli_on_reset_error"),
                    {b_.CreateLoad(b_.getInt64Ty(), curOnDepth_, "ondepth")});
    if (p->isFunction && p->retTy.isStruct())
      b_.CreateRetVoid(); // structure-valued: result written to the hidden pointer
    else if (p->isFunction)
      b_.CreateRet(llvm::Constant::getNullValue(retLLVM)); // fall-off: return a zero value
    else
      b_.CreateRetVoid();
  }
  curOnDepth_ = nullptr;
  curFn_ = nullptr;
}

// rule (56): a procedure with ENTRY statements. The shared implementation
// function (pre-created by declareProc) carries the whole body split into
// segments; each entry point's thunk was built by declareProc.
void IRGen::emitMultiEntryProc(HProc* p, const std::vector<HStmt*>& entries, llvm::Type* retLLVM) {
  llvm::Function* impl = mod_.getFunction(p->irName.substr(1) + ".impl");
  curFn_ = impl;
  curRetTy_ = p->commonRetTy;
  // The entry block must be the function's first block (so it is the real
  // entry and its allocas dominate every reachable segment block).
  llvm::BasicBlock* entry = llvm::BasicBlock::Create(ctx_, "entry", impl);
  for (auto& st : p->body)
    collectGotoBlocks(st.get());

  std::vector<Symbol*> uni;
  auto push = [&](Symbol* s) {
    if (std::find(uni.begin(), uni.end(), s) == uni.end())
      uni.push_back(s);
  };
  for (Symbol* s : p->paramSyms)
    push(s);
  for (HStmt* e : entries)
    for (Symbol* s : e->entryParamSyms)
      push(s);

  size_t ai = 0;
  for (Symbol* s : uni)
    symAddr_[s] = impl->getArg(ai++);
  for (Symbol* s : uni)
    if (isAdjustable(s))
      dynUb_[s] = impl->getArg(ai++);
  for (size_t i = 0; i < p->env.size(); ++i)
    symAddr_[p->env[i]] = impl->getArg(ai++);
  llvm::Value* sel = impl->getArg(ai++);

  b_.SetInsertPoint(entry);

  allocaLocals(p);
  emitInitials(p);
  recordDynParamUbs(uni);

  // Rule (91): save the ERROR handler depth for exit restore (see retPads).
  curOnDepth_ = nullptr;
  if (!onHandlers_.empty()) {
    curOnDepth_ = entryAlloca(b_.getInt64Ty(), "ondepth");
    b_.CreateStore(b_.CreateCall(runtimeFn("pli_on_depth_error"), {}), curOnDepth_);
  }

  // Entry selector: dispatch to the segment each call entered through.
  std::vector<llvm::BasicBlock*> segs;
  for (size_t i = 0; i <= entries.size(); ++i) {
    llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx_, "e.seg." + std::to_string(i), impl);
    segs.push_back(bb);
  }
  for (size_t i = 0; i < entries.size(); ++i)
    for (const std::string& label : entries[i]->labels)
      labelBlocks_[label] = segs[i + 1];
  llvm::SwitchInst* sw = b_.CreateSwitch(sel, segs[0], entries.size());
  for (size_t i = 0; i < entries.size(); ++i)
    sw->addCase(llvm::ConstantInt::get(b_.getInt64Ty(), i + 1), segs[i + 1]);

  // Segment 0 is the procedure's own start; each ENTRY begins the next segment.
  // Segments never fall through: close each one by branching to a per-segment
  // return pad that returns the impl's single result type (void segments return
  // void). Sema already rejected a RETURN whose shape disagrees with its own
  // segment, so every explicit RETURN in a segment matches the pad it reaches.
  llvm::Type* implRet = retLLVM;
  std::vector<llvm::BasicBlock*> retPads;
  for (size_t i = 0; i <= entries.size(); ++i) {
    retPads.push_back(llvm::BasicBlock::Create(ctx_, "e.ret." + std::to_string(i), impl));
  }
  size_t seg = 0;
  startBlock(segs[0]);
  for (auto& st : p->body) {
    if (st && st->kind == HStmt::Entry) {
      if (!blockTerminated(b_.GetInsertBlock()))
        b_.CreateBr(retPads[seg]);
      ++seg;
      startBlock(segs[seg]);
      continue;
    }
    emitStmt(st.get());
  }
  if (!blockTerminated(b_.GetInsertBlock()))
    b_.CreateBr(retPads[seg]);
  for (size_t i = 0; i < retPads.size(); ++i) {
    b_.SetInsertPoint(retPads[i]);
    // Rule (91): segment exit is procedure exit for handler scoping.
    if (curOnDepth_)
      b_.CreateCall(runtimeFn("pli_on_reset_error"),
                    {b_.CreateLoad(b_.getInt64Ty(), curOnDepth_, "ondepth")});
    if (implRet->isVoidTy())
      b_.CreateRetVoid();
    else
      b_.CreateRet(llvm::Constant::getNullValue(implRet));
  }
  curOnDepth_ = nullptr;
  curFn_ = nullptr;
}

std::string IRGen::entryIrName(const std::string& proc, const std::string& parent,
                               const std::string& entry) {
  return "@PLI_" + (parent.empty() ? std::string() : parent + "$") + proc + "$entry$" + entry;
}

// ---------------------------------------------------------------------------
// ON ERROR (rules (91)-(94))
// ---------------------------------------------------------------------------

// Assign dense handler ids (1-based per condition; 0 means SYSTEM) to every
// established (non-SYSTEM) ON-unit in emission order.
static void assignOnIdsStmt(HStmt* s, std::map<int, int>& next) {
  if (!s)
    return;
  if (s->kind == HStmt::On && !s->isSystem)
    s->onIndex = ++next[s->condKey];
  assignOnIdsStmt(s->thenS.get(), next);
  assignOnIdsStmt(s->elseS.get(), next);
  assignOnIdsStmt(s->unit.get(), next);
  for (auto& b : s->body)
    assignOnIdsStmt(b.get(), next);
}

void IRGen::assignOnIds(HProgram& prog) {
  std::map<int, int> next;
  for (auto& p : prog.procs)
    for (auto& b : p->body)
      assignOnIdsStmt(b.get(), next);
}

// Collect established ON-units keyed by (condition key, handler id).
static void collectOnStmts(HStmt* s, std::map<std::pair<int, int>, HStmt*>& out) {
  if (!s)
    return;
  if (s->kind == HStmt::On && !s->isSystem)
    out[{s->condKey, s->onIndex}] = s;
  collectOnStmts(s->thenS.get(), out);
  collectOnStmts(s->elseS.get(), out);
  collectOnStmts(s->unit.get(), out);
  for (auto& b : s->body)
    collectOnStmts(b.get(), out);
}

void IRGen::declareOnHandlers(HProgram& prog) {
  std::map<std::pair<int, int>, HStmt*> byId;
  for (auto& p : prog.procs)
    for (auto& b : p->body)
      collectOnStmts(b.get(), byId);
  bool first = true;
  int lastKey = 0;
  for (auto& [keyId, _] : byId) {
    // A fresh function list per condition; ids restart at 1 within a key.
    if (first || keyId.first != lastKey) {
      onHandlers_[keyId.first] = {};
      lastKey = keyId.first;
      first = false;
    }
    llvm::FunctionType* ft = llvm::FunctionType::get(b_.getVoidTy(), false);
    std::string name;
    if (keyId.first == 0)
      name = "PLI_ON_" + std::to_string(keyId.second);
    else if (keyId.first == Stmt::kSizeCondKey)
      name = "PLI_ON_SIZE_" + std::to_string(keyId.second);
    else if (keyId.first == Stmt::kSubscriptrangeCondKey)
      name = "PLI_ON_SUBSCRIPT_" + std::to_string(keyId.second);
    else if (keyId.first == Stmt::kZerodivideCondKey)
      name = "PLI_ON_ZERODIVIDE_" + std::to_string(keyId.second);
    else
      name = "PLI_ONC_" + std::to_string(keyId.first) + "_" + std::to_string(keyId.second);
    onHandlers_[keyId.first].push_back(llvm::Function::Create(
        ft, llvm::Function::InternalLinkage, name, &mod_));
  }
}

// Fill every handler function body. A handler runs without the establishing
// frame (sema rejected automatic-variable access), so only globals are seeded;
// labels inside the unit get their own blocks.
void IRGen::emitOnHandlers(HProgram& prog) {
  for (auto& p : prog.procs) {
    std::map<std::pair<int, int>, HStmt*> byId;
    for (auto& b : p->body)
      collectOnStmts(b.get(), byId);
    for (auto& [keyId, s] : byId) {
      llvm::Function* fn = onHandlers_[keyId.first][(size_t)keyId.second - 1];
      curFn_ = fn;
      curProc_ = p.get();
      curRetTy_ = Type::voidTy();
      inHandler_ = true;
      llvm::AllocaInst* savedDepth = curOnDepth_;
      curOnDepth_ = nullptr;
      labelBlocks_.clear();
      symAddr_.clear();
      for (Symbol* gs : sema_.storage())
        if (gs->isStatic && gs->kind == Symbol::Var)
          symAddr_[gs] = mod_.getGlobalVariable(gs->irName.substr(1), true);
      llvm::BasicBlock* entry = llvm::BasicBlock::Create(ctx_, "entry", fn);
      b_.SetInsertPoint(entry);
      collectGotoBlocks(s->unit.get());
      emitStmt(s->unit.get());
      if (!blockTerminated(b_.GetInsertBlock()))
        b_.CreateRetVoid();
      curOnDepth_ = savedDepth;
      inHandler_ = false;
      curFn_ = nullptr;
      curProc_ = nullptr;
    }
  }
}

// Establish a handler: push its id, or 0 for the system action.
void IRGen::emitOn(HStmt* s) {
  long long id = s->isSystem ? 0 : s->onIndex;
  if (s->condKey == 0)
    b_.CreateCall(runtimeFn("pli_on_push_error"), {i64(id)});
  else
    b_.CreateCall(runtimeFn("pli_on_push_cond"), {i64(s->condKey), i64(id)});
}

// Raise a condition: without an established handler take the system action
// (abort); otherwise run the topmost handler for that condition, then resume
// after the SIGNAL. Only ERROR touches ONCODE.
void IRGen::emitSignal(HStmt* s) {
  const std::vector<llvm::Function*>& handlers = onHandlers_[s->condKey];
  llvm::Value* top = s->condKey == 0
                         ? b_.CreateCall(runtimeFn("pli_on_top_error"), {}, "ontop")
                         : b_.CreateCall(runtimeFn("pli_on_top_cond"), {i64(s->condKey)},
                                         "ontop");
  llvm::Value* none = b_.CreateICmpEQ(top, i64(0), "onnosystem");
  llvm::BasicBlock* defBB = llvm::BasicBlock::Create(ctx_, "on.default", curFn_);
  llvm::BasicBlock* dspBB = llvm::BasicBlock::Create(ctx_, "on.dispatch", curFn_);
  llvm::BasicBlock* resBB = llvm::BasicBlock::Create(ctx_, "on.resume", curFn_);
  b_.CreateCondBr(none, defBB, dspBB);
  b_.SetInsertPoint(defBB);
  std::string msg;
  if (s->condKey == 0)
    msg = "SIGNAL ERROR";
  else if (s->condKey == Stmt::kSizeCondKey)
    msg = "SIGNAL SIZE";
  else if (s->condKey == Stmt::kSubscriptrangeCondKey)
    msg = "SIGNAL SUBSCRIPTRANGE";
  else if (s->condKey == Stmt::kZerodivideCondKey)
    msg = "SIGNAL ZERODIVIDE";
  else
    msg = "SIGNAL CONDITION(" + s->condName + ")";
  b_.CreateCall(runtimeFn("pli_signal_error"), {globalString(msg)});
  b_.CreateUnreachable();
  b_.SetInsertPoint(dspBB);
  llvm::SwitchInst* sw = b_.CreateSwitch(top, resBB, handlers.size());
  for (size_t i = 0; i < handlers.size(); ++i) {
    llvm::BasicBlock* hbb =
        llvm::BasicBlock::Create(ctx_, "on.handle." + std::to_string(i + 1), curFn_);
    sw->addCase(llvm::ConstantInt::get(b_.getInt64Ty(), (long long)i + 1), hbb);
    b_.SetInsertPoint(hbb);
    if (s->condKey == 0)
      b_.CreateCall(runtimeFn("pli_set_oncode"), {i32(1)});
    b_.CreateCall(handlers[i], {});
    if (s->condKey == 0)
      b_.CreateCall(runtimeFn("pli_set_oncode"), {i32(0)});
    b_.CreateBr(resBB);
  }
  b_.SetInsertPoint(resBB);
}

// ---------------------------------------------------------------------------
// statements
// ---------------------------------------------------------------------------
void IRGen::emitStmt(HStmt* s) {
  if (!s)
    return;
  if (!s->labels.empty()) {
    startBlock(labelBlocks_[s->labels.front()]);
  } else if (blockTerminated(b_.GetInsertBlock())) {
    newBlock(); // unreachable code (e.g. after STOP): start a fresh block
  }
  // Condition enable-state (rules (60)-(63), ADR-110): each statement sees
  // its own (NOSIZE) OR-inherited through enclosing statements, so a
  // prefixed group covers its body. Balanced by construction (single exit).
  noSizeStack_.push_back(s->noSize || (!noSizeStack_.empty() && noSizeStack_.back()));
  switch (s->kind) {
  case HStmt::Null:
  case HStmt::Declare:
  case HStmt::Entry: // segment marker; handled by emitMultiEntryProc
    break;
  case HStmt::Assign:
    emitAssign(s);
    break;
  case HStmt::If:
    emitIf(s);
    break;
  case HStmt::Group:
    for (auto& b : s->body)
      emitStmt(b.get());
    break;
  case HStmt::Begin: {
    // A BEGIN block scopes ON establishments (rule (91)): restore the entry
    // depth when the block exits. Skipped when the module establishes no
    // handlers, so the common path stays free.
    llvm::Value* blkDepth = nullptr;
    if (!onHandlers_.empty())
      blkDepth = b_.CreateCall(runtimeFn("pli_on_depth_error"), {}, "onblkdepth");
    for (auto& b : s->body)
      emitStmt(b.get());
    if (blkDepth)
      b_.CreateCall(runtimeFn("pli_on_reset_error"), {blkDepth});
    break;
  }
  case HStmt::DoWhile:
    emitDoWhile(s);
    break;
  case HStmt::DoIter:
    emitDoIter(s);
    break;
  case HStmt::Put:
    emitPut(s);
    break;
  case HStmt::Display: {
    // Rule (114): one scalar value plus a newline (REPLY stays diagnosed).
    Val v = emitExpr(s->value.get());
    switch (v.ty.k) {
    case TK::Char:
      b_.CreateCall(runtimeFn("pli_display_char"), {v.ptr, v.len});
      break;
    case TK::Float:
      b_.CreateCall(runtimeFn("pli_display_float"), {v.reg});
      break;
    case TK::Bit: {
      llvm::Value* bit = b_.CreateZExt(v.reg, b_.getInt8Ty(), "bit");
      b_.CreateCall(runtimeFn("pli_display_bit"), {bit});
      break;
    }
    case TK::FixedBin:
    case TK::FixedDec:
      if (v.ty.k == TK::FixedDec && v.ty.scale > 0)
        b_.CreateCall(runtimeFn("pli_display_decfixed"), {toI64(v), i64(v.ty.scale)});
      else
        b_.CreateCall(runtimeFn("pli_display_fixed"), {toI64(v)});
      break;
    case TK::Pointer:
      d_.error(s->loc, "a POINTER value cannot be written with DISPLAY in this stage", "(114)");
      break;
    case TK::Complex:
      b_.CreateCall(runtimeFn("pli_display_complex"),
                    {b_.CreateExtractValue(v.cpx, 0, "cpx.re"),
                     b_.CreateExtractValue(v.cpx, 1, "cpx.im")});
      break;
    default:
      break; // array/struct operands are diagnosed by sema
    }
    break;
  }
  case HStmt::Get:
    emitGet(s);
    break;
  case HStmt::CallS:
    emitCall(s);
    break;
  case HStmt::Allocate:
    emitAllocate(s);
    break;
  case HStmt::Free:
    emitFree(s);
    break;
  case HStmt::Open:
    emitOpen(s);
    break;
  case HStmt::Close:
    emitClose(s);
    break;
  case HStmt::Read:
    emitRecordRead(s);
    break;
  case HStmt::Write:
    emitRecordWrite(s);
    break;
  case HStmt::Return: {
    // Rule (91): procedure exit restores the entry ERROR depth, popping any
    // handlers this procedure established.
    if (curOnDepth_)
      b_.CreateCall(runtimeFn("pli_on_reset_error"),
                    {b_.CreateLoad(b_.getInt64Ty(), curOnDepth_, "ondepth")});
    if (curProc_->isFunction && curProc_->retTy.isStruct()) {
      // Structure-valued function (rule 127): copy the returned structure's
      // storage into the caller's result buffer, then return void.
      Val v = emitExpr(s->value.get());
      llvm::Value* sz = i64(mod_.getDataLayout().getTypeStoreSize(llvmTy(curProc_->retTy)));
      b_.CreateMemCpy(structRetPtr_, llvm::MaybeAlign(), v.ptr, llvm::MaybeAlign(), sz);
      b_.CreateRetVoid();
    } else if (!curRetTy_.isVoid()) {
      // Valued return: the impl's common entry type (rule (56)) may be valued
      // even when the primary entry is not a function.
      Val v = emitExpr(s->value.get());
      Val rv = convert(v, curRetTy_, s->loc);
      llvm::Value* reg = rv.reg;
      if (curRetTy_.isBit()) { // BIT returns are held in i8
        reg = b_.CreateZExt(reg, b_.getInt8Ty(), "retz");
      }
      b_.CreateRet(reg);
    } else {
      b_.CreateRetVoid();
    }
    break;
  }
  case HStmt::Stop:
    b_.CreateCall(runtimeFn("pli_stop"), {});
    b_.CreateUnreachable();
    break;
  case HStmt::Leave:
  case HStmt::Iterate: {
    // Extension (ADR-105): branch to the enclosing iterative group's end
    // (LEAVE) or re-entry (ITERATE: condition for WHILE, step for DO-loop).
    // Sema validated the target, so it is always present here.
    const LoopTargets* t = nullptr;
    for (auto it = loopStack_.rbegin(); it != loopStack_.rend(); ++it)
      if (s->name.empty() ||
          std::find(it->labels.begin(), it->labels.end(), s->name) != it->labels.end()) {
        t = &(*it);
        break;
      }
    if (t)
      branch(s->kind == HStmt::Leave ? t->breakBB : t->contBB);
    break;
  }
  case HStmt::On:
    emitOn(s);
    break;
  case HStmt::Revert:
    // REVERT pops one handler for the condition (a no-op when none).
    if (s->condKey == 0)
      b_.CreateCall(runtimeFn("pli_on_pop_error"), {});
    else
      b_.CreateCall(runtimeFn("pli_on_pop_cond"), {i64(s->condKey)});
    break;
  case HStmt::Signal:
    emitSignal(s);
    break;
  case HStmt::Goto:
    b_.CreateBr(labelBlocks_[s->name]);
    break;
  }
  noSizeStack_.pop_back();
}

void IRGen::emitAssign(HStmt* s) {
  if (!s->target)
    return;
  // Multiple assignment (rule 86): a, b, c = e — evaluate the RHS once and
  // store it to every target. Sema restricted targets to scalar variables and
  // array elements of one shared type, so each store reuses the scalar paths.
  if (!s->extraTargets.empty()) {
    Val v = emitExpr(s->value.get());
    auto storeOne = [&](HExpr* t) {
      if (t->kind == HExpr::Subscript && t->sym) {
        if (!t->memberPath.empty()) {
          const Type& arr = memberType(t->sym, t->memberPath);
          const Type& el = t->ty;
          if (el.isChar()) {
            d_.error(s->loc, "arrays of CHARACTER members are not implemented in this stage",
                     "(12)");
            return;
          }
          llvm::Value *ub = nullptr, *lb = nullptr;
          llvm::Value* base = arr.isDynamic()
                                  ? dynamicMemberBase(t->sym, t->memberPath, s->loc, ub, lb)
                                  : memberAddr(t->sym, t->memberPath, s->loc);
          llvm::Value* addr = arrayElementAddr(arr, base, t->args, s->loc, ub, lb);
          storeScalarTo(addr, el, convert(v, el, s->loc));
          return;
        }
        storeArrayElement(t->sym, t->args, v, s->loc);
        return;
      }
      if (t->kind == HExpr::VarRef && t->sym) {
        if (!t->memberPath.empty()) {
          const Type& leaf = t->ty;
          if (leaf.isChar()) {
            d_.error(s->loc, "CHARACTER structure members are not implemented in this stage",
                     "(11)");
            return;
          }
          // A locator-qualified target P->X.FIELD stores off the loaded pointer.
          llvm::Value* addr =
              t->locPtr ? locatorMemberAddr(t->sym, t->memberPath, emitExpr(t->locPtr.get()).reg)
                        : memberAddr(t->sym, t->memberPath, s->loc);
          storeScalarTo(addr, leaf, convert(v, leaf, s->loc));
          return;
        }
        storeTo(t->sym, v, s->loc);
        return;
      }
    };
    storeOne(s->target.get());
    for (auto& t : s->extraTargets)
      storeOne(t.get());
    return;
  }
  // BY NAME assignment (rule 86): S = T BY NAME copies each same-named member
  // of the target structure from the source structure, regardless of layout.
  if (s->byName) {
    HExpr* t = s->target.get();
    HExpr* v = s->value.get();
    if (t->kind != HExpr::VarRef || !t->sym || !t->ty.isStruct() || v->kind != HExpr::VarRef ||
        !v->sym || !v->ty.isStruct()) {
      d_.error(s->loc, "BY NAME assignment requires two structure references", "(86)");
      return;
    }
    llvm::Value* dst =
        t->memberPath.empty() ? addressOf(t->sym) : memberAddr(t->sym, t->memberPath, s->loc);
    llvm::Value* src =
        v->memberPath.empty() ? addressOf(v->sym) : memberAddr(v->sym, v->memberPath, s->loc);
    emitByNameCopy(dst, src, t->ty, v->ty, s->loc, t->sym, t->memberPath, v->sym,
                   v->memberPath);
    return;
  }
  if (s->target->kind == HExpr::Call && s->target->name == "SUBSTR") {
    HExpr* t = s->target.get();
    Val sv = emitExpr(t->args[0].get());
    Symbol* sym = t->args[0]->sym;
    Val start = emitExpr(t->args[1].get());
    Val len = emitExpr(t->args[2].get());
    Val rhs = emitExpr(s->value.get());
    b_.CreateCall(runtimeFn("pli_substr_assign"),
                  {sv.ptr, i64(sym->ty.len), toI64(start), toI64(len), rhs.ptr, rhs.len});
    return;
  }
  // Cross-section assignment (rule 126): B = A(i, *, ...) — the right-hand side
  // is a reduced-dim array value produced by '*' subscripts. Copy it into the
  // whole array target; anywhere else a cross-section is rejected in emitExpr.
  {
    HExpr* x = s->value.get();
    if (x->kind == HExpr::Subscript && x->sym && x->ty.isArray()) {
      bool isCross = false;
      for (auto& a : x->args)
        if (a->kind == HExpr::Star) {
          isCross = true;
          break;
        }
      if (isCross) {
        if (s->target->kind == HExpr::VarRef && s->target->sym && s->target->ty.isArray()) {
          emitCrossSectionAssign(s->target.get(), x, s->loc);
          return;
        }
        d_.error(s->loc, "cross-section assignment requires a whole-array target", "(126)");
        return;
      }
    }
  }
  // Array element assignment: A(i,j,...) = e (rule 126).
  if (s->target->kind == HExpr::Subscript && s->target->sym) {
    HExpr* t = s->target.get();
    Val v = emitExpr(s->value.get());
    if (!t->memberPath.empty()) {
      // An array of structures target arr(i).x = e (rules 124,126): subscript
      // to the element structure, then store through the member path.
      if (t->sym->ty.isArray()) {
        llvm::Value* elem = arrayElementAddr(t->sym->ty, addressOf(t->sym), t->args, s->loc);
        llvm::Value* addr = elementMemberAddr(t->sym, t->memberPath, elem);
        const Type& el = t->ty;
        if (el.isChar()) {
          d_.error(s->loc, "CHARACTER structure members are not implemented in this stage", "(11)");
          return;
        }
        storeScalarTo(addr, el, convert(v, el, s->loc));
        return;
      }
      // A subscripted member array S.A(i) = e (rules 124,126): store through
      // the member array field, bounds-checked like any array element.
      const Type& arr = memberType(t->sym, t->memberPath);
      const Type& el = t->ty;
      if (el.isChar()) {
        d_.error(s->loc, "arrays of CHARACTER members are not implemented in this stage", "(12)");
        return;
      }
      llvm::Value *ub = nullptr, *lb = nullptr;
      llvm::Value* base = arr.isDynamic() ? dynamicMemberBase(t->sym, t->memberPath, s->loc, ub, lb)
                                          : memberAddr(t->sym, t->memberPath, s->loc);
      llvm::Value* addr = arrayElementAddr(arr, base, t->args, s->loc, ub, lb);
      storeScalarTo(addr, el, convert(v, el, s->loc));
      return;
    }
    // An iSUB-DEFINED array target Y(k) (rule 134) has no storage of its own:
    // store into the live base element X(...k...).
    if (t->sym->definedBase && t->sym->definedIsubAxis >= 0) {
      llvm::Value* addr = definedSubElementAddr(t->sym, t->args, s->loc);
      storeScalarTo(addr, t->ty, convert(v, t->ty, s->loc));
      return;
    }
    storeArrayElement(t->sym, t->args, v, s->loc);
    return;
  }
  // Whole-structure assignment S = T (rule 127): copy the source structure's
  // storage into the target. Both sides are whole-structure references (a
  // top-level variable or a qualified member); sema checked identical shape.
  if (s->target->kind == HExpr::VarRef && s->target->sym && s->target->ty.isStruct()) {
    // Whole-structure assignment (rule 127): copy the source structure's storage
    // into the target. The RHS may be any structure-valued expression (a whole
    // variable, a minor-structure member, or a structure-returning call).
    HExpr* t = s->target.get();
    Val v = emitExpr(s->value.get());
    if (!v.ty.isStruct() || !v.ptr) {
      d_.error(s->loc, "right-hand side of a whole-structure assignment must be a structure value",
               "(127)");
      return;
    }
    llvm::Value* dst =
        t->memberPath.empty() ? addressOf(t->sym) : memberAddr(t->sym, t->memberPath, s->loc);
    llvm::Type* sty = llvmTy(t->ty);
    llvm::Value* sz = i64(mod_.getDataLayout().getTypeStoreSize(sty));
    // Deep copy when a dynamic member is present (rule (13), ADR-091): the
    // struct field holds a buffer pointer, so save the target pointers,
    // copy the storage, restore them, then copy each buffer's contents.
    std::vector<std::vector<unsigned>> dynPaths;
    std::vector<unsigned> dynPrefix;
    collectDynMemberPaths(t->ty, dynPrefix, dynPaths);
    if (dynPaths.empty()) {
      b_.CreateMemCpy(dst, llvm::MaybeAlign(), v.ptr, llvm::MaybeAlign(), sz);
      return;
    }
    HExpr* sv = s->value.get();
    if (!sv || sv->kind != HExpr::VarRef || !sv->sym || !sv->ty.isStruct() || sv->locPtr) {
      d_.error(s->loc,
               "a whole-structure source with a dynamic member must be a plain variable here",
               "(13)");
      return;
    }
    // One helper GEPs from an arbitrary struct base through a relative field
    // path (memberAddr always starts from the symbol's own base).
    auto fieldAddr = [&](llvm::Value* base, const Type& root,
                         const std::vector<unsigned>& rel) {
      llvm::Value* addr = base;
      const Type* cur = &root;
      for (unsigned f : rel) {
        addr = b_.CreateStructGEP(llvmTy(*cur), addr, f, "dcp.f");
        cur = &cur->members[f]->ty;
      }
      return addr;
    };
    std::vector<llvm::Value*> savedPtrs;
    savedPtrs.reserve(dynPaths.size());
    for (const auto& rel : dynPaths)
      savedPtrs.push_back(b_.CreateLoad(b_.getPtrTy(), fieldAddr(dst, t->ty, rel), "dcp.sv"));
    b_.CreateMemCpy(dst, llvm::MaybeAlign(), v.ptr, llvm::MaybeAlign(), sz);
    for (size_t i = 0; i < dynPaths.size(); ++i)
      b_.CreateStore(savedPtrs[i], fieldAddr(dst, t->ty, dynPaths[i]));
    for (const auto& rel : dynPaths) {
      const Type& arr = structPathType(t->ty, rel);
      const Type& el = arr.elementType();
      std::vector<unsigned> dstFull = t->memberPath;
      dstFull.insert(dstFull.end(), rel.begin(), rel.end());
      std::vector<unsigned> srcFull = sv->memberPath;
      srcFull.insert(srcFull.end(), rel.begin(), rel.end());
      auto dstIt = memberDyn_.find(MemberDyn{t->sym, dstFull});
      auto srcIt = memberDyn_.find(MemberDyn{sv->sym, srcFull});
      llvm::Value* dub = dstIt != memberDyn_.end() ? dstIt->second.ub : nullptr;
      llvm::Value* dlb = dstIt != memberDyn_.end() ? dstIt->second.lb : nullptr;
      llvm::Value* sub = srcIt != memberDyn_.end() ? srcIt->second.ub : nullptr;
      llvm::Value* slb = srcIt != memberDyn_.end() ? srcIt->second.lb : nullptr;
      if (!dub || !sub) {
        d_.error(s->loc, "a whole-structure source with a dynamic member is not addressable here",
                 "(13)");
        return;
      }
      const Dim& d0 = arr.dims[0];
      llvm::Value* dl = d0.lbDyn && dlb ? dlb : i64(d0.lb);
      llvm::Value* du = dub;
      llvm::Value* sl = d0.lbDyn && slb ? slb : i64(d0.lb);
      llvm::Value* su = sub;
      llvm::Value* dext = b_.CreateAdd(b_.CreateSub(du, dl, "dcp.e1"), i64(1), "dcp.dext");
      llvm::Value* sext = b_.CreateAdd(b_.CreateSub(su, sl, "dcp.e2"), i64(1), "dcp.sext");
      long long rest = 1;
      for (size_t k = 1; k < arr.dims.size(); ++k)
        rest *= (arr.dims[k].ub - arr.dims[k].lb + 1);
      if (rest != 1) {
        dext = b_.CreateMul(dext, i64(rest), "dcp.dall");
        sext = b_.CreateMul(sext, i64(rest), "dcp.sall");
      }
      // Mismatched live extents would overflow the target: trap loudly
      // through the subscript path rather than silently truncating.
      std::string id = std::to_string(n_++);
      llvm::BasicBlock* failL = llvm::BasicBlock::Create(ctx_, "dcp.fail." + id, curFn_);
      llvm::BasicBlock* okL = llvm::BasicBlock::Create(ctx_, "dcp.ok." + id, curFn_);
      b_.CreateCondBr(b_.CreateICmpNE(dext, sext, "dcp.mis"), failL, okL);
      startBlock(failL);
      // A shape mismatch has no index to clamp: a handled trap notifies the
      // handler, then aborts (rule 94).
      llvm::BasicBlock* abL = llvm::BasicBlock::Create(ctx_, "dcp.abort." + id, curFn_);
      emitCondTrap(Stmt::kSubscriptrangeCondKey, "pli_subscript_oob", "sub", abL);
      b_.SetInsertPoint(abL);
      b_.CreateUnreachable();
      startBlock(okL);
      llvm::Value* dbuf = b_.CreateLoad(b_.getPtrTy(), fieldAddr(dst, t->ty, rel), "dcp.dp");
      llvm::Value* sbuf = b_.CreateLoad(b_.getPtrTy(), fieldAddr(v.ptr, sv->ty, rel), "dcp.sp");
      llvm::Value* nbytes =
          b_.CreateMul(sext, i64(mod_.getDataLayout().getTypeStoreSize(llvmTy(el))), "dcp.n");
      b_.CreateMemCpy(dbuf, llvm::MaybeAlign(), sbuf, llvm::MaybeAlign(), nbytes);
    }
    return;
  }
  // Qualified member assignment: S.A = e (rule 124).
  if (s->target->kind == HExpr::VarRef && s->target->sym && !s->target->memberPath.empty()) {
    HExpr* t = s->target.get();
    const Type& leaf = t->ty;
    if (leaf.isChar()) {
      d_.error(s->loc, "CHARACTER structure members are not implemented in this stage", "(11)");
      return;
    }
    Val v = emitExpr(s->value.get());
    llvm::Value* addr = memberAddr(t->sym, t->memberPath, s->loc);
    storeScalarTo(addr, leaf, convert(v, leaf, s->loc));
    return;
  }
  if (s->target->kind != HExpr::VarRef || !s->target->sym)
    return;
  Val v = emitExpr(s->value.get());
  storeTo(s->target->sym, v, s->loc);
}

// ALLOCATE (rule 87): heap-allocate a based structure (rule 88, SET option) and
// store its address in the pointer target.
void IRGen::emitAllocate(HStmt* s) {
  for (size_t i = 0; i < s->allocBase.size(); ++i) {
    Symbol* bsym = s->allocBase[i]->sym;
    // The LLVM alloc size of the based structure (bytes) sizes the heap block.
    llvm::Value* sz = i64(mod_.getDataLayout().getTypeAllocSize(llvmTy(bsym->ty)).getFixedValue());
    llvm::Value* p = b_.CreateCall(runtimeFn("pli_alloc"), {sz}, "heap");
    HExpr* set = s->allocSet[i].get();
    storeTo(set->sym, Val{set->ty, p}, set->loc);
  }
}

// FREE (rule 90): release the heap storage addressed by a based pointer — the
// explicit locator when given, else the based variable's own BASED pointer.
void IRGen::emitFree(HStmt* s) {
  for (auto& f : s->freeBase) {
    Symbol* bsym = f->sym;
    llvm::Value* addr = f->locPtr ? emitExpr(f->locPtr.get()).reg : addressOf(bsym);
    b_.CreateCall(runtimeFn("pli_free"), {addr});
  }
}

// OPEN (rules 100,101): open the FILE variable's slot against the TITLE name.
// The slot is a compile-time constant on the symbol; mode 0 = INPUT, 1 = OUTPUT.
// A RECORD SEQUENTIAL open (rules (101),(112)) uses the binary record runtime.
void IRGen::emitOpen(HStmt* s) {
  Symbol* f = s->fileSym;
  llvm::Value* name = globalString(s->openTitle);
  b_.CreateCall(
      runtimeFn(s->openRecord ? "pli_file_open_record" : "pli_file_open"),
      {i64(f->fileSlot), name, i64((long long)s->openTitle.size()), i64(s->openInput ? 0 : 1)});
}

// WRITE (rules (112),(113)): append one fixed-size binary record — FIXED as 8
// bytes, FLOAT as 8 bytes, BIT(1) as 1 byte, CHAR(n) as n raw bytes.
void IRGen::emitRecordWrite(HStmt* s) {
  llvm::Value* slot = i64(s->fileSym->fileSlot);
  Val v = emitExpr(s->value.get());
  switch (v.ty.k) {
  case TK::FixedBin:
  case TK::FixedDec:
    b_.CreateCall(runtimeFn("pli_record_write_fixed"), {slot, toI64(v)});
    break;
  case TK::Float:
    b_.CreateCall(runtimeFn("pli_record_write_float"), {slot, v.reg});
    break;
  case TK::Bit: {
    llvm::Value* bit = b_.CreateZExt(v.reg, b_.getInt8Ty(), "rbit");
    b_.CreateCall(runtimeFn("pli_record_write_bit"), {slot, bit});
    break;
  }
  case TK::Char:
    b_.CreateCall(runtimeFn("pli_record_write_char"), {slot, v.ptr, v.len});
    break;
  default:
    break; // other types are diagnosed by sema
  }
}

// READ (rules (112),(113)): consume one fixed-size binary record into a plain
// scalar variable; a short read (EOF) raises ERROR in the runtime, since ON
// ENDFILE stays diagnosed.
void IRGen::emitRecordRead(HStmt* s) {
  llvm::Value* slot = i64(s->fileSym->fileSlot);
  HExpr* t = s->target.get();
  const Type& ty = t->ty;
  Val v;
  switch (ty.k) {
  case TK::FixedBin:
  case TK::FixedDec:
    v.reg = b_.CreateCall(runtimeFn("pli_record_read_fixed"), {slot});
    v.ty = Type::fixedBin(63, 0);
    storeTo(t->sym, v, s->loc);
    break;
  case TK::Float:
    v.reg = b_.CreateCall(runtimeFn("pli_record_read_float"), {slot});
    v.ty = Type::flt(6);
    storeTo(t->sym, v, s->loc);
    break;
  case TK::Bit: {
    llvm::Value* b = b_.CreateCall(runtimeFn("pli_record_read_bit"), {slot});
    v.reg = b_.CreateTrunc(b, b_.getInt1Ty(), "rbit");
    v.ty = Type::bit();
    storeTo(t->sym, v, s->loc);
    break;
  }
  case TK::Char: {
    // Fill a reusable entry buffer, then assign into the target.
    llvm::Value* buf = entryAlloca(llvm::ArrayType::get(b_.getInt8Ty(), ty.len), "rch");
    b_.CreateCall(runtimeFn("pli_record_read_char"), {slot, buf, i64(ty.len)});
    v.ptr = buf;
    v.len = i64(ty.len);
    v.ty = ty;
    storeTo(t->sym, v, s->loc);
    break;
  }
  default:
    break; // other types are diagnosed by sema
  }
}

// CLOSE (rules 102,103): close the FILE variable's slot.
void IRGen::emitClose(HStmt* s) {
  b_.CreateCall(runtimeFn("pli_file_close"), {i64(s->fileSym->fileSlot)});
}

void IRGen::emitIf(HStmt* s) {
  Val c = emitExpr(s->cond.get());
  llvm::Value* cond = toI1(c, s->loc);
  std::string id = std::to_string(n_++);
  llvm::BasicBlock* thenL = llvm::BasicBlock::Create(ctx_, "if.then." + id, curFn_);
  llvm::BasicBlock* endL = llvm::BasicBlock::Create(ctx_, "if.end." + id, curFn_);
  llvm::BasicBlock* elseL =
      s->elseS ? llvm::BasicBlock::Create(ctx_, "if.else." + id, curFn_) : nullptr;
  b_.CreateCondBr(cond, thenL, s->elseS ? elseL : endL);

  startBlock(thenL);
  emitStmt(s->thenS.get());
  branch(endL);

  if (s->elseS) {
    startBlock(elseL);
    emitStmt(s->elseS.get());
    branch(endL);
  }
  startBlock(endL);
}

void IRGen::emitDoWhile(HStmt* s) {
  std::string id = std::to_string(n_++);
  llvm::BasicBlock* condL = llvm::BasicBlock::Create(ctx_, "do.cond." + id, curFn_);
  llvm::BasicBlock* bodyL = llvm::BasicBlock::Create(ctx_, "do.body." + id, curFn_);
  llvm::BasicBlock* endL = llvm::BasicBlock::Create(ctx_, "do.end." + id, curFn_);
  if (s->until) {
    // DO UNTIL (extension, ADR-106): the body runs first, then a true
    // condition exits (LEAVE targets endL, ITERATE re-tests via condL).
    branch(bodyL);
    startBlock(bodyL);
    loopStack_.push_back({s->labels, endL, condL});
    for (auto& b : s->body)
      emitStmt(b.get());
    loopStack_.pop_back();
    branch(condL);
    startBlock(condL);
    Val c = emitExpr(s->cond.get());
    b_.CreateCondBr(toI1(c, s->loc), endL, bodyL);
    startBlock(endL);
    return;
  }
  branch(condL);
  startBlock(condL);
  Val c = emitExpr(s->cond.get());
  b_.CreateCondBr(toI1(c, s->loc), bodyL, endL);
  startBlock(bodyL);
  loopStack_.push_back({s->labels, endL, condL});
  for (auto& b : s->body)
    emitStmt(b.get());
  loopStack_.pop_back();
  branch(condL);
  startBlock(endL);
}

// DO v = e1 [TO e2] [BY e3] [WHILE(e4)];                    rules (71)-(73)
void IRGen::emitDoIter(HStmt* s) {
  Symbol* ctl = s->sym;
  if (!ctl)
    return;
  const Type& ct = ctl->ty;
  std::string id = std::to_string(n_++);

  Val from = emitExpr(s->from.get());
  storeTo(ctl, from, s->loc);

  llvm::Value *toAddr = nullptr, *byAddr = nullptr;
  if (s->to) {
    Val to = convert(emitExpr(s->to.get()), ct, s->loc);
    toAddr = entryAlloca(llvmTy(ct), "do.to." + id);
    storeScalarTo(toAddr, ct, to);
  }
  Val by;
  if (s->by) {
    by = convert(emitExpr(s->by.get()), ct, s->loc);
  } else {
    by.ty = ct;
    by.reg = ct.k == TK::Float ? flt(1.0) : llvm::ConstantInt::get(llvmTy(ct), 1, true);
  }
  byAddr = entryAlloca(llvmTy(ct), "do.by." + id);
  storeScalarTo(byAddr, ct, by);

  llvm::BasicBlock* condL = llvm::BasicBlock::Create(ctx_, "do.cond." + id, curFn_);
  llvm::BasicBlock* bodyL = llvm::BasicBlock::Create(ctx_, "do.body." + id, curFn_);
  llvm::BasicBlock* stepL = llvm::BasicBlock::Create(ctx_, "do.step." + id, curFn_);
  llvm::BasicBlock* endL = llvm::BasicBlock::Create(ctx_, "do.end." + id, curFn_);
  llvm::BasicBlock* upL = s->to ? llvm::BasicBlock::Create(ctx_, "do.up." + id, curFn_) : nullptr;
  llvm::BasicBlock* downL =
      s->to ? llvm::BasicBlock::Create(ctx_, "do.down." + id, curFn_) : nullptr;
  llvm::BasicBlock* testL =
      s->to ? llvm::BasicBlock::Create(ctx_, "do.test." + id, curFn_) : nullptr;

  branch(condL);
  startBlock(condL);

  if (s->to) {
    Val cv = loadSym(ctl, ct);
    llvm::Value* tv = b_.CreateLoad(llvmTy(ct), toAddr, "to");
    llvm::Value* bv = b_.CreateLoad(llvmTy(ct), byAddr, "by");
    llvm::Value* zero = ct.k == TK::Float ? flt(0.0) : llvm::Constant::getNullValue(llvmTy(ct));
    llvm::Value* neg;
    if (ct.k == TK::Float)
      neg = b_.CreateFCmpOLT(bv, zero, "negstep");
    else
      neg = b_.CreateICmpSLT(bv, zero, "negstep");
    b_.CreateCondBr(neg, downL, upL);

    startBlock(upL);
    llvm::Value* cu = ct.k == TK::Float ? b_.CreateFCmpOLE(cv.reg, tv, "cmpup")
                                        : b_.CreateICmpSLE(cv.reg, tv, "cmpup");
    b_.CreateCondBr(cu, testL, endL);

    startBlock(downL);
    llvm::Value* cd = ct.k == TK::Float ? b_.CreateFCmpOGE(cv.reg, tv, "cmpdn")
                                        : b_.CreateICmpSGE(cv.reg, tv, "cmpdn");
    b_.CreateCondBr(cd, testL, endL);

    startBlock(testL);
  }

  if (s->cond) { // WHILE clause
    Val w = emitExpr(s->cond.get());
    b_.CreateCondBr(toI1(w, s->loc), bodyL, endL);
  } else {
    branch(bodyL);
  }

  startBlock(bodyL);
  loopStack_.push_back({s->labels, endL, stepL});
  for (auto& b : s->body)
    emitStmt(b.get());
  loopStack_.pop_back();
  branch(stepL);

  startBlock(stepL);
  Val cv = loadSym(ctl, ct);
  llvm::Value* bv = b_.CreateLoad(llvmTy(ct), byAddr, "by");
  llvm::Value* nx =
      ct.k == TK::Float ? b_.CreateFAdd(cv.reg, bv, "next") : b_.CreateAdd(cv.reg, bv, "next");
  Val nv;
  nv.ty = ct;
  nv.reg = nx;
  storeTo(ctl, nv, s->loc);
  branch(condL);

  startBlock(endL);
}

// Data-directed output (rule (106), QR1.5): each item prints as NAME=value,
// ", "-separated and ";"-terminated. Names are compile-time globals; values
// reuse the list-directed printers (the runtime suppresses the blank
// separator after a name). Sema restricted items to plain scalar variables,
// so any other shape here is already diagnosed.
void IRGen::emitPutDataItems(HStmt* s) {
  for (auto& item : s->items) {
    HExpr* t = item.get();
    llvm::Value* name = globalString(t->sym->name);
    b_.CreateCall(runtimeFn("pli_put_data_name"),
                  {name, i64((long long)t->sym->name.size())});
    Val v = emitExpr(item.get());
    switch (v.ty.k) {
    case TK::Char:
      b_.CreateCall(runtimeFn("pli_put_list_char"), {v.ptr, v.len});
      break;
    case TK::Float:
      b_.CreateCall(runtimeFn("pli_put_list_float"), {v.reg});
      break;
    case TK::Bit: {
      llvm::Value* bit = b_.CreateZExt(v.reg, b_.getInt8Ty(), "bit");
      b_.CreateCall(runtimeFn("pli_put_list_bit"), {bit});
      break;
    }
    case TK::FixedBin:
    case TK::FixedDec:
      if (v.ty.k == TK::FixedDec && v.ty.scale > 0)
        b_.CreateCall(runtimeFn("pli_put_list_decfixed"), {toI64(v), i64(v.ty.scale)});
      else
        b_.CreateCall(runtimeFn("pli_put_list_fixed"), {toI64(v)});
      break;
    default:
      break;
    }
  }
  b_.CreateCall(runtimeFn("pli_put_data_end"), {});
}

void IRGen::emitPut(HStmt* s) {
  if (s->page)
    b_.CreateCall(runtimeFn("pli_put_page"), {});
  if (s->skip) {
    llvm::Value* n = i64(1);
    if (s->skipCount) {
      Val v = emitExpr(s->skipCount.get());
      n = toI64(v);
    }
    b_.CreateCall(runtimeFn("pli_put_skip"), {n});
  }
  // STRING (rule 105) sink: route the list-directed output into the character
  // variable instead of SYSPRINT.
  llvm::Value *sdata = nullptr, *slen = nullptr;
  if (s->stringTarget) {
    HExpr* st = s->stringTarget.get();
    sdata =
        st->memberPath.empty() ? addressOf(st->sym) : memberAddr(st->sym, st->memberPath, s->loc);
    slen = i64(st->ty.len);
    b_.CreateCall(runtimeFn("pli_string_put_open"), {sdata, slen});
  }
  // FILE ( f ) (rule 105): route the list-directed output through the named
  // file's stream instead of SYSPRINT.
  if (s->fileSym)
    b_.CreateCall(runtimeFn("pli_put_select"), {i64(s->fileSym->fileSlot)});
  if (s->edit) {
    emitPutEditItems(s);
  } else if (s->data) {
    emitPutDataItems(s);
  } else {
    for (auto& item : s->items) {
      Val v = emitExpr(item.get());
      switch (v.ty.k) {
      case TK::Char:
        b_.CreateCall(runtimeFn("pli_put_list_char"), {v.ptr, v.len});
        break;
      case TK::Float:
        b_.CreateCall(runtimeFn("pli_put_list_float"), {v.reg});
        break;
      case TK::Bit: {
        llvm::Value* bit = b_.CreateZExt(v.reg, b_.getInt8Ty(), "bit");
        b_.CreateCall(runtimeFn("pli_put_list_bit"), {bit});
        break;
      }
      case TK::FixedBin:
      case TK::FixedDec:
        if (v.ty.k == TK::FixedDec && v.ty.scale > 0)
          b_.CreateCall(runtimeFn("pli_put_list_decfixed"), {toI64(v), i64(v.ty.scale)});
        else
          b_.CreateCall(runtimeFn("pli_put_list_fixed"), {toI64(v)});
        break;
      case TK::Void:
        break;
      case TK::Struct:
        // Whole-structure values are diagnosed in emitExpr (rule 127); a
        // structure never reaches list-directed output as a value.
        break;
      case TK::Pointer:
        d_.error(s->loc, "a POINTER value cannot be written with PUT LIST in this stage", "(110)");
        break;
      case TK::Complex:
        // Complex output (CM5): real, sign, imaginary magnitude, I.
        b_.CreateCall(runtimeFn("pli_put_list_complex"),
                      {b_.CreateExtractValue(v.cpx, 0, "cpx.re"),
                       b_.CreateExtractValue(v.cpx, 1, "cpx.im")});
        break;
      }
    }
  }
  if (s->stringTarget)
    b_.CreateCall(runtimeFn("pli_string_put_close"), {sdata, slen});
  if (s->fileSym)
    b_.CreateCall(runtimeFn("pli_put_unselect"), {});
}

// GET DATA (rule (106), QR1.5): read NAME=value pairs in any order, storing
// each into the matching data-list item and skipping unknown names. Sema
// restricted items to plain scalar variables, so names are compile-time
// globals and any other shape here is already diagnosed.
void IRGen::emitGetDataItems(HStmt* s) {
  llvm::Value* nameBuf = entryAlloca(llvm::ArrayType::get(b_.getInt8Ty(), 64), "dataname");
  llvm::BasicBlock* loopL = llvm::BasicBlock::Create(ctx_, "data.loop", curFn_);
  llvm::BasicBlock* bodyL = llvm::BasicBlock::Create(ctx_, "data.body", curFn_);
  llvm::BasicBlock* endL = llvm::BasicBlock::Create(ctx_, "data.end", curFn_);
  b_.CreateBr(loopL);
  startBlock(loopL);
  llvm::Value* namelen =
      b_.CreateCall(runtimeFn("pli_get_data_next"), {nameBuf, i64(64)}, "dataname.len");
  llvm::Value* more = b_.CreateICmpNE(namelen, i64(0), "datamore");
  b_.CreateCondBr(more, bodyL, endL);
  startBlock(bodyL);
  for (auto& item : s->items) {
    HExpr* t = item.get();
    llvm::BasicBlock* readL = llvm::BasicBlock::Create(ctx_, "data.read", curFn_);
    llvm::BasicBlock* nextL = llvm::BasicBlock::Create(ctx_, "data.next", curFn_);
    llvm::Value* want = globalString(t->sym->name);
    llvm::Value* match = b_.CreateCall(runtimeFn("pli_data_name_is"),
                                       {nameBuf, namelen, want, i64((long long)t->sym->name.size())},
                                       "datamatch");
    b_.CreateCondBr(b_.CreateICmpNE(match, i32(0), "datahit"), readL, nextL);
    startBlock(readL);
    const Type& ty = t->ty;
    Val v;
    switch (ty.k) {
    case TK::FixedBin:
    case TK::FixedDec:
      if (ty.k == TK::FixedDec && ty.scale > 0) {
        llvm::Value* raw =
            b_.CreateCall(runtimeFn("pli_get_list_decfixed"), {i64(ty.scale)});
        v.reg = b_.CreateTrunc(raw, llvmTy(ty), "gdec");
        v.ty = ty;
      } else {
        v.reg = b_.CreateCall(runtimeFn("pli_get_list_fixed"), {});
        v.ty = Type::fixedBin(63, 0);
      }
      break;
    case TK::Float:
      v.reg = b_.CreateCall(runtimeFn("pli_get_list_float"), {});
      v.ty = Type::flt(6);
      break;
    case TK::Bit: {
      llvm::Value* b = b_.CreateCall(runtimeFn("pli_get_list_bit"), {});
      v.reg = b_.CreateTrunc(b, b_.getInt1Ty(), "gbit");
      v.ty = Type::bit();
      break;
    }
    case TK::Char: {
      llvm::Value* buf = entryAlloca(llvm::ArrayType::get(b_.getInt8Ty(), ty.len), "gdch");
      b_.CreateCall(runtimeFn("pli_get_list_char"), {buf, i64(ty.len)});
      v.ptr = buf;
      v.len = i64(ty.len);
      v.ty = ty;
      break;
    }
    default:
      d_.error(item->loc, "GET DATA of this type is not implemented in this stage", "(110)");
      b_.CreateCall(runtimeFn("pli_get_data_skip"), {});
      b_.CreateBr(loopL);
      startBlock(nextL);
      continue;
    }
    storeGetTarget(t, v, s->loc);
    b_.CreateBr(loopL);
    startBlock(nextL);
  }
  // No item matched: discard the pair's value and read on.
  b_.CreateCall(runtimeFn("pli_get_data_skip"), {});
  b_.CreateBr(loopL);
  startBlock(endL);
}

// GET (rules 104-109): list-directed input reads each data-list reference from
// SYSIN and stores the value, like an assignment target.
void IRGen::emitGet(HStmt* s) {
  // STRING (rule 105) source: route the list-directed input from the character
  // variable instead of SYSIN.
  llvm::Value *sdata = nullptr, *slen = nullptr;
  if (s->stringTarget) {
    HExpr* st = s->stringTarget.get();
    sdata =
        st->memberPath.empty() ? addressOf(st->sym) : memberAddr(st->sym, st->memberPath, s->loc);
    slen = i64(st->ty.len);
    b_.CreateCall(runtimeFn("pli_string_get_open"), {sdata, slen});
  }
  // FILE ( f ) (rule 105): route the list-directed input through the named
  // file's stream instead of SYSIN.
  if (s->fileSym)
    b_.CreateCall(runtimeFn("pli_get_select"), {i64(s->fileSym->fileSlot)});
  if (s->edit) {
    emitGetEditItems(s);
  } else if (s->data) {
    emitGetDataItems(s);
  } else {
    for (auto& item : s->items) {
      HExpr* t = item.get();
      const Type& ty = t->ty;
      Val v;
      switch (ty.k) {
      case TK::FixedBin:
      case TK::FixedDec:
        if (ty.k == TK::FixedDec && ty.scale > 0) {
          // Already scaled by the reader; truncate to the target width
          // without rescaling, then the store passes it through.
          llvm::Value* raw =
              b_.CreateCall(runtimeFn("pli_get_list_decfixed"), {i64(ty.scale)});
          v.reg = b_.CreateTrunc(raw, llvmTy(ty), "gdec");
          v.ty = ty;
        } else {
          v.reg = b_.CreateCall(runtimeFn("pli_get_list_fixed"), {});
          v.ty = Type::fixedBin(63, 0);
        }
        break;
      case TK::Float:
        v.reg = b_.CreateCall(runtimeFn("pli_get_list_float"), {});
        v.ty = Type::flt(6);
        break;
      case TK::Bit: {
        llvm::Value* b = b_.CreateCall(runtimeFn("pli_get_list_bit"), {});
        v.reg = b_.CreateTrunc(b, b_.getInt1Ty(), "gbit");
        v.ty = Type::bit();
        break;
      }
      case TK::Char: {
        // Read into a reusable entry buffer, then assign into the target.
        llvm::Value* buf = entryAlloca(llvm::ArrayType::get(b_.getInt8Ty(), ty.len), "gch");
        b_.CreateCall(runtimeFn("pli_get_list_char"), {buf, i64(ty.len)});
        v.ptr = buf;
        v.len = i64(ty.len);
        v.ty = ty;
        break;
      }
      case TK::Complex: {
        // Read both parts through element pointers, then load the pair.
        llvm::Value* pair = entryAlloca(llvmTy(ty), "gcx");
        b_.CreateCall(runtimeFn("pli_get_list_complex"),
                      {b_.CreateStructGEP(llvmTy(ty), pair, 0, "gcxr"),
                       b_.CreateStructGEP(llvmTy(ty), pair, 1, "gcxi")});
        v.cpx = b_.CreateLoad(llvmTy(ty), pair, "gcx");
        v.ty = ty;
        break;
      }
      default:
        d_.error(item->loc, "GET LIST of this type is not implemented in this stage", "(110)");
        continue;
      }
      storeGetTarget(t, v, s->loc);
    }
  }
  if (s->stringTarget)
    b_.CreateCall(runtimeFn("pli_string_get_close"), {});
  if (s->fileSym)
    b_.CreateCall(runtimeFn("pli_get_unselect"), {});
}

// Store an input value into a data-list reference (rules (109),(110)): the same
// target-addressing as an assignment's left-hand side.
void IRGen::storeGetTarget(HExpr* t, const Val& v, SourceLoc loc) {
  const Type& ty = t->ty;
  if (t->kind == HExpr::Subscript && t->sym) {
    if (!t->memberPath.empty()) {
      const Type& arr = memberType(t->sym, t->memberPath);
      llvm::Value *ub = nullptr, *lb = nullptr;
      llvm::Value* base = arr.isDynamic() ? dynamicMemberBase(t->sym, t->memberPath, loc, ub, lb)
                                          : memberAddr(t->sym, t->memberPath, loc);
      llvm::Value* addr = arrayElementAddr(arr, base, t->args, loc, ub, lb);
      storeScalarTo(addr, ty, convert(v, ty, loc));
      return;
    }
    if (t->sym->definedBase && t->sym->definedIsubAxis >= 0) {
      llvm::Value* addr = definedSubElementAddr(t->sym, t->args, loc);
      storeScalarTo(addr, ty, convert(v, ty, loc));
      return;
    }
    storeArrayElement(t->sym, t->args, v, loc);
    return;
  }
  if (t->kind == HExpr::VarRef && t->sym) {
    if (!t->memberPath.empty()) {
      llvm::Value* addr =
          t->locPtr ? locatorMemberAddr(t->sym, t->memberPath, emitExpr(t->locPtr.get()).reg)
                    : memberAddr(t->sym, t->memberPath, loc);
      storeScalarTo(addr, ty, convert(v, ty, loc));
      return;
    }
    storeTo(t->sym, v, loc);
    return;
  }
  d_.error(loc, "GET LIST target is not assignable in this stage", "(110)");
}

// Edit-directed output (rule (108)): walk the format list, pairing each A/F
// data format with the next data item and emitting the control (X/SKIP/PAGE/
// LINE) items in order.
void IRGen::emitPutEditItems(HStmt* s) {
  llvm::Value* defW = i64(0);
  llvm::Value* defD = i64(0);
  size_t di = 0;
  for (auto& f : s->formats) {
    switch (f.kind) {
    case HFormatItem::X: {
      llvm::Value* w = f.w ? toI64(emitExpr(f.w.get())) : defW;
      b_.CreateCall(runtimeFn("pli_put_edit_x"), {w});
      break;
    }
    case HFormatItem::Skip: {
      llvm::Value* n = f.w ? toI64(emitExpr(f.w.get())) : i64(1);
      b_.CreateCall(runtimeFn("pli_put_edit_skip"), {n});
      break;
    }
    case HFormatItem::Page:
      b_.CreateCall(runtimeFn("pli_put_edit_page"), {});
      break;
    case HFormatItem::Line: {
      llvm::Value* n = f.w ? toI64(emitExpr(f.w.get())) : i64(1);
      b_.CreateCall(runtimeFn("pli_put_edit_line"), {n});
      break;
    }
    case HFormatItem::A: {
      if (di >= s->items.size())
        break;
      HExpr* item = s->items[di++].get();
      Val v = emitExpr(item);
      llvm::Value* w = f.w ? toI64(emitExpr(f.w.get())) : defW;
      b_.CreateCall(runtimeFn("pli_put_edit_char"), {v.ptr, v.len, w});
      break;
    }
    case HFormatItem::F: {
      if (di >= s->items.size())
        break;
      HExpr* item = s->items[di++].get();
      Val v = emitExpr(item);
      llvm::Value* w = f.w ? toI64(emitExpr(f.w.get())) : defW;
      llvm::Value* d = f.d ? toI64(emitExpr(f.d.get())) : defD;
      if (v.ty.k == TK::Float) {
        b_.CreateCall(runtimeFn("pli_put_edit_float"), {v.reg, w, d});
      } else {
        llvm::Value* scale = v.ty.k == TK::FixedDec ? i64(v.ty.scale) : i64(0);
        b_.CreateCall(runtimeFn("pli_put_edit_fixed"), {toI64(v), scale, w, d});
      }
      break;
    }
    case HFormatItem::E: {
      if (di >= s->items.size())
        break;
      HExpr* item = s->items[di++].get();
      Val v = convert(emitExpr(item), Type::flt(6), item->loc);
      llvm::Value* w = f.w ? toI64(emitExpr(f.w.get())) : defW;
      llvm::Value* d = f.d ? toI64(emitExpr(f.d.get())) : defD;
      b_.CreateCall(runtimeFn("pli_put_edit_float_e"), {v.reg, w, d});
      break;
    }
    }
  }
}

// Edit-directed input (rule (108)): pair each A/F data format with the next
// data item and emit the control (X/SKIP) items in order.
void IRGen::emitGetEditItems(HStmt* s) {
  llvm::Value* defW = i64(0);
  size_t di = 0;
  for (auto& f : s->formats) {
    switch (f.kind) {
    case HFormatItem::X: {
      llvm::Value* w = f.w ? toI64(emitExpr(f.w.get())) : defW;
      b_.CreateCall(runtimeFn("pli_get_edit_x"), {w});
      break;
    }
    case HFormatItem::Skip: {
      llvm::Value* n = f.w ? toI64(emitExpr(f.w.get())) : i64(1);
      b_.CreateCall(runtimeFn("pli_get_edit_skip"), {n});
      break;
    }
    case HFormatItem::Page:
    case HFormatItem::Line:
      // Line control is not meaningful on input in this stage; ignored.
      break;
    case HFormatItem::A: {
      if (di >= s->items.size())
        break;
      HExpr* t = s->items[di++].get();
      const Type& ty = t->ty;
      llvm::Value* w = f.w ? toI64(emitExpr(f.w.get())) : defW;
      llvm::Value* buf = entryAlloca(llvm::ArrayType::get(b_.getInt8Ty(), ty.len), "gech");
      b_.CreateCall(runtimeFn("pli_get_edit_char"), {buf, i64(ty.len), w});
      Val v;
      v.ptr = buf;
      v.len = i64(ty.len);
      v.ty = ty;
      storeGetTarget(t, v, s->loc);
      break;
    }
    case HFormatItem::F:
    case HFormatItem::E: {
      if (di >= s->items.size())
        break;
      HExpr* t = s->items[di++].get();
      llvm::Value* w = f.w ? toI64(emitExpr(f.w.get())) : defW;
      Val v;
      v.reg = b_.CreateCall(runtimeFn("pli_get_edit_num"), {w});
      v.ty = Type::flt(6);
      storeGetTarget(t, v, s->loc);
      break;
    }
    }
  }
}

// Address of one call argument for a by-reference parameter (rule 4).
llvm::Value* IRGen::argAddr(HExpr* a, const Type& pty) {
  // A whole-structure argument is passed BY VALUE (rule 127): the source's
  // storage is copied into a fresh buffer, so the callee's writes do not reach
  // the caller's structure. Scalars and arrays remain by reference.
  if (pty.isStruct()) {
    llvm::Value* dst = entryAlloca(llvmTy(pty), "sv");
    Val av = emitExpr(a);
    llvm::Value* sz = i64(mod_.getDataLayout().getTypeStoreSize(llvmTy(pty)));
    b_.CreateMemCpy(dst, llvm::MaybeAlign(), av.ptr, llvm::MaybeAlign(), sz);
    return dst;
  }
  bool direct = a->kind == HExpr::VarRef && a->sym && a->sym->kind != Symbol::ProcName &&
                a->sym->ty.k == pty.k && a->sym->ty.len == pty.len && a->sym->ty.prec == pty.prec &&
                a->sym->ty.varying == pty.varying;
  if (direct)
    return addressOf(a->sym);
  llvm::Value* addr = entryAlloca(llvmTy(pty), "dummy");
  Val av = emitExpr(a);
  if (pty.isChar()) {
    Val cv = av;
    if (pty.varying) {
      llvm::Value* dp = b_.CreateStructGEP(llvmTy(pty), addr, 1, "vdata");
      llvm::Value* ln =
          b_.CreateCall(runtimeFn("pli_assign_varying"), {dp, i64(pty.len), cv.ptr, cv.len});
      llvm::Value* lp = b_.CreateStructGEP(llvmTy(pty), addr, 0, "vlenp");
      b_.CreateStore(b_.CreateTrunc(ln, b_.getInt32Ty(), "l32"), lp);
    } else {
      b_.CreateCall(runtimeFn("pli_assign_char"), {addr, i64(pty.len), cv.ptr, cv.len});
    }
  } else {
    Val cv = convert(av, pty, a->loc);
    storeScalarTo(addr, pty, cv);
  }
  return addr;
}

// Element count of a call argument passed to a `*`-extent parameter (rule 13):
// a fixed array contributes its constant extent, a dynamic-bound array its live
// recorded bound. Returns null for an unsupported argument form (the caller
// diagnoses it), so a `*` array cannot be forwarded to another `*` parameter in
// this stage.
llvm::Value* IRGen::argExtent(HExpr* a) {
  if (a->kind != HExpr::VarRef || !a->sym || !a->sym->ty.isArray())
    return nullptr;
  const Type& arr = a->sym->ty;
  const Dim& d = arr.dims[0];
  if (d.adj)
    return nullptr; // forwarding a `*` array: not served here
  if (d.dyn || d.lbDyn) {
    llvm::Value* ub = d.dyn ? (dynUb_.count(a->sym)
                                   ? dynUb_[a->sym]
                                   : (a->sym->dynUb ? toI64(emitExpr(a->sym->dynUb)) : i64(d.ub)))
                            : i64(d.ub);
    llvm::Value* lb = d.lbDyn ? (dynLb_.count(a->sym) ? dynLb_[a->sym] : i64(d.lb)) : i64(d.lb);
    llvm::Value* ext = b_.CreateAdd(b_.CreateSub(ub, lb, "e1"), i64(1), "ext");
    long long rest = 1;
    for (size_t k = 1; k < arr.dims.size(); ++k)
      rest *= (arr.dims[k].ub - arr.dims[k].lb + 1);
    if (rest != 1)
      ext = b_.CreateMul(ext, i64(rest), "extall");
    return ext;
  }
  return i64(d.ub - d.lb + 1);
}

// Append the callee's static-link arguments (its enclosing automatic
// variables, rule 8). Shared by emitCall and emitExpr.
void IRGen::appendStaticLinks(Proc* callee, std::vector<llvm::Value*>& args) {
  if (!callee)
    return;
  // Rule (91): a handler function has no establishing frame, so a call that
  // needs static links cannot be marshalled from an ON-unit.
  if (inHandler_ && !callee->env.empty()) {
    d_.error(callee->loc,
             "calling a procedure that captures enclosing state from an ON-unit is not "
             "implemented in this stage",
             "(91)");
    return;
  }
  for (Symbol* v : callee->env) {
    // "cousin" call: a sibling internal procedure reaching a non-adjacent
    // enclosing variable is not yet supported.
    if (v->owner != curProc_->src &&
        std::find(curProc_->env.begin(), curProc_->env.end(), v) == curProc_->env.end()) {
      d_.error(callee->loc,
               "a sibling internal procedure reaching a non-adjacent enclosing variable is not "
               "implemented in this stage",
               "(8)");
      continue;
    }
    args.push_back(addressOf(v));
  }
}

void IRGen::emitCall(HStmt* s) {
  if (!s->sym)
    return;
  Symbol* calleeSym = s->sym;
  Proc* callee = calleeSym->proc;
  Stmt* en = calleeSym->entry;
  llvm::Function* calleeF = calleeFn(calleeSym);
  std::vector<Symbol*> calleeParams =
      en ? en->entryParamSyms : (callee ? callee->paramSyms : std::vector<Symbol*>());

  std::vector<llvm::Value*> args;
  // A structure-returning callee (rule 127) takes a hidden result buffer as its
  // first argument; the CALL statement discards the returned value.
  Type rty = en ? (en->entryIsFunction ? en->entryRetTy : Type::voidTy())
                : (callee ? callee->retTy : Type::voidTy());
  if (rty.isStruct())
    args.push_back(entryAlloca(llvmTy(rty), "sret"));
  for (size_t i = 0; i < s->args.size(); ++i) {
    HExpr* a = s->args[i].get();
    Type pty;
    if (en || callee) {
      if (i < calleeParams.size())
        pty = calleeParams[i]->ty;
      else
        break;
    } else {
      if (i < calleeSym->entryParams.size())
        pty = calleeSym->entryParams[i];
      else
        break;
    }
    args.push_back(argAddr(a, pty));
  }
  // Hidden extent args for `*`-extent parameters (rule 13): the caller passes
  // the actual element count of each matching array argument.
  for (size_t i = 0; i < calleeParams.size() && i < s->args.size(); ++i) {
    if (isAdjustable(calleeParams[i])) {
      llvm::Value* ext = argExtent(s->args[i].get());
      if (!ext) {
        d_.error(s->args[i]->loc,
                 "a '*' extent parameter takes a fixed or dynamic-bound array in this stage",
                 "(13)");
        ext = i64(0);
      }
      args.push_back(ext);
    }
  }
  appendStaticLinks(callee, args);
  b_.CreateCall(calleeF, args);
}

// ---------------------------------------------------------------------------
// loads / stores
// ---------------------------------------------------------------------------
// Number of elements across all axes: the product of (ub - lb + 1) (rule (12)).
long long IRGen::arrayExtent(const Type& arr) {
  long long n = 1;
  for (const auto& d : arr.dims)
    n *= (d.ub - d.lb + 1);
  return n;
}

// Address of array element A(i,j,...) (rule 126): a runtime SUBSCRIPTRANGE
// check on each axis, then a GEP into the flat row-major [N x elemTy] storage.
// Each index is 1-based (or lb-based); the generated flat offset is
//   sum_k (i_k - lb_k) * stride_k,  stride_k = product of extents of later axes.
llvm::Value* IRGen::arrayElementAddr(const Type& arr, llvm::Value* base,
                                     const std::vector<HExprP>& idxs, SourceLoc loc,
                                     llvm::Value* dynUb, llvm::Value* dynLb) {
  const Type& el = arr.elementType();
  const size_t nAxes = arr.dims.size();

  // Dynamic array (rule (13)): the first axis has a runtime upper and/or lower
  // bound (later axes are fixed in this stage) and the storage is a bare element
  // buffer, so the flat offset is row-major over the runtime first-axis stride
  // and the GEP is on the element pointer.
  if (arr.isDynamic()) {
    llvm::Value* flat = i64(0);
    llvm::Value* oob = b_.getInt1(false);
    long long stride = 1;
    for (size_t k = nAxes; k-- > 0;) {
      const Dim& dk = arr.dims[k];
      llvm::Value* i = toI64(emitExpr(idxs[k].get()));
      llvm::Value* lb = k == 0 && dk.lbDyn ? dynLb : i64(dk.lb);
      llvm::Value* ub = k == 0 && dk.dyn ? dynUb : i64(dk.ub);
      oob = b_.CreateOr(
          oob, b_.CreateOr(b_.CreateICmpSLT(i, lb, "lo"), b_.CreateICmpSGT(i, ub, "hi")), "oob");
      // A handled slip resumes with this axis clamped into range (rule 94).
      llvm::Value* off = b_.CreateSub(clampIndex(i, lb, ub), lb, "off");
      flat = b_.CreateAdd(flat, b_.CreateMul(off, i64(stride), "scaled"), "flat");
      stride *= (dk.ub - dk.lb + 1); // later axes are fixed; only axis 0 is runtime
    }
    std::string id = std::to_string(n_++);
    llvm::BasicBlock* failL = llvm::BasicBlock::Create(ctx_, "sub.fail." + id, curFn_);
    llvm::BasicBlock* okL = llvm::BasicBlock::Create(ctx_, "sub.ok." + id, curFn_);
    b_.CreateCondBr(oob, failL, okL);
    startBlock(failL);
    emitCondTrap(Stmt::kSubscriptrangeCondKey, "pli_subscript_oob", "sub", okL);
    startBlock(okL);
    return b_.CreateInBoundsGEP(llvmTy(el), base, {flat}, "aelem");
  }

  // Emit every index and OR the per-axis out-of-bounds flags into one check,
  // accumulating the row-major flat offset (last axis is contiguous).
  llvm::Value* flat = i64(0);
  llvm::Value* oob = b_.getInt1(false);
  long long stride = 1;
  for (size_t k = nAxes; k-- > 0;) {
    const long long lb = arr.dims[k].lb, ub = arr.dims[k].ub;
    llvm::Value* i = toI64(emitExpr(idxs[k].get()));
    oob = b_.CreateOr(
        oob, b_.CreateOr(b_.CreateICmpSLT(i, i64(lb), "lo"), b_.CreateICmpSGT(i, i64(ub), "hi")),
        "oob");
    // A handled slip resumes with this axis clamped into range (rule 94).
    llvm::Value* off = b_.CreateSub(clampIndex(i, i64(lb), i64(ub)), i64(lb), "off");
    flat = b_.CreateAdd(flat, b_.CreateMul(off, i64(stride), "scaled"), "flat");
    stride *= (ub - lb + 1);
  }

  std::string id = std::to_string(n_++);
  llvm::BasicBlock* failL = llvm::BasicBlock::Create(ctx_, "sub.fail." + id, curFn_);
  llvm::BasicBlock* okL = llvm::BasicBlock::Create(ctx_, "sub.ok." + id, curFn_);
  b_.CreateCondBr(oob, failL, okL);

  startBlock(failL);
  emitCondTrap(Stmt::kSubscriptrangeCondKey, "pli_subscript_oob", "sub", okL);

  startBlock(okL);
  llvm::Type* arrTy = llvm::ArrayType::get(llvmTy(el), (unsigned)arrayExtent(arr));
  return b_.CreateInBoundsGEP(arrTy, base, {i64(0), flat}, "aelem");
}

// Address of a qualified member S.A.B (rule 124): descend the recorded field
// indices one struct at a time with CreateStructGEP (the same API the
// varying-string addressing uses), so each GEP selects one field of the
// current struct type.
llvm::Value* IRGen::memberAddr(Symbol* base, const std::vector<unsigned>& path, SourceLoc) {
  llvm::Value* addr = addressOf(base);
  const Type* cur = &base->ty;
  for (unsigned f : path) {
    addr = b_.CreateStructGEP(llvmTy(*cur), addr, f, "mem");
    cur = &cur->members[f]->ty;
  }
  return addr;
}

// Address of a member of one element of an array of structures arr(i).x
// (rule 124): GEP through the recorded field indices from a caller-supplied
// element-struct address, against the element structure type (mirrors
// memberAddr, which starts from the symbol's own base instead).
llvm::Value* IRGen::elementMemberAddr(Symbol* base, const std::vector<unsigned>& path,
                                      llvm::Value* elemAddr) {
  llvm::Value* addr = elemAddr;
  Type elem = base->ty.elementType(); // stable copy of the element structure type
  const Type* cur = &elem;
  for (unsigned f : path) {
    addr = b_.CreateStructGEP(llvmTy(*cur), addr, f, "mem");
    cur = &cur->members[f]->ty;
  }
  return addr;
}

// Address of a member of a locator-qualified reference P->X.FIELD (rule 124):
// GEP through the recorded field indices from a caller-supplied base address
// (the loaded pointer value), against the based structure type (mirrors
// memberAddr, which starts from the symbol's own base instead).
llvm::Value* IRGen::locatorMemberAddr(Symbol* base, const std::vector<unsigned>& path,
                                      llvm::Value* baseAddr) {
  llvm::Value* addr = baseAddr;
  const Type* cur = &base->ty;
  for (unsigned f : path) {
    addr = b_.CreateStructGEP(llvmTy(*cur), addr, f, "lmem");
    cur = &cur->members[f]->ty;
  }
  return addr;
}

// The resolved type of a qualified member S.A.B (rule 124): walk the recorded
// field indices (as memberAddr does, without emitting GEPs) to recover the leaf
// member's type — for a subscripted member array S.A(i), this is the array type.
const Type& IRGen::memberType(Symbol* base, const std::vector<unsigned>& path) {
  const Type* cur = &base->ty;
  for (unsigned f : path)
    cur = &cur->members[f]->ty;
  return *cur;
}

// Buffer pointer and live bounds of a dynamic-array structure member (rule 13):
// the struct field holds a pointer to a bare runtime-sized element buffer
// (allocated at entry), so load it; the bounds were recorded in memberDyn_ at
// entry too. Returns the element-buffer pointer and sets ub/lb for
// arrayElementAddr's runtime bounds check.
llvm::Value* IRGen::dynamicMemberBase(Symbol* base, const std::vector<unsigned>& path,
                                      SourceLoc loc, llvm::Value*& ub, llvm::Value*& lb) {
  auto it = memberDyn_.find(MemberDyn{base, path});
  ub = it != memberDyn_.end() ? it->second.ub : nullptr;
  lb = it != memberDyn_.end() ? it->second.lb : nullptr;
  return b_.CreateLoad(b_.getPtrTy(), memberAddr(base, path, loc), "mdynp");
}

// BY NAME assignment (rule 86): copy each member of `dst` (at dstBase) from the
// same-named member of `src` (at srcBase). Minor structures recurse by name; an
// array member is copied whole (sema required identical array types); a scalar
// member is loaded, converted to the target type and stored.
void IRGen::emitByNameCopy(llvm::Value* dstBase, llvm::Value* srcBase, const Type& dst,
                           const Type& src, SourceLoc loc, Symbol* dstSym,
                           const std::vector<unsigned>& dstPrefix, Symbol* srcSym,
                           const std::vector<unsigned>& srcPrefix) {
  for (size_t i = 0; i < dst.members.size(); ++i) {
    const Member& dm = *dst.members[i];
    size_t j = (size_t)-1;
    for (size_t k = 0; k < src.members.size(); ++k)
      if (src.members[k]->name == dm.name) {
        j = k;
        break;
      }
    if (j == (size_t)-1)
      continue; // name absent from the source: skipped
    const Member& sm = *src.members[j];
    llvm::Value* d = b_.CreateStructGEP(llvmTy(dst), dstBase, (unsigned)i, "bnm.d");
    llvm::Value* s = b_.CreateStructGEP(llvmTy(src), srcBase, (unsigned)j, "bnm.s");
    if (dm.ty.isStruct() && sm.ty.isStruct()) {
      std::vector<unsigned> dp = dstPrefix;
      dp.push_back((unsigned)i);
      std::vector<unsigned> sp = srcPrefix;
      sp.push_back((unsigned)j);
      emitByNameCopy(d, s, dm.ty, sm.ty, loc, dstSym, dp, srcSym, sp);
    } else if (dm.ty.isArray()) {
      if (dm.ty.isDynamic()) {
        // A same-named dynamic member (rules (13),(86), ADR-093): the field
        // holds a buffer pointer, so copy the buffer contents, not the
        // pointer (sema required identical array types). Layouts may differ,
        // so each side resolves its own path; a live-extent mismatch traps
        // rather than overflowing.
        if (!dstSym || !srcSym) {
          d_.error(loc, "BY NAME copy of a dynamic member needs plain variables here", "(13)");
          return;
        }
        std::vector<unsigned> dp = dstPrefix;
        dp.push_back((unsigned)i);
        std::vector<unsigned> sp = srcPrefix;
        sp.push_back((unsigned)j);
        auto dstIt = memberDyn_.find(MemberDyn{dstSym, dp});
        auto srcIt = memberDyn_.find(MemberDyn{srcSym, sp});
        if (dstIt == memberDyn_.end() || srcIt == memberDyn_.end()) {
          d_.error(loc, "BY NAME copy of a dynamic member is not addressable here", "(13)");
          return;
        }
        const Dim& d0 = dm.ty.dims[0];
        llvm::Value* dl = d0.lbDyn && dstIt->second.lb ? dstIt->second.lb : i64(d0.lb);
        llvm::Value* sl = d0.lbDyn && srcIt->second.lb ? srcIt->second.lb : i64(d0.lb);
        llvm::Value* dext =
            b_.CreateAdd(b_.CreateSub(dstIt->second.ub, dl, "bnm.e1"), i64(1), "bnm.dext");
        llvm::Value* sext =
            b_.CreateAdd(b_.CreateSub(srcIt->second.ub, sl, "bnm.e2"), i64(1), "bnm.sext");
        long long rest = 1;
        for (size_t k = 1; k < dm.ty.dims.size(); ++k)
          rest *= (dm.ty.dims[k].ub - dm.ty.dims[k].lb + 1);
        if (rest != 1) {
          dext = b_.CreateMul(dext, i64(rest), "bnm.dall");
          sext = b_.CreateMul(sext, i64(rest), "bnm.sall");
        }
        std::string id = std::to_string(n_++);
        llvm::BasicBlock* failL = llvm::BasicBlock::Create(ctx_, "bnm.fail." + id, curFn_);
        llvm::BasicBlock* okL = llvm::BasicBlock::Create(ctx_, "bnm.ok." + id, curFn_);
        b_.CreateCondBr(b_.CreateICmpNE(dext, sext, "bnm.mis"), failL, okL);
        startBlock(failL);
        // A shape mismatch has no index to clamp: a handled trap notifies
        // the handler, then aborts (rule 94).
        llvm::BasicBlock* abL = llvm::BasicBlock::Create(ctx_, "bnm.abort." + id, curFn_);
        emitCondTrap(Stmt::kSubscriptrangeCondKey, "pli_subscript_oob", "sub", abL);
        b_.SetInsertPoint(abL);
        b_.CreateUnreachable();
        startBlock(okL);
        const Type& el = dm.ty.elementType();
        llvm::Value* dbuf = b_.CreateLoad(b_.getPtrTy(), d, "bnm.dp");
        llvm::Value* sbuf = b_.CreateLoad(b_.getPtrTy(), s, "bnm.sp");
        llvm::Value* nbytes =
            b_.CreateMul(sext, i64(mod_.getDataLayout().getTypeStoreSize(llvmTy(el))), "bnm.n");
        b_.CreateMemCpy(dbuf, llvm::MaybeAlign(), sbuf, llvm::MaybeAlign(), nbytes);
      } else {
        llvm::Value* sz = i64(mod_.getDataLayout().getTypeStoreSize(llvmTy(dm.ty)));
        b_.CreateMemCpy(d, llvm::MaybeAlign(), s, llvm::MaybeAlign(), sz);
      }
    } else {
      Val sv;
      sv.ty = sm.ty;
      sv.reg = b_.CreateLoad(llvmTy(sm.ty), s, "bnm.ld");
      if (sm.ty.isBit())
        sv.reg = b_.CreateTrunc(sv.reg, b_.getInt1Ty(), "bnm.b1");
      storeScalarTo(d, dm.ty, convert(sv, dm.ty, loc));
    }
  }
}

// Store a structure's INITIAL element list (rule (26)) into its storage, walking
// members in declaration order. A nested structure recurses; an array member is
// filled element by element (an array of structures recurses per element); a
// scalar leaf stores the next value, converted to its type.
void IRGen::emitStructInitValues(llvm::Value* base, const Type& ty, const std::vector<Expr*>& vals,
                                 size_t& idx, SourceLoc loc) {
  for (size_t i = 0; i < ty.members.size(); ++i) {
    const Member& m = *ty.members[i];
    llvm::Value* mem = b_.CreateStructGEP(llvmTy(ty), base, (unsigned)i, "init.mem");
    if (m.ty.isStruct()) {
      emitStructInitValues(mem, m.ty, vals, idx, loc);
    } else if (m.ty.isArray()) {
      const Type& el = m.ty.elementType();
      if (m.ty.isDynamic()) {
        // A trailing dynamic member (rules (13),(26), ADR-092): the field
        // holds a runtime-sized buffer pointer (pass 3, pre-sized for the
        // itemlist), so store each remaining value straight-line like a
        // dynamic array. Sema restricted this to scalar elements.
        llvm::Value* buf = b_.CreateLoad(b_.getPtrTy(), mem, "init.mdyn");
        long long k = 0;
        while (idx < vals.size()) {
          llvm::Value* ep = b_.CreateGEP(llvmTy(el), buf, {i64(k++)}, "init.el");
          storeScalarTo(ep, el, initValue(el, vals[idx++]));
        }
      } else {
        llvm::Type* arrTy = llvm::ArrayType::get(llvmTy(el), (unsigned)arrayExtent(m.ty));
        for (long long k = 0; k < arrayExtent(m.ty); ++k) {
          llvm::Value* ep = b_.CreateInBoundsGEP(arrTy, mem, {i64(0), i64(k)}, "init.el");
          if (el.isStruct())
            emitStructInitValues(ep, el, vals, idx, loc);
          else
            storeScalarTo(ep, el, initValue(el, vals[idx++]));
        }
      }
    } else {
      if (m.ty.isChar())
        d_.error(loc, "CHARACTER structure members are not implemented in this stage", "(11)");
      else
        storeScalarTo(mem, m.ty, initValue(m.ty, vals[idx++]));
    }
  }
}

Val IRGen::loadSym(Symbol* sym, const Type& ty) {
  Val v;
  v.ty = ty;
  llvm::Value* addr = addressOf(sym);
  if (ty.isChar()) {
    if (ty.varying) {
      llvm::Value* dp = b_.CreateStructGEP(llvmTy(ty), addr, 1, "vdata");
      llvm::Value* lp = b_.CreateStructGEP(llvmTy(ty), addr, 0, "vlenp");
      llvm::Value* l32 = b_.CreateLoad(b_.getInt32Ty(), lp, "l32");
      v.ptr = dp;
      v.len = b_.CreateSExt(l32, b_.getInt64Ty(), "l64");
    } else {
      v.ptr = addr;
      v.len = i64(ty.len);
    }
    return v;
  }
  llvm::Value* r = b_.CreateLoad(llvmTy(ty), addr, "ld");
  if (ty.isComplex()) {
    // A complex value (QR2.2/CM5) is the {double,double} struct itself.
    v.cpx = r;
  } else if (ty.isBit()) {
    v.reg = b_.CreateTrunc(r, b_.getInt1Ty(), "b1");
  } else {
    v.reg = r;
  }
  return v;
}

void IRGen::storeScalarTo(llvm::Value* addr, const Type& ty, const Val& v) {
  if (ty.isComplex()) {
    // A complex value (QR2.2/CM5) is stored as the {double,double} struct.
    b_.CreateStore(v.cpx, addr);
    return;
  }
  llvm::Value* val = v.reg;
  if (ty.isBit()) {
    val = b_.CreateZExt(val, b_.getInt8Ty(), "z8");
  }
  b_.CreateStore(val, addr);
}

void IRGen::storeTo(Symbol* sym, const Val& v, SourceLoc loc) {
  const Type& dt = sym->ty;
  llvm::Value* addr = addressOf(sym);
  if (dt.isChar()) {
    if (!v.ty.isChar()) {
      d_.error(loc,
               "conversion from " + v.ty.desc() + " to " + dt.desc() +
                   " is not implemented in this stage",
               "(86)");
      return;
    }
    if (dt.varying) {
      llvm::Value* dp = b_.CreateStructGEP(llvmTy(dt), addr, 1, "vdata");
      llvm::Value* ln =
          b_.CreateCall(runtimeFn("pli_assign_varying"), {dp, i64(dt.len), v.ptr, v.len});
      llvm::Value* lp = b_.CreateStructGEP(llvmTy(dt), addr, 0, "vlenp");
      b_.CreateStore(b_.CreateTrunc(ln, b_.getInt32Ty(), "l32"), lp);
    } else {
      b_.CreateCall(runtimeFn("pli_assign_char"), {addr, i64(dt.len), v.ptr, v.len});
    }
    return;
  }
  Val cv = convert(v, dt, loc);
  storeScalarTo(addr, dt, cv);
}

// Load one array element (rule 126): a bounds-checked address, then a load of
// the element's scalar value. Character element arrays are diagnosed, not
// silently miscompiled (invariant 2).
Val IRGen::loadArrayElement(Symbol* sym, const std::vector<HExprP>& idxs, SourceLoc loc) {
  Val v;
  const Type& el = sym->ty.elementType();
  v.ty = el;
  if (el.isChar()) {
    d_.error(loc, "arrays of CHARACTER are not implemented in this stage", "(12)");
    v.reg = i64(0);
    return v;
  }
  llvm::Value* addr = arrayElementAddr(sym->ty, addressOf(sym), idxs, loc,
                                       dynUb_.count(sym) ? dynUb_[sym] : nullptr,
                                       dynLb_.count(sym) ? dynLb_[sym] : nullptr);
  llvm::Value* r = b_.CreateLoad(llvmTy(el), addr, "ald");
  if (el.isBit())
    v.reg = b_.CreateTrunc(r, b_.getInt1Ty(), "b1");
  else
    v.reg = r;
  return v;
}

// Store one array element (rule 126): convert to the element type, then store
// through the bounds-checked element address.
void IRGen::storeArrayElement(Symbol* sym, const std::vector<HExprP>& idxs, const Val& src,
                              SourceLoc loc) {
  const Type& el = sym->ty.elementType();
  if (el.isChar()) {
    d_.error(loc, "arrays of CHARACTER are not implemented in this stage", "(12)");
    return;
  }
  llvm::Value* addr = arrayElementAddr(sym->ty, addressOf(sym), idxs, loc,
                                       dynUb_.count(sym) ? dynUb_[sym] : nullptr,
                                       dynLb_.count(sym) ? dynLb_[sym] : nullptr);
  Val cv = convert(src, el, loc);
  storeScalarTo(addr, el, cv);
}

// Copy an N-star cross-section (rule 126) into a whole array target: `t` is
// the target array (plain variable or member), `x` is the source cross-section
// A(<fixed|*>, ...) of matching reduced shape (rank = number of '*' axes). The
// fixed axes' indices are evaluated and bounds-checked once; the '*' axes are
// gathered into the target by iterating its linear row-major index, decomposing
// each position into star-axis coordinates and mapping them to the source flat
// offset. A single '*' is the row/column case of this same affine gather.
void IRGen::emitCrossSectionAssign(HExpr* t, HExpr* x, SourceLoc loc) {
  const Type& srcArr = x->memberPath.empty() ? x->sym->ty : memberType(x->sym, x->memberPath);
  llvm::Value* srcBase =
      x->memberPath.empty() ? addressOf(x->sym) : memberAddr(x->sym, x->memberPath, loc);
  const Type& el = srcArr.elementType();
  const Type& tgtArr = t->memberPath.empty() ? t->sym->ty : memberType(t->sym, t->memberPath);
  llvm::Value* tgtBase =
      t->memberPath.empty() ? addressOf(t->sym) : memberAddr(t->sym, t->memberPath, loc);

  const size_t n = srcArr.dims.size();
  // Positions of the '*' axes in source axis order; the target is rank m with
  // the same extents as these axes in that order.
  std::vector<size_t> stars;
  for (size_t k = 0; k < n; ++k)
    if (x->args[k]->kind == HExpr::Star)
      stars.push_back(k);
  const size_t m = stars.size();

  // Row-major stride of each source axis: the product of the later-axis extents.
  std::vector<long long> stride(n);
  for (long long s = 1, k = (long long)n; k-- > 0;) {
    stride[k] = s;
    s *= (srcArr.dims[k].ub - srcArr.dims[k].lb + 1);
  }

  // Evaluate and bounds-check each fixed-axis index once; accumulate its
  // contribution to the source flat offset.
  llvm::Value* fixedFlat = i64(0);
  for (size_t k = 0; k < n; ++k) {
    if (x->args[k]->kind == HExpr::Star)
      continue;
    llvm::Value* iv = toI64(emitExpr(x->args[k].get()));
    llvm::Value* lb = i64(srcArr.dims[k].lb);
    llvm::Value* ub = i64(srcArr.dims[k].ub);
    llvm::Value* oob =
        b_.CreateOr(b_.CreateICmpSLT(iv, lb, "lo"), b_.CreateICmpSGT(iv, ub, "hi"), "oob");
    // A handled slip resumes with the index clamped into range (rule 94).
    llvm::Value* civ = clampIndex(iv, lb, ub);
    std::string fid = std::to_string(n_++);
    llvm::BasicBlock* failL = llvm::BasicBlock::Create(ctx_, "cs.fail." + fid, curFn_);
    llvm::BasicBlock* okL = llvm::BasicBlock::Create(ctx_, "cs.ok." + fid, curFn_);
    b_.CreateCondBr(oob, failL, okL);
    startBlock(failL);
    emitCondTrap(Stmt::kSubscriptrangeCondKey, "pli_subscript_oob", "sub", okL);
    startBlock(okL);
    fixedFlat = b_.CreateAdd(
        fixedFlat, b_.CreateMul(b_.CreateSub(civ, lb, "off"), i64(stride[k]), "scaled"), "ff");
  }

  // Target is rank m. Its row-major strides and total extent let a single linear
  // index be decomposed into star-axis coordinates; coordinate j is the index
  // along source axis stars[j], so its source offset is coord * stride[stars[j]].
  std::vector<long long> tstride(m);
  for (long long s = 1, j = (long long)m; j-- > 0;) {
    tstride[j] = s;
    s *= (srcArr.dims[stars[j]].ub - srcArr.dims[stars[j]].lb + 1);
  }
  const long long total = arrayExtent(tgtArr);
  llvm::Type* srcArrTy = llvm::ArrayType::get(llvmTy(el), (unsigned)arrayExtent(srcArr));
  llvm::Type* tgtArrTy = llvm::ArrayType::get(llvmTy(el), (unsigned)arrayExtent(tgtArr));
  llvm::AllocaInst* ctr = entryAlloca(b_.getInt64Ty(), "cs.i." + std::to_string(n_++));
  b_.CreateStore(i64(0), ctr);
  std::string id = std::to_string(n_++);
  llvm::BasicBlock* condL = llvm::BasicBlock::Create(ctx_, "cs.cond." + id, curFn_);
  llvm::BasicBlock* bodyL = llvm::BasicBlock::Create(ctx_, "cs.body." + id, curFn_);
  llvm::BasicBlock* stepL = llvm::BasicBlock::Create(ctx_, "cs.step." + id, curFn_);
  llvm::BasicBlock* endL = llvm::BasicBlock::Create(ctx_, "cs.end." + id, curFn_);
  branch(condL);
  startBlock(condL);
  llvm::Value* ti = b_.CreateLoad(b_.getInt64Ty(), ctr, "cst");
  b_.CreateCondBr(b_.CreateICmpSLT(ti, i64(total), "cscmp"), bodyL, endL);
  startBlock(bodyL);

  // Decompose the linear target index into star-axis coordinates and map each to
  // its source flat-offset contribution. All offsets are non-negative (fixed
  // axes are bounds-checked, coordinates run 0..extent-1), so the arithmetic is
  // unsigned.
  llvm::Value* srcFlat = fixedFlat;
  llvm::Value* rem = ti;
  for (size_t j = 0; j < m; ++j) {
    llvm::Value* coord = b_.CreateUDiv(rem, i64(tstride[j]), "csc");
    rem = b_.CreateURem(rem, i64(tstride[j]), "csr");
    srcFlat = b_.CreateAdd(srcFlat, b_.CreateMul(coord, i64(stride[stars[j]]), "csm"), "css");
  }

  llvm::Value* saddr = b_.CreateInBoundsGEP(srcArrTy, srcBase, {i64(0), srcFlat}, "csrc");
  Val sv;
  sv.ty = el;
  llvm::Value* lr = b_.CreateLoad(llvmTy(el), saddr, "csl");
  sv.reg = el.isBit() ? b_.CreateTrunc(lr, b_.getInt1Ty(), "csb") : lr;
  llvm::Value* taddr = b_.CreateInBoundsGEP(tgtArrTy, tgtBase, {i64(0), ti}, "cdst");
  storeScalarTo(taddr, el, convert(sv, el, loc));
  branch(stepL);
  startBlock(stepL);
  b_.CreateStore(b_.CreateAdd(ti, i64(1), "csinc"), ctr);
  branch(condL);
  startBlock(endL);
}

// Address of a DEFINED scalar element overlay (rule 24): Y overlays the base
// array element at the (compile-time checked) constant subscripts, so its
// address is a stable GEP into the base. Y has no storage of its own.
llvm::Value* IRGen::definedConstAddr(Symbol* sym) {
  Symbol* base = sym->definedBase;
  const Type& bty = base->ty;
  llvm::Value* baseAddr = addressOf(base);
  long long flat = 0, stride = 1;
  const size_t n = bty.dims.size();
  for (size_t k = n; k-- > 0;) {
    flat += (sym->definedConst[k] - bty.dims[k].lb) * stride;
    stride *= (bty.dims[k].ub - bty.dims[k].lb + 1);
  }
  llvm::Type* arrTy = llvm::ArrayType::get(llvmTy(bty.elementType()), (unsigned)arrayExtent(bty));
  return b_.CreateInBoundsGEP(arrTy, baseAddr, {i64(0), i64(flat)}, "defined");
}

// Address of a subscripted iSUB-DEFINED array element Y(k) (rules 134,126): Y
// is a 1-D live overlay of one axis of the base array X, so Y(k) is the base
// element X(fixed..., k, fixed...). The iSUB slot index is bounds-checked; the
// fixed subscripts were compile-time checked.
llvm::Value* IRGen::definedSubElementAddr(Symbol* y, const std::vector<HExprP>& idxs,
                                          SourceLoc loc) {
  Symbol* base = y->definedBase;
  const Type& bty = base->ty;
  const size_t n = bty.dims.size();
  llvm::Value* yidx = toI64(emitExpr(idxs[0].get()));
  const Dim& dd = bty.dims[y->definedIsubAxis];
  const int ilb = dd.lb, iub = dd.ub;
  // Affine iSUB base index m*1SUB + c (rule 134 index arithmetic): Y(i) overlays
  // X(m*i + c), so the base index is m*yidx + c and is bounds-checked against X.
  llvm::Value* bidx =
      b_.CreateAdd(b_.CreateMul(yidx, i64(y->definedIsubMult), "m"), i64(y->definedIsubAdd), "c");
  llvm::Value* oob = b_.CreateOr(b_.CreateICmpSLT(bidx, i64(ilb), "lo"),
                                 b_.CreateICmpSGT(bidx, i64(iub), "hi"), "oob");
  // A handled slip resumes with the base index clamped into range (rule 94).
  llvm::Value* cbidx = clampIndex(bidx, i64(ilb), i64(iub));
  std::string id = std::to_string(n_++);
  llvm::BasicBlock* failL = llvm::BasicBlock::Create(ctx_, "def.fail." + id, curFn_);
  llvm::BasicBlock* okL = llvm::BasicBlock::Create(ctx_, "def.ok." + id, curFn_);
  b_.CreateCondBr(oob, failL, okL);
  startBlock(failL);
  emitCondTrap(Stmt::kSubscriptrangeCondKey, "pli_subscript_oob", "sub", okL);
  startBlock(okL);

  llvm::Value* flat = i64(0);
  long long stride = 1;
  for (size_t k = n; k-- > 0;) {
    llvm::Value* iv = (int)k == y->definedIsubAxis ? cbidx : i64(y->definedConst[k]);
    flat = b_.CreateAdd(
        flat, b_.CreateMul(b_.CreateSub(iv, i64(bty.dims[k].lb), "o"), i64(stride), "s"), "f");
    stride *= (bty.dims[k].ub - bty.dims[k].lb + 1);
  }
  llvm::Type* arrTy = llvm::ArrayType::get(llvmTy(bty.elementType()), (unsigned)arrayExtent(bty));
  return b_.CreateInBoundsGEP(arrTy, addressOf(base), {i64(0), flat}, "delem");
}

// ---------------------------------------------------------------------------
// conversions (the M0 subset of the PL/I conversion rules)
// ---------------------------------------------------------------------------
Val IRGen::convert(const Val& v, const Type& dst, SourceLoc loc) {
  Val out;
  out.ty = dst;
  if (v.ty.isChar() || dst.isChar()) {
    if (v.ty.isChar() && dst.isChar())
      return v;
    d_.error(loc,
             "conversion between " + v.ty.desc() + " and " + dst.desc() +
                 " is not implemented in this stage",
             "(86)");
    out.reg = i64(0);
    return out;
  }

  // A pointer value is passed through unchanged between POINTER targets
  // (rule 15): pointer assignment copies the address, no numeric conversion.
  if (v.ty.isPointer() && dst.isPointer())
    return v;

  // Complex conversions (QR2.2/CM5): a complex value is an {double,double}
  // pair. complex -> complex passes through; complex -> real takes the real
  // part and converts it as a real; real -> complex uses the value as the real
  // part with a zero imaginary part.
  if (v.ty.isComplex() && dst.isComplex())
    return v;
  if (v.ty.isComplex() && !dst.isComplex()) {
    Val re;
    re.ty = Type::flt(6);
    re.reg = b_.CreateExtractValue(v.cpx, 0, "cpx.re");
    return convert(re, dst, loc);
  }
  if (!v.ty.isComplex() && dst.isComplex()) {
    Val re = convert(v, Type::flt(6), loc);
    llvm::Value* s =
        llvm::UndefValue::get(llvm::StructType::get(ctx_, {b_.getDoubleTy(), b_.getDoubleTy()}));
    s = b_.CreateInsertValue(s, re.reg, 0, "cpx.re");
    s = b_.CreateInsertValue(s, flt(0.0), 1, "cpx.im");
    out.cpx = s;
    return out;
  }

  const bool srcFloat = v.ty.k == TK::Float;
  const bool dstFloat = dst.k == TK::Float;
  const bool srcBit = v.ty.isBit();
  const bool dstBit = dst.isBit();

  if (dstBit) {
    out.reg = toI1(v, loc);
    return out;
  }
  if (srcBit) { // BIT -> arithmetic
    if (dstFloat) {
      llvm::Value* z = b_.CreateZExt(v.reg, b_.getInt32Ty(), "z");
      out.reg = b_.CreateSIToFP(z, b_.getDoubleTy(), "cvt");
    } else {
      out.reg = b_.CreateZExt(v.reg, llvmTy(dst), "cvt");
    }
    return out;
  }
  if (srcFloat && dstFloat)
    return v;
  if (srcFloat && !dstFloat) { // FLOAT -> FIXED truncates toward zero
    // A FIXED DECIMAL target holds the value scaled by 10^q (ADR-006), so
    // scale the float up first.
    llvm::Value* f = v.reg;
    if (dst.k == TK::FixedDec && dst.scale > 0)
      f = b_.CreateFMul(f, flt((double)pliPow10(dst.scale)), "fsc");
    // FPToSI outside the destination range is UB (QR1.2): check the float
    // first (a decimal target wider than i64 range still needs the i64
    // check, since the conversion itself goes through i64).
    if (dst.k == TK::FixedDec) {
      if (dst.prec <= 18)
        floatRangeTrap(f, -(double)pliPow10(dst.prec), false, (double)pliPow10(dst.prec));
      else
        floatRangeTrap(f, -9223372036854775808.0, true, 9223372036854775808.0);
    } else if (llvmTy(dst)->getIntegerBitWidth() == 64) {
      floatRangeTrap(f, -9223372036854775808.0, true, 9223372036854775808.0);
    } else {
      floatRangeTrap(f, -2147483648.0, true, 2147483648.0);
    }
    out.reg = b_.CreateFPToSI(f, llvmTy(dst), "cvt");
    return out;
  }
  if (!srcFloat && dstFloat) {
    // A FIXED DECIMAL source holds the value scaled by 10^q; divide back to
    // the true value before converting to float.
    llvm::Value* f = b_.CreateSIToFP(v.reg, b_.getDoubleTy(), "cvt");
    if (v.ty.k == TK::FixedDec && v.ty.scale > 0)
      f = b_.CreateFDiv(f, flt((double)pliPow10(v.ty.scale)), "fds");
    out.reg = f;
    return out;
  }
  // FIXED -> FIXED. A 10^q rescale applies only when a FIXED DECIMAL value
  // (whose stored integer is scaled by 10^q) is involved (ADR-006); FIXED
  // BINARY scale is 2-based and stays untouched by this feature. Rescale to
  // a DECIMAL target by 10^(dst.scale - src.scale); a scaled DECIMAL source
  // converting to a BINARY target reduces to its integer part.
  if (v.ty.isFixed() && dst.isFixed()) {
    // FIXED narrowing traps (QR1.2): a value the target cannot hold is a SIZE
    // overflow (hard ERROR until SIZE can route it). A DECIMAL target wider
    // than i64 range needs no check; a same-or-wider decimal source whose
    // rescaled digits fit is proven safe statically. BINARY targets keep the
    // arithmetic convention (width-checked, precision is not enforced).
    int dqDec = (dst.k == TK::FixedDec && v.ty.k == TK::FixedDec) ? dst.scale - v.ty.scale : 0;
    bool decFits = v.ty.k == TK::FixedDec && v.ty.prec + std::max(dqDec, 0) <= dst.prec;
    bool needDec = dst.k == TK::FixedDec && dst.prec <= 18 && !decFits;
    bool needBin = dst.k == TK::FixedBin && dst.intBits() == 32 && v.ty.intBits() == 64;
    long long limit = dst.k == TK::FixedDec ? pliPow10(dst.prec) : (1LL << 31);
    bool rescale = dst.k == TK::FixedDec && v.ty.scale != dst.scale;
    if (!rescale && (v.ty.k == TK::FixedDec && v.ty.scale > 0 && dst.k != TK::FixedDec))
      rescale = true; // DECIMAL source -> BINARY target: drop the fraction
    if (!rescale) {
      if (needDec || needBin) {
        llvm::Value* w = v.reg->getType()->getIntegerBitWidth() == 64
                              ? v.reg
                              : b_.CreateSExt(v.reg, b_.getInt64Ty(), "cvtw");
        magTrap(w, limit);
      }
      if (v.ty.intBits() == dst.intBits()) {
        out.reg = v.reg;
        return out;
      }
      if (v.ty.intBits() < dst.intBits())
        out.reg = b_.CreateSExt(v.reg, llvmTy(dst), "cvt");
      else
        out.reg = b_.CreateTrunc(v.reg, llvmTy(dst), "cvt");
      return out;
    }
    llvm::Value* r = b_.CreateSExt(v.reg, b_.getInt64Ty(), "res");
    int dq = dst.k == TK::FixedDec ? dst.scale - v.ty.scale : -v.ty.scale;
    if (dq > 0) {
      // A scale-up that wraps already exceeds any target: trap, so the
      // magnitude check below never reads a wrapped value.
      r = checkedArith(Tok::Star, r, i64(pliPow10(dq)));
    } else {
      int k = -dq;
      llvm::Value* div = i64(pliPow10(k));
      // round half away from zero: r + 5*10^(k-1)*sign
      llvm::Value* sign = b_.CreateSelect(b_.CreateICmpSLT(r, i64(0), "sgn"), i64(-1), i64(1));
      llvm::Value* adj = b_.CreateMul(i64(pliPow10(k - 1) * 5), sign, "adj");
      r = b_.CreateSDiv(b_.CreateAdd(r, adj, "rn"), div, "res");
    }
    if (needDec || needBin)
      magTrap(r, limit);
    out.reg = (unsigned)dst.intBits() == 64 ? r : b_.CreateTrunc(r, llvmTy(dst), "cvt");
    return out;
  }
  return out;
}

llvm::Value* IRGen::toI1(const Val& v, SourceLoc loc) {
  if (v.ty.isBit())
    return v.reg;
  if (v.ty.k == TK::Float)
    return b_.CreateFCmpUNE(v.reg, flt(0.0), "tst");
  if (v.ty.isNumeric())
    return b_.CreateICmpNE(v.reg, llvm::Constant::getNullValue(llvmTy(v.ty)), "tst");
  d_.error(loc, "value of type " + v.ty.desc() + " cannot be used as a condition", "(75)");
  return b_.getInt1(false);
}

llvm::Value* IRGen::toI64(const Val& v) {
  if (v.ty.k == TK::Float)
    return b_.CreateFPToSI(v.reg, b_.getInt64Ty(), "i64");
  if (v.ty.isBit())
    return b_.CreateZExt(v.reg, b_.getInt64Ty(), "i64");
  if (v.ty.intBits() == 64)
    return v.reg;
  return b_.CreateSExt(v.reg, b_.getInt64Ty(), "i64");
}

Val IRGen::charTemp(int len) {
  Val v;
  v.ty = Type::chr(len);
  v.ptr = entryAlloca(llvm::ArrayType::get(b_.getInt8Ty(), len), "cbuf");
  v.len = i64(len);
  return v;
}

// ---------------------------------------------------------------------------
// expressions
// ---------------------------------------------------------------------------
Val IRGen::emitExpr(HExpr* e) {
  Val v;
  if (!e)
    return v;
  switch (e->kind) {
  case HExpr::Convert:
    return convert(emitExpr(e->a.get()), e->convTo, e->loc);
  case HExpr::IntLit:
    v.ty = e->ty;
    v.reg = llvm::ConstantInt::get(llvmTy(e->ty), e->ival, true);
    return v;
  case HExpr::DecLit:
    v.ty = e->ty;
    v.reg = llvm::ConstantInt::get(llvmTy(e->ty), e->ival, true);
    return v;
  case HExpr::FltLit:
    v.ty = e->ty;
    v.reg = flt(e->fval);
    return v;
  case HExpr::ComplexLit: {
    // Imaginary constant (rule 139): 0 + fval*i as a constant pair.
    v.ty = e->ty;
    llvm::Value* s =
        llvm::UndefValue::get(llvm::StructType::get(ctx_, {b_.getDoubleTy(), b_.getDoubleTy()}));
    s = b_.CreateInsertValue(s, flt(0.0), 0, "cpx.re");
    s = b_.CreateInsertValue(s, flt(e->fval), 1, "cpx.im");
    v.cpx = s;
    return v;
  }
  case HExpr::CharLit: {
    v.ty = e->ty;
    v.ptr = globalString(e->sval);
    v.len = i64(e->sval.size());
    return v;
  }
  case HExpr::BitLit:
    v.ty = Type::bit(1);
    v.reg = b_.getInt1(!e->sval.empty() && e->sval[0] == '1');
    return v;
  case HExpr::Star:
    // A '*' is a cross-section axis marker (rule 126); outside a subscript it
    // is not a value. Sema only reaches here through a misused cross-section.
    d_.error(e->loc, "a cross-section '*' is only valid within a subscript", "(126)");
    v.ty = Type::voidTy();
    v.reg = i64(0);
    return v;
  case HExpr::Subscript:
    if (!e->sym) {
      v.ty = e->ty;
      v.reg = i64(0);
      return v;
    }
    // A cross-section A(*, ...) is a reduced-dim array value (rule 126), served
    // only as an assignment RHS (see emitAssign); anywhere else it is rejected.
    for (const auto& a : e->args)
      if (a->kind == HExpr::Star) {
        d_.error(
            e->loc,
            "a cross-section is only valid as the right-hand side of an assignment in this stage",
            "(126)");
        v.ty = e->ty;
        v.reg = i64(0);
        return v;
      }
    if (!e->memberPath.empty()) {
      // An array of structures arr(i).x (rules 124,126): the subscripts index
      // the array to one structure element, then the member path GEPs into it.
      if (e->sym->ty.isArray()) {
        llvm::Value* elem = arrayElementAddr(e->sym->ty, addressOf(e->sym), e->args, e->loc);
        llvm::Value* addr = elementMemberAddr(e->sym, e->memberPath, elem);
        const Type& el = e->ty;
        llvm::Value* r = b_.CreateLoad(llvmTy(el), addr, "aosld");
        v.ty = el;
        v.reg = el.isBit() ? b_.CreateTrunc(r, b_.getInt1Ty(), "b1") : r;
        return v;
      }
      // A subscripted member array S.A(i) (rules 124,126): the member array
      // lives at memberAddr(...) (a [N x elemTy] field, or a buffer pointer for
      // a dynamic member), so GEP into it as a normal array and load the leaf.
      const Type& arr = memberType(e->sym, e->memberPath);
      const Type& el = e->ty;
      if (el.isChar()) {
        d_.error(e->loc, "arrays of CHARACTER members are not implemented in this stage", "(12)");
        v.ty = el;
        v.reg = i64(0);
        return v;
      }
      llvm::Value *ub = nullptr, *lb = nullptr;
      llvm::Value* base = arr.isDynamic() ? dynamicMemberBase(e->sym, e->memberPath, e->loc, ub, lb)
                                          : memberAddr(e->sym, e->memberPath, e->loc);
      llvm::Value* addr = arrayElementAddr(arr, base, e->args, e->loc, ub, lb);
      llvm::Value* r = b_.CreateLoad(llvmTy(el), addr, "mald");
      v.ty = el;
      v.reg = el.isBit() ? b_.CreateTrunc(r, b_.getInt1Ty(), "b1") : r;
      return v;
    }
    // An iSUB-DEFINED array Y (rule 134) has no storage of its own: Y(k) is a
    // live overlay of a base element, so route the address to the base X.
    if (e->sym->definedBase && e->sym->definedIsubAxis >= 0) {
      llvm::Value* addr = definedSubElementAddr(e->sym, e->args, e->loc);
      const Type& el = e->ty;
      llvm::Value* r = b_.CreateLoad(llvmTy(el), addr, "defl");
      v.ty = el;
      v.reg = el.isBit() ? b_.CreateTrunc(r, b_.getInt1Ty(), "b1") : r;
      return v;
    }
    return loadArrayElement(e->sym, e->args, e->loc);
  case HExpr::VarRef:
    if (!e->sym) {
      v.ty = e->ty;
      v.reg = i64(0);
      return v;
    }
    if (!e->memberPath.empty()) {
      // Qualified member S.A.B (rule 124): load the leaf member.
      const Type& leaf = e->ty;
      if (leaf.isChar()) {
        d_.error(e->loc, "CHARACTER structure members are not implemented in this stage", "(11)");
        v.ty = leaf;
        v.reg = i64(0);
        return v;
      }
      // A locator-qualified member P->X.FIELD (rule 124) GEPs off the loaded
      // pointer value; otherwise off the based/symbol member address.
      llvm::Value* addr =
          e->locPtr ? locatorMemberAddr(e->sym, e->memberPath, emitExpr(e->locPtr.get()).reg)
                    : memberAddr(e->sym, e->memberPath, e->loc);
      v.ty = leaf;
      if (leaf.isStruct()) {
        // A whole minor structure member (rule 127): its value is its address.
        v.ptr = addr;
        return v;
      }
      llvm::Value* r = b_.CreateLoad(llvmTy(leaf), addr, "mld");
      v.reg = leaf.isBit() ? b_.CreateTrunc(r, b_.getInt1Ty(), "b1") : r;
      return v;
    }
    if (e->ty.isStruct()) {
      // A whole structure as a value (rule 127): carry its address.
      v.ty = e->ty;
      v.ptr = e->locPtr ? emitExpr(e->locPtr.get()).reg : addressOf(e->sym);
      return v;
    }
    return loadSym(e->sym, e->sym->ty);
  case HExpr::Call: {
    // Built-ins are emitted in emitBuiltin; a non-builtin call (a user
    // function procedure) falls through to the general path below.
    if (emitBuiltin(e, v))
      return v;
    if (!e->sym) {
      v.ty = e->ty;
      v.reg = i64(0);
      return v;
    }
    if (!e->sym->proc) {
      // rule (34): an external ENTRY(...) RETURNS(...) function, declared with
      // no PL/I body; call the C symbol directly. Args are passed by reference
      // (the descriptor carries each parameter's type).
      if (!e->sym->isEntry) {
        v.ty = e->ty;
        v.reg = i64(0);
        return v;
      }
      Type rty = e->sym->entryIsFunction ? e->sym->entryRetTy : Type::voidTy();
      llvm::Function* extFn = calleeFn(e->sym);
      std::vector<llvm::Value*> args;
      for (size_t i = 0; i < e->args.size() && i < e->sym->entryParams.size(); ++i) {
        HExpr* a = e->args[i].get();
        Type pty = e->sym->entryParams[i];
        args.push_back(argAddr(a, pty));
        if (pty.isArray() && !pty.dims.empty() && pty.dims[0].adj) {
          llvm::Value* ext = argExtent(a);
          args.push_back(ext ? ext : i64(0)); // hidden `*` extent arg (rule (13))
        }
      }
      if (!e->sym->entryIsFunction) {
        b_.CreateCall(extFn, args);
        v.ty = e->ty;
        v.reg = i64(0);
        return v;
      }
      llvm::CallInst* call = b_.CreateCall(extFn, args, "fres");
      v.ty = rty;
      v.reg = rty.isBit() ? b_.CreateTrunc(call, b_.getInt1Ty(), "fb") : call;
      return v;
    }
    Stmt* en = e->sym->entry;
    Proc* callee = e->sym->proc;
    Type rty = en ? (en->entryIsFunction ? en->entryRetTy : Type::voidTy()) : callee->retTy;
    llvm::Function* calleeFn =
        en ? mod_.getFunction(
                 entryIrName(callee->name, callee->parent ? callee->parent->name : "", en->name)
                     .substr(1))
           : mod_.getFunction(callee->irName.substr(1));
    std::vector<Symbol*> calleeParams = en ? en->entryParamSyms : callee->paramSyms;
    if (rty.isChar()) {
      v.ty = e->ty;
      v.reg = i64(0);
      return v;
    } // diagnosed in emitProc
    std::vector<llvm::Value*> args;
    llvm::Value* sretPtr = nullptr;
    if (rty.isStruct()) {
      // Structure-valued function (rule 127): the caller allocates the result
      // buffer and passes its address as the hidden first argument.
      sretPtr = entryAlloca(llvmTy(rty), "sret");
      args.push_back(sretPtr);
    }
    for (size_t i = 0; i < e->args.size(); ++i) {
      HExpr* a = e->args[i].get();
      Type pty;
      if (i < calleeParams.size())
        pty = calleeParams[i]->ty;
      else
        break;
      args.push_back(argAddr(a, pty));
    }
    for (size_t i = 0; i < calleeParams.size() && i < e->args.size(); ++i)
      if (isAdjustable(calleeParams[i])) {
        llvm::Value* ext = argExtent(e->args[i].get());
        if (!ext) {
          d_.error(e->args[i]->loc,
                   "a '*' extent parameter takes a fixed or dynamic-bound array in this stage",
                   "(13)");
          ext = i64(0);
        }
        args.push_back(ext);
      }
    appendStaticLinks(callee, args);
    // A structure-returning callee returns void (the result is written to the
    // hidden buffer), so the call cannot carry a value name.
    llvm::CallInst* call =
        rty.isStruct() ? b_.CreateCall(calleeFn, args) : b_.CreateCall(calleeFn, args, "fres");
    v.ty = rty;
    if (rty.isStruct()) {
      v.ptr = sretPtr; // the result lives in the caller's buffer
    } else if (rty.isBit()) {
      v.reg = b_.CreateTrunc(call, b_.getInt1Ty(), "fb");
    } else {
      v.reg = call;
    }
    return v;
  }
  case HExpr::Unary: {
    Val a = emitExpr(e->a.get());
    if (e->op == Tok::Not) {
      llvm::Value* bb = toI1(a, e->loc);
      v.ty = Type::bit(1);
      v.reg = b_.CreateXor(bb, b_.getInt1(true), "not");
      return v;
    }
    v.ty = a.ty;
    if (a.ty.k == TK::Float)
      v.reg = b_.CreateFNeg(a.reg, "neg");
    else if (a.ty.k == TK::FixedBin) {
      // Negating INT_MIN overflows (QR1.2): trap through the SIZE path.
      llvm::Type* ty = a.reg->getType();
      if (!sizeChecks()) {
        // (NOSIZE): the wrapped value stands (rules (60)-(63), ADR-110).
        v.reg = b_.CreateSub(llvm::Constant::getNullValue(ty), a.reg, "neg");
        return v;
      }
      unsigned bits = ty->getIntegerBitWidth();
      llvm::Value* lo =
          llvm::ConstantInt::get(ty, 1ULL << (bits - 1), true);
      llvm::Value* of = b_.CreateICmpEQ(a.reg, lo, "negof");
      int seq = ovSeq_++;
      llvm::BasicBlock* trapBB =
          llvm::BasicBlock::Create(ctx_, "ov.trap." + std::to_string(seq), curFn_);
      llvm::BasicBlock* okBB =
          llvm::BasicBlock::Create(ctx_, "ov.ok." + std::to_string(seq), curFn_);
      b_.CreateCondBr(of, trapBB, okBB);
      b_.SetInsertPoint(trapBB);
      emitSizeTrap(okBB);
      b_.SetInsertPoint(okBB);
      v.reg = b_.CreateSub(llvm::Constant::getNullValue(ty), a.reg, "neg");
    } else
      v.reg = b_.CreateSub(llvm::Constant::getNullValue(llvmTy(a.ty)), a.reg, "neg");
    return v;
  }
  case HExpr::Binary:
    break;
  }

  // ---- binary operators ----
  const Tok op = e->op;

  if (op == Tok::Concat) { // rule (119)
    Val a = emitExpr(e->a.get());
    Val b = emitExpr(e->b.get());
    if (!a.ty.isChar() || !b.ty.isChar()) {
      v.ty = e->ty;
      v.reg = i64(0);
      return v;
    }
    Val out = charTemp(e->ty.len);
    b_.CreateCall(runtimeFn("pli_concat"), {out.ptr, a.ptr, a.len, b.ptr, b.len});
    out.len = b_.CreateAdd(a.len, b.len, "clen");
    return out;
  }

  if (op == Tok::Amp || op == Tok::Bar) { // rules (116),(115)
    Val a = emitExpr(e->a.get());
    llvm::Value* ab = toI1(a, e->loc);
    Val b = emitExpr(e->b.get());
    llvm::Value* bb = toI1(b, e->loc);
    llvm::Value* r = op == Tok::Amp ? b_.CreateAnd(ab, bb, "and") : b_.CreateOr(ab, bb, "or");
    v.ty = Type::bit(1);
    v.reg = r;
    return v;
  }

  const bool isCmp = op == Tok::Eq || op == Tok::Ne || op == Tok::Lt || op == Tok::Le ||
                     op == Tok::Gt || op == Tok::Ge || op == Tok::Ngt || op == Tok::Nlt;

  Val a = emitExpr(e->a.get());
  Val b = emitExpr(e->b.get());

  if (isCmp && a.ty.isChar() && b.ty.isChar()) { // rule (117)
    llvm::Value* c = b_.CreateCall(runtimeFn("pli_cmp_char"), {a.ptr, a.len, b.ptr, b.len}, "scmp");
    llvm::CmpInst::Predicate pred = llvm::CmpInst::ICMP_EQ;
    switch (op) {
    case Tok::Eq:
      pred = llvm::CmpInst::ICMP_EQ;
      break;
    case Tok::Ne:
      pred = llvm::CmpInst::ICMP_NE;
      break;
    case Tok::Lt:
      pred = llvm::CmpInst::ICMP_SLT;
      break;
    case Tok::Le:
      pred = llvm::CmpInst::ICMP_SLE;
      break;
    case Tok::Gt:
      pred = llvm::CmpInst::ICMP_SGT;
      break;
    case Tok::Ge:
      pred = llvm::CmpInst::ICMP_SGE;
      break;
    case Tok::Ngt:
      pred = llvm::CmpInst::ICMP_SLE;
      break;
    case Tok::Nlt:
      pred = llvm::CmpInst::ICMP_SGE;
      break;
    default:
      break;
    }
    v.ty = Type::bit(1);
    v.reg = b_.CreateICmp(pred, c, i32(0), "cmp");
    return v;
  }

  if (isCmp && a.ty.isPointer() && b.ty.isPointer()) {
    // POINTER equality/inequality (rule (117)): compare the two addresses
    // directly; ordered comparisons were rejected in sema.
    llvm::CmpInst::Predicate pred = op == Tok::Eq ? llvm::CmpInst::ICMP_EQ : llvm::CmpInst::ICMP_NE;
    v.ty = Type::bit(1);
    v.reg = b_.CreateICmp(pred, a.reg, b.reg, "pcmp");
    return v;
  }

  if (isCmp && (a.ty.isComplex() || b.ty.isComplex())) {
    // Exact part-wise equality (CM5); ordered comparisons were diagnosed.
    Val ac = convert(a, Type::complexTy(), e->loc);
    Val bc = convert(b, Type::complexTy(), e->loc);
    llvm::Value* er = b_.CreateFCmp(llvm::CmpInst::FCMP_OEQ,
                                    b_.CreateExtractValue(ac.cpx, 0, "cpx.er"),
                                    b_.CreateExtractValue(bc.cpx, 0, "cpx.er"), "cpx.er");
    llvm::Value* ei = b_.CreateFCmp(llvm::CmpInst::FCMP_OEQ,
                                    b_.CreateExtractValue(ac.cpx, 1, "cpx.ei"),
                                    b_.CreateExtractValue(bc.cpx, 1, "cpx.ei"), "cpx.ei");
    llvm::Value* r = b_.CreateAnd(er, ei, "cpx.eq");
    if (op == Tok::Ne)
      r = b_.CreateNot(r, "cpx.ne");
    v.ty = Type::bit(1);
    v.reg = r;
    return v;
  }

  Type common = isCmp ? arithResultType(a.ty.isBit() ? Type::fixedBin(31, 0) : a.ty,
                                        b.ty.isBit() ? Type::fixedBin(31, 0) : b.ty)
                      : e->ty;
  if (!isCmp && (op == Tok::Slash || op == Tok::Power))
    common = Type::flt(e->ty.prec);
  Val av, bv;
  if (op == Tok::Star && common.isFixed()) {
    // A product's scale is the sum of the operand scales (ADR-006); multiply
    // the raw scaled integers without first rescaling either operand.
    Type wa = common;
    wa.scale = a.ty.isFixed() ? a.ty.scale : 0;
    Type wb = common;
    wb.scale = b.ty.isFixed() ? b.ty.scale : 0;
    av = convert(a, wa, e->loc);
    bv = convert(b, wb, e->loc);
  } else {
    av = convert(a, common, e->loc);
    bv = convert(b, common, e->loc);
  }
  const bool flt = common.k == TK::Float;

  if (isCmp) {
    llvm::Value* r;
    if (flt) {
      llvm::FCmpInst::Predicate pred = llvm::CmpInst::FCMP_OEQ;
      switch (op) {
      case Tok::Eq:
        pred = llvm::CmpInst::FCMP_OEQ;
        break;
      case Tok::Ne:
        pred = llvm::CmpInst::FCMP_UNE;
        break;
      case Tok::Lt:
        pred = llvm::CmpInst::FCMP_OLT;
        break;
      case Tok::Le:
        pred = llvm::CmpInst::FCMP_OLE;
        break;
      case Tok::Gt:
        pred = llvm::CmpInst::FCMP_OGT;
        break;
      case Tok::Ge:
        pred = llvm::CmpInst::FCMP_OGE;
        break;
      case Tok::Ngt:
        pred = llvm::CmpInst::FCMP_OLE;
        break;
      case Tok::Nlt:
        pred = llvm::CmpInst::FCMP_OGE;
        break;
      default:
        break;
      }
      r = b_.CreateFCmp(pred, av.reg, bv.reg, "cmp");
    } else {
      llvm::CmpInst::Predicate pred = llvm::CmpInst::ICMP_EQ;
      switch (op) {
      case Tok::Eq:
        pred = llvm::CmpInst::ICMP_EQ;
        break;
      case Tok::Ne:
        pred = llvm::CmpInst::ICMP_NE;
        break;
      case Tok::Lt:
        pred = llvm::CmpInst::ICMP_SLT;
        break;
      case Tok::Le:
        pred = llvm::CmpInst::ICMP_SLE;
        break;
      case Tok::Gt:
        pred = llvm::CmpInst::ICMP_SGT;
        break;
      case Tok::Ge:
        pred = llvm::CmpInst::ICMP_SGE;
        break;
      case Tok::Ngt:
        pred = llvm::CmpInst::ICMP_SLE;
        break;
      case Tok::Nlt:
        pred = llvm::CmpInst::ICMP_SGE;
        break;
      default:
        break;
      }
      r = b_.CreateICmp(pred, av.reg, bv.reg, "cmp");
    }
    v.ty = Type::bit(1);
    v.reg = r;
    return v;
  }

  llvm::Value* r;
  if (!isCmp && common.isComplex()) {
    // Complex arithmetic (CM5) over {double,double} pairs; mixed reals
    // arrive with a zero imaginary part via convert above.
    llvm::Value* ar = b_.CreateExtractValue(av.cpx, 0, "cpx.ar");
    llvm::Value* ai = b_.CreateExtractValue(av.cpx, 1, "cpx.ai");
    llvm::Value* br = b_.CreateExtractValue(bv.cpx, 0, "cpx.br");
    llvm::Value* bi = b_.CreateExtractValue(bv.cpx, 1, "cpx.bi");
    llvm::Value* rr = nullptr;
    llvm::Value* ri = nullptr;
    switch (op) {
    case Tok::Plus:
      rr = b_.CreateFAdd(ar, br, "cpx.rr");
      ri = b_.CreateFAdd(ai, bi, "cpx.ri");
      break;
    case Tok::Minus:
      rr = b_.CreateFSub(ar, br, "cpx.rr");
      ri = b_.CreateFSub(ai, bi, "cpx.ri");
      break;
    case Tok::Star:
      // (a+bi)(c+di) = (ac-bd) + (ad+bc)i.
      rr = b_.CreateFSub(b_.CreateFMul(ar, br), b_.CreateFMul(ai, bi), "cpx.rr");
      ri = b_.CreateFAdd(b_.CreateFMul(ar, bi), b_.CreateFMul(ai, br), "cpx.ri");
      break;
    case Tok::Slash: {
      // (a+bi)/(c+di) = ((ac+bd) + (bc-ad)i) / (c^2+d^2).
      llvm::Value* den =
          b_.CreateFAdd(b_.CreateFMul(br, br), b_.CreateFMul(bi, bi), "cpx.den");
      rr = b_.CreateFDiv(b_.CreateFAdd(b_.CreateFMul(ar, br), b_.CreateFMul(ai, bi)), den,
                         "cpx.rr");
      ri = b_.CreateFDiv(b_.CreateFSub(b_.CreateFMul(ai, br), b_.CreateFMul(ar, bi)), den,
                         "cpx.ri");
      break;
    }
    default:
      break; // complex ** is diagnosed by sema
    }
    llvm::Value* pair = llvm::UndefValue::get(llvmTy(common));
    pair = b_.CreateInsertValue(pair, rr, 0, "cpx.r");
    pair = b_.CreateInsertValue(pair, ri, 1, "cpx.i");
    v.ty = common;
    v.cpx = pair;
    return v;
  }
  switch (op) {
  case Tok::Plus:
    // FIXED overflow (QR1.2): a checked op traps on a wrapped intermediate
    // (BIT arithmetic is diagnosed in sema, so only FIXED reaches here);
    // DECIMAL precision against the declared digits is checked at narrowing
    // conversions instead (full widening stays D1/QR2).
    if (!flt && !isCmp && common.isFixed())
      r = checkedArith(op, av.reg, bv.reg);
    else
      r = flt ? b_.CreateFAdd(av.reg, bv.reg, "bin") : b_.CreateAdd(av.reg, bv.reg, "bin");
    break;
  case Tok::Minus:
    if (!flt && !isCmp && common.isFixed())
      r = checkedArith(op, av.reg, bv.reg);
    else
      r = flt ? b_.CreateFSub(av.reg, bv.reg, "bin") : b_.CreateSub(av.reg, bv.reg, "bin");
    break;
  case Tok::Star:
    if (!flt && !isCmp && common.isFixed())
      r = checkedArith(op, av.reg, bv.reg);
    else
      r = flt ? b_.CreateFMul(av.reg, bv.reg, "bin") : b_.CreateMul(av.reg, bv.reg, "bin");
    break;
  case Tok::Slash: {
    // ZERODIVIDE (rule 94): a zero divisor traps — hard abort when no handler
    // is established, else the handler runs and the division resumes with 0.
    // (Spelled out: the local `flt` flag shadows the flt() constant helper.)
    llvm::Value* fzero = llvm::ConstantFP::get(b_.getDoubleTy(), 0.0);
    llvm::Value* dz = b_.CreateFCmpOEQ(bv.reg, fzero, "zdiv");
    llvm::Value* div = b_.CreateFDiv(av.reg, bv.reg, "bin");
    r = zerodivideResume(dz, div, fzero);
    break;
  }
  case Tok::Power:
    r = b_.CreateCall(
        intrinsicFn("llvm.pow.f64", b_.getDoubleTy(), {b_.getDoubleTy(), b_.getDoubleTy()}),
        {av.reg, bv.reg}, "bin");
    break;
  default:
    r = b_.CreateAdd(i64(0), i64(0));
    break;
  }
  v.ty = common;
  v.reg = r;
  return v;
}
// Emit a built-in function call (SUBSTR, INDEX, ABS, ...). Returns true
// if `e` is one of the recognised built-ins, filling `result`; false if
// it is a user function procedure (handled in emitExpr's general path).
// Extracted from the emitExpr Call case so each built-in is a
// self-contained block.
bool IRGen::emitBuiltin(HExpr* e, Val& result) {
  // Names matching none of these are user function procedures.
  Val v;
  if (e->name == "NULL") {
    // NULL (rule 123, Appendix 1): the null POINTER value.
    v.ty = e->ty;
    v.reg = llvm::ConstantPointerNull::get(b_.getPtrTy());
    result = v;
    return true;
  }
  if (e->name == "ADDR") {
    // ADDR (rule 123, Appendix 1): the address of a variable as a POINTER.
    HExpr* a = e->args[0].get();
    if (a->kind != HExpr::VarRef || !a->sym) {
      d_.error(a->loc, "ADDR requires an unsubscripted variable in this stage", "(123)");
      v.ty = e->ty;
      v.reg = llvm::ConstantPointerNull::get(b_.getPtrTy());
      result = v;
      return true;
    }
    v.ty = e->ty;
    v.reg = addressOf(a->sym);
    result = v;
    return true;
  }
  if (e->name == "SUBSTR") {
    Val s = emitExpr(e->args[0].get());
    Val start = emitExpr(e->args[1].get());
    Val len = emitExpr(e->args[2].get());
    Val out = charTemp(e->ty.len);
    b_.CreateCall(runtimeFn("pli_substr"),
                  {out.ptr, out.len, s.ptr, s.len, toI64(start), toI64(len)});
    out.len = i64(e->ty.len);
    result = out;
    return true;
  }
  if (e->name == "INDEX") {
    Val a = emitExpr(e->args[0].get());
    Val b = emitExpr(e->args[1].get());
    llvm::Value* r = b_.CreateCall(runtimeFn("pli_index"), {a.ptr, a.len, b.ptr, b.len});
    v.ty = e->ty;
    v.reg = b_.CreateTrunc(r, b_.getInt32Ty(), "idx32");
    result = v;
    return true;
  }
  if (e->name == "ABS") {
    Val a = emitExpr(e->args[0].get());
    const Type& at = a.ty;
    llvm::Value* r;
    if (at.k == TK::Float) {
      r = b_.CreateCall(intrinsicFn("llvm.fabs.f64", b_.getDoubleTy(), {b_.getDoubleTy()}), {a.reg},
                        "abs");
    } else {
      llvm::Value* neg = b_.CreateSub(llvm::Constant::getNullValue(llvmTy(at)), a.reg, "absneg");
      llvm::Value* cmp =
          b_.CreateICmpSLT(a.reg, llvm::Constant::getNullValue(llvmTy(at)), "abscmp");
      r = b_.CreateSelect(cmp, neg, a.reg, "abs");
    }
    v.ty = e->ty;
    v.reg = r;
    result = v;
    return true;
  }
  // Scalar math built-ins (QR2.7, Appendix 1, <math.h> analogues): FLOOR,
  // CEIL, SQRT, EXP, LOG, SIN, COS, TAN, LOG2, LOG10, ATAN, SINH, COSH, TANH,
  // ATANH, ERF, ERFC, ASIN, ACOS, CBRT, and the degree trig variants SIND,
  // COSD, TAND, ATAND. The argument is converted to FLOAT and the matching
  // pli_* runtime wrapper is called.
  if (e->name == "FLOOR" || e->name == "CEIL" || e->name == "SQRT" || e->name == "EXP" ||
      e->name == "LOG" || e->name == "SIN" || e->name == "COS" || e->name == "TAN" ||
      e->name == "LOG2" || e->name == "LOG10" || e->name == "ATAN" || e->name == "SINH" ||
      e->name == "COSH" || e->name == "TANH" || e->name == "ATANH" || e->name == "ERF" ||
      e->name == "ERFC" || e->name == "SIND" || e->name == "COSD" || e->name == "TAND" ||
      e->name == "ATAND" || e->name == "ASIN" || e->name == "ACOS" || e->name == "CBRT") {
    Val x = convert(emitExpr(e->args[0].get()), Type::flt(6), e->loc);
    static const char* const kMathFn[] = {
        "pli_floor", "pli_ceil", "pli_sqrt",  "pli_exp",  "pli_log",  "pli_sin",  "pli_cos",
        "pli_tan",   "pli_log2", "pli_log10", "pli_atan", "pli_sinh", "pli_cosh", "pli_tanh",
        "pli_atanh", "pli_erf",  "pli_erfc",  "pli_sind", "pli_cosd", "pli_tand", "pli_atand",
        "pli_asin",  "pli_acos", "pli_cbrt"};
    static const char* const kMathName[] = {
        "FLOOR", "CEIL", "SQRT", "EXP",   "LOG", "SIN",  "COS",  "TAN",  "LOG2", "LOG10", "ATAN",
        "SINH",  "COSH", "TANH", "ATANH", "ERF", "ERFC", "SIND", "COSD", "TAND", "ATAND",
        "ASIN",  "ACOS", "CBRT"};
    int ix = 0;
    for (int i = 0; i < 24; ++i)
      if (e->name == kMathName[i])
        ix = i;
    v.ty = e->ty;
    v.reg = b_.CreateCall(runtimeFn(kMathFn[ix]), {x.reg}, "math");
    result = v;
    return true;
  }
  // ATAN2(y, x) (CM5): both arguments convert to FLOAT, C argument order.
  if (e->name == "ATAN2") {
    Val y = convert(emitExpr(e->args[0].get()), Type::flt(6), e->loc);
    Val x = convert(emitExpr(e->args[1].get()), Type::flt(6), e->loc);
    v.ty = e->ty;
    v.reg = b_.CreateCall(runtimeFn("pli_atan2"), {y.reg, x.reg}, "math");
    result = v;
    return true;
  }
  // Complex component/conjugate built-ins (QR2.2/CM5, Appendix 1). A complex
  // value is an {double,double} struct held in Val::cpx. COMPLEX builds one
  // from two FLOAT parts; REAL/IMAG extract a part as a FLOAT; CONJG negates
  // the imaginary part.
  if (e->name == "COMPLEX") {
    Val re = convert(emitExpr(e->args[0].get()), Type::flt(6), e->loc);
    Val im = convert(emitExpr(e->args[1].get()), Type::flt(6), e->loc);
    llvm::Value* s =
        llvm::UndefValue::get(llvm::StructType::get(ctx_, {b_.getDoubleTy(), b_.getDoubleTy()}));
    s = b_.CreateInsertValue(s, re.reg, 0, "cpx.re");
    s = b_.CreateInsertValue(s, im.reg, 1, "cpx.im");
    v.ty = e->ty;
    v.cpx = s;
    result = v;
    return true;
  }
  if (e->name == "REAL" || e->name == "IMAG") {
    Val z = emitExpr(e->args[0].get());
    v.ty = e->ty;
    v.reg = b_.CreateExtractValue(z.cpx, e->name == "REAL" ? 0 : 1, "part");
    result = v;
    return true;
  }
  if (e->name == "CONJG") {
    Val z = emitExpr(e->args[0].get());
    llvm::Value* re = b_.CreateExtractValue(z.cpx, 0, "cgr");
    llvm::Value* im = b_.CreateExtractValue(z.cpx, 1, "cgi");
    llvm::Value* nim = b_.CreateFNeg(im, "cgn");
    llvm::Value* s =
        llvm::UndefValue::get(llvm::StructType::get(ctx_, {b_.getDoubleTy(), b_.getDoubleTy()}));
    s = b_.CreateInsertValue(s, re, 0, "cg.re");
    s = b_.CreateInsertValue(s, nim, 1, "cg.im");
    v.ty = e->ty;
    v.cpx = s;
    result = v;
    return true;
  }
  if (e->name == "LENGTH") {
    Val a = emitExpr(e->args[0].get());
    v.ty = e->ty;
    v.reg = b_.CreateTrunc(a.len, b_.getInt32Ty(), "len32");
    result = v;
    return true;
  }
  if (e->name == "TRUNC") {
    Val a = emitExpr(e->args[0].get());
    if (a.ty.k == TK::Float) {
      llvm::Value* i = b_.CreateFPToSI(a.reg, b_.getInt64Ty(), "trunci");
      v.ty = e->ty;
      v.reg = b_.CreateSIToFP(i, b_.getDoubleTy(), "truncd");
    } else if (a.ty.k == TK::FixedDec && a.ty.scale > 0) {
      // Drop fractional digits toward zero: r = (r / 10^q) * 10^q (rule 135).
      llvm::Value* i = toI64(a);
      llvm::Value* p = i64(pliPow10(a.ty.scale));
      llvm::Value* t = b_.CreateSDiv(i, p, "trunci");
      v.ty = a.ty;
      v.reg = b_.CreateTrunc(b_.CreateMul(t, p, "truncd"), llvmTy(a.ty), "trunc");
    } else {
      v = a;
    }
    result = v;
    return true;
  }
  if (e->name == "PRECISION") {
    Val a = emitExpr(e->args[0].get());
    v = convert(a, e->ty, e->loc);
    result = v;
    return true;
  }
  if (e->name == "MIN" || e->name == "MAX") {
    Val a = emitExpr(e->args[0].get());
    Val b = emitExpr(e->args[1].get());
    const Type& common = e->ty;
    Val av = convert(a, common, e->loc);
    Val bv = convert(b, common, e->loc);
    llvm::Value* cmp = common.k == TK::Float ? b_.CreateFCmpOLT(av.reg, bv.reg, "mincmp")
                                             : b_.CreateICmpSLT(av.reg, bv.reg, "mincmp");
    llvm::Value* r = e->name == "MIN" ? b_.CreateSelect(cmp, av.reg, bv.reg, "min")
                                      : b_.CreateSelect(cmp, bv.reg, av.reg, "max");
    v.ty = common;
    v.reg = r;
    result = v;
    return true;
  }
  if (e->name == "MOD") {
    Val a = emitExpr(e->args[0].get());
    Val b = emitExpr(e->args[1].get());
    const Type& common = e->ty;
    Val av = convert(a, common, e->loc);
    Val bv = convert(b, common, e->loc);
    v.ty = common;
    if (common.k == TK::Float) {
      // ZERODIVIDE (rule 94): a zero divisor traps, resuming with 0.
      llvm::Value* dz = b_.CreateFCmpOEQ(bv.reg, flt(0.0), "zdiv");
      llvm::Value* m = b_.CreateCall(runtimeFn("pli_mod_dd"), {av.reg, bv.reg}, "mod");
      v.reg = zerodivideResume(dz, m, flt(0.0));
    } else {
      // ZERODIVIDE (rule 94): a zero divisor traps, resuming with 0 (the
      // runtime already yields 0 for this case; the trap adds notification).
      llvm::Value* bi = toI64(bv);
      llvm::Value* dz = b_.CreateICmpEQ(bi, i64(0), "zdiv");
      llvm::Value* m = b_.CreateCall(runtimeFn("pli_mod_ll"), {toI64(av), bi});
      v.reg = b_.CreateTrunc(zerodivideResume(dz, m, i64(0)), b_.getInt32Ty(), "mod32");
    }
    result = v;
    return true;
  }
  if (e->name == "MULTIPLY") {
    Val a = emitExpr(e->args[0].get());
    Val b = emitExpr(e->args[1].get());
    const Type& common = e->ty;
    Val av, bv;
    if (common.isFixed()) {
      // Product scale is the sum of the operand scales (ADR-006): multiply
      // the raw scaled integers without rescaling either operand first.
      Type wa = common;
      wa.scale = a.ty.isFixed() ? a.ty.scale : 0;
      Type wb = common;
      wb.scale = b.ty.isFixed() ? b.ty.scale : 0;
      av = convert(a, wa, e->loc);
      bv = convert(b, wb, e->loc);
    } else {
      av = convert(a, common, e->loc);
      bv = convert(b, common, e->loc);
    }
    // FIXED overflow (QR1.2): like Binary `*`, a wrapped product traps.
    llvm::Value* r = common.k == TK::Float  ? b_.CreateFMul(av.reg, bv.reg, "mul")
                      : common.isFixed()     ? checkedArith(Tok::Star, av.reg, bv.reg)
                                             : b_.CreateMul(av.reg, bv.reg, "mul");
    v.ty = common;
    v.reg = r;
    result = v;
    return true;
  }
  if (e->name == "DIVIDE") {
    Val a = emitExpr(e->args[0].get());
    Val b = emitExpr(e->args[1].get());
    const Type& common = e->ty;
    Val av = convert(a, common, e->loc);
    Val bv = convert(b, common, e->loc);
    v.ty = common;
    // ZERODIVIDE (rule 94): as for `/`, a zero divisor traps, resuming with 0.
    llvm::Value* dz = b_.CreateFCmpOEQ(bv.reg, flt(0.0), "zdiv");
    llvm::Value* div = b_.CreateFDiv(av.reg, bv.reg, "div");
    v.reg = zerodivideResume(dz, div, flt(0.0));
    result = v;
    return true;
  }
  if (e->name == "ROUND") {
    Val x = emitExpr(e->args[0].get());
    Val n = emitExpr(e->args[1].get());
    Val xd = convert(x, Type::flt(6), e->loc);
    v.ty = e->ty;
    v.reg = b_.CreateCall(runtimeFn("pli_round"), {xd.reg, toI64(n)}, "round");
    result = v;
    return true;
  }
  if (e->name == "REPEAT") {
    Val s = emitExpr(e->args[0].get());
    Val n = emitExpr(e->args[1].get());
    Val out = charTemp(e->ty.len);
    b_.CreateCall(runtimeFn("pli_repeat"), {out.ptr, out.len, s.ptr, s.len, toI64(n)});
    out.len = i64(e->ty.len);
    result = out;
    return true;
  }
  if (e->name == "VERIFY") {
    Val s = emitExpr(e->args[0].get());
    Val t = emitExpr(e->args[1].get());
    llvm::Value* r = b_.CreateCall(runtimeFn("pli_verify"), {s.ptr, s.len, t.ptr, t.len});
    v.ty = e->ty;
    v.reg = b_.CreateTrunc(r, b_.getInt32Ty(), "ver32");
    result = v;
    return true;
  }
  if (e->name == "TRANSLATE") {
    Val s = emitExpr(e->args[0].get());
    Val out = emitExpr(e->args[1].get());
    Val in = emitExpr(e->args[2].get());
    Val dst = charTemp(e->ty.len);
    b_.CreateCall(runtimeFn("pli_translate"),
                  {dst.ptr, dst.len, s.ptr, s.len, out.ptr, out.len, in.ptr, in.len});
    dst.len = i64(e->ty.len);
    result = dst;
    return true;
  }
  if (e->name == "TRIM") {
    Val s = emitExpr(e->args[0].get());
    Val dst = charTemp(e->ty.len);
    llvm::Value* pad = llvm::ConstantPointerNull::get(b_.getPtrTy());
    llvm::Value* padlen = i64(0);
    if (e->args.size() > 1) {
      Val p = emitExpr(e->args[1].get());
      pad = p.ptr;
      padlen = p.len;
    }
    b_.CreateCall(runtimeFn("pli_trim"), {dst.ptr, dst.len, s.ptr, s.len, pad, padlen});
    dst.len = i64(e->ty.len);
    result = dst;
    return true;
  }
  if (e->name == "TALLY") {
    Val x = emitExpr(e->args[0].get());
    Val y = emitExpr(e->args[1].get());
    llvm::Value* r = b_.CreateCall(runtimeFn("pli_tally"), {x.ptr, x.len, y.ptr, y.len});
    v.ty = e->ty;
    v.reg = b_.CreateTrunc(r, b_.getInt32Ty(), "tal32");
    result = v;
    return true;
  }
  if (e->name == "HIGH" || e->name == "LOW") {
    Val n = emitExpr(e->args[0].get());
    Val out = charTemp(e->ty.len);
    std::string fn = e->name == "HIGH" ? "pli_high" : "pli_low";
    b_.CreateCall(runtimeFn(fn), {out.ptr, toI64(n)});
    out.len = i64(e->ty.len);
    result = out;
    return true;
  }
  if (e->name == "DATE" || e->name == "TIME") {
    Val out = charTemp(e->ty.len);
    std::string fn = e->name == "DATE" ? "pli_date" : "pli_time";
    b_.CreateCall(runtimeFn(fn), {out.ptr, out.len});
    out.len = i64(e->ty.len);
    result = out;
    return true;
  }
  // ONCODE (rules (91)-(94)): the current ERROR code (1 inside a SIGNAL-raised
  // unit, 0 elsewhere); the runtime owns the value, codegen just reads it.
  if (e->name == "ONCODE") {
    v.ty = e->ty;
    v.reg = b_.CreateCall(runtimeFn("pli_oncode"), {}, "oncode");
    result = v;
    return true;
  }
  // Array attribute built-ins (M2, rule (123)): constant bounds fold to a
  // compile-time value. The argument is the unsubscripted array reference;
  // read its bounds from the symbol rather than emitting the array value.
  // LBOUND/HBOUND report the first dimension; DIM reports the total element
  // count (the product over all axes).
  if (e->name == "LBOUND" || e->name == "HBOUND" || e->name == "DIM") {
    HExpr* a = e->args[0].get();
    // The argument is an unsubscripted array reference, either a plain array
    // (a->sym) or a qualified structure member array S.V (a->sym + memberPath).
    const Type& arr = (a->sym && !a->memberPath.empty())
                          ? memberType(a->sym, a->memberPath)
                          : (a->sym ? a->sym->ty : Type::fixedBin(31, 0));
    v.ty = e->ty;
    // A dynamic (runtime-extent) array reports the live lower/upper bounds (a
    // constant lower bound stays constant), read from the recorded dope slot
    // (the member slot for a dynamic member, the symbol slot for a plain array).
    if (arr.isArray() && arr.isDynamic()) {
      const Dim& d0 = arr.dims[0];
      llvm::Value* lb = nullptr;
      llvm::Value* ub = nullptr;
      if (!a->memberPath.empty()) {
        auto it = memberDyn_.find(MemberDyn{a->sym, a->memberPath});
        llvm::Value* mulb = it != memberDyn_.end() ? it->second.lb : nullptr;
        llvm::Value* muub = it != memberDyn_.end() ? it->second.ub : nullptr;
        lb = d0.lbDyn ? (mulb ? mulb : i64(d0.lb)) : i64(d0.lb);
        ub = muub ? muub : i64(d0.ub);
      } else {
        lb = d0.lbDyn ? (dynLb_.count(a->sym) ? dynLb_[a->sym] : i64(d0.lb)) : i64(d0.lb);
        ub = dynUb_.count(a->sym) ? dynUb_[a->sym] : i64(d0.ub);
      }
      // DIM of a dynamic multi-axis array is the first-axis runtime extent times
      // the (fixed) product of the later axes' extents.
      long long rest = 1;
      for (size_t k = 1; k < arr.dims.size(); ++k)
        rest *= (arr.dims[k].ub - arr.dims[k].lb + 1);
      llvm::Value* raw = e->name == "LBOUND" ? lb
                         : e->name == "HBOUND"
                             ? ub
                             : [&] {
                                 llvm::Value* dim =
                                     b_.CreateAdd(b_.CreateSub(ub, lb, "e1"), i64(1), "ext");
                                 if (rest != 1)
                                   dim = b_.CreateMul(dim, i64(rest), "extall");
                                 return dim;
                               }();
      Val src;
      src.ty = Type::fixedBin(63, 0);
      src.reg = raw;
      v.reg = convert(src, e->ty, e->loc).reg;
      result = v;
      return true;
    }
    long long lb = arr.isArray() ? arr.dims[0].lb : 1;
    long long ub = arr.isArray() ? arr.dims[0].ub : 1;
    long long val = e->name == "LBOUND"   ? lb
                    : e->name == "HBOUND" ? ub
                                          : (arr.isArray() ? arrayExtent(arr) : 1);
    v.reg = llvm::ConstantInt::get(llvmTy(e->ty), val, true);
    result = v;
    return true;
  }
  // Array reduction built-ins (M2, rule (123)): walk the whole single-axis
  // extent and reduce — SUM/PROD over numeric elements, ANY/ALL over BIT
  // elements. The accumulator and counter live in entry allocas so they
  // survive the emitted loop's basic blocks.
  if (e->name == "SUM" || e->name == "PROD" || e->name == "ANY" || e->name == "ALL") {
    HExpr* a = e->args[0].get();
    if (!a->sym) {
      v.ty = e->ty;
      v.reg = llvm::Constant::getNullValue(llvmTy(e->ty));
      result = v;
      return true;
    }
    // The argument is an unsubscripted array reference: a plain array (a->sym)
    // or a qualified structure member array S.V (a->sym + memberPath).
    const bool isMember = !a->memberPath.empty();
    const Type& arr = isMember ? memberType(a->sym, a->memberPath) : a->sym->ty;
    const Type& el = arr.elementType();
    const bool isBit = e->name == "ANY" || e->name == "ALL";
    const bool isFloat = !isBit && el.k == TK::Float;
    llvm::Value *ub = nullptr, *lb = nullptr;
    llvm::Value* base =
        isMember ? (arr.isDynamic() ? dynamicMemberBase(a->sym, a->memberPath, e->loc, ub, lb)
                                    : memberAddr(a->sym, a->memberPath, e->loc))
                 : addressOf(a->sym);
    // A dynamic (runtime-extent) array is a bare element buffer sized by the
    // live bound; a fixed array is [N x elem]. Drive the loop by the element
    // count and address elements accordingly (rules (12),(13),(123)).
    const bool dyn = arr.isArray() && arr.isDynamic();
    llvm::Value* n;
    llvm::Type* arrTy = nullptr;
    if (dyn) {
      const Dim& d0 = arr.dims[0];
      llvm::Value *mub = nullptr, *mlb = nullptr;
      if (isMember) {
        auto it = memberDyn_.find(MemberDyn{a->sym, a->memberPath});
        mub = it != memberDyn_.end() ? it->second.ub : nullptr;
        mlb = it != memberDyn_.end() ? it->second.lb : nullptr;
      }
      llvm::Value* rlb = d0.lbDyn
                             ? (isMember ? (mlb ? mlb : i64(d0.lb))
                                         : (dynLb_.count(a->sym) ? dynLb_[a->sym] : i64(d0.lb)))
                             : i64(d0.lb);
      llvm::Value* rub = isMember ? (mub ? mub : i64(d0.ub))
                                  : (dynUb_.count(a->sym) ? dynUb_[a->sym] : i64(d0.ub));
      n = b_.CreateAdd(b_.CreateSub(rub, rlb, "e1"), i64(1), "rdn");
      long long rest = 1;
      for (size_t k = 1; k < arr.dims.size(); ++k)
        rest *= (arr.dims[k].ub - arr.dims[k].lb + 1);
      if (rest != 1)
        n = b_.CreateMul(n, i64(rest), "rdnall");
    } else {
      n = i64(arrayExtent(arr));
      arrTy = llvm::ArrayType::get(llvmTy(el), (unsigned)arrayExtent(arr));
    }

    std::string id = std::to_string(n_++);
    // Accumulator identity: 0 for SUM, 1 for PROD, false for ANY, true for ALL.
    llvm::Value* accInit;
    if (e->name == "SUM")
      accInit = llvm::Constant::getNullValue(llvmTy(e->ty));
    else if (e->name == "PROD")
      accInit = isFloat ? flt(1.0) : llvm::ConstantInt::get(llvmTy(e->ty), 1, true);
    else if (e->name == "ANY")
      accInit = b_.getInt1(false);
    else
      accInit = b_.getInt1(true); // ALL
    llvm::AllocaInst* acc = entryAlloca(isBit ? b_.getInt1Ty() : llvmTy(e->ty), "rd.acc." + id);
    b_.CreateStore(accInit, acc);
    llvm::AllocaInst* ctr = entryAlloca(b_.getInt64Ty(), "rd.i." + id);
    b_.CreateStore(i64(0), ctr);

    llvm::BasicBlock* condL = llvm::BasicBlock::Create(ctx_, "rd.cond." + id, curFn_);
    llvm::BasicBlock* bodyL = llvm::BasicBlock::Create(ctx_, "rd.body." + id, curFn_);
    llvm::BasicBlock* stepL = llvm::BasicBlock::Create(ctx_, "rd.step." + id, curFn_);
    llvm::BasicBlock* endL = llvm::BasicBlock::Create(ctx_, "rd.end." + id, curFn_);
    branch(condL);
    startBlock(condL);
    llvm::Value* c = b_.CreateLoad(b_.getInt64Ty(), ctr, "rdc");
    b_.CreateCondBr(b_.CreateICmpSLT(c, n, "rdcmp"), bodyL, endL);
    startBlock(bodyL);
    llvm::Value* ep = dyn ? b_.CreateInBoundsGEP(llvmTy(el), base, {c}, "rdp")
                          : b_.CreateInBoundsGEP(arrTy, base, {i64(0), c}, "rdp");
    Val ev;
    ev.ty = el;
    if (isBit)
      ev.reg = b_.CreateTrunc(b_.CreateLoad(llvmTy(el), ep, "rdb"), b_.getInt1Ty(), "rdb1");
    else
      ev.reg = b_.CreateLoad(llvmTy(el), ep, "rdl");
    llvm::Value* av = b_.CreateLoad(isBit ? b_.getInt1Ty() : llvmTy(e->ty), acc, "rdacc");
    llvm::Value* nv;
    if (e->name == "SUM")
      nv = isFloat ? b_.CreateFAdd(av, ev.reg, "rds") : b_.CreateAdd(av, ev.reg, "rds");
    else if (e->name == "PROD")
      nv = isFloat ? b_.CreateFMul(av, ev.reg, "rds") : b_.CreateMul(av, ev.reg, "rds");
    else if (e->name == "ANY")
      nv = b_.CreateOr(av, ev.reg, "rdo");
    else
      nv = b_.CreateAnd(av, ev.reg, "rda");
    b_.CreateStore(nv, acc);
    branch(stepL);
    startBlock(stepL);
    b_.CreateStore(b_.CreateAdd(c, i64(1), "rdinc"), ctr);
    branch(condL);
    startBlock(endL);
    v.ty = e->ty;
    v.reg = b_.CreateLoad(isBit ? b_.getInt1Ty() : llvmTy(e->ty), acc, "rdres");
    result = v;
    return true;
  }
  return false;
}
