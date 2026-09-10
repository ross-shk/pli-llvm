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
    // members, recursively laid out in declaration order.
    std::vector<llvm::Type*> mts;
    for (const auto& m : t.members)
      mts.push_back(llvmTy(m->ty));
    return llvm::StructType::get(ctx_, mts);
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
  llvm::FunctionType* ft = llvm::FunctionType::get(b_.getVoidTy(), pt, false);
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

  // Pre-declare every procedure's functions/aliases so a call site resolves
  // regardless of the order the (flattened) procedures are emitted in.
  for (auto& p : prog.procs)
    declareProc(p.get());
  for (auto& p : prog.procs)
    emitProc(p.get());

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
    llvm::Value* buf = b_.CreateAlloca(llvmTy(el), extent, s->irName.substr(1) + ".dyn");
    symAddr_[s] = buf;
    if (s->dynUb)
      dynUb_[s] = ub;
    if (s->dynLb)
      dynLb_[s] = lb;
  }
}

// A dynamic (runtime-extent) array parameter is a by-reference pointer to the
// caller's data with no own storage, so its extent must be read once from the
// bound argument (itself by-ref) at entry. Record it so subscripting and the
// array built-ins bounds-check against the live extent (rules (12),(13),(34)).
void IRGen::recordDynParamUbs(const std::vector<Symbol*>& params) {
  for (Symbol* s : params)
    if (s->ty.isDynamic() && s->dynUb && !dynUb_.count(s))
      dynUb_[s] = toI64(emitExpr(s->dynUb));
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
  llvm::Type* ret = p->isFunction ? llvmTy(p->retTy) : b_.getVoidTy();
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
    for (size_t i = 0; i < p->paramSyms.size(); ++i)
      pt.push_back(b_.getPtrTy());
    for (Symbol* s : p->paramSyms)
      if (isAdjustable(s))
        pt.push_back(b_.getInt64Ty()); // hidden `*` extent args
    for (size_t i = 0; i < p->env.size(); ++i)
      pt.push_back(b_.getPtrTy()); // links
    llvm::FunctionType* ft = llvm::FunctionType::get(ret, pt, false);
    llvm::Function* fn =
        llvm::Function::Create(ft, llvm::Function::InternalLinkage, p->irName.substr(1), &mod_);
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
  llvm::FunctionType* ift = llvm::FunctionType::get(ret, pt, false);
  llvm::Function* impl = llvm::Function::Create(ift, llvm::Function::InternalLinkage,
                                                p->irName.substr(1) + ".impl", &mod_);

  // A thunk marshals one entry's arguments and tail-calls the shared impl.
  int thunkN = 0;
  auto thunk = [&](const std::vector<Symbol*>& mine, llvm::Value* selv) {
    std::vector<llvm::Type*> sig;
    for (size_t i = 0; i < mine.size(); ++i)
      sig.push_back(b_.getPtrTy());
    for (Symbol* s : mine)
      if (isAdjustable(s))
        sig.push_back(b_.getInt64Ty()); // hidden `*` extent args
    for (size_t i = 0; i < p->env.size(); ++i)
      sig.push_back(b_.getPtrTy()); // links
    llvm::FunctionType* tft = llvm::FunctionType::get(ret, sig, false);
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
    if (ret->isVoidTy())
      b_.CreateRetVoid();
    else
      b_.CreateRet(call);
    return tf;
  };

  llvm::Function* t0 = thunk(p->paramSyms, i64(0));
  t0->setName(p->irName.substr(1));
  for (const auto& en : p->entryNames)
    aliasFor(en, t0);
  for (size_t i = 0; i < entries.size(); ++i) {
    llvm::Function* tf = thunk(entries[i]->entryParamSyms, i64(i + 1));
    tf->setName(entryIrName(p->name, p->parent ? p->parent->name : "", entries[i]->name).substr(1));
  }
}

void IRGen::emitProc(HProc* p) {
  curProc_ = p;
  labelBlocks_.clear();
  symAddr_.clear();
  // Re-seed globals: static storage resolves the same in every procedure.
  for (Symbol* s : sema_.storage())
    if (s->isStatic && s->kind == Symbol::Var)
      symAddr_[s] = mod_.getGlobalVariable(s->irName.substr(1), true);

  llvm::Type* retLLVM = p->isFunction ? llvmTy(p->retTy) : b_.getVoidTy();

  // rule (56): ENTRY statements declare alternate entry points.
  std::vector<HStmt*> entries;
  for (auto& st : p->body)
    if (st && st->kind == HStmt::Entry)
      entries.push_back(st.get());

  if (entries.empty())
    emitPlainProc(p, retLLVM);
  else
    emitMultiEntryProc(p, entries, retLLVM);
  curProc_ = nullptr;
}

// One procedure, one LLVM function: the ordinary path (no ENTRY statements).
// The function and its entry-namelist aliases were pre-created by declareProc.
void IRGen::emitPlainProc(HProc* p, llvm::Type* retLLVM) {
  llvm::Function* fn = mod_.getFunction(p->irName.substr(1));
  curFn_ = fn;
  // The entry block must be the function's first block (so it is the real
  // entry, and the allocas it holds dominate every reachable block).
  llvm::BasicBlock* entry = llvm::BasicBlock::Create(ctx_, "entry", fn);
  for (auto& st : p->body)
    collectGotoBlocks(st.get());

  // Parameter arguments become their symbols' addresses (PL/I by reference);
  // each `*`-extent parameter (rule 13) then reads its hidden i64 extent into a
  // dope slot; the trailing args are the static links (rule (8)).
  size_t ai = 0;
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

  for (auto& st : p->body)
    emitStmt(st.get());

  if (!blockTerminated(b_.GetInsertBlock())) {
    if (p->isFunction)
      b_.CreateRet(llvm::Constant::getNullValue(retLLVM)); // fall-off: return a zero value
    else
      b_.CreateRetVoid();
  }
  curFn_ = nullptr;
}

// rule (56): a procedure with ENTRY statements. The shared implementation
// function (pre-created by declareProc) carries the whole body split into
// segments; each entry point's thunk was built by declareProc.
void IRGen::emitMultiEntryProc(HProc* p, const std::vector<HStmt*>& entries, llvm::Type* retLLVM) {
  llvm::Function* impl = mod_.getFunction(p->irName.substr(1) + ".impl");
  curFn_ = impl;
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
  size_t seg = 0;
  startBlock(segs[0]);
  for (auto& st : p->body) {
    if (st && st->kind == HStmt::Entry) {
      ++seg;
      startBlock(segs[seg]); // fall through from the previous segment
      continue;
    }
    emitStmt(st.get());
  }
  if (!blockTerminated(b_.GetInsertBlock())) {
    if (p->isFunction)
      b_.CreateRet(llvm::Constant::getNullValue(retLLVM));
    else
      b_.CreateRetVoid();
  }
  curFn_ = nullptr;
}

std::string IRGen::entryIrName(const std::string& proc, const std::string& parent,
                               const std::string& entry) {
  return "@PLI_" + (parent.empty() ? std::string() : parent + "$") + proc + "$entry$" + entry;
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
  case HStmt::Begin: // a block executes its body as a group (rule (68))
    for (auto& b : s->body)
      emitStmt(b.get());
    break;
  case HStmt::DoWhile:
    emitDoWhile(s);
    break;
  case HStmt::DoIter:
    emitDoIter(s);
    break;
  case HStmt::Put:
    emitPut(s);
    break;
  case HStmt::CallS:
    emitCall(s);
    break;
  case HStmt::Return: {
    if (curProc_->isFunction) {
      Val v = emitExpr(s->value.get());
      Val rv = convert(v, curProc_->retTy, s->loc);
      llvm::Value* reg = rv.reg;
      if (curProc_->retTy.isBit()) { // BIT returns are held in i8
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
    break;
  case HStmt::Goto:
    b_.CreateBr(labelBlocks_[s->name]);
    break;
  }
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
          llvm::Value* addr =
              arrayElementAddr(arr, memberAddr(t->sym, t->memberPath, s->loc), t->args, s->loc);
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
          llvm::Value* addr = memberAddr(t->sym, t->memberPath, s->loc);
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
    emitByNameCopy(dst, src, t->ty, v->ty, s->loc);
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
      llvm::Value* addr =
          arrayElementAddr(arr, memberAddr(t->sym, t->memberPath, s->loc), t->args, s->loc);
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
    HExpr* t = s->target.get();
    HExpr* v = s->value.get();
    if (v->kind != HExpr::VarRef || !v->sym || !v->ty.isStruct()) {
      d_.error(s->loc,
               "right-hand side of a whole-structure assignment must be a structure reference",
               "(127)");
      return;
    }
    llvm::Value* dst =
        t->memberPath.empty() ? addressOf(t->sym) : memberAddr(t->sym, t->memberPath, s->loc);
    llvm::Value* src =
        v->memberPath.empty() ? addressOf(v->sym) : memberAddr(v->sym, v->memberPath, s->loc);
    llvm::Type* sty = llvmTy(t->ty);
    llvm::Value* sz = i64(mod_.getDataLayout().getTypeStoreSize(sty));
    b_.CreateMemCpy(dst, llvm::MaybeAlign(), src, llvm::MaybeAlign(), sz);
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
  // Whole-structure assignment (rule 127) is not served in this stage.
  if (s->target->kind == HExpr::VarRef && s->target->sym && s->target->ty.isStruct()) {
    d_.error(s->loc, "whole-structure assignment is not implemented in this stage", "(127)");
    return;
  }
  if (s->target->kind != HExpr::VarRef || !s->target->sym)
    return;
  Val v = emitExpr(s->value.get());
  storeTo(s->target->sym, v, s->loc);
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
  branch(condL);
  startBlock(condL);
  Val c = emitExpr(s->cond.get());
  b_.CreateCondBr(toI1(c, s->loc), bodyL, endL);
  startBlock(bodyL);
  for (auto& b : s->body)
    emitStmt(b.get());
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
  for (auto& b : s->body)
    emitStmt(b.get());
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
      b_.CreateCall(runtimeFn("pli_put_list_fixed"), {toI64(v)});
      break;
    case TK::Void:
      break;
    case TK::Struct:
      // Whole-structure values are diagnosed in emitExpr (rule 127); a
      // structure never reaches list-directed output as a value.
      break;
    }
  }
}

// Address of one call argument for a by-reference parameter (rule 4).
llvm::Value* IRGen::argAddr(HExpr* a, const Type& pty) {
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
      llvm::Value* off = b_.CreateSub(i, lb, "off");
      flat = b_.CreateAdd(flat, b_.CreateMul(off, i64(stride), "scaled"), "flat");
      stride *= (dk.ub - dk.lb + 1); // later axes are fixed; only axis 0 is runtime
    }
    std::string id = std::to_string(n_++);
    llvm::BasicBlock* failL = llvm::BasicBlock::Create(ctx_, "sub.fail." + id, curFn_);
    llvm::BasicBlock* okL = llvm::BasicBlock::Create(ctx_, "sub.ok." + id, curFn_);
    b_.CreateCondBr(oob, failL, okL);
    startBlock(failL);
    b_.CreateCall(runtimeFn("pli_subscript_oob"), {});
    b_.CreateUnreachable();
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
    llvm::Value* off = b_.CreateSub(i, i64(lb), "off");
    flat = b_.CreateAdd(flat, b_.CreateMul(off, i64(stride), "scaled"), "flat");
    stride *= (ub - lb + 1);
  }

  std::string id = std::to_string(n_++);
  llvm::BasicBlock* failL = llvm::BasicBlock::Create(ctx_, "sub.fail." + id, curFn_);
  llvm::BasicBlock* okL = llvm::BasicBlock::Create(ctx_, "sub.ok." + id, curFn_);
  b_.CreateCondBr(oob, failL, okL);

  startBlock(failL);
  b_.CreateCall(runtimeFn("pli_subscript_oob"), {});
  b_.CreateUnreachable();

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

// The resolved type of a qualified member S.A.B (rule 124): walk the recorded
// field indices (as memberAddr does, without emitting GEPs) to recover the leaf
// member's type — for a subscripted member array S.A(i), this is the array type.
const Type& IRGen::memberType(Symbol* base, const std::vector<unsigned>& path) {
  const Type* cur = &base->ty;
  for (unsigned f : path)
    cur = &cur->members[f]->ty;
  return *cur;
}

// BY NAME assignment (rule 86): copy each member of `dst` (at dstBase) from the
// same-named member of `src` (at srcBase). Minor structures recurse by name; an
// array member is copied whole (sema required identical array types); a scalar
// member is loaded, converted to the target type and stored.
void IRGen::emitByNameCopy(llvm::Value* dstBase, llvm::Value* srcBase, const Type& dst,
                           const Type& src, SourceLoc loc) {
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
      emitByNameCopy(d, s, dm.ty, sm.ty, loc);
    } else if (dm.ty.isArray()) {
      llvm::Value* sz = i64(mod_.getDataLayout().getTypeStoreSize(llvmTy(dm.ty)));
      b_.CreateMemCpy(d, llvm::MaybeAlign(), s, llvm::MaybeAlign(), sz);
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
      llvm::Type* arrTy = llvm::ArrayType::get(llvmTy(el), (unsigned)arrayExtent(m.ty));
      for (long long k = 0; k < arrayExtent(m.ty); ++k) {
        llvm::Value* ep = b_.CreateInBoundsGEP(arrTy, mem, {i64(0), i64(k)}, "init.el");
        if (el.isStruct())
          emitStructInitValues(ep, el, vals, idx, loc);
        else
          storeScalarTo(ep, el, initValue(el, vals[idx++]));
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
  if (ty.isBit()) {
    v.reg = b_.CreateTrunc(r, b_.getInt1Ty(), "b1");
  } else {
    v.reg = r;
  }
  return v;
}

void IRGen::storeScalarTo(llvm::Value* addr, const Type& ty, const Val& v) {
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
    std::string fid = std::to_string(n_++);
    llvm::BasicBlock* failL = llvm::BasicBlock::Create(ctx_, "cs.fail." + fid, curFn_);
    llvm::BasicBlock* okL = llvm::BasicBlock::Create(ctx_, "cs.ok." + fid, curFn_);
    b_.CreateCondBr(oob, failL, okL);
    startBlock(failL);
    b_.CreateCall(runtimeFn("pli_subscript_oob"), {});
    b_.CreateUnreachable();
    startBlock(okL);
    fixedFlat = b_.CreateAdd(
        fixedFlat, b_.CreateMul(b_.CreateSub(iv, lb, "off"), i64(stride[k]), "scaled"), "ff");
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
  std::string id = std::to_string(n_++);
  llvm::BasicBlock* failL = llvm::BasicBlock::Create(ctx_, "def.fail." + id, curFn_);
  llvm::BasicBlock* okL = llvm::BasicBlock::Create(ctx_, "def.ok." + id, curFn_);
  b_.CreateCondBr(oob, failL, okL);
  startBlock(failL);
  b_.CreateCall(runtimeFn("pli_subscript_oob"), {});
  b_.CreateUnreachable();
  startBlock(okL);

  llvm::Value* flat = i64(0);
  long long stride = 1;
  for (size_t k = n; k-- > 0;) {
    llvm::Value* iv = (int)k == y->definedIsubAxis ? bidx : i64(y->definedConst[k]);
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
    bool rescale = dst.k == TK::FixedDec && v.ty.scale != dst.scale;
    if (!rescale && (v.ty.k == TK::FixedDec && v.ty.scale > 0 && dst.k != TK::FixedDec))
      rescale = true; // DECIMAL source -> BINARY target: drop the fraction
    if (!rescale) {
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
      r = b_.CreateMul(r, i64(pliPow10(dq)), "res");
    } else {
      int k = -dq;
      llvm::Value* div = i64(pliPow10(k));
      // round half away from zero: r + 5*10^(k-1)*sign
      llvm::Value* sign = b_.CreateSelect(b_.CreateICmpSLT(r, i64(0), "sgn"), i64(-1), i64(1));
      llvm::Value* adj = b_.CreateMul(i64(pliPow10(k - 1) * 5), sign, "adj");
      r = b_.CreateSDiv(b_.CreateAdd(r, adj, "rn"), div, "res");
    }
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
      // lives at memberAddr(...) (a [N x elemTy] field), so GEP into it as a
      // normal array and load the leaf element.
      const Type& arr = memberType(e->sym, e->memberPath);
      const Type& el = e->ty;
      if (el.isChar()) {
        d_.error(e->loc, "arrays of CHARACTER members are not implemented in this stage", "(12)");
        v.ty = el;
        v.reg = i64(0);
        return v;
      }
      llvm::Value* addr =
          arrayElementAddr(arr, memberAddr(e->sym, e->memberPath, e->loc), e->args, e->loc);
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
      llvm::Value* addr = memberAddr(e->sym, e->memberPath, e->loc);
      v.ty = leaf;
      llvm::Value* r = b_.CreateLoad(llvmTy(leaf), addr, "mld");
      v.reg = leaf.isBit() ? b_.CreateTrunc(r, b_.getInt1Ty(), "b1") : r;
      return v;
    }
    if (e->ty.isStruct()) {
      // A whole structure as a value (rule 127) is not served in this stage.
      d_.error(e->loc, "a whole structure cannot be used as a value in this stage", "(127)");
      v.ty = e->ty;
      v.reg = i64(0);
      return v;
    }
    return loadSym(e->sym, e->sym->ty);
  case HExpr::Call: {
    // Built-ins are emitted in emitBuiltin; a non-builtin call (a user
    // function procedure) falls through to the general path below.
    if (emitBuiltin(e, v))
      return v;
    if (!e->sym || !e->sym->proc) {
      v.ty = e->ty;
      v.reg = i64(0);
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
    llvm::CallInst* call = b_.CreateCall(calleeFn, args, "fres");
    v.ty = rty;
    if (rty.isBit()) {
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
    else
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
  switch (op) {
  case Tok::Plus:
    r = flt ? b_.CreateFAdd(av.reg, bv.reg, "bin") : b_.CreateAdd(av.reg, bv.reg, "bin");
    break;
  case Tok::Minus:
    r = flt ? b_.CreateFSub(av.reg, bv.reg, "bin") : b_.CreateSub(av.reg, bv.reg, "bin");
    break;
  case Tok::Star:
    r = flt ? b_.CreateFMul(av.reg, bv.reg, "bin") : b_.CreateMul(av.reg, bv.reg, "bin");
    break;
  case Tok::Slash:
    r = b_.CreateFDiv(av.reg, bv.reg, "bin");
    break;
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
      v.reg = b_.CreateCall(runtimeFn("pli_mod_dd"), {av.reg, bv.reg}, "mod");
    } else {
      llvm::Value* r = b_.CreateCall(runtimeFn("pli_mod_ll"), {toI64(av), toI64(bv)});
      v.reg = b_.CreateTrunc(r, b_.getInt32Ty(), "mod32");
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
    llvm::Value* r = common.k == TK::Float ? b_.CreateFMul(av.reg, bv.reg, "mul")
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
    v.reg = b_.CreateFDiv(av.reg, bv.reg, "div");
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
  // Array attribute built-ins (M2, rule (123)): constant bounds fold to a
  // compile-time value. The argument is the unsubscripted array reference;
  // read its bounds from the symbol rather than emitting the array value.
  // LBOUND/HBOUND report the first dimension; DIM reports the total element
  // count (the product over all axes).
  if (e->name == "LBOUND" || e->name == "HBOUND" || e->name == "DIM") {
    HExpr* a = e->args[0].get();
    const Type& arr = a->sym ? a->sym->ty : Type::fixedBin(31, 0);
    v.ty = e->ty;
    // A dynamic (runtime-extent) array reports the live lower/upper bounds (a
    // constant lower bound stays constant), read from the recorded dope slot.
    if (arr.isArray() && arr.isDynamic()) {
      const Dim& d0 = arr.dims[0];
      llvm::Value* lb =
          d0.lbDyn ? (dynLb_.count(a->sym) ? dynLb_[a->sym] : i64(d0.lb)) : i64(d0.lb);
      llvm::Value* ub = d0.dyn ? dynUb_[a->sym] : i64(d0.ub);
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
    const Type& arr = a->sym->ty;
    const Type& el = arr.elementType();
    const bool isBit = e->name == "ANY" || e->name == "ALL";
    const bool isFloat = !isBit && el.k == TK::Float;
    llvm::Value* base = addressOf(a->sym);
    // A dynamic (runtime-extent) array is a bare element buffer sized by the
    // live bound; a fixed array is [N x elem]. Drive the loop by the element
    // count and address elements accordingly (rules (12),(13),(123)).
    const bool dyn = arr.isArray() && arr.isDynamic();
    llvm::Value* n;
    llvm::Type* arrTy = nullptr;
    if (dyn) {
      const Dim& d0 = arr.dims[0];
      llvm::Value* ub = dynUb_.count(a->sym) ? dynUb_[a->sym] : i64(d0.ub);
      llvm::Value* lb =
          d0.lbDyn ? (dynLb_.count(a->sym) ? dynLb_[a->sym] : i64(d0.lb)) : i64(d0.lb);
      n = b_.CreateAdd(b_.CreateSub(ub, lb, "e1"), i64(1), "rdn");
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
