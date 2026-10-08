// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM
#include "irgen.h"
#include <algorithm>
#include <set>

#ifndef PLIC_LLVM_VERSION
#define PLIC_LLVM_VERSION "unknown"
#endif

#include "llvm/AsmParser/Parser.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
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
// matching convert(): DECIMAL targets round half away from zero, BINARY
// targets truncate toward zero.
static long long pliRescaleDown(long long v, int k, bool decTarget) {
  long long p = pliPow10(k);
  if (!decTarget)
    return v / p; // C++ division truncates toward zero
  long long sign = v < 0 ? -1 : 1;
  return (v + pliPow10(k - 1) * 5 * sign) / p;
}

// 10^k as a compile-time 128-bit value (ADR-191): wide FIXED DECIMAL
// (>18 digit) rescale constants (e.g. 10^25) exceed an i64, so the i64
// helpers above are insufficient for the wide path. PliI128 keeps this
// portable (MSVC lacks __int128; clang-cl supports it natively).
static PliI128 pliPow10_128(int k) {
  PliI128 p = 1;
  for (int i = 0; i < k; ++i)
    p = p * 10;
  return p;
}

// Compile-time scale reduction in i128 (ADR-191): the wide counterpart of
// pliRescaleDown for DECIMAL literals whose scaled value exceeds i64.
static PliI128 pliRescaleDown128(PliI128 v, int k, bool decTarget) {
  PliI128 p = pliPow10_128(k);
  if (!decTarget)
    return v / p; // C++ division truncates toward zero
  PliI128 sign = v < 0 ? -1 : 1;
  return (v + pliPow10_128(k - 1) * 5 * sign) / p;
}

// Rescaled scaled-integer value of a DECIMAL literal for FIXED target t
// (ADR-191): computed in i128 from the full wideIval so >18-digit literals
// never read the wrapped i64 truncation.
static PliI128 decLitRescaled128(const Expr* e, const Type& t) {
  PliI128 w = e->wideIval;
  int dq = t.scale - e->decScale;
  return dq > 0   ? w * pliPow10_128(dq)
         : dq < 0 ? pliRescaleDown128(w, -dq, t.k == TK::FixedDec)
                  : w;
}

// True when a DECIMAL literal needs the wide constant path (ADR-191): its
// full value exceeds i64, or the target itself is wide (a narrow literal
// scaled up into a wide target can overflow i64 mid-rescale).
static bool needsWideDecLit(const Expr* e, const Type& t) {
  return e && e->kind == Expr::DecLit && (t.intBits() == 128 || e->decPrec > 18);
}
// HIR twin of the above for emitted decimal literals.
static bool needsWideDecLit(const HExpr* e, const Type& t) {
  return e && e->kind == HExpr::DecLit && (t.intBits() == 128 || e->decPrec > 18);
}

// Numeric value of a constant INITIAL expression (rule 26). A DECIMAL literal
// holds its value scaled by 10^q, so divide back to the true value.
static double iniNumeric(const Expr* e) {
  if (!e)
    return 0;
  if (e->kind == Expr::FltLit)
    return e->fval;
  if (e->kind == Expr::DecLit)
    return pli_to_double(e->wideIval) / (double)pliPow10(e->decScale);
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

// True when the member at `path` from structure type `root` lands in a packed
// (UNALIGNED) structure: such an address is not guaranteed the member type's
// natural alignment, so its loads/stores must use align 1 (ADR-169). Checks
// every enclosing structure along the path, including the leaf itself.
static bool packedMemberPath(const Type& root, const std::vector<unsigned>& path) {
  const Type* cur = &root;
  for (unsigned f : path) {
    if (cur->unaligned)
      return true;
    if (f >= cur->members.size())
      return false;
    cur = &cur->members[f]->ty;
  }
  return cur->unaligned;
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
    // BIT(1) is a single i8 in storage (`i1` in registers); wider strings
    // pack big-endian, ceil(n/8) bytes (rule (18), QR2.2).
    if (t.len == 1)
      return b_.getInt8Ty();
    return llvm::ArrayType::get(b_.getInt8Ty(), (unsigned)bitBytes(t.len));
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
    // UNALIGNED structures are packed: no inter-member padding, matching a C
    // `__attribute__((packed))` record (ADR-169).
    return llvm::StructType::get(ctx_, mts, t.unaligned);
  }
  case TK::Pointer:
  case TK::Offset: // an OFFSET is an opaque locator (rule (22))
  case TK::Area:   // an AREA is a pointer to the runtime region (rule (20))
  case TK::Entry:  // an ENTRY value is a procedure pointer (ADR-171)
    return b_.getPtrTy();
  case TK::Task:
  case TK::Event:
    // A TASK handle id / EVENT completion flag (rules (15),(79),(82), QR2.8):
    // a plain i32 word owned by the PL/I variable; the runtime serialises it.
    return b_.getInt32Ty();
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

std::vector<unsigned char> IRGen::packBitLiteral(const std::string& sval, int nbits) {
  std::vector<unsigned char> bytes((nbits + 7) / 8, 0);
  for (int i = 0; i < nbits; ++i) {
    char c = i < (int)sval.size() ? sval[i] : '0';
    if (c == '1')
      bytes[i / 8] |= (unsigned char)(0x80 >> (i % 8));
  }
  return bytes;
}

llvm::Value* IRGen::packBitValue(llvm::Value* agg, int nbits, SourceLoc loc) {
  if (nbits > 64) {
    d_.error(loc,
             "conversion of a BIT string longer than 64 bits to a number is not implemented in "
             "this stage",
             "(86)");
    return i64(0);
  }
  int B = bitBytes(nbits);
  llvm::Value* acc = i64(0);
  for (int k = 0; k < B; ++k) {
    llvm::Value* byte = b_.CreateExtractValue(agg, (unsigned)k, "pkx");
    llvm::Value* wide = b_.CreateZExt(byte, b_.getInt64Ty(), "pkw");
    acc = b_.CreateOr(b_.CreateShl(acc, 8, "pks"), wide, "pka");
  }
  // Aggregates are left-justified; the integer value is right-justified.
  if (int extra = 8 * B - nbits)
    acc = b_.CreateLShr(acc, (uint64_t)extra, "pkj");
  return acc;
}

llvm::Value* IRGen::unpackBitValue(llvm::Value* intval, int nbits, SourceLoc loc) {
  int B = bitBytes(nbits);
  llvm::Type* arrTy = llvm::ArrayType::get(b_.getInt8Ty(), (unsigned)B);
  if (nbits > 64) {
    d_.error(loc,
             "conversion of a number to a BIT string longer than 64 bits is not implemented in "
             "this stage",
             "(86)");
    return llvm::UndefValue::get(arrTy);
  }
  // Left-justify the low nbits before splitting into bytes.
  if (int extra = 8 * B - nbits)
    intval = b_.CreateShl(intval, (uint64_t)extra, "upj");
  llvm::Value* agg = llvm::UndefValue::get(arrTy);
  for (int k = 0; k < B; ++k) {
    llvm::Value* shifted = b_.CreateLShr(intval, (uint64_t)(8 * (B - 1 - k)), "upsh");
    llvm::Value* byte = b_.CreateTrunc(shifted, b_.getInt8Ty(), "upb");
    agg = b_.CreateInsertValue(agg, byte, (unsigned)k, "upi");
  }
  return agg;
}

std::vector<unsigned char> IRGen::packBitInit(const Expr* ini, int nbits) {
  if (ini && ini->kind == Expr::BitLit)
    return packBitLiteral(ini->sval, nbits);
  std::vector<unsigned char> bytes(bitBytes(nbits), 0);
  if (!ini)
    return bytes;
  unsigned long long v = (unsigned long long)(ini->ival != 0 ? ini->ival : (long long)ini->fval);
  for (int k = (int)bytes.size(); k-- > 0;) {
    bytes[k] = (unsigned char)(v & 0xFF);
    v >>= 8;
  }
  return bytes;
}

llvm::Value* IRGen::i32(int v) { return b_.getInt32(v); }
llvm::Value* IRGen::i64(long long v) { return b_.getInt64(v); }
// i128 constant (ADR-191): wide FIXED DECIMAL (>18 digit) needs constants
// beyond i64, e.g. 10^25 for a DECIMAL(25,2) rescale. LLVM has no signed i128
// builder shortcut, so split into lo/hi u64 halves.
llvm::Value* IRGen::i128(PliI128 v) {
  uint64_t lo = pli_slo64(v);
  uint64_t hi = pli_shi64(v);
  return llvm::ConstantInt::get(llvm::IntegerType::get(ctx_, 128), llvm::APInt(128, {lo, hi}));
}
llvm::Value* IRGen::flt(double d) { return llvm::ConstantFP::get(b_.getDoubleTy(), d); }
// LLVM constant for a wide DECIMAL literal already rescaled to FIXED target
// t's scale (ADR-191): i128 for a wide target, truncated i64 otherwise.
llvm::Constant* IRGen::wideDecConstant(const Type& t, PliI128 w) {
  if (t.intBits() == 128)
    return llvm::cast<llvm::Constant>(i128(w));
  return llvm::ConstantInt::get(llvmTy(t), pli_to_i64(w), true);
}

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
enum RtTok { RtVoid, RtI64, RtI32, RtI8, RtDouble, RtPtr, RtI32Ptr, RtI128 };
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
#define IPTR RtI32Ptr
#define I128 RtI128
#define PLI_STRIP(...) __VA_ARGS__ // turn the .def's (a, b, c) into a braced list
#define PLI_FN(name, ret, args) {#name, {ret, {PLI_STRIP args}}},
#include "../runtime/pli_rt_abi.def"
#undef PLI_FN
#undef PLI_STRIP
#undef IPTR
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

// LLVM-only facts about pli_* entries, audited against runtime/*.c.
// The shared runtime/pli_rt_abi.def stays signature-only; this parallel
// table is consumed solely by runtimeFn. A wrong row is a miscompile, so
// each tier below names what was checked: abort paths end in exit(),
// alloc wraps malloc/free, pure math touches no memory (constant tables
// only), readers/writers touch only their pointer arguments and never
// signal. Entries with latent abort paths (substr_assign*), clock reads
// (date/time), or global state (ON stack, I/O, CONTROLLED, tasks) take
// the default: nounwind only. Universal nounwind holds because the
// runtime is C without EH and no entry unwinds its caller back into
// generated code (task spawn hands the pointer to pthread_create).
enum RtMem {
  RtMemDefault,
  RtMemNone,
  RtMemArgRead,
  RtMemArgReadWrite,
  RtMemReadonly,
  RtMemOtherMod
};
struct RtAttr {
  bool noReturn = false;
  bool willReturn = false;
  bool alwaysInline = false;
  RtMem mem = RtMemDefault;
  bool allocMalloc = false; // malloc-family: fresh, arg-0-sized object
  bool allocFree = false;   // free-family: releases a prior allocation
  bool allocSize = false;   // allocsize: result points at arg-0 bytes
  bool nonNullRet = false;
};
static const std::map<std::string, RtAttr>& kRuntimeAttrs() {
  static const std::map<std::string, RtAttr> table = {
      // Abort paths: fini + exit(), never return.
      {"pli_signal_error", {.noReturn = true}},
      {"pli_subscript_oob", {.noReturn = true}},
      {"pli_zerodivide", {.noReturn = true}},
      {"pli_fixed_overflow", {.noReturn = true}},
      {"pli_conversion", {.noReturn = true}},
      {"pli_stop", {.noReturn = true}},
      // Allocation: malloc/free wrappers (aborts on OOM, so no willreturn).
      {"pli_alloc", {.allocMalloc = true, .allocSize = true, .nonNullRet = true}},
      {"pli_free", {.willReturn = true, .allocFree = true}},
      // CONTROLLED allocation/dealloc: modify global generation stacks (ADR-111).
      // Modelled as all-location readwrite (not a narrower location): the
      // embedded runtime.bc carries clang-inferred location facts (readers are
      // `read, inaccessiblemem: none`), and link-time merging INTERSECTS
      // conflicting memory facts — a narrower claim here collapses to
      // `memory(none)` and lets LLVM CSE an addr across an alloc. Loop
      // hoisting is done explicitly in IRGen (emitDoIter), so LLVM needs no
      // facts beyond "these calls all touch the same state".
      {"pli_ctl_alloc", {.willReturn = false, .mem = RtMemOtherMod}},
      {"pli_ctl_alloc_dims", {.willReturn = false, .mem = RtMemOtherMod}},
      {"pli_ctl_ensure", {.willReturn = false, .mem = RtMemOtherMod}},
      {"pli_ctl_free", {.willReturn = false, .mem = RtMemOtherMod}},
      {"pli_ctl_set_dim", {.willReturn = false, .mem = RtMemOtherMod}},
      // CONTROLLED / ON / AREA readers: read global state, never write (ADR-111).
      // All-location read for the same link-merge reason as above.
      {"pli_ctl_addr", {.willReturn = true, .mem = RtMemReadonly}},
      {"pli_ctl_len", {.willReturn = true, .mem = RtMemReadonly}},
      {"pli_ctl_depth", {.willReturn = true, .mem = RtMemReadonly}},
      {"pli_ctl_extent", {.willReturn = true, .mem = RtMemReadonly}},
      {"pli_ctl_rank", {.willReturn = true, .mem = RtMemReadonly}},
      // Pure math: arithmetic over args and constant tables only.
      {"pli_mod_ll", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_mod_dd", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_round", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_floor", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_ceil", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_sqrt", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_exp", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_log", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_sin", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_cos", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_tan", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_log2", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_log10", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_atan", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_asin", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_acos", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_cbrt", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_sinh", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_cosh", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_tanh", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_asinh", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_atanh", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_erf", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_erfc", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_sind", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_cosd", {.willReturn = true, .mem = RtMemNone}},
      {"pli_tand", {.willReturn = true, .mem = RtMemNone}},
      {"pli_atand", {.willReturn = true, .mem = RtMemNone}},
      {"pli_fixed_of_float", {.willReturn = true, .mem = RtMemNone}},
      // String readers (non-retired W4/misc): arg-pointed memory only.
      {"pli_rank", {.willReturn = true, .mem = RtMemArgRead}},
      {"pli_fixed_of_char", {.willReturn = true, .mem = RtMemArgReadWrite}},
      {"pli_data_name_is", {.willReturn = true, .mem = RtMemArgRead}},
      {"pli_char_of_float", {.willReturn = true, .mem = RtMemArgReadWrite}},
      {"pli_char_of_fixed", {.willReturn = true, .mem = RtMemArgReadWrite}},
      // String writers (non-retired): arg-pointed memory only, no error paths.
      {"pli_concat", {.willReturn = true, .mem = RtMemArgReadWrite}},
      {"pli_collate", {.willReturn = true, .mem = RtMemArgReadWrite}},
  };
  return table;
}

// Stamp one declaration with its side-table facts. Idempotent: the same
// values apply whether the declaration is fresh or cached.
static void applyRuntimeAttrs(llvm::Function* f) {
  llvm::LLVMContext& ctx = f->getContext();
  f->addFnAttr(llvm::Attribute::NoUnwind);
  auto& attrs = kRuntimeAttrs();
  auto it = attrs.find(f->getName().str());
  if (it == attrs.end())
    return;
  const RtAttr& a = it->second;
  if (a.noReturn) {
    f->addFnAttr(llvm::Attribute::NoReturn);
    f->addFnAttr(llvm::Attribute::Cold);
  }
  if (a.willReturn)
    f->addFnAttr(llvm::Attribute::WillReturn);
  if (a.alwaysInline) {
    // linkRuntimeBitcode stamped NoInline on all runtime bodies to keep the
    // non-wrappers alive across -dead_strip; the always-inline math wrappers
    // must be inlined, so drop NoInline before forcing AlwaysInline.
    f->removeFnAttr(llvm::Attribute::NoInline);
    f->addFnAttr(llvm::Attribute::AlwaysInline);
  } else
    f->addFnAttr(llvm::Attribute::NoInline);
  switch (a.mem) {
  case RtMemNone:
    f->setMemoryEffects(llvm::MemoryEffects::none());
    break;
  case RtMemArgRead:
    f->setMemoryEffects(llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref));
    break;
  case RtMemArgReadWrite:
    f->setMemoryEffects(llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::ModRef));
    break;
  case RtMemReadonly:
    f->setMemoryEffects(llvm::MemoryEffects::readOnly());
    break;
  case RtMemOtherMod: {
    f->setMemoryEffects(llvm::MemoryEffects::unknown());
    break;
  }
  default:
    break;
  }
  if (a.allocMalloc) {
#if LLVM_VERSION_MAJOR >= 20
    f->addFnAttr(llvm::Attribute::getWithAllocKind(ctx, llvm::AllocFnKind::Alloc |
                                                            llvm::AllocFnKind::Uninitialized));
#else
    // LLVM <20 has no allockind attr (getWithAllocKind); allocsize alone.
    (void)ctx;
#endif
    if (a.allocSize)
      f->addFnAttr(llvm::Attribute::getWithAllocSizeArgs(ctx, 0, std::nullopt));
    if (a.nonNullRet)
      f->addRetAttr(llvm::Attribute::get(ctx, llvm::Attribute::NonNull));
  } else if (a.allocFree) {
#if LLVM_VERSION_MAJOR >= 20
    f->addFnAttr(llvm::Attribute::getWithAllocKind(ctx, llvm::AllocFnKind::Free));
#endif
  }
}

// --- Lowering policy (design-docs/mlir-runtime-migration-plan.md §5.2) -----

// P0 policy: every operation defaults to runtime. P5 flips entries one wave at
// a time as lowerings land. The table is operation-name → prefers direct LLVM.
// P5 flips: W1, W2, W3 default to LLVM. W4 remains runtime (binary size
// regression: 51632 B LLVM vs 35440 B runtime).
static const std::map<std::string, bool>& kRuntimeDefault() {
  static const std::map<std::string, bool> table = {
      {"assign_char", true},
      {"index", true},
      {"assign_varying", true},
      {"high", true},
      {"low", true},
      {"uppercase", true},
      {"lowercase", true},
      {"reverse", true},
      {"center", true},
      {"cmp_char", true},
      // P5 flip: W2 default is now LLVM (was runtime)
      {"verify", true},
      {"verify_from", true},
      {"search", true},
      {"tally", true},
      // P5 flip: W3 default is now LLVM (was runtime)
      {"substr", true},
      {"substr_assign", true},
      {"substr_assign_varying", true},
      {"repeat", true},
      {"translate", true},
      {"trim", true},
      // P4+: W4 scalar math and conversions default to runtime until P5/W4 flip
      {"fixed_of_float", false},
      {"fixed_of_char", false},
  };
  return table;
}

// P6: W1/W2 operations whose pli_* C bodies have been removed; the runtime
// path is no longer available. Explicit runtime requests fall through to
// the auto/LLVM path instead of calling the deleted symbol.
static const std::set<std::string>& kRetiredRuntimeOps() {
  static const std::set<std::string> table = {
      // W1 (P6/W1)
      "assign_char",
      "index",
      "assign_varying",
      "high",
      "low",
      "uppercase",
      "lowercase",
      "reverse",
      "center",
      "cmp_char",
      // W2 (P6/W2)
      "verify",
      "verify_from",
      "search",
      "tally",
      // W3 (P6/W3)
      "substr",
      "substr_assign",
      "substr_assign_varying",
      "repeat",
      "translate",
      "trim",
  };
  return table;
}

bool IRGen::isRetiredRuntimeOp(const std::string& op) { return kRetiredRuntimeOps().count(op) > 0; }

// Operations with a direct LLVM lowering implemented (populated P1+).
static const std::set<std::string>& kLLVMLowerings() {
  static const std::set<std::string> table = {
      "assign_char",
      "index",
      "assign_varying",
      "high",
      "low",
      "uppercase",
      "lowercase",
      "reverse",
      "center",
      "cmp_char",
      // W2 search/scan operations
      "verify",
      "verify_from",
      "search",
      "tally",
      // W3 clipping, padding, capacity operations
      "substr",
      "substr_assign",
      "substr_assign_varying",
      "repeat",
      "translate",
      "trim",
      // W4 scalar conversions
      "fixed_of_float",
      "fixed_of_char",
  };
  return table;
}

bool IRGen::hasLLVMLowering(const std::string& op) { return kLLVMLowerings().count(op) > 0; }

bool IRGen::useRuntimeCall(const std::string& op) {
  LowerMode mode = lowerMode_;
  auto oit = perOpOverrides_.find(op);
  bool perOpOverride = (oit != perOpOverrides_.end());
  if (perOpOverride)
    mode = oit->second;
  switch (mode) {
  case LowerMode::Runtime:
    if (isRetiredRuntimeOp(op)) {
      if (perOpOverride) {
        d_.error(SourceLoc{},
                 "runtime path for '" + op +
                     "' has been retired; "
                     "use --experimental-lowering=llvm for this operation",
                 "(P6)");
      } else {
        d_.warn(SourceLoc{},
                "runtime path for '" + op +
                    "' has been retired; "
                    "falling back to LLVM lowering",
                "(P6)");
      }
      return false;
    }
    return true;
  case LowerMode::MLIR:
    // MLIR not compiled in this build (P2+ gates it). Fall back to runtime,
    // but retired ops have no runtime — use LLVM if available.
    if (isRetiredRuntimeOp(op) && hasLLVMLowering(op))
      return false;
    return true;
  case LowerMode::LLVM:
    if (hasLLVMLowering(op))
      return false;
    return true;
  case LowerMode::Auto: {
    auto dit = kRuntimeDefault().find(op);
    if (dit != kRuntimeDefault().end() && dit->second && hasLLVMLowering(op))
      return false;
    return true;
  }
  }
  return true;
}

// --- P0/P1 pilot dispatch wrappers (design-docs/mlir-runtime-migration-plan.md) ===

void IRGen::emitAssignChar(llvm::Value* dst, llvm::Value* dstLen, llvm::Value* src,
                           llvm::Value* srcLen) {
  if (useRuntimeCall("assign_char")) {
    b_.CreateCall(runtimeFn("pli_assign_char"), {dst, dstLen, src, srcLen});
    return;
  }
  emitAssignCharLLVM(dst, dstLen, src, srcLen);
}

llvm::Value* IRGen::emitIndex(llvm::Value* a, llvm::Value* aLen, llvm::Value* b,
                              llvm::Value* bLen) {
  if (useRuntimeCall("index")) {
    return b_.CreateCall(runtimeFn("pli_index"), {a, aLen, b, bLen});
  }
  // P1 direct LLVM lowering for INDEX goes here.
  return emitIndexLLVM(a, aLen, b, bLen);
}

// --- W1 dispatch wrappers (P3) ---

llvm::Value* IRGen::emitAssignVarying(llvm::Value* dst, llvm::Value* cap, llvm::Value* src,
                                      llvm::Value* srcLen) {
  if (useRuntimeCall("assign_varying")) {
    return b_.CreateCall(runtimeFn("pli_assign_varying"), {dst, cap, src, srcLen}, "varying.n");
  }
  return emitAssignVaryingLLVM(dst, cap, src, srcLen);
}

void IRGen::emitHigh(llvm::Value* dst, llvm::Value* n) {
  if (useRuntimeCall("high")) {
    b_.CreateCall(runtimeFn("pli_high"), {dst, n});
    return;
  }
  emitHighLLVM(dst, n);
}

void IRGen::emitLow(llvm::Value* dst, llvm::Value* n) {
  if (useRuntimeCall("low")) {
    b_.CreateCall(runtimeFn("pli_low"), {dst, n});
    return;
  }
  emitLowLLVM(dst, n);
}

void IRGen::emitUppercase(llvm::Value* dst, llvm::Value* dstcap, llvm::Value* src,
                          llvm::Value* srcLen) {
  if (useRuntimeCall("uppercase")) {
    b_.CreateCall(runtimeFn("pli_uppercase"), {dst, dstcap, src, srcLen});
    return;
  }
  emitUppercaseLLVM(dst, dstcap, src, srcLen);
}

void IRGen::emitLowercase(llvm::Value* dst, llvm::Value* dstcap, llvm::Value* src,
                          llvm::Value* srcLen) {
  if (useRuntimeCall("lowercase")) {
    b_.CreateCall(runtimeFn("pli_lowercase"), {dst, dstcap, src, srcLen});
    return;
  }
  emitLowercaseLLVM(dst, dstcap, src, srcLen);
}

void IRGen::emitReverse(llvm::Value* dst, llvm::Value* dstcap, llvm::Value* src,
                        llvm::Value* srcLen) {
  if (useRuntimeCall("reverse")) {
    b_.CreateCall(runtimeFn("pli_reverse"), {dst, dstcap, src, srcLen});
    return;
  }
  emitReverseLLVM(dst, dstcap, src, srcLen);
}

void IRGen::emitCenter(llvm::Value* dst, llvm::Value* dstcap, llvm::Value* src, llvm::Value* srcLen,
                       llvm::Value* w) {
  if (useRuntimeCall("center")) {
    b_.CreateCall(runtimeFn("pli_center"), {dst, dstcap, src, srcLen, w});
    return;
  }
  emitCenterLLVM(dst, dstcap, src, srcLen, w);
}

llvm::Value* IRGen::emitCmpChar(llvm::Value* a, llvm::Value* aLen, llvm::Value* b,
                                llvm::Value* bLen) {
  if (useRuntimeCall("cmp_char")) {
    return b_.CreateCall(runtimeFn("pli_cmp_char"), {a, aLen, b, bLen}, "scmp");
  }
  return emitCmpCharLLVM(a, aLen, b, bLen);
}

// --- W2 dispatch wrappers (search/scan operations) ---

llvm::Value* IRGen::emitVerify(llvm::Value* s, llvm::Value* sLen, llvm::Value* t,
                               llvm::Value* tLen) {
  if (useRuntimeCall("verify")) {
    return b_.CreateCall(runtimeFn("pli_verify"), {s, sLen, t, tLen}, "verify");
  }
  return emitVerifyLLVM(s, sLen, t, tLen);
}

llvm::Value* IRGen::emitVerifyFrom(llvm::Value* s, llvm::Value* sLen, llvm::Value* t,
                                   llvm::Value* tLen, llvm::Value* start) {
  if (useRuntimeCall("verify_from")) {
    return b_.CreateCall(runtimeFn("pli_verify_from"), {s, sLen, t, tLen, start}, "vrf");
  }
  return emitVerifyFromLLVM(s, sLen, t, tLen, start);
}

llvm::Value* IRGen::emitSearch(llvm::Value* s, llvm::Value* sLen, llvm::Value* t, llvm::Value* tLen,
                               llvm::Value* start) {
  if (useRuntimeCall("search")) {
    return b_.CreateCall(runtimeFn("pli_search"), {s, sLen, t, tLen, start}, "search");
  }
  return emitSearchLLVM(s, sLen, t, tLen, start);
}

llvm::Value* IRGen::emitTally(llvm::Value* x, llvm::Value* xLen, llvm::Value* y,
                              llvm::Value* yLen) {
  if (useRuntimeCall("tally")) {
    return b_.CreateCall(runtimeFn("pli_tally"), {x, xLen, y, yLen}, "tally");
  }
  return emitTallyLLVM(x, xLen, y, yLen);
}

// --- W3 dispatch wrappers (clipping, padding, capacity) ---

void IRGen::emitSubstr(llvm::Value* dst, llvm::Value* dstcap, llvm::Value* src, llvm::Value* srcLen,
                       llvm::Value* start, llvm::Value* len) {
  if (useRuntimeCall("substr")) {
    b_.CreateCall(runtimeFn("pli_substr"), {dst, dstcap, src, srcLen, start, len});
    return;
  }
  emitSubstrLLVM(dst, dstcap, src, srcLen, start, len);
}

void IRGen::emitSubstrAssign(llvm::Value* dst, llvm::Value* dstcap, llvm::Value* start,
                             llvm::Value* len, llvm::Value* src, llvm::Value* srcLen) {
  if (useRuntimeCall("substr_assign")) {
    b_.CreateCall(runtimeFn("pli_substr_assign"), {dst, dstcap, start, len, src, srcLen});
    return;
  }
  emitSubstrAssignLLVM(dst, dstcap, start, len, src, srcLen);
}

void IRGen::emitSubstrAssignVarying(llvm::Value* dst, llvm::Value* dstcap, llvm::Value* lenptr,
                                    llvm::Value* start, llvm::Value* len, llvm::Value* src,
                                    llvm::Value* srcLen) {
  if (useRuntimeCall("substr_assign_varying")) {
    b_.CreateCall(runtimeFn("pli_substr_assign_varying"),
                  {dst, dstcap, lenptr, start, len, src, srcLen});
    return;
  }
  emitSubstrAssignVaryingLLVM(dst, dstcap, lenptr, start, len, src, srcLen);
}

void IRGen::emitRepeat(llvm::Value* dst, llvm::Value* dstcap, llvm::Value* src, llvm::Value* srcLen,
                       llvm::Value* n) {
  if (useRuntimeCall("repeat")) {
    b_.CreateCall(runtimeFn("pli_repeat"), {dst, dstcap, src, srcLen, n});
    return;
  }
  emitRepeatLLVM(dst, dstcap, src, srcLen, n);
}

void IRGen::emitTranslate(llvm::Value* dst, llvm::Value* dstcap, llvm::Value* s, llvm::Value* sLen,
                          llvm::Value* out, llvm::Value* outLen, llvm::Value* in,
                          llvm::Value* inLen) {
  if (useRuntimeCall("translate")) {
    b_.CreateCall(runtimeFn("pli_translate"), {dst, dstcap, s, sLen, out, outLen, in, inLen});
    return;
  }
  emitTranslateLLVM(dst, dstcap, s, sLen, out, outLen, in, inLen);
}

void IRGen::emitTrim(llvm::Value* dst, llvm::Value* dstcap, llvm::Value* s, llvm::Value* sLen,
                     llvm::Value* pad, llvm::Value* padLen) {
  if (useRuntimeCall("trim")) {
    b_.CreateCall(runtimeFn("pli_trim"), {dst, dstcap, s, sLen, pad, padLen});
    return;
  }
  emitTrimLLVM(dst, dstcap, s, sLen, pad, padLen);
}

// Direct LLVM lowering for INDEX (P1+). Mirrors pli_index in rt_string.c:
//   blen <= 0 → 1; blen > alen → 0; else scan i for first run where
//   a[i..i+blen-1] == b[0..blen-1].
llvm::Value* IRGen::emitIndexLLVM(llvm::Value* a, llvm::Value* aLen, llvm::Value* b,
                                  llvm::Value* bLen) {
  llvm::Function* F = b_.GetInsertBlock()->getParent();
  llvm::Value* zero = llvm::ConstantInt::get(b_.getInt64Ty(), 0);
  llvm::Value* one64 = llvm::ConstantInt::get(b_.getInt64Ty(), 1);
  llvm::Value* aLen64 = b_.CreateZExtOrTrunc(aLen, b_.getInt64Ty(), "idx.alen");
  llvm::Value* bLen64 = b_.CreateZExtOrTrunc(bLen, b_.getInt64Ty(), "idx.blen");

  llvm::BasicBlock* checkBB = llvm::BasicBlock::Create(ctx_, "idx.check", F);
  llvm::BasicBlock* ret1BB = llvm::BasicBlock::Create(ctx_, "idx.ret1", F);
  llvm::BasicBlock* ret0BB = llvm::BasicBlock::Create(ctx_, "idx.ret0", F);
  llvm::BasicBlock* outerBB = llvm::BasicBlock::Create(ctx_, "idx.outer", F);
  llvm::BasicBlock* innerBB = llvm::BasicBlock::Create(ctx_, "idx.inner", F);
  llvm::BasicBlock* checkByteBB = llvm::BasicBlock::Create(ctx_, "idx.check_byte", F);
  llvm::BasicBlock* innerLatchBB = llvm::BasicBlock::Create(ctx_, "idx.inner.latch", F);
  llvm::BasicBlock* outerLatchBB = llvm::BasicBlock::Create(ctx_, "idx.outer.latch", F);
  llvm::BasicBlock* matchBB = llvm::BasicBlock::Create(ctx_, "idx.match", F);
  llvm::BasicBlock* outerEndBB = llvm::BasicBlock::Create(ctx_, "idx.outer.end", F);
  llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(ctx_, "idx.exit", F);

  // Entry: blen <= 0 → 1; else check blen > alen.
  llvm::Value* blenLe0 = b_.CreateICmpULE(bLen64, zero, "idx.blenle0");
  b_.CreateCondBr(blenLe0, ret1BB, checkBB);

  // ret1: empty needle → result = 1.
  startBlock(ret1BB);
  b_.CreateBr(exitBB);

  // check: blen > alen → result = 0; else enter outer loop.
  startBlock(checkBB);
  llvm::Value* blenGtAlen = b_.CreateICmpUGT(bLen64, aLen64, "idx.blen_gt_alen");
  b_.CreateCondBr(blenGtAlen, ret0BB, outerBB);

  // ret0: no possible match → result = 0.
  startBlock(ret0BB);
  b_.CreateBr(exitBB);

  // outer.header: i = phi, exit when i + blen > alen.
  startBlock(outerBB);
  llvm::PHINode* iPhi = b_.CreatePHI(b_.getInt64Ty(), 2, "idx.i");
  iPhi->addIncoming(zero, checkBB);
  llvm::Value* iPlusBlen = b_.CreateAdd(iPhi, bLen64, "idx.i_plus_blen");
  llvm::Value* outerExit = b_.CreateICmpUGT(iPlusBlen, aLen64, "idx.outer_exit");
  b_.CreateCondBr(outerExit, outerEndBB, innerBB);

  // inner.header: j = phi, full match when j >= blen.
  startBlock(innerBB);
  llvm::PHINode* jPhi = b_.CreatePHI(b_.getInt64Ty(), 2, "idx.j");
  jPhi->addIncoming(zero, outerBB);
  llvm::Value* jgeBl = b_.CreateICmpUGE(jPhi, bLen64, "idx.j_ge_blen");
  b_.CreateCondBr(jgeBl, matchBB, checkByteBB);

  // check_byte: compare a[i+j] vs b[j].
  startBlock(checkByteBB);
  llvm::Value* ij = b_.CreateAdd(iPhi, jPhi, "idx.ij");
  llvm::Value* aiPtr = b_.CreateGEP(b_.getInt8Ty(), a, {ij}, "idx.ai_ptr");
  llvm::Value* bjPtr = b_.CreateGEP(b_.getInt8Ty(), b, {jPhi}, "idx.bj_ptr");
  llvm::Value* ai = b_.CreateLoad(b_.getInt8Ty(), aiPtr, "idx.ai");
  llvm::Value* bj = b_.CreateLoad(b_.getInt8Ty(), bjPtr, "idx.bj");
  llvm::Value* mismatch = b_.CreateICmpNE(ai, bj, "idx.mismatch");
  b_.CreateCondBr(mismatch, outerLatchBB, innerLatchBB);

  // inner.latch: j = j + 1, back to inner.header.
  startBlock(innerLatchBB);
  llvm::Value* jNext = b_.CreateAdd(jPhi, one64, "idx.j_next");
  jPhi->addIncoming(jNext, innerLatchBB);
  b_.CreateBr(innerBB);

  // outer.latch: i = i + 1, back to outer.header.
  startBlock(outerLatchBB);
  llvm::Value* iNext = b_.CreateAdd(iPhi, one64, "idx.i_next");
  iPhi->addIncoming(iNext, outerLatchBB);
  b_.CreateBr(outerBB);

  // match: result = i + 1.
  startBlock(matchBB);
  llvm::Value* matchResult = b_.CreateAdd(iPhi, one64, "idx.result");
  b_.CreateBr(exitBB);

  // outer.end: no match found → result = 0.
  startBlock(outerEndBB);
  b_.CreateBr(exitBB);

  // exit: merge all result values.
  startBlock(exitBB);
  llvm::PHINode* resultPhi = b_.CreatePHI(b_.getInt64Ty(), 4, "idx.out");
  resultPhi->addIncoming(one64, ret1BB);
  resultPhi->addIncoming(zero, ret0BB);
  resultPhi->addIncoming(matchResult, matchBB);
  resultPhi->addIncoming(zero, outerEndBB);

  return resultPhi;
}

// Direct LLVM lowering for assign_char (P1+): copies min(dstLen, srcLen) bytes
// via memmove (overlap-safe per constraint 6), then blank-fills the tail.
// The shape follows the C runtime (rt_string.c pli_assign_char).
void IRGen::emitAssignCharLLVM(llvm::Value* dst, llvm::Value* dstLen, llvm::Value* src,
                               llvm::Value* srcLen) {
  llvm::Value* zero = llvm::ConstantInt::get(b_.getInt64Ty(), 0);
  llvm::Value* srcLen64 = b_.CreateZExtOrTrunc(srcLen, b_.getInt64Ty(), "ac.sl");
  llvm::Value* dstLen64 = b_.CreateZExtOrTrunc(dstLen, b_.getInt64Ty(), "ac.dl");
  llvm::Value* n =
      b_.CreateSelect(b_.CreateICmpULT(srcLen64, dstLen64), srcLen64, dstLen64, "ac.n");
  b_.CreateMemMove(dst, llvm::MaybeAlign(), src, llvm::MaybeAlign(), n, false);
  // Blank-fill the tail past the copied bytes (dstLen > n), starting at dst+n.
  // When n == 0 the whole buffer is filled; this matches pli_assign_char's
  // `if (dstlen > n) memset(dst+n, ' ', dstlen-n)`.
  llvm::Value* padLen = b_.CreateSelect(b_.CreateICmpUGT(dstLen64, n),
                                        b_.CreateSub(dstLen64, n, "ac.pad"), zero, "ac.padlen");
  llvm::Value* padPtr = b_.CreateGEP(b_.getInt8Ty(), dst, {n}, "ac.padptr");
  b_.CreateMemSet(padPtr, b_.getInt8(' '), padLen, llvm::MaybeAlign(), false);
}

// --- W1 direct LLVM lowerings (P3) ---
// Mirrors the C bodies in runtime/rt_string.c. Each preserves the exact
// semantics: overlap safety (memmove), zero-length guards, and blank padding.

// assign_varying: memmove(min(cap,srclen)) + blank-pad tail + return n.
llvm::Value* IRGen::emitAssignVaryingLLVM(llvm::Value* dst, llvm::Value* cap, llvm::Value* src,
                                          llvm::Value* srcLen) {
  llvm::Value* zero = llvm::ConstantInt::get(b_.getInt64Ty(), 0);
  llvm::Value* cap64 = b_.CreateZExtOrTrunc(cap, b_.getInt64Ty(), "av.cap");
  llvm::Value* srcLen64 = b_.CreateZExtOrTrunc(srcLen, b_.getInt64Ty(), "av.sl");
  llvm::Value* n = b_.CreateSelect(b_.CreateICmpULT(srcLen64, cap64), srcLen64, cap64, "av.n");
  b_.CreateMemMove(dst, llvm::MaybeAlign(), src, llvm::MaybeAlign(), n, false);
  llvm::Value* padLen = b_.CreateSelect(b_.CreateICmpUGT(cap64, n),
                                        b_.CreateSub(cap64, n, "av.pad"), zero, "av.padlen");
  llvm::Value* padPtr = b_.CreateGEP(b_.getInt8Ty(), dst, {n}, "av.padptr");
  b_.CreateMemSet(padPtr, b_.getInt8(' '), padLen, llvm::MaybeAlign(), false);
  return n;
}

// high(n): fill n bytes with 0xFF.
void IRGen::emitHighLLVM(llvm::Value* dst, llvm::Value* n) {
  b_.CreateMemSet(dst, b_.getInt8((unsigned char)0xFF), n, llvm::MaybeAlign(), false);
}

// low(n): fill n bytes with 0x00.
void IRGen::emitLowLLVM(llvm::Value* dst, llvm::Value* n) {
  b_.CreateMemSet(dst, b_.getInt8((unsigned char)0x00), n, llvm::MaybeAlign(), false);
}

// uppercase(dstcap, s, slen): byte-fold a-z → A-Z, blank-pad tail.
void IRGen::emitUppercaseLLVM(llvm::Value* dst, llvm::Value* dstcap, llvm::Value* src,
                              llvm::Value* srcLen) {
  llvm::Function* F = b_.GetInsertBlock()->getParent();
  llvm::Value* zero = llvm::ConstantInt::get(b_.getInt64Ty(), 0);
  llvm::Value* one = llvm::ConstantInt::get(b_.getInt64Ty(), 1);
  llvm::Value* dstcap64 = b_.CreateZExtOrTrunc(dstcap, b_.getInt64Ty(), "uc.cap");
  llvm::Value* srcLen64 = b_.CreateZExtOrTrunc(srcLen, b_.getInt64Ty(), "uc.sl");
  llvm::Value* limit =
      b_.CreateSelect(b_.CreateICmpULT(srcLen64, dstcap64), srcLen64, dstcap64, "uc.limit");
  llvm::Value* lcA = llvm::ConstantInt::get(b_.getInt8Ty(), (uint64_t)'a');
  llvm::Value* lcZ = llvm::ConstantInt::get(b_.getInt8Ty(), (uint64_t)'z');
  llvm::Value* caseDiff = llvm::ConstantInt::get(b_.getInt8Ty(), (uint64_t)('a' - 'A'));

  llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(ctx_, "uc.loop", F);
  llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(ctx_, "uc.body", F);
  llvm::BasicBlock* padBB = llvm::BasicBlock::Create(ctx_, "uc.pad", F);
  b_.CreateBr(loopBB);
  // Track predecessor for PHI: startBlock inserts br from current block.
  llvm::BasicBlock* pred = b_.GetInsertBlock();
  startBlock(loopBB);
  llvm::PHINode* i = b_.CreatePHI(b_.getInt64Ty(), 2, "uc.i");
  i->addIncoming(zero, pred);
  llvm::Value* done = b_.CreateICmpUGE(i, limit, "uc.done");
  b_.CreateCondBr(done, padBB, bodyBB);

  startBlock(bodyBB);
  llvm::Value* srcPtr = b_.CreateGEP(b_.getInt8Ty(), src, {i}, "uc.sptr");
  llvm::Value* c = b_.CreateLoad(b_.getInt8Ty(), srcPtr, "uc.c");
  llvm::Value* isLower =
      b_.CreateAnd(b_.CreateICmpULE(lcA, c), b_.CreateICmpULE(c, lcZ), "uc.islo");
  llvm::Value* fold = b_.CreateSub(c, caseDiff, "uc.fold");
  llvm::Value* out = b_.CreateSelect(isLower, fold, c, "uc.out");
  llvm::Value* dstPtr = b_.CreateGEP(b_.getInt8Ty(), dst, {i}, "uc.dptr");
  b_.CreateStore(out, dstPtr);
  llvm::Value* next = b_.CreateAdd(i, one, "uc.next");
  i->addIncoming(next, bodyBB);
  b_.CreateBr(loopBB);

  // Blank-pad dst[limit..dstcap) via memset.
  startBlock(padBB);
  llvm::Value* padLen = b_.CreateSelect(b_.CreateICmpUGT(dstcap64, limit),
                                        b_.CreateSub(dstcap64, limit, "uc.pad"), zero, "uc.padlen");
  llvm::Value* padPtr = b_.CreateGEP(b_.getInt8Ty(), dst, {limit}, "uc.padptr");
  b_.CreateMemSet(padPtr, b_.getInt8(' '), padLen, llvm::MaybeAlign(), false);
}

// lowercase(dstcap, s, slen): byte-fold A-Z → a-z, blank-pad tail.
void IRGen::emitLowercaseLLVM(llvm::Value* dst, llvm::Value* dstcap, llvm::Value* src,
                              llvm::Value* srcLen) {
  llvm::Function* F = b_.GetInsertBlock()->getParent();
  llvm::Value* zero = llvm::ConstantInt::get(b_.getInt64Ty(), 0);
  llvm::Value* one = llvm::ConstantInt::get(b_.getInt64Ty(), 1);
  llvm::Value* dstcap64 = b_.CreateZExtOrTrunc(dstcap, b_.getInt64Ty(), "lc.cap");
  llvm::Value* srcLen64 = b_.CreateZExtOrTrunc(srcLen, b_.getInt64Ty(), "lc.sl");
  llvm::Value* limit =
      b_.CreateSelect(b_.CreateICmpULT(srcLen64, dstcap64), srcLen64, dstcap64, "lc.limit");
  llvm::Value* ucA = llvm::ConstantInt::get(b_.getInt8Ty(), (uint64_t)'A');
  llvm::Value* ucZ = llvm::ConstantInt::get(b_.getInt8Ty(), (uint64_t)'Z');
  llvm::Value* caseDiff = llvm::ConstantInt::get(b_.getInt8Ty(), (uint64_t)('a' - 'A'));

  llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(ctx_, "lc.loop", F);
  llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(ctx_, "lc.body", F);
  llvm::BasicBlock* padBB = llvm::BasicBlock::Create(ctx_, "lc.pad", F);
  b_.CreateBr(loopBB);
  llvm::BasicBlock* pred = b_.GetInsertBlock();
  startBlock(loopBB);
  llvm::PHINode* i = b_.CreatePHI(b_.getInt64Ty(), 2, "lc.i");
  i->addIncoming(zero, pred);
  llvm::Value* done = b_.CreateICmpUGE(i, limit, "lc.done");
  b_.CreateCondBr(done, padBB, bodyBB);

  startBlock(bodyBB);
  llvm::Value* srcPtr = b_.CreateGEP(b_.getInt8Ty(), src, {i}, "lc.sptr");
  llvm::Value* c = b_.CreateLoad(b_.getInt8Ty(), srcPtr, "lc.c");
  llvm::Value* isUpper =
      b_.CreateAnd(b_.CreateICmpULE(ucA, c), b_.CreateICmpULE(c, ucZ), "lc.isup");
  llvm::Value* fold = b_.CreateAdd(c, caseDiff, "lc.fold");
  llvm::Value* out = b_.CreateSelect(isUpper, fold, c, "lc.out");
  llvm::Value* dstPtr = b_.CreateGEP(b_.getInt8Ty(), dst, {i}, "lc.dptr");
  b_.CreateStore(out, dstPtr);
  llvm::Value* next = b_.CreateAdd(i, one, "lc.next");
  i->addIncoming(next, bodyBB);
  b_.CreateBr(loopBB);

  startBlock(padBB);
  llvm::Value* padLen = b_.CreateSelect(b_.CreateICmpUGT(dstcap64, limit),
                                        b_.CreateSub(dstcap64, limit, "lc.pad"), zero, "lc.padlen");
  llvm::Value* padPtr = b_.CreateGEP(b_.getInt8Ty(), dst, {limit}, "lc.padptr");
  b_.CreateMemSet(padPtr, b_.getInt8(' '), padLen, llvm::MaybeAlign(), false);
}

// reverse(dstcap, s, slen): reverse-copy, blank-pad tail.
void IRGen::emitReverseLLVM(llvm::Value* dst, llvm::Value* dstcap, llvm::Value* src,
                            llvm::Value* srcLen) {
  llvm::Function* F = b_.GetInsertBlock()->getParent();
  llvm::Value* zero = llvm::ConstantInt::get(b_.getInt64Ty(), 0);
  llvm::Value* one = llvm::ConstantInt::get(b_.getInt64Ty(), 1);
  llvm::Value* dstcap64 = b_.CreateZExtOrTrunc(dstcap, b_.getInt64Ty(), "rv.cap");
  llvm::Value* srcLen64 = b_.CreateZExtOrTrunc(srcLen, b_.getInt64Ty(), "rv.sl");
  llvm::Value* n =
      b_.CreateSelect(b_.CreateICmpULT(srcLen64, dstcap64), srcLen64, dstcap64, "rv.n");

  llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(ctx_, "rv.loop", F);
  llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(ctx_, "rv.body", F);
  llvm::BasicBlock* padBB = llvm::BasicBlock::Create(ctx_, "rv.pad", F);
  b_.CreateBr(loopBB);
  llvm::BasicBlock* pred = b_.GetInsertBlock();
  startBlock(loopBB);
  llvm::PHINode* i = b_.CreatePHI(b_.getInt64Ty(), 2, "rv.i");
  i->addIncoming(zero, pred);
  llvm::Value* done = b_.CreateICmpUGE(i, n, "rv.done");
  b_.CreateCondBr(done, padBB, bodyBB);

  startBlock(bodyBB);
  // dst[i] = s[slen - 1 - i]
  llvm::Value* srcIdx = b_.CreateSub(b_.CreateSub(srcLen64, one, "rv.off"), i, "rv.sidx");
  llvm::Value* srcPtr = b_.CreateGEP(b_.getInt8Ty(), src, {srcIdx}, "rv.sptr");
  llvm::Value* c = b_.CreateLoad(b_.getInt8Ty(), srcPtr, "rv.c");
  llvm::Value* dstPtr = b_.CreateGEP(b_.getInt8Ty(), dst, {i}, "rv.dptr");
  b_.CreateStore(c, dstPtr);
  llvm::Value* next = b_.CreateAdd(i, one, "rv.next");
  i->addIncoming(next, bodyBB);
  b_.CreateBr(loopBB);

  startBlock(padBB);
  llvm::Value* padLen = b_.CreateSelect(b_.CreateICmpUGT(dstcap64, n),
                                        b_.CreateSub(dstcap64, n, "rv.pad"), zero, "rv.padlen");
  llvm::Value* padPtr = b_.CreateGEP(b_.getInt8Ty(), dst, {n}, "rv.padptr");
  b_.CreateMemSet(padPtr, b_.getInt8(' '), padLen, llvm::MaybeAlign(), false);
}

// center(dstcap, s, slen, w): center in field of width w (clamped to dstcap),
// blank-pad tail. Mirrors pli_center.
void IRGen::emitCenterLLVM(llvm::Value* dst, llvm::Value* dstcap, llvm::Value* src,
                           llvm::Value* srcLen, llvm::Value* w) {
  llvm::Value* zero = llvm::ConstantInt::get(b_.getInt64Ty(), 0);
  llvm::Value* dstcap64 = b_.CreateZExtOrTrunc(dstcap, b_.getInt64Ty(), "ct.cap");
  llvm::Value* srcLen64 = b_.CreateZExtOrTrunc(srcLen, b_.getInt64Ty(), "ct.sl");
  llvm::Value* w64 = w;
  // w < 0 → 0; field = min(w, dstcap); take = min(slen, field); left = (field-take)/2.
  llvm::Value* wClamped =
      b_.CreateSelect(b_.CreateICmpSLT(w64, zero, "ct.wneg"), zero, w64, "ct.wc");
  llvm::Value* field =
      b_.CreateSelect(b_.CreateICmpULT(wClamped, dstcap64), wClamped, dstcap64, "ct.field");
  llvm::Value* take =
      b_.CreateSelect(b_.CreateICmpULT(srcLen64, field), srcLen64, field, "ct.take");
  llvm::Value* left = b_.CreateExactUDiv(b_.CreateSub(field, take, "ct.excess"),
                                         llvm::ConstantInt::get(b_.getInt64Ty(), 2), "ct.left");

  // Fill [0, left) with spaces via memset.
  b_.CreateMemSet(dst, b_.getInt8(' '), left, llvm::MaybeAlign(), false);
  // Copy src[0..take) to dst[left..left+take) via memmove.
  b_.CreateMemMove(b_.CreateGEP(b_.getInt8Ty(), dst, {left}, "ct.dstcopy"), llvm::MaybeAlign(), src,
                   llvm::MaybeAlign(), take, false);
  // Blank-pad [left+take, dstcap).
  llvm::Value* padStart = b_.CreateAdd(left, take, "ct.padstart");
  llvm::Value* padLen =
      b_.CreateSelect(b_.CreateICmpUGT(dstcap64, padStart),
                      b_.CreateSub(dstcap64, padStart, "ct.pad"), zero, "ct.padlen");
  b_.CreateMemSet(b_.CreateGEP(b_.getInt8Ty(), dst, {padStart}, "ct.padptr"), b_.getInt8(' '),
                  padLen, llvm::MaybeAlign(), false);
}

// cmp_char(a, alen, b, blen): return -1/0/1 with blank extension.
// Iterates min(alen, blen) bytes; shorter operand is blank-extended.
// cmp_char(a, alen, b, blen): return -1/0/1 with blank extension.
llvm::Value* IRGen::emitCmpCharLLVM(llvm::Value* a, llvm::Value* aLen, llvm::Value* b,
                                    llvm::Value* bLen) {
  llvm::Function* F = b_.GetInsertBlock()->getParent();
  llvm::Value* zero = llvm::ConstantInt::get(b_.getInt64Ty(), 0);
  llvm::Value* one = llvm::ConstantInt::get(b_.getInt64Ty(), 1);
  llvm::Value* negOne = llvm::ConstantInt::get(b_.getInt64Ty(), (uint64_t)-1);
  llvm::Value* aLen64 = b_.CreateZExtOrTrunc(aLen, b_.getInt64Ty(), "sc.alen");
  llvm::Value* bLen64 = b_.CreateZExtOrTrunc(bLen, b_.getInt64Ty(), "sc.blen");
  llvm::Value* n = b_.CreateSelect(b_.CreateICmpULT(aLen64, bLen64), bLen64, aLen64, "sc.n");
  llvm::Value* blank = llvm::ConstantInt::get(b_.getInt8Ty(), (uint64_t)' ');

  llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(ctx_, "sc.loop", F);
  llvm::BasicBlock* cmpBB = llvm::BasicBlock::Create(ctx_, "sc.cmp", F);
  llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(ctx_, "sc.done", F);
  llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(ctx_, "sc.exit", F);

  llvm::BasicBlock* pred = b_.GetInsertBlock();
  b_.CreateBr(loopBB);
  startBlock(loopBB);
  llvm::PHINode* i = b_.CreatePHI(b_.getInt64Ty(), 2, "sc.i");
  i->addIncoming(zero, pred);
  llvm::Value* exhausted = b_.CreateICmpUGE(i, n, "sc.exhausted");
  b_.CreateCondBr(exhausted, doneBB, cmpBB);

  startBlock(cmpBB);
  llvm::Value* aiPtr = b_.CreateGEP(b_.getInt8Ty(), a, {i}, "sc.aptr");
  llvm::Value* biPtr = b_.CreateGEP(b_.getInt8Ty(), b, {i}, "sc.bptr");
  llvm::Value* ai = b_.CreateLoad(b_.getInt8Ty(), aiPtr, "sc.ac");
  llvm::Value* bi = b_.CreateLoad(b_.getInt8Ty(), biPtr, "sc.bc");
  // Blank-extend: pick from actual byte if i < len, else ' '.
  llvm::Value* aVal = b_.CreateSelect(b_.CreateICmpULT(i, aLen64), ai, blank, "sc.av");
  llvm::Value* bVal = b_.CreateSelect(b_.CreateICmpULT(i, bLen64), bi, blank, "sc.bv");
  llvm::Value* aExt = b_.CreateZExt(aVal, b_.getInt64Ty(), "sc.av64");
  llvm::Value* bExt = b_.CreateZExt(bVal, b_.getInt64Ty(), "sc.bv64");
  llvm::Value* diff = b_.CreateSub(aExt, bExt, "sc.diff");
  llvm::Value* lt = b_.CreateICmpSLT(diff, zero, "sc.lt");
  llvm::Value* gt = b_.CreateICmpSGT(diff, zero, "sc.gt");
  llvm::Value* pos = b_.CreateSelect(gt, one, zero, "sc.pos");
  llvm::Value* result = b_.CreateSelect(lt, negOne, pos, "sc.result");
  // On mismatch, exit with result; otherwise increment i and continue.
  llvm::Value* ne = b_.CreateICmpNE(aVal, bVal, "sc.ne");
  i->addIncoming(b_.CreateAdd(i, one, "sc.next"), cmpBB);
  b_.CreateCondBr(ne, exitBB, loopBB);

  startBlock(doneBB);
  b_.CreateBr(exitBB);

  // exitBB: merge mismatch result from cmpBB and zero from doneBB.
  startBlock(exitBB);
  llvm::PHINode* resultPhi = b_.CreatePHI(b_.getInt64Ty(), 2, "sc.result");
  resultPhi->addIncoming(result, cmpBB);
  resultPhi->addIncoming(zero, doneBB);
  // Truncate to i32 to match pli_cmp_char's return type (int).
  return b_.CreateTrunc(resultPhi, b_.getInt32Ty(), "sc.res");
}

// --- W2 direct LLVM lowerings (P3) ---
// Mirrors the C bodies in runtime/rt_string.c. Each returns i64 (truncated
// to i32 by the call site), preserving the runtime's 1-based semantics.

// verify(s, t): 1-based position of the first char of s that does NOT
// appear in t, or 0 if every char of s is in t. Mirrors pli_verify.
llvm::Value* IRGen::emitVerifyLLVM(llvm::Value* s, llvm::Value* sLen, llvm::Value* t,
                                   llvm::Value* tLen) {
  llvm::Function* F = b_.GetInsertBlock()->getParent();
  llvm::Value* zero = llvm::ConstantInt::get(b_.getInt64Ty(), 0);
  llvm::Value* one64 = llvm::ConstantInt::get(b_.getInt64Ty(), 1);
  llvm::Value* sLen64 = b_.CreateZExtOrTrunc(sLen, b_.getInt64Ty(), "vrf.slen");
  llvm::Value* tLen64 = b_.CreateZExtOrTrunc(tLen, b_.getInt64Ty(), "vrf.tlen");

  llvm::BasicBlock* outerBB = llvm::BasicBlock::Create(ctx_, "vrf.outer", F);
  llvm::BasicBlock* innerBB = llvm::BasicBlock::Create(ctx_, "vrf.inner", F);
  llvm::BasicBlock* checkByteBB = llvm::BasicBlock::Create(ctx_, "vrf.check_byte", F);
  llvm::BasicBlock* innerLatchBB = llvm::BasicBlock::Create(ctx_, "vrf.inner.latch", F);
  llvm::BasicBlock* foundBB = llvm::BasicBlock::Create(ctx_, "vrf.found", F);
  llvm::BasicBlock* nomatchBB = llvm::BasicBlock::Create(ctx_, "vrf.nomatch", F);
  llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(ctx_, "vrf.done", F);
  llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(ctx_, "vrf.exit", F);

  // Entry: slen <= 0 → done (return 0).
  llvm::BasicBlock* entryBB = b_.GetInsertBlock();
  llvm::Value* sEmpty = b_.CreateICmpULE(sLen64, zero, "vrf.s_empty");
  b_.CreateCondBr(sEmpty, doneBB, outerBB);

  // outer: i = phi; exit when i >= slen.
  startBlock(outerBB);
  llvm::PHINode* iPhi = b_.CreatePHI(b_.getInt64Ty(), 2, "vrf.i");
  iPhi->addIncoming(zero, entryBB);
  llvm::Value* iGeSlen = b_.CreateICmpUGE(iPhi, sLen64, "vrf.i_ge_slen");
  b_.CreateCondBr(iGeSlen, doneBB, innerBB);

  // inner: j = phi; if j >= tlen → char not in t → nomatch (return i+1).
  startBlock(innerBB);
  llvm::PHINode* jPhi = b_.CreatePHI(b_.getInt64Ty(), 2, "vrf.j");
  jPhi->addIncoming(zero, outerBB);
  llvm::Value* jGeTlen = b_.CreateICmpUGE(jPhi, tLen64, "vrf.j_ge_tlen");
  b_.CreateCondBr(jGeTlen, nomatchBB, checkByteBB);

  // check_byte: compare s[i] vs t[j]; if equal → found (advance i); else j++.
  startBlock(checkByteBB);
  llvm::Value* siPtr = b_.CreateGEP(b_.getInt8Ty(), s, {iPhi}, "vrf.si_ptr");
  llvm::Value* tjPtr = b_.CreateGEP(b_.getInt8Ty(), t, {jPhi}, "vrf.tj_ptr");
  llvm::Value* si = b_.CreateLoad(b_.getInt8Ty(), siPtr, "vrf.si");
  llvm::Value* tj = b_.CreateLoad(b_.getInt8Ty(), tjPtr, "vrf.tj");
  llvm::Value* eq = b_.CreateICmpEQ(si, tj, "vrf.eq");
  b_.CreateCondBr(eq, foundBB, innerLatchBB);

  // inner.latch: j = j + 1, back to inner.header.
  startBlock(innerLatchBB);
  jPhi->addIncoming(b_.CreateAdd(jPhi, one64, "vrf.j_next"), innerLatchBB);
  b_.CreateBr(innerBB);

  // found: s[i] is in t → advance i and retry.
  startBlock(foundBB);
  iPhi->addIncoming(b_.CreateAdd(iPhi, one64, "vrf.i_next"), foundBB);
  b_.CreateBr(outerBB);

  // nomatch: s[i] is NOT in t → return i + 1.
  startBlock(nomatchBB);
  llvm::Value* matchResult = b_.CreateAdd(iPhi, one64, "vrf.result");
  b_.CreateBr(exitBB);

  // done: no mismatched char found → return 0.
  startBlock(doneBB);
  b_.CreateBr(exitBB);

  // exit: merge results.
  startBlock(exitBB);
  llvm::PHINode* resultPhi = b_.CreatePHI(b_.getInt64Ty(), 2, "vrf.out");
  resultPhi->addIncoming(matchResult, nomatchBB);
  resultPhi->addIncoming(zero, doneBB);
  return resultPhi;
}

// verify_from(s, t, start): like verify but scans from position `start`
// (1-based, < 1 clamped to 1). Mirrors pli_verify_from.
llvm::Value* IRGen::emitVerifyFromLLVM(llvm::Value* s, llvm::Value* sLen, llvm::Value* t,
                                       llvm::Value* tLen, llvm::Value* start) {
  llvm::Function* F = b_.GetInsertBlock()->getParent();
  llvm::Value* zero = llvm::ConstantInt::get(b_.getInt64Ty(), 0);
  llvm::Value* one64 = llvm::ConstantInt::get(b_.getInt64Ty(), 1);
  llvm::Value* sLen64 = b_.CreateZExtOrTrunc(sLen, b_.getInt64Ty(), "vrf.slen");
  llvm::Value* tLen64 = b_.CreateZExtOrTrunc(tLen, b_.getInt64Ty(), "vrf.tlen");
  llvm::Value* start64 = b_.CreateZExtOrTrunc(start, b_.getInt64Ty(), "vrf.start");
  // start < 1 → 1; i starts at start-1 (0-based).
  llvm::Value* clamped =
      b_.CreateSelect(b_.CreateICmpULT(start64, one64), one64, start64, "vrf.clamped");
  llvm::Value* iStart = b_.CreateSub(clamped, one64, "vrf.istart");

  llvm::BasicBlock* outerBB = llvm::BasicBlock::Create(ctx_, "vrf.outer", F);
  llvm::BasicBlock* innerBB = llvm::BasicBlock::Create(ctx_, "vrf.inner", F);
  llvm::BasicBlock* checkByteBB = llvm::BasicBlock::Create(ctx_, "vrf.check_byte", F);
  llvm::BasicBlock* innerLatchBB = llvm::BasicBlock::Create(ctx_, "vrf.inner.latch", F);
  llvm::BasicBlock* foundBB = llvm::BasicBlock::Create(ctx_, "vrf.found", F);
  llvm::BasicBlock* nomatchBB = llvm::BasicBlock::Create(ctx_, "vrf.nomatch", F);
  llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(ctx_, "vrf.done", F);
  llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(ctx_, "vrf.exit", F);

  // Entry: slen <= 0 → done (return 0).
  llvm::BasicBlock* entryBB = b_.GetInsertBlock();
  llvm::Value* sEmpty = b_.CreateICmpULE(sLen64, zero, "vrf.s_empty");
  b_.CreateCondBr(sEmpty, doneBB, outerBB);

  startBlock(outerBB);
  llvm::PHINode* iPhi = b_.CreatePHI(b_.getInt64Ty(), 2, "vrf.i");
  iPhi->addIncoming(iStart, entryBB);
  llvm::Value* iGeSlen = b_.CreateICmpUGE(iPhi, sLen64, "vrf.i_ge_slen");
  b_.CreateCondBr(iGeSlen, doneBB, innerBB);

  startBlock(innerBB);
  llvm::PHINode* jPhi = b_.CreatePHI(b_.getInt64Ty(), 2, "vrf.j");
  jPhi->addIncoming(zero, outerBB);
  llvm::Value* jGeTlen = b_.CreateICmpUGE(jPhi, tLen64, "vrf.j_ge_tlen");
  b_.CreateCondBr(jGeTlen, nomatchBB, checkByteBB);

  startBlock(checkByteBB);
  llvm::Value* siPtr = b_.CreateGEP(b_.getInt8Ty(), s, {iPhi}, "vrf.si_ptr");
  llvm::Value* tjPtr = b_.CreateGEP(b_.getInt8Ty(), t, {jPhi}, "vrf.tj_ptr");
  llvm::Value* si = b_.CreateLoad(b_.getInt8Ty(), siPtr, "vrf.si");
  llvm::Value* tj = b_.CreateLoad(b_.getInt8Ty(), tjPtr, "vrf.tj");
  llvm::Value* eq = b_.CreateICmpEQ(si, tj, "vrf.eq");
  b_.CreateCondBr(eq, foundBB, innerLatchBB);

  startBlock(innerLatchBB);
  jPhi->addIncoming(b_.CreateAdd(jPhi, one64, "vrf.j_next"), innerLatchBB);
  b_.CreateBr(innerBB);

  startBlock(foundBB);
  iPhi->addIncoming(b_.CreateAdd(iPhi, one64, "vrf.i_next"), foundBB);
  b_.CreateBr(outerBB);

  startBlock(nomatchBB);
  llvm::Value* matchResult = b_.CreateAdd(iPhi, one64, "vrf.result");
  b_.CreateBr(exitBB);

  startBlock(doneBB);
  b_.CreateBr(exitBB);

  startBlock(exitBB);
  llvm::PHINode* resultPhi = b_.CreatePHI(b_.getInt64Ty(), 2, "vrf.out");
  resultPhi->addIncoming(matchResult, nomatchBB);
  resultPhi->addIncoming(zero, doneBB);
  return resultPhi;
}

// search(s, t, start): 1-based position of the first char of s (from `start`,
// clamped to 1) that IS in t, or 0. Mirrors pli_search.
llvm::Value* IRGen::emitSearchLLVM(llvm::Value* s, llvm::Value* sLen, llvm::Value* t,
                                   llvm::Value* tLen, llvm::Value* start) {
  llvm::Function* F = b_.GetInsertBlock()->getParent();
  llvm::Value* zero = llvm::ConstantInt::get(b_.getInt64Ty(), 0);
  llvm::Value* one64 = llvm::ConstantInt::get(b_.getInt64Ty(), 1);
  llvm::Value* sLen64 = b_.CreateZExtOrTrunc(sLen, b_.getInt64Ty(), "srch.slen");
  llvm::Value* tLen64 = b_.CreateZExtOrTrunc(tLen, b_.getInt64Ty(), "srch.tlen");
  llvm::Value* start64 = b_.CreateZExtOrTrunc(start, b_.getInt64Ty(), "srch.start");
  llvm::Value* clamped =
      b_.CreateSelect(b_.CreateICmpULT(start64, one64), one64, start64, "srch.clamped");
  llvm::Value* iStart = b_.CreateSub(clamped, one64, "srch.istart");

  llvm::BasicBlock* outerBB = llvm::BasicBlock::Create(ctx_, "srch.outer", F);
  llvm::BasicBlock* innerBB = llvm::BasicBlock::Create(ctx_, "srch.inner", F);
  llvm::BasicBlock* checkByteBB = llvm::BasicBlock::Create(ctx_, "srch.check_byte", F);
  llvm::BasicBlock* innerLatchBB = llvm::BasicBlock::Create(ctx_, "srch.inner.latch", F);
  llvm::BasicBlock* outerLatchBB = llvm::BasicBlock::Create(ctx_, "srch.outer.latch", F);
  llvm::BasicBlock* matchedBB = llvm::BasicBlock::Create(ctx_, "srch.matched", F);
  llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(ctx_, "srch.done", F);
  llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(ctx_, "srch.exit", F);

  llvm::BasicBlock* entryBB = b_.GetInsertBlock();
  llvm::Value* sEmpty = b_.CreateICmpULE(sLen64, zero, "srch.s_empty");
  b_.CreateCondBr(sEmpty, doneBB, outerBB);

  startBlock(outerBB);
  llvm::PHINode* iPhi = b_.CreatePHI(b_.getInt64Ty(), 2, "srch.i");
  iPhi->addIncoming(iStart, entryBB);
  llvm::Value* iGeSlen = b_.CreateICmpUGE(iPhi, sLen64, "srch.i_ge_slen");
  b_.CreateCondBr(iGeSlen, doneBB, innerBB);

  startBlock(innerBB);
  llvm::PHINode* jPhi = b_.CreatePHI(b_.getInt64Ty(), 2, "srch.j");
  jPhi->addIncoming(zero, outerBB);
  llvm::Value* jGeTlen = b_.CreateICmpUGE(jPhi, tLen64, "srch.j_ge_tlen");
  b_.CreateCondBr(jGeTlen, outerLatchBB, checkByteBB);

  startBlock(checkByteBB);
  llvm::Value* siPtr = b_.CreateGEP(b_.getInt8Ty(), s, {iPhi}, "srch.si_ptr");
  llvm::Value* tjPtr = b_.CreateGEP(b_.getInt8Ty(), t, {jPhi}, "srch.tj_ptr");
  llvm::Value* si = b_.CreateLoad(b_.getInt8Ty(), siPtr, "srch.si");
  llvm::Value* tj = b_.CreateLoad(b_.getInt8Ty(), tjPtr, "srch.tj");
  llvm::Value* eq = b_.CreateICmpEQ(si, tj, "srch.eq");
  b_.CreateCondBr(eq, matchedBB, innerLatchBB);

  startBlock(innerLatchBB);
  jPhi->addIncoming(b_.CreateAdd(jPhi, one64, "srch.j_next"), innerLatchBB);
  b_.CreateBr(innerBB);

  // outer.latch: char not in t → advance i.
  startBlock(outerLatchBB);
  iPhi->addIncoming(b_.CreateAdd(iPhi, one64, "srch.i_next"), outerLatchBB);
  b_.CreateBr(outerBB);

  // matched: char found → return i + 1.
  startBlock(matchedBB);
  llvm::Value* matchResult = b_.CreateAdd(iPhi, one64, "srch.result");
  b_.CreateBr(exitBB);

  startBlock(doneBB);
  b_.CreateBr(exitBB);

  startBlock(exitBB);
  llvm::PHINode* resultPhi = b_.CreatePHI(b_.getInt64Ty(), 2, "srch.out");
  resultPhi->addIncoming(matchResult, matchedBB);
  resultPhi->addIncoming(zero, doneBB);
  return resultPhi;
}

// tally(x, y): count of non-overlapping occurrences of y in x.
// Mirrors pli_tally: ylen<=0 or xlen<ylen → 0; otherwise scan with stride
// ylen on match, stride 1 on mismatch.
llvm::Value* IRGen::emitTallyLLVM(llvm::Value* x, llvm::Value* xLen, llvm::Value* y,
                                  llvm::Value* yLen) {
  llvm::Function* F = b_.GetInsertBlock()->getParent();
  llvm::Value* zero = llvm::ConstantInt::get(b_.getInt64Ty(), 0);
  llvm::Value* one64 = llvm::ConstantInt::get(b_.getInt64Ty(), 1);
  llvm::Value* xLen64 = b_.CreateZExtOrTrunc(xLen, b_.getInt64Ty(), "tally.xlen");
  llvm::Value* yLen64 = b_.CreateZExtOrTrunc(yLen, b_.getInt64Ty(), "tally.ylen");

  llvm::BasicBlock* outerBB = llvm::BasicBlock::Create(ctx_, "tally.outer", F);
  llvm::BasicBlock* innerBB = llvm::BasicBlock::Create(ctx_, "tally.inner", F);
  llvm::BasicBlock* checkByteBB = llvm::BasicBlock::Create(ctx_, "tally.check_byte", F);
  llvm::BasicBlock* innerLatchBB = llvm::BasicBlock::Create(ctx_, "tally.inner.latch", F);
  llvm::BasicBlock* matchBB = llvm::BasicBlock::Create(ctx_, "tally.match", F);
  llvm::BasicBlock* nomatchBB = llvm::BasicBlock::Create(ctx_, "tally.nomatch", F);
  llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(ctx_, "tally.done", F);
  llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(ctx_, "tally.exit", F);

  // Entry: ylen <= 0 → 0; xlen < ylen → 0.
  llvm::BasicBlock* entryBB = b_.GetInsertBlock();
  llvm::Value* yEmpty = b_.CreateICmpULE(yLen64, zero, "tally.y_empty");
  llvm::Value* xTooSmall = b_.CreateICmpULT(xLen64, yLen64, "tally.x_too_small");
  llvm::Value* abort = b_.CreateOr(yEmpty, xTooSmall, "tally.abort");
  b_.CreateCondBr(abort, doneBB, outerBB);

  // outer: i = phi, n = phi; exit when i + ylen > xlen.
  startBlock(outerBB);
  llvm::PHINode* iPhi = b_.CreatePHI(b_.getInt64Ty(), 3, "tally.i");
  iPhi->addIncoming(zero, entryBB);
  llvm::PHINode* nPhi = b_.CreatePHI(b_.getInt64Ty(), 3, "tally.n");
  nPhi->addIncoming(zero, entryBB);
  llvm::Value* iPlusYlen = b_.CreateAdd(iPhi, yLen64, "tally.i_plus_ylen");
  llvm::Value* outerExit = b_.CreateICmpUGT(iPlusYlen, xLen64, "tally.outer_exit");
  b_.CreateCondBr(outerExit, doneBB, innerBB);

  // inner: j = phi; if j >= ylen → all matched → matchBB.
  startBlock(innerBB);
  llvm::PHINode* jPhi = b_.CreatePHI(b_.getInt64Ty(), 2, "tally.j");
  jPhi->addIncoming(zero, outerBB);
  llvm::Value* jGeYlen = b_.CreateICmpUGE(jPhi, yLen64, "tally.j_ge_ylen");
  b_.CreateCondBr(jGeYlen, matchBB, checkByteBB);

  // check_byte: compare x[i+j] vs y[j]; mismatch → nomatchBB.
  startBlock(checkByteBB);
  llvm::Value* ij = b_.CreateAdd(iPhi, jPhi, "tally.ij");
  llvm::Value* xijPtr = b_.CreateGEP(b_.getInt8Ty(), x, {ij}, "tally.xij_ptr");
  llvm::Value* yjPtr = b_.CreateGEP(b_.getInt8Ty(), y, {jPhi}, "tally.yj_ptr");
  llvm::Value* xij = b_.CreateLoad(b_.getInt8Ty(), xijPtr, "tally.xij");
  llvm::Value* yj = b_.CreateLoad(b_.getInt8Ty(), yjPtr, "tally.yj");
  llvm::Value* mismatch = b_.CreateICmpNE(xij, yj, "tally.mismatch");
  b_.CreateCondBr(mismatch, nomatchBB, innerLatchBB);

  // inner.latch: j = j + 1, back to inner.header.
  startBlock(innerLatchBB);
  jPhi->addIncoming(b_.CreateAdd(jPhi, one64, "tally.j_next"), innerLatchBB);
  b_.CreateBr(innerBB);

  // match: n++ and i += ylen, then back to outer.
  startBlock(matchBB);
  iPhi->addIncoming(b_.CreateAdd(iPhi, yLen64, "tally.i_match"), matchBB);
  nPhi->addIncoming(b_.CreateAdd(nPhi, one64, "tally.n_next"), matchBB);
  b_.CreateBr(outerBB);

  // nomatch: i++ only, then back to outer.
  startBlock(nomatchBB);
  iPhi->addIncoming(b_.CreateAdd(iPhi, one64, "tally.i_nomatch"), nomatchBB);
  nPhi->addIncoming(nPhi, nomatchBB);
  b_.CreateBr(outerBB);

  // done: merge n from loop-exit or zero from entry-abort.
  startBlock(doneBB);
  llvm::PHINode* donePhi = b_.CreatePHI(b_.getInt64Ty(), 2, "tally.n_done");
  donePhi->addIncoming(nPhi, outerBB);
  donePhi->addIncoming(zero, entryBB);
  b_.CreateBr(exitBB);

  startBlock(exitBB);
  return donePhi;
}

// --- W3 direct LLVM lowerings (P3) ---
// Mirrors the C bodies in runtime/rt_string.c. Each takes dst/dstcap and
// produces the same bit-for-bit result as the runtime pli_* entry.

// substr(dst, dstcap, src, srclen, start, len): copy `len` chars from src
// starting at 1-based `start`, clipping to srclen and dstcap, blank-padding
// the tail. Mirrors pli_substr.
void IRGen::emitSubstrLLVM(llvm::Value* dst, llvm::Value* dstcap, llvm::Value* src,
                           llvm::Value* srcLen, llvm::Value* start, llvm::Value* len) {
  llvm::Value* one = llvm::ConstantInt::get(b_.getInt64Ty(), 1);
  llvm::Value* zero = llvm::ConstantInt::get(b_.getInt64Ty(), 0);
  llvm::Value* blank = llvm::ConstantInt::get(b_.getInt8Ty(), ' ');

  llvm::Value* dstcap64 = b_.CreateZExtOrTrunc(dstcap, b_.getInt64Ty(), "sub.dstcap");
  llvm::Value* srclen64 = b_.CreateZExtOrTrunc(srcLen, b_.getInt64Ty(), "sub.srclen");
  llvm::Value* start0 = b_.CreateSub(start, one, "sub.start0");

  // n = min(len, dstcap); avail = max(srclen - start0, 0); take = min(n, avail).
  llvm::Value* n = b_.CreateZExtOrTrunc(len, b_.getInt64Ty(), "sub.n");
  llvm::Value* nClipped = b_.CreateSelect(b_.CreateICmpUGT(n, dstcap64), dstcap64, n, "sub.nclip");
  llvm::Value* avail = b_.CreateSelect(b_.CreateICmpULT(srclen64, start0), zero,
                                       b_.CreateSub(srclen64, start0, "sub.avail"), "sub.avail");
  llvm::Value* take =
      b_.CreateSelect(b_.CreateICmpULT(avail, nClipped), avail, nClipped, "sub.take");
  // start < 1 → no data copied, blank-fill the whole result (matches runtime guard).
  llvm::Value* validStart = b_.CreateICmpUGE(start, one, "sub.valid_start");
  take = b_.CreateSelect(validStart, take, zero, "sub.take_safe");

  llvm::Value* srcPtr = b_.CreateGEP(b_.getInt8Ty(), src, {start0}, "sub.srcptr");
  b_.CreateMemMove(dst, llvm::MaybeAlign(), srcPtr, llvm::MaybeAlign(), take, false);
  // blank-pad tail of the extraction.
  llvm::Value* padLen = b_.CreateSub(nClipped, take, "sub.padlen");
  b_.CreateMemSet(b_.CreateGEP(b_.getInt8Ty(), dst, {take}, "sub.padptr"), blank, padLen,
                  llvm::MaybeAlign(), false);
}

// substr_assign(dst, dstcap, start, len, src, srclen): overwrite `len` chars of
// dst starting at 1-based `start` with src, blank-filling the tail. Mirrors
// pli_substr_assign.
void IRGen::emitSubstrAssignLLVM(llvm::Value* dst, llvm::Value* dstcap, llvm::Value* start,
                                 llvm::Value* len, llvm::Value* src, llvm::Value* srcLen) {
  llvm::Value* one = llvm::ConstantInt::get(b_.getInt64Ty(), 1);
  llvm::Value* blank = llvm::ConstantInt::get(b_.getInt8Ty(), ' ');

  llvm::Value* dstcap64 = b_.CreateZExtOrTrunc(dstcap, b_.getInt64Ty(), "sa.dstcap");
  llvm::Value* srcLen64 = b_.CreateZExtOrTrunc(srcLen, b_.getInt64Ty(), "sa.srclen");
  llvm::Value* start0 = b_.CreateSub(start, one, "sa.start0");

  // n = len; space = dstcap - (start-1); if space <= 0 → return.
  llvm::Value* n = b_.CreateZExtOrTrunc(len, b_.getInt64Ty(), "sa.n");
  llvm::Value* space = b_.CreateSub(dstcap64, start0, "sa.space");
  llvm::Value* nClipped = b_.CreateSelect(b_.CreateICmpUGT(n, space), space, n, "sa.nclip");

  // take = min(srcLen, nClipped).
  llvm::Value* take =
      b_.CreateSelect(b_.CreateICmpULT(srcLen64, nClipped), srcLen64, nClipped, "sa.take");

  // memmove src into dst[start-1..start0+nClip).
  llvm::Value* dstPtr = b_.CreateGEP(b_.getInt8Ty(), dst, {start0}, "sa.dstptr");
  b_.CreateMemMove(dstPtr, llvm::MaybeAlign(), src, llvm::MaybeAlign(), take, false);
  // blank-fill the rest.
  llvm::Value* padLen = b_.CreateSub(nClipped, take, "sa.padlen");
  llvm::Value* padPtr2 = b_.CreateAdd(start0, take, "sa.padptr_off");
  b_.CreateMemSet(b_.CreateGEP(b_.getInt8Ty(), dst, {padPtr2}, "sa.padptr"), blank, padLen,
                  llvm::MaybeAlign(), false);
}

// substr_assign_varying(dst, dstcap, lenptr, start, len, src, srclen):
// same as substr_assign, plus grow the VARYING live length when the overlay
// reaches past it. Mirrors pli_substr_assign_varying.
void IRGen::emitSubstrAssignVaryingLLVM(llvm::Value* dst, llvm::Value* dstcap, llvm::Value* lenptr,
                                        llvm::Value* start, llvm::Value* len, llvm::Value* src,
                                        llvm::Value* srcLen) {
  llvm::Value* one = llvm::ConstantInt::get(b_.getInt64Ty(), 1);
  llvm::Value* zero = llvm::ConstantInt::get(b_.getInt64Ty(), 0);
  llvm::Value* blank = llvm::ConstantInt::get(b_.getInt8Ty(), ' ');

  llvm::Value* dstcap64 = b_.CreateZExtOrTrunc(dstcap, b_.getInt64Ty(), "sarv.dstcap");
  llvm::Value* srcLen64 = b_.CreateZExtOrTrunc(srcLen, b_.getInt64Ty(), "sarv.srclen");
  llvm::Value* start0 = b_.CreateSub(start, one, "sarv.start0");

  // load old live length.
  llvm::Value* oldLen = b_.CreateLoad(b_.getInt32Ty(), lenptr, "sarv.oldlen");
  llvm::Value* oldLen64 = b_.CreateZExtOrTrunc(oldLen, b_.getInt64Ty(), "sarv.oldlen64");
  llvm::Value* oldClamped =
      b_.CreateSelect(b_.CreateICmpSGT(oldLen64, dstcap64), dstcap64, oldLen64, "sarv.oldc");

  // n = len; space = dstcap - (start-1); nClip = min(n, space).
  llvm::Value* n = b_.CreateZExtOrTrunc(len, b_.getInt64Ty(), "sarv.n");
  llvm::Value* space = b_.CreateSub(dstcap64, start0, "sarv.space");
  llvm::Value* nClipped = b_.CreateSelect(b_.CreateICmpUGT(n, space), space, n, "sarv.nclip");

  // Fill gap between old and start with blanks if start > old.
  llvm::Value* gap =
      b_.CreateSelect(b_.CreateICmpUGT(start0, oldClamped),
                      b_.CreateSub(start0, oldClamped, "sarv.gap"), zero, "sarv.gapval");
  b_.CreateMemSet(b_.CreateGEP(b_.getInt8Ty(), dst, {oldClamped}, "sarv.gappptr"), blank, gap,
                  llvm::MaybeAlign(), false);

  // take = min(srcLen, nClip).
  llvm::Value* take =
      b_.CreateSelect(b_.CreateICmpULT(srcLen64, nClipped), srcLen64, nClipped, "sarv.take");
  // memmove src into dst[start-1..].
  llvm::Value* dstPtr = b_.CreateGEP(b_.getInt8Ty(), dst, {start0}, "sarv.dstptr");
  b_.CreateMemMove(dstPtr, llvm::MaybeAlign(), src, llvm::MaybeAlign(), take, false);
  // blank-pad tail of the overlay.
  llvm::Value* padLen = b_.CreateSub(nClipped, take, "sarv.padlen");
  llvm::Value* padPtr2 = b_.CreateAdd(start0, take, "sarv.padptr_off");
  b_.CreateMemSet(b_.CreateGEP(b_.getInt8Ty(), dst, {padPtr2}, "sarv.padptr"), blank, padLen,
                  llvm::MaybeAlign(), false);

  // Grow live length: end = max(start0 + nClip, oldLen).
  llvm::Value* end = b_.CreateAdd(start0, nClipped, "sarv.end");
  end = b_.CreateSelect(b_.CreateICmpUGT(end, oldClamped), end, oldClamped, "sarv.endmax");
  llvm::Value* end32 = b_.CreateTrunc(end, b_.getInt32Ty(), "sarv.end32");
  b_.CreateStore(end32, lenptr, false);
}

// repeat(dst, dstcap, src, srclen, n): copy src n times into dst, then
// blank-pad the remainder. Mirrors pli_repeat.
void IRGen::emitRepeatLLVM(llvm::Value* dst, llvm::Value* dstcap, llvm::Value* src,
                           llvm::Value* srcLen, llvm::Value* n) {
  llvm::Value* one = llvm::ConstantInt::get(b_.getInt64Ty(), 1);
  llvm::Value* zero = llvm::ConstantInt::get(b_.getInt64Ty(), 0);
  llvm::Value* blank = llvm::ConstantInt::get(b_.getInt8Ty(), ' ');

  llvm::Value* dstcap64 = b_.CreateZExtOrTrunc(dstcap, b_.getInt64Ty(), "rep.dstcap");
  llvm::Value* srcLen64 = b_.CreateZExtOrTrunc(srcLen, b_.getInt64Ty(), "rep.srclen");

  llvm::Function* F = b_.GetInsertBlock()->getParent();
  llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(ctx_, "rep.loop", F);
  llvm::BasicBlock* latchBB = llvm::BasicBlock::Create(ctx_, "rep.latch", F);
  llvm::BasicBlock* endBB = llvm::BasicBlock::Create(ctx_, "rep.end", F);

  llvm::Value* kN = b_.CreateZExtOrTrunc(n, b_.getInt64Ty(), "rep.n");
  b_.CreateMemSet(dst, blank, dstcap64, llvm::MaybeAlign(), false);
  b_.CreateCondBr(b_.CreateICmpULE(kN, zero, "rep.n_zero"), endBB, loopBB);

  llvm::BasicBlock* preLoopBB = b_.GetInsertBlock();
  startBlock(loopBB);
  llvm::PHINode* k = b_.CreatePHI(b_.getInt64Ty(), 2, "rep.k");
  k->addIncoming(zero, preLoopBB);
  // out = k * srclen; if out >= dstcap → exit.
  llvm::Value* outOff = b_.CreateMul(k, srcLen64, "rep.out_off");
  b_.CreateCondBr(b_.CreateOr(b_.CreateICmpUGE(k, kN, "rep.k_done"),
                              b_.CreateICmpUGE(outOff, dstcap64, "rep.out_full"), "rep.exit_cond"),
                  endBB, latchBB);

  startBlock(latchBB);
  // copyLen = min(srcLen, dstcap - out).
  llvm::Value* room = b_.CreateSub(dstcap64, outOff, "rep.room");
  llvm::Value* copyLen =
      b_.CreateSelect(b_.CreateICmpULE(srcLen64, room), srcLen64, room, "rep.copy_len");
  llvm::Value* dstPtr = b_.CreateGEP(b_.getInt8Ty(), dst, {outOff}, "rep.dstptr");
  b_.CreateMemMove(dstPtr, llvm::MaybeAlign(), src, llvm::MaybeAlign(), copyLen, false);
  k->addIncoming(b_.CreateAdd(k, one, "rep.k_next"), latchBB);
  b_.CreateBr(loopBB);

  startBlock(endBB);
}

// translate(dst, dstcap, s, slen, out, outlen, in, inlen): for each char in s,
// look it up in `in`; if found at index j < outlen, output out[j]; else
// passthrough. Mirrors pli_translate.
void IRGen::emitTranslateLLVM(llvm::Value* dst, llvm::Value* dstcap, llvm::Value* s,
                              llvm::Value* sLen, llvm::Value* out, llvm::Value* outLen,
                              llvm::Value* in, llvm::Value* inLen) {
  llvm::Value* one = llvm::ConstantInt::get(b_.getInt64Ty(), 1);
  llvm::Value* zero = llvm::ConstantInt::get(b_.getInt64Ty(), 0);

  llvm::Value* sLen64 = b_.CreateZExtOrTrunc(sLen, b_.getInt64Ty(), "tr.slen");
  llvm::Value* dstcap64 = b_.CreateZExtOrTrunc(dstcap, b_.getInt64Ty(), "tr.dstcap");
  llvm::Value* outLen64 = b_.CreateZExtOrTrunc(outLen, b_.getInt64Ty(), "tr.outlen");
  llvm::Value* inLen64 = b_.CreateZExtOrTrunc(inLen, b_.getInt64Ty(), "tr.inlen");

  llvm::Function* F = b_.GetInsertBlock()->getParent();
  llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(ctx_, "tr.loop", F);
  llvm::BasicBlock* innerBB = llvm::BasicBlock::Create(ctx_, "tr.inner", F);
  llvm::BasicBlock* checkBB = llvm::BasicBlock::Create(ctx_, "tr.check", F);
  llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(ctx_, "tr.done", F);

  // Outer loop: i = phi.  Load s[i]; exit when i >= slen or i >= dstcap.
  llvm::BasicBlock* preLoopBB = b_.GetInsertBlock();
  b_.CreateBr(loopBB);
  startBlock(loopBB);
  llvm::PHINode* iPhi = b_.CreatePHI(b_.getInt64Ty(), 2, "tr.i");
  iPhi->addIncoming(zero, preLoopBB);
  llvm::Value* si =
      b_.CreateLoad(b_.getInt8Ty(), b_.CreateGEP(b_.getInt8Ty(), s, {iPhi}, "tr.siptr"), "tr.si");
  llvm::Value* iDone = b_.CreateOr(b_.CreateICmpUGE(iPhi, sLen64, "tr.i_ge_slen"),
                                   b_.CreateICmpUGE(iPhi, dstcap64, "tr.i_ge_dst"), "tr.i_done");
  b_.CreateCondBr(iDone, doneBB, innerBB);

  llvm::BasicBlock* loadBB = llvm::BasicBlock::Create(ctx_, "tr.load", F);

  // Inner loop: k = phi.  If k >= inlen → not found, go to checkBB.
  // Otherwise load in[k], compare with s[i]; match → checkBB, mismatch → k++, loop.
  startBlock(innerBB);
  llvm::PHINode* kPhi = b_.CreatePHI(b_.getInt64Ty(), 2, "tr.k");
  kPhi->addIncoming(zero, loopBB);
  llvm::Value* kGeInlen = b_.CreateICmpUGE(kPhi, inLen64, "tr.k_ge_inlen");
  b_.CreateCondBr(kGeInlen, checkBB, loadBB);

  startBlock(loadBB);
  llvm::Value* ik =
      b_.CreateLoad(b_.getInt8Ty(), b_.CreateGEP(b_.getInt8Ty(), in, {kPhi}, "tr.inptr"), "tr.ik");
  llvm::Value* siEqIk = b_.CreateICmpEQ(si, ik, "tr.eq");
  kPhi->addIncoming(b_.CreateAdd(kPhi, one, "tr.k_next"), loadBB);
  b_.CreateCondBr(siEqIk, checkBB, innerBB);

  // check: if k < inlen → found → out[k] (or blank if k >= outlen); else passthrough.
  startBlock(checkBB);
  llvm::Value* kLtIn = b_.CreateICmpULT(kPhi, inLen64, "tr.k_lt_inlen");
  llvm::Value* outK = b_.CreateLoad(
      b_.getInt8Ty(), b_.CreateGEP(b_.getInt8Ty(), out, {kPhi}, "tr.outptr"), "tr.outk");
  llvm::Value* blank = llvm::ConstantInt::get(b_.getInt8Ty(), ' ');
  llvm::Value* byteVal = b_.CreateSelect(
      kLtIn,
      b_.CreateSelect(b_.CreateICmpULT(kPhi, outLen64, "tr.k_lt_out"), outK, blank, "tr.out_sel"),
      si, "tr.byte");
  b_.CreateStore(byteVal, b_.CreateGEP(b_.getInt8Ty(), dst, {iPhi}, "tr.dstptr"), false);
  iPhi->addIncoming(b_.CreateAdd(iPhi, one, "tr.i_next"), checkBB);
  b_.CreateBr(loopBB);

  startBlock(doneBB);
}

// trim(dst, dstcap, s, slen, pad, padlen): copy s minus leading/trailing pad
// characters, left-justified, blank-padded to dstcap. A null pad means blanks.
void IRGen::emitTrimLLVM(llvm::Value* dst, llvm::Value* dstcap, llvm::Value* s, llvm::Value* sLen,
                         llvm::Value* pad, llvm::Value* padLen) {
  llvm::Value* one = llvm::ConstantInt::get(b_.getInt64Ty(), 1);
  llvm::Value* zero = llvm::ConstantInt::get(b_.getInt64Ty(), 0);
  llvm::Value* blank = llvm::ConstantInt::get(b_.getInt8Ty(), ' ');

  llvm::Value* sLen64 = b_.CreateZExtOrTrunc(sLen, b_.getInt64Ty(), "trim.slen");
  llvm::Value* dstcap64 = b_.CreateZExtOrTrunc(dstcap, b_.getInt64Ty(), "trim.dstcap");
  llvm::Value* padLen64 = b_.CreateZExtOrTrunc(padLen, b_.getInt64Ty(), "trim.padlen");
  llvm::Value* padNonNull =
      b_.CreateICmpNE(pad, llvm::ConstantPointerNull::get(b_.getPtrTy()), "trim.pad_nonnull");

  llvm::Function* F = b_.GetInsertBlock()->getParent();
  llvm::Value* loSlot = b_.CreateAlloca(b_.getInt64Ty(), nullptr, "trim.lo");
  llvm::Value* hiSlot = b_.CreateAlloca(b_.getInt64Ty(), nullptr, "trim.hi");
  llvm::Value* outSlot = b_.CreateAlloca(b_.getInt64Ty(), nullptr, "trim.out");
  b_.CreateStore(zero, loSlot, false);
  b_.CreateStore(sLen64, hiSlot, false);
  b_.CreateStore(zero, outSlot, false);

  // isPad(c): if !padNonNull → c == blank; else memchr(pad, c, padlen) != null.
  auto emitIsPad = [&](llvm::Value* c) -> llvm::Value* {
    llvm::Value* isBlank = b_.CreateICmpEQ(c, blank, "trim.isblank");
    llvm::FunctionType* memchrType = llvm::FunctionType::get(
        b_.getPtrTy(), {b_.getPtrTy(), b_.getInt32Ty(), b_.getInt64Ty()}, false);
    llvm::Function* memchrFn = llvm::cast<llvm::Function>(
        b_.GetInsertBlock()->getModule()->getOrInsertFunction("memchr", memchrType).getCallee());
    llvm::Value* c32 = b_.CreateZExt(c, b_.getInt32Ty(), "trim.c32");
    llvm::Value* found = b_.CreateCall(memchrFn, {pad, c32, padLen64}, "trim.memchr");
    llvm::Value* inSet =
        b_.CreateICmpNE(found, llvm::ConstantPointerNull::get(b_.getPtrTy()), "trim.inset");
    return b_.CreateSelect(padNonNull, inSet, isBlank, "trim.ispad");
  };

  llvm::BasicBlock* loBB = llvm::BasicBlock::Create(ctx_, "trim.lo", F);
  llvm::BasicBlock* loBodyBB = llvm::BasicBlock::Create(ctx_, "trim.lo_body", F);
  llvm::BasicBlock* loExitBB = llvm::BasicBlock::Create(ctx_, "trim.lo_ex", F);
  llvm::BasicBlock* hiBB = llvm::BasicBlock::Create(ctx_, "trim.hi", F);
  llvm::BasicBlock* hiBodyBB = llvm::BasicBlock::Create(ctx_, "trim.hi_body", F);
  llvm::BasicBlock* hiExitBB = llvm::BasicBlock::Create(ctx_, "trim.hi_ex", F);
  llvm::BasicBlock* copyBB = llvm::BasicBlock::Create(ctx_, "trim.copy", F);
  llvm::BasicBlock* copyBodyBB = llvm::BasicBlock::Create(ctx_, "trim.copy_body", F);
  llvm::BasicBlock* padBB = llvm::BasicBlock::Create(ctx_, "trim.pad", F);
  llvm::BasicBlock* padBodyBB = llvm::BasicBlock::Create(ctx_, "trim.pad_body", F);
  llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(ctx_, "trim.done", F);

  // Phase 1: lo scan — while (lo < hi && isPad(s[lo])) lo++.
  b_.CreateBr(loBB);
  startBlock(loBB);
  {
    llvm::Value* lo = b_.CreateLoad(b_.getInt64Ty(), loSlot, "trim.lo_v");
    llvm::Value* hi = b_.CreateLoad(b_.getInt64Ty(), hiSlot, "trim.hi_v");
    llvm::Value* loLtHi = b_.CreateICmpULT(lo, hi, "trim.lo_lt_hi");
    llvm::Value* c = b_.CreateLoad(b_.getInt8Ty(),
                                   b_.CreateGEP(b_.getInt8Ty(), s, {lo}, "trim.lo_ptr"), "trim.c");
    llvm::Value* isPad = emitIsPad(c);
    b_.CreateCondBr(b_.CreateAnd(loLtHi, isPad, "trim.lo_cont"), loBodyBB, loExitBB);
  }
  startBlock(loBodyBB);
  {
    llvm::Value* lo = b_.CreateLoad(b_.getInt64Ty(), loSlot, "trim.lo_inc");
    b_.CreateStore(b_.CreateAdd(lo, one, "trim.lo_n"), loSlot, false);
    b_.CreateBr(loBB);
  }

  // Phase 2: hi scan — while (hi > lo && isPad(s[hi-1])) hi--.
  startBlock(loExitBB);
  b_.CreateBr(hiBB);
  startBlock(hiBB);
  {
    llvm::Value* lo = b_.CreateLoad(b_.getInt64Ty(), loSlot, "trim.hi_lo_v");
    llvm::Value* hi = b_.CreateLoad(b_.getInt64Ty(), hiSlot, "trim.hi_v");
    llvm::Value* hiGtLo = b_.CreateICmpUGT(hi, lo, "trim.hi_gt_lo");
    llvm::Value* c = b_.CreateLoad(
        b_.getInt8Ty(),
        b_.CreateGEP(b_.getInt8Ty(), s, {b_.CreateSub(hi, one, "trim.hi_m1")}, "trim.hi_ptr"),
        "trim.c2");
    llvm::Value* isPad = emitIsPad(c);
    b_.CreateCondBr(b_.CreateAnd(hiGtLo, isPad, "trim.hi_cont"), hiBodyBB, hiExitBB);
  }
  startBlock(hiBodyBB);
  {
    llvm::Value* hi = b_.CreateLoad(b_.getInt64Ty(), hiSlot, "trim.hi_dec");
    b_.CreateStore(b_.CreateSub(hi, one, "trim.hi_n"), hiSlot, false);
    b_.CreateBr(hiBB);
  }

  // Phase 3: copy s[lo..hi-1] → dst[0..], stop at dstcap.
  startBlock(hiExitBB);
  b_.CreateBr(copyBB);
  startBlock(copyBB);
  {
    llvm::Value* lo = b_.CreateLoad(b_.getInt64Ty(), loSlot, "trim.cp_lo");
    llvm::Value* hi = b_.CreateLoad(b_.getInt64Ty(), hiSlot, "trim.cp_hi");
    llvm::Value* out = b_.CreateLoad(b_.getInt64Ty(), outSlot, "trim.cp_out");
    llvm::Value* loLtHi = b_.CreateICmpULT(lo, hi, "trim.cp_lo_lt_hi");
    llvm::Value* outLtCap = b_.CreateICmpULT(out, dstcap64, "trim.cp_out_lt_cap");
    b_.CreateCondBr(b_.CreateAnd(loLtHi, outLtCap, "trim.cp_cont"), copyBodyBB, padBB);
  }
  startBlock(copyBodyBB);
  {
    llvm::Value* lo = b_.CreateLoad(b_.getInt64Ty(), loSlot, "trim.cpb_lo");
    llvm::Value* out = b_.CreateLoad(b_.getInt64Ty(), outSlot, "trim.cpb_out");
    llvm::Value* c = b_.CreateLoad(
        b_.getInt8Ty(), b_.CreateGEP(b_.getInt8Ty(), s, {lo}, "trim.cp_src"), "trim.cp_c");
    b_.CreateStore(c, b_.CreateGEP(b_.getInt8Ty(), dst, {out}, "trim.cp_dst"), false);
    b_.CreateStore(b_.CreateAdd(lo, one, "trim.cp_lo_n"), loSlot, false);
    b_.CreateStore(b_.CreateAdd(out, one, "trim.cp_out_n"), outSlot, false);
    b_.CreateBr(copyBB);
  }

  // Phase 4: blank-pad dst[out..] up to dstcap.
  startBlock(padBB);
  {
    llvm::Value* out = b_.CreateLoad(b_.getInt64Ty(), outSlot, "trim.pp_out");
    b_.CreateCondBr(b_.CreateICmpULT(out, dstcap64, "trim.pp_cont"), padBodyBB, doneBB);
  }
  startBlock(padBodyBB);
  {
    llvm::Value* out = b_.CreateLoad(b_.getInt64Ty(), outSlot, "trim.pp_inc");
    b_.CreateStore(blank, b_.CreateGEP(b_.getInt8Ty(), dst, {out}, "trim.pp_dst"), false);
    b_.CreateStore(b_.CreateAdd(out, one, "trim.pp_out_n"), outSlot, false);
    b_.CreateBr(padBB);
  }
  startBlock(doneBB);
}

// Direct LLVM lowering for FIXED(float) (W4): the runtime pli_fixed_of_float
// clamps out-of-range/NaN values (NaN→0, ±Inf→i64 min/max, else truncates
// toward zero). The saturated FP→SI intrinsic replicates this exactly:
// fptosi.sat.i64.f64 returns 0 for NaN, i64 min/max for ±Inf and OOR, and
// truncates toward zero for in-range values. The caller truncates to the
// destination FIXED width, matching the runtime's (long long return).
llvm::Value* IRGen::emitFixedOfFloatLLVM(llvm::Value* x) {
  llvm::Function* sat = intrinsicFn("llvm.fptosi.sat.i64.f64", b_.getInt64Ty(), {b_.getDoubleTy()});
  return b_.CreateCall(sat, {x}, "fxof_f");
}

// Direct LLVM lowering for FIXED(char) (W4): mirrors rt_stream.c pli_fixed_of_char.
// Skip leading blanks/tabs, consume an optional sign, scan digit chars, and
// store sawDigit into *okSlot (0 when no digits → CONVERSION trap by caller).
llvm::Value* IRGen::emitFixedOfCharLLVM(llvm::Value* s, llvm::Value* slen, llvm::Value* okSlot) {
  llvm::Function* F = b_.GetInsertBlock()->getParent();
  llvm::Value* zero = llvm::ConstantInt::get(b_.getInt64Ty(), 0);
  llvm::Value* one = llvm::ConstantInt::get(b_.getInt64Ty(), 1);
  llvm::Value* ten = llvm::ConstantInt::get(b_.getInt64Ty(), 10);
  llvm::Value* space = llvm::ConstantInt::get(b_.getInt8Ty(), ' ');
  llvm::Value* tab = llvm::ConstantInt::get(b_.getInt8Ty(), '\t');
  llvm::Value* plus = llvm::ConstantInt::get(b_.getInt8Ty(), '+');
  llvm::Value* minus = llvm::ConstantInt::get(b_.getInt8Ty(), '-');
  llvm::Value* zeroCh = llvm::ConstantInt::get(b_.getInt8Ty(), '0');
  llvm::Value* nineCh = llvm::ConstantInt::get(b_.getInt8Ty(), '9');
  llvm::Value* slen64 = b_.CreateZExtOrTrunc(slen, b_.getInt64Ty(), "fxc.slen");

  llvm::Value* idxSlot = b_.CreateAlloca(b_.getInt64Ty(), nullptr, "fxc.idx");
  llvm::Value* valSlot = b_.CreateAlloca(b_.getInt64Ty(), nullptr, "fxc.val");
  llvm::Value* negSlot = b_.CreateAlloca(b_.getInt1Ty(), nullptr, "fxc.neg");
  llvm::Value* sawSlot = b_.CreateAlloca(b_.getInt1Ty(), nullptr, "fxc.saw");
  b_.CreateStore(zero, idxSlot, false);
  b_.CreateStore(zero, valSlot, false);
  b_.CreateStore(b_.getFalse(), negSlot, false);
  b_.CreateStore(b_.getFalse(), sawSlot, false);

  // skip spaces/tabs
  llvm::BasicBlock* skipBB = llvm::BasicBlock::Create(ctx_, "fxc.skip", F);
  llvm::BasicBlock* skipBodyBB = llvm::BasicBlock::Create(ctx_, "fxc.skip_body", F);
  llvm::BasicBlock* signBB = llvm::BasicBlock::Create(ctx_, "fxc.sign", F);
  llvm::BasicBlock* digitBB = llvm::BasicBlock::Create(ctx_, "fxc.digit", F);
  llvm::BasicBlock* digitBodyBB = llvm::BasicBlock::Create(ctx_, "fxc.digit_body", F);
  llvm::BasicBlock* digitExitBB = llvm::BasicBlock::Create(ctx_, "fxc.digit_ex", F);
  b_.CreateBr(skipBB);
  startBlock(skipBB);
  {
    llvm::Value* i = b_.CreateLoad(b_.getInt64Ty(), idxSlot, "fxc.i");
    llvm::Value* cond = b_.CreateICmpULT(i, slen64, "fxc.skip_cond");
    llvm::Value* c = b_.CreateLoad(b_.getInt8Ty(),
                                   b_.CreateGEP(b_.getInt8Ty(), s, {i}, "fxc.skip_ptr"), "fxc.c");
    llvm::Value* isBlank = b_.CreateOr(b_.CreateICmpEQ(c, space, "fxc.isspace"),
                                       b_.CreateICmpEQ(c, tab, "fxc.istab"), "fxc.isblank");
    b_.CreateCondBr(b_.CreateAnd(cond, isBlank, "fxc.skip_cont"), skipBodyBB, signBB);
  }
  startBlock(skipBodyBB);
  {
    llvm::Value* i = b_.CreateLoad(b_.getInt64Ty(), idxSlot, "fxc.i_inc");
    b_.CreateStore(b_.CreateAdd(i, one, "fxc.i_next"), idxSlot, false);
    b_.CreateBr(skipBB);
  }

  // optional sign
  startBlock(signBB);
  {
    llvm::Value* i = b_.CreateLoad(b_.getInt64Ty(), idxSlot, "fxc.sign_i");
    llvm::Value* idxLtLen = b_.CreateICmpULT(i, slen64, "fxc.idx_lt_len");
    llvm::Value* c = b_.CreateLoad(b_.getInt8Ty(),
                                   b_.CreateGEP(b_.getInt8Ty(), s, {i}, "fxc.sign_ptr"), "fxc.sc");
    llvm::Value* isPlus = b_.CreateICmpEQ(c, plus, "fxc.is_plus");
    llvm::Value* isMinus = b_.CreateICmpEQ(c, minus, "fxc.is_minus");
    llvm::Value* hasSign =
        b_.CreateAnd(idxLtLen, b_.CreateOr(isPlus, isMinus, "fxc.is_sign"), "fxc.has_sign");
    llvm::Value* iNext = b_.CreateAdd(i, one, "fxc.sign_next");
    b_.CreateStore(b_.CreateSelect(hasSign, iNext, i, "fxc.idx_after_sign"), idxSlot, false);
    b_.CreateStore(isMinus, negSlot, false);
  }
  b_.CreateBr(digitBB);

  // digit scan (blocks declared above)
  startBlock(digitBB);
  {
    llvm::Value* i = b_.CreateLoad(b_.getInt64Ty(), idxSlot, "fxc.d_i");
    llvm::Value* cond = b_.CreateICmpULT(i, slen64, "fxc.d_cond");
    llvm::Value* c =
        b_.CreateLoad(b_.getInt8Ty(), b_.CreateGEP(b_.getInt8Ty(), s, {i}, "fxc.d_ptr"), "fxc.d_c");
    llvm::Value* isDigit = b_.CreateAnd(b_.CreateICmpUGE(c, zeroCh, "fxc.d_ge0"),
                                        b_.CreateICmpULE(c, nineCh, "fxc.d_le9"), "fxc.is_digit");
    b_.CreateCondBr(b_.CreateAnd(cond, isDigit, "fxc.d_cont"), digitBodyBB, digitExitBB);
  }
  startBlock(digitBodyBB);
  {
    llvm::Value* i = b_.CreateLoad(b_.getInt64Ty(), idxSlot, "fxc.d_i_inc");
    llvm::Value* v = b_.CreateLoad(b_.getInt64Ty(), valSlot, "fxc.d_val");
    llvm::Value* c = b_.CreateLoad(
        b_.getInt8Ty(), b_.CreateGEP(b_.getInt8Ty(), s, {i}, "fxc.d_digit_ptr"), "fxc.d_digit");
    llvm::Value* dig =
        b_.CreateZExt(b_.CreateSub(c, zeroCh, "fxc.d_sub"), b_.getInt64Ty(), "fxc.digit_val");
    llvm::Value* nv = b_.CreateAdd(b_.CreateMul(v, ten, "fxc.d_mul"), dig, "fxc.d_add");
    b_.CreateStore(nv, valSlot, false);
    b_.CreateStore(b_.getTrue(), sawSlot, false);
    b_.CreateStore(b_.CreateAdd(i, one, "fxc.d_next"), idxSlot, false);
    b_.CreateBr(digitBB);
  }

  startBlock(digitExitBB);
  {
    llvm::Value* saw = b_.CreateLoad(b_.getInt1Ty(), sawSlot, "fxc.saw");
    llvm::Value* okVal = b_.CreateZExt(saw, b_.getInt32Ty(), "fxc.ok_val");
    b_.CreateStore(okVal, okSlot, false);
    llvm::Value* val = b_.CreateLoad(b_.getInt64Ty(), valSlot, "fxc.val_load");
    llvm::Value* isNeg = b_.CreateLoad(b_.getInt1Ty(), negSlot, "fxc.neg_load");
    llvm::Value* negVal = b_.CreateSub(zero, val, "fxc.neg_val");
    return b_.CreateSelect(isNeg, negVal, val, "fxc.result");
  }
}

// Get (or create) a declaration for a runtime `pli_*` function. The signature
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
    case RtI32Ptr:
      return b_.getPtrTy();
    case RtI128:
      return b_.getInt128Ty();
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
  if (!f)
    f = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, name, &mod_);
  applyRuntimeAttrs(f);
  return f;
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
  llvm::Value* topCtx =
      b_.CreateCall(runtimeFn("pli_on_top_ctx"), {i64((long long)key)}, tag + "ctx");
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
    b_.CreateCall(handlers[i], {topCtx});
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
  if (!zdivChecks())
    return computed; // (NOZERODIVIDE): the raw result stands (rules (60)-(63), ADR-112)
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
  if (!sizeChecks()) {
    // (NOSIZE): plain arithmetic — the wrapped value stands (rules (60)-(63),
    // ADR-110). Skip the overflow intrinsic entirely; its struct return and
    // extractvalue block the optimizer's overflow-narrowing passes.
    std::string name = op == Tok::Plus ? "bin" : op == Tok::Minus ? "bin" : "bin";
    if (op == Tok::Plus)
      return b_.CreateAdd(a, b, name);
    if (op == Tok::Minus)
      return b_.CreateSub(a, b, name);
    return b_.CreateMul(a, b, name);
  }
  llvm::Type* ty = a->getType();
  unsigned bits = ty->getIntegerBitWidth();
  // Constant-fold overflow checks (Task 12): both operands compile-time known,
  // so compute the result and overflow flag at emit time and drop the
  // with.overflow intrinsic + trap block. Only matters at -O0/-O1 (-O2/-O3
  // default to NOSIZE per Task 7). Sound because both operands are constants.
  if (auto* ca = llvm::dyn_cast<llvm::ConstantInt>(a)) {
    if (auto* cb = llvm::dyn_cast<llvm::ConstantInt>(b)) {
      const llvm::APInt& av = ca->getValue();
      const llvm::APInt& bv = cb->getValue();
      bool of = false;
      llvm::APInt rv;
      if (op == Tok::Plus)
        rv = av.sadd_ov(bv, of);
      else if (op == Tok::Minus)
        rv = av.ssub_ov(bv, of);
      else // Tok::Star
        rv = av.smul_ov(bv, of);
      if (!of)
        return llvm::ConstantInt::get(ty, rv);
      // Overflow possible: fall through to the intrinsic + trap path below.
    }
  }
  std::string base = op == Tok::Plus ? "sadd" : op == Tok::Minus ? "ssub" : "smul";
  std::string iname = "llvm." + base + ".with.overflow.i" + std::to_string(bits);
  llvm::Type* st = llvm::StructType::get(ctx_, {ty, b_.getInt1Ty()});
  llvm::Value* ov = b_.CreateCall(intrinsicFn(iname, st, {ty, ty}), {a, b}, "ov");
  llvm::Value* r = b_.CreateExtractValue(ov, 0, "bin");
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
// INT_MIN/INT128_MIN is caught without negating it. The limit always fits in
// an i64 (it is pliPow10(prec<=18) or 2^31); for a wide (>18 digit) source
// (ADR-191) the limit is sign-extended to i128 so the compare is same-width.
void IRGen::magTrap(llvm::Value* v, long long limit) {
  if (!sizeChecks())
    return; // (NOSIZE): the wrapped value stands (rules (60)-(63), ADR-110)
  unsigned bits = v->getType()->getIntegerBitWidth();
  llvm::Value* limHi = bits == 128 ? i128(pli_from_i64(limit)) : i64(limit);
  llvm::Value* limLo = bits == 128 ? i128(pli_neg_s(pli_from_i64(limit))) : i64(-limit);
  llvm::Value* hi = b_.CreateICmpSGE(v, limHi, "dov.hi");
  llvm::Value* lo = b_.CreateICmpSLE(v, limLo, "dov.lo");
  llvm::Value* of = b_.CreateOr(hi, lo, "dov");
  int seq = ovSeq_++;
  llvm::BasicBlock* trapBB =
      llvm::BasicBlock::Create(ctx_, "ov.trap." + std::to_string(seq), curFn_);
  llvm::BasicBlock* okBB = llvm::BasicBlock::Create(ctx_, "ov.ok." + std::to_string(seq), curFn_);
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
  llvm::Value* okLo =
      loIncl ? b_.CreateFCmpOGE(f, flt(lo), "frt.lo") : b_.CreateFCmpOGT(f, flt(lo), "frt.lo");
  llvm::Value* okHi = b_.CreateFCmpOLT(f, flt(hi), "frt.hi");
  llvm::Value* ok = b_.CreateAnd(okLo, okHi, "frt.ok");
  int seq = ovSeq_++;
  llvm::BasicBlock* trapBB =
      llvm::BasicBlock::Create(ctx_, "ov.trap." + std::to_string(seq), curFn_);
  llvm::BasicBlock* okBB = llvm::BasicBlock::Create(ctx_, "ov.ok." + std::to_string(seq), curFn_);
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
  // Static-analysis P1: every ENTRY symbol is created alongside its Proc, but
  // guard the dereference anyway — an entry without a procedure falls through
  // to the existing null (caller-must-handle) tail below instead of crashing.
  if (en && callee)
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
  for (size_t i = 0; i < sym->entryParams.size(); ++i) {
    // A by-value entry (rules (34),(38)) takes FIXED/FLOAT/POINTER scalars
    // as C values; everything else keeps the by-reference address form.
    const Type& t = sym->entryParams[i];
    if (sym->entryByValue && (t.isFixed() || t.k == TK::Float || t.isPointer()))
      pt.push_back(llvmTy(t));
    else
      pt.push_back(b_.getPtrTy());
  }
  // External ENTRY symbols (no PL/I body): by-value C entries (rules
  // (34),(38)) use their declared params verbatim — any char(*) lengths
  // must be explicit scalar fixed-bin(31) params (c_bridge.inc pattern).
  // A plain-EXTERNAL PL/I entry is a procedure in another object file and
  // follows the PL/I inter-procedure convention: hidden extent/length args
  // for `*`-extent arrays (rule (13)) and CHAR(*) params (rule (18)),
  // exactly as if the procedure were defined in this compilation.
  if (!sym->proc && !en) {
    if (!sym->entryByValue)
      for (const Type& t : sym->entryParams) {
        if (t.isArray() && !t.dims.empty() && t.dims[0].adj)
          pt.push_back(b_.getInt64Ty()); // hidden `*` extent arg (rule (13))
        if (t.isChar() && t.starLen)
          pt.push_back(b_.getInt64Ty()); // hidden length arg (rule (18))
      }
    // An external character-valued function (rules (34),(37)) returns through
    // a hidden result buffer like structures do: the caller allocates it and
    // passes its address as the first argument, so the declaration is
    // `void @NAME(ptr sret, ...)`.
    if (sym->entryIsFunction && sym->entryRetTy.isChar()) {
      std::vector<llvm::Type*> pt2 = pt;
      pt2.insert(pt2.begin(), b_.getPtrTy());
      llvm::FunctionType* ft2 = llvm::FunctionType::get(b_.getVoidTy(), pt2, false);
      return llvm::Function::Create(ft2, llvm::Function::ExternalLinkage, name, &mod_);
    }
    llvm::Type* rty = sym->entryIsFunction ? llvmTy(sym->entryRetTy) : b_.getVoidTy();
    llvm::FunctionType* ft = llvm::FunctionType::get(rty, pt, false);
    return llvm::Function::Create(ft, llvm::Function::ExternalLinkage, name, &mod_);
  }
  for (const Type& t : sym->entryParams) {
    if (t.isArray() && !t.dims.empty() && t.dims[0].adj)
      pt.push_back(b_.getInt64Ty()); // hidden `*` extent arg (rule (13))
    if (t.isChar() && t.starLen)
      pt.push_back(b_.getInt64Ty()); // hidden length arg (rule (18))
  }
  // An external character-valued function (rules (34),(37)) returns through a
  // hidden result buffer like structures do; for PL/I callers outside this
  // TU we cannot generate the buffer allocation site here, but the external
  // declaration must match the callee's inter-procedure signature (ptr-first).
  bool extChar = sym->entryIsFunction && sym->entryRetTy.isChar();
  std::vector<llvm::Type*> pt2 = pt;
  if (extChar) {
    // Emit the declaration with hidden-result-pointer semantics: the external
    // entry returns void and accepts a ptr as first argument (the sret).
    pt2.insert(pt2.begin(), b_.getPtrTy());
    llvm::FunctionType* ft2 = llvm::FunctionType::get(b_.getVoidTy(), pt2, false);
    return llvm::Function::Create(ft2, llvm::Function::ExternalLinkage, name, &mod_);
  }
  llvm::Type* rty = sym->entryIsFunction ? llvmTy(sym->entryRetTy) : b_.getVoidTy(); // rule (34)
  llvm::FunctionType* ft = llvm::FunctionType::get(rty, pt, false);
  return llvm::Function::Create(ft, llvm::Function::ExternalLinkage, name, &mod_);
}

// LLVM function type of a call through an entry variable/parameter (IBM ENTRY
// VARIABLE extension, ADR-171), built from its declared descriptor. PL/I
// passes every argument by reference (ptr) plus a hidden i64 for each
// `*`-extent / CHAR(*) parameter; a by-value C entry marshals FIXED/FLOAT
// scalars and POINTERs as values. No static-link arguments are threaded: a
// procedure that captures enclosing state is diagnosed at assignment.
llvm::FunctionType* IRGen::entryFnType(Symbol* sym) {
  std::vector<llvm::Type*> pt;
  bool sret = sym->entryIsFunction && (sym->entryRetTy.isStruct() || sym->entryRetTy.isChar());
  if (sret)
    pt.push_back(b_.getPtrTy());
  for (const Type& t : sym->entryParams) {
    if (sym->entryByValue && (t.isFixed() || t.k == TK::Float || t.isPointer()))
      pt.push_back(llvmTy(t));
    else
      pt.push_back(b_.getPtrTy());
  }
  if (!sym->entryByValue)
    for (const Type& t : sym->entryParams) {
      if (t.isArray() && !t.dims.empty() && t.dims[0].adj)
        pt.push_back(b_.getInt64Ty()); // hidden `*` extent arg (rule (13))
      if (t.isChar() && t.starLen)
        pt.push_back(b_.getInt64Ty()); // hidden length arg (rule (18))
    }
  llvm::Type* rt =
      sret ? b_.getVoidTy() : (sym->entryIsFunction ? llvmTy(sym->entryRetTy) : b_.getVoidTy());
  return llvm::FunctionType::get(rt, pt, false);
}

llvm::CallInst* IRGen::emitEntryCall(Symbol* sym, std::vector<HExprP>& args, SourceLoc loc,
                                     llvm::Value** sretPtr) {
  if (sretPtr)
    *sretPtr = nullptr;
  Type rty = sym->entryIsFunction ? sym->entryRetTy : Type::voidTy();
  bool sret = rty.isStruct() || rty.isChar();
  llvm::FunctionType* ft = entryFnType(sym);
  std::vector<llvm::Value*> callArgs;
  if (sret) {
    llvm::Value* buf = rty.isChar() ? sretAlloc(rty) : entryAlloca(llvmTy(rty), "sret");
    if (sretPtr)
      *sretPtr = buf;
    callArgs.push_back(buf);
  }
  for (size_t i = 0; i < args.size() && i < sym->entryParams.size(); ++i) {
    HExpr* a = args[i].get();
    const Type& pty = sym->entryParams[i];
    if (a->kind == HExpr::Star) {
      // Omitted OPTIONAL (extension, ADR-119): a null pointer.
      callArgs.push_back(llvm::Constant::getNullValue(b_.getPtrTy()));
      continue;
    }
    if (sym->entryByValue)
      callArgs.push_back(marshalArg(a, pty, loc));
    else
      callArgs.push_back(argAddr(a, pty));
  }
  // Trailing omitted OPTIONALs pad with nulls to the full declared signature.
  for (size_t i = args.size(); i < sym->entryParams.size(); ++i)
    callArgs.push_back(llvm::Constant::getNullValue(b_.getPtrTy()));
  if (!sym->entryByValue)
    for (size_t i = 0; i < sym->entryParams.size(); ++i) {
      const Type& t = sym->entryParams[i];
      if (!((t.isArray() && !t.dims.empty() && t.dims[0].adj) || (t.isChar() && t.starLen)))
        continue;
      HExpr* a = (i >= args.size() || args[i]->kind == HExpr::Star) ? nullptr : args[i].get();
      llvm::Value* ext = hiddenAdjustArg(a, t, t.controlled);
      if (!ext) {
        d_.error(loc, "a '*' extent parameter takes a fixed or dynamic-bound array in this stage",
                 "(13)");
        ext = i64(0);
      }
      callArgs.push_back(ext);
    }
  llvm::Value* fp = loadSym(sym, sym->ty).reg;
  // A void-returning entry (or a hidden-buffer result) cannot name the call.
  llvm::CallInst* call = (sret || rty.isVoid()) ? b_.CreateCall(ft, fp, callArgs)
                                                : b_.CreateCall(ft, fp, callArgs, "fres");
  flushVarWrites();
  return call;
}

// ---------------------------------------------------------------------------
// module
// ---------------------------------------------------------------------------
std::string IRGen::run(HProgram& prog) {
  // Reject unsupported signatures before constructing a partial module.
  for (auto& p : prog.procs) {
    bool hasEntries = false;
    for (auto& st : p->body)
      if (st && st->kind == HStmt::Entry) {
        hasEntries = true;
        break;
      }
    // A character-valued function (rules (34),(37)) returns through a hidden
    // result buffer like a structure (rule 127); with ENTRY segments the
    // shared impl cannot carry the buffer, so it stays diagnosed (rule 56).
    if (hasEntries && p->commonRetTy.isChar())
      for (auto& st : p->body)
        if (st && st->kind == HStmt::Entry)
          d_.error(st->loc,
                   "a character-returning procedure with ENTRY statements is not implemented in "
                   "this stage",
                   "(56)");
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

  // Bitcode runtime (OPTIMIZATION.md §10, P3): link the pli_* definitions the
  // module references in from runtime.bc, so clang compiles real bodies and
  // can inline/specialize them instead of resolving external archive calls.
  // Only the MAIN unit embeds: runtime globals (CONTROLLED and ON stacks,
  // I/O state) must be shared across units, so every other unit keeps
  // external pli_* references that the embedded copy or libpli.a satisfies.
  // -emit-llvm keeps the raw module (its IR goldens pin compiler output, not
  // the runtime); the driver passes linkBitcode_ = false there.
  if (linkBitcode_ && !runtimeBc_.empty() && prog.mainProc)
    if (!linkRuntimeBitcode())
      return "";

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
  // Use clean target triple (no features). SME features (zcm, zcz) are
  // disabled explicitly in createTargetMachine.
  if (!triple_.empty()) {
    llvm::Triple t(triple_);
    if (t.isMacOSX()) {
      mod_.setTargetTriple(llvm::Triple("arm64-apple-macosx15.0"));
    } else {
      mod_.setTargetTriple(t);
    }
  }
  mod_.print(os, nullptr);
  return ir;
}

// Bitcode runtime (OPTIMIZATION.md §10, P3): pull the pli_* definitions the
// module references out of runtime.bc so clang compiles real bodies and can
// inline/specialize them. The LLVM version stamp (rt_core.c plic_llvm_version)
// is checked first: runtime.bc is built by the same LLVM as plic, and a stale
// bitcode would mis-compile silently. The whole runtime is merged into the
// MAIN unit with external linkage, so every unit's pli_* references resolve
// to this one shared copy and the runtime's global state stays shared.
bool IRGen::linkRuntimeBitcode() {
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> rt = llvm::parseIRFile(runtimeBc_, err, ctx_);
  if (!rt) {
    d_.error({},
             "internal error: cannot load the runtime bitcode '" + runtimeBc_ +
                 "': " + err.getMessage().str(),
             "");
    return false;
  }
  // A missing or mismatched stamp means a foreign or stale runtime.bc: the
  // emitted IR syntax is tied to the LLVM plic was built against.
  std::string stamp = "unknown";
  if (llvm::GlobalVariable* gv = rt->getNamedGlobal("plic_llvm_version"))
    if (llvm::Constant* init = gv->getInitializer())
      if (llvm::ConstantDataArray* s = llvm::dyn_cast<llvm::ConstantDataArray>(init))
        stamp = s->getAsCString().str(); // strips the trailing NUL
  if (stamp != PLIC_LLVM_VERSION) {
    d_.error({},
             "runtime bitcode '" + runtimeBc_ + "' was built with LLVM " + stamp +
                 " but plic with LLVM " PLIC_LLVM_VERSION +
                 " — rebuild it or pass --no-bitcode-runtime",
             "");
    return false;
  }
  // A runtime built for another target must not be merged into this module.
  // Compare arch + OS family, not the version-qualified triple string: Apple's
  // darwin/macosx naming and the OS version differ between LLVM builds (e.g.
  // arm64-apple-darwin25.6.0 vs arm64-apple-macosx26.0.0) yet are the same ABI.
  llvm::Triple rtTriple(rt->getTargetTriple());
  llvm::Triple tgtTriple(triple_);
  // LLVM 23 splits the Darwin family into Darwin and MacOSX OSType values;
  // both are the same ABI family, so fold them onto one key.
  auto osKey = [](llvm::Triple::OSType os) -> int {
    return (os == llvm::Triple::Darwin || os == llvm::Triple::MacOSX) ? (int)llvm::Triple::Darwin
                                                                      : (int)os;
  };
  if (rtTriple.getArch() != tgtTriple.getArch() ||
      osKey(rtTriple.getOS()) != osKey(tgtTriple.getOS())) {
    d_.error({},
             "runtime bitcode target '" + rtTriple.str() + "' does not match '" + triple_ +
                 "' — rebuild it or pass --no-bitcode-runtime",
             "");
    return false;
  }
  // The whole runtime is merged (no LinkOnlyNeeded) because library units
  // reference pli_* symbols this module does not call itself: only a full
  // merge guarantees every unit's reference resolves to this one embedded
  // copy and the archive is never pulled (its members would duplicate the
  // symbols otherwise). Dead-stripping at the final link removes whatever the
  // program does not use. The definitions keep their external linkage so the
  // runtime's global state (CONTROLLED/ON stacks, I/O) stays shared across
  // units; the P0 side-table facts are re-applied so the optimizer keeps the
  // audited memory/alloc effects on the real bodies.
  // Strip the build host's target-cpu / target-features from the pre-compiled
  // runtime (IRGen P0): if they survive into the merged module, LLVM's inliner
  // refuses to honour the AlwaysInline that applyRuntimeAttrs stamps on the
  // math wrappers (pli_sin, pli_mod_ll, ...) when the feature sets differ.
  // KEEP probe-stack: runtime.bc is built for apple-m1, and __chkstk_darwin is
  // the native probe method for arm64-apple (our targets), so it is sound.
  // Stamp NoInline on every runtime body so the non-always-inline wrappers
  // (e.g. pli_put_list_char) are not generically inlined at -O2 and survive
  // -dead_strip at link (tests/driver/link.sh dead-strip sub-check). The math
  // wrappers are re-attributed by applyRuntimeAttrs below, which clears NoInline
  // and sets AlwaysInline for them. Mirrors linkEmbeddedLibPLI
  // (codegen.cpp:80-95) / emitRuntimeObject (codegen.cpp:128-139).
  for (auto& F : *rt) {
    if (F.hasFnAttribute("target-cpu"))
      F.removeFnAttr("target-cpu");
    if (F.hasFnAttribute("target-features"))
      F.removeFnAttr("target-features");
    F.addFnAttr(llvm::Attribute::NoInline);
  }
  // Align the data layout with ours first: both describe the same ABI, but
  // clang's darwin layout carries extra address-space pointee specs that the
  // runtime C never uses, and the linker warns on any textual difference.
  if (!mod_.getDataLayoutStr().empty())
    rt->setDataLayout(mod_.getDataLayoutStr());
  if (llvm::Linker::linkModules(mod_, std::move(rt), llvm::Linker::None)) {
    d_.error({}, "internal error: linking the runtime bitcode into the module failed", "");
    return false;
  }
  auto& sigs = kRuntimeSigs();
  for (llvm::Function& f : mod_.functions())
    if (!f.isDeclaration() && sigs.count(f.getName().str()))
      applyRuntimeAttrs(&f);
  return true;
}

// In-process backend ownership (review §4): build via the tested textual
// path, then re-parse into a caller-owned context (triple/DataLayout ride in
// the IR, so the module is emission-ready). Null when diagnostics failed.
std::unique_ptr<IRGen::OwnedModule> IRGen::takeModule(HProgram& prog) {
  std::string ir = run(prog);
  if (!d_.ok())
    return nullptr;
  auto om = std::make_unique<OwnedModule>();
  om->ctx = std::make_unique<llvm::LLVMContext>();
  llvm::SMDiagnostic smErr;
  auto mem = llvm::MemoryBuffer::getMemBuffer(ir, "plic");
  om->mod = llvm::parseAssembly(mem->getMemBufferRef(), smErr, *om->ctx);
  if (!om->mod) {
    d_.error({}, "internal error: could not re-parse generated IR", "");
    return nullptr;
  }
  return om;
}

// An LLVM scalar constant for an INITIAL element value (rule 26). Null is
// returned only for types without a constant form (e.g. STRUCT), which callers
// fall back to zero-initialising.
llvm::Constant* IRGen::scalarInitConstant(const Type& t, const Expr* ini) {
  switch (t.k) {
  case TK::FixedBin:
  case TK::FixedDec: {
    // Wide DECIMAL literal (ADR-191): rescale the full i128 value, not the wrapped i64 truncation.
    if (needsWideDecLit(ini, t))
      return wideDecConstant(t, decLitRescaled128(ini, t));
    long long v = 0;
    if (ini) {
      if (ini->kind == Expr::DecLit) {
        v = ini->ival;
        int dq = t.scale - ini->decScale; // rescale to the target type's scale
        v = dq > 0 ? v * pliPow10(dq) : dq < 0 ? pliRescaleDown(v, -dq, t.k == TK::FixedDec) : v;
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
    if (t.len == 1) {
      int v = 0;
      if (ini)
        v = ini->kind == Expr::BitLit ? (!ini->sval.empty() && ini->sval[0] == '1')
                                      : (ini->ival != 0 || ini->fval != 0);
      return llvm::ConstantInt::get(b_.getInt8Ty(), v);
    }
    std::vector<unsigned char> bytes = packBitInit(ini, t.len);
    return llvm::ConstantDataArray::get(ctx_, bytes);
  }
  case TK::Task:
    return llvm::ConstantInt::get(b_.getInt32Ty(), 0, true);
  case TK::Event:
    // A fresh EVENT is complete (1), so WAIT before any CALL returns at once;
    // CALL resets it to 0 and the task's end sets it back to 1 (rule (79)).
    return llvm::ConstantInt::get(b_.getInt32Ty(), 1, true);
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
  case TK::Char: {
    // A character INITIAL (rule (26)): materialise the blank-padded bytes
    // into a global so the value has a pointer form like any char value.
    std::string text(t.len, ' ');
    int actualLen = 0;
    if (e && e->kind == Expr::CharLit) {
      actualLen = (int)e->sval.size();
      for (int i = 0; i < t.len && i < actualLen; ++i)
        text[i] = e->sval[i];
    }
    llvm::Constant* data = llvm::ConstantDataArray::getString(ctx_, text, false);
    llvm::GlobalVariable* g = new llvm::GlobalVariable(
        mod_, data->getType(), true, llvm::GlobalValue::PrivateLinkage, data, ".initchar");
    g->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    v.ptr = b_.CreateConstGEP2_32(data->getType(), g, 0, 0, "initcp");
    // For VARYING strings, the live length is the actual string length, not the max
    v.len = i64(t.varying ? actualLen : t.len);
    break;
  }
  case TK::Bit: {
    if (t.len == 1) {
      v.reg = b_.getInt1(e->kind == Expr::BitLit ? (!e->sval.empty() && e->sval[0] == '1')
                                                 : (e->ival != 0 || e->fval != 0));
      break;
    }
    std::vector<unsigned char> bytes = packBitInit(e, t.len);
    v.reg = llvm::ConstantDataArray::get(ctx_, bytes);
    break;
  }
  default: { // Fixed
    // Wide DECIMAL literal (ADR-191): rescale the full i128 value, not the wrapped i64 truncation.
    if (needsWideDecLit(e, t)) {
      v.reg = wideDecConstant(t, decLitRescaled128(e, t));
      break;
    }
    long long iv = e->kind == Expr::FltLit ? (long long)e->fval : e->ival;
    if (e->kind == Expr::DecLit) {
      int dq = t.scale - e->decScale; // rescale to the target type's scale
      iv = dq > 0 ? iv * pliPow10(dq) : dq < 0 ? pliRescaleDown(iv, -dq, t.k == TK::FixedDec) : iv;
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
      } else if (t.elementType().isEvent()) {
        // A STATIC EVENT array starts complete (1) per element (rule (79)).
        std::vector<llvm::Constant*> els((size_t)arrayExtent(t),
                                         llvm::ConstantInt::get(b_.getInt32Ty(), 1, true));
        ginit = llvm::ConstantArray::get(llvm::cast<llvm::ArrayType>(gt), els);
      } else {
        ginit = llvm::ConstantAggregateZero::get(gt);
      }
    } else {
      ginit = scalarInitConstant(t, s->initExpr);
      if (!ginit)
        ginit = llvm::ConstantAggregateZero::get(gt); // STRUCT (rule 11)
    }
    // EXTERNAL (rule (42)) uses common linkage so every translation unit
    // shares one definition at link time; STATIC stays module-private.
    // Common linkage requires a zero initializer (a varying's blank fill
    // is non-zero but its length is 0, so zero-fill is equivalent).
    auto link = s->external ? llvm::GlobalValue::CommonLinkage : llvm::GlobalValue::InternalLinkage;
    if (s->external)
      ginit = llvm::ConstantAggregateZero::get(gt);
    auto* g = new llvm::GlobalVariable(mod_, gt, false, link, ginit, s->irName.substr(1));
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
  // A CONTROLLED variable (rules (15),(87)-(90), ADR-140) has no frame
  // storage: its address is the latest generation on its runtime stack.
  // Inside a hoisted DO loop the preheader already read it (emitDoIter).
  if (sym->controlled) {
    auto hit = ctlAddrHoist_.find(sym);
    if (hit != ctlAddrHoist_.end())
      return hit->second;
    return b_.CreateCall(runtimeFn("pli_ctl_addr"), {ctlKeyOf(sym)}, "ctladdr");
  }
  // rule (8): an enclosing variable is reached through this frame's static
  // link; otherwise it is this frame's own storage (globals / allocas /
  // parameters). Both are recorded in symAddr_.
  return symAddr_.count(sym) ? symAddr_[sym] : nullptr;
}

void IRGen::allocaLocals(HProc* p) {
  areaLocals_.clear();
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
    // Constant scalar INITIAL/VALUE stores at once (rule 26): dynamic bound
    // expressions below load locals, and the store must dominate those loads
    // even without optimization. emitInitials skips these (same helper).
    if (Val early; constScalarInit(s, early))
      storeTo(s, early, s->loc);
    if (s->ty.isArea()) {
      // An AUTOMATIC AREA (rule (20)): create the runtime region at entry and
      // destroy it on exit (emitCtlEpilogue). Allocation from it supports
      // out-of-order FREE via the region's size accounting.
      llvm::Value* region = b_.CreateCall(runtimeFn("pli_area_create"), {i64(s->areaSize)}, "area");
      b_.CreateStore(region, a);
      areaLocals_.push_back(s);
    } else if (s->ty.isArray() && (s->ty.elementType().isTask() || s->ty.elementType().isEvent())) {
      // A TASK/EVENT array (rules (15),(79),(82)): each element is its own
      // handle/flag — TASK starts unset (0), EVENT starts complete (1).
      bool isTask = s->ty.elementType().isTask();
      llvm::ArrayType* arrTy =
          llvm::ArrayType::get(llvmTy(s->ty.elementType()), (unsigned)arrayExtent(s->ty));
      for (long long i = 0, n = arrayExtent(s->ty); i < n; ++i) {
        llvm::Value* ep =
            b_.CreateInBoundsGEP(arrTy, a, {i64(0), i64(i)}, isTask ? "tk.i" : "ev.i");
        b_.CreateStore(i32(isTask ? 0 : 1), ep);
      }
    } else if (s->ty.isTask())
      b_.CreateStore(i32(0), a);
    else if (s->ty.isEvent())
      // A fresh EVENT is complete (1); CALL resets it (rule (79), QR2.8).
      b_.CreateStore(i32(1), a);
    // A structure starts zero-filled (rules (11),(26)): INITIAL itemlists may
    // skip members, and BY NAME leaves unmatched members alone, so an
    // uninitialized read of a member would be LLVM poison; zero-fill at entry.
    else if (s->ty.isStruct())
      b_.CreateStore(llvm::ConstantAggregateZero::get(llvmTy(s->ty)), a);
    if (s->ty.isChar() && !s->ty.isArray()) { // blank fill (scalar char only)
      std::string blanks(s->ty.len, ' ');
      llvm::Value* g = globalString(blanks);
      if (s->ty.varying) {
        llvm::Value* lenp = b_.CreateStructGEP(llvmTy(s->ty), a, 0, "lenp");
        b_.CreateStore(i32(0), lenp);
        // Blank-fill the data area too (rule (18)): a VARYING actual passed
        // to a non-VARYING `CHAR(*)` dummy overlays data by reference, and
        // scans past the live length must see blanks, not uninitialized bytes.
        llvm::Value* dp = b_.CreateStructGEP(llvmTy(s->ty), a, 1, "vdat");
        b_.CreateMemCpy(dp, llvm::MaybeAlign(), g, llvm::MaybeAlign(), i64(s->ty.len));
      } else {
        emitAssignChar(a, i64(s->ty.len), g, i64(0));
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
  // Pass 2b: BASED overlays with runtime bounds (rules (13),(25)). The
  // bound expressions are evaluated at entry into the dope slots, exactly
  // like pass 2 — but nothing is allocated, since a BASED symbol has no
  // storage of its own (it stays out of localSyms). Subscripting and the
  // array built-ins then check against the live extent.
  for (Symbol* s : sema_.storage()) {
    if (s->kind != Symbol::Var || !s->ty.isDynamic() || !s->basedBase)
      continue;
    if (s->owner != p->src)
      continue;
    if (s->dynUb && !dynUb_.count(s))
      dynUb_[s] = toI64(emitExpr(s->dynUb));
    if (s->dynLb && !dynLb_.count(s))
      dynLb_[s] = toI64(emitExpr(s->dynLb));
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
      llvm::StoreInst* sp = b_.CreateStore(buf, memberAddr(s, mh.path, s->loc));
      if (packedMemberPath(s->ty, mh.path))
        sp->setAlignment(llvm::Align(1));
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

// Folded constant INITIAL/VALUE on a Fixed/Float scalar: the store must
// dominate later entry-block loads (dynamic bounds in allocaLocals pass 2),
// which only optimization hid before (unoptimized loads read garbage).
bool IRGen::constScalarInit(Symbol* s, Val& out) {
  if (!s || s->controlled || s->initCallH || !s->initExpr)
    return false;
  if (s->ty.isArray() || s->ty.isStruct() || s->ty.isChar())
    return false;
  out.ty = s->ty;
  Expr* e = s->initExpr;
  if (s->ty.k == TK::Float) {
    out.reg = flt(iniNumeric(e));
    return true;
  }
  if (s->ty.k == TK::FixedBin || s->ty.k == TK::FixedDec) {
    // Wide DECIMAL literal (ADR-191): rescale the full i128 value.
    if (needsWideDecLit(e, s->ty)) {
      out.reg = wideDecConstant(s->ty, decLitRescaled128(e, s->ty));
      return true;
    }
    long long val = e->kind == Expr::FltLit ? (long long)e->fval : e->ival;
    if (e->kind == Expr::DecLit) {
      int dq = s->ty.scale - e->decScale; // rescale to the target scale
      val = dq > 0   ? val * pliPow10(dq)
            : dq < 0 ? pliRescaleDown(val, -dq, s->ty.k == TK::FixedDec)
                     : val;
    }
    out.reg = llvm::ConstantInt::get(llvmTy(s->ty), val, true);
    return true;
  }
  return false;
}

void IRGen::emitInitials(HProc* p) {
  std::vector<HStmt*> decls;
  for (auto& st : p->body)
    collectDeclStmts(st.get(), decls);
  for (HStmt* st : decls) {
    for (auto& item : st->decls) {
      Symbol* sym = item.sym;
      // CONTROLLED INITIAL assigns per allocation (rules (15),(26),
      // SC26-3114), not at block entry: implicit/explicit/package paths emit
      // it after each pli_ctl_alloc via emitCtlInit. Skip here so the store
      // never runs against an empty stack (NULL) before allocation.
      if (sym && sym->controlled)
        continue;
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
          Val v = initValue(et, e);
          if (et.isChar()) {
            storeCharTo(p, et, v, item.loc, false);
          } else {
            storeScalarTo(p, et, v, false);
          }
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
      // Stored at alloca time already (see allocaLocals); skip the repeat.
      if (Val early; constScalarInit(item.sym, early))
        continue;
      Val v;
      v.ty = item.sym->ty;
      switch (item.sym->ty.k) {
      case TK::Float: {
        v.reg = flt(iniNumeric(e));
        break;
      }
      case TK::Bit: {
        if (item.sym->ty.len == 1) {
          bool one = e->kind == Expr::BitLit ? (!e->sval.empty() && e->sval[0] == '1')
                                             : (e->ival != 0 || e->fval != 0);
          v.reg = b_.getInt1(one);
          break;
        }
        std::vector<unsigned char> bytes = packBitInit(e, item.sym->ty.len);
        v.reg = llvm::ConstantDataArray::get(
            ctx_, llvm::ArrayRef<unsigned char>(bytes.data(), bytes.size()));
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
        // Wide DECIMAL literal (ADR-191): rescale the full i128 value.
        if (needsWideDecLit(e, item.sym->ty)) {
          v.reg = wideDecConstant(item.sym->ty, decLitRescaled128(e, item.sym->ty));
          break;
        }
        long long val = e->kind == Expr::FltLit ? (long long)e->fval : e->ival;
        if (e->kind == Expr::DecLit) {
          int dq = item.sym->ty.scale - e->decScale; // rescale to the target scale
          val = dq > 0   ? val * pliPow10(dq)
                : dq < 0 ? pliRescaleDown(val, -dq, item.sym->ty.k == TK::FixedDec)
                         : val;
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
void IRGen::collectGotoBlocks(HStmt* s, std::vector<int>& chain) {
  if (!s)
    return;
  if (!s->labels.empty() && s->kind != HStmt::Entry) {
    std::string blk = "L" + std::to_string(n_++);
    llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx_, blk, curFn_);
    for (const std::string& l : s->labels) {
      labelBlocks_[l] = bb;
      labelBeginChain_[l] = chain; // enclosing BEGIN scopes (rule 91)
    }
  }
  bool isBegin = s->kind == HStmt::Begin;
  if (isBegin) {
    s->onScope = ++nextOnScope_;
    chain.push_back(s->onScope);
  }
  if (s->thenS)
    collectGotoBlocks(s->thenS.get(), chain);
  if (s->elseS)
    collectGotoBlocks(s->elseS.get(), chain);
  for (auto& b : s->body)
    collectGotoBlocks(b.get(), chain);
  if (isBegin)
    chain.pop_back();
}

// Handler depth to restore when a GO TO leaves enclosing BEGIN blocks (rule
// (91); Y33 block termination): the depth saved at the entry of the outermost
// exited block. The target's scope chain must be a prefix of the active chain
// (a GO TO may not enter an inactive block); a target in the same scope leaves
// no block and needs no restore.
llvm::Value* IRGen::onExitDepth(const std::string& label) {
  auto it = labelBeginChain_.find(label);
  if (it == labelBeginChain_.end())
    return nullptr;
  const std::vector<int>& target = it->second;
  if (target.size() >= onScopes_.size())
    return nullptr; // same scope, or an (illegal) jump into a block
  for (size_t i = 0; i < target.size(); ++i)
    if (onScopes_[i].id != target[i])
      return nullptr; // target in a sibling/inactive block
  return onScopes_[target.size()].entryDepth;
}

// Pre-create a procedure's functions and aliases so a call site resolves
// regardless of emission order. Plain procedures get one function (filled by
// emitPlainProc); multi-entry procedures get the shared impl (filled by
// emitMultiEntryProc) plus a fully-built tail-calling thunk per entry name.
void IRGen::declareProc(HProc* p) {
  if (p->isPackage)
    return; // packages emit no function (extension, ADR-109)
  bool sret = p->isFunction && (p->retTy.isStruct() || p->retTy.isChar());
  // A structure-valued (rule 127) or character-valued (rules (34),(37))
  // function returns through a hidden result pointer and returns void: the
  // caller allocates the storage and passes its address as the first argument.
  // The shared implementation returns the single result type shared by every
  // function-valued entry point (rule 56): the procedure's own RETURNS type
  // when it is a function, else the common RETURNS type of its function ENTRYs
  // (a non-function primary entry may coexist with function segments).
  llvm::Type* implRet = (sret || p->commonRetTy.isVoid()) ? b_.getVoidTy() : llvmTy(p->commonRetTy);
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
        pt.push_back(b_.getInt64Ty()); // hidden `*` extent / CHAR(*) length args
    for (size_t i = 0; i < p->env.size(); ++i)
      pt.push_back(b_.getPtrTy()); // links
    llvm::FunctionType* ft = llvm::FunctionType::get(implRet, pt, false);
    // Rule (42): an external procedure is visible to the linker under its
    // upper-cased name; every other procedure stays module-private.
    auto linkage =
        p->isExternal ? llvm::Function::ExternalLinkage : llvm::Function::InternalLinkage;
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
      pt.push_back(b_.getInt64Ty()); // hidden `*` extent / CHAR(*) length args
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
        sig.push_back(b_.getInt64Ty()); // hidden `*` extent / CHAR(*) length args
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

  llvm::Function* t0 =
      thunk(p->paramSyms, i64(0), p->isFunction ? llvmTy(p->retTy) : b_.getVoidTy());
  t0->setName(p->irName.substr(1));
  // Rule (42): the primary entry thunk of an external procedure is the
  // link-visible symbol; the shared impl stays module-private.
  if (p->isExternal)
    t0->setLinkage(llvm::Function::ExternalLinkage);
  for (const auto& en : p->entryNames)
    aliasFor(en, t0);
  for (size_t i = 0; i < entries.size(); ++i) {
    llvm::Function* tf =
        thunk(entries[i]->entryParamSyms, i64(i + 1),
              entries[i]->entryIsFunction ? llvmTy(entries[i]->entryRetTy) : b_.getVoidTy());
    tf->setName(entryIrName(p->name, p->parent ? p->parent->name : "", entries[i]->name).substr(1));
  }
}

// Does `s` establish any handler (rule 91) at the current block level? A
// nested BEGIN scopes its own handler depth (it restores on exit), so it is
// not descended; ON-unit bodies are, because a nested ON inside a handler runs
// within this block's dynamic extent and must be undone here. The runtime's
// handler depth spans every condition (one shared stack), so any `ON` — not
// just `ON ERROR` — forces the save/restore. Procedure-precise skip: only
// blocks that can push a handler pay the entry/exit depth save/restore
// (OPTIMIZATION.md §8.1).
static bool hasOnAtLevel(HStmt* s) {
  if (!s)
    return false;
  if (s->kind == HStmt::Begin)
    return false; // a nested BEGIN restores its own entry depth
  if (s->kind == HStmt::On && !s->isSystem)
    return true;
  if (hasOnAtLevel(s->thenS.get()))
    return true;
  if (hasOnAtLevel(s->elseS.get()))
    return true;
  if (hasOnAtLevel(s->unit.get()))
    return true; // an ON in this unit runs within this block
  for (auto& b : s->body)
    if (hasOnAtLevel(b.get()))
      return true;
  return false;
}

// True if any top-level statement of `p` establishes a handler, so the
// procedure must save/restore the handler depth at entry/exit.
static bool procEstablishesOn(HProc* p) {
  for (auto& st : p->body)
    if (hasOnAtLevel(st.get()))
      return true;
  return false;
}

void IRGen::emitProc(HProc* p) {
  if (p->isPackage)
    return; // packages emit no function (extension, ADR-109)
  curProc_ = p;
  labelBlocks_.clear();
  labelBeginChain_.clear();
  onScopes_.clear();
  symAddr_.clear();
  structRetPtr_ = nullptr;
  ctlImplicitAlloc_.clear();
  ctlAddrHoist_.clear();
  ctlLenHoist_.clear();
  ctlExtHoist_.clear();
  // Re-seed globals: static storage resolves the same in every procedure.
  for (Symbol* s : sema_.storage())
    if (s->isStatic && s->kind == Symbol::Var)
      symAddr_[s] = mod_.getGlobalVariable(s->irName.substr(1), true);

  llvm::Type* retLLVM = (p->isFunction && (p->retTy.isStruct() || p->retTy.isChar()))
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
  std::vector<int> beginChain;
  for (auto& st : p->body)
    collectGotoBlocks(st.get(), beginChain);

  // Parameter arguments become their symbols' addresses (PL/I by reference);
  // each `*`-extent parameter (rule 13) then reads its hidden i64 extent into a
  // dope slot; the trailing args are the static links (rule (8)).
  size_t ai = 0;
  if (p->isFunction && (p->retTy.isStruct() || p->retTy.isChar()))
    structRetPtr_ = fn->getArg(ai++); // hidden result pointer (rules 127, (34))
  for (Symbol* s : p->paramSyms)
    symAddr_[s] = fn->getArg(ai++);
  std::vector<std::pair<Symbol*, llvm::Value*>> ctlHid;
  for (Symbol* s : p->paramSyms)
    if (isAdjustable(s)) {
      // A `*`-extent array reads its hidden extent into dynUb_; a `CHAR(*)`
      // parameter reads its hidden length into dynLen_ (rules (13),(18)).
      // A CONTROLLED `CHAR(*)` dummy takes the actual's slot key instead
      // (rules (15),(18)); its alias binds once the entry block is live.
      llvm::Value* hv = fn->getArg(ai++);
      if (s->controlled && isStarLen(s->ty))
        ctlHid.push_back({s, hv});
      else if (isStarLen(s->ty))
        dynLen_[s] = hv;
      else
        dynUb_[s] = hv;
    }
  for (size_t i = 0; i < p->env.size(); ++i)
    symAddr_[p->env[i]] = fn->getArg(ai++);

  b_.SetInsertPoint(entry);

  // CONTROLLED dummy association (rule (15)): -1 (non-controlled actual)
  // keeps the dummy's own slot; any other value is the caller's key.
  for (auto& [s, hv] : ctlHid)
    ctlKey_[s] = b_.CreateSelect(b_.CreateICmpEQ(hv, i64(-1), "ctlmine"),
                                 i64((long long)s->ctlSlot), hv, "ctlkey");

  allocaLocals(p);
  emitInitials(p); // INITIAL attribute on AUTOMATIC variables (rule 26)
  recordDynParamUbs(p->paramSyms);

  // Implicit ALLOCATE for every CONTROLLED variable declared in this proc:
  // each gets a default-sized generation so first-use references work without
  // an explicit ALLOCATE statement (IBM Enterprise PL/I). CONTROLLED vars are
  // excluded from localSyms (no own frame storage), so they are found by
  // scanning all storage and filtering to this proc's declarations. INITIAL
  // assigns into the fresh generation per allocation (SC26-3114).
  for (Symbol* sym : sema_.storage())
    if (sym->controlled && !sym->isStatic && sym->owner == p->src) {
      ensureCtlAlloc(sym);
      emitCtlInit(sym, sym->loc);
    }
  // Package CONTROLLED generations are program-lifetime: ensure one exists
  // without ever popping per proc (extension, ADR-109).
  emitPackageCtlEnsure();

  // Rule (91): save the handler depth so procedure exit restores the
  // caller's establishment state (the runtime depth spans all conditions).
  // Skipped when this procedure establishes no handler (procedure-precise,
  // OPTIMIZATION.md §8.1): a procedure that cannot push onto the handler
  // stack needs no restore.
  curOnDepth_ = nullptr;
  if (procEstablishesOn(p)) {
    curOnDepth_ = entryAlloca(b_.getInt64Ty(), "ondepth");
    b_.CreateStore(b_.CreateCall(runtimeFn("pli_on_depth_error"), {}), curOnDepth_);
  }

  for (auto& st : p->body)
    emitStmt(st.get());

  if (!blockTerminated(b_.GetInsertBlock())) {
    emitCtlEpilogue();
    if (curOnDepth_)
      b_.CreateCall(runtimeFn("pli_on_reset_error"),
                    {b_.CreateLoad(b_.getInt64Ty(), curOnDepth_, "ondepth")});
    if (p->isFunction && (p->retTy.isStruct() || p->retTy.isChar()))
      b_.CreateRetVoid(); // hidden-buffer result already written by RETURN
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
  std::vector<int> beginChain;
  for (auto& st : p->body)
    collectGotoBlocks(st.get(), beginChain);

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
  std::vector<std::pair<Symbol*, llvm::Value*>> ctlHid;
  for (Symbol* s : uni)
    if (isAdjustable(s)) {
      // Hidden args as in emitProc: extent, `CHAR(*)` length, or the
      // CONTROLLED `CHAR(*)` dummy's slot key (rules (13),(15),(18)).
      llvm::Value* hv = impl->getArg(ai++);
      if (s->controlled && isStarLen(s->ty))
        ctlHid.push_back({s, hv});
      else if (isStarLen(s->ty))
        dynLen_[s] = hv;
      else
        dynUb_[s] = hv;
    }
  for (size_t i = 0; i < p->env.size(); ++i)
    symAddr_[p->env[i]] = impl->getArg(ai++);
  llvm::Value* sel = impl->getArg(ai++);

  b_.SetInsertPoint(entry);

  // CONTROLLED dummy association (rule (15)), as in emitProc.
  for (auto& [s, hv] : ctlHid)
    ctlKey_[s] = b_.CreateSelect(b_.CreateICmpEQ(hv, i64(-1), "ctlmine"),
                                 i64((long long)s->ctlSlot), hv, "ctlkey");

  allocaLocals(p);
  emitInitials(p);
  recordDynParamUbs(uni);

  // Implicit ALLOCATE for every CONTROLLED variable declared in this proc,
  // with INITIAL assigned per allocation (SC26-3114).
  for (Symbol* sym : sema_.storage())
    if (sym->controlled && !sym->isStatic && sym->owner == p->src) {
      ensureCtlAlloc(sym);
      emitCtlInit(sym, sym->loc);
    }
  // Package CONTROLLED generations are program-lifetime (ADR-109).
  emitPackageCtlEnsure();

  // Rule (91): save the handler depth for exit restore (see retPads).
  curOnDepth_ = nullptr;
  if (procEstablishesOn(p)) {
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
    emitCtlEpilogue();
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
    // Every handler takes its capture context (rule 91): the establishing-frame
    // addresses of automatics the unit touches, or NULL when it touches none.
    llvm::FunctionType* ft = llvm::FunctionType::get(b_.getVoidTy(), {b_.getPtrTy()}, false);
    std::string name;
    if (keyId.first == 0)
      name = "PLI_ON_" + std::to_string(keyId.second);
    else if (keyId.first == Stmt::kSizeCondKey)
      name = "PLI_ON_SIZE_" + std::to_string(keyId.second);
    else if (keyId.first == Stmt::kSubscriptrangeCondKey)
      name = "PLI_ON_SUBSCRIPT_" + std::to_string(keyId.second);
    else if (keyId.first == Stmt::kZerodivideCondKey)
      name = "PLI_ON_ZERODIVIDE_" + std::to_string(keyId.second);
    else if (keyId.first == Stmt::kConversionCondKey)
      name = "PLI_ON_CONVERSION_" + std::to_string(keyId.second);
    else
      name = "PLI_ONC_" + std::to_string(keyId.first) + "_" + std::to_string(keyId.second);
    onHandlers_[keyId.first].push_back(
        llvm::Function::Create(ft, llvm::Function::InternalLinkage, name, &mod_));
  }
}

// Fill every handler function body. A handler runs without the establishing
// frame; automatics the unit touches (collected by sema) arrive through the
// capture-context argument, so only globals are seeded directly; labels
// inside the unit get their own blocks.
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
      labelBeginChain_.clear();
      onScopes_.clear();
      symAddr_.clear();
      ctlImplicitAlloc_.clear(); // a handler allocates/frees nothing implicitly
      ctlAddrHoist_.clear();
      ctlLenHoist_.clear();
      ctlExtHoist_.clear();
      areaLocals_.clear(); // ... and owns no AREA regions
      for (Symbol* gs : sema_.storage())
        if (gs->isStatic && gs->kind == Symbol::Var)
          symAddr_[gs] = mod_.getGlobalVariable(gs->irName.substr(1), true);
      llvm::BasicBlock* entry = llvm::BasicBlock::Create(ctx_, "entry", fn);
      b_.SetInsertPoint(entry);
      // Bind the capture context: the i-th slot holds the address of the i-th
      // collected automatic, in the same order establishment stored them.
      if (!s->onCaps.empty()) {
        llvm::Value* ctxArg = fn->getArg(0);
        std::vector<llvm::Type*> fields(s->onCaps.size(), b_.getPtrTy());
        llvm::StructType* capTy = llvm::StructType::get(ctx_, fields);
        for (size_t i = 0; i < s->onCaps.size(); ++i) {
          llvm::Value* fp = b_.CreateStructGEP(capTy, ctxArg, (unsigned)i, "capf");
          symAddr_[s->onCaps[i]] = b_.CreateLoad(b_.getPtrTy(), fp, "capv");
        }
      }
      std::vector<int> beginChain;
      collectGotoBlocks(s->unit.get(), beginChain);
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

// Establish a handler: push its id (0 for the system action) plus the handler
// address and the capture context for automatics the unit touches (rule 91).
// The address/context pair is what lets a SIGNAL raised anywhere — including
// another object file — run this module's handler against this frame.
void IRGen::emitOn(HStmt* s) {
  long long id = s->isSystem ? 0 : s->onIndex;
  llvm::Value* fnv = llvm::Constant::getNullValue(b_.getPtrTy());
  llvm::Value* ctx = llvm::Constant::getNullValue(b_.getPtrTy());
  if (!s->isSystem) {
    llvm::Function* fn = onHandlers_[s->condKey][(size_t)s->onIndex - 1];
    fnv = b_.CreateBitCast(fn, b_.getPtrTy(), "onfn");
    if (!s->onCaps.empty()) {
      std::vector<llvm::Type*> fields(s->onCaps.size(), b_.getPtrTy());
      llvm::StructType* capTy = llvm::StructType::get(ctx_, fields);
      llvm::Value* cap = entryAlloca(capTy, "oncap");
      for (size_t i = 0; i < s->onCaps.size(); ++i) {
        llvm::Value* fp = b_.CreateStructGEP(capTy, cap, (unsigned)i, "capf");
        b_.CreateStore(addressOf(s->onCaps[i]), fp);
      }
      ctx = cap;
    }
  }
  if (s->condKey == 0)
    b_.CreateCall(runtimeFn("pli_on_push_error"), {i64(id), fnv, ctx});
  else
    b_.CreateCall(runtimeFn("pli_on_push_cond"), {i64(s->condKey), i64(id), fnv, ctx});
}

// Raise a condition: without an established handler take the system action
// (abort); otherwise run the topmost handler for that condition, then resume
// after the SIGNAL. The handler address and its capture context come from the
// runtime stack — not from this module's handler list — so a SIGNAL raised
// here runs a unit established anywhere, including another object file
// (rules (91)-(94)). ERROR resets ONCODE around its handlers; a SIGNAL ...
// SET ONCODE(expr) stores its code first so the unit observes it.
void IRGen::emitSignal(HStmt* s) {
  if (s->oncodeExpr) {
    Val code = emitExpr(s->oncodeExpr.get());
    Val c = convert(code, Type::fixedBin(31, 0), s->loc);
    b_.CreateCall(runtimeFn("pli_set_oncode"), {c.reg});
  }
  llvm::Value* fn = b_.CreateCall(runtimeFn("pli_on_top_fn"), {i64(s->condKey)}, "ontopfn");
  llvm::Value* ctx = b_.CreateCall(runtimeFn("pli_on_top_ctx"), {i64(s->condKey)}, "ontopctx");
  llvm::Value* none =
      b_.CreateICmpEQ(fn, llvm::Constant::getNullValue(b_.getPtrTy()), "onnosystem");
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
  else if (s->condKey == Stmt::kConversionCondKey)
    msg = "SIGNAL CONVERSION";
  else
    msg = "SIGNAL CONDITION(" + s->condName + ")";
  b_.CreateCall(runtimeFn("pli_signal_error"), {globalString(msg)});
  b_.CreateUnreachable();
  b_.SetInsertPoint(dspBB);
  if (s->condKey == 0)
    b_.CreateCall(runtimeFn("pli_set_oncode"), {i32(1)});
  llvm::FunctionType* hft = llvm::FunctionType::get(b_.getVoidTy(), {b_.getPtrTy()}, false);
  b_.CreateCall(hft, fn, {ctx});
  if (s->condKey == 0)
    b_.CreateCall(runtimeFn("pli_set_oncode"), {i32(0)});
  b_.CreateBr(resBB);
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
  // Condition enable-state (rules (60)-(63), ADR-110/112): each statement
  // sees its own disables OR-inherited through enclosing statements, so a
  // prefixed group covers its body. Balanced by construction (single exit).
  CheckState top;
  if (!checkStack_.empty())
    top = checkStack_.back();
  top.noSize = top.noSize || s->noSize;
  top.noSub = top.noSub || s->noSub;
  top.noZdiv = top.noZdiv || s->noZdiv;
  top.noConv = top.noConv || s->noConv;
  checkStack_.push_back(top);
  switch (s->kind) {
  case HStmt::Null:
  case HStmt::Declare:
  case HStmt::Entry:  // segment marker; handled by emitMultiEntryProc
  case HStmt::Format: // remote repository only; R spliced in sema, no code
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
    // depth when the block exits. Skipped when the block itself establishes
    // no handler (procedure-precise, OPTIMIZATION.md §8.1), so a block
    // that cannot push onto the handler stack stays free. When the body ends
    // with a terminator (e.g. a bare RETURN ending an ON-unit, rule 91) there
    // is no fall-through to scope: skip the restore, mirroring procedure
    // exit. The entry depth is also what a GO TO leaving this block restores.
    llvm::Value* blkDepth = nullptr;
    for (auto& b : s->body)
      if (hasOnAtLevel(b.get())) {
        blkDepth = b_.CreateCall(runtimeFn("pli_on_depth_error"), {}, "onblkdepth");
        break;
      }
    onScopes_.push_back({s->onScope, blkDepth});
    for (auto& b : s->body)
      emitStmt(b.get());
    onScopes_.pop_back();
    if (blkDepth && !blockTerminated(b_.GetInsertBlock()))
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
      if (v.ty.k == TK::FixedDec && v.ty.scale > 0) {
        if (v.ty.intBits() == 128) // ADR-191: >18 digit DECIMAL needs __int128 I/O
          b_.CreateCall(runtimeFn("pli_display_decfixed128"), {v.reg, i64(v.ty.scale)});
        else
          b_.CreateCall(runtimeFn("pli_display_decfixed"), {toI64(v), i64(v.ty.scale)});
      } else if (v.ty.k == TK::FixedDec && v.ty.intBits() == 128) {
        b_.CreateCall(runtimeFn("pli_display_decfixed128"), {v.reg, i64(0)});
      } else {
        b_.CreateCall(runtimeFn("pli_display_fixed"), {toI64(v)});
      }
      break;
    case TK::Pointer:
      d_.error(s->loc, "a POINTER value cannot be written with DISPLAY in this stage", "(114)");
      break;
    case TK::Complex:
      b_.CreateCall(runtimeFn("pli_display_complex"), {b_.CreateExtractValue(v.cpx, 0, "cpx.re"),
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
  case HStmt::Wait:
    emitWait(s);
    break;
  case HStmt::Delay:
    emitDelay(s);
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
    if (!s->value) {
      // A bare RETURN ends an ON-unit and resumes after the SIGNAL (rule
      // 91); sema rejects it in procedure bodies, so a handler (whose
      // hidden result pointers are null) is the only path that gets here.
      emitCtlEpilogue();
      b_.CreateRetVoid();
    } else if (curProc_->isFunction && curProc_->retTy.isStruct()) {
      // Structure-valued function (rule 127): copy the returned structure's
      // storage into the caller's result buffer, then return void.
      Val v = emitExpr(s->value.get());
      llvm::Value* sz = i64(mod_.getDataLayout().getTypeStoreSize(llvmTy(curProc_->retTy)));
      b_.CreateMemCpy(structRetPtr_, llvm::MaybeAlign(), v.ptr, llvm::MaybeAlign(), sz);
      emitCtlEpilogue();
      b_.CreateRetVoid();
    } else if (curProc_->isFunction && curProc_->retTy.isChar()) {
      // Character-valued function (rules (34),(37)): copy the value into the
      // caller's hidden buffer, blank-padding or truncating (rule (86)). A
      // `CHAR(*) VARYING` result uses the caller-sized max, never the static
      // placeholder length.
      Val v = emitExpr(s->value.get());
      const Type& rty = curProc_->retTy;
      if (rty.starLen && rty.varying) {
        llvm::Value* dp = b_.CreateStructGEP(sretBufTy(rty), structRetPtr_, 1, "rdata");
        llvm::Value* ln = emitAssignVarying(dp, i64(kStarRetMax), v.ptr, v.len);
        llvm::Value* lp = b_.CreateStructGEP(sretBufTy(rty), structRetPtr_, 0, "rlenp");
        b_.CreateStore(b_.CreateTrunc(ln, b_.getInt32Ty(), "rl32"), lp);
      } else if (rty.starLen) {
        emitAssignChar(structRetPtr_, i64(kStarRetMax), v.ptr, v.len);
      } else {
        storeCharTo(structRetPtr_, rty, v, s->loc);
      }
      emitCtlEpilogue();
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
      emitCtlEpilogue();
      b_.CreateRet(reg);
    } else {
      emitCtlEpilogue();
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
    // Rule (91) / Y33 block termination: a GO TO that leaves enclosing BEGIN
    // blocks reverts the handlers those blocks established, restoring the
    // depth saved at the outermost exited block's entry.
    if (llvm::Value* depth = onExitDepth(s->name))
      b_.CreateCall(runtimeFn("pli_on_reset_error"), {depth});
    b_.CreateBr(labelBlocks_[s->name]);
    break;
  }
  checkStack_.pop_back();
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
          llvm::Value *ub = nullptr, *lb = nullptr;
          llvm::Value* base = arr.isDynamic()
                                  ? dynamicMemberBase(t->sym, t->memberPath, s->loc, ub, lb)
                                  : memberAddr(t->sym, t->memberPath, s->loc);
          llvm::Value* addr = arrayElementAddr(arr, base, t->args, s->loc, ub, lb);
          bool packed = packedMemberPath(t->sym->ty, t->memberPath);
          if (el.isChar())
            storeCharTo(addr, el, v, s->loc, packed);
          else
            storeScalarTo(addr, el, convert(v, el, s->loc), packed);
          return;
        }
        storeArrayElement(t->sym, t->args, v, s->loc, t->locPtr.get());
        return;
      }
      if (t->kind == HExpr::VarRef && t->sym) {
        if (!t->memberPath.empty()) {
          const Type& leaf = t->ty;
          // A locator-qualified target P->X.FIELD stores off the loaded pointer.
          llvm::Value* addr =
              t->locPtr ? locatorMemberAddr(t->sym, t->memberPath, emitExpr(t->locPtr.get()).reg)
                        : memberAddr(t->sym, t->memberPath, s->loc);
          bool packed = packedMemberPath(t->sym->ty, t->memberPath);
          if (leaf.isChar())
            storeCharTo(addr, leaf, v, s->loc, packed);
          else
            storeScalarTo(addr, leaf, convert(v, leaf, s->loc), packed);
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
  // An arrayed structure target matches its elements pairwise (rule 86).
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
    if (t->ty.isArray() || v->ty.isArray()) {
      if (!t->ty.isArray() || !v->ty.isArray() || arrayExtent(t->ty) != arrayExtent(v->ty)) {
        d_.error(s->loc, "BY NAME assignment between arrayed structures needs matching extents",
                 "(86)");
        return;
      }
      llvm::Type* dat = llvmTy(t->ty);
      llvm::Type* sat = llvmTy(v->ty);
      const Type& elD = t->ty.elementType();
      const Type& elS = v->ty.elementType();
      for (long long e = 0; e < arrayExtent(t->ty); ++e) {
        llvm::Value* dEl = b_.CreateInBoundsGEP(dat, dst, {i64(0), i64(e)}, "bnm.dae");
        llvm::Value* sEl = b_.CreateInBoundsGEP(sat, src, {i64(0), i64(e)}, "bnm.sae");
        emitByNameCopy(dEl, sEl, elD, elS, s->loc, nullptr, {}, nullptr, {});
      }
      return;
    }
    emitByNameCopy(dst, src, t->ty, v->ty, s->loc, t->sym, t->memberPath, v->sym, v->memberPath);
    return;
  }
  if (s->target->kind == HExpr::Call && s->target->name == "SUBSTR") {
    HExpr* t = s->target.get();
    Val sv = emitExpr(t->args[0].get());
    Symbol* sym = t->args[0]->sym;
    Val start = emitExpr(t->args[1].get());
    Val len = emitExpr(t->args[2].get());
    Val rhs = emitExpr(s->value.get());
    // The write region clips to the base capacity: static for fixed and
    // VARYING strings, live for adjustable `CHAR(*)` (rule (18)) and
    // CONTROLLED generations (rule (15)).
    llvm::Value* cap = sv.len;
    if (sym && sym->ty.isChar()) {
      if (llvm::Value* live = adjustLen(sym, sym->ty))
        cap = live;
      else
        cap = i64(sym->ty.len);
    }
    // A VARYING target grows its live length when the overlay reaches past
    // it (rule (86)); fixed targets keep the in-place overwrite below.
    HExpr* base = t->args[0].get();
    if (base->kind == HExpr::VarRef && sym && sym->ty.isChar() && sym->ty.varying &&
        base->memberPath.empty()) {
      llvm::Value* stor = addressOf(sym);
      llvm::Value* lp = b_.CreateStructGEP(llvmTy(sym->ty), stor, 0, "vlenp");
      llvm::Value* dp = b_.CreateStructGEP(llvmTy(sym->ty), stor, 1, "vdata");
      emitSubstrAssignVarying(dp, cap, lp, toI64(start), toI64(len), rhs.ptr, rhs.len);
      return;
    }
    emitSubstrAssign(sv.ptr, cap, toI64(start), toI64(len), rhs.ptr, rhs.len);
    return;
  }
  // Slice assignment (rule 126): a cross-section A(i, *, ...) on either side
  // denotes a reduced-rank view of an array. Copy element-wise between the two
  // views (a whole array is the all-'*' case); a scalar value broadcasts. The
  // value's bare-element case still rejects a '*' via emitExpr.
  {
    auto isCross = [](HExpr* e) {
      if (e->kind != HExpr::Subscript)
        return false;
      for (auto& a : e->args)
        if (a->kind == HExpr::Star)
          return true;
      return false;
    };
    if (isCross(s->target.get()) || isCross(s->value.get())) {
      emitCrossSectionAssign(s->target.get(), s->value.get(), s->loc);
      return;
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
        bool packed = packedMemberPath(t->sym->ty.elementType(), t->memberPath);
        if (el.isChar())
          storeCharTo(addr, el, v, s->loc, packed);
        else
          storeScalarTo(addr, el, convert(v, el, s->loc), packed);
        return;
      }
      // A subscripted member array S.A(i) = e (rules 124,126): store through
      // the member array field, bounds-checked like any array element.
      const Type& arr = memberType(t->sym, t->memberPath);
      const Type& el = t->ty;
      llvm::Value *ub = nullptr, *lb = nullptr;
      llvm::Value* base = arr.isDynamic() ? dynamicMemberBase(t->sym, t->memberPath, s->loc, ub, lb)
                                          : memberAddr(t->sym, t->memberPath, s->loc);
      llvm::Value* addr = arrayElementAddr(arr, base, t->args, s->loc, ub, lb);
      bool packed = packedMemberPath(t->sym->ty, t->memberPath);
      if (el.isChar())
        storeCharTo(addr, el, v, s->loc, packed);
      else
        storeScalarTo(addr, el, convert(v, el, s->loc), packed);
      return;
    }
    // An iSUB-DEFINED array target Y(k) (rule 134) has no storage of its own:
    // store into the live base element X(...k...).
    if (t->sym->definedBase && t->sym->definedIsubAxis >= 0) {
      llvm::Value* addr = definedSubElementAddr(t->sym, t->args, s->loc);
      storeScalarTo(addr, t->ty, convert(v, t->ty, s->loc));
      return;
    }
    storeArrayElement(t->sym, t->args, v, s->loc, t->locPtr.get());
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
    // A locator-qualified whole-structure target P->X stores
    // through the locator value, mirroring the member load path.
    llvm::Value* dst;
    if (t->locPtr)
      dst = t->memberPath.empty()
                ? emitExpr(t->locPtr.get()).reg
                : locatorMemberAddr(t->sym, t->memberPath, emitExpr(t->locPtr.get()).reg);
    else
      dst = t->memberPath.empty() ? addressOf(t->sym) : memberAddr(t->sym, t->memberPath, s->loc);
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
    auto fieldAddr = [&](llvm::Value* base, const Type& root, const std::vector<unsigned>& rel) {
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
    for (const auto& rel : dynPaths) {
      llvm::LoadInst* lp = b_.CreateLoad(b_.getPtrTy(), fieldAddr(dst, t->ty, rel), "dcp.sv");
      if (packedMemberPath(t->ty, rel))
        lp->setAlignment(llvm::Align(1));
      savedPtrs.push_back(lp);
    }
    b_.CreateMemCpy(dst, llvm::MaybeAlign(), v.ptr, llvm::MaybeAlign(), sz);
    for (size_t i = 0; i < dynPaths.size(); ++i) {
      llvm::StoreInst* sp = b_.CreateStore(savedPtrs[i], fieldAddr(dst, t->ty, dynPaths[i]));
      if (packedMemberPath(t->ty, dynPaths[i]))
        sp->setAlignment(llvm::Align(1));
    }
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
      llvm::LoadInst* dbuf = b_.CreateLoad(b_.getPtrTy(), fieldAddr(dst, t->ty, rel), "dcp.dp");
      llvm::LoadInst* sbuf = b_.CreateLoad(b_.getPtrTy(), fieldAddr(v.ptr, sv->ty, rel), "dcp.sp");
      if (packedMemberPath(t->ty, rel))
        dbuf->setAlignment(llvm::Align(1));
      if (packedMemberPath(sv->ty, rel))
        sbuf->setAlignment(llvm::Align(1));
      llvm::Value* nbytes =
          b_.CreateMul(sext, i64(mod_.getDataLayout().getTypeStoreSize(llvmTy(el))), "dcp.n");
      b_.CreateMemCpy(dbuf, llvm::MaybeAlign(), sbuf, llvm::MaybeAlign(), nbytes);
    }
    return;
  }
  // Qualified member assignment: S.A = e (rule 124). A locator-qualified
  // whole reference P->X (empty member path) stores through the locator too.
  if (s->target->kind == HExpr::VarRef && s->target->sym &&
      (!s->target->memberPath.empty() || s->target->locPtr)) {
    HExpr* t = s->target.get();
    const Type& leaf = t->ty;
    Val v = emitExpr(s->value.get());
    // A locator-qualified target P->X.FIELD stores off the loaded
    // locator value; otherwise off the based/symbol member address.
    llvm::Value* addr =
        t->locPtr ? locatorMemberAddr(t->sym, t->memberPath, emitExpr(t->locPtr.get()).reg)
                  : memberAddr(t->sym, t->memberPath, s->loc);
    bool packed = packedMemberPath(t->sym->ty, t->memberPath);
    if (leaf.isChar())
      storeCharTo(addr, leaf, v, s->loc, packed);
    else
      storeScalarTo(addr, leaf, convert(v, leaf, s->loc), packed);
    return;
  }
  if (s->target->kind != HExpr::VarRef || !s->target->sym)
    return;
  Val v = emitExpr(s->value.get());
  storeTo(s->target->sym, v, s->loc);
}

// Implicit ALLOCATE for CONTROLLED variables (IBM Enterprise PL/I): push a
// generation sized to the compile-time descriptor if this symbol has not been
// implicitly allocated in the current procedure yet. CHAR(*) is skipped — its
// runtime length cannot be known without an explicit ALLOCATE statement. A
// `(*)` CONTROLLED numeric array is skipped the same way — its extents come
// from each ALLOCATE (rules (13),(89)).
// True when a CONTROLLED array declares a non-`*` runtime upper bound `A(n)`:
// the extent is evaluated at allocation time and recorded per generation,
// unlike a `(*)` axis that only an explicit ALLOCATE can size.
bool IRGen::ctlDeclaredRuntimeBound(Symbol* sym) {
  if (!sym || !sym->controlled || !sym->ty.isArray() || !sym->ty.isDynamic())
    return false;
  for (const auto& d : sym->ty.dims)
    if ((d.dyn || d.lbDyn) && !d.adj)
      return true;
  return false;
}

// Push a CONTROLLED generation sized from the DECLARE bounds (rules (13),(89)):
// a runtime first-axis upper bound is evaluated here; later axes are static.
void IRGen::ctlAllocDeclared(Symbol* sym, SourceLoc loc) {
  const Type& arr = sym->ty;
  size_t rank = arr.dims.size();
  const Type& el = arr.elementType();
  long long elemSz = (long long)mod_.getDataLayout().getTypeAllocSize(llvmTy(el)).getFixedValue();
  if (elemSz <= 0)
    elemSz = 1;
  std::vector<llvm::Value*> exts(rank);
  for (size_t k = 0; k < rank; ++k) {
    const Dim& d = arr.dims[k];
    if (k == 0 && (d.dyn || d.lbDyn)) {
      llvm::Value* ub = sym->dynUb ? toI64(emitExpr(sym->dynUb)) : i64(d.ub);
      llvm::Value* lb = sym->dynLb ? toI64(emitExpr(sym->dynLb)) : i64(d.lb);
      exts[k] = b_.CreateAdd(b_.CreateSub(ub, lb, "ctld"), i64(1), "ctlde");
    } else {
      exts[k] = i64((long long)d.ub - (long long)d.lb + 1);
    }
  }
  llvm::Value* total = i64(elemSz);
  for (size_t k = 0; k < rank; ++k)
    total = b_.CreateMul(total, exts[k], "ctldtot");
  if (rank == 1) {
    b_.CreateCall(runtimeFn("pli_ctl_alloc"), {ctlKeyOf(sym), total});
  } else {
    b_.CreateCall(runtimeFn("pli_ctl_alloc_dims"), {ctlKeyOf(sym), total, i64((long long)rank)});
    for (size_t k = 0; k < rank; ++k)
      b_.CreateCall(runtimeFn("pli_ctl_set_dim"), {ctlKeyOf(sym), i64((long long)k), exts[k]});
  }
  (void)loc;
}

void IRGen::ensureCtlAlloc(Symbol* sym, SourceLoc loc) {
  // Only skip CHAR(*) adjustable-length types and (*) CONTROLLED dynamic
  // arrays; everything else gets default-sized implicit allocation.
  if (sym->ty.isChar() && sym->ty.starLen && !sym->dynLenExpr)
    return;
  if (sym->controlled && sym->ty.isArray() && sym->ty.isDynamic()) {
    // A `(*)` axis is ALLOCATE-supplied (no implicit generation); a non-`*`
    // runtime bound `A(n)` sizes the implicit generation at block entry.
    if (!ctlDeclaredRuntimeBound(sym))
      return;
    if (ctlImplicitAlloc_.count(sym))
      return;
    ctlImplicitAlloc_.insert(sym);
    ctlAllocDeclared(sym, loc);
    return;
  }
  if (ctlImplicitAlloc_.count(sym))
    return;
  ctlImplicitAlloc_.insert(sym);
  llvm::Value* sz = i64(mod_.getDataLayout().getTypeAllocSize(llvmTy(sym->ty)).getFixedValue());
  if (sym->ty.isChar() && sym->ty.starLen && sym->dynLenExpr)
    sz = toI64(emitExpr(sym->dynLenExpr), loc);
  // Fixed N-D numeric arrays keep per-generation live extents: record the
  // descriptor extents for the implicit generation (rules (13),(89)).
  bool ndDims = isCtlNDynArray(sym) && !sym->ty.isDynamic();
  if (ndDims) {
    size_t rank = sym->ty.dims.size();
    b_.CreateCall(runtimeFn("pli_ctl_alloc_dims"), {ctlKeyOf(sym), sz, i64((long long)rank)});
    emitCtlNDDescDims(sym);
  } else {
    b_.CreateCall(runtimeFn("pli_ctl_alloc"), {ctlKeyOf(sym), sz});
  }
}

// INITIAL on a CONTROLLED generation (rules (15),(26), SC26-3114): assign the
// declared initial value into the just-allocated generation. A no-INITIAL
// symbol is a no-op; a structure zeroes first so per-member INITIAL
// zero-fill slots (nullptr) keep zero-initialised storage like AUTOMATIC.
void IRGen::emitCtlInit(Symbol* sym, SourceLoc loc) {
  if (!sym || !sym->controlled)
    return;
  if (!sym->initElems.empty() && sym->ty.isStruct()) {
    llvm::Value* base = addressOf(sym);
    b_.CreateStore(llvm::ConstantAggregateZero::get(llvmTy(sym->ty)), base);
    size_t idx = 0;
    emitStructInitValues(base, sym->ty, sym->initElems, idx, loc);
    return;
  }
  if (!sym->initElems.empty() && sym->ty.isArray() && !sym->ty.isDynamic()) {
    const Type& et = sym->ty.elementType();
    llvm::Type* aty = llvm::ArrayType::get(llvmTy(et), (unsigned)arrayExtent(sym->ty));
    llvm::Value* base = addressOf(sym);
    int i = 0;
    for (Expr* e : sym->initElems) {
      llvm::Value* idx = i32(i++);
      llvm::Value* p = b_.CreateGEP(aty, base, {i32(0), idx}, "ctl.init.el");
      storeScalarTo(p, et, initValue(et, e));
    }
    return;
  }
  if (sym->initExpr) {
    Expr* e = sym->initExpr;
    Val v;
    v.ty = sym->ty;
    switch (sym->ty.k) {
    case TK::Float:
      v.reg = flt(iniNumeric(e));
      break;
    case TK::Bit: {
      if (sym->ty.len == 1) {
        bool one = e->kind == Expr::BitLit ? (!e->sval.empty() && e->sval[0] == '1')
                                           : (e->ival != 0 || e->fval != 0);
        v.reg = b_.getInt1(one);
        break;
      }
      std::vector<unsigned char> bytes = packBitInit(e, sym->ty.len);
      v.reg = llvm::ConstantDataArray::get(
          ctx_, llvm::ArrayRef<unsigned char>(bytes.data(), bytes.size()));
      break;
    }
    case TK::Char: {
      Val cv;
      cv.ty = sym->ty;
      cv.ptr = globalString(e->sval);
      cv.len = i64(e->sval.size());
      storeTo(sym, cv, loc);
      return;
    }
    default: {
      // Wide DECIMAL literal (ADR-191): rescale the full i128 value.
      if (needsWideDecLit(e, sym->ty)) {
        v.reg = wideDecConstant(sym->ty, decLitRescaled128(e, sym->ty));
        break;
      }
      long long val = e->kind == Expr::FltLit ? (long long)e->fval : e->ival;
      if (e->kind == Expr::DecLit) {
        int dq = sym->ty.scale - e->decScale;
        val = dq > 0   ? val * pliPow10(dq)
              : dq < 0 ? pliRescaleDown(val, -dq, sym->ty.k == TK::FixedDec)
                       : val;
      }
      v.reg = llvm::ConstantInt::get(llvmTy(sym->ty), val, true);
      break;
    }
    }
    storeTo(sym, v, loc);
    return;
  }
  if (sym->initCallH) {
    Val v = emitExpr(sym->initCallH);
    storeTo(sym, v, loc);
    return;
  }
}

// Package CONTROLLED generations (extension, ADR-109): program-lifetime
// storage shared by member procedures. Ensure one generation exists (alloc +
// INITIAL on first entry, reuse after) without ever popping per proc.
void IRGen::emitPackageCtlEnsure() {
  for (Symbol* sym : sema_.storage()) {
    if (!sym->controlled || sym->kind != Symbol::Var)
      continue;
    if (!sym->owner || !sym->owner->isPackage)
      continue;
    if (sym->ty.isChar() && sym->ty.starLen && !sym->dynLenExpr)
      continue;
    if (sym->ty.isArray() && sym->ty.isDynamic())
      continue;
    llvm::Value* addr = b_.CreateCall(runtimeFn("pli_ctl_addr"), {ctlKeyOf(sym)}, "pkgctl");
    llvm::Value* isnull =
        b_.CreateICmpEQ(addr, llvm::Constant::getNullValue(addr->getType()), "pkgctlnull");
    llvm::BasicBlock* needBB = llvm::BasicBlock::Create(ctx_, "pkgctl.need", curFn_);
    llvm::BasicBlock* haveBB = llvm::BasicBlock::Create(ctx_, "pkgctl.have", curFn_);
    b_.CreateCondBr(isnull, needBB, haveBB);
    b_.SetInsertPoint(needBB);
    llvm::Value* sz = i64(mod_.getDataLayout().getTypeAllocSize(llvmTy(sym->ty)).getFixedValue());
    if (sym->ty.isChar() && sym->ty.starLen && sym->dynLenExpr)
      sz = toI64(emitExpr(sym->dynLenExpr), sym->loc);
    // Fixed N-D numeric arrays keep per-generation live extents: record the
    // descriptor extents for the program-lifetime generation (rules (13),(89)).
    if (isCtlNDynArray(sym) && !sym->ty.isDynamic()) {
      size_t rank = sym->ty.dims.size();
      b_.CreateCall(runtimeFn("pli_ctl_alloc_dims"), {ctlKeyOf(sym), sz, i64((long long)rank)});
      emitCtlNDDescDims(sym);
    } else {
      b_.CreateCall(runtimeFn("pli_ctl_alloc"), {ctlKeyOf(sym), sz});
    }
    if (sym->ty.isChar() && sym->ty.varying) {
      llvm::Value* vaddr =
          b_.CreateCall(runtimeFn("pli_ctl_addr"), {ctlKeyOf(sym)}, "pkgctl.vaddr");
      llvm::Value* lp = b_.CreateStructGEP(llvmTy(sym->ty), vaddr, 0, "pkgctl.vlenp");
      b_.CreateStore(b_.getInt32(0), lp);
    }
    emitCtlInit(sym, sym->loc);
    b_.CreateBr(haveBB);
    b_.SetInsertPoint(haveBB);
  }
}

// Implicit FREE for CONTROLLED variables at procedure exit: pop one generation
// for each symbol this proc implicitly allocated, so explicit ALLOCATE/FREE
// pairs inside the body stay balanced on top of the implicit generation.
void IRGen::emitCtlEpilogue() {
  for (Symbol* sym : ctlImplicitAlloc_)
    b_.CreateCall(runtimeFn("pli_ctl_free"), {ctlKeyOf(sym)});
  // Destroy the procedure's AUTOMATIC AREA regions (rule (20)). Any blocks
  // still allocated in them are released with the region.
  for (Symbol* sym : areaLocals_)
    b_.CreateCall(runtimeFn("pli_area_destroy"), {b_.CreateLoad(b_.getPtrTy(), addressOf(sym))});
}

// ALLOCATE (rule 87): heap-allocate a based structure (rule 88, SET option) and
// store its address in the pointer target; or push a CONTROLLED generation.
// A CHARACTER item (rule 89) sizes from its dimension `(expr)` and/or
// `CHAR(expr)` when present, else the `CHAR(expr)` descriptor, else the
// compile-time descriptor. A VARYING generation holds `{i32 cur, [max x i8]}`
// so its byte size is `4 + max`; its current length starts at 0.
void IRGen::emitAllocate(HStmt* s) {
  static const std::vector<HStmt::HAllocBound> noBounds;
  for (size_t i = 0; i < s->allocBase.size(); ++i) {
    Symbol* bsym = s->allocBase[i]->sym;
    // The LLVM alloc size of the based structure (bytes) sizes the heap block.
    llvm::Value* sz = i64(mod_.getDataLayout().getTypeAllocSize(llvmTy(bsym->ty)).getFixedValue());
    if (!bsym->controlled && bsym->ty.isArray() && bsym->ty.isDynamic()) {
      // A dynamic-extent BASED heap array (rules (25),(89)): size the block
      // from the live DECLARE bounds at ALLOCATE time. The first axis may be
      // dynamic (later axes are static — sema gates the rest); each reference
      // reads the same live bounds through the dope slots below.
      const Type& el = bsym->ty.elementType();
      long long elemSz =
          (long long)mod_.getDataLayout().getTypeAllocSize(llvmTy(el)).getFixedValue();
      if (elemSz <= 0)
        elemSz = 1;
      llvm::Value* total = i64(elemSz);
      for (size_t k = 0; k < bsym->ty.dims.size(); ++k) {
        const Dim& d = bsym->ty.dims[k];
        llvm::Value* ub =
            (k == 0 && bsym->dynUb) ? toI64(emitExpr(bsym->dynUb), s->loc) : i64(d.ub);
        llvm::Value* lb =
            (k == 0 && d.lbDyn && bsym->dynLb) ? toI64(emitExpr(bsym->dynLb), s->loc) : i64(d.lb);
        if (k == 0) {
          // Later references check against the ALLOCATE-time extent, which
          // may differ from the entry-evaluated one when the bound changed.
          if (bsym->dynUb)
            dynUb_[bsym] = ub;
          if (d.lbDyn && bsym->dynLb)
            dynLb_[bsym] = lb;
        }
        llvm::Value* ext = b_.CreateAdd(b_.CreateSub(ub, lb, "bheap.e1"), i64(1), "bheap.ext");
        total = b_.CreateMul(total, ext, "bheap.tot");
      }
      sz = total;
    }
    if (bsym->controlled) {
      const auto& bounds = (i < s->allocBounds.size()) ? s->allocBounds[i] : noBounds;
      HExpr* clE = (i < s->allocCharLen.size()) ? s->allocCharLen[i].get() : nullptr;
      bool singleDim = bounds.size() == 1 && bounds[0].ub && !bounds[0].lb && !bounds[0].star;
      HExpr* dimE = singleDim ? bounds[0].ub.get() : nullptr;
      bool singleStar = bounds.size() == 1 && bounds[0].star;
      if (bsym->ty.isChar() && (dimE || clE)) {
        HExpr* se = dimE ? dimE : clE;
        llvm::Value* max = toI64(emitExpr(se), s->loc);
        if (bsym->ty.varying)
          sz = b_.CreateAdd(max, i64(4), "vmaxsz");
        else
          sz = max;
      } else if (!bsym->ty.isChar() && dimE && !clE) {
        // CONTROLLED 1-D numeric generation (rules (13),(89)): a scalar
        // `DCL FDS FIXED CONTROLLED`, a `DCL G(*)` array, or a fixed
        // `DCL A(10)` array sized by `ALLOCATE x (n)` holds n elements;
        // the generation is n * elemSize (lb preserved from the DECLARE).
        const Type& el = bsym->ty.isArray() ? bsym->ty.elementType() : bsym->ty;
        long long elemSz =
            (long long)mod_.getDataLayout().getTypeAllocSize(llvmTy(el)).getFixedValue();
        llvm::Value* n = toI64(emitExpr(dimE), s->loc);
        sz = b_.CreateMul(n, i64(elemSz), "ctln");
      } else if (!bsym->ty.isChar() && singleStar && !clE &&
                 (isCtlDynArray(bsym) || (!bsym->ty.isArray() && bsym->controlled))) {
        // `ALLOCATE x (*)` reuses the previous generation's extent (IBM (89)).
        // With no previous generation a fixed/scalar DECLARE falls back to
        // the descriptor size; a `(*)` DECLARE with an empty stack traps.
        llvm::Value* prevLen = b_.CreateCall(runtimeFn("pli_ctl_len"), {ctlKeyOf(bsym)}, "ctlprev");
        if (bsym->ty.isDynamic()) {
          llvm::Value* depth =
              b_.CreateCall(runtimeFn("pli_ctl_depth"), {ctlKeyOf(bsym)}, "ctldepth");
          llvm::Value* empty = b_.CreateICmpEQ(depth, i64(0), "ctlempty");
          std::string id = std::to_string(n_++);
          llvm::BasicBlock* trapL = llvm::BasicBlock::Create(ctx_, "ctlstar.trap." + id, curFn_);
          llvm::BasicBlock* okL = llvm::BasicBlock::Create(ctx_, "ctlstar.ok." + id, curFn_);
          b_.CreateCondBr(empty, trapL, okL);
          startBlock(trapL);
          b_.CreateCall(
              runtimeFn("pli_signal_error"),
              {globalString("ALLOCATE (*) of '" + bsym->name + "' with no previous generation")});
          b_.CreateUnreachable();
          startBlock(okL);
          sz = prevLen;
        } else {
          llvm::Value* depth =
              b_.CreateCall(runtimeFn("pli_ctl_depth"), {ctlKeyOf(bsym)}, "ctldepth");
          llvm::Value* hasPrev = b_.CreateICmpNE(depth, i64(0), "ctlhasprev");
          sz = b_.CreateSelect(hasPrev, prevLen, sz, "ctlstarsz");
        }
      } else if (!bsym->ty.isChar() && !bounds.empty() && !clE && isCtlNDynArray(bsym) &&
                 bounds.size() == bsym->ty.dims.size()) {
        // CONTROLLED N-D numeric generation (rules (13),(89)): one bound per
        // axis; a `*` bound copies that axis from the previous generation
        // (descriptor fallback for fixed axes, trap for `(*)` axes with an
        // empty stack). The generation holds prod(extents) elements.
        size_t rank = bsym->ty.dims.size();
        const Type& el = bsym->ty.elementType();
        long long elemSz =
            (long long)mod_.getDataLayout().getTypeAllocSize(llvmTy(el)).getFixedValue();
        if (elemSz <= 0)
          elemSz = 1;
        // Star extents read the previous top, so resolve them before pushing.
        bool needPrevTrap = false;
        for (size_t k = 0; k < rank; ++k)
          if (bounds[k].star) {
            const Dim& d = bsym->ty.dims[k];
            if (d.adj || d.dyn || d.lbDyn)
              needPrevTrap = true;
          }
        llvm::Value* depth = nullptr;
        if (needPrevTrap) {
          depth = b_.CreateCall(runtimeFn("pli_ctl_depth"), {ctlKeyOf(bsym)}, "ctldepth");
          llvm::Value* empty = b_.CreateICmpEQ(depth, i64(0), "ctlempty");
          std::string id = std::to_string(n_++);
          llvm::BasicBlock* trapL = llvm::BasicBlock::Create(ctx_, "ctlstar.trap." + id, curFn_);
          llvm::BasicBlock* okL = llvm::BasicBlock::Create(ctx_, "ctlstar.ok." + id, curFn_);
          b_.CreateCondBr(empty, trapL, okL);
          startBlock(trapL);
          b_.CreateCall(
              runtimeFn("pli_signal_error"),
              {globalString("ALLOCATE (*) of '" + bsym->name + "' with no previous generation")});
          b_.CreateUnreachable();
          startBlock(okL);
        } else if (bounds.empty() == false) {
          bool anyStar = false;
          for (const auto& b : bounds)
            if (b.star)
              anyStar = true;
          if (anyStar)
            depth = b_.CreateCall(runtimeFn("pli_ctl_depth"), {ctlKeyOf(bsym)}, "ctldepth");
        }
        std::vector<llvm::Value*> exts(rank);
        for (size_t k = 0; k < rank; ++k) {
          if (bounds[k].star) {
            llvm::Value* prev = b_.CreateCall(runtimeFn("pli_ctl_extent"),
                                              {ctlKeyOf(bsym), i64((long long)k)}, "ctlprev");
            const Dim& d = bsym->ty.dims[k];
            if (!d.adj && !d.dyn && !d.lbDyn) {
              // Fixed axis: fall back to the DECLARE extent when the stack
              // is empty (first explicit ALLOCATE over the implicit one).
              long long descExt = (long long)d.ub - (long long)d.lb + 1;
              llvm::Value* hasPrev = b_.CreateICmpNE(depth, i64(0), "ctlhasprev");
              exts[k] = b_.CreateSelect(hasPrev, prev, i64(descExt), "ctlstarext");
            } else {
              exts[k] = prev; // trapped above when the stack is empty
            }
          } else if (bounds[k].ub) {
            exts[k] = toI64(emitExpr(bounds[k].ub.get()), s->loc);
          } else {
            exts[k] = i64(0); // lb:ub pairs stay diagnosed in sema; never crash here
          }
        }
        llvm::Value* total = i64(elemSz);
        for (size_t k = 0; k < rank; ++k)
          total = b_.CreateMul(total, exts[k], "ctltot");
        b_.CreateCall(runtimeFn("pli_ctl_alloc_dims"),
                      {ctlKeyOf(bsym), total, i64((long long)rank)});
        for (size_t k = 0; k < rank; ++k)
          b_.CreateCall(runtimeFn("pli_ctl_set_dim"), {ctlKeyOf(bsym), i64((long long)k), exts[k]});
        // INITIAL assigns with each allocation (rules (15),(26), SC26-3114).
        emitCtlInit(bsym, s->loc);
        continue;
      } else if (ctlDeclaredRuntimeBound(bsym) && bounds.empty()) {
        // A bare ALLOCATE of a CONTROLLED array with a non-`*` runtime DECLARE
        // extent (rules (13),(89)): push a generation of the declared extent.
        ctlAllocDeclared(bsym, s->loc);
        emitCtlInit(bsym, s->loc);
        continue;
      } else if (bsym->ty.isChar() && bsym->ty.starLen && bsym->dynLenExpr) {
        sz = toI64(emitExpr(bsym->dynLenExpr), s->loc);
      }
      b_.CreateCall(runtimeFn("pli_ctl_alloc"), {ctlKeyOf(bsym), sz});
      if (isCtlNDynArray(bsym)) {
        // A bare N-D ALLOCATE re-pushes the DECLARE extents so the new top
        // generation's live bounds match the descriptor (an `(*)` axis never
        // reaches here: sema requires explicit bounds for it).
        emitCtlNDDescDims(bsym);
      }
      if (bsym->ty.isChar() && bsym->ty.varying) {
        // A fresh VARYING generation starts empty (cur = 0); an INITIAL value
        // overwrites it next via emitCtlInit.
        llvm::Value* addr = b_.CreateCall(runtimeFn("pli_ctl_addr"), {ctlKeyOf(bsym)}, "vaddr");
        llvm::Value* lp = b_.CreateStructGEP(llvmTy(bsym->ty), addr, 0, "vlenp");
        b_.CreateStore(b_.getInt32(0), lp);
      }
      // INITIAL assigns with each allocation (rules (15),(26), SC26-3114).
      emitCtlInit(bsym, s->loc);
      continue;
    }
    // IN ( area ) (rule (88)): take the block from the AREA region instead of
    // the default heap; the address is still an opaque locator either way.
    HExpr* areaE = (i < s->allocArea.size()) ? s->allocArea[i].get() : nullptr;
    llvm::Value* p =
        areaE ? b_.CreateCall(runtimeFn("pli_area_alloc"), {emitExpr(areaE).reg, sz}, "aheap")
              : b_.CreateCall(runtimeFn("pli_alloc"), {sz}, "heap");
    HExpr* set = (i < s->allocSet.size()) ? s->allocSet[i].get() : nullptr;
    if (set)
      storeTo(set->sym, Val{set->ty, p}, set->loc);
    // ALLOCATE of a BASED variable also sets the variable's own BASED locator
    // (rule (88)), so references through the based name address the new cell.
    if (bsym->basedBase && (!set || bsym->basedBase != set->sym))
      b_.CreateStore(p, addressOf(bsym->basedBase));
  }
}

// FREE (rule 90): release the heap storage addressed by a based pointer — the
// explicit locator when given, else the based variable's own BASED pointer —
// or pop a CONTROLLED generation.
void IRGen::emitFree(HStmt* s) {
  for (size_t i = 0; i < s->freeBase.size(); ++i) {
    Symbol* bsym = s->freeBase[i]->sym;
    if (bsym && bsym->controlled) {
      b_.CreateCall(runtimeFn("pli_ctl_free"), {ctlKeyOf(bsym)});
      continue;
    }
    llvm::Value* addr =
        s->freeBase[i]->locPtr ? emitExpr(s->freeBase[i]->locPtr.get()).reg : addressOf(bsym);
    // IN ( area ) (rule (90)): return the block to its AREA region.
    HExpr* areaE = (i < s->freeArea.size()) ? s->freeArea[i].get() : nullptr;
    if (areaE)
      b_.CreateCall(runtimeFn("pli_area_free"), {emitExpr(areaE).reg, addr});
    else
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

// Collect CONTROLLED uses and calls under one expression: VarRef/Subscript on
// a controlled symbol is a generation-stack read; any Call may reallocate a
// aliased dummy stack, so it blocks hoisting (flushVarWrites already reloads).
void IRGen::collectCtlExprUses(HExpr* e, std::unordered_set<Symbol*>& uses, bool& hasCall) {
  if (!e)
    return;
  if (e->kind == HExpr::Call)
    hasCall = true;
  if ((e->kind == HExpr::VarRef || e->kind == HExpr::Subscript) && e->sym && e->sym->controlled)
    uses.insert(e->sym);
  collectCtlExprUses(e->a.get(), uses, hasCall);
  collectCtlExprUses(e->b.get(), uses, hasCall);
  collectCtlExprUses(e->locPtr.get(), uses, hasCall);
  for (auto& a : e->args)
    collectCtlExprUses(a.get(), uses, hasCall);
}

// Walk one statement for loop-hoist analysis: `uses` gains every CONTROLLED
// symbol read, `muts` every symbol an ALLOCATE/FREE (or a growing CHAR(*)
// store) may re-stack, and `hasCall` any call/handler/wait that may do so
// transitively. Missing a mutator would miscompile, so CALLs block hoisting.
void IRGen::collectCtlLoopInfo(HStmt* s, std::unordered_set<Symbol*>& uses,
                               std::unordered_set<Symbol*>& muts, bool& hasCall) {
  if (!s)
    return;
  auto walkE = [&](HExpr* e) { collectCtlExprUses(e, uses, hasCall); };
  switch (s->kind) {
  case HStmt::Allocate:
    for (auto& b : s->allocBase) {
      if (b && b->sym && b->sym->controlled)
        muts.insert(b->sym);
      walkE(b.get());
    }
    for (auto& e : s->allocSet)
      walkE(e.get());
    for (auto& e : s->allocArea)
      walkE(e.get());
    for (auto& e : s->allocCharLen)
      walkE(e.get());
    for (auto& bb : s->allocBounds)
      for (auto& b : bb) {
        walkE(b.lb.get());
        walkE(b.ub.get());
      }
    return;
  case HStmt::Free:
    for (auto& b : s->freeBase) {
      if (b && b->sym && b->sym->controlled)
        muts.insert(b->sym);
      walkE(b.get());
    }
    for (auto& e : s->freeArea)
      walkE(e.get());
    return;
  case HStmt::CallS:
    hasCall = true;
    for (auto& a : s->args)
      walkE(a.get());
    walkE(s->taskRef.get());
    walkE(s->eventRef.get());
    walkE(s->priorityExpr.get());
    return;
  case HStmt::On:
    hasCall = true;
    walkE(s->oncodeExpr.get());
    return;
  case HStmt::Wait:
    hasCall = true;
    for (auto& e : s->waitEvents)
      walkE(e.get());
    walkE(s->waitCount.get());
    return;
  case HStmt::Assign: {
    auto noteTarget = [&](HExpr* t) {
      if (t && t->sym && t->sym->controlled && t->sym->ty.isChar() && t->sym->ty.starLen)
        muts.insert(t->sym);
      walkE(t);
    };
    noteTarget(s->target.get());
    for (auto& t : s->extraTargets)
      noteTarget(t.get());
    walkE(s->value.get());
    walkE(s->cond.get());
    walkE(s->from.get());
    walkE(s->to.get());
    walkE(s->by.get());
    walkE(s->stringTarget.get());
    break;
  }
  default:
    break;
  }
  walkE(s->target.get());
  walkE(s->value.get());
  walkE(s->cond.get());
  walkE(s->from.get());
  walkE(s->to.get());
  walkE(s->by.get());
  walkE(s->skipCount.get());
  walkE(s->stringTarget.get());
  walkE(s->oncodeExpr.get());
  walkE(s->waitCount.get());
  for (auto& t : s->extraTargets)
    walkE(t.get());
  for (auto& it : s->items)
    walkE(it.get());
  for (auto& a : s->args)
    walkE(a.get());
  for (auto& e : s->waitEvents)
    walkE(e.get());
  for (auto& f : s->formats) {
    walkE(f.w.get());
    walkE(f.d.get());
    walkE(f.s.get());
    for (auto& sub : f.subs) {
      walkE(sub.w.get());
      walkE(sub.d.get());
      walkE(sub.s.get());
    }
  }
  if (s->thenS)
    collectCtlLoopInfo(s->thenS.get(), uses, muts, hasCall);
  if (s->elseS)
    collectCtlLoopInfo(s->elseS.get(), uses, muts, hasCall);
  if (s->unit && s->kind != HStmt::On)
    collectCtlLoopInfo(s->unit.get(), uses, muts, hasCall);
  for (auto& b : s->body)
    collectCtlLoopInfo(b.get(), uses, muts, hasCall);
}

// Hoist loop-invariant CONTROLLED reads before a DO loop: for every used
// symbol with no ALLOCATE/FREE/CALL inside, emit one addr+len (+N-D extents)
// that the body reuses via the hoist maps. Element stores write the data
// buffer, never the stack metadata, so the hoist is sound. Callers erase the
// returned symbols from the maps on loop exit.
void IRGen::hoistCtlForLoopBody(const std::vector<HStmtP>& body, HExpr* cond,
                                std::vector<Symbol*>& hoisted,
                                std::vector<std::pair<Symbol*, size_t>>& hoistedExt) {
  std::unordered_set<Symbol*> uses, muts;
  bool hasCall = false;
  for (auto& b : body)
    if (b)
      collectCtlLoopInfo(b.get(), uses, muts, hasCall);
  if (cond)
    collectCtlExprUses(cond, uses, hasCall);
  if (hasCall || uses.empty())
    return;
  for (Symbol* sym : uses) {
    if (!sym || !sym->controlled || muts.count(sym) || ctlAddrHoist_.count(sym))
      continue;
    ctlAddrHoist_[sym] = b_.CreateCall(runtimeFn("pli_ctl_addr"), {ctlKeyOf(sym)}, "ctlhoist.a");
    ctlLenHoist_[sym] = b_.CreateCall(runtimeFn("pli_ctl_len"), {ctlKeyOf(sym)}, "ctlhoist.l");
    hoisted.push_back(sym);
    if (isCtlNDynArray(sym)) {
      for (size_t k = 0; k < sym->ty.dims.size(); ++k) {
        auto key = std::make_pair(sym, k);
        ctlExtHoist_[key] = b_.CreateCall(runtimeFn("pli_ctl_extent"),
                                          {ctlKeyOf(sym), i64((long long)k)}, "ctlhoist.e");
        hoistedExt.push_back(key);
      }
    }
  }
}

void IRGen::emitDoWhile(HStmt* s) {
  std::string id = std::to_string(n_++);
  llvm::BasicBlock* condL = llvm::BasicBlock::Create(ctx_, "do.cond." + id, curFn_);
  llvm::BasicBlock* bodyL = llvm::BasicBlock::Create(ctx_, "do.body." + id, curFn_);
  llvm::BasicBlock* endL = llvm::BasicBlock::Create(ctx_, "do.end." + id, curFn_);
  // Loop-invariant CONTROLLED bases hoist before the loop (no stack mutation
  // inside, so one addr+len serves every iteration).
  std::vector<Symbol*> hoisted;
  std::vector<std::pair<Symbol*, size_t>> hoistedExt;
  hoistCtlForLoopBody(s->body, s->cond.get(), hoisted, hoistedExt);
  auto unhoist = [&] {
    for (Symbol* h : hoisted) {
      ctlAddrHoist_.erase(h);
      ctlLenHoist_.erase(h);
    }
    for (auto& k : hoistedExt)
      ctlExtHoist_.erase(k);
  };
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
    unhoist();
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
  unhoist();
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

  // P1, Task 8: hoist loop-invariant TO and BY as SSA values instead of
  // storing to an alloca and reloading every iteration — the per-iteration
  // loads block LLVM's loop-invariant-sinking / LSR on the bounds (collatz,
  // array_sort: the loads survive into optimized IR and pin the bound in a
  // memory operand that can't be CSE'd across blocks).
  Val tv;
  if (s->to)
    tv = convert(emitExpr(s->to.get()), ct, s->loc);
  Val by;
  if (s->by) {
    by = convert(emitExpr(s->by.get()), ct, s->loc);
  } else {
    by.ty = ct;
    by.reg = ct.k == TK::Float ? flt(1.0) : llvm::ConstantInt::get(llvmTy(ct), 1, true);
  }

  llvm::BasicBlock* condL = llvm::BasicBlock::Create(ctx_, "do.cond." + id, curFn_);
  llvm::BasicBlock* bodyL = llvm::BasicBlock::Create(ctx_, "do.body." + id, curFn_);
  llvm::BasicBlock* stepL = llvm::BasicBlock::Create(ctx_, "do.step." + id, curFn_);
  llvm::BasicBlock* endL = llvm::BasicBlock::Create(ctx_, "do.end." + id, curFn_);
  llvm::BasicBlock* upL = s->to ? llvm::BasicBlock::Create(ctx_, "do.up." + id, curFn_) : nullptr;
  llvm::BasicBlock* downL =
      s->to ? llvm::BasicBlock::Create(ctx_, "do.down." + id, curFn_) : nullptr;
  llvm::BasicBlock* testL =
      s->to ? llvm::BasicBlock::Create(ctx_, "do.test." + id, curFn_) : nullptr;

  // Loop-invariant CONTROLLED bases hoist here (after FROM/TO/BY, before the
  // loop): one addr+len per used stack serves the whole body. A body with
  // ALLOCATE/FREE/CALL keeps per-iteration calls (hoistCtl checks).
  std::vector<Symbol*> ctlHoisted;
  std::vector<std::pair<Symbol*, size_t>> ctlHoistedExt;
  hoistCtlForLoopBody(s->body, s->cond.get(), ctlHoisted, ctlHoistedExt);

  branch(condL);
  startBlock(condL);

  if (s->to) {
    Val cv = loadSym(ctl, ct);
    llvm::Value* bv = by.reg;
    llvm::Value* zero = ct.k == TK::Float ? flt(0.0) : llvm::Constant::getNullValue(llvmTy(ct));
    llvm::Value* neg;
    if (ct.k == TK::Float)
      neg = b_.CreateFCmpOLT(bv, zero, "negstep");
    else
      neg = b_.CreateICmpSLT(bv, zero, "negstep");
    b_.CreateCondBr(neg, downL, upL);

    startBlock(upL);
    llvm::Value* cu = ct.k == TK::Float ? b_.CreateFCmpOLE(cv.reg, tv.reg, "cmpup")
                                        : b_.CreateICmpSLE(cv.reg, tv.reg, "cmpup");
    b_.CreateCondBr(cu, testL, endL);

    startBlock(downL);
    llvm::Value* cd = ct.k == TK::Float ? b_.CreateFCmpOGE(cv.reg, tv.reg, "cmpdn")
                                        : b_.CreateICmpSGE(cv.reg, tv.reg, "cmpdn");
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
  llvm::Value* bv = by.reg;
  llvm::Value* nx =
      ct.k == TK::Float ? b_.CreateFAdd(cv.reg, bv, "next") : b_.CreateAdd(cv.reg, bv, "next");
  Val nv;
  nv.ty = ct;
  nv.reg = nx;
  storeTo(ctl, nv, s->loc);
  branch(condL);

  startBlock(endL);
  for (Symbol* h : ctlHoisted) {
    ctlAddrHoist_.erase(h);
    ctlLenHoist_.erase(h);
  }
  for (auto& k : ctlHoistedExt)
    ctlExtHoist_.erase(k);
}

// Data-directed output (rule (106), QR1.5): each item prints as NAME=value,
// ", "-separated and ";"-terminated. Names are compile-time globals; values
// reuse the list-directed printers (the runtime suppresses the blank
// separator after a name). Sema restricted items to plain scalar variables,
// so any other shape here is already diagnosed.
void IRGen::emitPutDataItems(HStmt* s) {
  for (auto& item : s->items) {
    HExpr* t = item.get();
    if (t->ty.isStruct()) {
      emitPutDataStruct(t, t->sym->name);
    } else {
      emitPutDataScalar(t, t->sym->name);
    }
  }
  b_.CreateCall(runtimeFn("pli_put_data_end"), {});
}

void IRGen::emitPutDataStruct(HExpr* t, const std::string& prefix) {
  const Type& ty = t->ty;
  for (size_t i = 0; i < ty.members.size(); ++i) {
    const auto& m = ty.members[i];
    std::string memberName = prefix + "." + m->name;
    HExprP memberExpr = std::make_unique<HExpr>();
    memberExpr->kind = HExpr::VarRef;
    memberExpr->sym = t->sym;
    memberExpr->ty = m->ty;
    memberExpr->memberPath = t->memberPath;
    memberExpr->memberPath.push_back(i);
    memberExpr->loc = t->loc;
    if (m->ty.isStruct()) {
      emitPutDataStruct(memberExpr.get(), memberName);
    } else {
      emitPutDataScalar(memberExpr.get(), memberName);
    }
  }
}

void IRGen::emitPutDataScalar(HExpr* t, const std::string& name) {
  llvm::Value* nameVal = globalString(name);
  b_.CreateCall(runtimeFn("pli_put_data_name"), {nameVal, i64((long long)name.size())});
  Val v = emitExpr(t);
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
    if (v.ty.k == TK::FixedDec && v.ty.scale > 0) {
      if (v.ty.intBits() == 128) // ADR-191: >18 digit DECIMAL needs __int128 I/O
        b_.CreateCall(runtimeFn("pli_put_list_decfixed128"), {v.reg, i64(v.ty.scale)});
      else
        b_.CreateCall(runtimeFn("pli_put_list_decfixed"), {toI64(v), i64(v.ty.scale)});
    } else if (v.ty.k == TK::FixedDec && v.ty.intBits() == 128) {
      b_.CreateCall(runtimeFn("pli_put_list_decfixed128"), {v.reg, i64(0)});
    } else {
      b_.CreateCall(runtimeFn("pli_put_list_fixed"), {toI64(v)});
    }
    break;
  default:
    break;
  }
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
  bool isVarying = false;
  llvm::Value* slenPrefix = nullptr; // for varying: pointer to length prefix
  if (s->stringTarget) {
    HExpr* st = s->stringTarget.get();
    if (st->ty.isChar() && st->ty.varying) {
      // VARYING string: data is at struct index 1, length prefix at index 0
      llvm::Value* base =
          st->memberPath.empty() ? addressOf(st->sym) : memberAddr(st->sym, st->memberPath, s->loc);
      sdata = b_.CreateStructGEP(llvmTy(st->sym->ty), base, 1, "vdata");
      slen = i64(st->ty.len); // capacity
      slenPrefix = b_.CreateStructGEP(llvmTy(st->sym->ty), base, 0, "vlen");
      isVarying = true;
    } else {
      sdata =
          st->memberPath.empty() ? addressOf(st->sym) : memberAddr(st->sym, st->memberPath, s->loc);
      slen = i64(st->ty.len);
    }
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
        if (v.ty.k == TK::FixedDec && v.ty.scale > 0) {
          if (v.ty.intBits() == 128) // ADR-191: >18 digit DECIMAL needs __int128 I/O
            b_.CreateCall(runtimeFn("pli_put_list_decfixed128"), {v.reg, i64(v.ty.scale)});
          else
            b_.CreateCall(runtimeFn("pli_put_list_decfixed"), {toI64(v), i64(v.ty.scale)});
        } else if (v.ty.k == TK::FixedDec && v.ty.intBits() == 128) {
          b_.CreateCall(runtimeFn("pli_put_list_decfixed128"), {v.reg, i64(0)});
        } else {
          b_.CreateCall(runtimeFn("pli_put_list_fixed"), {toI64(v)});
        }
        break;
      case TK::Void:
        break;
      case TK::Struct:
        // Whole-structure values are diagnosed in emitExpr (rule 127); a
        // structure never reaches list-directed output as a value.
        break;
      case TK::Pointer:
      case TK::Offset:
      case TK::Area:
      case TK::Entry:
        d_.error(s->loc,
                 "a POINTER/OFFSET/AREA/ENTRY value cannot be written with PUT LIST in this stage",
                 "(110)");
        break;
      case TK::Complex:
        // Complex output (CM5): real, sign, imaginary magnitude, I.
        b_.CreateCall(
            runtimeFn("pli_put_list_complex"),
            {b_.CreateExtractValue(v.cpx, 0, "cpx.re"), b_.CreateExtractValue(v.cpx, 1, "cpx.im")});
        break;
      case TK::Task:
      case TK::Event:
        d_.error(s->loc, "a TASK/EVENT value cannot be written with PUT LIST in this stage",
                 "(110)");
        break;
      }
    }
  }
  if (s->stringTarget) {
    llvm::Value* written = b_.CreateCall(runtimeFn("pli_string_put_close"), {sdata, slen});
    if (isVarying) {
      // Store the actual length written into the varying string's length prefix
      b_.CreateStore(b_.CreateTrunc(written, b_.getInt32Ty(), "vlen32"), slenPrefix);
    }
  }
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
    llvm::Value* match =
        b_.CreateCall(runtimeFn("pli_data_name_is"),
                      {nameBuf, namelen, want, i64((long long)t->sym->name.size())}, "datamatch");
    b_.CreateCondBr(b_.CreateICmpNE(match, i32(0), "datahit"), readL, nextL);
    startBlock(readL);
    const Type& ty = t->ty;
    Val v;
    switch (ty.k) {
    case TK::FixedBin:
    case TK::FixedDec:
      if (ty.k == TK::FixedDec && ty.scale > 0) {
        llvm::Value* raw = b_.CreateCall(runtimeFn("pli_get_list_decfixed"), {i64(ty.scale)});
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
          llvm::Value* raw = b_.CreateCall(runtimeFn("pli_get_list_decfixed"), {i64(ty.scale)});
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
      storeScalarTo(addr, ty, convert(v, ty, loc), packedMemberPath(t->sym->ty, t->memberPath));
      return;
    }
    if (t->sym->definedBase && t->sym->definedIsubAxis >= 0) {
      llvm::Value* addr = definedSubElementAddr(t->sym, t->args, loc);
      storeScalarTo(addr, ty, convert(v, ty, loc));
      return;
    }
    storeArrayElement(t->sym, t->args, v, loc, t->locPtr.get());
    return;
  }
  if (t->kind == HExpr::VarRef && t->sym) {
    if (!t->memberPath.empty()) {
      llvm::Value* addr =
          t->locPtr ? locatorMemberAddr(t->sym, t->memberPath, emitExpr(t->locPtr.get()).reg)
                    : memberAddr(t->sym, t->memberPath, loc);
      storeScalarTo(addr, ty, convert(v, ty, loc), packedMemberPath(t->sym->ty, t->memberPath));
      return;
    }
    storeTo(t->sym, v, loc);
    return;
  }
  d_.error(loc, "GET LIST target is not assignable in this stage", "(110)");
}

// Edit-directed output (rule (108)): walk the format list, pairing each
// A/B/C/F/E data format with the next data item and emitting the control
// (X/SKIP/PAGE/LINE/COLUMN) items in order.
void IRGen::emitPutEditItems(HStmt* s) {
  llvm::Value* defW = i64(0);
  llvm::Value* defD = i64(0);
  size_t di = 0;
  // One real (F/E) part of a COMPLEX value (rule (51)): both parts are
  // doubles, so F uses the float path and E the scientific path.
  auto putRealPart = [&](llvm::Value* dbl, const HFormatItem& sub) {
    llvm::Value* w = sub.w ? toI64(emitExpr(sub.w.get())) : defW;
    llvm::Value* d = sub.d ? toI64(emitExpr(sub.d.get())) : defD;
    if (sub.s)
      (void)toI64(emitExpr(sub.s.get())); // scale evaluated for effects (50)
    if (sub.kind == HFormatItem::E)
      b_.CreateCall(runtimeFn("pli_put_edit_float_e"), {dbl, w, d});
    else
      b_.CreateCall(runtimeFn("pli_put_edit_float"), {dbl, w, d});
  };
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
    case HFormatItem::Column: {
      // Column positioning (rule (48)): pad blanks to 1-based column n.
      llvm::Value* n = f.w ? toI64(emitExpr(f.w.get())) : i64(1);
      b_.CreateCall(runtimeFn("pli_put_edit_column"), {n});
      break;
    }
    case HFormatItem::Remote:
      d_.error(s->loc, "unexpanded remote format R (internal)", "(55)");
      break;
    case HFormatItem::A: {
      if (di >= s->items.size())
        break;
      HExpr* item = s->items[di++].get();
      Val v = emitExpr(item);
      llvm::Value* w = f.w ? toI64(emitExpr(f.w.get())) : defW;
      b_.CreateCall(runtimeFn("pli_put_edit_char"), {v.ptr, v.len, w});
      break;
    }
    case HFormatItem::B: {
      // Bit-string output (rule (52)): materialize the packed big-endian
      // bytes into a temp and write them as '0'/'1' right-justified in w.
      if (di >= s->items.size())
        break;
      HExpr* item = s->items[di++].get();
      Val v = emitExpr(item);
      long long nbits = v.ty.isBit() && v.ty.len > 0 ? v.ty.len : 1;
      long long nbytes = (nbits + 7) / 8;
      llvm::Value* tmp =
          entryAlloca(llvm::ArrayType::get(b_.getInt8Ty(), (unsigned)nbytes), "pebit");
      llvm::Value* ptr = b_.CreateInBoundsGEP(
          llvm::ArrayType::get(b_.getInt8Ty(), (unsigned)nbytes), tmp, {i64(0), i64(0)}, "pebitp");
      if (nbits == 1) {
        llvm::Value* one = b_.CreateZExt(v.reg, b_.getInt8Ty(), "b8");
        // BIT(1) travels as i1; the packed byte holds it in the low bit, but
        // the runtime reads the top bit of each byte, so shift into place.
        llvm::Value* top = b_.CreateShl(one, llvm::ConstantInt::get(b_.getInt8Ty(), 7), "btop");
        b_.CreateStore(top, ptr);
      } else {
        b_.CreateStore(v.reg, tmp);
      }
      llvm::Value* w = f.w ? toI64(emitExpr(f.w.get())) : i64(nbits);
      b_.CreateCall(runtimeFn("pli_put_edit_bit"), {ptr, i64(nbits), w});
      break;
    }
    case HFormatItem::C: {
      // Complex output (rule (51)): one COMPLEX item through 1-2 inner real
      // formats; a single sub serves both parts.
      if (di >= s->items.size())
        break;
      HExpr* item = s->items[di++].get();
      Val v = emitExpr(item);
      llvm::Value* re = b_.CreateExtractValue(v.cpx, 0, "cpx.re");
      llvm::Value* im = b_.CreateExtractValue(v.cpx, 1, "cpx.im");
      if (f.subs.empty()) {
        d_.error(s->loc, "a C format requires an inner F/E format", "(51)");
        break;
      }
      putRealPart(re, f.subs[0]);
      putRealPart(im, f.subs.size() > 1 ? f.subs[1] : f.subs[0]);
      break;
    }
    case HFormatItem::F: {
      if (di >= s->items.size())
        break;
      HExpr* item = s->items[di++].get();
      Val v = emitExpr(item);
      llvm::Value* w = f.w ? toI64(emitExpr(f.w.get())) : defW;
      llvm::Value* d = f.d ? toI64(emitExpr(f.d.get())) : defD;
      if (f.s)
        (void)toI64(emitExpr(f.s.get())); // scale evaluated for effects (50)
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
      if (f.s)
        (void)toI64(emitExpr(f.s.get())); // scale evaluated for effects (50)
      b_.CreateCall(runtimeFn("pli_put_edit_float_e"), {v.reg, w, d});
      break;
    }
    }
  }
}

// Edit-directed input (rule (108)): pair each A/B/C/F/E data format with
// the next data item and emit the control (X/SKIP/COLUMN) items in order.
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
    case HFormatItem::Column: {
      // Input positioning (rule (48)): skip forward to 1-based column n,
      // opening a fresh line first when already past n.
      llvm::Value* n = f.w ? toI64(emitExpr(f.w.get())) : i64(1);
      b_.CreateCall(runtimeFn("pli_get_edit_column"), {n});
      break;
    }
    case HFormatItem::Remote:
      d_.error(s->loc, "unexpanded remote format R (internal)", "(55)");
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
      if (f.s)
        (void)toI64(emitExpr(f.s.get())); // scale evaluated for effects (50)
      Val v;
      v.reg = b_.CreateCall(runtimeFn("pli_get_edit_num"), {w});
      v.ty = Type::flt(6);
      storeGetTarget(t, v, s->loc);
      break;
    }
    case HFormatItem::B: {
      // Bit-string input (rule (52)): read a w-character '0'/'1' field into
      // a temp packed buffer, then store through the normal BIT path.
      if (di >= s->items.size())
        break;
      HExpr* t = s->items[di++].get();
      long long nbits = t->ty.isBit() && t->ty.len > 0 ? t->ty.len : 1;
      long long nbytes = (nbits + 7) / 8;
      llvm::Value* tmp =
          entryAlloca(llvm::ArrayType::get(b_.getInt8Ty(), (unsigned)nbytes), "gebit");
      llvm::Value* ptr = b_.CreateInBoundsGEP(
          llvm::ArrayType::get(b_.getInt8Ty(), (unsigned)nbytes), tmp, {i64(0), i64(0)}, "gebitp");
      llvm::Value* w = f.w ? toI64(emitExpr(f.w.get())) : i64(nbits);
      b_.CreateCall(runtimeFn("pli_get_edit_bit"), {ptr, i64(nbits), w});
      Val v;
      v.ty = Type::bit((int)nbits);
      if (nbits == 1) {
        // The runtime packs into the top bit; BIT(1) travels as the low bit.
        llvm::Value* byte = b_.CreateLoad(b_.getInt8Ty(), ptr, "gb8");
        llvm::Value* low = b_.CreateLShr(byte, llvm::ConstantInt::get(b_.getInt8Ty(), 7), "gb7");
        v.reg = b_.CreateTrunc(low, b_.getInt1Ty(), "gb1");
      } else {
        v.reg = b_.CreateLoad(llvm::ArrayType::get(b_.getInt8Ty(), (unsigned)nbytes), tmp, "gb");
      }
      storeGetTarget(t, v, s->loc);
      break;
    }
    case HFormatItem::C: {
      // Complex input (rule (51)): read the real/imaginary parts through the
      // inner real widths, then store the {double,double} pair.
      if (di >= s->items.size())
        break;
      HExpr* t = s->items[di++].get();
      if (f.subs.empty()) {
        d_.error(s->loc, "a C format requires an inner F/E format", "(51)");
        break;
      }
      const HFormatItem& s1 = f.subs[0];
      const HFormatItem& s2 = f.subs.size() > 1 ? f.subs[1] : f.subs[0];
      llvm::Value* w1 = s1.w ? toI64(emitExpr(s1.w.get())) : defW;
      llvm::Value* w2 = s2.w ? toI64(emitExpr(s2.w.get())) : defW;
      if (s1.s)
        (void)toI64(emitExpr(s1.s.get()));
      if (s2.s)
        (void)toI64(emitExpr(s2.s.get()));
      llvm::Value* re = b_.CreateCall(runtimeFn("pli_get_edit_num"), {w1});
      llvm::Value* im = b_.CreateCall(runtimeFn("pli_get_edit_num"), {w2});
      llvm::Value* cpx = llvm::UndefValue::get(llvmTy(Type::complexTy()));
      cpx = b_.CreateInsertValue(cpx, re, 0, "cpx.re");
      cpx = b_.CreateInsertValue(cpx, im, 1, "cpx.im");
      Val v;
      v.cpx = cpx;
      v.ty = Type::complexTy();
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
  // BYADDR(s) opts a single call out of the copy (rule (38)): the caller's
  // own address rides through, so callee writes are visible. Sema validated
  // the shape; anything else here falls through to the safe copy path.
  if (a->kind == HExpr::Call && a->name == "BYADDR" && a->args.size() == 1) {
    HExpr* inner = a->args[0].get();
    if (inner->kind == HExpr::VarRef && inner->sym && inner->ty.isStruct())
      return inner->memberPath.empty() ? addressOf(inner->sym)
                                       : memberAddr(inner->sym, inner->memberPath, a->loc);
  }
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
  if (pty.isChar() && pty.starLen) {
    // An adjustable-length `CHAR(*)` parameter (rule (18)) takes the caller's
    // character variable directly by reference (no copy): the callee's length
    // comes from the hidden argument and its writes reach the caller's buffer.
    if (a->kind == HExpr::VarRef && a->sym && a->sym->ty.isChar() &&
        a->sym->ty.varying == pty.varying && a->memberPath.empty())
      return addressOf(a->sym);
    // A VARYING variable into a fixed `CHAR(*)` dummy (rule (18)): overlay the
    // string's data area by reference — the hidden argument carries its max
    // (argLen); a copy would expose the uninitialized tail.
    if (a->kind == HExpr::VarRef && a->sym && a->sym->ty.isChar() && a->sym->ty.varying &&
        !pty.varying && a->memberPath.empty())
      return b_.CreateStructGEP(llvmTy(a->sym->ty), addressOf(a->sym), 1, "vdata");
    // A non-VARYING actual into a `CHAR(*) VARYING` dummy (rule (18)): wrap
    // into a temp varying struct so the callee's struct-path read sees a
    // length prefix; the hidden argument carries the same live length as max.
    if (pty.varying && pty.starLen) {
      Val av0 = emitExpr(a);
      if (av0.ty.isChar()) {
        llvm::ConstantInt* capC =
            llvm::dyn_cast<llvm::ConstantInt>(av0.len ? av0.len : i64(av0.ty.len));
        int cap = capC ? static_cast<int>(capC->getZExtValue()) : 256;
        if (cap <= 0)
          cap = 1;
        llvm::Type* st = llvm::StructType::get(
            ctx_, {b_.getInt32Ty(), llvm::ArrayType::get(b_.getInt8Ty(), cap)});
        llvm::AllocaInst* tmp = entryAlloca(st, "vstar");
        llvm::Value* lp = b_.CreateStructGEP(st, tmp, 0, "vlp");
        llvm::Value* srcLen = av0.len ? av0.len : i64(av0.ty.len);
        b_.CreateStore(b_.CreateTrunc(srcLen, b_.getInt32Ty(), "vl32"), lp);
        llvm::Value* dp = b_.CreateStructGEP(st, tmp, 1, "vdp");
        b_.CreateMemCpy(dp, llvm::MaybeAlign(), av0.ptr, llvm::MaybeAlign(), srcLen);
        return tmp;
      }
    }
    // Non-variable character argument (expression/literal): evaluate the
    // expression into a temp buffer of the result's declared capacity, then
    // pass the buffer pointer and its live length via argLen().
    Val av = emitExpr(a);
    if (av.ty.isChar()) {
      llvm::ConstantInt* capConst = llvm::dyn_cast<llvm::ConstantInt>(av.len);
      int cap;
      if (capConst)
        cap = static_cast<int>(capConst->getZExtValue());
      else
        cap = 256; // fallback for dynamic-length expressions
      llvm::AllocaInst* buf = entryAlloca(llvm::ArrayType::get(b_.getInt8Ty(), cap), "cbuf");
      b_.CreateMemCpy(buf, llvm::MaybeAlign(), av.ptr, llvm::MaybeAlign(), av.len);
      return buf;
    }
    d_.error(a->loc,
             "an adjustable-length CHARACTER parameter takes a character variable in this stage",
             "(18)");
    return llvm::Constant::getNullValue(b_.getPtrTy());
  }
  if (direct)
    return addressOf(a->sym);
  llvm::Value* addr = entryAlloca(llvmTy(pty), "dummy");
  Val av = emitExpr(a);
  if (pty.isChar()) {
    Val cv = av;
    if (pty.varying) {
      llvm::Value* dp = b_.CreateStructGEP(llvmTy(pty), addr, 1, "vdata");
      llvm::Value* ln = emitAssignVarying(dp, i64(pty.len), cv.ptr, cv.len);
      llvm::Value* lp = b_.CreateStructGEP(llvmTy(pty), addr, 0, "vlenp");
      b_.CreateStore(b_.CreateTrunc(ln, b_.getInt32Ty(), "l32"), lp);
      // A varying-char variable of a different declared length is copied into
      // the dummy above; remember to copy the callee's result back so a
      // "growing" string (appends inside the callee) reaches the caller.
      if (a->kind == HExpr::VarRef && a->sym && a->sym->ty.isChar() && a->sym->ty.varying &&
          a->memberPath.empty())
        pendingVarWrites_.push_back({addr, a->sym, pty});
    } else {
      emitAssignChar(addr, i64(pty.len), cv.ptr, cv.len);
    }
  } else {
    Val cv = convert(av, pty, a->loc);
    storeScalarTo(addr, pty, cv);
  }
  return addr;
}

// Emit the copy-back for every varying-char argument marshalled through a
// parameter-sized dummy (see argAddr), clamped to the caller's capacity.
// For CONTROLLED variables the generation stack can change during the call —
// a callee or a transitive call may free / allocate on this variable's slot,
// invalidating the data pointer cached by `addressOf` at call-site time.
// Reload the latest-generation pointer so the write-back hits live memory.
void IRGen::flushVarWrites() {
  for (const PendingVarWrite& w : pendingVarWrites_) {
    const Type& cty = w.sym->ty;
    // Controls where we store back into: either the alloca'd descriptor (for
    // non-controlled char varying args) or the runtime generation itself (for
    // CONTROLLED variables, whose addr comes from pli_ctl_addr(slot)).
    llvm::Value* target = addressOf(w.sym);
    if (w.sym->controlled) {
      // Reload generation-pointer; non-controlled stays as-is (already points
      // to its frame-allocated descriptor struct).
      target = b_.CreateCall(runtimeFn("pli_ctl_addr"), {ctlKeyOf(w.sym)}, "ctlfresh");
    }
    // Load length and data pointer from the parameter-sized dummy created by
    // argAddr.  Layout: {i32 cur_len, [N x i8] data}.
    llvm::Value* dlp = b_.CreateStructGEP(llvmTy(w.pty), w.dummy, 0, "dlenp");
    llvm::Value* dlen =
        b_.CreateSExt(b_.CreateLoad(b_.getInt32Ty(), dlp, "dlen"), b_.getInt64Ty(), "dlen64");
    llvm::Value* ddata = b_.CreateStructGEP(llvmTy(w.pty), w.dummy, 1, "ddata");
    if (w.sym->controlled && cty.starLen) {
      // CHAR(*) VARYING CONTROLLED: the heap generation is {i32 cur_len, [max
      // x i8] data} with no outer descriptor.  Offset 0 → length field, offset
      // 4 → start of character data.  Get the live max via pli_ctl_len.
      llvm::Value* ctllen = b_.CreateCall(runtimeFn("pli_ctl_len"), {ctlKeyOf(w.sym)}, "ctllen");
      llvm::Value* max = b_.CreateSub(ctllen, i64(4), "ctlmax");
      // Data lives at offset 4 (skip the i32 length field).
      llvm::Value* dp = b_.CreateGEP(b_.getInt8Ty(), target, {b_.getInt64(4)}, "data_ptr");
      // Write data from the dummy into the generation buffer; store the
      // (clamped) result length back to offset 0.
      llvm::Value* ln = emitAssignVarying(dp, max, ddata, dlen);
      b_.CreateStore(b_.CreateTrunc(ln, b_.getInt32Ty()), target);
    } else {
      // Fixed-size or non-controlled CHAR varying: write into the alloca'd
      // descriptor struct using StructGEP offsets 0 (length) and 1 (data).
      llvm::Value* cdata = b_.CreateStructGEP(llvmTy(cty), target, 1, "cdata");
      llvm::Value* ln = emitAssignVarying(cdata, i64(cty.len), ddata, dlen);
      llvm::Value* clp = b_.CreateStructGEP(llvmTy(cty), target, 0, "clenp");
      b_.CreateStore(b_.CreateTrunc(ln, b_.getInt32Ty(), "clen"), clp);
    }
  }
  pendingVarWrites_.clear();
}

// VARYINGZ argument for a by-value C entry (ADR-168): copy the actual's
// current value into a fresh buffer and append a NUL, so the C callee sees a
// `char *` whose length is found by scanning. The buffer capacity is the
// actual variable's declared maximum; a runtime-capacity adjustable actual has
// no compile-time buffer size and is diagnosed.
llvm::Value* IRGen::cstrArg(HExpr* a, const Type& pty, SourceLoc loc) {
  (void)pty;
  Val av = emitExpr(a);
  if (!av.ty.isChar()) {
    d_.error(loc, "a VARYINGZ parameter takes a character argument (ADR-168)", "(18)");
    return llvm::ConstantPointerNull::get(b_.getPtrTy());
  }
  int cap = av.ty.len;
  if (a->kind == HExpr::VarRef && a->sym && a->sym->ty.isChar()) {
    if (a->sym->ty.starLen || a->sym->ty.isArray()) {
      d_.error(loc,
               "a VARYINGZ parameter takes a fixed-length or explicitly sized character "
               "variable in this stage (ADR-168)",
               "(18)");
      return llvm::ConstantPointerNull::get(b_.getPtrTy());
    }
    cap = a->sym->ty.len;
  }
  if (cap < 0)
    cap = 0;
  llvm::AllocaInst* buf =
      entryAlloca(llvm::ArrayType::get(b_.getInt8Ty(), (unsigned)cap + 1), "zstr");
  b_.CreateMemCpy(buf, llvm::MaybeAlign(), av.ptr, llvm::MaybeAlign(), av.len);
  llvm::Value* z = b_.CreateGEP(b_.getInt8Ty(), buf, {av.len}, "zterm");
  b_.CreateStore(b_.getInt8(0), z);
  return buf;
}

// Marshal one argument for a by-value C entry (rules (34),(38)): FIXED and
// FLOAT scalars convert to the parameter type and ride as values, POINTERs
// ride as the pointer itself; anything else keeps the argAddr form.
llvm::Value* IRGen::marshalArg(HExpr* a, const Type& pty, SourceLoc loc) {
  if (pty.isFixed() || pty.k == TK::Float) {
    Val av = emitExpr(a);
    Val cv = convert(av, pty, loc);
    return cv.reg;
  }
  if (pty.isPointer()) {
    Val av = emitExpr(a);
    if (!av.ty.isPointer()) {
      d_.error(loc, "by-value POINTER parameter requires a pointer argument", "(38)");
      return llvm::ConstantPointerNull::get(b_.getPtrTy());
    }
    return av.reg;
  }
  // VARYINGZ (ADR-168) rides as a NUL-terminated C string, not a varying
  // descriptor pointer.
  if (pty.isChar() && pty.varyingz)
    return cstrArg(a, pty, loc);
  return argAddr(a, pty);
}

// Element count of a call argument passed to a `*`-extent parameter (rule 13):
// a fixed array contributes its constant extent, a dynamic-bound array its live
// recorded bound, and a `*` parameter forwards this frame's own hidden extent.
// Returns null for an unsupported argument form (the caller diagnoses it).
llvm::Value* IRGen::argExtent(HExpr* a) {
  if (a->kind != HExpr::VarRef || !a->sym || !a->sym->ty.isArray())
    return nullptr;
  const Type& arr = a->sym->ty;
  const Dim& d = arr.dims[0];
  if (d.adj) {
    // Forwarding a `*` parameter (rule (13)): pass along the hidden extent
    // this frame received for it. Only a parameter of the procedure being
    // emitted owns a live slot here — a `*` reference from another frame
    // (e.g. an outer procedure's parameter) has no addressable extent value
    // and stays diagnosed at the call site.
    bool mine = false;
    if (curProc_) {
      for (Symbol* q : curProc_->paramSyms)
        if (q == a->sym)
          mine = true;
      for (auto& st : curProc_->body)
        if (st && st->kind == HStmt::Entry)
          for (Symbol* q : st->entryParamSyms)
            if (q == a->sym)
              mine = true;
    }
    auto it = dynUb_.find(a->sym);
    if (mine && it != dynUb_.end())
      return it->second;
    return nullptr;
  }
  if (d.dyn || d.lbDyn) {
    llvm::Value* ub = d.dyn ? (dynUb_.count(a->sym)
                                   ? dynUb_[a->sym]
                                   : (a->sym->dynUb ? toI64(emitExpr(a->sym->dynUb)) : i64(d.ub)))
                            : i64(d.ub);
    llvm::Value* lb = d.lbDyn ? (dynLb_.count(a->sym) ? dynLb_[a->sym] : i64(d.lb)) : i64(d.lb);
    // The hidden `*` extent is the first axis's extent (ub - lb + 1), not the
    // total element count: the callee stores it as dynUb_ (the upper bound of
    // axis 1) and computes DIM as extent × product of later axes. For 1-D arrays
    // rest == 1 anyway, so this is a no-op there.
    return b_.CreateAdd(b_.CreateSub(ub, lb, "e1"), i64(1), "ext");
  }
  return i64(d.ub - d.lb + 1);
}

// Buffer capacity of a call argument passed to an adjustable-length `CHAR(*)`
// parameter (rule (18)): a fixed char variable's declared length, a forwarded
// `CHAR(*)` parameter's live length, or the emitted value's length. A
// `CHAR(n) VARYING` actual takes its declared max in both cases (the capacity
// the callee may read or write; the data area is blank-filled at allocation
// so scans past the live length see blanks). `ptyVarying` is the parameter's
// VARYING attribute.
llvm::Value* IRGen::argLen(HExpr* a, bool ptyVarying) {
  if (a->kind == HExpr::VarRef && a->sym && a->sym->ty.isChar()) {
    // Forwarding a `CHAR(*)` parameter (rule (18)): pass the live length this
    // frame received for it. A CONTROLLED adjustable passes its current
    // generation size (rule (15)).
    if (a->sym->ty.starLen) {
      if (llvm::Value* live = adjustLen(a->sym, a->sym->ty))
        return live;
      return i64(0);
    }
    // A `CHAR(n) VARYING` actual with a non-VARYING dummy takes its declared
    // max (rule (18)): the callee overlays the data area by reference and
    // needs the full capacity for writes (fill_buf) and scans (get_len);
    // the live length rides in the prefix for varying-aware callees.
    if (a->sym->ty.varying && !ptyVarying) {
      return i64(a->sym->ty.len);
    }
    // A fixed char variable's declared length is the caller buffer's capacity.
    return i64(a->sym->ty.len);
  }
  Val av = emitExpr(a);
  if (av.ty.isChar())
    return av.len ? av.len : i64(av.ty.len);
  return i64(0);
}

// Effective generation-stack key of a CONTROLLED variable (rules (15),(87)):
// the alias key bound at entry for a controlled `CHAR(*)` dummy, else the
// symbol's own deterministic slot (sema's qualified-name hash).
llvm::Value* IRGen::ctlKeyOf(Symbol* sym) {
  auto it = ctlKey_.find(sym);
  return it != ctlKey_.end() ? it->second : i64((long long)sym->ctlSlot);
}

// Hidden i64 for a CONTROLLED `CHAR(*)` dummy at a call site (rules (15),
// (18)): the actual variable's generation-stack key so callee ALLOCATE/FREE
// act on the caller's stack; -1 when the actual is not controlled (the dummy
// then gets a fresh stack of its own).
llvm::Value* IRGen::ctlKeyArg(HExpr* a) {
  if (a->kind == HExpr::VarRef && a->sym && a->sym->controlled && a->memberPath.empty())
    return ctlKeyOf(a->sym);
  return i64(-1);
}

// The hidden i64 an adjustable parameter takes at a call site (rules (13),
// (15),(18)): a CONTROLLED `CHAR(*)` dummy gets the actual's slot key (the
// stack it will allocate and free on), an omitted `*` or left-out trailing
// OPTIONAL gets zero (non-controlled) or -1 (controlled: fresh stack), and
// everything else keeps the extent/length convention.
llvm::Value* IRGen::hiddenAdjustArg(HExpr* a, const Type& ty, bool ctl) {
  if (ctl && isStarLen(ty))
    return a ? ctlKeyArg(a) : i64(-1);
  return a ? (isStarLen(ty) ? argLen(a, ty.varying) : argExtent(a)) : i64(0);
}

// Live capacity of an adjustable-length `CHAR(*)` value (rule (18)): the
// hidden argument for a parameter (current length for FIXED CHAR(*), max
// capacity for VARYING), or the current generation size for a CONTROLLED
// variable (rule (15)). For `CHAR(*) VARYING CONTROLLED` the generation holds
// a `{i32 cur, [max x i8]}` struct, so the live max is `pli_ctl_len - 4`.
// Returns nullptr when the value is not an adjustable character.
llvm::Value* IRGen::adjustLen(Symbol* sym, const Type& ty) {
  if (!ty.isChar() || !ty.starLen)
    return nullptr;
  if (ty.varying) {
    if (sym && sym->controlled) {
      auto hit = ctlLenHoist_.find(sym);
      llvm::Value* total = (hit != ctlLenHoist_.end())
                               ? hit->second
                               : b_.CreateCall(runtimeFn("pli_ctl_len"), {ctlKeyOf(sym)}, "ctllen");
      return b_.CreateSub(total, i64(4), "ctlmax");
    }
    // Non-controlled VARYING STAR parameter: max capacity from hidden arg.
    if (sym && dynLen_.count(sym))
      return dynLen_[sym];
    return nullptr;
  }
  if (sym && sym->controlled) {
    auto hit = ctlLenHoist_.find(sym);
    if (hit != ctlLenHoist_.end())
      return hit->second;
    return b_.CreateCall(runtimeFn("pli_ctl_len"), {ctlKeyOf(sym)}, "ctllen");
  }
  if (sym && dynLen_.count(sym))
    return dynLen_[sym];
  return nullptr;
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
  // An indirect CALL through an entry variable (IBM extension, ADR-171): load
  // the stored procedure pointer and call it with its declared signature. The
  // result, if any, is discarded.
  if (s->sym->isEntryVar) {
    emitEntryCall(s->sym, s->args, s->loc, nullptr);
    return;
  }
  // Any task option makes the CALL asynchronous (rule (79), QR2.8).
  if (s->hasTaskOpt || s->eventRef || s->priorityExpr) {
    emitAsyncCall(s);
    return;
  }
  Symbol* calleeSym = s->sym;
  Proc* callee = calleeSym->proc;
  Stmt* en = calleeSym->entry;
  llvm::Function* calleeF = calleeFn(calleeSym);
  std::vector<Symbol*> calleeParams =
      en ? en->entryParamSyms : (callee ? callee->paramSyms : std::vector<Symbol*>());

  std::vector<llvm::Value*> args;
  // A structure-returning (rule 127) or character-returning (rules (34),(37))
  // callee takes a hidden result buffer as its first argument; the CALL
  // statement discards the returned value.
  Type rty = en ? (en->entryIsFunction ? en->entryRetTy : Type::voidTy())
                : (callee ? callee->retTy : Type::voidTy());
  if (rty.isStruct() || rty.isChar())
    args.push_back(rty.isChar() ? sretAlloc(rty) : entryAlloca(llvmTy(rty), "sret"));
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
    if (a->kind == HExpr::Star) {
      // Omitted OPTIONAL (extension, ADR-119): the generated code supplies
      // a null pointer, which OMITTED/PRESENT test. Sema validated it.
      args.push_back(llvm::Constant::getNullValue(b_.getPtrTy()));
      continue;
    }
    // A by-value C entry (rules (34),(38)) takes scalar/pointer values.
    if (calleeSym->entryByValue)
      args.push_back(marshalArg(a, pty, s->loc));
    else
      args.push_back(argAddr(a, pty));
  }
  // Trailing omitted OPTIONALs pad with nulls to the full signature, so the
  // callee's hidden-result and static-link positions never shift (sema
  // validated the remainder is OPTIONAL).
  if (en || callee)
    for (size_t i = s->args.size(); i < calleeParams.size(); ++i)
      args.push_back(llvm::Constant::getNullValue(b_.getPtrTy()));
  // Hidden extent/length args for `*`-extent array and `CHAR(*)` parameters
  // (rules (13),(18)): the caller passes the actual extent of each matching
  // array argument, or the actual buffer length of each character argument.
  for (size_t i = 0; i < calleeParams.size(); ++i) {
    if (isAdjustable(calleeParams[i])) {
      // An omitted '*' (or left-out trailing OPTIONAL) takes a zero extent
      // (or the -1 fresh-stack key for a CONTROLLED dummy) alongside its
      // null address (extension, ADR-119).
      HExpr* a =
          (i >= s->args.size() || s->args[i]->kind == HExpr::Star) ? nullptr : s->args[i].get();
      llvm::Value* ext = hiddenAdjustArg(a, calleeParams[i]->ty, calleeParams[i]->controlled);
      if (!ext) {
        d_.error(s->args[i]->loc,
                 "a '*' extent parameter takes a fixed or dynamic-bound array in this stage",
                 "(13)");
        ext = i64(0);
      }
      args.push_back(ext);
    }
  }
  // The same hidden args when CALLing a plain-EXTERNAL PL/I entry (no local
  // Proc/ENTRY statement): driven by the ENTRY declaration's parameter
  // types. By-value C entries carry none (lengths are explicit there).
  if (!en && !callee && !calleeSym->entryByValue)
    for (size_t i = 0; i < calleeSym->entryParams.size(); ++i) {
      const Type& t = calleeSym->entryParams[i];
      if (!((t.isArray() && !t.dims.empty() && t.dims[0].adj) || (t.isChar() && t.starLen)))
        continue;
      HExpr* a =
          (i >= s->args.size() || s->args[i]->kind == HExpr::Star) ? nullptr : s->args[i].get();
      llvm::Value* ext = hiddenAdjustArg(a, t, t.controlled);
      if (!ext) {
        d_.error(s->args[i]->loc,
                 "a '*' extent parameter takes a fixed or dynamic-bound array in this stage",
                 "(13)");
        ext = i64(0);
      }
      args.push_back(ext);
    }
  appendStaticLinks(callee, args);
  b_.CreateCall(calleeF, args);
  flushVarWrites();
}

// Address of an EVENT/TASK reference (rules (79),(82)): a scalar variable's
// own cell, an array element ev(i) through the element-address path, or a
// structure member S.EV through the member path. Returns null when no symbol.
llvm::Value* IRGen::taskEventAddr(HExpr* e, SourceLoc loc) {
  if (!e || !e->sym)
    return nullptr;
  if (e->kind == HExpr::Subscript && e->sym) {
    if (!e->memberPath.empty()) {
      // A member of one element of an array of structures arr(i).ev.
      llvm::Value* elem = arrayElementAddr(e->sym->ty, addressOf(e->sym), e->args, loc);
      return elementMemberAddr(e->sym, e->memberPath, elem);
    }
    if (e->sym->controlled) {
      if (isCtlDynArray(e->sym))
        return ctlDynElementAddr(e->sym, e->args, loc);
      if (isCtlNDynArray(e->sym))
        return ctlNDElementAddr(e->sym, e->args, loc);
    }
    if (e->sym->ty.isDynamic())
      return arrayElementAddr(e->sym->ty, addressOf(e->sym), e->args, loc,
                              dynUb_.count(e->sym) ? dynUb_[e->sym] : nullptr,
                              dynLb_.count(e->sym) ? dynLb_[e->sym] : nullptr);
    return arrayElementAddr(e->sym->ty, addressOf(e->sym), e->args, loc);
  }
  if (!e->memberPath.empty())
    return memberAddr(e->sym, e->memberPath, loc);
  return addressOf(e->sym);
}

// Asynchronous CALL (rule (79), QR2.8): marshal the callee arguments as for a
// synchronous call, pack them into a heap context, and run a per-site wrapper
// on a detached thread. The caller returns at once; WAIT/EVENT synchronises.
// Async argument storage shares the caller's frame, so the caller must WAIT
// for the task before leaving the block (C28 task-storage rule).
void IRGen::emitAsyncCall(HStmt* s) {
  Symbol* calleeSym = s->sym;
  Proc* callee = calleeSym->proc;
  Stmt* en = calleeSym->entry;
  if (!callee && !en)
    return; // external async: diagnosed by sema (79)
  llvm::Function* calleeF = calleeFn(calleeSym);
  std::vector<Symbol*> calleeParams =
      en ? en->entryParamSyms : (callee ? callee->paramSyms : std::vector<Symbol*>());

  // Marshal the callee arguments exactly as a synchronous call would.
  std::vector<llvm::Value*> callArgs;
  Type rty = en ? (en->entryIsFunction ? en->entryRetTy : Type::voidTy())
                : (callee ? callee->retTy : Type::voidTy());
  if (rty.isStruct() || rty.isChar())
    callArgs.push_back(rty.isChar() ? sretAlloc(rty) : entryAlloca(llvmTy(rty), "sret"));
  for (size_t i = 0; i < s->args.size(); ++i) {
    HExpr* a = s->args[i].get();
    Type pty;
    if (i < calleeParams.size())
      pty = calleeParams[i]->ty;
    else
      break;
    if (a->kind == HExpr::Star) {
      callArgs.push_back(llvm::Constant::getNullValue(b_.getPtrTy()));
      continue;
    }
    callArgs.push_back(argAddr(a, pty));
  }
  if (en || callee)
    for (size_t i = s->args.size(); i < calleeParams.size(); ++i)
      callArgs.push_back(llvm::Constant::getNullValue(b_.getPtrTy()));
  for (size_t i = 0; i < calleeParams.size(); ++i) {
    if (isAdjustable(calleeParams[i])) {
      HExpr* a =
          (i >= s->args.size() || s->args[i]->kind == HExpr::Star) ? nullptr : s->args[i].get();
      llvm::Value* ext = hiddenAdjustArg(a, calleeParams[i]->ty, calleeParams[i]->controlled);
      if (!ext)
        ext = i64(0); // diagnosed by sema through the sync path (13)
      callArgs.push_back(ext);
    }
  }
  appendStaticLinks(callee, callArgs);
  // PRIORITY is evaluated for its effects, then ignored (best-effort, (79)).
  if (s->priorityExpr) {
    Val pv = emitExpr(s->priorityExpr.get());
    (void)toI64(pv);
  }

  llvm::Value* eventAddr = llvm::Constant::getNullValue(b_.getPtrTy());
  if (s->eventRef)
    eventAddr = taskEventAddr(s->eventRef.get(), s->loc);
  llvm::Value* taskAddr = nullptr;
  if (s->taskRef)
    taskAddr = taskEventAddr(s->taskRef.get(), s->loc);

  // Context layout: [event addr, ...callArgs in order]. All addresses are
  // opaque pointers; extents are i64.
  std::vector<llvm::Type*> fields = {b_.getPtrTy()};
  for (llvm::Value* v : callArgs)
    fields.push_back(v->getType());
  llvm::StructType* ctxTy = llvm::StructType::get(ctx_, fields);

  // Per-site wrapper: unpack the context, call the procedure, complete the
  // event, free the context, return null.
  llvm::FunctionType* wft = llvm::FunctionType::get(b_.getPtrTy(), {b_.getPtrTy()}, false);
  llvm::Function* wrap = llvm::Function::Create(wft, llvm::Function::InternalLinkage,
                                                "pli_task_wrap_" + std::to_string(n_++), &mod_);
  llvm::BasicBlock* savedBB = b_.GetInsertBlock();
  llvm::BasicBlock* wbb = llvm::BasicBlock::Create(ctx_, "entry", wrap);
  b_.SetInsertPoint(wbb);
  llvm::Value* ctx = wrap->arg_begin();
  std::vector<llvm::Value*> wargs;
  for (size_t k = 0; k < callArgs.size(); ++k) {
    llvm::Value* fp = b_.CreateStructGEP(ctxTy, ctx, (unsigned)(k + 1), "carg");
    wargs.push_back(b_.CreateLoad(fields[k + 1], fp, "cval"));
  }
  b_.CreateCall(calleeF, wargs);
  llvm::Value* evp = b_.CreateStructGEP(ctxTy, ctx, 0, "cev");
  llvm::Value* ev = b_.CreateLoad(b_.getPtrTy(), evp, "ev");
  b_.CreateCall(runtimeFn("pli_event_complete"), {ev});
  b_.CreateCall(runtimeFn("pli_free"), {ctx});
  b_.CreateRet(llvm::Constant::getNullValue(b_.getPtrTy()));
  b_.SetInsertPoint(savedBB);

  // Fill the context at the call site, reset the event first so a fast task
  // cannot complete before the reset (lost wakeup), then spawn.
  llvm::Value* size = i64((long long)mod_.getDataLayout().getTypeStoreSize(ctxTy));
  llvm::Value* ctxRaw = b_.CreateCall(runtimeFn("pli_alloc"), {size}, "tctx");
  llvm::Value* evField = b_.CreateStructGEP(ctxTy, ctxRaw, 0, "tev");
  b_.CreateStore(eventAddr, evField);
  for (size_t k = 0; k < callArgs.size(); ++k) {
    llvm::Value* fp = b_.CreateStructGEP(ctxTy, ctxRaw, (unsigned)(k + 1), "targ");
    b_.CreateStore(callArgs[k], fp);
  }
  if (s->eventRef)
    b_.CreateCall(runtimeFn("pli_event_reset"), {eventAddr});
  if (taskAddr)
    b_.CreateCall(runtimeFn("pli_task_note"), {taskAddr});
  llvm::Value* fnPtr = b_.CreateBitCast(wrap, b_.getPtrTy(), "twrap");
  b_.CreateCall(runtimeFn("pli_task_spawn"), {fnPtr, ctxRaw});
  pendingVarWrites_.clear(); // no sync point to copy a varying arg back
}

void IRGen::emitWait(HStmt* s) {
  // WAIT (rule (82)): no count waits for every event; WAIT(evs)(k) waits for
  // any k of them through pli_wait_n.
  if (!s->waitCount) {
    for (auto& e : s->waitEvents) {
      llvm::Value* addr = taskEventAddr(e.get(), s->loc);
      if (!addr)
        continue;
      b_.CreateCall(runtimeFn("pli_event_wait"), {addr});
    }
    return;
  }
  size_t n = s->waitEvents.size();
  llvm::Value* arr = entryAlloca(llvm::ArrayType::get(b_.getPtrTy(), (unsigned)n), "wait.evs");
  for (size_t i = 0; i < n; ++i) {
    llvm::Value* ep = b_.CreateInBoundsGEP(llvm::ArrayType::get(b_.getPtrTy(), (unsigned)n), arr,
                                           {i64(0), i64((long long)i)}, "we");
    llvm::Value* addr = taskEventAddr(s->waitEvents[i].get(), s->loc);
    if (!addr)
      addr = llvm::Constant::getNullValue(b_.getPtrTy());
    b_.CreateStore(addr, ep);
  }
  Val need = emitExpr(s->waitCount.get());
  b_.CreateCall(runtimeFn("pli_wait_n"), {arr, i64((long long)n), toI64(need)});
}

void IRGen::emitDelay(HStmt* s) {
  // DELAY (rule (83)): sleep N milliseconds; N <= 0 is a no-op.
  if (!s->value)
    return;
  Val v = emitExpr(s->value.get());
  b_.CreateCall(runtimeFn("pli_delay"), {toI64(v)});
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
                                     const std::vector<HExprP>& idxs, SourceLoc, llvm::Value* dynUb,
                                     llvm::Value* dynLb) {
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
      // A handled slip resumes with this axis clamped into range (rule 94);
      // (NOSUBSCRIPTRANGE) trusts the raw index and emits no check.
      llvm::Value* idx = subChecks() ? clampIndex(i, lb, ub) : i;
      llvm::Value* off = b_.CreateSub(idx, lb, "off");
      flat = b_.CreateAdd(flat, b_.CreateMul(off, i64(stride), "scaled"), "flat");
      stride *= (dk.ub - dk.lb + 1); // later axes are fixed; only axis 0 is runtime
    }
    if (subChecks()) {
      std::string id = std::to_string(n_++);
      llvm::BasicBlock* failL = llvm::BasicBlock::Create(ctx_, "sub.fail." + id, curFn_);
      llvm::BasicBlock* okL = llvm::BasicBlock::Create(ctx_, "sub.ok." + id, curFn_);
      b_.CreateCondBr(oob, failL, okL);
      startBlock(failL);
      emitCondTrap(Stmt::kSubscriptrangeCondKey, "pli_subscript_oob", "sub", okL);
      startBlock(okL);
    }
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
    // A handled slip resumes with this axis clamped into range (rule 94);
    // (NOSUBSCRIPTRANGE) trusts the raw index and emits no check.
    llvm::Value* idx = subChecks() ? clampIndex(i, i64(lb), i64(ub)) : i;
    llvm::Value* off = b_.CreateSub(idx, i64(lb), "off");
    flat = b_.CreateAdd(flat, b_.CreateMul(off, i64(stride), "scaled"), "flat");
    stride *= (ub - lb + 1);
  }

  if (subChecks()) {
    std::string id = std::to_string(n_++);
    llvm::BasicBlock* failL = llvm::BasicBlock::Create(ctx_, "sub.fail." + id, curFn_);
    llvm::BasicBlock* okL = llvm::BasicBlock::Create(ctx_, "sub.ok." + id, curFn_);
    b_.CreateCondBr(oob, failL, okL);

    startBlock(failL);
    emitCondTrap(Stmt::kSubscriptrangeCondKey, "pli_subscript_oob", "sub", okL);

    startBlock(okL);
  }
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
  llvm::LoadInst* p = b_.CreateLoad(b_.getPtrTy(), memberAddr(base, path, loc), "mdynp");
  if (packedMemberPath(base->ty, path))
    p->setAlignment(llvm::Align(1));
  return p;
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
        llvm::LoadInst* dbuf = b_.CreateLoad(b_.getPtrTy(), d, "bnm.dp");
        llvm::LoadInst* sbuf = b_.CreateLoad(b_.getPtrTy(), s, "bnm.sp");
        if (dst.unaligned)
          dbuf->setAlignment(llvm::Align(1));
        if (src.unaligned)
          sbuf->setAlignment(llvm::Align(1));
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
      llvm::LoadInst* lv = b_.CreateLoad(llvmTy(sm.ty), s, "bnm.ld");
      if (src.unaligned)
        lv->setAlignment(llvm::Align(1));
      sv.reg = lv;
      if (sm.ty.isBit())
        sv.reg = b_.CreateTrunc(sv.reg, b_.getInt1Ty(), "bnm.b1");
      storeScalarTo(d, dm.ty, convert(sv, dm.ty, loc), dst.unaligned);
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
        llvm::LoadInst* buf = b_.CreateLoad(b_.getPtrTy(), mem, "init.mdyn");
        if (ty.unaligned)
          buf->setAlignment(llvm::Align(1));
        long long k = 0;
        while (idx < vals.size()) {
          llvm::Value* ep = b_.CreateGEP(llvmTy(el), buf, {i64(k++)}, "init.el");
          storeScalarTo(ep, el, initValue(el, vals[idx++]));
        }
      } else {
        llvm::Type* arrTy = llvm::ArrayType::get(llvmTy(el), (unsigned)arrayExtent(m.ty));
        for (long long k = 0; k < arrayExtent(m.ty); ++k) {
          llvm::Value* ep = b_.CreateInBoundsGEP(arrTy, mem, {i64(0), i64(k)}, "init.el");
          if (el.isStruct()) {
            emitStructInitValues(ep, el, vals, idx, loc);
          } else {
            // A null slot marks a per-member-INIT zero-fill (rule (26)):
            // keep the zero-initialised element, still consuming the slot.
            if (idx >= vals.size())
              return;
            Expr* ve = vals[idx++];
            if (!ve)
              continue;
            if (el.isChar())
              storeCharTo(ep, el, initValue(el, ve), loc, ty.unaligned);
            else
              storeScalarTo(ep, el, initValue(el, ve), ty.unaligned);
          }
        }
      }
    } else {
      // A null slot marks a per-member-INIT zero-fill (rule (26)): the
      // field keeps its zero-initialised storage, so skip the store — but
      // still consume the slot so siblings stay aligned.
      if (idx >= vals.size())
        return;
      Expr* ve = vals[idx++];
      if (!ve)
        continue;
      if (m.ty.isChar())
        storeCharTo(mem, m.ty, initValue(m.ty, ve), loc, ty.unaligned);
      else
        storeScalarTo(mem, m.ty, initValue(m.ty, ve), ty.unaligned);
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
    } else if (ty.starLen) {
      // An adjustable-length `CHAR(*)` (rule (18)): a parameter's live length
      // is the hidden argument the caller supplied (read once at entry); a
      // CONTROLLED variable's is its current generation size (rule (15)).
      v.ptr = addr;
      if (llvm::Value* live = adjustLen(sym, ty))
        v.len = live;
      else
        v.len = i64(ty.len);
    } else {
      v.ptr = addr;
      // A whole fixed array value spans every element; an element value spans
      // its declared length (rules (12),(18)).
      v.len = ty.isArray() ? i64(arrayExtent(ty) * (long long)ty.len) : i64(ty.len);
    }
    return v;
  }
  llvm::Value* r = b_.CreateLoad(llvmTy(ty), addr, "ld");
  if (ty.isComplex()) {
    // A complex value (QR2.2/CM5) is the {double,double} struct itself.
    v.cpx = r;
  } else if (ty.isBit()) {
    // BIT(1) travels as i1 in registers; wider strings ride whole as the
    // packed byte aggregate.
    v.reg = ty.len == 1 ? b_.CreateTrunc(r, b_.getInt1Ty(), "b1") : r;
  } else {
    v.reg = r;
  }
  return v;
}

void IRGen::storeScalarTo(llvm::Value* addr, const Type& ty, const Val& v, bool packed) {
  if (ty.isComplex()) {
    // A complex value (QR2.2/CM5) is stored as the {double,double} struct.
    llvm::StoreInst* si = b_.CreateStore(v.cpx, addr);
    if (packed)
      si->setAlignment(llvm::Align(1));
    return;
  }
  llvm::Value* val = v.reg;
  if (ty.isBit()) {
    if (ty.len == 1)
      val = b_.CreateZExt(val, b_.getInt8Ty(), "z8");
    // Wider strings store whole; differing lengths convert before this
    // (hir emits Convert whenever the bit lengths differ).
  }
  llvm::StoreInst* si = b_.CreateStore(val, addr);
  if (packed)
    si->setAlignment(llvm::Align(1));
}

void IRGen::storeCharTo(llvm::Value* addr, const Type& dt, const Val& v, SourceLoc loc,
                        bool packed) {
  if (!v.ty.isChar()) {
    d_.error(loc,
             "conversion from " + v.ty.desc() + " to " + dt.desc() +
                 " is not implemented in this stage",
             "(86)");
    return;
  }
  if (dt.varying) {
    llvm::Value* dp = b_.CreateStructGEP(llvmTy(dt), addr, 1, "vdata");
    llvm::Value* ln = emitAssignVarying(dp, i64(dt.len), v.ptr, v.len);
    llvm::Value* lp = b_.CreateStructGEP(llvmTy(dt), addr, 0, "vlenp");
    llvm::StoreInst* si = b_.CreateStore(b_.CreateTrunc(ln, b_.getInt32Ty(), "l32"), lp);
    if (packed)
      si->setAlignment(llvm::Align(1));
  } else if (dt.isArray()) {
    // A whole fixed array copy moves every element's bytes; identical shapes
    // were checked in sema, and any residual length difference blank-pads or
    // truncates per element (rules (12),(18),(86)).
    long long total = arrayExtent(dt) * (long long)dt.len;
    emitAssignChar(addr, i64(total), v.ptr, v.len);
  } else {
    emitAssignChar(addr, i64(dt.len), v.ptr, v.len);
  }
}

void IRGen::storeTo(Symbol* sym, const Val& v, SourceLoc loc) {
  const Type& dt = sym->ty;
  llvm::Value* addr = addressOf(sym);
  if (dt.isChar()) {
    if (dt.starLen && dt.varying) {
      // `CHAR(*) VARYING` target (rules (18),(89)): either CONTROLLED with a
      // heap generation that may grow, or a parameter whose max capacity is the
      // hidden i64 arg the caller supplied. Truncate source to max via
      // pli_assign_varying (VARYING semantics, no expansion).
      // A CONTROLLED variable with no current generation gets one implicitly,
      // sized to the source (Y33-6003 CONTROLLED first-store).
      if (sym && sym->controlled) {
        llvm::Value* want = v.len;
        if (want && want->getType()->getIntegerBitWidth() != 64 && !llvm::isa<llvm::Constant>(want))
          want = b_.CreateSExt(want, b_.getInt64Ty(), "vsrc");
        llvm::Value* need = want ? b_.CreateAdd(want, i64(4), "vneed") : i64(64);
        b_.CreateCall(runtimeFn("pli_ctl_ensure"), {ctlKeyOf(sym), need});
        addr = b_.CreateCall(runtimeFn("pli_ctl_addr"), {ctlKeyOf(sym)}, "caddr");
      }
      llvm::Value* max = adjustLen(sym, dt);
      if (!max)
        max = i64(dt.len);
      llvm::Value* dp = b_.CreateStructGEP(llvmTy(dt), addr, 1, "vdata");
      llvm::Value* ln = emitAssignVarying(dp, max, v.ptr, v.len);
      llvm::Value* lp = b_.CreateStructGEP(llvmTy(dt), addr, 0, "vlenp");
      b_.CreateStore(b_.CreateTrunc(ln, b_.getInt32Ty(), "l32"), lp);
      return;
    }
    if (dt.starLen) {
      // An adjustable-length `CHAR(*)` target (rule (18)): write into the
      // buffer, blank-padding/truncating to the live capacity — the hidden
      // length for a parameter, the current generation size for CONTROLLED
      // (rule (15)).
      llvm::Value* live = adjustLen(sym, dt);
      if (!live)
        live = i64(dt.len);
      // For adjustable CHAR(*) on CONTROLLED variables (rule (18)): if the
      // current generation is too small for the source content, free it and
      // allocate a replacement sized to the source. Then always reload fresh
      // addr+len for assign_char so we never write past the live region.
      if (sym && sym->controlled && !v.ty.varying && v.len) {
        llvm::Value* srclen = v.len;
        if (srclen->getType()->getIntegerBitWidth() != 64)
          srclen = b_.CreateSExt(srclen, b_.getInt64Ty(), "srcl");
        llvm::Value* needs = b_.CreateICmpUGT(srclen, live, "needexp");
        llvm::BasicBlock* expBB = llvm::BasicBlock::Create(ctx_, "expan", curFn_);
        llvm::BasicBlock* copyBB = llvm::BasicBlock::Create(ctx_, "copyif", curFn_);
        llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(ctx_, "expend", curFn_);
        b_.CreateCondBr(needs, expBB, copyBB);
        b_.SetInsertPoint(expBB);
        // Free the too-small generation only when one exists (first store
        // into an empty stack just allocates).
        llvm::Value* had = b_.CreateICmpUGT(live, i64(0), "hadgen");
        llvm::BasicBlock* frL = llvm::BasicBlock::Create(ctx_, "expfree", curFn_);
        llvm::BasicBlock* alL = llvm::BasicBlock::Create(ctx_, "expalloc", curFn_);
        b_.CreateCondBr(had, frL, alL);
        b_.SetInsertPoint(frL);
        b_.CreateCall(runtimeFn("pli_ctl_free"), {ctlKeyOf(sym)});
        b_.CreateBr(alL);
        b_.SetInsertPoint(alL);
        b_.CreateCall(runtimeFn("pli_ctl_alloc"), {ctlKeyOf(sym), srclen});
        b_.CreateBr(mergeBB);
        b_.SetInsertPoint(copyBB);
        b_.CreateBr(mergeBB);
        b_.SetInsertPoint(mergeBB);
        // Always reload fresh addr+len — may have changed in expBB
        llvm::Value* faddr = b_.CreateCall(runtimeFn("pli_ctl_addr"), {ctlKeyOf(sym)}, "fraddr");
        llvm::Value* flen = b_.CreateCall(runtimeFn("pli_ctl_len"), {ctlKeyOf(sym)}, "flen");
        emitAssignChar(faddr, flen, v.ptr, v.len);
        return;
      }
      emitAssignChar(addr, live, v.ptr, v.len);
      return;
    }
    storeCharTo(addr, dt, v, loc);
    return;
  }
  Val cv = convert(v, dt, loc);
  storeScalarTo(addr, dt, cv);
}

// A CONTROLLED numeric array with per-generation extents (rules (13),(89),
// IBM Enterprise PL/I: ALLOCATE bounds override DECLARE bounds): a scalar
// CONTROLLED numeric variable used with one subscript, a 1-D (*) CONTROLLED
// array, or a fixed 1-D CONTROLLED numeric array (whose ALLOCATE (n) overrides
// the DECLARE extent, lb preserved). The live extent is per-generation, so it
// cannot ride a static dope slot.
bool IRGen::isCtlDynArray(Symbol* sym) {
  if (!sym || !sym->controlled)
    return false;
  if (!sym->ty.isArray())
    return !sym->ty.isChar() && !sym->ty.isStruct() && !sym->ty.isPointer() && !sym->ty.isVoid() &&
           (sym->ty.isNumeric() || sym->ty.isBit());
  if (sym->ty.dims.size() != 1)
    return false;
  const Type& el = sym->ty.elementType();
  if (el.isChar() || el.isStruct() || el.isPointer() || el.isVoid())
    return false;
  if (!el.isNumeric() && !el.isBit())
    return false;
  // Dynamic (*) plus fixed 1-D: both size per-generation (bare = descriptor,
  // ALLOCATE (n) = override). Multi-axis stays on the static path here;
  // N-D overrides use the dims runtime (isCtlNDynArray).
  return true;
}

// Live extent of a CONTROLLED dynamic array (rule (126)): `pli_ctl_len(key) /
// elemSize`. An empty stack reports 0, so every subscript is out of bounds
// until the first ALLOCATE (n).
llvm::Value* IRGen::ctlDynBound(Symbol* sym) {
  const Type& el = sym->ty.isArray() ? sym->ty.elementType() : sym->ty;
  long long elemSz = (long long)mod_.getDataLayout().getTypeAllocSize(llvmTy(el)).getFixedValue();
  if (elemSz <= 0)
    elemSz = 1;
  // Hoisted loops reuse the preheader len; the udiv stays loop-invariant too.
  auto hit = ctlLenHoist_.find(sym);
  llvm::Value* total = (hit != ctlLenHoist_.end())
                           ? hit->second
                           : b_.CreateCall(runtimeFn("pli_ctl_len"), {ctlKeyOf(sym)}, "ctllen");
  return b_.CreateUDiv(total, i64(elemSz), "ctlub");
}

// Live lower bound of a CONTROLLED dynamic array: the DECLARE lb for a fixed
// 1-D array (preserved across an ALLOCATE (n) override, so bare `ALLOCATE A`
// keeps e.g. 0:9), else 1 for scalar/`(*)` generations.
long long IRGen::ctlDynLb(Symbol* sym) {
  if (sym && sym->ty.isArray() && sym->ty.dims.size() == 1 && !sym->ty.dims[0].adj &&
      !sym->ty.isDynamic())
    return sym->ty.dims[0].lb;
  return 1;
}

// Address of one CONTROLLED dynamic array element (rule 126): a runtime
// SUBSCRIPTRANGE check against the live bound, then a GEP off the top
// generation base as a bare element pointer (mirrors the dynamic-array path).
llvm::Value* IRGen::ctlDynElementAddr(Symbol* sym, const std::vector<HExprP>& idxs, SourceLoc loc) {
  const Type& el = sym->ty.isArray() ? sym->ty.elementType() : sym->ty;
  llvm::Value* base = addressOf(sym);
  llvm::Value* ext = ctlDynBound(sym);
  long long lbConst = ctlDynLb(sym);
  llvm::Value* lb = i64(lbConst);
  // ub = lb + extent - 1 (empty stack: extent 0 -> ub = lb - 1, all OOB).
  llvm::Value* ub = b_.CreateSub(b_.CreateAdd(lb, ext, "ctlhi"), i64(1), "ctlub");
  llvm::Value* i = toI64(emitExpr(idxs[0].get()));
  llvm::Value* oob =
      b_.CreateOr(b_.CreateICmpSLT(i, lb, "lo"), b_.CreateICmpSGT(i, ub, "hi"), "oob");
  llvm::Value* idx = subChecks() ? clampIndex(i, lb, ub) : i;
  llvm::Value* flat = b_.CreateSub(idx, lb, "off");
  if (subChecks()) {
    std::string id = std::to_string(n_++);
    llvm::BasicBlock* failL = llvm::BasicBlock::Create(ctx_, "sub.fail." + id, curFn_);
    llvm::BasicBlock* okL = llvm::BasicBlock::Create(ctx_, "sub.ok." + id, curFn_);
    b_.CreateCondBr(oob, failL, okL);
    startBlock(failL);
    emitCondTrap(Stmt::kSubscriptrangeCondKey, "pli_subscript_oob", "sub", okL);
    startBlock(okL);
  }
  (void)loc;
  return b_.CreateInBoundsGEP(llvmTy(el), base, {flat}, "ctlelem");
}

bool IRGen::isCtlNDynArray(Symbol* sym) {
  if (!sym || !sym->controlled || !sym->ty.isArray() || sym->ty.dims.size() < 2)
    return false;
  const Type& el = sym->ty.elementType();
  if (el.isChar() || el.isStruct() || el.isPointer() || el.isVoid())
    return false;
  return el.isNumeric() || el.isBit();
}

long long IRGen::ctlNDLb(Symbol* sym, size_t axis) {
  if (sym && sym->ty.isArray() && axis < sym->ty.dims.size()) {
    const Dim& d = sym->ty.dims[axis];
    if (!d.adj && !d.dyn && !d.lbDyn)
      return d.lb;
  }
  return 1;
}

llvm::Value* IRGen::ctlNDExtent(Symbol* sym, size_t axis) {
  // Hoisted loops reuse the preheader extent for this axis.
  auto hit = ctlExtHoist_.find({sym, axis});
  if (hit != ctlExtHoist_.end())
    return hit->second;
  return b_.CreateCall(runtimeFn("pli_ctl_extent"), {ctlKeyOf(sym), i64((long long)axis)},
                       "ctlext");
}

llvm::Value* IRGen::ctlNDElementAddr(Symbol* sym, const std::vector<HExprP>& idxs, SourceLoc loc) {
  // N-D CONTROLLED element (rules (13),(126)): per-axis live bounds
  // [lb, lb+ext-1] with static lb, row-major strides from the live extents.
  const Type& el = sym->ty.elementType();
  llvm::Value* base = addressOf(sym);
  size_t rank = sym->ty.dims.size();
  std::vector<llvm::Value*> exts(rank), lbs(rank), ubs(rank), offs(rank);
  for (size_t k = 0; k < rank; ++k) {
    exts[k] = ctlNDExtent(sym, k);
    lbs[k] = i64(ctlNDLb(sym, k));
    ubs[k] = b_.CreateSub(b_.CreateAdd(lbs[k], exts[k], "ctlhi"), i64(1), "ctlub");
  }
  llvm::Value* oob = b_.getInt1(false);
  std::vector<llvm::Value*> idxs64(rank);
  for (size_t k = 0; k < rank; ++k) {
    idxs64[k] = toI64(emitExpr(idxs[k].get()));
    llvm::Value* lo = b_.CreateICmpSLT(idxs64[k], lbs[k], "lo");
    llvm::Value* hi = b_.CreateICmpSGT(idxs64[k], ubs[k], "hi");
    oob = b_.CreateOr(oob, b_.CreateOr(lo, hi, "oobk"), "oob");
  }
  // Clamp each axis into range when checks apply, then flatten row-major.
  llvm::Value* flat = i64(0);
  for (size_t k = 0; k < rank; ++k) {
    llvm::Value* idx = subChecks() ? clampIndex(idxs64[k], lbs[k], ubs[k]) : idxs64[k];
    offs[k] = b_.CreateSub(idx, lbs[k], "off");
  }
  for (size_t k = 0; k < rank; ++k) {
    llvm::Value* stride = i64(1);
    for (size_t j = k + 1; j < rank; ++j)
      stride = b_.CreateMul(stride, exts[j], "st");
    flat = b_.CreateAdd(flat, b_.CreateMul(offs[k], stride, "sw"), "flat");
  }
  if (subChecks()) {
    std::string id = std::to_string(n_++);
    llvm::BasicBlock* failL = llvm::BasicBlock::Create(ctx_, "sub.fail." + id, curFn_);
    llvm::BasicBlock* okL = llvm::BasicBlock::Create(ctx_, "sub.ok." + id, curFn_);
    b_.CreateCondBr(oob, failL, okL);
    startBlock(failL);
    emitCondTrap(Stmt::kSubscriptrangeCondKey, "pli_subscript_oob", "sub", okL);
    startBlock(okL);
  }
  (void)loc;
  return b_.CreateInBoundsGEP(llvmTy(el), base, {flat}, "ctlelem");
}

void IRGen::emitCtlNDDescDims(Symbol* sym) {
  size_t rank = sym->ty.dims.size();
  for (size_t k = 0; k < rank; ++k) {
    const Dim& d = sym->ty.dims[k];
    long long ext = (long long)d.ub - (long long)d.lb + 1;
    if (d.adj || d.dyn || d.lbDyn)
      ext = 0; // `(*)` axes have no descriptor extent; explicit ALLOCATE fills them
    b_.CreateCall(runtimeFn("pli_ctl_set_dim"), {ctlKeyOf(sym), i64((long long)k), i64(ext)});
  }
}

// Load one array element (rule 126): a bounds-checked address, then a load of
// the element's scalar value. Character element arrays are diagnosed, not
// silently miscompiled (invariant 2).
Val IRGen::loadArrayElement(Symbol* sym, const std::vector<HExprP>& idxs, SourceLoc loc,
                            HExpr* locPtr) {
  Val v;
  const Type& el = sym->ty.isArray() ? sym->ty.elementType() : sym->ty;
  v.ty = el;
  if (el.isChar()) {
    // Fixed CHARACTER element (rules (12),(18)): the value is a view of the
    // element's bytes. A VARYING element is a struct { i32 len, [N x i8] data }.
    if (el.varying) {
      llvm::Value* base = locPtr ? emitExpr(locPtr).reg : addressOf(sym);
      llvm::Value* elemAddr =
          arrayElementAddr(sym->ty, base, idxs, loc, dynUb_.count(sym) ? dynUb_[sym] : nullptr,
                           dynLb_.count(sym) ? dynLb_[sym] : nullptr);
      // Load the length prefix (index 0 of the struct)
      llvm::Value* lenPtr = b_.CreateStructGEP(llvmTy(el), elemAddr, 0, "vlen");
      llvm::Value* len = b_.CreateLoad(b_.getInt32Ty(), lenPtr, "vlen");
      v.len = b_.CreateSExt(len, b_.getInt64Ty(), "vlen64");
      // Get the data pointer (index 1 of the struct)
      v.ptr = b_.CreateStructGEP(llvmTy(el), elemAddr, 1, "vdata");
      return v;
    }
    llvm::Value* base = locPtr ? emitExpr(locPtr).reg : addressOf(sym);
    v.ptr = arrayElementAddr(sym->ty, base, idxs, loc, dynUb_.count(sym) ? dynUb_[sym] : nullptr,
                             dynLb_.count(sym) ? dynLb_[sym] : nullptr);
    v.len = i64(el.len);
    return v;
  }
  if (isCtlDynArray(sym)) {
    llvm::Value* addr = ctlDynElementAddr(sym, idxs, loc);
    llvm::Value* r = b_.CreateLoad(llvmTy(el), addr, "ald");
    if (el.isBit() && el.len == 1)
      v.reg = b_.CreateTrunc(r, b_.getInt1Ty(), "b1");
    else
      v.reg = r;
    return v;
  }
  if (isCtlNDynArray(sym)) {
    llvm::Value* addr = ctlNDElementAddr(sym, idxs, loc);
    llvm::Value* r = b_.CreateLoad(llvmTy(el), addr, "ald");
    if (el.isBit() && el.len == 1)
      v.reg = b_.CreateTrunc(r, b_.getInt1Ty(), "b1");
    else
      v.reg = r;
    return v;
  }
  // A locator-qualified subscript P->X(i) (rules 124,126) addresses off the
  // locator value, not the declared BASED pointer.
  llvm::Value* base = locPtr ? emitExpr(locPtr).reg : addressOf(sym);
  llvm::Value* addr =
      arrayElementAddr(sym->ty, base, idxs, loc, dynUb_.count(sym) ? dynUb_[sym] : nullptr,
                       dynLb_.count(sym) ? dynLb_[sym] : nullptr);
  llvm::Value* r = b_.CreateLoad(llvmTy(el), addr, "ald");
  if (el.isBit() && el.len == 1)
    v.reg = b_.CreateTrunc(r, b_.getInt1Ty(), "b1");
  else
    v.reg = r;
  return v;
}

// Store one array element (rule 126): convert to the element type, then store
// through the bounds-checked element address.
void IRGen::storeArrayElement(Symbol* sym, const std::vector<HExprP>& idxs, const Val& src,
                              SourceLoc loc, HExpr* locPtr) {
  const Type& el = sym->ty.isArray() ? sym->ty.elementType() : sym->ty;
  if (el.isChar()) {
    // Fixed CHARACTER element (rules (12),(18)): blank-pad/truncate the source
    // into the element's bytes. A VARYING element uses pli_assign_varying.
    if (el.varying) {
      llvm::Value* base = locPtr ? emitExpr(locPtr).reg : addressOf(sym);
      llvm::Value* elemAddr =
          arrayElementAddr(sym->ty, base, idxs, loc, dynUb_.count(sym) ? dynUb_[sym] : nullptr,
                           dynLb_.count(sym) ? dynLb_[sym] : nullptr);
      // Get the data pointer (index 1 of the struct)
      llvm::Value* dp = b_.CreateStructGEP(llvmTy(el), elemAddr, 1, "vdata");
      // Get the length prefix pointer (index 0 of the struct)
      llvm::Value* lp = b_.CreateStructGEP(llvmTy(el), elemAddr, 0, "vlenp");
      // Use pli_assign_varying to store the varying string
      Val cv = convert(src, el, loc);
      llvm::Value* written = emitAssignVarying(dp, i64(el.len), cv.ptr, cv.len);
      // Store the returned length into the length prefix
      b_.CreateStore(b_.CreateTrunc(written, b_.getInt32Ty(), "vlen32"), lp);
      return;
    }
    llvm::Value* base = locPtr ? emitExpr(locPtr).reg : addressOf(sym);
    llvm::Value* addr =
        arrayElementAddr(sym->ty, base, idxs, loc, dynUb_.count(sym) ? dynUb_[sym] : nullptr,
                         dynLb_.count(sym) ? dynLb_[sym] : nullptr);
    storeCharTo(addr, el, src, loc);
    return;
  }
  if (isCtlDynArray(sym)) {
    llvm::Value* addr = ctlDynElementAddr(sym, idxs, loc);
    Val cv = convert(src, el, loc);
    storeScalarTo(addr, el, cv);
    return;
  }
  if (isCtlNDynArray(sym)) {
    llvm::Value* addr = ctlNDElementAddr(sym, idxs, loc);
    Val cv = convert(src, el, loc);
    storeScalarTo(addr, el, cv);
    return;
  }
  // A locator-qualified target P->X(i) = e (rules 124,126) stores off the
  // locator value, not the declared BASED pointer.
  llvm::Value* base = locPtr ? emitExpr(locPtr).reg : addressOf(sym);
  llvm::Value* addr =
      arrayElementAddr(sym->ty, base, idxs, loc, dynUb_.count(sym) ? dynUb_[sym] : nullptr,
                       dynLb_.count(sym) ? dynLb_[sym] : nullptr);
  Val cv = convert(src, el, loc);
  storeScalarTo(addr, el, cv);
}

// Copy a slice (rule 126) between two array views `t` and `x`. Either side may
// be a cross-section A(<fixed|*>, ...) denoting a reduced-rank view; a whole
// array is the all-'*' case. The fixed axes' indices are evaluated and
// bounds-checked once; the '*' axes are walked by linear row-major index,
// decomposing each position into star-axis coordinates and mapping them to both
// views' flat offsets (a single '*' is the row/column case of this same affine
// gather). A scalar value broadcasts over the target slice.
void IRGen::emitCrossSectionAssign(HExpr* t, HExpr* x, SourceLoc loc) {
  // A view over an array or cross-section: full type and base, the positions of
  // the '*' axes, the flat offset accumulated from the fixed axes, the full
  // row-major strides and the reduced extent (product of the star extents).
  struct View {
    const Type* arr = nullptr;
    llvm::Value* base = nullptr;
    std::vector<size_t> stars;
    llvm::Value* fixedFlat = nullptr;
    std::vector<long long> stride;
    long long total = 1;
    bool packed = false;
  };
  auto build = [&](HExpr* e) -> View {
    View v;
    v.packed = !e->memberPath.empty() && packedMemberPath(e->sym->ty, e->memberPath);
    v.arr = e->memberPath.empty() ? &e->sym->ty : &memberType(e->sym, e->memberPath);
    v.base = e->memberPath.empty() ? addressOf(e->sym) : memberAddr(e->sym, e->memberPath, loc);
    const size_t n = v.arr->dims.size();
    v.stride.resize(n);
    for (long long s = 1, k = (long long)n; k-- > 0;) {
      v.stride[k] = s;
      s *= (v.arr->dims[k].ub - v.arr->dims[k].lb + 1);
    }
    // A whole array (VarRef) has every axis as a star; a cross-section marks
    // them explicitly and folds each fixed axis into the flat offset.
    const bool whole = e->kind != HExpr::Subscript;
    v.fixedFlat = i64(0);
    for (size_t k = 0; k < n; ++k) {
      if (whole || e->args[k]->kind == HExpr::Star) {
        v.stars.push_back(k);
        v.total *= (v.arr->dims[k].ub - v.arr->dims[k].lb + 1);
        continue;
      }
      llvm::Value* iv = toI64(emitExpr(e->args[k].get()));
      llvm::Value* lb = i64(v.arr->dims[k].lb);
      llvm::Value* ub = i64(v.arr->dims[k].ub);
      llvm::Value* oob =
          b_.CreateOr(b_.CreateICmpSLT(iv, lb, "lo"), b_.CreateICmpSGT(iv, ub, "hi"), "oob");
      // A handled slip resumes with the index clamped into range (rule 94);
      // (NOSUBSCRIPTRANGE) trusts the raw index and emits no check.
      llvm::Value* civ = subChecks() ? clampIndex(iv, lb, ub) : iv;
      if (subChecks()) {
        std::string fid = std::to_string(n_++);
        llvm::BasicBlock* failL = llvm::BasicBlock::Create(ctx_, "cs.fail." + fid, curFn_);
        llvm::BasicBlock* okL = llvm::BasicBlock::Create(ctx_, "cs.ok." + fid, curFn_);
        b_.CreateCondBr(oob, failL, okL);
        startBlock(failL);
        emitCondTrap(Stmt::kSubscriptrangeCondKey, "pli_subscript_oob", "sub", okL);
        startBlock(okL);
      }
      v.fixedFlat = b_.CreateAdd(
          v.fixedFlat, b_.CreateMul(b_.CreateSub(civ, lb, "off"), i64(v.stride[k]), "scaled"),
          "ff");
    }
    return v;
  };

  const View tv = build(t);
  const Type& el = tv.arr->elementType(); // sema required both sides same shape/type
  const size_t m = tv.stars.size();
  // Reduced row-major strides of the '*' axes let one linear index be
  // decomposed into star-axis coordinates.
  std::vector<long long> rstride(m);
  for (long long s = 1, j = (long long)m; j-- > 0;) {
    rstride[j] = s;
    s *= (tv.arr->dims[tv.stars[j]].ub - tv.arr->dims[tv.stars[j]].lb + 1);
  }
  const long long total = tv.total;
  llvm::Type* tgtArrTy = llvm::ArrayType::get(llvmTy(el), (unsigned)arrayExtent(*tv.arr));

  // A scalar value broadcasts over the target slice; otherwise gather element by
  // element from the source view (same shape, possibly different rank/layout).
  const bool broadcast = !x->ty.isArray();
  View sv;
  llvm::Type* srcArrTy = nullptr;
  Val scalar;
  if (broadcast) {
    scalar = emitExpr(x);
  } else {
    sv = build(x);
    srcArrTy = llvm::ArrayType::get(llvmTy(el), (unsigned)arrayExtent(*sv.arr));
  }

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

  // Target flat offset for this linear position.
  llvm::Value* tgtFlat = tv.fixedFlat;
  llvm::Value* trem = ti;
  for (size_t j = 0; j < m; ++j) {
    llvm::Value* coord = b_.CreateUDiv(trem, i64(rstride[j]), "csc");
    trem = b_.CreateURem(trem, i64(rstride[j]), "csr");
    tgtFlat = b_.CreateAdd(tgtFlat, b_.CreateMul(coord, i64(tv.stride[tv.stars[j]]), "csm"), "css");
  }

  Val val = scalar;
  if (!broadcast) {
    // Source flat offset for the same star-axis coordinates. All offsets are
    // non-negative (fixed axes are bounds-checked, coordinates run 0..extent-1),
    // so the arithmetic is unsigned.
    llvm::Value* srcFlat = sv.fixedFlat;
    llvm::Value* srem = ti;
    for (size_t j = 0; j < m; ++j) {
      llvm::Value* coord = b_.CreateUDiv(srem, i64(rstride[j]), "cssc");
      srem = b_.CreateURem(srem, i64(rstride[j]), "cssr");
      srcFlat =
          b_.CreateAdd(srcFlat, b_.CreateMul(coord, i64(sv.stride[sv.stars[j]]), "csms"), "csss");
    }
    llvm::Value* saddr = b_.CreateInBoundsGEP(srcArrTy, sv.base, {i64(0), srcFlat}, "csrc");
    llvm::LoadInst* lr = b_.CreateLoad(llvmTy(el), saddr, "csl");
    if (sv.packed)
      lr->setAlignment(llvm::Align(1));
    val.ty = el;
    val.reg = el.isBit() && el.len == 1 ? b_.CreateTrunc(lr, b_.getInt1Ty(), "csb") : lr;
  }
  llvm::Value* taddr = b_.CreateInBoundsGEP(tgtArrTy, tv.base, {i64(0), tgtFlat}, "cdst");
  storeScalarTo(taddr, el, convert(val, el, loc), tv.packed);
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
llvm::Value* IRGen::definedSubElementAddr(Symbol* y, const std::vector<HExprP>& idxs, SourceLoc) {
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
  // A handled slip resumes with the base index clamped into range (rule 94);
  // (NOSUBSCRIPTRANGE) trusts the raw index and emits no check.
  llvm::Value* cbidx = subChecks() ? clampIndex(bidx, i64(ilb), i64(iub)) : bidx;
  if (subChecks()) {
    std::string id = std::to_string(n_++);
    llvm::BasicBlock* failL = llvm::BasicBlock::Create(ctx_, "def.fail." + id, curFn_);
    llvm::BasicBlock* okL = llvm::BasicBlock::Create(ctx_, "def.ok." + id, curFn_);
    b_.CreateCondBr(oob, failL, okL);
    startBlock(failL);
    emitCondTrap(Stmt::kSubscriptrangeCondKey, "pli_subscript_oob", "sub", okL);
    startBlock(okL);
  }

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

  // A locator value is passed through unchanged between POINTER/OFFSET targets
  // (rules (15),(22)): assignment copies the address, no numeric conversion.
  if (v.ty.isLocator() && dst.isLocator())
    return v;

  // ENTRY values (IBM extension, ADR-171): a procedure pointer copies through.
  if (v.ty.isEntry() && dst.isEntry())
    return v;

  // TASK/EVENT handles (rules (15),(79),(82), QR2.8): same-type copies pass
  // through; mixing them with anything else stays diagnosed by sema.
  if (v.ty.isTask() && dst.isTask())
    return v;
  if (v.ty.isEvent() && dst.isEvent())
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
    if (dst.len == 1) {
      // BIT(1) keeps truthiness semantics, including numeric sources.
      out.reg = toI1(v, loc);
      return out;
    }
    if (v.ty.isBit()) {
      if (v.ty.len == dst.len) {
        out = v;
        return out;
      }
      // Bit-to-bit copies bytes directly (pad/truncate on the right); an
      // integer round-trip would destroy string positions.
      int SB = bitBytes(v.ty.len), DB = bitBytes(dst.len);
      llvm::Type* arrTy = llvm::ArrayType::get(b_.getInt8Ty(), (unsigned)DB);
      llvm::Value* agg = llvm::Constant::getNullValue(arrTy);
      for (int k = 0; k < SB && k < DB; ++k) {
        llvm::Value* byte = b_.CreateExtractValue(v.reg, (unsigned)k, "bcx");
        agg = b_.CreateInsertValue(agg, byte, (unsigned)k, "bci");
      }
      int keep = dst.len - 8 * (DB - 1);
      if (keep < 8) {
        llvm::Value* last = b_.CreateExtractValue(agg, (unsigned)(DB - 1), "bcl");
        last = b_.CreateAnd(last, b_.getInt8((int8_t)(0xFF << (8 - keep))), "bcm");
        agg = b_.CreateInsertValue(agg, last, (unsigned)(DB - 1), "bcw");
      }
      out.reg = agg;
      return out;
    }
    // An integer source contributes its low bits; a float truncates toward
    // zero first (range-checked like other float conversions).
    llvm::Value* iv = nullptr;
    if (v.ty.k == TK::Float) {
      floatRangeTrap(v.reg, -9223372036854775808.0, true, 9223372036854775808.0);
      iv = b_.CreateFPToSI(v.reg, b_.getInt64Ty(), "biv");
    } else if (v.ty.intBits() == 64) {
      iv = v.reg;
    } else {
      iv = b_.CreateZExt(v.reg, b_.getInt64Ty(), "biv");
    }
    out.reg = unpackBitValue(iv, dst.len, loc);
    return out;
  }
  if (srcBit) { // BIT -> arithmetic takes the binary value, big-endian
    llvm::Value* iv = v.ty.len == 1 ? b_.CreateZExt(v.reg, b_.getInt64Ty(), "biv")
                                    : packBitValue(v.reg, v.ty.len, loc);
    if (dstFloat) {
      out.reg = b_.CreateSIToFP(iv, b_.getDoubleTy(), "cvt");
    } else if (llvmTy(dst)->getIntegerBitWidth() == 64) {
      out.reg = iv;
    } else {
      out.reg = b_.CreateTrunc(iv, llvmTy(dst), "cvt");
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
        // Wide (>18 digit) DECIMAL target (ADR-191): the value may legitimately
        // exceed i64 range (e.g. DECIMAL(25,2) up to ~3.3e24), so bound the
        // FP->SI conversion by the declared precision, not 2^63.
        floatRangeTrap(f, -pli_to_double(pliPow10_128(dst.prec)), false,
                       pli_to_double(pliPow10_128(dst.prec)));
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
        // Trap at the source's native width; magTrap sign-extends the i64
        // limit to i128 for a wide (>18 digit) source (ADR-191) so an i128
        // value is never truncated into a narrower compare.
        unsigned srcBits = v.reg->getType()->getIntegerBitWidth();
        llvm::Value* w =
            (srcBits == 64)
                ? v.reg
                : (srcBits == 32 ? b_.CreateSExt(v.reg, b_.getInt64Ty(), "cvtw") : v.reg);
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
    // Work in the wider of the source/destination widths so an i128 value
    // (ADR-191: >18 digit DECIMAL) is never narrowed before the rescale/magTrap
    // reads it. The result is truncated to dst's width at the end.
    bool wide = std::max(v.ty.intBits(), dst.intBits()) == 128;
    llvm::Value* r = b_.CreateSExt(v.reg, wide ? b_.getInt128Ty() : b_.getInt64Ty(), "res");
    int dq = dst.k == TK::FixedDec ? dst.scale - v.ty.scale : -v.ty.scale;
    if (dq > 0) {
      // A scale-up that wraps already exceeds any target: trap, so the
      // magnitude check below never reads a wrapped value.
      r = checkedArith(Tok::Star, r, wide ? i128(pliPow10_128(dq)) : i64(pliPow10(dq)));
    } else if (dst.k == TK::FixedDec) {
      // DECIMAL->DECIMAL assignment rounds half away from zero (Y33-6003).
      int k = -dq;
      if (wide) {
        llvm::Value* div = i128(pliPow10_128(k));
        llvm::Value* sign = b_.CreateSelect(b_.CreateICmpSLT(r, i128(0), "sgn"), i128(-1), i128(1));
        llvm::Value* adj = b_.CreateMul(i128(pliPow10_128(k - 1) * 5), sign, "adj");
        r = b_.CreateSDiv(b_.CreateAdd(r, adj, "rn"), div, "res");
      } else {
        llvm::Value* div = i64(pliPow10(k));
        llvm::Value* sign = b_.CreateSelect(b_.CreateICmpSLT(r, i64(0), "sgn"), i64(-1), i64(1));
        llvm::Value* adj = b_.CreateMul(i64(pliPow10(k - 1) * 5), sign, "adj");
        r = b_.CreateSDiv(b_.CreateAdd(r, adj, "rn"), div, "res");
      }
    } else {
      // DECIMAL->BINARY assignment truncates the fraction toward zero.
      r = b_.CreateSDiv(r, i64(pliPow10(-dq)), "res");
    }
    if (needDec || needBin)
      magTrap(r, limit);
    unsigned dstBits = (unsigned)dst.intBits();
    unsigned rBits = r->getType()->getIntegerBitWidth();
    if (rBits == dstBits)
      out.reg = r;
    else if (rBits < dstBits)
      out.reg = b_.CreateSExt(r, llvmTy(dst), "cvt");
    else
      out.reg = b_.CreateTrunc(r, llvmTy(dst), "cvt");
    return out;
  }
  return out;
}

llvm::Value* IRGen::toI1(const Val& v, SourceLoc loc) {
  if (v.ty.isBit()) {
    if (v.ty.len == 1)
      return v.reg;
    // A wider string is true when any byte is nonzero.
    llvm::Value* acc = b_.getInt1(false);
    for (int k = 0, B = bitBytes(v.ty.len); k < B; ++k) {
      llvm::Value* byte = b_.CreateExtractValue(v.reg, (unsigned)k, "tstx");
      llvm::Value* nz = b_.CreateICmpNE(byte, b_.getInt8(0), "tstnz");
      acc = b_.CreateOr(acc, nz, "tstor");
    }
    return acc;
  }
  if (v.ty.k == TK::Float)
    return b_.CreateFCmpUNE(v.reg, flt(0.0), "tst");
  if (v.ty.isNumeric())
    return b_.CreateICmpNE(v.reg, llvm::Constant::getNullValue(llvmTy(v.ty)), "tst");
  d_.error(loc, "value of type " + v.ty.desc() + " cannot be used as a condition", "(75)");
  return b_.getInt1(false);
}

llvm::Value* IRGen::toI64(const Val& v, SourceLoc loc) {
  if (v.ty.k == TK::Float)
    return b_.CreateFPToSI(v.reg, b_.getInt64Ty(), "i64");
  if (v.ty.isBit())
    return v.ty.len == 1 ? b_.CreateZExt(v.reg, b_.getInt64Ty(), "i64")
                         : packBitValue(v.reg, v.ty.len, loc);
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

Val IRGen::charOf(HExpr* e) {
  Val v = emitExpr(e);
  if (v.ty.isChar())
    return v;
  Val dst = charTemp(24);
  if (e->ty.k == TK::Float) {
    b_.CreateCall(runtimeFn("pli_char_of_float"), {dst.ptr, dst.len, v.reg});
  } else {
    llvm::Value* iv = toI64(convert(v, Type::fixedBin(31, 0), e->loc));
    b_.CreateCall(runtimeFn("pli_char_of_fixed"), {dst.ptr, dst.len, iv});
  }
  return dst;
}

// Hidden result buffer for a character function (rules (34),(37)): a
// `CHAR(*) VARYING` result rides a caller-sized max so short static
// descriptors (len 1) never truncate the returned value.
llvm::Type* IRGen::sretBufTy(const Type& rty) {
  if (rty.isChar() && rty.starLen) {
    if (rty.varying) {
      llvm::Type* data = llvm::ArrayType::get(b_.getInt8Ty(), (unsigned)kStarRetMax);
      return llvm::StructType::get(b_.getInt32Ty(), data);
    }
    return llvm::ArrayType::get(b_.getInt8Ty(), (unsigned)kStarRetMax);
  }
  return llvmTy(rty);
}

llvm::Value* IRGen::sretAlloc(const Type& rty, const char* name) {
  llvm::Value* buf = entryAlloca(sretBufTy(rty), name);
  if (rty.isChar() && rty.starLen && rty.varying) {
    // A fresh VARYING result starts empty; the callee sets the live length.
    llvm::Value* lp = b_.CreateStructGEP(sretBufTy(rty), buf, 0, "slenp");
    b_.CreateStore(b_.getInt32(0), lp);
  }
  return buf;
}

// ---------------------------------------------------------------------------
// expressions
// ---------------------------------------------------------------------------
Val IRGen::emitExpr(HExpr* e) {
  Val v;
  if (!e)
    return v;
  switch (e->kind) {
  case HExpr::Convert: {
    HExpr* src = e->a.get();
    // Fast path (P1, Task 6): FIXED / FIXED → FIXED uses sdiv directly,
    // skipping the FDiv float round-trip (FDiv + FPTosi). Only valid when the
    // FLOAT result is immediately narrowed back to a scale-0 FIXED target —
    // that conversion context is visible here in the Convert node, not in the
    // Binary/Slash case (which always yields FLOAT per PL/I). For FLOAT
    // results the fall-through below emits exact FDiv.
    if (e->convTo.isFixed() && e->convTo.scale == 0 && src && src->kind == HExpr::Binary &&
        src->op == Tok::Slash) {
      Val av = emitExpr(src->a.get());
      Val bv = emitExpr(src->b.get());
      if (av.ty.isFixed() && bv.ty.isFixed() && av.ty.scale == 0 && bv.ty.scale == 0) {
        Val v;
        v.ty = e->convTo;
        llvm::Value* ai = toI64(av, e->loc);
        llvm::Value* bi = toI64(bv, e->loc);
        llvm::Value* q;
        // ZERODIVIDE (rule 94): a zero divisor traps, resuming with 0.
        // Guard constant-zero divisor: CreateSDiv(x, 0) is UB, so emit
        // zerodivideResume with a const-true condition and value 0.
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(bi)) {
          if (ci->isZero()) {
            q = zerodivideResume(b_.getTrue(), i64(0), i64(0));
          } else {
            q = b_.CreateSDiv(ai, bi, "bin");
          }
        } else {
          // UB-safe: sdiv by zero is UB, so the raw sdiv must not execute
          // with a zero divisor even speculatively — LLVM would otherwise
          // assume dz==false and delete the trap. Divide by a safe divisor
          // (1 when dz) and discard via the resume select.
          llvm::Value* dz = b_.CreateICmpEQ(bi, i64(0), "zdiv");
          llvm::Value* safe = b_.CreateSelect(dz, i64(1), bi, "zdiv.safe");
          q = b_.CreateSDiv(ai, safe, "bin");
          q = zerodivideResume(dz, q, i64(0));
        }
        v.reg = convert(Val{Type::fixedBin(63, 0), q}, e->convTo, e->loc).reg;
        return v;
      }
    }
    return convert(emitExpr(e->a.get()), e->convTo, e->loc);
  }
  case HExpr::IntLit:
    v.ty = e->ty;
    v.reg = llvm::ConstantInt::get(llvmTy(e->ty), e->ival, true);
    return v;
  case HExpr::DecLit:
    v.ty = e->ty;
    // Wide DECIMAL literal (ADR-191): the scaled value exceeds i64, so the
    // i64 ival truncation would wrap; emit the full i128 wideIval instead.
    if (needsWideDecLit(e, e->ty))
      v.reg = i128(e->wideIval);
    else
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
  case HExpr::BitLit: {
    int n = e->ty.isBit() && e->ty.len > 0 ? e->ty.len : 1;
    if (n == 1) {
      v.ty = Type::bit(1);
      v.reg = b_.getInt1(!e->sval.empty() && e->sval[0] == '1');
      return v;
    }
    std::vector<unsigned char> bytes = packBitLiteral(e->sval, n);
    v.ty = Type::bit(n);
    v.reg = llvm::ConstantDataArray::get(ctx_, bytes);
    return v;
  }
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
        llvm::LoadInst* r = b_.CreateLoad(llvmTy(el), addr, "aosld");
        if (packedMemberPath(e->sym->ty.elementType(), e->memberPath))
          r->setAlignment(llvm::Align(1));
        v.ty = el;
        v.reg = el.isBit() && el.len == 1 ? b_.CreateTrunc(r, b_.getInt1Ty(), "b1") : r;
        return v;
      }
      // A subscripted member array S.A(i) (rules 124,126): the member array
      // lives at memberAddr(...) (a [N x elemTy] field, or a buffer pointer for
      // a dynamic member), so GEP into it as a normal array and load the leaf.
      const Type& arr = memberType(e->sym, e->memberPath);
      const Type& el = e->ty;
      llvm::Value *ub = nullptr, *lb = nullptr;
      llvm::Value* base = arr.isDynamic() ? dynamicMemberBase(e->sym, e->memberPath, e->loc, ub, lb)
                                          : memberAddr(e->sym, e->memberPath, e->loc);
      llvm::Value* addr = arrayElementAddr(arr, base, e->args, e->loc, ub, lb);
      bool packed = packedMemberPath(e->sym->ty, e->memberPath);
      v.ty = el;
      if (el.isChar()) {
        if (el.varying) {
          llvm::Value* dp = b_.CreateStructGEP(llvmTy(el), addr, 1, "mvdata");
          llvm::Value* lp = b_.CreateStructGEP(llvmTy(el), addr, 0, "mvlenp");
          llvm::LoadInst* l32 = b_.CreateLoad(b_.getInt32Ty(), lp, "ml32");
          if (packed)
            l32->setAlignment(llvm::Align(1));
          v.ptr = dp;
          v.len = b_.CreateSExt(l32, b_.getInt64Ty(), "ml64");
        } else {
          v.ptr = addr;
          v.len = i64(el.len);
        }
        return v;
      }
      llvm::LoadInst* r = b_.CreateLoad(llvmTy(el), addr, "mald");
      if (packed)
        r->setAlignment(llvm::Align(1));
      v.reg = el.isBit() && el.len == 1 ? b_.CreateTrunc(r, b_.getInt1Ty(), "b1") : r;
      return v;
    }
    // An iSUB-DEFINED array Y (rule 134) has no storage of its own: Y(k) is a
    // live overlay of a base element, so route the address to the base X.
    if (e->sym->definedBase && e->sym->definedIsubAxis >= 0) {
      llvm::Value* addr = definedSubElementAddr(e->sym, e->args, e->loc);
      const Type& el = e->ty;
      llvm::Value* r = b_.CreateLoad(llvmTy(el), addr, "defl");
      v.ty = el;
      v.reg = el.isBit() && el.len == 1 ? b_.CreateTrunc(r, b_.getInt1Ty(), "b1") : r;
      return v;
    }
    return loadArrayElement(e->sym, e->args, e->loc, e->locPtr.get());
  case HExpr::VarRef:
    if (!e->sym) {
      v.ty = e->ty;
      v.reg = i64(0);
      return v;
    }
    if (e->sym->kind == Symbol::ProcName) {
      // A procedure name used as a procedure value (IBM ENTRY VARIABLE
      // extension, ADR-171): its address. A procedure that captures enclosing
      // state needs a static link a raw function pointer cannot carry, so it is
      // diagnosed rather than silently mis-called.
      v.ty = Type::entryTy();
      if (e->sym->proc && !e->sym->proc->env.empty()) {
        d_.error(e->loc,
                 "assigning a procedure that captures enclosing state to an ENTRY value is not "
                 "implemented (ADR-171)",
                 "(8)");
        v.reg = llvm::Constant::getNullValue(b_.getPtrTy());
        return v;
      }
      llvm::Function* f = calleeFn(e->sym);
      v.reg = f ? (llvm::Value*)f : llvm::Constant::getNullValue(b_.getPtrTy());
      return v;
    }
    if (!e->memberPath.empty() || e->locPtr) {
      // Qualified member S.A.B (rule 124): load the leaf member. A
      // locator-qualified whole reference P->X (empty member path) loads
      // through the locator value as well.
      const Type& leaf = e->ty;
      // A locator-qualified member P->X.FIELD (rule 124) GEPs off the loaded
      // pointer value; otherwise off the based/symbol member address.
      llvm::Value* addr =
          e->locPtr ? locatorMemberAddr(e->sym, e->memberPath, emitExpr(e->locPtr.get()).reg)
                    : memberAddr(e->sym, e->memberPath, e->loc);
      bool packed = packedMemberPath(e->sym->ty, e->memberPath);
      v.ty = leaf;
      if (leaf.isChar()) {
        if (leaf.varying) {
          llvm::Value* dp = b_.CreateStructGEP(llvmTy(leaf), addr, 1, "mvdata");
          llvm::Value* lp = b_.CreateStructGEP(llvmTy(leaf), addr, 0, "mvlenp");
          llvm::LoadInst* l32 = b_.CreateLoad(b_.getInt32Ty(), lp, "ml32");
          if (packed)
            l32->setAlignment(llvm::Align(1));
          v.ptr = dp;
          v.len = b_.CreateSExt(l32, b_.getInt64Ty(), "ml64");
        } else {
          v.ptr = addr;
          v.len = i64(leaf.len);
        }
        return v;
      }
      v.ty = leaf;
      if (leaf.isStruct()) {
        // A whole minor structure member (rule 127): its value is its address.
        v.ptr = addr;
        return v;
      }
      llvm::LoadInst* r = b_.CreateLoad(llvmTy(leaf), addr, "mld");
      if (packed)
        r->setAlignment(llvm::Align(1));
      v.reg = leaf.isBit() && leaf.len == 1 ? b_.CreateTrunc(r, b_.getInt1Ty(), "b1") : r;
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
    if (e->sym->isEntryVar) {
      // An indirect function reference through an entry variable (IBM
      // extension, ADR-171): load the stored pointer and call it.
      llvm::Value* sretPtr = nullptr;
      llvm::CallInst* call = emitEntryCall(e->sym, e->args, e->loc, &sretPtr);
      Type rty = e->sym->entryIsFunction ? e->sym->entryRetTy : Type::voidTy();
      v.ty = rty;
      if (rty.isStruct()) {
        v.ptr = sretPtr;
      } else if (rty.isChar()) {
        // Struct/char returns stay diagnosed in sema; handled defensively.
        if (rty.varying) {
          llvm::Value* lp = b_.CreateStructGEP(sretBufTy(rty), sretPtr, 0, "clenp");
          v.ptr = b_.CreateStructGEP(sretBufTy(rty), sretPtr, 1, "cdata");
          v.len =
              b_.CreateSExt(b_.CreateLoad(b_.getInt32Ty(), lp, "cl32"), b_.getInt64Ty(), "cl64");
        } else {
          v.ptr = sretPtr;
          v.len = i64(rty.len);
        }
      } else if (!rty.isVoid()) {
        v.reg = rty.isBit() ? b_.CreateTrunc(call, b_.getInt1Ty(), "fb") : call;
      } else {
        v.reg = i64(0);
      }
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
      // An external character-valued function (rules (34),(37)) returns
      // through a hidden result buffer, exactly like a locally-defined
      // function procedure: allocate the buffer, pass it as the first
      // argument, and return its address/length to the caller.
      std::vector<llvm::Value*> args;
      llvm::Value* sretPtr = nullptr;
      if (rty.isChar()) {
        sretPtr = sretAlloc(rty);
        args.push_back(sretPtr);
      }
      for (size_t i = 0; i < e->args.size() && i < e->sym->entryParams.size(); ++i) {
        HExpr* a = e->args[i].get();
        Type pty = e->sym->entryParams[i];
        // A by-value C entry (rules (34),(38)) takes scalar/pointer values.
        if (e->sym->entryByValue)
          args.push_back(marshalArg(a, pty, e->loc));
        else
          args.push_back(argAddr(a, pty));
      }
      // Hidden extent/length args for the `*`-extent array and CHAR(*)
      // params of a plain-EXTERNAL PL/I entry (rules (13),(18)): the same
      // convention as for a locally-defined procedure. By-value C entries
      // carry none (lengths are explicit params there).
      if (!e->sym->entryByValue)
        for (size_t i = 0; i < e->sym->entryParams.size(); ++i) {
          const Type& t = e->sym->entryParams[i];
          if (!((t.isArray() && !t.dims.empty() && t.dims[0].adj) || (t.isChar() && t.starLen)))
            continue;
          HExpr* a =
              (i >= e->args.size() || e->args[i]->kind == HExpr::Star) ? nullptr : e->args[i].get();
          llvm::Value* ext = hiddenAdjustArg(a, t, t.controlled);
          if (!ext) {
            d_.error(e->args[i]->loc,
                     "a '*' extent parameter takes a fixed or dynamic-bound array in this stage",
                     "(13)");
            ext = i64(0);
          }
          args.push_back(ext);
        }
      if (!e->sym->entryIsFunction) {
        b_.CreateCall(extFn, args);
        flushVarWrites();
        v.ty = e->ty;
        v.reg = i64(0);
        return v;
      }
      if (rty.isChar()) {
        // The character result lives in the caller's buffer (the sret); the
        // callee returns void, so the call carries no result value name.
        b_.CreateCall(extFn, args);
        flushVarWrites();
        v.ty = rty;
        if (rty.varying) {
          llvm::Value* lp = b_.CreateStructGEP(sretBufTy(rty), sretPtr, 0, "clenp");
          v.ptr = b_.CreateStructGEP(sretBufTy(rty), sretPtr, 1, "cdata");
          v.len =
              b_.CreateSExt(b_.CreateLoad(b_.getInt32Ty(), lp, "cl32"), b_.getInt64Ty(), "cl64");
        } else if (rty.starLen) {
          v.ptr = sretPtr;
          v.len = i64(kStarRetMax);
        } else {
          v.ptr = sretPtr;
          v.len = i64(rty.len);
        }
        return v;
      }
      llvm::CallInst* call = b_.CreateCall(extFn, args, "fres");
      flushVarWrites();
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
    std::vector<llvm::Value*> args;
    llvm::Value* sretPtr = nullptr;
    if (rty.isStruct() || rty.isChar()) {
      // Hidden-buffer result (rules 127 and (34),(37)): the caller allocates
      // the result buffer and passes its address as the first argument.
      sretPtr = rty.isChar() ? sretAlloc(rty) : entryAlloca(llvmTy(rty), "sret");
      args.push_back(sretPtr);
    }
    for (size_t i = 0; i < e->args.size(); ++i) {
      HExpr* a = e->args[i].get();
      Type pty;
      if (i < calleeParams.size())
        pty = calleeParams[i]->ty;
      else
        break;
      if (a->kind == HExpr::Star) {
        // Omitted OPTIONAL (extension, ADR-119): null pointer, as in emitCall.
        args.push_back(llvm::Constant::getNullValue(b_.getPtrTy()));
        continue;
      }
      args.push_back(argAddr(a, pty));
    }
    // Trailing omitted OPTIONALs pad with nulls to the full signature.
    for (size_t i = e->args.size(); i < calleeParams.size(); ++i)
      args.push_back(llvm::Constant::getNullValue(b_.getPtrTy()));
    for (size_t i = 0; i < calleeParams.size(); ++i)
      if (isAdjustable(calleeParams[i])) {
        // An omitted '*' (or left-out trailing OPTIONAL) takes a zero extent
        // (or the -1 fresh-stack key for a CONTROLLED dummy).
        HExpr* a =
            (i >= e->args.size() || e->args[i]->kind == HExpr::Star) ? nullptr : e->args[i].get();
        llvm::Value* ext = hiddenAdjustArg(a, calleeParams[i]->ty, calleeParams[i]->controlled);
        if (!ext) {
          d_.error(e->args[i]->loc,
                   "a '*' extent parameter takes a fixed or dynamic-bound array in this stage",
                   "(13)");
          ext = i64(0);
        }
        args.push_back(ext);
      }
    appendStaticLinks(callee, args);
    // A hidden-buffer callee returns void (the result is written to the
    // buffer), so the call cannot carry a value name.
    llvm::CallInst* call = (rty.isStruct() || rty.isChar()) ? b_.CreateCall(calleeFn, args)
                                                            : b_.CreateCall(calleeFn, args, "fres");
    flushVarWrites();
    v.ty = rty;
    if (rty.isStruct()) {
      v.ptr = sretPtr; // the result lives in the caller's buffer
    } else if (rty.isChar()) {
      // The character result lives in the caller's buffer: data pointer plus
      // live length (reloaded for VARYING, caller max for STAR, declared
      // otherwise).
      if (rty.varying) {
        llvm::Value* lp = b_.CreateStructGEP(sretBufTy(rty), sretPtr, 0, "clenp");
        v.ptr = b_.CreateStructGEP(sretBufTy(rty), sretPtr, 1, "cdata");
        v.len = b_.CreateSExt(b_.CreateLoad(b_.getInt32Ty(), lp, "cl32"), b_.getInt64Ty(), "cl64");
      } else if (rty.starLen) {
        v.ptr = sretPtr;
        v.len = i64(kStarRetMax);
      } else {
        v.ptr = sretPtr;
        v.len = i64(rty.len);
      }
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
      if (a.ty.isBit() && a.ty.len > 1) {
        // Bitwise NOT over the packed bytes, masking the unused low bits
        // of the last byte back to zero.
        int B = bitBytes(a.ty.len);
        llvm::Type* arrTy = llvm::ArrayType::get(b_.getInt8Ty(), (unsigned)B);
        llvm::Value* out = llvm::UndefValue::get(arrTy);
        for (int k = 0; k < B; ++k) {
          llvm::Value* byte = b_.CreateExtractValue(a.reg, (unsigned)k, "notx");
          llvm::Value* nb = b_.CreateXor(byte, b_.getInt8(-1), "notb");
          if (k == B - 1) {
            int keep = a.ty.len - 8 * (B - 1);
            if (keep < 8)
              nb = b_.CreateAnd(nb, b_.getInt8((int8_t)(0xFF << (8 - keep))), "notm");
          }
          out = b_.CreateInsertValue(out, nb, (unsigned)k, "noti");
        }
        v.ty = a.ty;
        v.reg = out;
        return v;
      }
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
      llvm::Value* lo = llvm::ConstantInt::get(ty, 1ULL << (bits - 1), true);
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
    // Flatten a concat chain A || B || C into one buffer with a single length
    // computation and sequential byte copies (P2, OPTIMIZATION §7.4) instead of
    // left-leaning temporaries. Leaves are evaluated in source order; the flat
    // result buffer is fresh, so no leaf storage can alias it. Any side may be
    // an adjustable `CHAR(*)` (rule (18)) / CONTROLLED generation (rule (15))
    // whose live length exceeds its static descriptor, so size from the live sum.
    std::vector<Val> leaves;
    std::vector<HExpr*> stack{e};
    while (!stack.empty()) {
      HExpr* ex = stack.back();
      stack.pop_back();
      if (ex->kind == HExpr::Binary && ex->op == Tok::Concat) {
        stack.push_back(ex->b.get());
        stack.push_back(ex->a.get());
        continue;
      }
      leaves.push_back(charOf(ex));
    }
    v.ty = e->ty;
    llvm::Value* total = leaves[0].len;
    for (size_t i = 1; i < leaves.size(); ++i)
      total = b_.CreateAdd(total, leaves[i].len, "clen");
    v.ptr = b_.CreateCall(runtimeFn("pli_alloc"), {total}, "cbuf");
    v.len = total;
    llvm::Value* off = i64(0);
    for (auto& leaf : leaves) {
      llvm::Value* dst = b_.CreateGEP(b_.getInt8Ty(), v.ptr, off, "cblk");
      b_.CreateMemCpy(dst, llvm::MaybeAlign(), leaf.ptr, llvm::MaybeAlign(), leaf.len);
      off = b_.CreateAdd(off, leaf.len, "coff");
    }
    return v;
  }

  if (op == Tok::Amp || op == Tok::Bar) { // rules (116),(115)
    Val a = emitExpr(e->a.get());
    Val b = emitExpr(e->b.get());
    if (a.ty.isBit() && b.ty.isBit() && (a.ty.len > 1 || b.ty.len > 1)) {
      // Bitwise logic over packed bytes; sema requires identical lengths.
      if (a.ty.len != b.ty.len) {
        d_.error(e->loc, "logical BIT operands must share one length in this stage", "(116)");
        v.ty = Type::bit(a.ty.len);
        v.reg = llvm::UndefValue::get(llvm::ArrayType::get(b_.getInt8Ty(), 1));
        return v;
      }
      int B = bitBytes(a.ty.len);
      llvm::Type* arrTy = llvm::ArrayType::get(b_.getInt8Ty(), (unsigned)B);
      llvm::Value* out = llvm::UndefValue::get(arrTy);
      for (int k = 0; k < B; ++k) {
        llvm::Value* x = b_.CreateExtractValue(a.reg, (unsigned)k, "andx");
        llvm::Value* y = b_.CreateExtractValue(b.reg, (unsigned)k, "andy");
        llvm::Value* r = op == Tok::Amp ? b_.CreateAnd(x, y, "and") : b_.CreateOr(x, y, "or");
        out = b_.CreateInsertValue(out, r, (unsigned)k, "andi");
      }
      v.ty = a.ty;
      v.reg = out;
      return v;
    }
    llvm::Value* ab = toI1(a, e->loc);
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
    llvm::Value* c = emitCmpChar(a.ptr, a.len, b.ptr, b.len);
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

  if (isCmp && a.ty.isLocator() && b.ty.isLocator()) {
    // POINTER/OFFSET equality/inequality (rules (22),(117)): compare the two
    // addresses directly; ordered comparisons were rejected in sema.
    llvm::CmpInst::Predicate pred = op == Tok::Eq ? llvm::CmpInst::ICMP_EQ : llvm::CmpInst::ICMP_NE;
    v.ty = Type::bit(1);
    v.reg = b_.CreateICmp(pred, a.reg, b.reg, "pcmp");
    return v;
  }

  if (isCmp && (a.ty.isComplex() || b.ty.isComplex())) {
    // Exact part-wise equality (CM5); ordered comparisons were diagnosed.
    Val ac = convert(a, Type::complexTy(), e->loc);
    Val bc = convert(b, Type::complexTy(), e->loc);
    llvm::Value* er =
        b_.CreateFCmp(llvm::CmpInst::FCMP_OEQ, b_.CreateExtractValue(ac.cpx, 0, "cpx.er"),
                      b_.CreateExtractValue(bc.cpx, 0, "cpx.er"), "cpx.er");
    llvm::Value* ei =
        b_.CreateFCmp(llvm::CmpInst::FCMP_OEQ, b_.CreateExtractValue(ac.cpx, 1, "cpx.ei"),
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
  // Power always promotes to FLOAT (rule 121, CM5). Division stays in the
  // type sema chose: FIXED for integer-valued operands (P3, ADR-190 — sdiv),
  // FLOAT for non-integer operands (fdiv).
  if (!isCmp && op == Tok::Power && !common.isComplex())
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
      llvm::Value* den = b_.CreateFAdd(b_.CreateFMul(br, br), b_.CreateFMul(bi, bi), "cpx.den");
      rr =
          b_.CreateFDiv(b_.CreateFAdd(b_.CreateFMul(ar, br), b_.CreateFMul(ai, bi)), den, "cpx.rr");
      ri =
          b_.CreateFDiv(b_.CreateFSub(b_.CreateFMul(ai, br), b_.CreateFMul(ar, bi)), den, "cpx.ri");
      break;
    }
    case Tok::Power: {
      // Complex exponentiation a**b = exp(b*log(a)) (rule 121, CM5).
      // log(a) = ln|a| + i*arg(a); b*log(a) done as complex multiply.
      llvm::Value* mag2 = b_.CreateFAdd(b_.CreateFMul(ar, ar), b_.CreateFMul(ai, ai), "cpx.mag2");
      llvm::Value* lnMag = b_.CreateFMul(b_.CreateCall(runtimeFn("pli_log"), {mag2}, "cpx.ln"),
                                         llvm::ConstantFP::get(b_.getDoubleTy(), 0.5), "cpx.lnmag");
      llvm::Value* theta = b_.CreateCall(runtimeFn("pli_atan2"), {ai, ar}, "cpx.arg");
      llvm::Value* wr = b_.CreateFSub(b_.CreateFMul(br, lnMag), b_.CreateFMul(bi, theta), "cpx.wr");
      llvm::Value* wi = b_.CreateFAdd(b_.CreateFMul(br, theta), b_.CreateFMul(bi, lnMag), "cpx.wi");
      llvm::Value* ew = b_.CreateCall(runtimeFn("pli_exp"), {wr}, "cpx.ew");
      rr = b_.CreateFMul(ew, b_.CreateCall(runtimeFn("pli_cos"), {wi}, "cpx.cos"), "cpx.rr");
      ri = b_.CreateFMul(ew, b_.CreateCall(runtimeFn("pli_sin"), {wi}, "cpx.sin"), "cpx.ri");
      // 0**0 is 1+0i; the log/exp chain would yield NaN there.
      llvm::Value* isZeroBase =
          b_.CreateFCmpOEQ(mag2, llvm::ConstantFP::get(b_.getDoubleTy(), 0.0), "cpx.zb");
      llvm::Value* brZero =
          b_.CreateFCmpOEQ(br, llvm::ConstantFP::get(b_.getDoubleTy(), 0.0), "cpx.bz");
      llvm::Value* biZero =
          b_.CreateFCmpOEQ(bi, llvm::ConstantFP::get(b_.getDoubleTy(), 0.0), "cpx.iz");
      llvm::Value* isZeroExp = b_.CreateAnd(brZero, biZero, "cpx.ze");
      llvm::Value* bothZero = b_.CreateAnd(isZeroBase, isZeroExp, "cpx.zz");
      rr = b_.CreateSelect(bothZero, llvm::ConstantFP::get(b_.getDoubleTy(), 1.0), rr, "cpx.rr0");
      ri = b_.CreateSelect(bothZero, llvm::ConstantFP::get(b_.getDoubleTy(), 0.0), ri, "cpx.ri0");
      break;
    }
    default:
      break;
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
    if (flt) {
      // FLOAT division (ADR-014): fdiv, zero-divide resumes with 0.0.
      llvm::Value* fzero = llvm::ConstantFP::get(b_.getDoubleTy(), 0.0);
      llvm::Value* dz = b_.CreateFCmpOEQ(bv.reg, fzero, "zdiv");
      llvm::Value* div = b_.CreateFDiv(av.reg, bv.reg, "bin");
      r = zerodivideResume(dz, div, fzero);
    } else {
      // Integer division (P3, ADR-190): sdiv truncates toward zero, matching
      // PL/I's FIXED division semantics (rule 121). Operands widened so
      // CreateSDiv is well-defined (32-bit sdiv has narrower range); widened to
      // i64 normally, i128 for a wide (>18 digit) DECIMAL common (ADR-191).
      // ZERODIVIDE (rule 94): a zero divisor traps, resuming with 0 — guarded
      // to avoid CreateSDiv UB (LLVM: sdiv by zero is UB).
      bool wide = common.intBits() == 128; // ADR-191
      // Operands widened to the division width (i64, or i128 for a wide
      // >18-digit DECIMAL common) so CreateSDiv is well-defined and so the ICmp
      // zero-divide guard compares same-width values. av/bv are already
      // converted to `common`, so widen only when their storage is narrower.
      llvm::Type* wty = wide ? b_.getInt128Ty() : b_.getInt64Ty();
      unsigned wbits = wide ? 128 : 64;
      llvm::Value* ai = (av.reg->getType()->getIntegerBitWidth() == wbits)
                            ? av.reg
                            : b_.CreateSExt(av.reg, wty, "ai");
      llvm::Value* bi = (bv.reg->getType()->getIntegerBitWidth() == wbits)
                            ? bv.reg
                            : b_.CreateSExt(bv.reg, wty, "bi");
      llvm::Value* z0 = wide ? i128(0) : i64(0);
      llvm::Value* q;
      if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(bi)) {
        if (ci->isZero()) {
          q = zerodivideResume(b_.getTrue(), z0, z0);
        } else {
          q = b_.CreateSDiv(ai, bi, "bin");
        }
      } else {
        // UB-safe (see Convert fast path above): never sdiv by a possibly-
        // zero divisor; use a safe divisor and discard via resume select.
        // Wide-aware (ADR-191): guard compares z0 and safe-one matches width.
        llvm::Value* dz = b_.CreateICmpEQ(bi, z0, "zdiv");
        llvm::Value* one = wide ? i128(1) : i64(1);
        llvm::Value* safe = b_.CreateSelect(dz, one, bi, "zdiv.safe");
        q = b_.CreateSDiv(ai, safe, "bin");
        q = zerodivideResume(dz, q, z0);
      }
      if (wide)
        r = q; // already at common's i128 width and scale
      else
        r = convert(Val{Type::fixedBin(63, 0), q}, common, e->loc).reg;
    }
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
  if (e->name == "BYADDR") {
    // BYADDR only has meaning as a direct call argument (handled in
    // argAddr); anywhere else it is diagnosed, never silently zeroed.
    d_.error(e->loc, "BYADDR is only valid as a direct call argument", "(38)");
    v.ty = e->ty;
    v.reg = i64(0);
    result = v;
    return true;
  }
  if (e->name == "NULL") {
    // NULL (rule 123, Appendix 1): the null POINTER value.
    v.ty = e->ty;
    v.reg = llvm::ConstantPointerNull::get(b_.getPtrTy());
    result = v;
    return true;
  }
  if (e->name == "ADDR") {
    // ADDR (rule 123, Appendix 1; Y33-6003 BASED builtin; IBM storage-
    // control): the address of a variable as a POINTER. A subscripted
    // element (rules 123,126, wishlist #4) addresses the same storage
    // load/store use, so SUBSCRIPTRANGE still applies.
    HExpr* a = e->args[0].get();
    if (a->kind == HExpr::VarRef && a->sym) {
      v.ty = e->ty;
      // A qualified member S.A.B GEPs off the base; a locator P->X
      // addresses off the locator value instead of the own base.
      if (a->locPtr) {
        llvm::Value* base = emitExpr(a->locPtr.get()).reg;
        v.reg = a->memberPath.empty() ? base : locatorMemberAddr(a->sym, a->memberPath, base);
      } else {
        v.reg =
            a->memberPath.empty() ? addressOf(a->sym) : memberAddr(a->sym, a->memberPath, a->loc);
      }
      result = v;
      return true;
    }
    if (a->kind == HExpr::Subscript && a->sym) {
      for (const auto& ix : a->args)
        if (ix->kind == HExpr::Star) {
          d_.error(a->loc, "ADDR of a cross-section is not implemented in this stage", "(126)");
          v.ty = e->ty;
          v.reg = llvm::ConstantPointerNull::get(b_.getPtrTy());
          result = v;
          return true;
        }
      v.ty = e->ty;
      // An array of structures arr(i).x: index to the element struct,
      // then GEP through the member path from that element address.
      if (!a->memberPath.empty() && a->sym->ty.isArray()) {
        llvm::Value* base = a->locPtr ? emitExpr(a->locPtr.get()).reg : addressOf(a->sym);
        llvm::Value* elem = arrayElementAddr(a->sym->ty, base, a->args, a->loc,
                                             dynUb_.count(a->sym) ? dynUb_[a->sym] : nullptr,
                                             dynLb_.count(a->sym) ? dynLb_[a->sym] : nullptr);
        v.reg = elementMemberAddr(a->sym, a->memberPath, elem);
        result = v;
        return true;
      }
      // A subscripted member array S.A(i): the member holds the array.
      if (!a->memberPath.empty()) {
        if (a->locPtr) {
          d_.error(a->loc,
                   "ADDR of a locator-qualified member array is not implemented in this stage",
                   "(124)");
          v.reg = llvm::ConstantPointerNull::get(b_.getPtrTy());
          result = v;
          return true;
        }
        const Type& arr = memberType(a->sym, a->memberPath);
        llvm::Value *ub = nullptr, *lb = nullptr;
        llvm::Value* base = arr.isDynamic()
                                ? dynamicMemberBase(a->sym, a->memberPath, a->loc, ub, lb)
                                : memberAddr(a->sym, a->memberPath, a->loc);
        v.reg = arrayElementAddr(arr, base, a->args, a->loc, ub, lb);
        result = v;
        return true;
      }
      // A CONTROLLED dynamic element sizes from the live generation.
      if (isCtlDynArray(a->sym)) {
        v.reg = ctlDynElementAddr(a->sym, a->args, a->loc);
        result = v;
        return true;
      }
      // An N-D CONTROLLED element addresses off the live per-axis extents.
      if (isCtlNDynArray(a->sym)) {
        v.reg = ctlNDElementAddr(a->sym, a->args, a->loc);
        result = v;
        return true;
      }
      // An iSUB-DEFINED element overlays its base (rule 134).
      if (a->sym->definedBase && a->sym->definedIsubAxis >= 0) {
        v.reg = definedSubElementAddr(a->sym, a->args, a->loc);
        result = v;
        return true;
      }
      // Plain fixed/dynamic and BASED overlays share the load path's
      // base: the locator value for P->X(i), else the symbol base
      // (a BASED base loads its POINTER, a CONTROLLED base its top).
      llvm::Value* base = a->locPtr ? emitExpr(a->locPtr.get()).reg : addressOf(a->sym);
      v.reg = arrayElementAddr(a->sym->ty, base, a->args, a->loc,
                               dynUb_.count(a->sym) ? dynUb_[a->sym] : nullptr,
                               dynLb_.count(a->sym) ? dynLb_[a->sym] : nullptr);
      result = v;
      return true;
    }
    d_.error(a->loc, "ADDR requires a variable or array element in this stage", "(123)");
    v.ty = e->ty;
    v.reg = llvm::ConstantPointerNull::get(b_.getPtrTy());
    result = v;
    return true;
  }
  if (e->name == "SUBSTR") {
    Val s = emitExpr(e->args[0].get());
    Val start = emitExpr(e->args[1].get());
    Val len = emitExpr(e->args[2].get());
    // Source capacity for clipping: static, except for adjustable `CHAR(*)`
    // (rule (18)) / CONTROLLED (rule (15)) sources, where it is live.
    Symbol* ssysm = e->args[0]->sym;
    llvm::Value* capV = (ssysm && ssysm->ty.isChar()) ? adjustLen(ssysm, ssysm->ty) : nullptr;
    Val out = charTemp(e->ty.len);
    if (e->ty.varying) {
      // A runtime length (rule (123)): clamp the live length at zero and
      // the source maximum, and transmit exactly that (start keeps the
      // blank-fill semantics of the constant path). The result buffer is
      // sized by the same maximum, so an adjustable source gets a
      // runtime-sized buffer evaluated here (its capacity can change between
      // evaluations, e.g. across CONTROLLED reallocations).
      llvm::Value* ln = toI64(len);
      llvm::Value* nonneg =
          b_.CreateSelect(b_.CreateICmpSLT(ln, i64(0), "svneg"), i64(0), ln, "sv0");
      llvm::Value* cap = capV ? capV : i64(e->ty.len);
      llvm::Value* live =
          b_.CreateSelect(b_.CreateICmpSGT(nonneg, cap, "svbig"), cap, nonneg, "svlive");
      if (capV) {
        // Adjustable source: replace the static placeholder buffer with a
        // runtime-sized one evaluated here.
        out.ty = e->ty;
        out.ptr = b_.CreateAlloca(b_.getInt8Ty(), cap, "svbuf");
        out.len = cap;
      }
      emitSubstr(out.ptr, out.len, s.ptr, s.len, toI64(start), live);
      out.len = live;
      result = out;
      return true;
    }
    emitSubstr(out.ptr, out.len, s.ptr, s.len, toI64(start), toI64(len));
    out.len = i64(e->ty.len);
    result = out;
    return true;
  }
  if (e->name == "INDEX") {
    Val a = emitExpr(e->args[0].get());
    Val b = emitExpr(e->args[1].get());
    llvm::Value* r = emitIndex(a.ptr, a.len, b.ptr, b.len);
    v.ty = e->ty;
    v.reg = b_.CreateTrunc(r, b_.getInt32Ty(), "idx32");
    result = v;
    return true;
  }
  if (e->name == "ABS") {
    Val a = emitExpr(e->args[0].get());
    const Type& at = a.ty;
    llvm::Value* r;
    if (at.isComplex()) {
      // |x + iy| = sqrt(x*x + y*y) over the FLOAT pair (rule (123)).
      llvm::Value* re = b_.CreateExtractValue(a.cpx, 0, "abre");
      llvm::Value* im = b_.CreateExtractValue(a.cpx, 1, "abim");
      llvm::Value* m =
          b_.CreateFAdd(b_.CreateFMul(re, re, "abq1"), b_.CreateFMul(im, im, "abq2"), "abq");
      r = b_.CreateCall(runtimeFn("pli_sqrt"), {m}, "abss");
    } else if (at.k == TK::Float) {
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
  // SIGN built-in (rule (123), Appendix 1): -1/0/+1 for negative/zero/positive,
  // pure LLVM selects, no runtime call. The argument is compared to zero in
  // its own type (integer or float), so NaN yields 0 (unordered both ways).
  if (e->name == "SIGN") {
    Val a = emitExpr(e->args[0].get());
    llvm::Value* z = llvm::Constant::getNullValue(llvmTy(a.ty));
    llvm::Value* neg = (a.ty.k == TK::Float) ? b_.CreateFCmpOLT(a.reg, z, "sgnn")
                                             : b_.CreateICmpSLT(a.reg, z, "sgnn");
    llvm::Value* pos = (a.ty.k == TK::Float) ? b_.CreateFCmpOGT(a.reg, z, "sgnp")
                                             : b_.CreateICmpSGT(a.reg, z, "sgnp");
    llvm::Value* m1 = i64(-1);
    llvm::Value* one = i64(1);
    llvm::Value* r = b_.CreateSelect(neg, m1, b_.CreateSelect(pos, one, i64(0), "signp"), "sign");
    v.ty = e->ty;
    v.reg = b_.CreateTrunc(r, b_.getInt32Ty(), "sign32");
    result = v;
    return true;
  }
  // Scalar math built-ins (QR2.7, Appendix 1, <math.h> analogues): FLOOR,
  // CEIL, SQRT, EXP, LOG, SIN, COS, TAN, LOG2, LOG10, ATAN, SINH, COSH, TANH,
  // ATANH, ERF, ERFC, ASIN, ACOS, CBRT, and the degree trig variants SIND,
  // COSD, TAND, ATAND. The argument is converted to FLOAT and the matching
  // function is called. Non-degree variants map to LLVM intrinsics (inlined
  // and lowered to hardware/libm by the optimizer), except TAN/ATAN/SINH/
  // COSH/TANH/ASIN/ACOS on LLVM < 20, whose backends leave the llvm.* call
  // in the object file (undefined symbol at link); those use the pli_*
  // musl-port wrappers instead. Degree variants keep pli_* runtime wrappers
  // that convert to radians first.
  if (e->name == "FLOOR" || e->name == "CEIL" || e->name == "SQRT" || e->name == "EXP" ||
      e->name == "LOG" || e->name == "SIN" || e->name == "COS" || e->name == "TAN" ||
      e->name == "LOG2" || e->name == "LOG10" || e->name == "ATAN" || e->name == "SINH" ||
      e->name == "COSH" || e->name == "TANH" || e->name == "ATANH" || e->name == "ERF" ||
      e->name == "ERFC" || e->name == "SIND" || e->name == "COSD" || e->name == "TAND" ||
      e->name == "ATAND" || e->name == "ASIN" || e->name == "ACOS" || e->name == "CBRT") {
    Val x = convert(emitExpr(e->args[0].get()), Type::flt(6), e->loc);
    static const char* const kMathFn[] = {
        "pli_floor", "pli_ceil", "pli_sqrt",  "pli_exp",   "pli_log",  "pli_sin",
        "pli_cos",   "pli_tan",  "pli_log2",  "pli_log10", "pli_atan", "pli_sinh",
        "pli_cosh",  "pli_tanh", "pli_atanh", "pli_erf",   "pli_erfc", "pli_sind",
        "pli_cosd",  "pli_tand", "pli_atand", "pli_asin",  "pli_acos", "pli_cbrt"};
    static const char* const kMathName[] = {"FLOOR", "CEIL", "SQRT",  "EXP",   "LOG",  "SIN",
                                            "COS",   "TAN",  "LOG2",  "LOG10", "ATAN", "SINH",
                                            "COSH",  "TANH", "ATANH", "ERF",   "ERFC", "SIND",
                                            "COSD",  "TAND", "ATAND", "ASIN",  "ACOS", "CBRT"};
    static const char* const kLLVMIntrinsic[] = {
        "llvm.floor.f64", "llvm.ceil.f64", "llvm.sqrt.f64", "llvm.exp.f64",  "llvm.log.f64",
        "llvm.sin.f64",   "llvm.cos.f64",  "llvm.tan.f64",  "llvm.log2.f64", "llvm.log10.f64",
        "llvm.atan.f64",  "llvm.sinh.f64", "llvm.cosh.f64", "llvm.tanh.f64", nullptr,
        nullptr,          nullptr,         nullptr,         nullptr,         nullptr,
        nullptr,          "llvm.asin.f64", "llvm.acos.f64", nullptr};
    int ix = 0;
    for (int i = 0; i < 24; ++i)
      if (e->name == kMathName[i])
        ix = i;
    v.ty = e->ty;
    const char* intrinsic = kLLVMIntrinsic[ix];
#if LLVM_VERSION_MAJOR < 20
    // LLVM < 20 has no backend lowering for these seven (the llvm.* call
    // survives to the object file and fails the link); route them through
    // the pli_* musl-port wrappers, which exist for every entry above.
    if (intrinsic &&
        (e->name == "TAN" || e->name == "ATAN" || e->name == "SINH" || e->name == "COSH" ||
         e->name == "TANH" || e->name == "ASIN" || e->name == "ACOS"))
      intrinsic = nullptr;
#endif
    if (intrinsic)
      v.reg = b_.CreateCall(intrinsicFn(intrinsic, b_.getDoubleTy(), {b_.getDoubleTy()}), {x.reg},
                            "math");
    else
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
  // OMITTED/PRESENT (extension, ADR-119): an omitted OPTIONAL arrives as a
  // null by-reference slot. Compare the parameter's address, never loading
  // through it — the slot itself may be null. Sema restricted the argument
  // to an OPTIONAL parameter.
  if (e->name == "OMITTED" || e->name == "PRESENT") {
    HExpr* a = e->args[0].get();
    llvm::Value* addr = (a->sym && a->sym->kind != Symbol::ProcName) ? addressOf(a->sym) : nullptr;
    if (!addr) {
      d_.error(e->loc, "could not address the OPTIONAL parameter", "(123)");
      v.ty = e->ty;
      v.reg = b_.getInt1(false);
      result = v;
      return true;
    }
    llvm::Value* isNull =
        b_.CreateICmpEQ(addr, llvm::Constant::getNullValue(b_.getPtrTy()), "omnull");
    v.ty = e->ty;
    v.reg = e->name == "OMITTED" ? isNull : b_.CreateNot(isNull, "ompres");
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
  // REVERSE (rule (123)): mirror the string via the runtime.
  if (e->name == "REVERSE") {
    Val s = emitExpr(e->args[0].get());
    Val dst = charTemp(e->ty.len);
    emitReverse(dst.ptr, dst.len, s.ptr, s.len);
    dst.len = i64(e->ty.len);
    result = dst;
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
  // MIN/MAX (rule (123)): fold the arguments left to right with a select per
  // pair in the common arithmetic type.
  if (e->name == "MIN" || e->name == "MAX") {
    const Type& common = e->ty;
    llvm::Value* acc = convert(emitExpr(e->args[0].get()), common, e->loc).reg;
    for (size_t i = 1; i < e->args.size(); ++i) {
      llvm::Value* nx = convert(emitExpr(e->args[i].get()), common, e->loc).reg;
      llvm::Value* cmp = common.k == TK::Float ? b_.CreateFCmpOLT(acc, nx, "mincmp")
                                               : b_.CreateICmpSLT(acc, nx, "mincmp");
      acc = e->name == "MIN" ? b_.CreateSelect(cmp, acc, nx, "min")
                             : b_.CreateSelect(cmp, nx, acc, "max");
    }
    v.ty = common;
    v.reg = acc;
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
      // ZERODIVIDE (rule 94): a zero divisor traps, resuming with 0.
      llvm::Value* avi = toI64(av);
      llvm::Value* m;
      if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(bv.reg)) {
        // FAST PATH: constant divisor — inline the MOD computation directly
        // instead of calling pli_mod_ll, eliminating the runtime call entirely
        // (rules (60)-(63), ADR-112). srem gives sign-of-dividend; PL/I MOD
        // takes sign-of-divisor, so a conditional add corrects when signs differ
        // and the remainder is nonzero.
        llvm::APInt cv = ci->getValue();
        if (cv.isZero()) {
          // MOD(x, 0) resumes with 0; trap via zerodivideResume if checked.
          m = zerodivideResume(b_.getTrue(), i64(0), i64(0));
        } else {
          llvm::Value* divisor = i64(cv.getSExtValue());
          llvm::Value* r = b_.CreateSRem(avi, divisor, "mod");
          llvm::Value* adj = b_.CreateAdd(r, divisor, "modadj");
          llvm::Value* nz = b_.CreateICmpNE(r, i64(0), "modnz");
          // Adjust when remainder is nonzero and sign(r) != sign(divisor).
          bool bNeg = cv.isNegative();
          llvm::Value* needAdj =
              bNeg ? b_.CreateICmpUGE(r, i64(0), "modpos") : b_.CreateICmpSLT(r, i64(0), "modneg");
          m = b_.CreateSelect(nz, b_.CreateSelect(needAdj, adj, r), r, "mods");
        }
      } else {
        // General path: pli_mod_ll is AlwaysInline (Task 5), so LLVM can
        // inline and fold the body when the divisor is known at -O2+.
        llvm::Value* bi = toI64(bv);
        llvm::Value* dz = b_.CreateICmpEQ(bi, i64(0), "zdiv");
        m = b_.CreateCall(runtimeFn("pli_mod_ll"), {avi, bi});
        m = zerodivideResume(dz, m, i64(0));
      }
      llvm::Value* m64 = b_.CreateTrunc(m, b_.getInt32Ty(), "mod32");
      // Keep the value's LLVM width in sync with e->ty for later compares.
      v.reg = common.intBits() == 32 ? m64 : b_.CreateSExt(m64, llvmTy(common), "modw");
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
    llvm::Value* r = common.k == TK::Float ? b_.CreateFMul(av.reg, bv.reg, "mul")
                     : common.isFixed()    ? checkedArith(Tok::Star, av.reg, bv.reg)
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
    emitRepeat(out.ptr, out.len, s.ptr, s.len, toI64(n));
    out.len = i64(e->ty.len);
    result = out;
    return true;
  }
  if (e->name == "VERIFY") {
    Val s = emitExpr(e->args[0].get());
    Val t = emitExpr(e->args[1].get());
    llvm::Value* r;
    if (e->args.size() == 3) {
      Val st = emitExpr(e->args[2].get());
      r = emitVerifyFrom(s.ptr, s.len, t.ptr, t.len, toI64(st));
    } else {
      r = emitVerify(s.ptr, s.len, t.ptr, t.len);
    }
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
    emitTranslate(dst.ptr, dst.len, s.ptr, s.len, out.ptr, out.len, in.ptr, in.len);
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
    emitTrim(dst.ptr, dst.len, s.ptr, s.len, pad, padlen);
    dst.len = i64(e->ty.len);
    result = dst;
    return true;
  }
  if (e->name == "TALLY") {
    Val x = emitExpr(e->args[0].get());
    Val y = emitExpr(e->args[1].get());
    llvm::Value* r = emitTally(x.ptr, x.len, y.ptr, y.len);
    v.ty = e->ty;
    v.reg = b_.CreateTrunc(r, b_.getInt32Ty(), "tal32");
    result = v;
    return true;
  }
  if (e->name == "UPPERCASE") {
    Val s = emitExpr(e->args[0].get());
    Val dst = charTemp(e->ty.len);
    emitUppercase(dst.ptr, dst.len, s.ptr, s.len);
    dst.len = i64(e->ty.len);
    result = dst;
    return true;
  }
  if (e->name == "HIGH" || e->name == "LOW") {
    Val n = emitExpr(e->args[0].get());
    Val out = charTemp(e->ty.len);
    if (e->name == "HIGH")
      emitHigh(out.ptr, toI64(n));
    else
      emitLow(out.ptr, toI64(n));
    out.len = i64(e->ty.len);
    result = out;
    return true;
  }
  // LOWERCASE/CENTER/SEARCH/VERIFY/RANK/COLLATE (rule (123)): merged from
  // the builtins bridge; char builders follow the UPPERCASE pattern.
  if (e->name == "LOWERCASE") {
    Val s = emitExpr(e->args[0].get());
    Val dst = charTemp(e->ty.len);
    emitLowercase(dst.ptr, dst.len, s.ptr, s.len);
    dst.len = i64(e->ty.len);
    result = dst;
    return true;
  }
  if (e->name == "CENTER") {
    Val s = emitExpr(e->args[0].get());
    Val w = emitExpr(e->args[1].get());
    Val dst = charTemp(e->ty.len);
    emitCenter(dst.ptr, dst.len, s.ptr, s.len, toI64(w));
    dst.len = i64(e->ty.len);
    result = dst;
    return true;
  }
  if (e->name == "SEARCH") {
    Val s = emitExpr(e->args[0].get());
    Val t = emitExpr(e->args[1].get());
    Val st = emitExpr(e->args[2].get());
    llvm::Value* r = emitSearch(s.ptr, s.len, t.ptr, t.len, toI64(st));
    v.ty = e->ty;
    v.reg = b_.CreateTrunc(r, b_.getInt32Ty(), "srch32");
    result = v;
    return true;
  }
  if (e->name == "VERIFY") {
    Val s = emitExpr(e->args[0].get());
    Val t = emitExpr(e->args[1].get());
    llvm::Value* r;
    if (e->args.size() == 3) {
      Val st = emitExpr(e->args[2].get());
      r = emitVerifyFrom(s.ptr, s.len, t.ptr, t.len, toI64(st));
    } else {
      r = emitVerify(s.ptr, s.len, t.ptr, t.len);
    }
    v.ty = e->ty;
    v.reg = b_.CreateTrunc(r, b_.getInt32Ty(), "ver32");
    result = v;
    return true;
  }
  if (e->name == "RANK") {
    Val s = emitExpr(e->args[0].get());
    llvm::Value* r = b_.CreateCall(runtimeFn("pli_rank"), {s.ptr, s.len});
    v.ty = e->ty;
    v.reg = b_.CreateTrunc(r, b_.getInt32Ty(), "rnk32");
    result = v;
    return true;
  }
  if (e->name == "COLLATE") {
    Val n = emitExpr(e->args[0].get());
    Val dst = charTemp(e->ty.len);
    b_.CreateCall(runtimeFn("pli_collate"), {dst.ptr, dst.len, toI64(n)});
    dst.len = i64(e->ty.len);
    result = dst;
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
  // SYSPARM (rule (123)): the SYSPARM option value baked in at compile time;
  // a constant string with the option's exact length (possibly zero).
  if (e->name == "SYSPARM") {
    const std::string& s = sema_.sysparm();
    v.ty = e->ty;
    v.ptr = globalString(s);
    v.len = i64((long long)s.size());
    result = v;
    return true;
  }
  // FIXED (rule (123)): FIXED(char) parses decimal text, FIXED(numeric)
  // truncates toward zero (floats clamp out-of-range, NaN reads as 0).
  if (e->name == "FIXED") {
    Val a = emitExpr(e->args[0].get());
    if (a.ty.isChar()) {
      // CONVERSION (rule 94): when the text holds no digits, FIXED(char)
      // traps through the CONVERSION dispatch (abort or ON unit); the resume
      // value is 0, the value the LLVM or runtime path leaves in `ok=0`.
      llvm::Value* okSlot = entryAlloca(b_.getInt32Ty(), "conv_ok");
      llvm::Value* r;
      if (useRuntimeCall("fixed_of_char")) {
        r = b_.CreateCall(runtimeFn("pli_fixed_of_char"), {a.ptr, a.len, okSlot}, "fxc_rt");
      } else {
        r = emitFixedOfCharLLVM(a.ptr, a.len, okSlot);
      }
      if (convChecks()) {
        llvm::Value* ok = b_.CreateLoad(b_.getInt32Ty(), okSlot, "conv_ok");
        llvm::Value* fail = b_.CreateICmpEQ(ok, i32(0), "conv_fail");
        llvm::BasicBlock* trapBB =
            llvm::BasicBlock::Create(ctx_, "conv.trap." + std::to_string(n_++), curFn_);
        llvm::BasicBlock* okBB =
            llvm::BasicBlock::Create(ctx_, "conv.ok." + std::to_string(n_++), curFn_);
        b_.CreateCondBr(fail, trapBB, okBB);
        b_.SetInsertPoint(trapBB);
        emitCondTrap(Stmt::kConversionCondKey, "pli_conversion", "conv", okBB);
        b_.SetInsertPoint(okBB);
      }
      v.ty = e->ty;
      v.reg = b_.CreateTrunc(r, llvmTy(e->ty), "fxc");
      result = v;
      return true;
    }
    if (a.ty.k == TK::Float) {
      llvm::Value* r;
      if (useRuntimeCall("fixed_of_float")) {
        r = b_.CreateCall(runtimeFn("pli_fixed_of_float"), {a.reg}, "fxf_rt");
      } else {
        r = emitFixedOfFloatLLVM(a.reg);
      }
      v.ty = e->ty;
      v.reg = b_.CreateTrunc(r, llvmTy(e->ty), "fxf");
      result = v;
      return true;
    }
    v = convert(a, e->ty, e->loc);
    result = v;
    return true;
  }
  // CHAR (rule (123)): renders a scalar value as text into a fresh buffer;
  // a character argument passes through (truncated/padded by assignment).
  if (e->name == "CHAR") {
    Val a = emitExpr(e->args[0].get());
    if (a.ty.isChar()) {
      result = a;
      return true;
    }
    Val dst = charTemp(e->ty.len);
    if (a.ty.k == TK::Float) {
      b_.CreateCall(runtimeFn("pli_char_of_float"), {dst.ptr, dst.len, a.reg});
    } else {
      llvm::Value* iv = toI64(convert(a, Type::fixedBin(31, 0), e->loc));
      b_.CreateCall(runtimeFn("pli_char_of_fixed"), {dst.ptr, dst.len, iv});
    }
    dst.len = i64(e->ty.len);
    result = dst;
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
  // SYSTEM (rule (123)): invoke the command processor with command string x,
  // returning the processor's exit code as FIXED BIN(31). Numeric arguments
  // are converted to character via CHAR; character arguments pass through
  // directly (the runtime null-terminates internally).
  if (e->name == "SYSTEM") {
    Val a = emitExpr(e->args[0].get());
    Val cmd = a;
    if (!a.ty.isChar()) {
      int buflen = 13; // enough for any FIXED BIN(31,0): sign + 10 digits + nul
      cmd = charTemp(buflen);
      if (a.ty.k == TK::Float)
        b_.CreateCall(runtimeFn("pli_char_of_float"), {cmd.ptr, cmd.len, a.reg});
      else {
        llvm::Value* iv = toI64(convert(a, Type::fixedBin(31, 0), e->loc));
        b_.CreateCall(runtimeFn("pli_char_of_fixed"), {cmd.ptr, cmd.len, iv});
      }
    }
    v.ty = e->ty;
    v.reg = b_.CreateCall(runtimeFn("pli_system"), {cmd.ptr, cmd.len}, "syso");
    result = v;
    return true;
  }
  // EVENT (rules (79),(82), QR2.8): poll an event's completion as BIT(1).
  if (e->name == "EVENT") {
    HExpr* a = e->args[0].get();
    llvm::Value* addr =
        (a && a->sym) ? addressOf(a->sym) : llvm::Constant::getNullValue(b_.getPtrTy());
    llvm::Value* st = b_.CreateCall(runtimeFn("pli_event_status"), {addr}, "evst");
    v.ty = e->ty;
    v.reg = b_.CreateTrunc(st, b_.getInt1Ty(), "evb");
    result = v;
    return true;
  }
  // Array attribute built-ins (M2, rule (123)): constant bounds fold to a
  // compile-time value. The argument is the unsubscripted array reference;
  // read its bounds from the symbol rather than emitting the array value.
  // LBOUND/HBOUND report the first dimension; DIM/DIMENSION report the total
  // element count (the product over all axes). With a second integer-constant
  // axis argument (1-based) each reports that axis: the bound or the extent.
  if (e->name == "LBOUND" || e->name == "HBOUND" || e->name == "DIM" || e->name == "DIMENSION") {
    HExpr* a = e->args[0].get();
    // The argument is an unsubscripted array reference, either a plain array
    // (a->sym) or a qualified structure member array S.V (a->sym + memberPath).
    const Type& arr = (a->sym && !a->memberPath.empty())
                          ? memberType(a->sym, a->memberPath)
                          : (a->sym ? a->sym->ty : Type::fixedBin(31, 0));
    v.ty = e->ty;
    const bool isDim = e->name == "DIM" || e->name == "DIMENSION";
    // A selected axis (0-based), or -1 for the default single-argument form.
    long long sel = e->args.size() == 2 ? e->args[1]->ival - 1 : -1;
    // A CONTROLLED dynamic numeric array (rules (13),(89)): lb is the DECLARE
    // lb for a fixed 1-D array (else 1), ub is lb + live extent - 1, DIM is the
    // live extent; covers scalar, (*) and fixed 1-D forms.
    if (a->sym && a->memberPath.empty() && isCtlDynArray(a->sym) && (sel < 0 || sel == 0)) {
      llvm::Value* ext = ctlDynBound(a->sym);
      long long lbConst = ctlDynLb(a->sym);
      llvm::Value* raw;
      if (e->name == "LBOUND")
        raw = i64(lbConst);
      else if (isDim)
        raw = ext;
      else
        raw = b_.CreateSub(b_.CreateAdd(i64(lbConst), ext, "ctlhi"), i64(1), "ctlub");
      Val src;
      src.ty = Type::fixedBin(63, 0);
      src.reg = raw;
      v.reg = convert(src, e->ty, e->loc).reg;
      result = v;
      return true;
    }
    // An N-D CONTROLLED numeric array (rules (13),(89)): lower bounds are
    // static (the DECLARE lb, else 1) while extents are per-generation live.
    // LBOUND/HBOUND report the selected axis (axis 0 when unselected); DIM
    // reports the selected axis extent, or the total (product) when unselected.
    if (a->sym && a->memberPath.empty() && isCtlNDynArray(a->sym)) {
      size_t rank = a->sym->ty.dims.size();
      llvm::Value* raw = nullptr;
      if (isDim && sel < 0) {
        raw = i64(1);
        for (size_t k = 0; k < rank; ++k)
          raw = b_.CreateMul(raw, ctlNDExtent(a->sym, k), "ctldimtot");
      } else {
        size_t k = sel < 0 ? 0 : (size_t)sel;
        if (k >= rank)
          k = 0;
        long long lbConst = ctlNDLb(a->sym, k);
        llvm::Value* ext = ctlNDExtent(a->sym, k);
        if (e->name == "LBOUND")
          raw = i64(lbConst);
        else if (isDim)
          raw = ext;
        else
          raw = b_.CreateSub(b_.CreateAdd(i64(lbConst), ext, "ctlhi"), i64(1), "ctlub");
      }
      Val src;
      src.ty = Type::fixedBin(63, 0);
      src.reg = raw;
      v.reg = convert(src, e->ty, e->loc).reg;
      result = v;
      return true;
    }
    // A per-axis request against a dynamic first axis uses the live bounds;
    // later axes are always constant in this stage.
    if (sel >= 0 && arr.isArray() && arr.isDynamic() && sel == 0) {
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
      llvm::Value* raw = e->name == "LBOUND" ? lb
                         : e->name == "HBOUND"
                             ? ub
                             : b_.CreateAdd(b_.CreateSub(ub, lb, "e1"), i64(1), "ext");
      Val src;
      src.ty = Type::fixedBin(63, 0);
      src.reg = raw;
      v.reg = convert(src, e->ty, e->loc).reg;
      result = v;
      return true;
    }
    // A per-axis request against a constant axis folds directly.
    if (sel >= 0 && arr.isArray() && sel < (long long)arr.dims.size()) {
      const Dim& d = arr.dims[(size_t)sel];
      long long val = e->name == "LBOUND" ? d.lb : e->name == "HBOUND" ? d.ub : (d.ub - d.lb + 1);
      (void)isDim;
      v.reg = llvm::ConstantInt::get(llvmTy(e->ty), val, true);
      result = v;
      return true;
    }
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
    // A fixed member array in an UNALIGNED structure is not naturally aligned;
    // a dynamic member's buffer is separately allocated, so it is (ADR-169).
    const bool packed = isMember && !dyn && packedMemberPath(a->sym->ty, a->memberPath);
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
    llvm::LoadInst* rl = b_.CreateLoad(llvmTy(el), ep, isBit ? "rdb" : "rdl");
    if (packed)
      rl->setAlignment(llvm::Align(1));
    if (isBit)
      ev.reg = b_.CreateTrunc(rl, b_.getInt1Ty(), "rdb1");
    else
      ev.reg = rl;
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
