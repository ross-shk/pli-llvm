// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
#include "irgen.h"
#include "codegen.h" // pli::applyRuntimeFnAttrs declaration
#include "embedded_runtime.h"
#include "irgen_decisions.h" // pli:: the PL/I-specific decisions (ADR-260)
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <limits>
#include <set>

#ifndef PLI_LLVM_VERSION
#define PLI_LLVM_VERSION "unknown"
#endif

#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Analysis/ValueTracking.h"
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

// base^exp as a constant at an arbitrary integer width (ADR-289): the exact
// wide cross-base rescale works in i256, where 10^38 still fits.
static llvm::ConstantInt* intPowConst(llvm::Type* ty, int base, int exp) {
  unsigned bits = ty->getIntegerBitWidth();
  llvm::APInt acc(bits, 1);
  llvm::APInt b(bits, base);
  for (int i = 0; i < exp; ++i)
    acc *= b;
  return llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(ty, acc));
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

// Stored integer of an INITIAL/VALUE literal for a FIXED BINARY(p,q)
// target (ADR-284): value * 2^q, with a decimal literal's fraction
// truncated toward zero — the same rule convert() applies at run time.
static PliI128 binLitRescaled(const Expr* e, const Type& t) {
  if (!e)
    return 0;
  if (e->kind == Expr::FltLit)
    return pli_from_i64((long long)std::ldexp(e->fval, t.scale));
  PliI128 v = e->kind == Expr::DecLit ? e->wideIval : pli_from_i64(e->ival);
  if (t.scale > 0)
    v = v * pli_from_i64(1LL << t.scale);
  if (e->kind == Expr::DecLit && e->decScale > 0)
    v = v / pliPow10_128(e->decScale);
  return v;
}

// True when a DECIMAL literal needs the wide constant path (ADR-191): its
// full value exceeds i64, or the target itself is wide (a narrow literal
// scaled up into a wide target can overflow i64 mid-rescale). A >18-digit
// integer constant is likewise genuinely wide (rule 135): for a wide target
// rescale from the full wideIval, never the saturated i64 ival.
static bool needsWideDecLit(const Expr* e, const Type& t) {
  if (!e)
    return false;
  if (e->kind == Expr::DecLit)
    return t.intBits() == 128 || e->decPrec > 18;
  return e->kind == Expr::IntLit && t.intBits() == 128;
}
// HIR twin of the above for emitted decimal literals.
static bool needsWideDecLit(const HExpr* e, const Type& t) {
  if (!e)
    return false;
  if (e->kind == HExpr::DecLit)
    return t.intBits() == 128 || e->decPrec > 18;
  return e->kind == HExpr::IntLit && t.intBits() == 128;
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
  if (e->kind == Expr::IntLit)
    return pli_to_double(e->wideIval); // exact for wide literals (ADR-191)
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
    //
    // ADR-245 (the PL/I string padding invariant): the declared extent is
    // the whole value. A BIT(n) is exactly ceil(n/8) bytes and the bits
    // above n in the last byte are zero, so the MULTICS question "may this
    // fetch read padding bits?" (padded_ref) answers "yes, and they are
    // zero" — the value cannot depend on what sits next to it.
    if (t.len == 1)
      return b_.getInt8Ty();
    return llvm::ArrayType::get(b_.getInt8Ty(), (unsigned)bitBytes(t.len));
  case TK::Char: {
    // ADR-245: a CHARACTER(n) is exactly n bytes. There is no storage
    // padding in the LLVM sense, so "may this fetch read padding bits?"
    // (MULTICS padded_ref) answers "no" and "must this store zero the
    // padding bits?" (padded_for_store_ref) answers "there are none".
    // MULTICS conflated the two in 1980 and shipped a variable that was
    // stored unpadded but fetched padded; keep the two questions apart —
    // the fetch side is address+length (loadSym), the store side is
    // bounded by the declared length (storeCharTo).
    if (t.varying) {
      llvm::Type* data = llvm::ArrayType::get(b_.getInt8Ty(), t.len);
      return llvm::StructType::get(b_.getInt32Ty(), data);
    }
    return llvm::ArrayType::get(b_.getInt8Ty(), t.len);
  }
  case TK::Struct: {
    // UNION overlay (ADR-202): all members share offset 0, so the LLVM storage
    // is a byte array sized to the largest member (dynamic arrays use pointer
    // size). Member access casts to the member's type at offset 0.
    if (t.isUnion) {
      unsigned maxBytes = 0;
      for (const auto& m : t.members) {
        if (m->ty.isArray() && m->ty.isDynamic())
          maxBytes = std::max(maxBytes, (unsigned)mod_.getDataLayout().getPointerSize());
        else
          maxBytes = std::max(maxBytes,
                              (unsigned)mod_.getDataLayout().getTypeStoreSize(llvmTy(m->ty)));
      }
      return llvm::ArrayType::get(b_.getInt8Ty(), maxBytes);
    }
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
   case TK::Label:  // a LABEL variable holds an IR block address (rule (21))
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

// WS-K8 (ADR-257): an allocation the compiler can watch fills the anchor
// slot with the new generation's address, so a later reference is a load.
void IRGen::ctlNoteAlloc(Symbol* sym, llvm::Value* addr) {
  if (!sym || !sym->ctlDirect || !addr)
    return;
  b_.CreateStore(addr, ctlAnchorSlot(sym));
}

// ... and a free refreshes it from the address the free returns: the
// generation below the popped one is the current one now.
void IRGen::ctlNoteFree(Symbol* sym, llvm::Value* addr) {
  if (!sym || !sym->ctlDirect || !addr)
    return;
  b_.CreateStore(addr, ctlAnchorSlot(sym));
}

// WS-K8 (ADR-257): the declared length of a statically bounded CONTROLLED
// variable — the size of its generation in BYTES, which is what the runtime's
// `pli_ctl_len` returns and what the bounds check divides by the element
// size.
long long IRGen::ctlDeclaredExtent(Symbol* sym) {
  if (!sym || !sym->controlled)
    return 0;
  const Type& el = sym->ty.isArray() ? sym->ty.elementType() : sym->ty;
  long long elemBytes =
      (long long)mod_.getDataLayout().getTypeAllocSize(llvmTy(el)).getFixedValue();
  if (elemBytes <= 0)
    elemBytes = 1;
  long long count = 1;
  for (const Dim& d : sym->ty.dims) {
    if (d.dyn || d.lbDyn)
      return 0;
    count *= (long long)d.ub - (long long)d.lb + 1;
  }
  return count * elemBytes;
}

// WS-K8 (ADR-257): the slot that holds a CONTROLLED variable's current
// generation. The ALLOCATE fills it (the runtime returns the new
// generation's address), so a reference is a load rather than a runtime
// call. The slot starts null, which is what the runtime returns for an
// empty generation stack — so a reference before the first ALLOCATE faults
// the same way it does today.
llvm::Value* IRGen::ctlAnchorSlot(Symbol* sym) {
  auto hit = ctlAnchor_.find(sym);
  if (hit != ctlAnchor_.end())
    return hit->second;
  llvm::BasicBlock* entry = &curFn_->getEntryBlock();
  llvm::IRBuilder<> ab(entry, entry->begin());
  llvm::AllocaInst* slot = ab.CreateAlloca(b_.getPtrTy(), nullptr, "ctl.anchor");
  // The null store goes immediately after the alloca, at the very start of
  // the entry block, so it dominates every use of the slot.
  llvm::Instruction* next = slot->getNextNode();
  llvm::IRBuilder<> sb(entry, next ? next->getIterator() : entry->end());
  sb.CreateStore(llvm::ConstantPointerNull::get(b_.getPtrTy()), slot);
  ctlAnchor_[sym] = slot;
  return slot;
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
enum RtTok { RtVoid, RtI64, RtI32, RtI8, RtDouble, RtPtr, RtI32Ptr };
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
      {"pli_overflow", {.noReturn = true}},
      {"pli_fixedoverflow", {.noReturn = true}},
      {"pli_overflow_error", {.noReturn = true}},
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
      {"pli_ctl_alloc", {.willReturn = false, .alwaysInline = true, .mem = RtMemOtherMod}},
      {"pli_ctl_alloc_dims", {.willReturn = false, .mem = RtMemOtherMod}},
      {"pli_ctl_ensure", {.willReturn = false, .mem = RtMemOtherMod}},
      {"pli_ctl_free", {.willReturn = false, .mem = RtMemOtherMod}},
      {"pli_ctl_free_addr", {.willReturn = true, .alwaysInline = true, .mem = RtMemOtherMod}},
      {"pli_ctl_set_dim", {.willReturn = false, .mem = RtMemOtherMod}},
      // Runtime init/fini: touch global state, always return.
      {"pli_rt_init", {.willReturn = true, .mem = RtMemOtherMod}},
      {"pli_rt_fini", {.willReturn = true, .mem = RtMemOtherMod}},
      // ADR-234: the MAIN procedure runs as a task, so this calls
      // arbitrary PL/I code: it must never be memory(none).
      {"pli_sched_main", {.willReturn = true, .mem = RtMemOtherMod}},
      // CONTROLLED / ON / AREA readers: read global state, never write (ADR-111).
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
      {"pli_cosd", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_tand", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_atand", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_atan2", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_fixed_of_float", {.willReturn = true, .mem = RtMemNone}},
      // String readers: arg-pointed memory only, no error paths.
      {"pli_verify", {.willReturn = true, .mem = RtMemArgRead}},
      {"pli_tally", {.willReturn = true, .mem = RtMemArgRead}},
      {"pli_cmp_char", {.willReturn = true, .mem = RtMemArgRead}},
      {"pli_index", {.willReturn = true, .mem = RtMemArgRead}},
      {"pli_search", {.willReturn = true, .mem = RtMemArgRead}},
      {"pli_verify_from", {.willReturn = true, .mem = RtMemArgRead}},
      {"pli_rank", {.willReturn = true, .mem = RtMemArgRead}},
      {"pli_fixed_char_parts", {.willReturn = true, .mem = RtMemArgReadWrite}},
      {"pli_data_name_is", {.willReturn = true, .mem = RtMemArgRead}},
      // String writers: arg-pointed memory only, no error paths.
      {"pli_assign_char", {.willReturn = true, .mem = RtMemArgReadWrite}},
      {"pli_assign_varying", {.willReturn = true, .mem = RtMemArgReadWrite}},
      {"pli_concat", {.willReturn = true, .mem = RtMemArgReadWrite}},
      {"pli_substr", {.willReturn = true, .mem = RtMemArgReadWrite}},
      {"pli_substr_assign", {.willReturn = true, .mem = RtMemArgReadWrite}},
      {"pli_substr_assign_varying", {.willReturn = true, .mem = RtMemArgReadWrite}},
      {"pli_repeat", {.willReturn = true, .mem = RtMemArgReadWrite}},
      {"pli_translate", {.willReturn = true, .mem = RtMemArgReadWrite}},
      {"pli_trim", {.willReturn = true, .mem = RtMemArgReadWrite}},
      {"pli_uppercase", {.willReturn = true, .mem = RtMemArgReadWrite}},
      {"pli_lowercase", {.willReturn = true, .mem = RtMemArgReadWrite}},
      {"pli_center", {.willReturn = true, .mem = RtMemArgReadWrite}},
      {"pli_collate", {.willReturn = true, .mem = RtMemArgReadWrite}},
      {"pli_reverse", {.willReturn = true, .mem = RtMemArgReadWrite}},
      {"pli_high", {.willReturn = true, .mem = RtMemArgReadWrite}},
      {"pli_low", {.willReturn = true, .mem = RtMemArgReadWrite}},
      {"pli_char_of_fixed", {.willReturn = true, .mem = RtMemArgReadWrite}},
      {"pli_char_of_float", {.willReturn = true, .mem = RtMemArgReadWrite}},
      {"pli_char_of_complex", {.willReturn = true, .mem = RtMemArgReadWrite}},
      {"pli_isochar", {.willReturn = true, .mem = RtMemArgReadWrite}},
      {"pli_file_open_slot", {.willReturn = true, .mem = RtMemReadonly}},
      {"pli_lineno", {.willReturn = true, .mem = RtMemReadonly}},
      {"pli_pageno", {.willReturn = true, .mem = RtMemReadonly}},
      {"pli_set_pageno", {.willReturn = false, .mem = RtMemOtherMod}},
      {"pli_on_source", {.willReturn = true, .mem = RtMemReadonly}},
      {"pli_set_on_source", {.willReturn = false, .mem = RtMemOtherMod}},
      {"pli_unspec_assign", {.willReturn = false, .mem = RtMemOtherMod}},
      {"pli_onkey", {.willReturn = true, .mem = RtMemReadonly}},
      // ON handler stack (rules (91)-(94)): readers only read the
      // thread-local stack, writers push/pop/reset it. Push may abort on
      // overflow, so it claims no willReturn; the rest always return.
      // T2a: mark readers always-inline so LLVM folds the inlined scan into
      // the trap block; T2b: the pair functions replace the two-scan dispatch.
      {"pli_on_top_cond", {.willReturn = true, .alwaysInline = true, .mem = RtMemReadonly}},
      {"pli_on_top_ctx", {.willReturn = true, .alwaysInline = true, .mem = RtMemReadonly}},
      {"pli_on_top_fn", {.willReturn = true, .alwaysInline = true, .mem = RtMemReadonly}},
      {"pli_on_top_pair", {.willReturn = true, .alwaysInline = true, .mem = RtMemArgReadWrite}},
      {"pli_on_top_fn_pair", {.willReturn = true, .alwaysInline = true, .mem = RtMemArgReadWrite}},
      {"pli_on_top_error", {.willReturn = true, .alwaysInline = true, .mem = RtMemReadonly}},
      {"pli_on_depth_error", {.willReturn = true, .mem = RtMemReadonly}},
      {"pli_on_pop_cond", {.willReturn = true, .mem = RtMemOtherMod}},
      {"pli_on_pop_error", {.willReturn = true, .mem = RtMemOtherMod}},
      {"pli_on_reset_error", {.willReturn = true, .mem = RtMemOtherMod}},
      {"pli_on_push_cond", {.mem = RtMemOtherMod}},
      {"pli_on_push_error", {.mem = RtMemOtherMod}},
      {"pli_empty_area", {.willReturn = true, .alwaysInline = true, .mem = RtMemNone}},
      {"pli_put_edit_pic", {.willReturn = true, .alwaysInline = true, .mem = RtMemOtherMod}},
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

// Re-apply the audited P0 side-table facts to a runtime definition merged
// from the embedded bitcode. Unknown names (user functions) are left alone —
// unlike applyRuntimeAttrs, this never stamps NoUnwind on names outside the table.
void pli::applyRuntimeFnAttrs(llvm::Function& f) {
  if (kRuntimeAttrs().count(f.getName().str()) == 0)
    return;
  applyRuntimeAttrs(&f);
}

// The name of a memory effect, for the --print-facts report.
static const char* effectName(pli::MemEffect e) {
  switch (e) {
  case pli::MemEffect::None:
    return "none";
  case pli::MemEffect::ArgRead:
    return "argmem-read";
  case pli::MemEffect::ArgReadWrite:
    return "argmem-readwrite";
  case pli::MemEffect::Read:
    return "read";
  case pli::MemEffect::Unknown:
    break;
  }
  return "unknown";
}

// Stamp the memory effect the decision layer computed for a procedure
// (ADR-264). The attribute is the only channel: a PL/I-specific notion that
// LLVM cannot express as an attribute gets dropped with a note in
// --print-facts, not a custom pass (the plan's C1).
static void applyMemoryEffect(llvm::Function* f, pli::MemEffect e) {
  switch (e) {
  case pli::MemEffect::None:
    f->setMemoryEffects(llvm::MemoryEffects::none());
    break;
  case pli::MemEffect::ArgRead:
    f->setMemoryEffects(llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref));
    break;
  case pli::MemEffect::ArgReadWrite:
    f->setMemoryEffects(llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::ModRef));
    break;
  case pli::MemEffect::Read:
    f->setMemoryEffects(llvm::MemoryEffects::readOnly());
    break;
  case pli::MemEffect::Unknown:
    break; // no promise: the function keeps LLVM's default
  }
}

// --- WS-A2 (ADR-267): the digest of a PL/I signature ----------------------
//
// A published promise names the signature it was published for. The digest is
// over the PL/I types, not the LLVM ones, so it survives the other module
// being rebuilt with a different compiler version.
static std::string digestOf(const std::vector<Type>& params, bool isFunction, const Type& retTy) {
  uint64_t h = 1469598103934665603ULL; // FNV-1a
  auto mix = [&](uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      h ^= (v >> (i * 8)) & 0xff;
      h *= 1099511628211ULL;
    }
  };
  auto mixType = [&](const Type& t) {
    mix(static_cast<uint64_t>(t.k));
    mix(static_cast<uint64_t>(t.prec));
    mix(static_cast<uint64_t>(t.scale));
    mix(static_cast<uint64_t>(t.len));
    mix(t.varying ? 1 : 0);
    mix(t.starLen ? 1 : 0);
    mix(t.dims.size());
    for (const auto& d : t.dims) {
      mix(static_cast<uint64_t>(d.lb));
      mix(static_cast<uint64_t>(d.ub));
      mix(d.dyn ? 1 : 0);
      mix(d.lbDyn ? 1 : 0);
      mix(d.adj ? 1 : 0);
    }
  };
  mix(isFunction ? 1 : 0);
  mix(params.size());
  for (const auto& t : params)
    mixType(t);
  mixType(retTy);
  char buf[17];
  snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
  return buf;
}

// The digest of a definition's signature: the promise is published for it.
std::string IRGen::definitionDigest(const HProc* p) const {
  std::vector<Type> params;
  for (const Symbol* s : p->paramSyms)
    params.push_back(s->ty);
  return digestOf(params, p->isFunction, p->retTy);
}

// The digest of a declaration's signature: the promise is consumed for it.
static std::string declarationDigest(const Symbol& sym) {
  return digestOf(sym.entryParams, sym.entryIsFunction, sym.entryRetTy);
}

// WS-A2 (ADR-267): apply a published promise to an external declaration.
// The caller's optimizer cannot see the callee's body, so this description is
// all it has; without it the call keeps LLVM's default effects and cannot be
// hoisted or speculated. The promise is used only when it was published for
// this signature: a promise for a different procedure is not a promise here.
void IRGen::applyPublishedEffect(llvm::Function* f, const Symbol* sym) {
  if (!sym)
    return;
  auto it = published_.find(sym->name);
  if (it == published_.end())
    return;
  const pli::PublishedEffect& pub = it->second;
  if (pub.digest != declarationDigest(*sym))
    return; // published for a different signature: claim nothing
  applyMemoryEffect(f, pub.effects.effect);
  if (pub.effects.nounwind)
    f->setDoesNotThrow();
  if (pub.effects.willReturn)
    f->setWillReturn();
}

// --- Lowering policy (design-docs/mlir-runtime-migration-plan.md §5.2) ---

bool IRGen::hasLLVMLowering(const std::string& op) { return pli::hasLLVMLowering(op); }

// The lowering policy as --print-facts prints it: the base mode, then any
// per-operation override (ADR-260).
static std::string loweringModeName(LowerMode mode,
                                    const std::map<std::string, LowerMode>& perOp) {
  auto name = [](LowerMode m) -> const char* {
    switch (m) {
    case LowerMode::Runtime: return "runtime";
    case LowerMode::LLVM: return "llvm";
    case LowerMode::MLIR: return "mlir";
    case LowerMode::Auto: return "auto";
    }
    return "auto";
  };
  std::string out = name(mode);
  for (const auto& kv : perOp)
    out += " (" + kv.first + "=" + name(kv.second) + ")";
  return out;
}

// The lowering decision for one call site (ADR-254): the path, and the
// reason. The user's --experimental-lowering override wins — it is the
// differential oracle (constraint C3) — otherwise the decision layer's
// policy decides (constraint C2).
pli::InlineDecision IRGen::inlineDecision(const std::string& op, const pli::CallSite& site) {
  pli::InlineDecision d = pli::decideInline(op, site);
  LowerMode mode = lowerMode_;
  auto oit = perOpOverrides_.find(op);
  if (oit != perOpOverrides_.end())
    mode = oit->second;
  switch (mode) {
  case LowerMode::Runtime:
    d.path = pli::InlinePath::Library;
    d.reason = "the lowering mode forces the library path";
    break;
  case LowerMode::MLIR:
    // MLIR not compiled in this build (P2+ gates it). Fall back to runtime.
    d.path = pli::InlinePath::Library;
    d.reason = "the lowering mode is MLIR, which this build does not have";
    break;
  case LowerMode::LLVM:
    // A compile-time table is an inline path, so the forced mode keeps it.
    if (d.path != pli::InlinePath::ConstantTable) {
      if (pli::hasLLVMLowering(op)) {
        d.path = pli::InlinePath::LLVM;
        d.reason = "the lowering mode forces the inline path";
      } else {
        d.path = pli::InlinePath::Library;
        d.reason = "no inline expansion for this operation";
      }
    }
    break;
  case LowerMode::Auto:
    break;
  }
  return d;
}

bool IRGen::runtimePath(const std::string& op) {
  return inlineDecision(op, {}).library();
}

bool IRGen::useRuntimeCall(const std::string& op) {
  pli::InlineDecision d = inlineDecision(op, {});
  bool useRuntime = d.library();
  // Record the decision so --print-facts can show it (ADR-260), with the
  // reason the decision layer gave (ADR-254).
  facts_.recordBuiltin(op, useRuntime ? "runtime" : "llvm", d.reason);
  // The library-call remark (ADR-254): IBM flags every conversion a library
  // routine does (zos:23609-23610), so the user can find the hot spots.
  if (useRuntime && remarkLibraryCalls_)
    d_.remark(curLoc_, "the library routine does this conversion: " + op + " (" + d.reason + ")");
  return useRuntime;
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

// --- W4 dispatch wrappers (P3) ---

void IRGen::emitCharOfFixed(llvm::Value* dst, llvm::Value* dstcap, llvm::Value* v) {
  if (useRuntimeCall("char_of_fixed")) {
    b_.CreateCall(runtimeFn("pli_char_of_fixed"), {dst, dstcap, v});
    return;
  }
  emitCharOfFixedLLVM(dst, dstcap, v);
}

void IRGen::emitCharOfFloat(llvm::Value* dst, llvm::Value* dstcap, llvm::Value* x) {
  if (useRuntimeCall("char_of_float")) {
    b_.CreateCall(runtimeFn("pli_char_of_float"), {dst, dstcap, x});
    return;
  }
  emitCharOfFloatLLVM(dst, dstcap, x);
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

// The membership table of a constant character alphabet: entry c is 1 when
// the character code c is in the alphabet, 0 otherwise. The table is a
// module-level constant, one per distinct alphabet, reused by every call
// site that uses it (MULTICS compile_exp.pl1:1348-1380 built the same table
// at compile time).
llvm::GlobalVariable* IRGen::searchTable(const std::string& alphabet) {
  auto it = searchTables_.find(alphabet);
  if (it != searchTables_.end())
    return it->second;
  // The table's bytes are a LLVM-free decision (ADR-241, the plan's C2).
  std::string bytes = pli::searchTableBytes(alphabet);
  llvm::Constant* data = llvm::ConstantDataArray::get(
      ctx_, llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
  auto* g = new llvm::GlobalVariable(mod_, llvm::ArrayType::get(b_.getInt8Ty(), 256), true,
                                     llvm::GlobalValue::PrivateLinkage, data, "pli_search_table");
  g->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  searchTables_[alphabet] = g;
  return g;
}

// The translate table of a constant (out, in) pair (ADR-273): entry c is the
// character a byte with value c maps to, so the scan of `in` becomes one
// lookup per character.
llvm::GlobalVariable* IRGen::translateTable(const std::string& out, const std::string& in) {
  std::string key = out;
  key += '\0';
  key += in;
  auto it = translateTables_.find(key);
  if (it != translateTables_.end())
    return it->second;
  // The table's bytes are a LLVM-free decision (ADR-273, the plan's C2).
  std::string bytes = pli::translateTableBytes(out, in);
  llvm::Constant* data = llvm::ConstantDataArray::get(
      ctx_, llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
  auto* g =
      new llvm::GlobalVariable(mod_, llvm::ArrayType::get(b_.getInt8Ty(), 256), true,
                               llvm::GlobalValue::PrivateLinkage, data, "pli_translate_table");
  g->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  translateTables_[key] = g;
  return g;
}

// VERIFY with a constant alphabet: the first character of `s` that is *not*
// in the alphabet, 1-based; 0 when every character is in it. The table
// replaces the inner scan of the alphabet (ADR-241).
llvm::Value* IRGen::emitVerifyTable(llvm::Value* s, llvm::Value* sLen, llvm::Value* table) {
  llvm::Function* F = b_.GetInsertBlock()->getParent();
  llvm::Value* zero = llvm::ConstantInt::get(b_.getInt64Ty(), 0);
  llvm::Value* sLen64 = b_.CreateZExtOrTrunc(sLen, b_.getInt64Ty(), "vrf.slen");

  llvm::BasicBlock* outerBB = llvm::BasicBlock::Create(ctx_, "vrf.outer", F);
  llvm::BasicBlock* checkBB = llvm::BasicBlock::Create(ctx_, "vrf.check_byte", F);
  llvm::BasicBlock* latchBB = llvm::BasicBlock::Create(ctx_, "vrf.outer.latch", F);
  llvm::BasicBlock* foundBB = llvm::BasicBlock::Create(ctx_, "vrf.nomatch", F);
  llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(ctx_, "vrf.done", F);
  llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(ctx_, "vrf.exit", F);

  llvm::BasicBlock* entryBB = b_.GetInsertBlock();
  llvm::Value* sEmpty = b_.CreateICmpULE(sLen64, zero, "vrf.s_empty");
  b_.CreateCondBr(sEmpty, doneBB, outerBB);

  startBlock(outerBB);
  llvm::PHINode* iPhi = b_.CreatePHI(b_.getInt64Ty(), 2, "vrf.i");
  iPhi->addIncoming(zero, entryBB);
  llvm::Value* iGeSlen = b_.CreateICmpUGE(iPhi, sLen64, "vrf.i_ge_slen");
  b_.CreateCondBr(iGeSlen, doneBB, checkBB);

  startBlock(checkBB);
  llvm::Value* siPtr = b_.CreateGEP(b_.getInt8Ty(), s, {iPhi}, "vrf.si_ptr");
  llvm::Value* si = b_.CreateLoad(b_.getInt8Ty(), siPtr, "vrf.si");
  // The character code is the byte value (0..255), which indexes the table.
  llvm::Value* code = b_.CreateZExt(si, b_.getInt32Ty(), "vrf.code");
  llvm::Value* tblPtr = b_.CreateGEP(b_.getInt8Ty(), table, {code}, "vrf.tbl_ptr");
  llvm::Value* inSet = b_.CreateLoad(b_.getInt8Ty(), tblPtr, "vrf.in_set");
  llvm::Value* notIn =
      b_.CreateICmpEQ(inSet, llvm::ConstantInt::get(b_.getInt8Ty(), 0), "vrf.not_in");
  b_.CreateCondBr(notIn, foundBB, latchBB);

  startBlock(latchBB);
  iPhi->addIncoming(b_.CreateAdd(iPhi, llvm::ConstantInt::get(b_.getInt64Ty(), 1), "vrf.i_next"),
                    latchBB);
  b_.CreateBr(outerBB);

  startBlock(foundBB);
  llvm::Value* matchResult =
      b_.CreateAdd(iPhi, llvm::ConstantInt::get(b_.getInt64Ty(), 1), "vrf.result");
  b_.CreateBr(exitBB);

  startBlock(doneBB);
  b_.CreateBr(exitBB);

  startBlock(exitBB);
  llvm::PHINode* resultPhi = b_.CreatePHI(b_.getInt64Ty(), 2, "vrf.out");
  resultPhi->addIncoming(matchResult, foundBB);
  resultPhi->addIncoming(zero, doneBB);
  return resultPhi;
}

// VERIFY(s,t,from) with a constant alphabet: as emitVerifyTable, but the
// scan starts at position `start` (1-based, clamped to 1).
llvm::Value* IRGen::emitVerifyFromTable(llvm::Value* s, llvm::Value* sLen, llvm::Value* table,
                                        llvm::Value* start) {
  llvm::Function* F = b_.GetInsertBlock()->getParent();
  llvm::Value* zero = llvm::ConstantInt::get(b_.getInt64Ty(), 0);
  llvm::Value* one64 = llvm::ConstantInt::get(b_.getInt64Ty(), 1);
  llvm::Value* sLen64 = b_.CreateZExtOrTrunc(sLen, b_.getInt64Ty(), "vrf.slen");
  llvm::Value* start64 = b_.CreateZExtOrTrunc(start, b_.getInt64Ty(), "vrf.start");
  llvm::Value* clamped =
      b_.CreateSelect(b_.CreateICmpULT(start64, one64), one64, start64, "vrf.clamped");
  llvm::Value* iStart = b_.CreateSub(clamped, one64, "vrf.istart");

  llvm::BasicBlock* outerBB = llvm::BasicBlock::Create(ctx_, "vrf.outer", F);
  llvm::BasicBlock* checkBB = llvm::BasicBlock::Create(ctx_, "vrf.check_byte", F);
  llvm::BasicBlock* latchBB = llvm::BasicBlock::Create(ctx_, "vrf.outer.latch", F);
  llvm::BasicBlock* foundBB = llvm::BasicBlock::Create(ctx_, "vrf.nomatch", F);
  llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(ctx_, "vrf.done", F);
  llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(ctx_, "vrf.exit", F);

  llvm::BasicBlock* entryBB = b_.GetInsertBlock();
  llvm::Value* sEmpty = b_.CreateICmpULE(sLen64, zero, "vrf.s_empty");
  b_.CreateCondBr(sEmpty, doneBB, outerBB);

  startBlock(outerBB);
  llvm::PHINode* iPhi = b_.CreatePHI(b_.getInt64Ty(), 2, "vrf.i");
  iPhi->addIncoming(iStart, entryBB);
  llvm::Value* iGeSlen = b_.CreateICmpUGE(iPhi, sLen64, "vrf.i_ge_slen");
  b_.CreateCondBr(iGeSlen, doneBB, checkBB);

  startBlock(checkBB);
  llvm::Value* siPtr = b_.CreateGEP(b_.getInt8Ty(), s, {iPhi}, "vrf.si_ptr");
  llvm::Value* si = b_.CreateLoad(b_.getInt8Ty(), siPtr, "vrf.si");
  llvm::Value* code = b_.CreateZExt(si, b_.getInt32Ty(), "vrf.code");
  llvm::Value* tblPtr = b_.CreateGEP(b_.getInt8Ty(), table, {code}, "vrf.tbl_ptr");
  llvm::Value* inSet = b_.CreateLoad(b_.getInt8Ty(), tblPtr, "vrf.in_set");
  llvm::Value* notIn =
      b_.CreateICmpEQ(inSet, llvm::ConstantInt::get(b_.getInt8Ty(), 0), "vrf.not_in");
  b_.CreateCondBr(notIn, foundBB, latchBB);

  startBlock(latchBB);
  iPhi->addIncoming(b_.CreateAdd(iPhi, one64, "vrf.i_next"), latchBB);
  b_.CreateBr(outerBB);

  startBlock(foundBB);
  llvm::Value* matchResult = b_.CreateAdd(iPhi, one64, "vrf.result");
  b_.CreateBr(exitBB);

  startBlock(doneBB);
  b_.CreateBr(exitBB);

  startBlock(exitBB);
  llvm::PHINode* resultPhi = b_.CreatePHI(b_.getInt64Ty(), 2, "vrf.out");
  resultPhi->addIncoming(matchResult, foundBB);
  resultPhi->addIncoming(zero, doneBB);
  return resultPhi;
}

// SEARCH with a constant alphabet: the first character of `s` that *is* in
// the alphabet, 1-based, starting at `start` (clamped to 1); 0 when there is
// none (ADR-241).
llvm::Value* IRGen::emitSearchTable(llvm::Value* s, llvm::Value* sLen, llvm::Value* table,
                                    llvm::Value* start) {
  llvm::Function* F = b_.GetInsertBlock()->getParent();
  llvm::Value* zero = llvm::ConstantInt::get(b_.getInt64Ty(), 0);
  llvm::Value* one64 = llvm::ConstantInt::get(b_.getInt64Ty(), 1);
  llvm::Value* sLen64 = b_.CreateZExtOrTrunc(sLen, b_.getInt64Ty(), "srch.slen");
  llvm::Value* start64 = b_.CreateZExtOrTrunc(start, b_.getInt64Ty(), "srch.start");
  llvm::Value* clamped =
      b_.CreateSelect(b_.CreateICmpULT(start64, one64), one64, start64, "srch.clamped");
  llvm::Value* iStart = b_.CreateSub(clamped, one64, "srch.istart");

  llvm::BasicBlock* outerBB = llvm::BasicBlock::Create(ctx_, "srch.outer", F);
  llvm::BasicBlock* checkBB = llvm::BasicBlock::Create(ctx_, "srch.check_byte", F);
  llvm::BasicBlock* latchBB = llvm::BasicBlock::Create(ctx_, "srch.outer.latch", F);
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
  b_.CreateCondBr(iGeSlen, doneBB, checkBB);

  startBlock(checkBB);
  llvm::Value* siPtr = b_.CreateGEP(b_.getInt8Ty(), s, {iPhi}, "srch.si_ptr");
  llvm::Value* si = b_.CreateLoad(b_.getInt8Ty(), siPtr, "srch.si");
  llvm::Value* code = b_.CreateZExt(si, b_.getInt32Ty(), "srch.code");
  llvm::Value* tblPtr = b_.CreateGEP(b_.getInt8Ty(), table, {code}, "srch.tbl_ptr");
  llvm::Value* inSet = b_.CreateLoad(b_.getInt8Ty(), tblPtr, "srch.in_set");
  llvm::Value* isIn =
      b_.CreateICmpNE(inSet, llvm::ConstantInt::get(b_.getInt8Ty(), 0), "srch.is_in");
  b_.CreateCondBr(isIn, matchedBB, latchBB);

  startBlock(latchBB);
  iPhi->addIncoming(b_.CreateAdd(iPhi, one64, "srch.i_next"), latchBB);
  b_.CreateBr(outerBB);

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

// verify(s, t[, start]): the first character of s that is absent from the
// alphabet t, 1-based. A constant alphabet uses the membership table.
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

// Fetch (or insert) a libc scan declaration with the full audited fact set:
// nounwind + willreturn + nofree/nosync/nocallback + argmem-read-only +
// speculatable. Speculation soundness: every emitted call passes valid
// string pointers with honest in-bounds lengths (the memcmp operands are
// additionally clamped to a zero-length no-read on unusable candidates), so
// speculative execution can neither fault nor observe anything — mirroring
// clang's own treatment of these builtins. Lets LICM hoist the calls out of
// conditional blocks in hot loops (P4).
llvm::Function* IRGen::scanFn(const std::string& name, llvm::Type* ret,
                              llvm::ArrayRef<llvm::Type*> params) {
  llvm::Function* f = llvm::cast<llvm::Function>(
      b_.GetInsertBlock()->getModule()->getOrInsertFunction(name,
          llvm::FunctionType::get(ret, params, false)).getCallee());
  f->addFnAttr(llvm::Attribute::NoUnwind);
  f->addFnAttr(llvm::Attribute::WillReturn);
  f->addFnAttr(llvm::Attribute::NoFree);
  f->addFnAttr(llvm::Attribute::NoSync);
  f->addFnAttr(llvm::Attribute::NoCallback);
  f->addFnAttr(llvm::Attribute::Speculatable);
  f->setMemoryEffects(llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref));
  return f;
}

// P7.2: detect a constant string needle (pointer to a private GlobalVariable
// initializer that is a ConstantDataArray of i8 with known content).
// Returns the pointer-stripped global + the byte span, or nullptr.
static std::pair<llvm::GlobalVariable*, llvm::StringRef>
extractConstantNeedle(llvm::Value* v) {
  if (auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(v->stripPointerCasts())) {
    if (gv->isConstant()) {
      if (auto* ca = llvm::dyn_cast<llvm::ConstantDataArray>(gv->getInitializer())) {
        if (ca->isString()) {
          return {gv, ca->getAsString()};
        }
      }
    }
  }
  return {};
}

// Direct LLVM lowering for INDEX (P1+). Mirrors pli_index in rt_string.c:
//   blen <= 0 -> 1; blen > alen -> 0; else scan a for first run where
//   a[pos..pos+blen-1] == b[0..blen-1].
//
// Fast path: a single memchr(a, b[0], alen) call with loop-invariant
// arguments, followed by a single memcmp(a+safePos, b, safeLen)
// verification. Both calls are argmem-read-only and unconditional within
// this block, so once the blen > alen guard above folds (loop-invariant),
// LLVM's LICM hoists each out of hot loops — matching strstr codegen parity
// in the benchmark harness. The backend lowers memcmp to a SIMD compare
// instead of an IR-level byte loop (P4 verify-loop vectorization).
// Clamping (safePos/safeLen collapse to (0, 0) on a memchr miss or overrun)
// keeps the memcmp in bounds on every path, including speculative ones.
//
// Slow path: falls back to pli_index runtime call for the rare case where
// the first byte of the needle appears before the actual match position.
//
// P7.2 SIMD fast-path: for constant needles of length 2..16, replaces
// the memchr+memcmp pair with a target-detection branch that uses SIMD
// intrinsics (NEON cmeq on ARM64, SSE4.2 pcmpestrm on x86) for vectorized
// verification. The slow path loops back to memchr from the next candidate
// instead of calling the pli_index runtime, eliminating per-iteration runtime
// overhead for constant needles. This closes the gap where the C baseline
// uses strstr (libc SIMD) and we fall through to a scalar pli_index loop.
llvm::Value* IRGen::emitIndexLLVM(llvm::Value* a, llvm::Value* aLen, llvm::Value* b,
                                  llvm::Value* bLen) {
  // P7.2: dispatch to SIMD fast-path for constant needles of length 2..16
  if (auto [gv, str] = extractConstantNeedle(b); gv && !str.empty()) {
    llvm::Triple tgtTriple(triple_);
    // The needle length is the runtime string length, not str.size() (the
    // ConstantDataArray may carry a trailing NUL). A constant length lets the
    // x86 path compare a fixed-width vector.
    if (auto* blenC = llvm::dyn_cast<llvm::ConstantInt>(bLen)) {
      uint64_t realLen = blenC->getZExtValue();
      if (realLen >= 1 && realLen <= 16 &&
          tgtTriple.getArch() == llvm::Triple::x86_64)
        return emitIndexConstantSSE2(a, aLen, b, bLen, (unsigned char)str[0], realLen);
    }
    uint64_t nlen = str.size();
    // ConstantDataArray::getString stores a NUL terminator; the needle
    // length is the value bLen (not str.size, which includes '\0').
    // We use nlen as upper bound on the needle length we can handle
    // with a single SIMD register (16 bytes).
    if (nlen >= 2 && nlen <= 17) {
      if (tgtTriple.getArch() == llvm::Triple::aarch64)
        return emitIndexConstantNEON(a, aLen, b, bLen, str);
    }
  }

  llvm::Function* F = b_.GetInsertBlock()->getParent();
  llvm::Value* zero = llvm::ConstantInt::get(b_.getInt64Ty(), 0);
  llvm::Value* one64 = llvm::ConstantInt::get(b_.getInt64Ty(), 1);
  llvm::Value* aLen64 = b_.CreateZExtOrTrunc(aLen, b_.getInt64Ty(), "idx.alen");
  llvm::Value* bLen64 = b_.CreateZExtOrTrunc(bLen, b_.getInt64Ty(), "idx.blen");

  llvm::BasicBlock* checkBB = llvm::BasicBlock::Create(ctx_, "idx.check", F);
  llvm::BasicBlock* ret1BB = llvm::BasicBlock::Create(ctx_, "idx.ret1", F);
  llvm::BasicBlock* ret0BB = llvm::BasicBlock::Create(ctx_, "idx.ret0", F);
  llvm::BasicBlock* fastPathBB = llvm::BasicBlock::Create(ctx_, "idx.fast_path", F);
  llvm::BasicBlock* verifyBB = llvm::BasicBlock::Create(ctx_, "idx.verify", F);
  llvm::BasicBlock* matchBB = llvm::BasicBlock::Create(ctx_, "idx.match", F);
  llvm::BasicBlock* slowPathBB = llvm::BasicBlock::Create(ctx_, "idx.slow_path", F);
  llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(ctx_, "idx.exit", F);

  // Entry: blen <= 0 -> 1; else check blen > alen -> 0; else fast path.
  llvm::Value* blenLe0 = b_.CreateICmpULE(bLen64, zero, "idx.blenle0");
  b_.CreateCondBr(blenLe0, ret1BB, checkBB);

  // ret1: empty needle -> result = 1.
  startBlock(ret1BB);
  b_.CreateBr(exitBB);

  // check: blen > alen -> 0; else fast-path memchr(a, b[0], alen).
  startBlock(checkBB);
  llvm::Value* blenGtAlen = b_.CreateICmpUGT(bLen64, aLen64, "idx.blen_gt_alen");
  b_.CreateCondBr(blenGtAlen, ret0BB, fastPathBB);

  // ret0: no possible match -> result = 0.
  startBlock(ret0BB);
  b_.CreateBr(exitBB);

  // fast_path: single memchr(a, b[0], alen) with loop-invariant args so
  // LLVM LICM hoists it out of enclosing loops (matches strstr codegen).
  startBlock(fastPathBB);
  llvm::Value* b0 = b_.CreateLoad(b_.getInt8Ty(), b, "idx.b0");
  llvm::Value* b032 = b_.CreateZExt(b0, b_.getInt32Ty(), "idx.b032");

  llvm::Function* memchrFn = scanFn("memchr", b_.getPtrTy(),
                                      {b_.getPtrTy(), b_.getInt32Ty(), b_.getInt64Ty()});
  llvm::CallInst* foundCall = b_.CreateCall(memchrFn, {a, b032, aLen64}, "idx.memchr");
  foundCall->addFnAttr(llvm::Attribute::Speculatable);
  llvm::Value* found = foundCall;

  // found == null -> no match -> return 0
  llvm::Value* foundNull = b_.CreateICmpEQ(
      found, llvm::ConstantPointerNull::get(b_.getPtrTy()), "idx.found_null");

  // pos = found - a (candidate position in haystack)
  llvm::Value* foundInt = b_.CreatePtrToInt(found, b_.getInt64Ty(), "idx.found_int");
  llvm::Value* aInt = b_.CreatePtrToInt(a, b_.getInt64Ty(), "idx.a_int");
  llvm::Value* pos = b_.CreateSub(foundInt, aInt, "idx.pos");

  // pos + blen > alen -> needle extends past end -> return 0
  llvm::Value* posPlusBlen = b_.CreateAdd(pos, bLen64, "idx.pos_blen");
  llvm::Value* dontFit = b_.CreateICmpUGT(posPlusBlen, aLen64, "idx.dont_fit");

  llvm::Value* shouldExit = b_.CreateOr(foundNull, dontFit, "idx.should_exit");

  // Clamp the verify operands so the memcmp below is in bounds on every
  // path: (0, 0) reads nothing when the candidate is unusable, otherwise
  // the exact (pos, blen). Zero-length memcmp dereferences nothing, so the
  // call below is safe to execute (and hoist) unconditionally.
  llvm::Value* safePos = b_.CreateSelect(shouldExit, zero, pos, "idx.safe_pos");
  llvm::Value* safeLen = b_.CreateSelect(shouldExit, zero, bLen64, "idx.safe_len");
  llvm::Value* aPosPtr = b_.CreateGEP(b_.getInt8Ty(), a, {safePos}, "idx.apos_ptr");
  llvm::Function* memcmpFn = scanFn("memcmp", b_.getInt32Ty(),
                                      {b_.getPtrTy(), b_.getPtrTy(), b_.getInt64Ty()});
  llvm::CallInst* cmpCall = b_.CreateCall(memcmpFn, {aPosPtr, b, safeLen}, "idx.memcmp");
  cmpCall->addFnAttr(llvm::Attribute::Speculatable);
  llvm::Value* cmp = cmpCall;
  llvm::Value* cmpEq =
      b_.CreateICmpEQ(cmp, llvm::ConstantInt::get(b_.getInt32Ty(), 0), "idx.cmp_eq");
  llvm::Value* verified =
      b_.CreateAnd(cmpEq, b_.CreateNot(shouldExit, "idx.not_exit"), "idx.verified");

  // Miss/overrun exits without touching the slow path; otherwise the
  // verified flag picks match vs. slow path in the tail block.
  b_.CreateCondBr(shouldExit, ret0BB, verifyBB);

  // verify: tail branch only (all computation hoisted above so LICM can
  // lift the memchr+memcmp pair out of enclosing loops together).
  startBlock(verifyBB);
  b_.CreateCondBr(verified, matchBB, slowPathBB);

  // match: result = pos + 1 (PL/I is 1-based).
  startBlock(matchBB);
  llvm::Value* matchResult = b_.CreateAdd(pos, one64, "idx.result");
  b_.CreateBr(exitBB);

  // slow_path: first candidate didn't match -> delegate to runtime pli_index
  // (handles arbitrary needle patterns with full correctness).
  startBlock(slowPathBB);
  llvm::Value* slowResult =
      b_.CreateCall(runtimeFn("pli_index"), {a, aLen, b, bLen}, "idx.slow");
  b_.CreateBr(exitBB);

  // exit: merge all result values.
  startBlock(exitBB);
  llvm::PHINode* resultPhi = b_.CreatePHI(b_.getInt64Ty(), 4, "idx.out");
  resultPhi->addIncoming(one64, ret1BB);
  resultPhi->addIncoming(zero, ret0BB);
  resultPhi->addIncoming(matchResult, matchBB);
  resultPhi->addIncoming(slowResult, slowPathBB);

   return resultPhi;
}

// SSE2-accelerated INDEX for a constant needle on x86-64. glibc's memchr is
// SIMD, but the static musl runtime the compiler links is not, so the INDEX
// fast path pays a scalar scan on Linux. Scan 16 bytes at a time with the
// baseline SSE2 compare, then verify the candidate with memcmp. The vector
// load is bounded by rem >= 16, so it never reads past the haystack.
llvm::Value* IRGen::emitIndexConstantSSE2(llvm::Value* a, llvm::Value* aLen, llvm::Value* b,
                                          llvm::Value* bLen, unsigned char first,
                                          uint64_t needleLen) {
  llvm::Function* F = b_.GetInsertBlock()->getParent();
  llvm::Type* i64 = b_.getInt64Ty();
  llvm::Value* zero = llvm::ConstantInt::get(i64, 0);
  llvm::Value* one = llvm::ConstantInt::get(i64, 1);
  llvm::Value* aLen64 = b_.CreateZExtOrTrunc(aLen, i64, "idx.alen");
  llvm::Value* bLen64 = b_.CreateZExtOrTrunc(bLen, i64, "idx.blen");
  llvm::VectorType* vecTy = llvm::VectorType::get(b_.getInt8Ty(), 16, false);
  llvm::Value* firstVec = llvm::ConstantVector::getSplat(
      llvm::ElementCount::get(16, false), llvm::ConstantInt::get(b_.getInt8Ty(), first));
  llvm::Value* firstScalar = llvm::ConstantInt::get(b_.getInt8Ty(), first);
  llvm::Function* memcmpFn =
      scanFn("memcmp", b_.getInt32Ty(), {b_.getPtrTy(), b_.getPtrTy(), i64});

  // The CFG: guards, a 16-byte scan loop, a scalar tail, and one verify block.
  llvm::BasicBlock* ret1BB = llvm::BasicBlock::Create(ctx_, "idx.ret1", F);
  llvm::BasicBlock* ret0BB = llvm::BasicBlock::Create(ctx_, "idx.ret0", F);
  llvm::BasicBlock* checkBB = llvm::BasicBlock::Create(ctx_, "idx.check", F);
  llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(ctx_, "idx.sse.loop", F);
  llvm::BasicBlock* vecBB = llvm::BasicBlock::Create(ctx_, "idx.sse.vec", F);
  llvm::BasicBlock* vecNextBB = llvm::BasicBlock::Create(ctx_, "idx.sse.vec_next", F);
  llvm::BasicBlock* vecFoundBB = llvm::BasicBlock::Create(ctx_, "idx.sse.found", F);
  llvm::BasicBlock* tailBB = llvm::BasicBlock::Create(ctx_, "idx.sse.tail", F);
  llvm::BasicBlock* tailCheckBB = llvm::BasicBlock::Create(ctx_, "idx.sse.tail_check", F);
  llvm::BasicBlock* tailNextBB = llvm::BasicBlock::Create(ctx_, "idx.sse.tail_next", F);
  llvm::BasicBlock* verifyBB = llvm::BasicBlock::Create(ctx_, "idx.sse.verify", F);
  llvm::BasicBlock* verifyMemBB = llvm::BasicBlock::Create(ctx_, "idx.sse.verify_mem", F);
  llvm::BasicBlock* verifyNextBB = llvm::BasicBlock::Create(ctx_, "idx.sse.verify_next", F);
  llvm::BasicBlock* matchBB = llvm::BasicBlock::Create(ctx_, "idx.sse.match", F);
  llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(ctx_, "idx.sse.exit", F);

  // An empty needle matches at 1; a needle longer than the haystack cannot.
  b_.CreateCondBr(b_.CreateICmpULE(bLen64, zero, "idx.blenle0"), ret1BB, checkBB);
  startBlock(checkBB);
  b_.CreateCondBr(b_.CreateICmpUGT(bLen64, aLen64, "idx.blen_gt_alen"), ret0BB, loopBB);

  // The scan position: entry, after a full vector, after a scalar byte, or
  // after a candidate that did not verify.
  startBlock(loopBB);
  llvm::PHINode* pos = b_.CreatePHI(i64, 4, "idx.pos");
  pos->addIncoming(zero, checkBB);
  llvm::Value* rem = b_.CreateSub(aLen64, pos, "idx.rem");
  b_.CreateCondBr(b_.CreateICmpUGE(rem, llvm::ConstantInt::get(i64, 16), "idx.can_vec"),
                  vecBB, tailBB);

  startBlock(vecBB);
  llvm::Value* vecPtr = b_.CreateGEP(b_.getInt8Ty(), a, {pos}, "idx.vec_ptr");
  llvm::Value* hayVec = b_.CreateAlignedLoad(vecTy, vecPtr, llvm::Align(1), "idx.hay_vec");
  llvm::Value* cmpVec = b_.CreateICmpEQ(hayVec, firstVec, "idx.cmp_vec");
  llvm::Value* bits = b_.CreateBitCast(cmpVec, b_.getInt16Ty(), "idx.bits");
  b_.CreateCondBr(b_.CreateICmpNE(bits, llvm::ConstantInt::get(b_.getInt16Ty(), 0),
                                  "idx.any"),
                  vecFoundBB, vecNextBB);

  startBlock(vecNextBB);
  llvm::Value* afterVec = b_.CreateAdd(pos, llvm::ConstantInt::get(i64, 16), "idx.next_vec");
  b_.CreateBr(loopBB);
  pos->addIncoming(afterVec, vecNextBB);

  startBlock(vecFoundBB);
  llvm::Function* cttz = llvm::Intrinsic::getOrInsertDeclaration(
      b_.GetInsertBlock()->getModule(), llvm::Intrinsic::cttz, {b_.getInt16Ty()});
  llvm::Value* off16 =
      b_.CreateCall(cttz, {bits, b_.getFalse()}, "idx.off16");
  llvm::Value* off = b_.CreateZExt(off16, i64, "idx.off");
  llvm::Value* vecCand = b_.CreateAdd(pos, off, "idx.vec_cand");
  b_.CreateBr(verifyBB);

  startBlock(tailBB);
  b_.CreateCondBr(b_.CreateICmpEQ(rem, zero, "idx.tail_done"), ret0BB, tailCheckBB);
  startBlock(tailCheckBB);
  llvm::Value* tailPtr = b_.CreateGEP(b_.getInt8Ty(), a, {pos}, "idx.tail_ptr");
  llvm::Value* tailCh = b_.CreateLoad(b_.getInt8Ty(), tailPtr, "idx.tail_ch");
  b_.CreateCondBr(b_.CreateICmpEQ(tailCh, firstScalar, "idx.tail_first"),
                  verifyBB, tailNextBB);
  startBlock(tailNextBB);
  llvm::Value* afterByte = b_.CreateAdd(pos, one, "idx.next_byte");
  b_.CreateBr(loopBB);
  pos->addIncoming(afterByte, tailNextBB);

  // Verify: the candidate must fit, then memcmp decides.
  startBlock(verifyBB);
  llvm::PHINode* cand = b_.CreatePHI(i64, 2, "idx.cand");
  cand->addIncoming(vecCand, vecFoundBB);
  cand->addIncoming(pos, tailCheckBB);
  llvm::Value* end = b_.CreateAdd(cand, llvm::ConstantInt::get(i64, needleLen), "idx.cand_end");
  b_.CreateCondBr(b_.CreateICmpULE(end, aLen64, "idx.cand_fits"), verifyMemBB, ret0BB);

  startBlock(verifyMemBB);
  llvm::Value* candPtr = b_.CreateGEP(b_.getInt8Ty(), a, {cand}, "idx.cand_ptr");
  llvm::Value* cmp = b_.CreateCall(
      memcmpFn, {candPtr, b, llvm::ConstantInt::get(i64, needleLen)}, "idx.memcmp");
  b_.CreateCondBr(b_.CreateICmpEQ(cmp, llvm::ConstantInt::get(b_.getInt32Ty(), 0),
                                  "idx.cmp_eq"),
                  matchBB, verifyNextBB);
  startBlock(verifyNextBB);
  llvm::Value* afterCand = b_.CreateAdd(cand, one, "idx.next_cand");
  b_.CreateBr(loopBB);
  pos->addIncoming(afterCand, verifyNextBB);

  startBlock(matchBB);
  llvm::Value* matchResult = b_.CreateAdd(cand, one, "idx.result");
  b_.CreateBr(exitBB);
  startBlock(ret0BB);
  b_.CreateBr(exitBB);
  startBlock(ret1BB);
  b_.CreateBr(exitBB);

  startBlock(exitBB);
  llvm::PHINode* result = b_.CreatePHI(i64, 3, "idx.out");
  result->addIncoming(one, ret1BB);
  result->addIncoming(zero, ret0BB);
  result->addIncoming(matchResult, matchBB);
  return result;
}

// P7.2: SIMD-accelerated INDEX for constant needles on ARM64 NEON.
//
// Algorithm:
//   1. Guard: blen <= 0 → 1; blen > alen → 0 (same as emitIndexLLVM).
//   2. Build a <16 x i8> needle vector (zero-padded) and a <16 x i8>
//      mask (first n bytes = 0xFF, rest = 0x00) from the constant needle.
//   3. Use memchr (libc NEON-internal) to find the first char of the
//      needle — fast SIMD character search (16 bytes/instruction).
//   4. Verify the candidate by loading <16 x i8> from haystack, XOR
//      with needle_vec.  ICmpEQ(diff, zeros) gives 0xFF where bytes match.
//      AND with mask_vec isolates the needle-length bytes.  If the result
//      equals mask_vec → all needle bytes match → return pos+1.
//   5. On verify miss, call memchr again from pos+1 (no pli_index runtime,
//      eliminating the scalar loop for constant needles).
//
// Target-detection note: on x86 we would use llvm.x86.sse41.pcmpestrm128
// which compares a 16-byte haystack against a needle in one hardware
// instruction; on ARM64 the cmeq approach is the equivalent. Both are
// gated on the target triple in emitIndexLLVM's dispatch.
llvm::Value* IRGen::emitIndexConstantNEON(llvm::Value* a, llvm::Value* aLen,                                           llvm::Value* /*b*/,
                                          llvm::Value* bLen, llvm::StringRef needleStr) {
  llvm::Function* F = b_.GetInsertBlock()->getParent();
  llvm::Value* zero = llvm::ConstantInt::get(b_.getInt64Ty(), 0);
  llvm::Value* one64 = llvm::ConstantInt::get(b_.getInt64Ty(), 1);
  llvm::Value* aLen64 = b_.CreateZExtOrTrunc(aLen, b_.getInt64Ty(), "idx.alen");
  llvm::Value* bLen64 = b_.CreateZExtOrTrunc(bLen, b_.getInt64Ty(), "idx.blen");
  uint64_t needleLen = needleStr.size();

  // <16 x i8> needle vector (zero-padded) + mask (first n bytes 0xFF).
  std::vector<llvm::Constant*> needleElems, maskElems;
  for (uint64_t i = 0; i < 16; ++i) {
    needleElems.push_back(llvm::ConstantInt::get(
        b_.getInt8Ty(), i < needleLen ? (uint8_t)needleStr[i] : 0));
    maskElems.push_back(llvm::ConstantInt::get(
        b_.getInt8Ty(), i < needleLen ? 0xFF : 0x00));
  }
  llvm::Value* needleVec = llvm::ConstantVector::get(needleElems);
  llvm::Value* maskVec = llvm::ConstantVector::get(maskElems);
  llvm::VectorType* vecTy = llvm::VectorType::get(b_.getInt8Ty(), 16, false);
  llvm::Value* zeroVec = llvm::ConstantVector::getSplat(
      llvm::ElementCount::get(16, false), llvm::ConstantInt::get(b_.getInt8Ty(), 0));

  // memchr function: reused for finding first-char candidates.
  llvm::Function* memchrFn = scanFn("memchr", b_.getPtrTy(),
                                    {b_.getPtrTy(), b_.getInt32Ty(), b_.getInt64Ty()});
  uint64_t firstChar = needleLen > 0 ? (uint8_t)needleStr[0] : 0;

  // Capture the current insertion block — it's the predecessor of searchBB
  // on first entry (we emit the condBr below into this block).
  llvm::BasicBlock* callerBB = b_.GetInsertBlock();

  // Basic blocks (built up-front; all PHIs get their incoming values set later).
  // CFG:
  //   caller → (blen<=0) → ret1 → exit
  //   caller → (blen>0) → search
  //   search: found=null → ret0 → exit
  //   search: found≠null → verify
  //   verify: match → matchBB → exit
  //   verify: miss → search (retry from pos+1)
  llvm::BasicBlock* ret1BB = llvm::BasicBlock::Create(ctx_, "idx.ret1", F);
  llvm::BasicBlock* ret0BB = llvm::BasicBlock::Create(ctx_, "idx.ret0", F);
  llvm::BasicBlock* searchBB = llvm::BasicBlock::Create(ctx_, "idx.simd_search", F);
  llvm::BasicBlock* verifyBB = llvm::BasicBlock::Create(ctx_, "idx.simd_verify", F);
  llvm::BasicBlock* matchBB = llvm::BasicBlock::Create(ctx_, "idx.simd_match", F);
  llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(ctx_, "idx.exit", F);

  // Entry: blen <= 0 → ret1; else search
  llvm::Value* blenLe0 = b_.CreateICmpULE(bLen64, zero, "idx.blenle0");
  b_.CreateCondBr(blenLe0, ret1BB, searchBB);  // callerBB is predecessor

  startBlock(ret1BB);
  b_.CreateBr(exitBB);

  // search: memchr(a + start, needle[0], aLen - start)
  startBlock(searchBB);
  llvm::PHINode* start = b_.CreatePHI(b_.getInt64Ty(), 2, "idx.start");
  start->addIncoming(zero, callerBB);

  // rem = aLen - start (bytes remaining)
  llvm::Value* rem = b_.CreateSub(aLen64, start, "idx.rem");
  llvm::Value* aStartPtr = b_.CreateGEP(b_.getInt8Ty(), a, {start}, "idx.astart");
  llvm::Value* firstChar32 = llvm::ConstantInt::get(b_.getInt32Ty(), firstChar);
  llvm::CallInst* foundCall = b_.CreateCall(memchrFn, {aStartPtr, firstChar32, rem}, "idx.memchr");
  foundCall->addFnAttr(llvm::Attribute::Speculatable);
  llvm::Value* found = foundCall;

  // found == null → ret0 (no more candidates)
  llvm::Value* foundNull = b_.CreateICmpEQ(
      found, llvm::ConstantPointerNull::get(b_.getPtrTy()), "idx.found_null");
  b_.CreateCondBr(foundNull, ret0BB, verifyBB);

  startBlock(verifyBB);
  // pos = found - a (absolute position in haystack)
  llvm::Value* foundInt = b_.CreatePtrToInt(found, b_.getInt64Ty(), "idx.found_int");
  llvm::Value* aInt = b_.CreatePtrToInt(a, b_.getInt64Ty(), "idx.a_int");
  llvm::Value* pos = b_.CreateSub(foundInt, aInt, "idx.pos");

  // Load 16 bytes from candidate position, XOR with needle_vec.
  // The load may read past the buffer boundary; since PL/I string buffers
  // are stack allocas with fixed size and the mask zeroes irrelevant bytes,
  // this is safe after SROA.
  llvm::Value* aPosPtr = b_.CreateGEP(b_.getInt8Ty(), a, {pos}, "idx.apos_ptr");
  llvm::Value* hayVec =
      b_.CreateAlignedLoad(vecTy, aPosPtr, llvm::Align(1), "idx.hay_vec");
  llvm::Value* diff = b_.CreateXor(hayVec, needleVec, "idx.diff");

  // AND with mask to isolate needle-length bytes: result is 0 where byte
  // matches (diff=0) within needle range, non-zero where mismatch.
  llvm::Value* anded = b_.CreateAnd(diff, maskVec, "idx.masked");
  // all_zero: true where anded == 0 (i.e., match within needle range)
  llvm::Value* matchVec = b_.CreateICmpEQ(anded, zeroVec, "idx.cmp_eq");
  // Reduce <16 x i1> to i16, then check if the first needleLen bits are set.
  llvm::Value* bits = b_.CreateBitCast(matchVec, llvm::Type::getIntNTy(ctx_, 16), "idx.bits");
  // Create a mask with the first needleLen bits set (e.g., needleLen=8 → 0xFF)
  uint32_t matchMaskVal = (1u << needleLen) - 1;
  llvm::Value* matchMask =
      llvm::ConstantInt::get(b_.getInt16Ty(), matchMaskVal);
  llvm::Value* maskedBits = b_.CreateAnd(bits, matchMask, "idx.masked_bits");
  llvm::Value* allMatch = b_.CreateICmpEQ(maskedBits, matchMask, "idx.all_match");

  // Branch: match → matchBB, miss → retry search from pos+1
  llvm::Value* posPlus1 = b_.CreateAdd(pos, one64, "idx.next_start");
  b_.CreateCondBr(allMatch, matchBB, searchBB);
  start->addIncoming(posPlus1, verifyBB);

  // match: result = pos + 1 (PL/I is 1-based)
  startBlock(matchBB);
  llvm::Value* matchResult = b_.CreateAdd(pos, one64, "idx.result");
  b_.CreateBr(exitBB);

  startBlock(ret0BB);
  b_.CreateBr(exitBB);

  // exit: merge all result values.
  startBlock(exitBB);
  llvm::PHINode* resultPhi = b_.CreatePHI(b_.getInt64Ty(), 4, "idx.out");
  resultPhi->addIncoming(one64, ret1BB);
  resultPhi->addIncoming(zero, ret0BB);
  resultPhi->addIncoming(matchResult, matchBB);

  return resultPhi;
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
  // The result is a fixed string, so the field past the copied text is blank:
  // the rule the other string built-ins use (ADR-273). Without it the tail of
  // the temporary is uninitialized and the two lowerings disagree.
  llvm::Value* copyLen =
      b_.CreateSelect(b_.CreateICmpULE(sLen64, dstcap64), sLen64, dstcap64, "tr.copy_len");
  b_.CreateMemSet(b_.CreateGEP(b_.getInt8Ty(), dst, {copyLen}, "tr.padptr"), blank,
                  b_.CreateSub(dstcap64, copyLen, "tr.padlen"), llvm::MaybeAlign(), false);
}

// translate(dst, dstcap, s, slen, table): as emitTranslateLLVM, but `table` is
// the compile-time translation table of a constant (out, in) pair, so the
// scan of `in` becomes one lookup per character (ADR-273). Mirrors MULTICS's
// inline translate sequence (compile_exp.pl1:1392-1440).
void IRGen::emitTranslateTable(llvm::Value* dst, llvm::Value* dstcap, llvm::Value* s,
                               llvm::Value* sLen, llvm::Value* table) {
  llvm::Value* one = llvm::ConstantInt::get(b_.getInt64Ty(), 1);
  llvm::Value* zero = llvm::ConstantInt::get(b_.getInt64Ty(), 0);

  llvm::Value* sLen64 = b_.CreateZExtOrTrunc(sLen, b_.getInt64Ty(), "tr.slen");
  llvm::Value* dstcap64 = b_.CreateZExtOrTrunc(dstcap, b_.getInt64Ty(), "tr.dstcap");
  // The copy stops at the shorter of the source and the destination, the
  // bound both the scan and the runtime call use.
  llvm::Value* n = b_.CreateSelect(b_.CreateICmpULE(sLen64, dstcap64), sLen64, dstcap64, "tr.n");

  llvm::Function* F = b_.GetInsertBlock()->getParent();
  llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(ctx_, "tr.loop", F);
  llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(ctx_, "tr.done", F);

  b_.CreateCondBr(b_.CreateICmpULE(n, zero, "tr.empty"), doneBB, loopBB);

  llvm::BasicBlock* preLoopBB = b_.GetInsertBlock();
  startBlock(loopBB);
  llvm::PHINode* iPhi = b_.CreatePHI(b_.getInt64Ty(), 2, "tr.i");
  iPhi->addIncoming(zero, preLoopBB);
  llvm::Value* si =
      b_.CreateLoad(b_.getInt8Ty(), b_.CreateGEP(b_.getInt8Ty(), s, {iPhi}, "tr.siptr"), "tr.si");
  // The character code is the byte value (0..255), which indexes the table.
  llvm::Value* code = b_.CreateZExt(si, b_.getInt32Ty(), "tr.code");
  llvm::Value* mapped = b_.CreateLoad(
      b_.getInt8Ty(), b_.CreateGEP(b_.getInt8Ty(), table, {code}, "tr.tbl_ptr"), "tr.mapped");
  b_.CreateStore(mapped, b_.CreateGEP(b_.getInt8Ty(), dst, {iPhi}, "tr.dstptr"), false);
  // One index per character; the back edge feeds the phi from this block.
  llvm::Value* iNext = b_.CreateAdd(iPhi, one, "tr.i_next");
  iPhi->addIncoming(iNext, loopBB);
  b_.CreateCondBr(b_.CreateICmpULT(iNext, n, "tr.more"), loopBB, doneBB);

  startBlock(doneBB);
  // The field past the copied text is blank, as in the scan lowering above.
  llvm::Value* blank = llvm::ConstantInt::get(b_.getInt8Ty(), ' ');
  b_.CreateMemSet(b_.CreateGEP(b_.getInt8Ty(), dst, {n}, "tr.padptr"), blank,
                  b_.CreateSub(dstcap64, n, "tr.padlen"), llvm::MaybeAlign(), false);
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

// Direct LLVM lowering for FIXED(char) fractional text (W4): mirrors
// rt_stream.c pli_fixed_char_parts. Accumulates up to 38 significant digits
// in i256 and up to 76 fractional digits; a second decimal point or trailing
// text ends the scan. *ok=0 on no digits or digit overflow.
llvm::Value* IRGen::emitFixedCharPartsLLVM(llvm::Value* s, llvm::Value* slen,
                                           llvm::Value* okSlot, llvm::Value* fracSlot) {
  llvm::Function* F = b_.GetInsertBlock()->getParent();
  llvm::Type* i256Ty = b_.getIntNTy(256);
  llvm::Value* zero64 = llvm::ConstantInt::get(b_.getInt64Ty(), 0);
  llvm::Value* one64 = llvm::ConstantInt::get(b_.getInt64Ty(), 1);
  llvm::Value* zero32 = llvm::ConstantInt::get(b_.getInt32Ty(), 0);
  llvm::Value* one32 = llvm::ConstantInt::get(b_.getInt32Ty(), 1);
  llvm::Value* maxDigits = i32(38);
  llvm::Value* maxFrac = i32(76);
  llvm::Value* zero256 = llvm::ConstantInt::get(i256Ty, 0);
  llvm::Value* ten256 = llvm::ConstantInt::get(i256Ty, 10);
  llvm::Value* space = llvm::ConstantInt::get(b_.getInt8Ty(), ' ');
  llvm::Value* tab = llvm::ConstantInt::get(b_.getInt8Ty(), '\t');
  llvm::Value* plus = llvm::ConstantInt::get(b_.getInt8Ty(), '+');
  llvm::Value* minus = llvm::ConstantInt::get(b_.getInt8Ty(), '-');
  llvm::Value* dotCh = llvm::ConstantInt::get(b_.getInt8Ty(), '.');
  llvm::Value* zeroCh = llvm::ConstantInt::get(b_.getInt8Ty(), '0');
  llvm::Value* nineCh = llvm::ConstantInt::get(b_.getInt8Ty(), '9');
  llvm::Value* slen64 = b_.CreateZExtOrTrunc(slen, b_.getInt64Ty(), "fxc.slen");

  llvm::Value* idxSlot = entryAlloca(b_.getInt64Ty(), "fxc.idx");
  llvm::Value* allSlot = entryAlloca(i256Ty, "fxc.all");
  llvm::Value* digitsSlot = entryAlloca(b_.getInt32Ty(), "fxc.digits");
  llvm::Value* negSlot = entryAlloca(b_.getInt1Ty(), "fxc.neg");
  llvm::Value* sawSlot = entryAlloca(b_.getInt1Ty(), "fxc.saw");
  llvm::Value* seenSlot = entryAlloca(b_.getInt1Ty(), "fxc.seen");
  llvm::Value* dotSlot = entryAlloca(b_.getInt1Ty(), "fxc.dot");
  llvm::Value* overflowSlot = entryAlloca(b_.getInt1Ty(), "fxc.overflow");
  b_.CreateStore(zero64, idxSlot, false);
  b_.CreateStore(zero256, allSlot, false);
  b_.CreateStore(zero32, digitsSlot, false);
  b_.CreateStore(zero32, fracSlot, false);
  b_.CreateStore(b_.getFalse(), negSlot, false);
  b_.CreateStore(b_.getFalse(), sawSlot, false);
  b_.CreateStore(b_.getFalse(), seenSlot, false);
  b_.CreateStore(b_.getFalse(), dotSlot, false);
  b_.CreateStore(b_.getFalse(), overflowSlot, false);

  llvm::BasicBlock* skipBB = llvm::BasicBlock::Create(ctx_, "fxc.skip", F);
  llvm::BasicBlock* skipBodyBB = llvm::BasicBlock::Create(ctx_, "fxc.skip_body", F);
  llvm::BasicBlock* signBB = llvm::BasicBlock::Create(ctx_, "fxc.sign", F);
  llvm::BasicBlock* intBB = llvm::BasicBlock::Create(ctx_, "fxc.int", F);
  llvm::BasicBlock* intBodyBB = llvm::BasicBlock::Create(ctx_, "fxc.int_body", F);
  llvm::BasicBlock* dotBB = llvm::BasicBlock::Create(ctx_, "fxc.dot", F);
  llvm::BasicBlock* dotBodyBB = llvm::BasicBlock::Create(ctx_, "fxc.dot_body", F);
  llvm::BasicBlock* fracBB = llvm::BasicBlock::Create(ctx_, "fxc.frac", F);
  llvm::BasicBlock* fracBodyBB = llvm::BasicBlock::Create(ctx_, "fxc.frac_body", F);
  llvm::BasicBlock* checkBB = llvm::BasicBlock::Create(ctx_, "fxc.check_exp", F);
  llvm::BasicBlock* expBB = llvm::BasicBlock::Create(ctx_, "fxc.exp", F);
  llvm::BasicBlock* finishBB = llvm::BasicBlock::Create(ctx_, "fxc.finish", F);
  b_.CreateBr(skipBB);

  // leading blanks/tabs
  startBlock(skipBB);
  {
    llvm::Value* i = b_.CreateLoad(b_.getInt64Ty(), idxSlot, "fxc.i");
    llvm::Value* inRange = b_.CreateICmpULT(i, slen64, "fxc.skip_cond");
    llvm::Value* c = b_.CreateLoad(b_.getInt8Ty(),
                                   b_.CreateGEP(b_.getInt8Ty(), s, {i}, "fxc.skip_ptr"), "fxc.c");
    llvm::Value* blank = b_.CreateOr(b_.CreateICmpEQ(c, space, "fxc.isspace"),
                                     b_.CreateICmpEQ(c, tab, "fxc.istab"), "fxc.isblank");
    b_.CreateCondBr(b_.CreateAnd(inRange, blank, "fxc.skip_cont"), skipBodyBB, signBB);
  }
  startBlock(skipBodyBB);
  {
    llvm::Value* i = b_.CreateLoad(b_.getInt64Ty(), idxSlot, "fxc.i_inc");
    b_.CreateStore(b_.CreateAdd(i, one64, "fxc.i_next"), idxSlot, false);
    b_.CreateBr(skipBB);
  }

  // optional sign
  startBlock(signBB);
  {
    llvm::Value* i = b_.CreateLoad(b_.getInt64Ty(), idxSlot, "fxc.sign_i");
    llvm::Value* inRange = b_.CreateICmpULT(i, slen64, "fxc.idx_lt_len");
    llvm::Value* c = b_.CreateLoad(b_.getInt8Ty(),
                                   b_.CreateGEP(b_.getInt8Ty(), s, {i}, "fxc.sign_ptr"), "fxc.sc");
    llvm::Value* isPlus = b_.CreateICmpEQ(c, plus, "fxc.is_plus");
    llvm::Value* isMinus = b_.CreateICmpEQ(c, minus, "fxc.is_minus");
    llvm::Value* hasSign =
        b_.CreateAnd(inRange, b_.CreateOr(isPlus, isMinus, "fxc.is_sign"), "fxc.has_sign");
    llvm::Value* iNext = b_.CreateAdd(i, one64, "fxc.sign_next");
    b_.CreateStore(b_.CreateSelect(hasSign, iNext, i, "fxc.idx_after_sign"), idxSlot, false);
    b_.CreateStore(isMinus, negSlot, false);
  }
  b_.CreateBr(intBB);

  // Shared digit body: leading zeros do not count as significant digits;
  // fractional digits increment *frac even when they are leading zeros.
  auto emitDigitBody = [&](bool fractional) {
    llvm::Value* i = b_.CreateLoad(b_.getInt64Ty(), idxSlot, "fxc.d_i");
    llvm::Value* c = b_.CreateLoad(
        b_.getInt8Ty(), b_.CreateGEP(b_.getInt8Ty(), s, {i}, "fxc.d_ptr"), "fxc.d_c");
    llvm::Value* dig8 = b_.CreateSub(c, zeroCh, "fxc.d_sub");
    llvm::Value* dig32 = b_.CreateZExt(dig8, b_.getInt32Ty(), "fxc.dig32");
    llvm::Value* dig256 = b_.CreateZExt(dig8, i256Ty, "fxc.dig256");
    llvm::Value* seen = b_.CreateLoad(b_.getInt1Ty(), seenSlot, "fxc.seen");
    llvm::Value* sig = b_.CreateOr(seen, b_.CreateICmpNE(dig32, zero32, "fxc.nonzero"),
                                   "fxc.sig");
    llvm::Value* digits = b_.CreateLoad(b_.getInt32Ty(), digitsSlot, "fxc.digits");
    llvm::Value* frac = b_.CreateLoad(b_.getInt32Ty(), fracSlot, "fxc.frac");
    llvm::Value* overflow = b_.CreateLoad(b_.getInt1Ty(), overflowSlot, "fxc.ov");
    llvm::Value* capDigits = b_.CreateAnd(sig, b_.CreateICmpUGE(digits, maxDigits, "fxc.capd"),
                                          "fxc.cap_digits");
    llvm::Value* capFrac =
        fractional ? b_.CreateICmpUGE(frac, maxFrac, "fxc.capf") : b_.getFalse();
    llvm::Value* cap = b_.CreateOr(capDigits, capFrac, "fxc.cap");
    llvm::Value* canAcc = b_.CreateAnd(b_.CreateNot(overflow), b_.CreateNot(cap), "fxc.canacc");
    llvm::Value* all = b_.CreateLoad(i256Ty, allSlot, "fxc.all");
    llvm::Value* nv = b_.CreateAdd(b_.CreateMul(all, ten256, "fxc.mul10"), dig256,
                                   "fxc.adddig");
    b_.CreateStore(b_.CreateSelect(canAcc, nv, all, "fxc.all_next"), allSlot, false);
    llvm::Value* newDigits = b_.CreateAdd(digits, one32, "fxc.digits_next");
    b_.CreateStore(b_.CreateSelect(b_.CreateAnd(canAcc, sig), newDigits, digits,
                                   "fxc.digits_sel"),
                   digitsSlot, false);
    llvm::Value* newOverflow = b_.CreateOr(overflow, cap, "fxc.ov_next");
    if (fractional) {
      llvm::Value* newFrac = b_.CreateAdd(frac, one32, "fxc.frac_next");
      llvm::Value* fInc = b_.CreateNot(newOverflow, "fxc.frac_inc");
      b_.CreateStore(b_.CreateSelect(fInc, newFrac, frac, "fxc.frac_sel"), fracSlot, false);
    }
    b_.CreateStore(b_.CreateOr(seen, sig, "fxc.seen_next"), seenSlot, false);
    b_.CreateStore(newOverflow, overflowSlot, false);
    b_.CreateStore(b_.getTrue(), sawSlot, false);
    b_.CreateStore(b_.CreateAdd(i, one64, "fxc.d_next"), idxSlot, false);
  };

  // integer digits
  startBlock(intBB);
  {
    llvm::Value* i = b_.CreateLoad(b_.getInt64Ty(), idxSlot, "fxc.ib_i");
    llvm::Value* inRange = b_.CreateICmpULT(i, slen64, "fxc.ib_cond");
    llvm::Value* c = b_.CreateLoad(
        b_.getInt8Ty(), b_.CreateGEP(b_.getInt8Ty(), s, {i}, "fxc.ib_ptr"), "fxc.ib_c");
    llvm::Value* isDigit = b_.CreateAnd(b_.CreateICmpUGE(c, zeroCh, "fxc.ib_ge0"),
                                        b_.CreateICmpULE(c, nineCh, "fxc.ib_le9"), "fxc.is_digit");
    b_.CreateCondBr(b_.CreateAnd(inRange, isDigit, "fxc.ib_cont"), intBodyBB, dotBB);
  }
  startBlock(intBodyBB);
  {
    emitDigitBody(false);
    b_.CreateBr(intBB);
  }

  // one optional decimal point
  startBlock(dotBB);
  {
    llvm::Value* i = b_.CreateLoad(b_.getInt64Ty(), idxSlot, "fxc.dot_i");
    llvm::Value* inRange = b_.CreateICmpULT(i, slen64, "fxc.dot_cond");
    llvm::Value* c = b_.CreateLoad(b_.getInt8Ty(),
                                   b_.CreateGEP(b_.getInt8Ty(), s, {i}, "fxc.dot_ptr"), "fxc.dot_c");
    llvm::Value* isDot = b_.CreateICmpEQ(c, dotCh, "fxc.is_dot");
    llvm::Value* hadDot = b_.CreateLoad(b_.getInt1Ty(), dotSlot, "fxc.had_dot");
    llvm::Value* take = b_.CreateAnd(
        inRange, b_.CreateAnd(isDot, b_.CreateNot(hadDot), "fxc.dot_new"), "fxc.take_dot");
    b_.CreateCondBr(take, dotBodyBB, checkBB);
  }
  startBlock(dotBodyBB);
  {
    llvm::Value* i = b_.CreateLoad(b_.getInt64Ty(), idxSlot, "fxc.dot_body_i");
    b_.CreateStore(b_.getTrue(), dotSlot, false);
    b_.CreateStore(b_.CreateAdd(i, one64, "fxc.dot_next"), idxSlot, false);
    b_.CreateBr(fracBB);
  }

  // fractional digits
  startBlock(fracBB);
  {
    llvm::Value* i = b_.CreateLoad(b_.getInt64Ty(), idxSlot, "fxc.fb_i");
    llvm::Value* inRange = b_.CreateICmpULT(i, slen64, "fxc.fb_cond");
    llvm::Value* c = b_.CreateLoad(
        b_.getInt8Ty(), b_.CreateGEP(b_.getInt8Ty(), s, {i}, "fxc.fb_ptr"), "fxc.fb_c");
    llvm::Value* isDigit = b_.CreateAnd(b_.CreateICmpUGE(c, zeroCh, "fxc.fb_ge0"),
                                        b_.CreateICmpULE(c, nineCh, "fxc.fb_le9"), "fxc.fb_digit");
    b_.CreateCondBr(b_.CreateAnd(inRange, isDigit, "fxc.fb_cont"), fracBodyBB, checkBB);
  }
  startBlock(fracBodyBB);
  {
    emitDigitBody(true);
    b_.CreateBr(fracBB);
  }

  // Reject exponent text (1e3) rather than returning the integer prefix.
  startBlock(checkBB);
  {
    llvm::Value* i = b_.CreateLoad(b_.getInt64Ty(), idxSlot, "fxc.ck_i");
    llvm::Value* inRange = b_.CreateICmpULT(i, slen64, "fxc.ck_range");
    b_.CreateCondBr(inRange, expBB, finishBB);
  }
  startBlock(expBB);
  {
    llvm::Value* i = b_.CreateLoad(b_.getInt64Ty(), idxSlot, "fxc.exp_i");
    llvm::Value* c = b_.CreateLoad(b_.getInt8Ty(),
                                   b_.CreateGEP(b_.getInt8Ty(), s, {i}, "fxc.exp_ptr"), "fxc.exp_c");
    llvm::Value* isExp = b_.CreateOr(b_.CreateICmpEQ(c, llvm::ConstantInt::get(b_.getInt8Ty(), 'e'),
                                                     "fxc.is_e"),
                                     b_.CreateICmpEQ(c, llvm::ConstantInt::get(b_.getInt8Ty(), 'E'),
                                                     "fxc.is_E"),
                                     "fxc.is_exp");
    llvm::Value* ov = b_.CreateLoad(b_.getInt1Ty(), overflowSlot, "fxc.exp_ov");
    b_.CreateStore(b_.CreateOr(ov, isExp, "fxc.exp_bad"), overflowSlot, false);
    b_.CreateBr(finishBB);
  }

  startBlock(finishBB);
  {
    llvm::Value* saw = b_.CreateLoad(b_.getInt1Ty(), sawSlot, "fxc.saw");
    llvm::Value* overflow = b_.CreateLoad(b_.getInt1Ty(), overflowSlot, "fxc.ov_final");
    llvm::Value* okVal = b_.CreateZExt(
        b_.CreateAnd(saw, b_.CreateNot(overflow), "fxc.ok"), b_.getInt32Ty(), "fxc.ok_val");
    b_.CreateStore(okVal, okSlot, false);
    llvm::Value* all = b_.CreateLoad(i256Ty, allSlot, "fxc.all_load");
    llvm::Value* isNeg = b_.CreateLoad(b_.getInt1Ty(), negSlot, "fxc.neg_load");
    llvm::Value* negVal = b_.CreateSub(zero256, all, "fxc.neg_val");
    return b_.CreateSelect(isNeg, negVal, all, "fxc.result");
  }
}

// Shared character parse (rule (123)): parse the signed significant digits and
// fraction count, raising CONVERSION (rule 94) on unparseable text. The resume
// value is the parsed prefix.
llvm::Value* IRGen::emitCharParts(llvm::Value* s, llvm::Value* slen,
                                  llvm::Value* fracSlot) {
  llvm::Value* okSlot = entryAlloca(b_.getInt32Ty(), "fxc.ok");
  llvm::Value* all;
  if (useRuntimeCall("fixed_of_char")) {
    // The runtime returns the signed digits as (lo, hi) i64 halves because the
    // ABI does not pass i128 by value.
    llvm::Type* i128Ty = b_.getInt128Ty();
    llvm::Type* i256Ty = b_.getIntNTy(256);
    llvm::Type* pairTy = llvm::StructType::get(ctx_, {b_.getInt64Ty(), b_.getInt64Ty()});
    llvm::Value* outSlot = entryAlloca(pairTy, "fxc.parts");
    b_.CreateCall(runtimeFn("pli_fixed_char_parts"), {s, slen, okSlot, fracSlot, outSlot});
    llvm::Value* lo = b_.CreateLoad(
        b_.getInt64Ty(), b_.CreateStructGEP(pairTy, outSlot, 0, "fxc.lo.ptr"), "fxc.lo");
    llvm::Value* hi = b_.CreateLoad(
        b_.getInt64Ty(), b_.CreateStructGEP(pairTy, outSlot, 1, "fxc.hi.ptr"), "fxc.hi");
    llvm::Value* shift = llvm::ConstantInt::get(i128Ty, 64);
    llvm::Value* all128 = b_.CreateOr(
        b_.CreateZExt(lo, i128Ty, "fxc.lo.z"),
        b_.CreateShl(b_.CreateZExt(hi, i128Ty, "fxc.hi.z"), shift, "fxc.hi.shift"), "fxc.all128");
    all = b_.CreateSExt(all128, i256Ty, "fxc.all");
  } else {
    all = emitFixedCharPartsLLVM(s, slen, okSlot, fracSlot);
  }

  // CONVERSION (rule 94): no digits (or digit overflow) traps.
  if (convChecks()) {
    llvm::Value* ok = b_.CreateLoad(b_.getInt32Ty(), okSlot, "conv_ok");
    llvm::Value* fail = b_.CreateICmpEQ(ok, i32(0), "conv_fail");
    llvm::BasicBlock* trapBB =
        llvm::BasicBlock::Create(ctx_, "conv.trap." + std::to_string(n_++), curFn_);
    llvm::BasicBlock* okBB =
        llvm::BasicBlock::Create(ctx_, "conv.ok." + std::to_string(n_++), curFn_);
    b_.CreateCondBr(fail, trapBB, okBB);
    b_.SetInsertPoint(trapBB);
    emitCondTrap(Stmt::kConversionCondKey, "pli_conversion", okBB);
    b_.SetInsertPoint(okBB);
  }
  return all;
}

// Character text → fixed target (rule (123)): parse the signed significant
// digits and fraction count, then rescale in i256. DECIMAL targets round half
// away from zero; BINARY targets truncate toward zero. The parsed magnitude is
// limited to 38 significant digits.
Val IRGen::emitFixedFromChar(llvm::Value* s, llvm::Value* slen, const Type& dst) {
  llvm::Type* i256Ty = b_.getIntNTy(256);
  llvm::Value* zero256 = llvm::ConstantInt::get(i256Ty, 0);
  llvm::Value* one256 = llvm::ConstantInt::get(i256Ty, 1);
  llvm::Value* ten256 = llvm::ConstantInt::get(i256Ty, 10);
  llvm::Value* zero64 = i64(0);
  llvm::Value* one64 = i64(1);

  llvm::Value* fracSlot = entryAlloca(b_.getInt32Ty(), "fxc.frac");
  llvm::Value* all = emitCharParts(s, slen, fracSlot);

  // Signed digits → magnitude plus sign.
  llvm::Value* isNeg = b_.CreateICmpSLT(all, zero256, "fxc.neg");
  llvm::Value* mag =
      b_.CreateSelect(isNeg, b_.CreateSub(zero256, all, "fxc.negmag"), all, "fxc.mag");

  // 10^frac, built by a short loop (frac <= 76, so it stays inside i256).
  llvm::Value* powSlot = entryAlloca(i256Ty, "fxc.pow");
  llvm::Value* kSlot = entryAlloca(b_.getInt64Ty(), "fxc.k");
  b_.CreateStore(one256, powSlot, false);
  b_.CreateStore(zero64, kSlot, false);
  llvm::Value* frac32 = b_.CreateLoad(b_.getInt32Ty(), fracSlot, "fxc.frac.load");
  llvm::Value* frac64 = b_.CreateZExt(frac32, b_.getInt64Ty(), "fxc.frac64");
  llvm::BasicBlock* powBB = llvm::BasicBlock::Create(ctx_, "fxc.pow", curFn_);
  llvm::BasicBlock* powBodyBB = llvm::BasicBlock::Create(ctx_, "fxc.pow_body", curFn_);
  llvm::BasicBlock* powDoneBB = llvm::BasicBlock::Create(ctx_, "fxc.pow_done", curFn_);
  b_.CreateBr(powBB);
  startBlock(powBB);
  {
    llvm::Value* k = b_.CreateLoad(b_.getInt64Ty(), kSlot, "fxc.k.load");
    b_.CreateCondBr(b_.CreateICmpSLT(k, frac64, "fxc.k_lt"), powBodyBB, powDoneBB);
  }
  startBlock(powBodyBB);
  {
    llvm::Value* p = b_.CreateLoad(i256Ty, powSlot, "fxc.pow.load");
    llvm::Value* k = b_.CreateLoad(b_.getInt64Ty(), kSlot, "fxc.k.inc");
    b_.CreateStore(b_.CreateMul(p, ten256, "fxc.pow.next"), powSlot, false);
    b_.CreateStore(b_.CreateAdd(k, one64, "fxc.k.next"), kSlot, false);
    b_.CreateBr(powBB);
  }
  startBlock(powDoneBB);
  llvm::Value* pow10 = b_.CreateLoad(i256Ty, powSlot, "fxc.pow10");

  llvm::Value* raw;
  if (dst.k == TK::FixedDec) {
    // Decimal target: all * 10^q / 10^f, rounded half away from zero.
    llvm::Value* scaled = b_.CreateMul(mag, intPowConst(i256Ty, 10, dst.scale), "fxc.dec.mul");
    llvm::Value* q = b_.CreateSDiv(scaled, pow10, "fxc.dec.q");
    llvm::Value* r = b_.CreateSRem(scaled, pow10, "fxc.dec.r");
    llvm::Value* twice = b_.CreateMul(r, llvm::ConstantInt::get(i256Ty, 2), "fxc.dec.2r");
    llvm::Value* roundUp = b_.CreateICmpUGE(twice, pow10, "fxc.dec.round");
    q = b_.CreateAdd(q, b_.CreateZExt(roundUp, i256Ty, "fxc.dec.inc"), "fxc.dec.rounded");
    raw = b_.CreateSelect(isNeg, b_.CreateSub(zero256, q, "fxc.dec.neg"), q, "fxc.dec.raw");
    wideMagTrap(raw, dst.prec);
  } else {
    // Binary target: all * 2^q / 10^f, truncated toward zero.
    llvm::Value* scaled = b_.CreateMul(mag, intPowConst(i256Ty, 2, dst.scale), "fxc.bin.mul");
    llvm::Value* q = b_.CreateSDiv(scaled, pow10, "fxc.bin.q");
    raw = b_.CreateSelect(isNeg, b_.CreateSub(zero256, q, "fxc.bin.neg"), q, "fxc.bin.raw");
    raw = fixedBinPrecTrap(raw, dst.prec);
  }
  Val v;
  v.ty = dst;
  v.reg = b_.CreateTrunc(raw, llvmTy(dst), "fxc.trunc");
  return v;
}

// Character text → FLOAT (rule (123)): parse the signed significant digits and
// fraction count, then divide the digits by 10^frac in double. Each conversion
// rounds once, so the result is within a few ulps of a correctly rounded parse.
Val IRGen::emitFloatFromChar(llvm::Value* s, llvm::Value* slen, const Type& dst) {
  llvm::Type* i256Ty = b_.getIntNTy(256);
  llvm::Value* zero64 = i64(0);
  llvm::Value* one64 = i64(1);
  llvm::Value* one256 = llvm::ConstantInt::get(i256Ty, 1);
  llvm::Value* ten256 = llvm::ConstantInt::get(i256Ty, 10);

  llvm::Value* fracSlot = entryAlloca(b_.getInt32Ty(), "fxc.frac");
  llvm::Value* all = emitCharParts(s, slen, fracSlot);

  // 10^frac, built by a short loop (frac <= 76, so it stays inside i256).
  llvm::Value* powSlot = entryAlloca(i256Ty, "fxc.fpow");
  llvm::Value* kSlot = entryAlloca(b_.getInt64Ty(), "fxc.fk");
  b_.CreateStore(one256, powSlot, false);
  b_.CreateStore(zero64, kSlot, false);
  llvm::Value* frac32 = b_.CreateLoad(b_.getInt32Ty(), fracSlot, "fxc.ffrac.load");
  llvm::Value* frac64 = b_.CreateZExt(frac32, b_.getInt64Ty(), "fxc.ffrac64");
  llvm::BasicBlock* powBB = llvm::BasicBlock::Create(ctx_, "fxc.fpow", curFn_);
  llvm::BasicBlock* powBodyBB = llvm::BasicBlock::Create(ctx_, "fxc.fpow_body", curFn_);
  llvm::BasicBlock* powDoneBB = llvm::BasicBlock::Create(ctx_, "fxc.fpow_done", curFn_);
  b_.CreateBr(powBB);
  startBlock(powBB);
  {
    llvm::Value* k = b_.CreateLoad(b_.getInt64Ty(), kSlot, "fxc.fk.load");
    b_.CreateCondBr(b_.CreateICmpSLT(k, frac64, "fxc.fk_lt"), powBodyBB, powDoneBB);
  }
  startBlock(powBodyBB);
  {
    llvm::Value* p = b_.CreateLoad(i256Ty, powSlot, "fxc.fpow.load");
    llvm::Value* k = b_.CreateLoad(b_.getInt64Ty(), kSlot, "fxc.fk.inc");
    b_.CreateStore(b_.CreateMul(p, ten256, "fxc.fpow.next"), powSlot, false);
    b_.CreateStore(b_.CreateAdd(k, one64, "fxc.fk.next"), kSlot, false);
    b_.CreateBr(powBB);
  }
  startBlock(powDoneBB);
  llvm::Value* pow10 = b_.CreateLoad(i256Ty, powSlot, "fxc.fpow10");

  llvm::Value* digits = b_.CreateSIToFP(all, b_.getDoubleTy(), "fxc.fdigits");
  llvm::Value* scale = b_.CreateSIToFP(pow10, b_.getDoubleTy(), "fxc.fscale");
  Val v;
  v.ty = dst;
  v.reg = b_.CreateFDiv(digits, scale, "fxc.fvalue");
  return v;
}

// Direct LLVM lowering for CHAR(fixed) (W4): format an i64 as a decimal
// string into dst, blank-padded to dstcap. Mirrors rt_stream.c pli_char_of_fixed.
void IRGen::emitCharOfFixedLLVM(llvm::Value* dst, llvm::Value* dstcap, llvm::Value* v) {
  llvm::Function* F = b_.GetInsertBlock()->getParent();
  llvm::Value* zero = llvm::ConstantInt::get(b_.getInt64Ty(), 0);
  llvm::Value* one = llvm::ConstantInt::get(b_.getInt64Ty(), 1);
  llvm::Value* ten = llvm::ConstantInt::get(b_.getInt64Ty(), 10);
  llvm::Value* space = llvm::ConstantInt::get(b_.getInt8Ty(), ' ');
  llvm::Value* minus = llvm::ConstantInt::get(b_.getInt8Ty(), '-');
  llvm::Value* zeroCh = llvm::ConstantInt::get(b_.getInt8Ty(), '0');
  llvm::Value* dbufCap = llvm::ConstantInt::get(b_.getInt64Ty(), 24);
  llvm::Value* vc = b_.CreateZExtOrTrunc(v, b_.getInt64Ty(), "cof.v");
  llvm::Value* cap = b_.CreateZExtOrTrunc(dstcap, b_.getInt64Ty(), "cof.cap");

  // Stack buffer for reversed digits (max 20 digits + sign for i64).
  llvm::Value* dbuf = entryAlloca(llvm::ArrayType::get(b_.getInt8Ty(), 24), "cof.dbuf");
  llvm::Value* cntSlot = entryAlloca(b_.getInt64Ty(), "cof.cnt");
  llvm::Value* valSlot = entryAlloca(b_.getInt64Ty(), "cof.val");
  llvm::Value* negSlot = entryAlloca(b_.getInt1Ty(), "cof.neg");

  // Determine sign; work with unsigned absolute value via wrapping negation
  // so that INT64_MIN (abs = 2^63) is handled correctly by unsigned div/rem.
  llvm::Value* isNeg = b_.CreateICmpSLT(vc, zero, "cof.is_neg");
  llvm::Value* negVal = b_.CreateSub(zero, vc, "cof.neg_val");
  llvm::Value* av = b_.CreateSelect(isNeg, negVal, vc, "cof.av");
  b_.CreateStore(isNeg, negSlot, false);
  b_.CreateStore(av, valSlot, false);
  b_.CreateStore(zero, cntSlot, false);

  // Pre-seed dbuf[23] = '0' — first extraction iteration (if any) overwrites it
  // with the units digit; if v == 0 the loop never runs and this '0' remains.
  b_.CreateStore(zeroCh, b_.CreateGEP(b_.getInt8Ty(), dbuf,
                                      {b_.CreateSub(dbufCap, one, "cof.seed")}, "cof.seed_ptr"));

  llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(ctx_, "cof.loop", F);
  llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(ctx_, "cof.body", F);
  llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(ctx_, "cof.exit", F);
  llvm::BasicBlock* afterBB = llvm::BasicBlock::Create(ctx_, "cof.after", F);
  b_.CreateBr(loopBB);

  // Loop header: load val, check > 0.
  startBlock(loopBB);
  {
    llvm::Value* val = b_.CreateLoad(b_.getInt64Ty(), valSlot, "cof.val_l");
    llvm::Value* cond = b_.CreateICmpUGT(val, zero, "cof.val_gt0");
    b_.CreateCondBr(cond, bodyBB, exitBB);
  }

  // Loop body: extract lowest digit (unsigned rem), store at dbuf[cap-1-cnt],
  // then unsigned-divide.
  startBlock(bodyBB);
  {
    llvm::Value* val = b_.CreateLoad(b_.getInt64Ty(), valSlot, "cof.val_b");
    llvm::Value* cnt = b_.CreateLoad(b_.getInt64Ty(), cntSlot, "cof.cnt_b");
    llvm::Value* mod = b_.CreateURem(val, ten, "cof.urmod");
    llvm::Value* digit =
        b_.CreateAdd(b_.CreateTrunc(mod, b_.getInt8Ty(), "cof.mod8"), zeroCh, "cof.digit");
    llvm::Value* pos = b_.CreateSub(dbufCap, b_.CreateAdd(cnt, one, "cof.next_cnt"), "cof.pos");
    b_.CreateStore(digit, b_.CreateGEP(b_.getInt8Ty(), dbuf, {pos}, "cof.dptr"));
    b_.CreateStore(b_.CreateAdd(cnt, one, "cof.cnt_inc"), cntSlot, false);
    b_.CreateStore(b_.CreateUDiv(val, ten, "cof.udiv"), valSlot, false);
    b_.CreateBr(loopBB);
  }

  // After extraction: cnt = digit count, digits at dbuf[24-cnt .. 23].
  // For v == 0, cnt = 0; the afterBB block ensures at least one '0' digit.
  startBlock(exitBB);
  {
    b_.CreateBr(afterBB);
  }

  // Write sign + digits to dst, then blank-pad.
  startBlock(afterBB);
  {
    llvm::Value* cnt = b_.CreateLoad(b_.getInt64Ty(), cntSlot, "cof.cnt_a");
    // Zero case: cnt = 0 → use 1 (dbuf[23] was pre-seeded with '0').
    llvm::Value* cntClamped =
        b_.CreateSelect(b_.CreateICmpEQ(cnt, zero, "cof.cnt0"), one, cnt, "cof.cnt_clamped");
    llvm::Value* neg = b_.CreateLoad(b_.getInt1Ty(), negSlot, "cof.neg_a");
    // If negative, write '-' at dst[0]; digits start at dst[1] (or dst[0]).
    llvm::Value* dstOff = b_.CreateSelect(neg, one, zero, "cof.dst_off");
    // Source: dbuf[24 - cntClamped]
    llvm::Value* srcStart = b_.CreateSub(dbufCap, cntClamped, "cof.src");
    llvm::Value* srcPtr = b_.CreateGEP(b_.getInt8Ty(), dbuf, {srcStart}, "cof.srcp");
    // Write sign char at dst[0] if negative.
    b_.CreateStore(minus, b_.CreateGEP(b_.getInt8Ty(), dst, {zero}, "cof.sp_ptr"),
                   /*volatile=*/false);
    // Copy digit chars to dst[dstOff..].
    b_.CreateMemCpy(b_.CreateGEP(b_.getInt8Ty(), dst, {dstOff}, "cof.dst_start"),
                    llvm::MaybeAlign(), srcPtr, llvm::MaybeAlign(), cntClamped);
    // Blank-pad: dst[dstOff + cnt .. cap) with spaces.
    llvm::Value* writeEnd = b_.CreateAdd(dstOff, cntClamped, "cof.wend");
    llvm::Value* padLen =
        b_.CreateSelect(b_.CreateICmpUGT(cap, writeEnd, "cof.cap_gt_wend"),
                        b_.CreateSub(cap, writeEnd, "cof.pad_len"), zero, "cof.padlen");
    b_.CreateMemSet(b_.CreateGEP(b_.getInt8Ty(), dst, {writeEnd}, "cof.padptr"), space, padLen,
                    llvm::MaybeAlign(), false);
  }
}

// Direct LLVM lowering for CHAR(float) (W4): format a double as %.6g into dst,
// blank-padded to dstcap. Uses sprintf (standard C library) to eliminate the
// opaque pli_char_of_float call barrier; system sprintf is LLVM-optimizable.
void IRGen::emitCharOfFloatLLVM(llvm::Value* dst, llvm::Value* dstcap, llvm::Value* x) {
  llvm::Function* F = b_.GetInsertBlock()->getParent();
  llvm::Value* zero = llvm::ConstantInt::get(b_.getInt64Ty(), 0);
  llvm::Value* space = llvm::ConstantInt::get(b_.getInt8Ty(), ' ');
  llvm::Value* cap = b_.CreateZExtOrTrunc(dstcap, b_.getInt64Ty(), "cof.cap");

  // Temp buffer for sprintf output (max ~16 chars for %.6g, +1 for NUL).
  llvm::Value* tmp = entryAlloca(llvm::ArrayType::get(b_.getInt8Ty(), 48), "cof.tmp");
  llvm::Value* tmpPtr = b_.CreateGEP(b_.getInt8Ty(), tmp, {zero}, "cof.tmp0");
  // Format string "%.6g".
  llvm::Value* fmt = b_.CreateGlobalString("%.6g", "cof.fmt");
  // sprintf(char*, const char*, double) — variadic but only one vararg.
  llvm::FunctionType* sprintfType =
      llvm::FunctionType::get(b_.getInt32Ty(), {b_.getPtrTy(), b_.getPtrTy()}, true);
  llvm::Function* sprintfFn = llvm::cast<llvm::Function>(
      F->getParent()->getOrInsertFunction("sprintf", sprintfType).getCallee());
  llvm::Value* n = b_.CreateCall(sprintfFn, {tmpPtr, fmt, x}, "cof.n");
  // Copy min(n, cap) bytes from tmp to dst, then blank-pad.
  llvm::Value* n64 = b_.CreateZExtOrTrunc(n, b_.getInt64Ty(), "cof.n64");
  llvm::Value* copyLen =
      b_.CreateSelect(b_.CreateICmpULT(n64, cap, "cof.nltcap"), n64, cap, "cof.cplen");
  b_.CreateMemCpy(dst, llvm::MaybeAlign(), tmpPtr, llvm::MaybeAlign(), copyLen);
  llvm::Value* padLen =
      b_.CreateSelect(b_.CreateICmpUGT(cap, copyLen, "cof.cap_gt_cl"),
                      b_.CreateSub(cap, copyLen, "cof.pad_len"), zero, "cof.padlen");
  b_.CreateMemSet(b_.CreateGEP(b_.getInt8Ty(), dst, {copyLen}, "cof.padptr"), space, padLen,
                  llvm::MaybeAlign(), false);
}
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

// Wide (>18 digit) FIXED DECIMAL output (ADR-191): the runtime takes the
// 128-bit value as explicit (lo, hi) i64 halves, never as a by-value i128.
// Clang lowers a by-value __int128 parameter indirectly (by pointer) on
// x86_64-pc-windows-msvc but in registers on SysV/ARM64, so a by-value i128
// call in generated IR disagrees with the runtime body on Windows.
void IRGen::emitWideDecOut(const char* fn, llvm::Value* wideReg, long long scale) {
  llvm::Value* lo = b_.CreateTrunc(wideReg, b_.getInt64Ty(), "wdec.lo");
  llvm::Value* hi = b_.CreateTrunc(b_.CreateLShr(wideReg, (uint64_t)64, "wdec.shr"),
                                   b_.getInt64Ty(), "wdec.hi");
  deferCall(fn, {lo, hi, i64(scale)});
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
// WS-F1 (ADR-249): the ON dispatch of one condition key, as a cold function
// of its own. The dispatch is the stack scan, the handler calls and the
// abort — everything a raised condition needs, and nothing the hot path
// needs. Keeping it in its own function keeps the hot function's body small
// and gives the backend a cold function to put out of line. Mirrors MULTICS
// cg_error.pl1, where the condition path is separate from the normal path.
llvm::Function* IRGen::onTrapDispatch(int key, const std::string& abortFn, bool notifyOnly) {
  auto cached = trapDispatch_.find(key);
  if (cached != trapDispatch_.end())
    return cached->second;
  auto it = onHandlers_.find(key);
  if (it == onHandlers_.end())
    return nullptr; // no handler: the caller aborts inline
  const std::vector<llvm::Function*>& handlers = it->second;
  llvm::Function* F = llvm::Function::Create(
      llvm::FunctionType::get(b_.getVoidTy(), false), llvm::GlobalValue::InternalLinkage,
      "pli_on_trap." + (abortFn.compare(0, 4, "pli_") == 0 ? abortFn.substr(4) : abortFn),
      &mod_);
  F->addFnAttr(llvm::Attribute::Cold);
  F->addFnAttr(llvm::Attribute::NoInline);
  trapDispatch_[key] = F;

  // Build the body with the builder pointed at the dispatch function.
  llvm::BasicBlock* savedBB = b_.GetInsertBlock();
  llvm::BasicBlock* entry = llvm::BasicBlock::Create(ctx_, "entry", F);
  b_.SetInsertPoint(entry);
  // The scan: the top handler for this condition and its capture context.
  // The slot lives in this function's entry block, so a trap in a loop does
  // not grow the caller's stack (ADR-263).
  llvm::AllocaInst* ctxSlot = b_.CreateAlloca(b_.getPtrTy(), nullptr, "on.ctxslot");
  llvm::Value* top = b_.CreateCall(runtimeFn("pli_on_top_pair"), {i64((long long)key), ctxSlot},
                                    "on.top");
  llvm::Value* topCtx = b_.CreateLoad(b_.getPtrTy(), ctxSlot, "on.ctx");
  llvm::Value* none = b_.CreateICmpEQ(top, i64(0), "on.none");
  llvm::BasicBlock* abortBB = llvm::BasicBlock::Create(ctx_, "on.abort", F);
  llvm::BasicBlock* dspBB = llvm::BasicBlock::Create(ctx_, "on.dispatch", F);
  // The empty-stack edge is cold; weight it so the dispatch stays out of the
  // way of the handler path.
  llvm::MDBuilder mdb(ctx_);
  if (auto* br = b_.CreateCondBr(none, abortBB, dspBB))
    br->setMetadata(llvm::LLVMContext::MD_prof, mdb.createBranchWeights({1, 99}));
  b_.SetInsertPoint(abortBB);
  b_.CreateCall(runtimeFn(abortFn), {});
  // A notify-and-continue condition (STRINGRANGE) returns from the dispatch;
  // every other served condition aborts on the empty-handler edge.
  if (notifyOnly)
    b_.CreateRetVoid();
  else
    b_.CreateUnreachable();
  b_.SetInsertPoint(dspBB);
  llvm::BasicBlock* retBB = llvm::BasicBlock::Create(ctx_, "on.ret", F);
  llvm::SwitchInst* sw = b_.CreateSwitch(top, retBB, handlers.size());
  for (size_t i = 0; i < handlers.size(); ++i) {
    llvm::BasicBlock* hbb =
        llvm::BasicBlock::Create(ctx_, "on.handle." + std::to_string(i + 1), F);
    sw->addCase(llvm::ConstantInt::get(b_.getInt64Ty(), (long long)i + 1), hbb);
    b_.SetInsertPoint(hbb);
    b_.CreateCall(handlers[i], {topCtx});
    b_.CreateBr(retBB);
  }
  b_.SetInsertPoint(retBB);
  b_.CreateRetVoid();
  b_.SetInsertPoint(savedBB);
  return F;
}

void IRGen::emitCondTrap(int key, const std::string& abortFn, llvm::BasicBlock* okBB,
                         bool notifyOnly) {
  auto it = onHandlers_.find(key);
  if (it == onHandlers_.end()) {
    // No handler for this condition: the abort is already a call to a cold,
    // noreturn runtime entry, so the trap block is as small as it can be.
    // A notify-and-continue condition returns and resumes at okBB.
    b_.CreateCall(runtimeFn(abortFn), {});
    if (notifyOnly)
      b_.CreateBr(okBB);
    else
      b_.CreateUnreachable();
    return;
  }
  // WS-F1 (ADR-249): the dispatch is a cold function of its own; the hot
  // path carries the branch and the call, and nothing else.
  llvm::Function* dispatch = onTrapDispatch(key, abortFn, notifyOnly);
  if (!dispatch) {
    // No dispatch could be built (no handler for the key): take the
    // implicit action inline.
    b_.CreateCall(runtimeFn(abortFn), {});
    if (notifyOnly)
      b_.CreateBr(okBB);
    else
      b_.CreateUnreachable();
    return;
  }
  b_.CreateCall(dispatch, {});
  b_.CreateBr(okBB);
}

// UNDERFLOW (rule 94; ADR-326): a floating-point result whose magnitude is
// below the smallest normal double is set to zero and the condition is raised,
// taking IBM's notify-and-continue implicit action when no unit is established.
// An exact zero from addition or subtraction is not an underflow (IBM's
// significance-error rule), so `a`/`b` — the operands of a multiply, divide or
// power — are null for those and the flushed-to-zero test is skipped.
Val IRGen::applyUnderflow(Val r, llvm::Value* a, llvm::Value* b) {
  if (!underflowChecks())
    return r;
  std::string id = std::to_string(n_++);
  llvm::Value* zero = llvm::ConstantFP::get(b_.getDoubleTy(), 0.0);
  llvm::Value* min = llvm::ConstantFP::get(b_.getDoubleTy(), std::numeric_limits<double>::min());
  llvm::Function* fabsFn = intrinsicFn("llvm.fabs.f64", b_.getDoubleTy(), {b_.getDoubleTy()});
  llvm::Value* mag = b_.CreateCall(fabsFn, {r.reg}, "uf.mag." + id);
  llvm::Value* tiny = b_.CreateFCmpOLT(mag, min, "uf.tiny." + id);
  llvm::Value* nz = b_.CreateFCmpONE(mag, zero, "uf.nz." + id);
  llvm::Value* bad = b_.CreateAnd(tiny, nz, "uf.bad." + id);
  if (a && b) {
    // A product, quotient or power of two non-zero values that lands on exact
    // zero lost the whole exponent range.
    llvm::Value* rz = b_.CreateFCmpOEQ(r.reg, zero, "uf.rz." + id);
    llvm::Value* ab = b_.CreateAnd(b_.CreateFCmpONE(a, zero, "uf.az." + id),
                                   b_.CreateFCmpONE(b, zero, "uf.bz." + id), "uf.ab." + id);
    bad = b_.CreateOr(bad, b_.CreateAnd(rz, ab, "uf.flush." + id), "uf.any." + id);
  }
  llvm::BasicBlock* computeBB = b_.GetInsertBlock();
  llvm::BasicBlock* trapBB = llvm::BasicBlock::Create(ctx_, "uf.trap." + id, curFn_);
  llvm::BasicBlock* okBB = llvm::BasicBlock::Create(ctx_, "uf.ok." + id, curFn_);
  // intent: underflow is rare, so weight the clean edge and keep the hot block
  // contiguous, the same shape the zero-divide trap uses.
  llvm::MDBuilder mdb(ctx_);
  if (auto* br = b_.CreateCondBr(bad, trapBB, okBB))
    br->setMetadata(llvm::LLVMContext::MD_prof, mdb.createBranchWeights({1, 99}));
  b_.SetInsertPoint(trapBB);
  emitCondTrap(Stmt::kUnderflowCondKey, "pli_underflow", okBB, /*notifyOnly=*/true);
  b_.SetInsertPoint(okBB);
  llvm::PHINode* phi = b_.CreatePHI(r.reg->getType(), 2, "uf.r." + id);
  phi->addIncoming(r.reg, computeBB);
  for (llvm::BasicBlock* pred : llvm::predecessors(okBB))
    if (pred != computeBB)
      phi->addIncoming(zero, pred);
  r.reg = phi;
  return r;
}

// OVERFLOW (rule 94; ADR-333): a floating-point operator result whose
// magnitude exceeds the largest double raises the condition. IBM leaves the
// result undefined and its implicit action is a message followed by ERROR; a
// unit that returns normally re-raises ERROR. The value stays whatever IEEE
// produced (±inf), which is IBM's "undefined" result made concrete.
Val IRGen::applyOverflow(Val r) {
  if (!overflowChecks())
    return r;
  std::string id = std::to_string(n_++);
  llvm::Value* max = llvm::ConstantFP::get(b_.getDoubleTy(), std::numeric_limits<double>::max());
  llvm::Function* fabsFn = intrinsicFn("llvm.fabs.f64", b_.getDoubleTy(), {b_.getDoubleTy()});
  llvm::Value* mag = b_.CreateCall(fabsFn, {r.reg}, "of.mag." + id);
  // An infinite result is the only way |x| exceeds the largest finite double;
  // NaN compares false, so an invalid operand does not raise OVERFLOW.
  llvm::Value* bad = b_.CreateFCmpOGT(mag, max, "of.bad." + id);
  llvm::BasicBlock* computeBB = b_.GetInsertBlock();
  llvm::BasicBlock* trapBB = llvm::BasicBlock::Create(ctx_, "of.trap." + id, curFn_);
  llvm::BasicBlock* okBB = llvm::BasicBlock::Create(ctx_, "of.ok." + id, curFn_);
  llvm::MDBuilder mdb(ctx_);
  if (auto* br = b_.CreateCondBr(bad, trapBB, okBB))
    br->setMetadata(llvm::LLVMContext::MD_prof, mdb.createBranchWeights({1, 99}));
  b_.SetInsertPoint(trapBB);
  llvm::Function* dispatch = onTrapDispatch(Stmt::kOverflowCondKey, "pli_overflow");
  if (!dispatch) {
    // No unit in this module: message plus ERROR, which never returns.
    b_.CreateCall(runtimeFn("pli_overflow"), {});
    b_.CreateUnreachable();
  } else {
    b_.CreateCall(dispatch, {});
    // IBM's normal-return rule: the returned OVERFLOW unit raises ERROR.
    emitErrorRaise();
    b_.CreateBr(okBB);
  }
  b_.SetInsertPoint(okBB);
  llvm::PHINode* phi = b_.CreatePHI(r.reg->getType(), 2, "of.r." + id);
  phi->addIncoming(r.reg, computeBB);
  for (llvm::BasicBlock* pred : llvm::predecessors(okBB))
    if (pred != computeBB)
      phi->addIncoming(r.reg, pred);
  r.reg = phi;
  return r;
}

// The ERROR condition through the runtime ON stack (rule 94), used by IBM's
// normal-return rule for OVERFLOW: an ERROR unit established anywhere runs and
// execution resumes after it; without one the system action aborts. Mirrors
// emitSignal's ERROR branch, with ONCODE 1 inside the unit (ADR-076).
void IRGen::emitErrorRaise() {
  // The slot lives in the entry block: a trap inside a loop must not grow the
  // caller's stack each time the unit returns (ADR-263).
  llvm::Value* fnSlot = entryAlloca(b_.getPtrTy(), "on.ctxslot");
  llvm::Value* fn = b_.CreateCall(runtimeFn("pli_on_top_fn_pair"), {i64(0), fnSlot}, "ontopfn");
  llvm::Value* ctx = b_.CreateLoad(b_.getPtrTy(), fnSlot, "ontopctx");
  llvm::Value* none = b_.CreateICmpEQ(fn, llvm::Constant::getNullValue(b_.getPtrTy()), "onnosystem");
  llvm::BasicBlock* defBB = llvm::BasicBlock::Create(ctx_, "on.default", curFn_);
  llvm::BasicBlock* dspBB = llvm::BasicBlock::Create(ctx_, "on.dispatch", curFn_);
  b_.CreateCondBr(none, defBB, dspBB);
  b_.SetInsertPoint(defBB);
  b_.CreateCall(runtimeFn("pli_overflow_error"), {});
  b_.CreateUnreachable();
  b_.SetInsertPoint(dspBB);
  b_.CreateCall(runtimeFn("pli_set_oncode"), {i32(1)});
  llvm::FunctionType* hft = llvm::FunctionType::get(b_.getVoidTy(), {b_.getPtrTy()}, false);
  b_.CreateCall(hft, fn, {ctx});
  b_.CreateCall(runtimeFn("pli_set_oncode"), {i32(0)});
}

// STRINGRANGE check (rules (60)-(63),(94)): raise before the extraction when
// the argument triple is invalid. IBM's normal return then gives the clamped
// substring the lowerings already compute. y = LENGTH(x)+1 with z = 0 is
// explicitly not a raise.
void IRGen::emitStringrangeCheck(const Val& start, const Val& len, llvm::Value* srcLen) {
  llvm::Value* one = i64(1);
  llvm::Value* zero = i64(0);
  llvm::Value* start64 = toI64(start);
  llvm::Value* len64 = toI64(len);
  llvm::Value* srclen64 = b_.CreateZExtOrTrunc(srcLen, b_.getInt64Ty(), "sr.srclen");
  // start < 1, len < 0, or len reaches past the source's current length.
  llvm::Value* lo = b_.CreateICmpSLT(start64, one, "sr.lo");
  llvm::Value* neg = b_.CreateICmpSLT(len64, zero, "sr.neg");
  llvm::Value* avail =
      b_.CreateSub(srclen64, b_.CreateSub(start64, one, "sr.start0"), "sr.avail");
  llvm::Value* past = b_.CreateICmpSGT(len64, avail, "sr.past");
  llvm::Value* bad = b_.CreateOr(b_.CreateOr(lo, neg, "sr.bad1"), past, "sr.bad");
  std::string id = std::to_string(n_++);
  llvm::BasicBlock* trapL = llvm::BasicBlock::Create(ctx_, "sr.trap." + id, curFn_);
  llvm::BasicBlock* okL = llvm::BasicBlock::Create(ctx_, "sr.ok." + id, curFn_);
  b_.CreateCondBr(bad, trapL, okL);
  b_.SetInsertPoint(trapL);
  emitCondTrap(Stmt::kStringrangeCondKey, "pli_stringrange", okL, /*notifyOnly=*/true);
  b_.SetInsertPoint(okL);
}

// SIZE dispatch (QR1.4, rules (91)-(94)): body of a fixed-overflow trap
// block; the computational-trap shape with the SIZE key and abort call.
void IRGen::emitSizeTrap(llvm::BasicBlock* okBB) {
  emitCondTrap(Stmt::kSizeCondKey, "pli_fixed_overflow", okBB);
}

// Clamp an index into [lb, ub] (SUBSCRIPTRANGE resume value, rule 94): the
// guarded address computation uses the clamped index, so a handled slip
// touches the nearest edge element instead of out-of-bounds storage.
llvm::Value* IRGen::clampIndex(llvm::Value* i, llvm::Value* lb, llvm::Value* ub) {
  llvm::Value* lo = b_.CreateSelect(b_.CreateICmpSLT(i, lb, "clo"), lb, i, "cidx.lo");
  return b_.CreateSelect(b_.CreateICmpSGT(lo, ub, "chi"), ub, lo, "cidx");
}

// ZERODIVIDE dispatch (rule 94): two-phase API that branches before the
// division so the hot path carries no safe-divisor select or resume select.
// beginZerodivide emits the isZero branch with cold weighting and switches the
// insert point to the compute block; the caller emits the division there.
// endZerodivide emits the trap dispatch (reusing emitCondTrap) and a phi at the
// join that resumes with `zero` on every faulting path and `computed` on the
// clean path. When checks are off, beginZerodivide returns an inactive handle
// and endZerodivide returns `computed` untouched.
IRGen::ZdivCheck IRGen::beginZerodivide(llvm::Value* isZero, llvm::Value* zero, int key) {
  if (!zdivChecks())
    return ZdivCheck{}; // (NOZERODIVIDE): the raw result stands (rules (60)-(63), ADR-112)
  std::string zid = std::to_string(n_++);
  llvm::BasicBlock* trapBB = llvm::BasicBlock::Create(ctx_, "zd.trap." + zid, curFn_);
  llvm::BasicBlock* computeBB = llvm::BasicBlock::Create(ctx_, "zd.compute." + zid, curFn_);
  llvm::BasicBlock* okBB = llvm::BasicBlock::Create(ctx_, "zd.ok." + zid, curFn_);
  // intent: the faulting-divisor edge is cold (~1% taken); weight the hot
  // compute edge so the loop stays contiguous for the optimizer.
  llvm::MDBuilder zmdb(ctx_);
  if (auto* br = b_.CreateCondBr(isZero, trapBB, computeBB))
    br->setMetadata(llvm::LLVMContext::MD_prof, zmdb.createBranchWeights({1, 99}));
  b_.SetInsertPoint(computeBB);
  return ZdivCheck{trapBB, computeBB, okBB, zero, key};
}
llvm::Value* IRGen::endZerodivide(ZdivCheck z, llvm::Value* computed) {
  if (!z.computeBB)
    return computed; // (NOZERODIVIDE): the raw result stands (rules (60)-(63), ADR-112)
  // intent: close the compute block and dispatch the trap. emitCondTrap creates
  // handler blocks that branch to okBB; when no handler exists for `key` it
  // emits abort+unreachable and okBB keeps only computeBB as a predecessor, so
  // the phi trivially resolves to `computed`.
  b_.CreateBr(z.okBB);
  b_.SetInsertPoint(z.trapBB);
  emitCondTrap(z.key, "pli_zerodivide", z.okBB);
  b_.SetInsertPoint(z.okBB);
  llvm::PHINode* phi = b_.CreatePHI(computed->getType(), 1, "zdiv.r");
  phi->addIncoming(computed, z.computeBB);
  for (llvm::BasicBlock* pred : llvm::predecessors(z.okBB))
    if (pred != z.computeBB)
      phi->addIncoming(z.zero, pred);
  return phi;
}

// Constant-zero divisor fast path (rule 94): emit the trap directly and
// return `zero` — no phi or dead compute block.
llvm::Value* IRGen::zerodivideResume(llvm::Value* isZero, llvm::Value* computed,
                                     llvm::Value* zero) {
  if (!zdivChecks())
    return computed; // (NOZERODIVIDE): the raw result stands (rules (60)-(63), ADR-112)
  std::string zid = std::to_string(n_++);
  llvm::BasicBlock* trapBB = llvm::BasicBlock::Create(ctx_, "zd.trap." + zid, curFn_);
  llvm::BasicBlock* okBB = llvm::BasicBlock::Create(ctx_, "zd.ok." + zid, curFn_);
  llvm::MDBuilder zmdb(ctx_);
  if (auto* br = b_.CreateCondBr(isZero, trapBB, okBB))
    br->setMetadata(llvm::LLVMContext::MD_prof, zmdb.createBranchWeights({1, 99}));
  b_.SetInsertPoint(trapBB);
  emitCondTrap(Stmt::kZerodivideCondKey, "pli_zerodivide", okBB);
  b_.SetInsertPoint(okBB);
  return b_.CreateSelect(isZero, zero, computed, "zdiv.r");
}

// FIXED BINARY checked +,-,* (QR1.2): the overflow intrinsic yields the
// value plus a flag; a set flag traps through the SIZE path (hard ERROR
// when no SIZE handler is established, QR1.4).
llvm::Value* IRGen::checkedArith(Tok op, llvm::Value* a, llvm::Value* b, bool dec) {
  // A FIXED DECIMAL result is subject to FIXEDOVERFLOW (rule 94; ADR-334) when
  // it needs more than the PL/I maximum of 31 digits. Only the i128 storage
  // can hold such a value; the i64 intermediates top out at 18 digits, so a
  // wrap there stays on the SIZE path (recorded in GC).
  const bool wrapOn = sizeChecks();
  const bool foverOn = dec && fixedOverflowChecks() && a->getType()->isIntegerTy(128);
  if (!wrapOn && !foverOn) {
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
  // A FIXEDOVERFLOW site keeps the intrinsic path: the digit-length test needs
  // the value in an IR compare, not only the wrap flag.
  if (wrapOn && !foverOn) {
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
  }
  std::string base = op == Tok::Plus ? "sadd" : op == Tok::Minus ? "ssub" : "smul";
  std::string iname = "llvm." + base + ".with.overflow.i" + std::to_string(bits);
  llvm::Type* st = llvm::StructType::get(ctx_, {ty, b_.getInt1Ty()});
  llvm::Value* ov = b_.CreateCall(intrinsicFn(iname, st, {ty, ty}), {a, b}, "ov");
  llvm::Value* r = b_.CreateExtractValue(ov, 0, "bin");
  llvm::Value* of = b_.CreateExtractValue(ov, 1, "ovf");
  llvm::Value* bad = of;
  if (foverOn) {
    // The raw scaled integer's digit count is the result's precision, so
    // |result| >= 10^31 is exactly "more than 31 digits". A wrap of the i128
    // product is far past that too, so both edges share the one condition.
    llvm::Value* lim = i128(pliPow10_128(31));
    llvm::Value* negLim = i128(pli_neg_s(pliPow10_128(31)));
    llvm::Value* hi = b_.CreateICmpSGE(r, lim, "dfl.hi");
    llvm::Value* lo = b_.CreateICmpSLE(r, negLim, "dfl.lo");
    bad = b_.CreateOr(b_.CreateOr(hi, lo, "dfl.len"), of, "dfl.bad");
  }
  int seq = ovSeq_++;
  llvm::BasicBlock* trapBB =
      llvm::BasicBlock::Create(ctx_, "ov.trap." + std::to_string(seq), curFn_);
  llvm::BasicBlock* okBB = llvm::BasicBlock::Create(ctx_, "ov.ok." + std::to_string(seq), curFn_);
  b_.CreateCondBr(bad, trapBB, okBB);
  b_.SetInsertPoint(trapBB);
  if (foverOn)
    emitCondTrap(Stmt::kFixedoverflowCondKey, "pli_fixedoverflow", okBB);
  else
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

// Wide FIXED DECIMAL precision trap (ADR-289): the 10^prec limit does not fit
// i64, so compare at the value's width (the i256 cross-base rescale) before it
// narrows to i128. Same SIZE path and resume contract as magTrap.
void IRGen::wideMagTrap(llvm::Value* v, int prec) {
  if (!sizeChecks())
    return;
  unsigned bits = v->getType()->getIntegerBitWidth();
  llvm::APInt lim(bits, 1);
  llvm::APInt ten(bits, 10);
  for (int i = 0; i < prec; ++i)
    lim *= ten;
  llvm::Value* limHi = llvm::ConstantInt::get(v->getType(), lim);
  llvm::Value* limLo = llvm::ConstantInt::get(v->getType(), -lim);
  llvm::Value* hi = b_.CreateICmpSGE(v, limHi, "wov.hi");
  llvm::Value* lo = b_.CreateICmpSLE(v, limLo, "wov.lo");
  llvm::Value* of = b_.CreateOr(hi, lo, "wov");
  int seq = ovSeq_++;
  llvm::BasicBlock* trapBB =
      llvm::BasicBlock::Create(ctx_, "ov.trap." + std::to_string(seq), curFn_);
  llvm::BasicBlock* okBB = llvm::BasicBlock::Create(ctx_, "ov.ok." + std::to_string(seq), curFn_);
  b_.CreateCondBr(of, trapBB, okBB);
  b_.SetInsertPoint(trapBB);
  emitSizeTrap(okBB);
  b_.SetInsertPoint(okBB);
}

// FIXED BINARY declared-precision trap (QR1.2, rule (86), ADR-282): PL/I
// raises SIZE when a value exceeds FIXED BINARY(p)'s declared range
// [-2^p, 2^p-1] (rules (60)-(63)). The storage-width check cannot see it when
// p is narrower than the slot. On a resumed SIZE the value wraps inside the
// declared precision (p+1 bits, two's complement), so a value stored through
// the trap is always inside the range the type promises.
llvm::Value* IRGen::fixedBinPrecTrap(llvm::Value* v, int prec) {
  if (!sizeChecks() || prec <= 0)
    return v; // (NOSIZE): the wrapped value stands (rules (60)-(63), ADR-110)
  if (!v->getType()->isIntegerTy())
    return v; // a whole array is copied; each element already carries the type
  unsigned bits = v->getType()->getIntegerBitWidth();
  // p = bits-1 is the full signed slot: the storage check already enforces it,
  // and 2^p is not a representable positive constant at that width.
  if (prec >= (int)bits - 1)
    return v;
  llvm::APInt limAp(bits, 1);
  limAp <<= prec;
  llvm::Value* lim = llvm::ConstantInt::get(v->getType(), limAp);
  llvm::Value* negLim = llvm::ConstantInt::get(v->getType(), -limAp);
  llvm::Value* hi = b_.CreateICmpSGE(v, lim, "pov.hi");
  llvm::Value* lo = b_.CreateICmpSLT(v, negLim, "pov.lo");
  llvm::Value* of = b_.CreateOr(hi, lo, "pov");
  // Resume value: truncate to the p+1 signed bits and sign-extend back, the
  // wrap the declared precision defines (ADR-282).
  llvm::Value* low = b_.CreateTrunc(v, b_.getIntNTy((unsigned)prec + 1), "pov.wrap");
  llvm::Value* wrapped = b_.CreateSExt(low, v->getType(), "pov.ws");
  int seq = ovSeq_++;
  llvm::BasicBlock* trapBB =
      llvm::BasicBlock::Create(ctx_, "ov.trap." + std::to_string(seq), curFn_);
  llvm::BasicBlock* okBB = llvm::BasicBlock::Create(ctx_, "ov.ok." + std::to_string(seq), curFn_);
  b_.CreateCondBr(of, trapBB, okBB);
  b_.SetInsertPoint(trapBB);
  emitSizeTrap(okBB);
  b_.SetInsertPoint(okBB);
  return b_.CreateSelect(of, wrapped, v, "pov.r");
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
  // External ENTRY symbols (no PL/I body): parameters that normally carry
  // hidden extent/length arguments (rules (13),(18)) append those hidden
  // i64 values to the end of the C signature (even for by-value C
  // entries).
  // A plain-EXTERNAL PL/I entry is a procedure in another object file and
  // follows the PL/I inter-procedure convention: hidden extent/length args
  // for `*`-extent arrays (rule (13)) and CHAR(*) params (rule (18)),
  // exactly as if the procedure were defined in this compilation.
  if (!sym->proc && !en) {
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
      llvm::Function* decl =
          llvm::Function::Create(ft2, llvm::Function::ExternalLinkage, name, &mod_);
      applyPublishedEffect(decl, sym);
      return decl;
    }
    llvm::Type* rty = sym->entryIsFunction ? llvmTy(sym->entryRetTy) : b_.getVoidTy();
    llvm::FunctionType* ft = llvm::FunctionType::get(rty, pt, false);
    llvm::Function* decl = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, name, &mod_);
    applyPublishedEffect(decl, sym);
    return decl;
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
  // Evaluate each actual once for this call: the scope keeps the materialized
  // values alive across the argument loop and the hidden length arguments.
  ActualScope scope(*this);
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
// True when the statement list holds an asynchronous CALL (rule (79)): the
// same condition emitCall uses to route to emitAsyncCall.
static bool stmtHasAsync(const HStmt* s) {
  if (!s)
    return false;
  if (s->kind == HStmt::CallS && (s->hasTaskOpt || s->eventRef || s->priorityExpr))
    return true;
  if (stmtHasAsync(s->thenS.get()) || stmtHasAsync(s->elseS.get()) || stmtHasAsync(s->unit.get()))
    return true;
  for (const auto& st : s->body)
    if (stmtHasAsync(st.get()))
      return true;
  return false;
}

// True when the program has an asynchronous CALL: then its output statements
// are written under the output lock, so a task's record reaches the stream
// whole (ADR-233).
static bool programHasAsync(HProgram& prog) {
  for (auto& p : prog.procs)
    for (auto& st : p->body)
      if (stmtHasAsync(st.get()))
        return true;
  return false;
}

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

  // Module-level facts for --print-facts (ADR-260): the lowering policy and
  // which checks the driver enabled.
  facts_.setLoweringMode(loweringModeName(lowerMode_, perOpOverrides_));
  facts_.setCheck("size", !noSizeChecks_);
  facts_.setCheck("subscript", !noSubChecks_);
  facts_.setCheck("zerodivide", !noZdivChecks_);
  facts_.setCheck("conversion", !noConvChecks_);

  // ON ERROR handlers (rules (91)-(94)): number the established units, then
  // pre-create their functions so SIGNAL dispatch resolves regardless of
  // emission order (mirroring declareProc for procedures).
  assignOnIds(prog);
  declareOnHandlers(prog);

  // P6.1b (ADR-215): decide by-value parameters once for the whole program
  // so declareProc, emitProc, and every call site agree on signatures.
  computeByValueParams(prog);
  // WS-A1 (ADR-264): what each procedure can do to memory. The decision layer
  // computes it (the plan's C2); the verdict reaches LLVM only as a function
  // attribute (the plan's C1).
  {
    std::unordered_set<const Symbol*> byValue;
    for (const auto& kv : byValParam_)
      if (kv.second) // the map holds every parameter; only the true ones ride by value
        byValue.insert(kv.first);
    pli::EffectOptions eopts;
    eopts.sizeChecks = sizeChecks();
    eopts.subscriptChecks = subChecks();
    eopts.zerodivideChecks = zdivChecks();
    eopts.conversionChecks = convChecks();
    eopts.overflowChecks = overflowChecks();
    eopts.fixedOverflowChecks = fixedOverflowChecks();
    if (!noMemoryEffects_)
      effects_ = pli::analyzeEffects(prog, byValue, eopts);
  }
  // ADR-233: a program with an asynchronous CALL (rule (79)) writes to the
  // stream from more than one thread, so its output statements are bracketed
  // with the output lock. A program without one keeps the plain path.
  hasAsync_ = programHasAsync(prog);
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
     // Noinline: the optimizer's interprocedural analysis on the inlined body
     // can mis-reason about opaque pli_* pseudo-variable calls (set_pageno /
     // pageno) and collapse main() to unreachable. Keeping the MAIN proc as a
     // call boundary preserves correct control flow through pli_rt_fini.
     mfn->addFnAttr(llvm::Attribute::NoInline);
     b_.SetInsertPoint(bb);
     // ADR-234: the MAIN procedure runs as a task on worker 0, so the
     // scheduler can multiplex it with the program's tasks. In the legacy
     // pthread model pli_sched_main calls the entry directly and nothing
     // changes.
     b_.CreateCall(runtimeFn("pli_sched_main"), {b_.CreateBitCast(mfn, b_.getPtrTy())});
     b_.CreateCall(runtimeFn("pli_rt_fini"), {});
     b_.CreateRet(i32(0));
  }

  // Bitcode runtime (OPTIMIZATION.md §10, P3): link the pli_* definitions the
  // module references in from the runtime bitcode, so clang compiles real
  // bodies and can inline/specialize them instead of leaving external calls.
  // Only the MAIN unit embeds: runtime globals (CONTROLLED and ON stacks,
  // I/O state) must be shared across units, so every other unit keeps
  // external pli_* references that the embedded copy satisfies.
  // -emit-llvm keeps the raw module (its IR goldens pin compiler output, not
  // the runtime); the driver passes linkBitcode_ = false there. The embedded
  // blob is always the source; --rt-platform swaps only the platform TU.
  if (linkBitcode_ && prog.mainProc)
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
  os << "; Generated by pli (PL/I -> LLVM)\n";
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
// module references out of the runtime bitcode so clang compiles real bodies
// and can inline/specialize them. The LLVM version stamp (rt_core.c
// pli_llvm_version) is checked first: the bitcode is built by the same LLVM
// as pli, and a stale bitcode would mis-compile silently. The whole runtime
// is merged into the MAIN unit with external linkage, so every unit's pli_*
// references resolve to this one shared copy and the runtime's global state
// stays shared. The portable blob always comes from the embedded bundle
// (single-binary distribution: no external runtime file is accepted).
bool IRGen::linkRuntimeBitcode() {
  // Parse the embedded bundle for this target.
  std::unique_ptr<llvm::Module> rt;
  const pli::RuntimeBlob* blob = pli::selectRuntimeForTarget(llvm::Triple(triple_));
  if (!blob) {
    d_.error({}, "internal error: no embedded runtime for target '" + triple_ + "'", "");
    return false;
  }
  llvm::MemoryBufferRef buf(
      llvm::StringRef(reinterpret_cast<const char*>(blob->data), blob->len), "pli_runtime.bc");
  auto modOrErr = llvm::parseBitcodeFile(buf, ctx_);
  if (!modOrErr) {
    d_.error({}, "internal error: cannot parse the embedded runtime bitcode", "");
    llvm::consumeError(modOrErr.takeError());
    return false;
  }
  rt = std::move(*modOrErr);
  const std::string rtName = "embedded pli_runtime.bc";
  // A missing or mismatched stamp means a foreign or stale runtime.bc: the
  // emitted IR syntax is tied to the LLVM pli was built against.
  std::string stamp = "unknown";
  if (llvm::GlobalVariable* gv = rt->getNamedGlobal("pli_llvm_version"))
    if (llvm::Constant* init = gv->getInitializer())
      if (llvm::ConstantDataArray* s = llvm::dyn_cast<llvm::ConstantDataArray>(init))
        stamp = s->getAsCString().str(); // strips the trailing NUL
  if (stamp != PLI_LLVM_VERSION) {
    d_.error({}, "runtime bitcode '" + rtName + "' was built with LLVM " + stamp +
                     " but pli with LLVM " PLI_LLVM_VERSION +
                     " — rebuild pli (the runtime is embedded)",
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
                 " — rebuild pli (the runtime is embedded)",
             "");
    return false;
  }
  // The whole runtime is merged (no LinkOnlyNeeded) because library units
  // reference pli_* symbols this module does not call itself: only a full
  // merge guarantees every unit's reference resolves to this one embedded
  // copy. Dead-stripping at the final link removes whatever the
  // program does not use. The definitions keep their external linkage so the
  // runtime's global state (CONTROLLED/ON stacks, I/O) stays shared across
  // units; the P0 side-table facts are re-applied so the optimizer keeps the
  // audited memory/alloc effects on the real bodies.
  // Strip the build host's target-cpu / target-features / probe-stack /
  // tune-cpu from the pre-compiled runtime (IRGen P0, P5.1): if they survive
  // into the merged module, LLVM's inliner refuses to honour the AlwaysInline
  // that applyRuntimeAttrs stamps on the math wrappers (pli_sin, pli_mod_ll, ...)
  // and the pair functions (pli_on_top_pair, pli_on_top_fn_pair) — tune-cpu
  // differs between the build host and the link-time target and blocks inlining.
  // A probe-stack / target-cpu mismatch also breaks the backend on some targets.
  // The TargetMachine re-applies its own defaults at codegen, so stripping
  // is sound on every target.
  // Stamp NoInline on every runtime body so the non-always-inline wrappers
  // (e.g. pli_put_list_char) are not generically inlined at -O2 and survive
  // -dead_strip at link (tests/driver/link.sh dead-strip sub-check). The math
  // wrappers are re-attributed by applyRuntimeAttrs below, which clears NoInline
  // and sets AlwaysInline for them.
  for (auto& F : *rt) {
    if (F.hasFnAttribute("target-cpu"))
      F.removeFnAttr("target-cpu");
    if (F.hasFnAttribute("target-features"))
      F.removeFnAttr("target-features");
    if (F.hasFnAttribute("probe-stack"))
      F.removeFnAttr("probe-stack");
    if (F.hasFnAttribute("tune-cpu"))
      F.removeFnAttr("tune-cpu");
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
  // Split runtime: merge the platform TU the same way (custom --rt-platform
  // file or the embedded default). Its definitions satisfy the portable
  // blob's pli_plat_*/thread references; anything still undefined afterwards
  // is an incomplete custom platform, diagnosed by name.
  {
    std::string platErr;
    std::unique_ptr<llvm::Module> plat =
        pli::loadPlatformModule(ctx_, llvm::Triple(triple_), platformBc_, platErr);
    if (!plat) {
      d_.error({}, "internal error: " + (platErr.empty() ? "no embedded platform runtime for target '" +
                                                                   triple_ + "'"
                                                         : platErr),
               "");
      return false;
    }
    if (!mod_.getDataLayoutStr().empty())
      plat->setDataLayout(mod_.getDataLayoutStr());
    if (llvm::Linker::linkModules(mod_, std::move(plat), llvm::Linker::None)) {
      d_.error({}, "internal error: linking the platform runtime into the module failed", "");
      return false;
    }
    if (std::string seamErr = pli::checkPlatformSeamComplete(mod_); !seamErr.empty()) {
      d_.error({}, seamErr, "");
      return false;
    }
  }
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
  auto mem = llvm::MemoryBuffer::getMemBuffer(ir, "pli");
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
    // A binary target scales by 2^q (ADR-284); decimal literals truncate
    // their 10^s fraction with the same rule as the runtime path.
    if (t.k == TK::FixedBin)
      return wideDecConstant(t, binLitRescaled(ini, t));
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
    // A binary target scales by 2^q (ADR-284), the INITIAL counterpart of
    // convert()'s FIXED -> FIXED path.
    if (t.k == TK::FixedBin) {
      v.reg = wideDecConstant(t, binLitRescaled(e, t));
      break;
    }
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
    // A POSITION'd overlay (rule 24) starts `definedPos` bytes into the base
    // (or into the selected element), so its address is a byte GEP from there.
    llvm::Value* base = (!sym->definedConst.empty() && !sym->hasIsubOverlay())
                            ? definedConstAddr(sym)
                            : addressOf(sym->definedBase);
    if (sym->definedPos > 0)
      return b_.CreateInBoundsGEP(b_.getInt8Ty(), base, i64(sym->definedPos), "dpos");
    return base;
  }
  // A CONTROLLED variable (rules (15),(87)-(90), ADR-140) has no frame
  // storage: its address is the latest generation on its runtime stack.
  // Inside a hoisted DO loop the preheader already read it (emitDoIter).
  if (sym->controlled) {
    auto hit = ctlAddrHoist_.find(sym);
    if (hit != ctlAddrHoist_.end())
      return hit->second;
    // WS-K8 (ADR-257): a variable the compiler can address directly keeps
    // its generation in a slot, so the reference is a load, not a call. The
    // slot lives in the declaring procedure's frame: an ON handler (a
    // function of its own) and a nested procedure reach the variable
    // through the frame, so they ask the runtime for the generation.
    if (sym->ctlDirect && !inHandler_ && curProc_ && curProc_->src == sym->owner)
      return b_.CreateLoad(b_.getPtrTy(), ctlAnchorSlot(sym), "ctl.anchor.load");
    return b_.CreateCall(runtimeFn("pli_ctl_addr"), {ctlKeyOf(sym)}, "ctladdr");
  }
  // A STATIC AREA (rule (20)) lives for the whole program: create its region on
  // first use, so a block allocated by one activation stays valid for the next
  // and a later activation can FREE it. The slot is the module global.
  if (sym->ty.isArea() && sym->isStatic && !sym->ty.isArray()) {
    auto slot = symAddr_.find(sym);
    if (slot != symAddr_.end())
      b_.CreateCall(runtimeFn("pli_area_lazy"), {slot->second, i64(sym->areaSize)});
  }
  // rule (8): an enclosing variable is reached through this frame's static
  // link; otherwise it is this frame's own storage (globals / allocas /
  // parameters). Both are recorded in symAddr_.
  return symAddr_.count(sym) ? symAddr_[sym] : nullptr;
}

void IRGen::allocaLocals(HProc* p) {
  areaSlots_.clear();
  // P2 alias facts start fresh per function (see aliasScopeOf_): scopes from
  // an enclosing procedure must never describe this frame's storage.
  aliasDomain_ = nullptr;
  aliasScopes_.clear();
  aliasScopeOf_.clear();
  llvm::MDBuilder mdb(ctx_);
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
     // P2: tag plain AUTOMATIC allocas with alias-disjoint facts so LLVM
     // knows loads through this pointer cannot be clobbered by calls or
     // stores via unrelated pointers (exercises LICM + LAA). Only plain
     // automatic locals qualify — the alias taxonomy (ADR-265) decides which
     // those are, in one place. All scopes share one domain per function so
     // the facts compare; scalar loads/stores stamp the matching
     // !alias.scope/!noalias via annotateAlias. (ADR-002: IRGen only.)
     if (pli::aliasClass(s) == pli::AliasClass::Disjoint) {
       if (!aliasDomain_)
         aliasDomain_ = mdb.createAnonymousAliasScopeDomain("pli");
       llvm::MDNode* scope = mdb.createAnonymousAliasScope(aliasDomain_, "plain-automatic");
       aliasScopes_.push_back(scope);
       aliasScopeOf_[s] = scope;
       llvm::AllocaInst* ai = llvm::cast<llvm::AllocaInst>(a);
       ai->setMetadata(llvm::LLVMContext::MD_noalias,
                        llvm::MDNode::get(ctx_, {scope}));
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
      // out-of-order FREE via the region's size accounting. An AREA array gets
      // one region per element, each slot remembered for the epilogue.
      long long n = s->ty.isArray() ? arrayExtent(s->ty) : 1;
      llvm::ArrayType* arrTy =
          s->ty.isArray() ? llvm::ArrayType::get(b_.getPtrTy(), (unsigned)n) : nullptr;
      for (long long i = 0; i < n; ++i) {
        llvm::Value* slot =
            arrTy ? b_.CreateInBoundsGEP(arrTy, a, {i64(0), i64(i)}, "ar.i") : a;
        llvm::Value* region =
            b_.CreateCall(runtimeFn("pli_area_create"), {i64(s->areaSize)}, "area");
        b_.CreateStore(region, slot);
        areaSlots_.push_back(slot);
      }
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
    if (s->kind != Symbol::Var || !s->ty.isDynamic() ||
        (!s->basedBase && !s->basedNoPtr))
      continue;
    if (s->owner != p->src)
      continue;
    if (s->dynUb && !dynUb_.count(s))
      dynUb_[s] = toI64(emitExpr(s->dynUb));
    // A `(*)` or per-ALLOCATE BASED axis has no DECLARE bound expression;
    // seed the dope slot so a reference emitted before the first ALLOCATE
    // still has a value.
    if (s->ty.dims[0].adj && !dynUb_.count(s))
      dynUb_[s] = i64(s->ty.dims[0].ub);
    if (s->ty.dims[0].dyn && !s->dynUb && !dynUb_.count(s))
      dynUb_[s] = i64(s->ty.dims[0].ub);
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
    collectDeclStmts(s->unit.get(), out); // ON-unit block locals (ADR-299)
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
    // A binary target scales by 2^q (ADR-284); the decimal rescale below
    // only applies to FIXED DECIMAL.
    if (s->ty.k == TK::FixedBin) {
      out.reg = wideDecConstant(s->ty, binLitRescaled(e, s->ty));
      return true;
    }
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
  if ((!s->labels.empty() || !s->labelElems.empty()) && s->kind != HStmt::Entry) {
    std::string blk = "L" + std::to_string(n_++);
    llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx_, blk, curFn_);
    stmtBlocks_[s] = bb;
    for (const std::string& l : s->labels) {
      labelBlocks_[l] = bb;
      labelBeginChain_[l] = chain; // enclosing BEGIN scopes (rule 91)
    }
    // Rule (65): the label-array element is initialized with this statement's
    // address when the procedure is entered.
    for (const LabelElem& e : s->labelElems)
      if (e.sym)
        labelElemInits_.push_back({e.sym, e.subs, bb});
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

// Rule (65) initial-label: give each label-array element used as a statement
// prefix the address of that statement. The array is a fixed-extent automatic
// (sema checked it), so the element sits at a constant offset from its base and
// the store belongs in the entry block, where every activation performs it.
void IRGen::emitLabelElemInits() {
  for (const LabelElemInit& e : labelElemInits_) {
    const Type& arr = e.sym->ty;
    long long flat = 0, stride = 1;
    for (size_t k = arr.dims.size(); k-- > 0;) {
      flat += (e.subs[k] - arr.dims[k].lb) * stride;
      stride *= (long long)(arr.dims[k].ub - arr.dims[k].lb + 1);
    }
    llvm::Value* addr = b_.CreateInBoundsGEP(llvmTy(arr.elementType()), addressOf(e.sym),
                                             {i64(flat)}, "lelem");
    b_.CreateStore(llvm::BlockAddress::get(curFn_, e.bb), addr);
  }
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
// P6.1: decide whether a procedure is a candidate for AlwaysInline.
// Conservative: only non-recursive internal procs with plain params
// (no adjustable arrays, CHAR(*), BASED, CONTROLLED, DEFINED, or STATIC).
bool IRGen::inlineInternal(const HProc* p) const {
  if (p->isRecursive || p->isMain || p->isExternal || p->isPackage
      || !p->entryNames.empty() || !inlineProcs())
    return false;
  for (const Symbol* prm : p->paramSyms) {
    if (!prm)
      continue;
    if (prm->ty.isArray() || isStarLen(prm->ty) || prm->isBased()
        || prm->controlled || prm->definedBase || prm->isStatic)
      return false;
  }
  return true;
}

// P6.1b (ADR-215): true when e's tree mentions a VarRef to s at any depth.
bool IRGen::exprMentions(HExpr* e, Symbol* s) const {
  if (!e)
    return false;
  if ((e->kind == HExpr::VarRef || e->kind == HExpr::Subscript) && e->sym == s)
    return true;
  if (exprMentions(e->a.get(), s) || exprMentions(e->b.get(), s) ||
      exprMentions(e->locPtr.get(), s))
    return true;
  for (auto& a : e->args)
    if (exprMentions(a.get(), s))
      return true;
  return false;
}

// P6.1b (ADR-215): true when evaluating e takes s's address or stores
// through it. Plain value reads (arithmetic, builtin arguments, indices)
// are false; a bare VarRef to s is a read EXCEPT as a user-call argument,
// where argAddr passes the address (v1: always by reference — a call to a
// proc that itself takes the parameter by value would be safe, left as a
// documented refinement). ADDR/BYADDR/OMITTED/PRESENT expose the address
// in any position.
bool IRGen::exprTakesAddr(HExpr* e, Symbol* s) const {
  if (!e)
    return false;
  if (e->kind == HExpr::Subscript && e->sym == s)
    return true; // a scalar parameter is never subscripted; defensive
  if (e->kind == HExpr::VarRef && e->sym == s)
    return !e->memberPath.empty() || (bool)e->locPtr; // qualified refs need an address
  if (e->kind == HExpr::Call && (e->name == "ADDR" || e->name == "BYADDR" ||
                                 e->name == "OMITTED" || e->name == "PRESENT")) {
    for (auto& a : e->args)
      if (exprMentions(a.get(), s))
        return true;
    return false;
  }
  if (e->kind == HExpr::Call && e->sym &&
      (e->sym->proc || e->sym->isEntry || e->sym->isEntryVar)) {
    // A user, external-entry, or entry-variable call: a bare VarRef argument
    // rides by reference (argAddr), so the address escapes there.
    for (auto& a : e->args) {
      HExpr* arg = a.get();
      if (arg && arg->kind == HExpr::VarRef && arg->sym == s)
        return true;
      if (exprTakesAddr(arg, s))
        return true;
    }
    return false;
  }
  if (exprTakesAddr(e->a.get(), s) || exprTakesAddr(e->b.get(), s) ||
      exprTakesAddr(e->locPtr.get(), s))
    return true;
  for (auto& a : e->args)
    if (exprTakesAddr(a.get(), s))
      return true;
  return false;
}

// P6.1b (ADR-215): true when st stores through s or exposes s's address.
// `target` (Assign/Read INTO) is a write; pseudo-variable targets
// (SUBSTR/EXPONENT/UNSPEC calls) write their variable anywhere inside.
// GET items and PUT STRING targets write; everything else reads.
bool IRGen::stmtTakesAddr(HStmt* st, Symbol* s) const {
  if (!st)
    return false;
  auto targetTakes = [&](HExpr* t) {
    if (!t)
      return false;
    if (t->kind == HExpr::Call)
      return exprMentions(t, s); // pseudo-variable assignment writes inside
    if (t->kind == HExpr::VarRef && t->sym == s)
      return true;
    return exprTakesAddr(t, s);
  };
  if (targetTakes(st->target.get()))
    return true;
  for (auto& t : st->extraTargets)
    if (targetTakes(t.get()))
      return true;
  if (st->kind == HStmt::DoIter && st->sym == s)
    return true; // the control variable is stepped in place
  if (st->fileSym == s)
    return true; // a FILE handle is passed by reference; defensive
  if (st->kind == HStmt::Get) {
    for (auto& it : st->items)
      if (targetTakes(it.get()))
        return true;
  } else {
    for (auto& it : st->items)
      if (exprTakesAddr(it.get(), s))
        return true;
  }
  if (st->stringTarget &&
      ((st->kind == HStmt::Put && targetTakes(st->stringTarget.get())) ||
       (st->kind != HStmt::Put && exprTakesAddr(st->stringTarget.get(), s))))
    return true;
  for (auto& a : st->args) {
    HExpr* arg = a.get();
    if (arg && arg->kind == HExpr::VarRef && arg->sym == s)
      return true; // v1: any direct call argument keeps by reference
    if (exprTakesAddr(arg, s))
      return true;
  }
  // CALL task/event handles and WAIT events are references, not values.
  if (exprMentions(st->taskRef.get(), s) || exprMentions(st->eventRef.get(), s))
    return true;
  for (auto& w : st->waitEvents)
    if (exprMentions(w.get(), s))
      return true;
  if (exprTakesAddr(st->value.get(), s) || exprTakesAddr(st->cond.get(), s) ||
      exprTakesAddr(st->from.get(), s) || exprTakesAddr(st->to.get(), s) ||
      exprTakesAddr(st->by.get(), s) || exprTakesAddr(st->gotoTarget.get(), s) ||
      exprTakesAddr(st->skipCount.get(), s) || exprTakesAddr(st->waitCount.get(), s) ||
      exprTakesAddr(st->priorityExpr.get(), s) || exprTakesAddr(st->oncodeExpr.get(), s))
    return true;
  for (auto& f : st->formats)
    if (exprTakesAddr(f.w.get(), s) || exprTakesAddr(f.d.get(), s) ||
        exprTakesAddr(f.s.get(), s))
      return true;
  for (auto& b : st->allocBase)
    if (exprTakesAddr(b.get(), s))
      return true;
  for (auto& al : st->allocSet)
    if (targetTakes(al.get())) // ALLOCATE .. SET(p) points p elsewhere
      return true;
  for (auto& ar : st->allocArea)
    if (exprTakesAddr(ar.get(), s))
      return true;
  for (auto& bb : st->allocBounds)
    for (auto& bnd : bb)
      if (exprTakesAddr(bnd.lb.get(), s) || exprTakesAddr(bnd.ub.get(), s))
        return true;
  for (auto& l : st->allocCharLen)
    if (exprTakesAddr(l.get(), s))
      return true;
  for (auto& f : st->freeBase)
    if (exprTakesAddr(f.get(), s))
      return true;
  for (auto& f : st->freeArea)
    if (exprTakesAddr(f.get(), s))
      return true;
  for (auto& d : st->decls)
    for (auto& bnd : d.dynBounds)
      if (exprTakesAddr(bnd.get(), s))
        return true;
  for (auto& d : st->decls)
    for (auto& bnd : d.dynLbBounds)
      if (exprTakesAddr(bnd.get(), s))
        return true;
  for (auto& d : st->decls)
    for (auto& bnd : d.dynMemberBounds)
      if (exprTakesAddr(bnd.get(), s))
        return true;
  for (auto& d : st->decls)
    if (exprTakesAddr(d.dynLenBound.get(), s) || exprTakesAddr(d.init.get(), s) ||
        exprTakesAddr(d.initCall.get(), s))
      return true;
  if (stmtTakesAddr(st->thenS.get(), s) || stmtTakesAddr(st->elseS.get(), s) ||
      stmtTakesAddr(st->unit.get(), s))
    return true;
  for (auto& b : st->body)
    if (stmtTakesAddr(b.get(), s))
      return true;
  return false;
}

// P6.1b (ADR-215): decide every internal parameter's convention once per
// program. A parameter rides by value when (a) the procedure is eligible —
// internal, single-entry, unnamed, inline-procs gate on (checks off), never
// address-taken as an ENTRY value, never called asynchronously — and (b)
// the parameter is a plain FIXED/FLOAT/POINTER scalar that no body in its
// lexical subtree stores through or exposes the address of. By reference
// is always sound, so every uncertainty keeps by reference.
void IRGen::computeByValueParams(const HProgram& prog) {
  byValParam_.clear();
  // Lexical children for the subtree walk (nested bodies can reference an
  // outer parameter through a static link). HProc::src points back at the
  // AST procedure, which is what Symbol::proc references.
  std::unordered_map<const HProc*, std::vector<const HProc*>> kids;
  std::unordered_map<const void*, const HProc*> bySrc;
  for (auto& p : prog.procs) {
    if (p->parent)
      kids[p->parent].push_back(p.get());
    if (p->src)
      bySrc[(const void*)p->src] = p.get();
  }
  auto hprocOf = [&](const void* src) -> const HProc* {
    auto it = bySrc.find(src);
    return it != bySrc.end() ? it->second : nullptr;
  };
  // Whole-program facts: address-taken procedures (ENTRY values use the
  // all-pointer convention) and asynchronously-called procedures (the task
  // context packs addresses).
  std::unordered_set<const HProc*> addrTaken, asyncCalled;
  std::function<void(HExpr*)> scanExpr;
  scanExpr = [&](HExpr* e) {
    if (!e)
      return;
    if (e->kind == HExpr::VarRef && e->sym && e->sym->kind == Symbol::ProcName && e->sym->proc)
      if (const HProc* hp = hprocOf(e->sym->proc))
        addrTaken.insert(hp);
    scanExpr(e->a.get());
    scanExpr(e->b.get());
    scanExpr(e->locPtr.get());
    for (auto& a : e->args)
      scanExpr(a.get());
  };
  std::function<void(HStmt*)> scan;
  scan = [&](HStmt* st) {
    if (!st)
      return;
    if ((st->kind == HStmt::CallS) && (st->hasTaskOpt || st->eventRef || st->priorityExpr) &&
        st->sym && st->sym->proc)
      if (const HProc* hp = hprocOf(st->sym->proc))
        asyncCalled.insert(hp);
    scanExpr(st->target.get());
    scanExpr(st->value.get());
    scanExpr(st->cond.get());
    scanExpr(st->from.get());
    scanExpr(st->to.get());
    scanExpr(st->by.get());
    for (auto& t : st->extraTargets)
      scanExpr(t.get());
    for (auto& it : st->items)
      scanExpr(it.get());
    scanExpr(st->stringTarget.get());
    scanExpr(st->gotoTarget.get());
    scanExpr(st->skipCount.get());
    scanExpr(st->waitCount.get());
    scanExpr(st->priorityExpr.get());
    scanExpr(st->taskRef.get());
    scanExpr(st->eventRef.get());
    for (auto& w : st->waitEvents)
      scanExpr(w.get());
    scanExpr(st->oncodeExpr.get());
    for (auto& a : st->args)
      scanExpr(a.get());
    for (auto& b : st->allocBase)
      scanExpr(b.get());
    for (auto& al : st->allocSet)
      scanExpr(al.get());
    for (auto& ar : st->allocArea)
      scanExpr(ar.get());
    for (auto& bb : st->allocBounds) {
      for (auto& bnd : bb) {
        scanExpr(bnd.lb.get());
        scanExpr(bnd.ub.get());
      }
    }
    for (auto& l : st->allocCharLen)
      scanExpr(l.get());
    for (auto& f : st->freeBase)
      scanExpr(f.get());
    for (auto& f : st->freeArea)
      scanExpr(f.get());
    for (auto& f : st->formats) {
      scanExpr(f.w.get());
      scanExpr(f.d.get());
      scanExpr(f.s.get());
    }
    for (auto& d : st->decls) {
      for (auto& bnd : d.dynBounds)
        scanExpr(bnd.get());
      for (auto& bnd : d.dynLbBounds)
        scanExpr(bnd.get());
      for (auto& bnd : d.dynMemberBounds)
        scanExpr(bnd.get());
      scanExpr(d.dynLenBound.get());
      scanExpr(d.init.get());
      scanExpr(d.initCall.get());
    }
    scan(st->thenS.get());
    scan(st->elseS.get());
    scan(st->unit.get());
    for (auto& b : st->body)
      scan(b.get());
  };
  for (auto& p : prog.procs) {
    for (auto& st : p->body)
      scan(st.get());
  }
  // A DEFINED overlay stores through its base's cell, so a parameter that
  // is any overlay's base keeps by reference.
  std::unordered_set<Symbol*> overlayBase;
  for (Symbol* sym : sema_.storage())
    if (sym && sym->definedBase)
      overlayBase.insert(sym->definedBase);
  for (auto& p : prog.procs) {
    HProc* proc = p.get();
    bool eligible = !proc->isMain && !proc->isExternal && !proc->isPackage &&
                    proc->entryNames.empty() && inlineProcs() &&
                    !addrTaken.count(proc) && !asyncCalled.count(proc);
    if (eligible)
      for (auto& st : proc->body)
        if (st && st->kind == HStmt::Entry) {
          eligible = false; // multi-entry thunks marshal pointers
          break;
        }
    // The lexical subtree: this body plus every nested procedure body.
    std::vector<const HProc*> subtree{proc};
    for (size_t i = 0; i < subtree.size(); ++i) {
      auto it = kids.find(subtree[i]);
      if (it != kids.end())
        subtree.insert(subtree.end(), it->second.begin(), it->second.end());
    }
    for (Symbol* prm : proc->paramSyms) {
      bool byVal = eligible && prm && !prm->ty.isArray() && !isStarLen(prm->ty) &&
                   !prm->isBased() && !prm->controlled && !prm->isStatic && !prm->definedBase &&
                   !prm->isOptional && !overlayBase.count(prm) &&
                   (prm->ty.isFixed() || prm->ty.k == TK::Float || prm->ty.isPointer()) &&
                   !isAdjustable(prm);
      if (byVal)
        for (const HProc* q : subtree)
          for (auto& st : q->body)
            if (stmtTakesAddr(st.get(), prm)) {
              byVal = false;
              break;
            }
      byValParam_[prm] = byVal ? 1 : 0;
    }
  }
}

// P6.3: strip through cast instructions (sext/trunc/zext) to find whether v
// traces back to a load from a non-negative DO index's alloca.
bool IRGen::isDerivedFromNonNegDoIndex(llvm::Value* v) const {
  while (auto* inst = llvm::dyn_cast<llvm::Instruction>(v)) {
    if (auto* li = llvm::dyn_cast<llvm::LoadInst>(inst)) {
      llvm::Value* ptr = li->getPointerOperand();
      for (Symbol* s : nonNegDoIndices_) {
        auto it = symAddr_.find(s);
        if (it != symAddr_.end() && it->second == ptr)
          return true;
      }
      return false;
    }
    if (inst->getNumOperands() >= 1)
      v = inst->getOperand(0);
    else
      return false;
  }
  return false;
}

// The memory-effect verdict for a procedure (ADR-264).
pli::MemEffect IRGen::effectOf(const HProc* p) const {
  auto it = effects_.find(p);
  return it == effects_.end() ? pli::MemEffect::Unknown : it->second.effect;
}

// The control-flow verdict for a procedure (ADR-264): can it unwind, and does
// it always return?
void IRGen::applyControlAttrs(llvm::Function* f, const HProc* p) {
  auto it = effects_.find(p);
  if (it == effects_.end())
    return;
  if (it->second.nounwind)
    f->addFnAttr(llvm::Attribute::NoUnwind);
  if (it->second.willReturn)
    f->addFnAttr(llvm::Attribute::WillReturn);
}

// ADR-271: describe the shape of a by-ref array parameter to LLVM. The
// dummy's declared extent is known at compile time, so its base pointer
// addresses at least that many elements and LLVM may widen the loads it
// makes through it (and vectorize a loop over the parameter).
//
// The claim holds for a conforming program: PL/I requires the actual to be
// conformable with the dummy (rule (34)), so it carries at least the
// dummy's elements. It says nothing about alignment — a BASED actual's base
// pointer carries no alignment guarantee — so `align` is not claimed.
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
      // P6.1b (ADR-215): read-only scalar params ride by value so the
      // optimizer sees SSA arguments (const-prop folds bounded recursion).
      pt.push_back(isByValueParam(p->paramSyms[i]) ? llvmTy(p->paramSyms[i]->ty)
                                                   : b_.getPtrTy());
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
    applyMemoryEffect(fn, effectOf(p));
    applyControlAttrs(fn, p);
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
  applyMemoryEffect(impl, effectOf(p));
  applyControlAttrs(impl, p);

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
    applyMemoryEffect(tf, effectOf(p));
    applyControlAttrs(tf, p);
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

// Does `s` establish or revert a handler (rules (91),(92)) at the current
// block level? A nested BEGIN scopes its own handler depth (it restores on
// exit), so it is not descended; ON-unit bodies are, because a nested ON
// inside a handler runs within this block's dynamic extent and must be undone
// here. The runtime's handler depth spans every condition (one shared stack),
// so any `ON` or `REVERT` — not just `ON ERROR` — forces the save/restore:
// REVERT needs the activation base to tell its own unit from an enclosing
// one (ADR-350). Procedure-precise skip: only blocks that can touch a
// handler pay the entry/exit depth save/restore (OPTIMIZATION.md §8.1).
static bool hasOnAtLevel(HStmt* s) {
  if (!s)
    return false;
  if (s->kind == HStmt::Begin)
    return false; // a nested BEGIN restores its own entry depth
  if (s->kind == HStmt::On || s->kind == HStmt::Revert)
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

// True if any top-level statement of `p` establishes or reverts a handler,
// so the procedure must save/restore the handler depth at entry/exit.
static bool procEstablishesOn(HProc* p) {
  for (auto& st : p->body)
    if (hasOnAtLevel(st.get()))
      return true;
  return false;
}

void IRGen::emitProc(HProc* p) {
  if (p->isPackage)
    return; // packages emit no function (extension, ADR-109)
  // P6.1: mark small internal non-recursive procs for inlining so hot call
  // sites pay no frame setup at -O2 (LLVM inlines at -O3 already).
  if (inlineInternal(p)) {
    if (llvm::Function* fn = mod_.getFunction(p->irName.substr(1))) {
      fn->removeFnAttr(llvm::Attribute::NoInline);
      fn->addFnAttr(llvm::Attribute::AlwaysInline);
    }
  }
  curProc_ = p;
  // Open the procedure's fact block so --print-facts groups the decisions by
  // procedure (ADR-260).
  facts_.beginProc(p->name);
  // The memory-effect promise made about this procedure (ADR-264), so
  // --print-facts shows what the compiler claimed and why.
  {
    auto it = effects_.find(p);
    if (it != effects_.end()) {
      facts_.setEffects(p->name, effectName(it->second.effect), it->second.note,
                        it->second.nounwind, it->second.willReturn);
    }
  }
  labelBlocks_.clear();
  labelBeginChain_.clear();
  labelElemInits_.clear();
  stmtBlocks_.clear();
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
  // WS-K8 (ADR-257): create this procedure's anchors now, before any
  // statement, so the slot's initialisation (a null store in the entry
  // block) dominates every use of it.
  for (Symbol* sym : sema_.storage())
    if (sym && sym->controlled && sym->ctlDirect && sym->owner == p->src)
      ctlAnchorSlot(sym);
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

  // P6.1b (ADR-215): a by-value parameter keeps a frame cell holding the
  // entry value, so every address-taking path (static links, argAddr of a
  // nested call, DEFINED-free reads) keeps working unchanged. No store to
  // the cell exists anywhere (proven by computeByValueParams), so reads see
  // the entry value and the inliner sees an SSA argument after SROA.
  for (Symbol* s : p->paramSyms) {
    if (!isByValueParam(s))
      continue;
    llvm::AllocaInst* cell = entryAlloca(llvmTy(s->ty), s->name + ".bv");
    b_.CreateStore(symAddr_[s], cell);
    symAddr_[s] = cell;
  }

  // CONTROLLED dummy association (rule (15)): -1 (non-controlled actual)
  // keeps the dummy's own slot; any other value is the caller's key.
  for (auto& [s, hv] : ctlHid)
    ctlKey_[s] = b_.CreateSelect(b_.CreateICmpEQ(hv, i64(-1), "ctlmine"),
                                 i64((long long)s->ctlSlot), hv, "ctlkey");

  allocaLocals(p);
  emitInitials(p); // INITIAL attribute on AUTOMATIC variables (rule 26)
  emitLabelElemInits(); // rule (65) label-array prefixes
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
    // FINISH (rule 94; ADR-322) is raised before the handler state goes.
    emitFinishSignal();
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
  emitLabelElemInits(); // rule (65) label-array prefixes
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
    // FINISH (rule 94; ADR-322): segment exit is procedure exit.
    emitFinishSignal();
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
    else if (keyId.first == Stmt::kStringrangeCondKey)
      name = "PLI_ON_STRINGRANGE_" + std::to_string(keyId.second);
    else
      name = "PLI_ONC_" + std::to_string(keyId.first) + "_" + std::to_string(keyId.second);
    onHandlers_[keyId.first].push_back(
        llvm::Function::Create(ft, llvm::Function::InternalLinkage, name, &mod_));
    // T1b (rule 94): mark handler functions AlwaysInline when checks are
    // off (-O2/-O3 default) so trivial inline handlers fold into the trap
    // block, eliminating call/ret overhead on the faulting path.
    if (inlineProcs())
      onHandlers_[keyId.first].back()->addFnAttr(llvm::Attribute::AlwaysInline);
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
      labelElemInits_.clear();
      stmtBlocks_.clear();
      onScopes_.clear();
      symAddr_.clear();
      ctlImplicitAlloc_.clear(); // a handler allocates/frees nothing implicitly
      ctlAddrHoist_.clear();
      ctlLenHoist_.clear();
      ctlExtHoist_.clear();
      areaSlots_.clear(); // ... and owns no AREA regions
      for (Symbol* gs : sema_.storage())
        if (gs->isStatic && gs->kind == Symbol::Var)
          symAddr_[gs] = mod_.getGlobalVariable(gs->irName.substr(1), true);
      llvm::BasicBlock* entry = llvm::BasicBlock::Create(ctx_, "entry", fn);
      b_.SetInsertPoint(entry);
      // An ON unit is a block activation of its own (rule 91; ADR-350): save
      // its entry depth so a unit armed inside it ends when it returns.
      curOnUnitDepth_ = entryAlloca(b_.getInt64Ty(), "onunitdepth");
      b_.CreateStore(b_.CreateCall(runtimeFn("pli_on_depth_error"), {}),
                     curOnUnitDepth_);
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
      if (!blockTerminated(b_.GetInsertBlock())) {
        // Unit return terminates the activation: drop the units it armed.
        b_.CreateCall(runtimeFn("pli_on_reset_error"),
                      {b_.CreateLoad(b_.getInt64Ty(), curOnUnitDepth_, "onunitdepth")});
        b_.CreateRetVoid();
      }
      curOnUnitDepth_ = nullptr;
      curOnDepth_ = savedDepth;
      inHandler_ = false;
      curFn_ = nullptr;
      curProc_ = nullptr;
    }
  }
}

// FINISH (rule 94; ADR-322): raise the condition at a task procedure's exit.
// Every procedure exit that establishes an ON unit calls this; the runtime
// compares the procedure against the one the spawn wrapper recorded, so a
// nested or normal call, and the main task, do nothing.
void IRGen::emitFinishSignal() {
  if (!curOnDepth_ || !curFn_)
    return;
  b_.CreateCall(runtimeFn("pli_signal_finish"),
                {b_.CreateBitCast(curFn_, b_.getPtrTy(), "finproc")});
}

// The activation base for ON and REVERT (rule (91); ADR-350): the innermost
// block activation's entry depth — an ON-unit body, a BEGIN block, or the
// procedure. Only units established at or above this depth belong to the
// current activation. When no depth was saved (no ON/REVERT can occur here),
// read the live depth so a REVERT stays a no-op and an ON simply pushes.
llvm::Value* IRGen::onBaseDepth() {
  if (curOnUnitDepth_)
    return b_.CreateLoad(b_.getInt64Ty(), curOnUnitDepth_, "onunitbase");
  if (!onScopes_.empty() && onScopes_.back().entryDepth)
    return onScopes_.back().entryDepth;
  if (curOnDepth_)
    return b_.CreateLoad(b_.getInt64Ty(), curOnDepth_, "onprocbase");
  return b_.CreateCall(runtimeFn("pli_on_depth_error"), {}, "onlivebase");
}

// Establish a handler: push its id (0 for the system action) plus the handler
// address and the capture context for automatics the unit touches (rule 91).
// The address/context pair is what lets a SIGNAL raised anywhere — including
// another object file — run this module's handler against this frame. The
// activation base makes a repeated ON replace the earlier unit (ADR-350).
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
        // The captured automatic is pinned in storage: an ON unit reads it
        // in the raiser's frame (ADR-253). Report which ones, so a user can
        // see the ORDER rule at work (--print-facts).
        facts_.recordPinned(s->onCaps[i]->name);
      }
      ctx = cap;
    }
  }
  b_.CreateCall(runtimeFn("pli_on_establish"),
                {fileCondKeyValue(s), i64(id), fnv, ctx, onBaseDepth()});
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
  // T4: single combined scan replaces pli_on_top_fn + pli_on_top_ctx.
  llvm::Value* fnSlot = b_.CreateAlloca(b_.getPtrTy(), nullptr, "on.ctxslot");
  llvm::Value* fn =
      b_.CreateCall(runtimeFn("pli_on_top_fn_pair"), {i64(s->condKey), fnSlot}, "ontopfn");
  llvm::Value* ctx = b_.CreateLoad(b_.getPtrTy(), fnSlot, "ontopctx");
  llvm::Value* none =
      b_.CreateICmpEQ(fn, llvm::Constant::getNullValue(b_.getPtrTy()), "onnosystem");
  llvm::BasicBlock* defBB = llvm::BasicBlock::Create(ctx_, "on.default", curFn_);
  llvm::BasicBlock* dspBB = llvm::BasicBlock::Create(ctx_, "on.dispatch", curFn_);
  llvm::BasicBlock* resBB = llvm::BasicBlock::Create(ctx_, "on.resume", curFn_);
  b_.CreateCondBr(none, defBB, dspBB);
  b_.SetInsertPoint(defBB);
  // STRINGRANGE's implicit action is "message printed, processing continues"
  // (rules (60)-(63),(94)); every other served condition aborts.
  if (s->condKey == Stmt::kStringrangeCondKey) {
    b_.CreateCall(runtimeFn("pli_stringrange"), {});
    b_.CreateBr(resBB);
  } else if (s->condKey == Stmt::kOverflowCondKey) {
    // OVERFLOW's implicit action is a message followed by ERROR (ADR-333).
    b_.CreateCall(runtimeFn("pli_overflow"), {});
    b_.CreateUnreachable();
  } else if (s->condKey == Stmt::kFixedoverflowCondKey) {
    // FIXEDOVERFLOW's implicit action is a message followed by ERROR (ADR-334).
    b_.CreateCall(runtimeFn("pli_fixedoverflow"), {});
    b_.CreateUnreachable();
  } else {
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
  }
  b_.SetInsertPoint(dspBB);
  if (s->condKey == 0)
    b_.CreateCall(runtimeFn("pli_set_oncode"), {i32(1)});
  llvm::FunctionType* hft = llvm::FunctionType::get(b_.getVoidTy(), {b_.getPtrTy()}, false);
  b_.CreateCall(hft, fn, {ctx});
  if (s->condKey == 0)
    b_.CreateCall(runtimeFn("pli_set_oncode"), {i32(0)});
  // IBM's normal-return rule for OVERFLOW: the returned unit raises ERROR
  // (ADR-333), which either runs an ERROR unit or takes the system action.
  if (s->condKey == Stmt::kOverflowCondKey)
    emitErrorRaise();
  b_.CreateBr(resBB);
  b_.SetInsertPoint(resBB);
}

// ---------------------------------------------------------------------------
// statements
// ---------------------------------------------------------------------------
void IRGen::emitStmt(HStmt* s) {
  if (!s)
    return;
  // The current source location, for a remark raised inside a built-in
  // lowering (ADR-254).
  curLoc_ = s->loc;
  if (auto it = stmtBlocks_.find(s); it != stmtBlocks_.end()) {
    // A labelled statement starts the block collected for it (rules (64),(65)).
    startBlock(it->second);
  } else if (blockTerminated(b_.GetInsertBlock())) {
    newBlock(); // unreachable code (e.g. after STOP): start a fresh block
  }
  // Condition enable-state (rules (60)-(63), ADR-110/112): each statement
  // sees its own disables OR-inherited through enclosing statements, so a
  // prefixed group covers its body. A CHECK(...) prefix (rule (95), ADR-320)
  // clears the inherited disable for the named check instead. Balanced by
  // construction (single exit).
  CheckState top;
  if (!checkStack_.empty())
    top = checkStack_.back();
  top.noSize = s->chkSize ? false : (top.noSize || s->noSize);
  top.noSub = s->chkSub ? false : (top.noSub || s->noSub);
  top.noZdiv = s->chkZdiv ? false : (top.noZdiv || s->noZdiv);
  top.noConv = s->chkConv ? false : (top.noConv || s->noConv);
  top.noUnder = s->chkUnder ? false : (top.noUnder || s->noUnder);
  top.noOver = s->chkOver ? false : (top.noOver || s->noOver);
  top.noFOver = s->chkFOver ? false : (top.noFOver || s->noFOver);
  // An explicit (STRINGRANGE)/(NOSTRINGRANGE) prefix wins for its subtree.
  if (s->strRange != 0)
    top.strRange = s->strRange;
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
    // ADR-233: the value is evaluated first, then written under the output
    // lock when the program has a task.
    Val v = emitExpr(s->value.get());
    switch (v.ty.k) {
    case TK::Char:
      deferCall("pli_display_char", {v.ptr, v.len});
      break;
    case TK::Float:
      deferCall("pli_display_float", {v.reg});
      break;
    case TK::Bit: {
      llvm::Value* bit = b_.CreateZExt(v.reg, b_.getInt8Ty(), "bit");
      deferCall("pli_display_bit", {bit});
      break;
    }
    case TK::FixedBin:
    case TK::FixedDec:
      if (v.ty.k == TK::FixedDec && v.ty.scale > 0) {
        if (v.ty.intBits() == 128) // ADR-191: >18 digit DECIMAL needs wide I/O
          emitWideDecOut("pli_display_decfixed128", v.reg, v.ty.scale);
        else
          deferCall("pli_display_decfixed", {toI64(v), i64(v.ty.scale)});
      } else if (v.ty.k == TK::FixedDec && v.ty.intBits() == 128) {
        emitWideDecOut("pli_display_decfixed128", v.reg, 0);
      } else if (v.ty.k == TK::FixedBin && v.ty.scale > 0) {
        // The stored integer is value*2^q; the runtime prints the point
        // (ADR-284).
        deferCall("pli_display_fixed_scaled", {toI64(v), i64(v.ty.scale)});
      } else {
        deferCall("pli_display_fixed", {toI64(v)});
      }
      break;
    case TK::Pointer:
      d_.error(s->loc, "a POINTER value cannot be written with DISPLAY in this stage", "(114)");
      break;
    case TK::Complex:
      deferCall("pli_display_complex", {b_.CreateExtractValue(v.cpxVal(), 0, "cpx.re"),
                                        b_.CreateExtractValue(v.cpxVal(), 1, "cpx.im")});
      break;
    default:
      break; // array/struct operands are diagnosed by sema
    }
    flushCalls("pli_out_begin", "pli_out_end");
    // DISPLAY ... REPLY (rule 114): after the prompt is written, read one
    // reply line from the active input into the character variable.
    if (s->replyTarget) {
      HExpr* rt = s->replyTarget.get();
      const Type& rty = rt->ty;
      llvm::Value* buf =
          entryAlloca(llvm::ArrayType::get(b_.getInt8Ty(), (unsigned)rty.len), "reply");
      b_.CreateCall(runtimeFn("pli_get_reply"), {buf, i64(rty.len)});
      Val rv;
      rv.ptr = buf;
      rv.len = i64(rty.len);
      rv.ty = rty;
      storeGetTarget(rt, rv, s->loc);
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
  case HStmt::Rewrite:
    emitRecordRewrite(s);
    break;
  case HStmt::Delete:
    emitRecordDelete(s);
    break;
  case HStmt::Return: {
    // FINISH (rule 94; ADR-322): an early return is the task procedure's
    // exit too, so the condition is raised here before the handler state goes.
    emitFinishSignal();
    // Rule (91): procedure exit restores the entry ERROR depth, popping any
    // handlers this procedure established.
    if (curOnDepth_)
      b_.CreateCall(runtimeFn("pli_on_reset_error"),
                    {b_.CreateLoad(b_.getInt64Ty(), curOnDepth_, "ondepth")});
    // A RETURN inside an ON unit ends the unit's activation (ADR-350).
    if (curOnUnitDepth_)
      b_.CreateCall(runtimeFn("pli_on_reset_error"),
                    {b_.CreateLoad(b_.getInt64Ty(), curOnUnitDepth_, "onunitdepth")});
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
    // REVERT pops the unit this activation established for the condition and
    // ignores an enclosing activation's unit (rule (92); ADR-350).
    b_.CreateCall(runtimeFn("pli_on_revert"), {fileCondKeyValue(s), onBaseDepth()});
    break;
  case HStmt::Signal:
    emitSignal(s);
    break;
  case HStmt::Goto:
    // Rule (91) / Y33 block termination: a GO TO that leaves enclosing BEGIN
    // blocks reverts the handlers those blocks established, restoring the
    // depth saved at the outermost exited block's entry.
      if (s->gotoTarget) {
       Val v = emitExpr(s->gotoTarget.get());
       llvm::Value* addr = v.reg;
       if (!addr)
         addr = llvm::ConstantPointerNull::get(b_.getPtrTy());
       if (!addr->getType()->isPointerTy())
         addr = b_.CreateIntToPtr(addr, b_.getPtrTy(), "gotoptr");
       // Collect every block an indirect transfer can reach: the named labels,
       // the alternate entry segments, and the label-array prefixes (rule (65)).
       std::vector<llvm::BasicBlock*> dests;
       auto addDest = [&](llvm::BasicBlock* bb) {
         if (bb && std::find(dests.begin(), dests.end(), bb) == dests.end())
           dests.push_back(bb);
       };
       for (auto& kv : labelBlocks_)
         addDest(kv.second);
       for (auto& kv : stmtBlocks_)
         addDest(kv.second);
       llvm::IndirectBrInst* ib = b_.CreateIndirectBr(addr, (unsigned)dests.size());
       for (llvm::BasicBlock* d : dests)
         ib->addDestination(d);
    } else {
      // Direct GO TO label: jump to the named block.
      if (llvm::Value* depth = onExitDepth(s->name))
        b_.CreateCall(runtimeFn("pli_on_reset_error"), {depth});
      b_.CreateBr(labelBlocks_[s->name]);
    }
    break;
  }
  checkStack_.pop_back();
}

// Self-append peephole (rule (119), P4): X = X || L1 || ... || Ln where X is
// an AUTOMATIC scalar VARYING target and every tail leaf is a string
// literal or a safe variable. Appends each leaf in place past the live
// length, truncating at capacity exactly like the generic temp-buffer +
// assign path (which copies the temp's first min(cap, total) bytes back),
// then stores the new length. Skips the temp buffer (pli_alloc + double
// copy) and the blank-pad, whose extent is provably empty here: the live
// length only grows, so the new tail [n, cap) sits inside the already-blank
// old tail [old, cap). Literal globals cannot overlap X and carry no side
// effects; a variable tail is safe when it is a same-frame AUTOMATIC local
// (a disjoint frame alloca — never a parameter that could alias X, never
// BASED/CONTROLLED/STATIC/DEFINED storage) or a by-value parameter (a
// private stable cell, ADR-215), so in-place evaluation order is
// unobservable. Reads of X itself always span [0, old) while writes land
// at [old, ...), so X = X || X is safe too. Returns false otherwise.
bool IRGen::emitSelfAppend(HStmt* s) {
  if (!s->extraTargets.empty())
    return false;
  HExpr* t = s->target.get();
  HExpr* rhs = s->value.get();
  if (!t || !rhs || t->kind != HExpr::VarRef || !t->sym)
    return false;
  Symbol* sym = t->sym;
  if (!t->memberPath.empty() || t->locPtr || !sym->ty.isChar() || sym->ty.isArray() ||
      !sym->ty.varying || sym->ty.starLen)
    return false;
  // AUTOMATIC storage only: the frame alloca is stable and its tail starts
  // blank (allocaLocals blank-fills); CONTROLLED generations, STATICs, BASED
  // cells and DEFINED overlays keep the generic path and its blank repair.
  if (sym->isBased() || sym->controlled || sym->isStatic || sym->definedBase)
    return false;
  if (rhs->kind != HExpr::Binary || rhs->op != Tok::Concat)
    return false;
  // Flatten the spine (as the generic concat lowering does): leaves[0] must
  // be X itself, every other leaf a string literal.
  std::vector<HExpr*> order;
  std::vector<HExpr*> stack{rhs};
  while (!stack.empty()) {
    HExpr* ex = stack.back();
    stack.pop_back();
    if (ex->kind == HExpr::Binary && ex->op == Tok::Concat) {
      stack.push_back(ex->b.get());
      stack.push_back(ex->a.get());
      continue;
    }
    order.push_back(ex);
  }
  if (order.empty() || order[0]->kind != HExpr::VarRef || order[0]->sym != sym ||
      !order[0]->memberPath.empty() || order[0]->locPtr)
    return false;
  // A variable tail must read disjoint storage: a same-frame AUTOMATIC
  // local (its frame alloca cannot overlap X) or a by-value parameter (a
  // private stable cell, ADR-215). By-reference parameters could alias X
  // through the caller, and BASED/CONTROLLED/STATIC/DEFINED cells have no
  // such guarantee, so they keep the generic path.
  auto varTailOk = [&](HExpr* leaf) {
    if (leaf->kind != HExpr::VarRef || !leaf->sym || !leaf->memberPath.empty() || leaf->locPtr)
      return false;
    Symbol* ls = leaf->sym;
    if (!ls->ty.isChar() || ls->ty.isArray() || !ls->ty.varying || ls->ty.starLen)
      return false;
    if (ls->isBased() || ls->controlled || ls->isStatic || ls->definedBase)
      return false;
    if (isByValueParam(ls))
      return true;
    for (const HProc* q = curProc_; q; q = q->parent)
      if (std::find(q->paramSyms.begin(), q->paramSyms.end(), ls) != q->paramSyms.end())
        return false;
    return true;
  };
  for (size_t i = 1; i < order.size(); ++i) {
    if (order[i]->kind == HExpr::CharLit)
      continue;
    if (!varTailOk(order[i]))
      return false;
  }
  llvm::Value* stor = addressOf(sym);
  if (!stor)
    return false;
  llvm::Value* lp = b_.CreateStructGEP(llvmTy(sym->ty), stor, 0, "sa.vlenp");
  llvm::Value* dp = b_.CreateStructGEP(llvmTy(sym->ty), stor, 1, "sa.vdata");
  llvm::Value* old32 = b_.CreateLoad(b_.getInt32Ty(), lp, "sa.old");
  llvm::Value* off = b_.CreateZExt(old32, b_.getInt64Ty(), "sa.off");
  llvm::Value* cap = i64(sym->ty.len);
  // Clamp a corrupt live length into range so the remainder below cannot
  // underflow; every writer stores min(cap, total), so this never fires.
  off = b_.CreateSelect(b_.CreateICmpULT(off, cap, "sa.inrange"), off, cap, "sa.clamp");
  for (size_t i = 1; i < order.size(); ++i) {
    // One leaf's source bytes: a literal's global, or a variable tail's
    // live [0, len) window. Reads always precede the writes below in
    // program order per leaf, and every write lands at [off, ...) past all
    // previously-read bytes, so in-place order is unobservable.
    llvm::Value* srcp;
    llvm::Value* srclen;
    if (order[i]->kind == HExpr::CharLit) {
      const std::string& lit = order[i]->sval;
      if (lit.empty())
        continue;
      srcp = globalString(lit);
      srclen = i64((long long)lit.size());
    } else {
      Symbol* ls = order[i]->sym;
      llvm::Value* stor = addressOf(ls);
      if (!stor)
        return false;
      llvm::Value* lp =
          b_.CreateStructGEP(llvmTy(ls->ty), stor, 0, "sa.vlenp");
      llvm::Value* dp =
          b_.CreateStructGEP(llvmTy(ls->ty), stor, 1, "sa.vdata");
      srcp = dp;
      srclen = b_.CreateZExt(b_.CreateLoad(b_.getInt32Ty(), lp, "sa.vlen"), b_.getInt64Ty(),
                             "sa.vlen64");
    }
    llvm::Value* rem = b_.CreateSub(cap, off, "sa.rem");
    llvm::Value* take =
        b_.CreateSelect(b_.CreateICmpULT(srclen, rem, "sa.fits"), srclen, rem, "sa.take");
    llvm::Value* dstp = b_.CreateGEP(b_.getInt8Ty(), dp, {off}, "sa.dstp");
    b_.CreateMemMove(dstp, llvm::MaybeAlign(), srcp, llvm::MaybeAlign(), take, false);
    off = b_.CreateAdd(off, take, "sa.off");
  }
  b_.CreateStore(b_.CreateTrunc(off, b_.getInt32Ty(), "sa.len"), lp);
  return true;
}

// Rule (119): a concat RHS evaluates into a fresh pli_alloc buffer; once an
// assignment has copied it into the target the buffer is dead. Free it so
// repeated assignment in a loop does not grow RSS without bound (the -O2
// pipeline often removes the pair on its own, but -O0 must not OOM). The
// buffer is fresh per evaluation with no aliases, and every call site runs
// after the consuming store completes. Any other RHS shape is a no-op.
void IRGen::freeConcatTemp(HExpr* rhs, llvm::Value* buf) {
  if (rhs && rhs->kind == HExpr::Binary && rhs->op == Tok::Concat && buf)
    b_.CreateCall(runtimeFn("pli_free"), {buf});
}

// True when `s` is a parameter of the current procedure or of one of its
// enclosing procedures: such a symbol's storage is the caller's.
bool IRGen::isParamOfScope(Symbol* s) const {
  for (const HProc* q = curProc_; q; q = q->parent)
    if (std::find(q->paramSyms.begin(), q->paramSyms.end(), s) != q->paramSyms.end())
      return true;
  return false;
}

// Constant-offset concatenation store (rule (119), ADR-242): A = B || C
// where A is a fixed-length CHARACTER(n) and every leaf of the
// concatenation has a compile-time-constant length. The result is the
// leftmost n characters of the concatenation, blank-padded to n, so each
// leaf is stored straight into A at a constant offset — no temporary
// buffer, no length arithmetic, no allocation. Mirrors MULTICS
// cat_op.pl1:140-167, which folds a constant tail length into the
// target's constant offset.
//
// Safety: the leaves are read while A is written, so a leaf must read
// storage that cannot overlap A's. A leaf is therefore a literal or a
// whole variable that is not A, is not BASED/CONTROLLED/DEFINED storage,
// and is not a by-reference parameter (its storage is the caller's, and
// the caller may have passed A itself); a by-value parameter is a private
// cell (ADR-215). Two distinct variables of one frame have disjoint
// storage, and distinct frames are disjoint too. The target owns its
// storage for the same reason: a by-reference target keeps the generic
// path. Returns false when the shape does not qualify, and the caller
// then runs the generic concat-and-assign path.
bool IRGen::emitConcatConstStore(HStmt* s) {
  if (!s->extraTargets.empty())
    return false;
  HExpr* t = s->target.get();
  HExpr* rhs = s->value.get();
  if (!t || !rhs || t->kind != HExpr::VarRef || !t->sym)
    return false;
  if (!t->memberPath.empty() || t->locPtr)
    return false;
  Symbol* dst = t->sym;
  // The target must own a fixed-length character buffer of known size.
  if (!dst->ty.isChar() || dst->ty.varying || dst->ty.starLen || dst->ty.isArray())
    return false;
  if (dst->ty.len <= 0)
    return false;
  if (dst->isBased() || dst->controlled || dst->definedBase)
    return false;
  // A by-reference target's storage is the caller's, which may be the very
  // variable a leaf reads.
  if (isParamOfScope(dst) && !isByValueParam(dst))
    return false;
  if (rhs->kind != HExpr::Binary || rhs->op != Tok::Concat)
    return false;
  // Flatten the spine (as the generic concat lowering does): the leaves come
  // out in source order.
  std::vector<HExpr*> leaves;
  std::vector<HExpr*> stack{rhs};
  while (!stack.empty()) {
    HExpr* ex = stack.back();
    stack.pop_back();
    if (ex->kind == HExpr::Binary && ex->op == Tok::Concat) {
      stack.push_back(ex->b.get());
      stack.push_back(ex->a.get());
      continue;
    }
    leaves.push_back(ex);
  }
  struct LeafSrc {
    llvm::Value* ptr;
    long long len;
  };
  std::vector<LeafSrc> srcs;
  for (HExpr* leaf : leaves) {
    if (leaf->kind == HExpr::CharLit) {
      if (leaf->sval.empty())
        continue; // an empty literal contributes no bytes
      srcs.push_back({globalString(leaf->sval), (long long)leaf->sval.size()});
      continue;
    }
    if (leaf->kind != HExpr::VarRef || !leaf->sym)
      return false;
    Symbol* ls = leaf->sym;
    if (!leaf->memberPath.empty() || leaf->locPtr)
      return false;
    if (!ls->ty.isChar() || ls->ty.varying || ls->ty.starLen || ls->ty.isArray())
      return false;
    if (ls == dst)
      return false;
    if (ls->isBased() || ls->controlled || ls->definedBase)
      return false;
    // A by-reference parameter's storage is the caller's and could be the
    // target's; a by-value parameter is a private cell (ADR-215).
    if (isParamOfScope(ls) && !isByValueParam(ls))
      return false;
    llvm::Value* addr = addressOf(ls);
    if (!addr)
      return false;
    srcs.push_back({addr, ls->ty.len});
  }
  llvm::Value* dstAddr = addressOf(dst);
  if (!dstAddr)
    return false;
  // The lowering policy wins (ADR-260): with
  // --experimental-lowering=concat_const:runtime the generic path is kept,
  // so the differential oracle can compare the two lowerings.
  if (runtimePath("concat_const")) {
    facts_.recordBuiltin("concat_const", "runtime");
    return false;
  }
  facts_.recordBuiltin("concat_const", "llvm");
  // The store plan — how many bytes each leaf contributes and how many blanks
  // follow them — is a decision (the plan's C2), so it lives in
  // src/irgen_decisions.cpp.
  std::vector<long long> leafLen;
  leafLen.reserve(srcs.size());
  for (const LeafSrc& l : srcs)
    leafLen.push_back(l.len);
  pli::ConcatStorePlan plan = pli::planConcatStore(leafLen, dst->ty.len);
  if (!plan.ok)
    return false;
  // One store per leaf at a constant offset; the bytes past the
  // concatenation are blanks, exactly as the generic assign path leaves
  // them (rule (86)).
  long long off = 0;
  for (size_t i = 0; i < srcs.size(); ++i) {
    const long long take = plan.sizes[i];
    if (take > 0) {
      llvm::Value* at = b_.CreateGEP(b_.getInt8Ty(), dstAddr, {b_.getInt64(off)}, "cc.at");
      b_.CreateMemCpy(at, llvm::MaybeAlign(), srcs[i].ptr, llvm::MaybeAlign(),
                      llvm::ConstantInt::get(b_.getInt64Ty(), take));
      off += take;
    }
  }
  if (plan.pad > 0)
    b_.CreateMemSet(b_.CreateGEP(b_.getInt8Ty(), dstAddr, {b_.getInt64(off)}, "cc.pad"),
                    b_.getInt8(' '), llvm::ConstantInt::get(b_.getInt64Ty(), plan.pad),
                    llvm::MaybeAlign(), false);
  return true;
}

// Pack a BIT literal into the integer its packed bytes denote (big-endian,
// first character in the most significant bit). Widths above 64 are not
// handled here; the caller keeps the generic byte-array path for those.
static bool bitLitToU64(const std::string& bits, int nbits, uint64_t& out) {
  if (nbits <= 0 || nbits > 64 || (int)bits.size() != nbits)
    return false;
  uint64_t v = 0;
  for (int i = 0; i < nbits; ++i)
    v = (v << 1) | (bits[i] == '1' ? 1u : 0u);
  out = v;
  return true;
}

// Merge two masked bit tests on the same value into one masked test
// (ADR-243): `(X & M1) = E1 & (X & M2) = E2` is `(X & (M1|M2)) = (E1|E2)`,
// when the masks are disjoint and each expectation lies inside its own
// mask. Mirrors MULTICS combine_tests.pl1:159-300, which requires the two
// tests to read the same storage word of the same top-level object.
//
// The win is one masked test instead of two: the value is loaded once, the
// mask is applied once, and the branch is one. LLVM cannot do this itself,
// because PL/I's bit strings are bit-granular inside a byte array, so the
// masks only exist at the PL/I level.
//
// The decision — whether the two tests may be merged, and what the merged
// mask and expectation are — is pli::mergeBitTests (the plan's C2); what
// stays here is the matching of the expression's shape and the emission.
bool IRGen::emitCombinedBitTest(HExpr* e, Val& out) {
  // One side of the conjunction: (X & M) = E, with X the tested value, M a
  // constant mask and E the constant expectation.
  struct Side {
    HExpr* var = nullptr;
    std::string mask;
    std::string want;
    int nbits = 0;
  };
  auto parseSide = [](HExpr* t, Side& s) -> bool {
    if (!t || t->kind != HExpr::Binary || t->op != Tok::Eq)
      return false;
    HExpr* masked = t->a.get();
    HExpr* want = t->b.get();
    if (!masked || masked->kind != HExpr::Binary || masked->op != Tok::Amp)
      return false;
    if (!want || want->kind != HExpr::BitLit)
      return false;
    HExpr* var = masked->a.get();
    HExpr* mask = masked->b.get();
    if (!var || var->kind != HExpr::VarRef || !var->sym)
      return false;
    if (!mask || mask->kind != HExpr::BitLit)
      return false;
    // A one-bit value's `&` is a logical conjunction, not a mask.
    if (!var->ty.isBit() || var->ty.len <= 1)
      return false;
    // The mask and the expectation must have the tested value's length, so
    // they select the same bits.
    if (mask->ty.len != var->ty.len || want->ty.len != var->ty.len)
      return false;
    if ((int)mask->sval.size() != var->ty.len || (int)want->sval.size() != var->ty.len)
      return false;
    s.var = var;
    s.mask = mask->sval;
    s.want = want->sval;
    s.nbits = var->ty.len;
    return true;
  };
  Side s1, s2;
  if (!parseSide(e->a.get(), s1) || !parseSide(e->b.get(), s2))
    return false;
  // The two tests must read the same value (MULTICS's `same_word`: the same
  // storage object of the same top-level symbol). A locator-qualified
  // reference reads through a runtime pointer, and two different locators are
  // different storage, so only a plain reference qualifies.
  if (s1.var->sym != s2.var->sym || s1.var->memberPath != s2.var->memberPath || s1.var->locPtr ||
      s2.var->locPtr)
    return false;
  if (s1.nbits != s2.nbits)
    return false;
  // The merge itself: disjoint masks, each expectation inside its mask.
  pli::BitTestMerge merge = pli::mergeBitTests(s1.mask, s1.want, s2.mask, s2.want);
  if (!merge.ok)
    return false;
  // The lowering policy wins (ADR-260): with
  // --experimental-lowering=bit_merge:runtime the two tests stay separate,
  // so the differential oracle can compare the two lowerings.
  if (runtimePath("bit_merge")) {
    facts_.recordBuiltin("bit_merge", "runtime");
    return false;
  }
  facts_.recordBuiltin("bit_merge", "llvm");
  // One load of the value, masked with the merged constant mask. The packed
  // value is a bijection on n-bit strings, so pack(X & M) is pack(X) & M:
  // one integer AND and one compare replace the per-byte chain and preserve
  // every bit, including those above bit 31.
  Val x = emitExpr(s1.var);
  if (!x.ty.isBit() || x.ty.len != s1.nbits || !x.reg)
    return false;
  uint64_t maskVal = 0, wantVal = 0;
  if (!bitLitToU64(merge.mask, s1.nbits, maskVal) ||
      !bitLitToU64(merge.want, s1.nbits, wantVal))
    return false;
  // The packed form needs the optimizer to pay off; -O0 keeps the byte-array
  // construction, which is fewer instructions unoptimized.
  if (!optimize_) {
    const int B = bitBytes(s1.nbits);
    std::vector<unsigned char> maskBytes = packBitLiteral(merge.mask, s1.nbits);
    std::vector<unsigned char> wantBytes = packBitLiteral(merge.want, s1.nbits);
    llvm::Type* arrTy = llvm::ArrayType::get(b_.getInt8Ty(), (unsigned)B);
    llvm::Value* masked = llvm::UndefValue::get(arrTy);
    for (int k = 0; k < B; ++k) {
      llvm::Value* xb = b_.CreateExtractValue(x.reg, (unsigned)k, "bt.x");
      llvm::Value* mb = llvm::ConstantInt::get(b_.getInt8Ty(), maskBytes[k]);
      masked = b_.CreateInsertValue(masked, b_.CreateAnd(xb, mb, "bt.and"), (unsigned)k);
    }
    Val mv;
    mv.ty = Type::bit(s1.nbits);
    mv.reg = masked;
    Val wv;
    wv.ty = Type::bit(s1.nbits);
    wv.reg = llvm::ConstantDataArray::get(ctx_, wantBytes);
    llvm::Value* mp = packBitValue(masked, s1.nbits, e->loc);
    llvm::Value* wp = packBitValue(wv.reg, s1.nbits, e->loc);
    out.ty = Type::bit(1);
    out.reg = b_.CreateICmpEQ(mp, wp, "bt.cmp");
    return true;
  }
  llvm::Value* xp = packBitValue(x.reg, s1.nbits, e->loc);
  llvm::Value* maskedInt;
  llvm::Value* wantInt;
  if (s1.nbits <= 32) {
    xp = b_.CreateTrunc(xp, b_.getInt32Ty(), "bt.x32");
    maskedInt = b_.CreateAnd(xp, b_.getInt32((int)maskVal), "bt.and32");
    wantInt = b_.getInt32((int)wantVal);
  } else {
    maskedInt = b_.CreateAnd(xp, i64((long long)maskVal), "bt.and");
    wantInt = i64((long long)wantVal);
  }
  out.ty = Type::bit(1);
  out.reg = b_.CreateICmpEQ(maskedInt, wantInt, "bt.cmp");
  return true;
}

// Lower one masked bit test, (X & M) = E, on BIT(n<=64) as a packed-integer
// compare. The packed value is a bijection on n-bit strings, so
// pack(X & M) = pack(X) & pack(M): one integer AND and one compare replace
// the byte-array AND and the per-byte equality chain. The generic byte-array
// lowering stays the fallback (and the differential-oracle path).
bool IRGen::emitMaskedBitTest(HExpr* e, Val& out) {
  const Tok op = e->op;
  if (op != Tok::Eq && op != Tok::Ne)
    return false;
  // One side is the masked value, the other a BIT literal expectation.
  HExpr* masked = nullptr;
  HExpr* want = nullptr;
  if (e->a && e->a->kind == HExpr::Binary && e->a->op == Tok::Amp &&
      e->b && e->b->kind == HExpr::BitLit) {
    masked = e->a.get();
    want = e->b.get();
  } else if (e->b && e->b->kind == HExpr::Binary && e->b->op == Tok::Amp &&
             e->a && e->a->kind == HExpr::BitLit) {
    masked = e->b.get();
    want = e->a.get();
  } else {
    return false;
  }
  // The mask is the literal operand of the AND; the other is the tested value.
  HExpr* value = masked->a.get();
  HExpr* mask = masked->b.get();
  if (!mask || mask->kind != HExpr::BitLit) {
    value = masked->b.get();
    mask = masked->a.get();
  }
  if (!value || !mask || mask->kind != HExpr::BitLit)
    return false;
  if (!value->ty.isBit() || value->ty.len < 2 || value->ty.len > 64)
    return false;
  const int nbits = value->ty.len;
  // The mask, the expectation and the value must all be the same bit string.
  if (mask->ty.len != nbits || want->ty.len != nbits)
    return false;
  uint64_t maskVal = 0, wantVal = 0;
  if (!bitLitToU64(mask->sval, nbits, maskVal) ||
      !bitLitToU64(want->sval, nbits, wantVal))
    return false;
  // The lowering policy wins (ADR-260): the runtime path keeps the generic
  // byte-array lowering so the differential oracle can compare the two.
  // -O0 also keeps it: packing is extra work without the optimizer.
  if (!optimize_)
    return false;
  if (runtimePath("bit_test")) {
    facts_.recordBuiltin("bit_test", "runtime");
    return false;
  }
  facts_.recordBuiltin("bit_test", "llvm");
  // Evaluate the tested value once, pack it, then one AND and one compare.
  Val x = emitExpr(value);
  if (!x.ty.isBit() || x.ty.len != nbits || !x.reg)
    return false;
  llvm::Value* xp = packBitValue(x.reg, nbits, e->loc);
  llvm::Value* maskedInt;
  llvm::Value* wantInt;
  if (nbits <= 32) {
    // A 32-bit string stays in i32: the same lane width a C uint32_t uses.
    xp = b_.CreateTrunc(xp, b_.getInt32Ty(), "bt.x32");
    maskedInt = b_.CreateAnd(xp, b_.getInt32((int)maskVal), "bt.and32");
    wantInt = b_.getInt32((int)wantVal);
  } else {
    maskedInt = b_.CreateAnd(xp, i64((long long)maskVal), "bt.and");
    wantInt = i64((long long)wantVal);
  }
  llvm::Value* eq = b_.CreateICmpEQ(maskedInt, wantInt, "bt.eq");
  out.ty = Type::bit(1);
  out.reg = op == Tok::Ne ? b_.CreateNot(eq, "bt.ne") : eq;
  return true;
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
          // A locator-qualified member P->S.A(i) (rules 124,126) stores
          // through the locator value, not the declared BASED pointer.
          llvm::Value* base =
              t->locPtr
                  ? locatorMemberAddr(t->sym, t->memberPath, emitExpr(t->locPtr.get()).reg)
                  : (arr.isDynamic()
                         ? dynamicMemberBase(t->sym, t->memberPath, s->loc, ub, lb)
                         : memberAddr(t->sym, t->memberPath, s->loc));
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
    // Every store above copied the RHS; a concat temp is dead now.
    freeConcatTemp(s->value.get(), v.ptr);
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
      freeConcatTemp(s->value.get(), rhs.ptr);
      return;
    }
    emitSubstrAssign(sv.ptr, cap, toI64(start), toI64(len), rhs.ptr, rhs.len);
    freeConcatTemp(s->value.get(), rhs.ptr);
    return;
  }
  // EXPONENT pseudo-variable (rule (123)): exponent(fvar) = n decomposes fvar
  // via llvm.frexp, replaces the exponent with n, and stores the result back.
  if (s->target->kind == HExpr::Call && s->target->name == "EXPONENT") {
    HExpr* t = s->target.get();
    HExpr* base = t->args[0].get();
    Symbol* sym = base->sym;
    llvm::Value* addr = addressOf(sym);
    llvm::Value* oldVal = b_.CreateLoad(b_.getDoubleTy(), addr, "exp_old");
    Val rhs = emitExpr(s->value.get());
    llvm::Value* newExp = toI64(rhs);
    llvm::StructType* st = llvm::StructType::get(ctx_, {b_.getDoubleTy(), b_.getInt32Ty()});
    llvm::Value* pair = b_.CreateCall(
        intrinsicFn("llvm.frexp.f64.i32", st, {b_.getDoubleTy()}),
        {oldVal}, "exp_decomp");
    llvm::Value* mant = b_.CreateExtractValue(pair, 0, "mant");
    llvm::Value* newExpI32 = b_.CreateTrunc(newExp, b_.getInt32Ty(), "exp_i32");
    llvm::Value* newVal =
        b_.CreateCall(intrinsicFn("llvm.ldexp.f64.i32", b_.getDoubleTy(), {b_.getDoubleTy(), b_.getInt32Ty()}),
                      {mant, newExpI32}, "exp_new");
    b_.CreateStore(newVal, addr);
    return;
  }
  // PAGENO pseudo-variable (TR 123, ADR-201): pageno(f) = n calls the runtime
  // setter to update the per-file page counter.
  if (s->target->kind == HExpr::Call && s->target->name == "PAGENO") {
    HExpr* t = s->target.get();
    Val rhs = emitExpr(s->value.get());
    Val slot = emitExpr(t->args[0].get());
    b_.CreateCall(runtimeFn("pli_set_pageno"), {toI64(slot), toI64(rhs)});
    return;
  }
  // PRIORITY pseudo-variable (rule 79, ADR-298): priority(t) = n calls the
  // runtime setter; the value is relative to self.
  if (s->target->kind == HExpr::Call && s->target->name == "PRIORITY") {
    HExpr* t = s->target.get();
    HExpr* a = t->args[0].get();
    Val rhs = emitExpr(s->value.get());
    llvm::Value* idVal = i64(0);
    /* The handle cell may be an array element or a member, not just the
     * symbol's base address (rules (79),(82)). */
    if (llvm::Value* idAddr = taskEventAddr(a, s->loc)) {
      llvm::Value* id32 = b_.CreateLoad(b_.getInt32Ty(), idAddr, "tid");
      idVal = b_.CreateSExt(id32, b_.getInt64Ty(), "tid64");
    }
    b_.CreateCall(runtimeFn("pli_task_set_priority_of"),
                  {idVal, b_.CreateTrunc(toI64(rhs), b_.getInt32Ty(), "p32")});
    freeConcatTemp(s->value.get(), {});
    return;
  }
  // ONSOURCE pseudo-variable (TR 123, ADR-210): onsource = key calls the runtime
  // setter to update the source condition key of the current SIGNAL handler.
  if (s->target->kind == HExpr::VarRef && s->target->name == "ONSOURCE") {
    Val rhs = emitExpr(s->value.get());
    b_.CreateCall(runtimeFn("pli_set_on_source"), {b_.CreateTrunc(toI64(rhs), b_.getInt32Ty(), "src32")});
    return;
  }
  // UNSPEC pseudo-variable (rule (123), ADR-196): UNSPEC(var) = bits copies the
  // RHS bit view into the raw storage of var.
  if (s->target->kind == HExpr::Call && s->target->name == "UNSPEC") {
    HExpr* t = s->target.get();
    HExpr* var = t->args[0].get();
    Val rhs = emitExpr(s->value.get());
    if (var->kind == HExpr::VarRef && var->sym) {
      llvm::Value* addr = addressOf(var->sym);
      unsigned dstSz = (unsigned)mod_.getDataLayout().getTypeStoreSize(llvmTy(var->sym->ty)).getFixedValue();
      unsigned srcSz = (unsigned)mod_.getDataLayout().getTypeStoreSize(rhs.reg->getType()).getFixedValue();
      // Store RHS in a temp so we can pass a pointer to pli_unspec_assign.
      llvm::Value* tmp = b_.CreateAlloca(rhs.reg->getType(), nullptr, "unspec_rhs");
      b_.CreateStore(rhs.reg, tmp);
      b_.CreateCall(runtimeFn("pli_unspec_assign"), {addr, i64(dstSz), tmp, i64(srcSz)});
    }
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
        freeConcatTemp(s->value.get(), v.ptr);
        return;
      }
      // A subscripted member array S.A(i) = e (rules 124,126): store through
      // the member array field, bounds-checked like any array element.
      const Type& arr = memberType(t->sym, t->memberPath);
      const Type& el = t->ty;
      llvm::Value *ub = nullptr, *lb = nullptr;
      // A locator-qualified member P->S.A(i) (rules 124,126) stores through
      // the locator value, not the declared BASED pointer.
      llvm::Value* base =
          t->locPtr
              ? locatorMemberAddr(t->sym, t->memberPath, emitExpr(t->locPtr.get()).reg)
              : (arr.isDynamic() ? dynamicMemberBase(t->sym, t->memberPath, s->loc, ub, lb)
                                 : memberAddr(t->sym, t->memberPath, s->loc));
      llvm::Value* addr = arrayElementAddr(arr, base, t->args, s->loc, ub, lb);
      bool packed = packedMemberPath(t->sym->ty, t->memberPath);
      if (el.isChar())
        storeCharTo(addr, el, v, s->loc, packed);
      else
        storeScalarTo(addr, el, convert(v, el, s->loc), packed);
      freeConcatTemp(s->value.get(), v.ptr);
      return;
    }
    // An iSUB-DEFINED array target Y(k) (rule 134) has no storage of its own:
    // store into the live base element X(...k...).
    if (t->sym->definedBase && t->sym->hasIsubOverlay()) {
      llvm::Value* addr = definedSubElementAddr(t->sym, t->args, s->loc);
      storeScalarTo(addr, t->ty, convert(v, t->ty, s->loc));
      freeConcatTemp(s->value.get(), v.ptr);
      return;
    }
    storeArrayElement(t->sym, t->args, v, s->loc, t->locPtr.get());
    freeConcatTemp(s->value.get(), v.ptr);
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
       addr = structMemberGEP(*cur, addr, f, "dcp.f");
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
      emitCondTrap(Stmt::kSubscriptrangeCondKey, "pli_subscript_oob", abL);
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
    freeConcatTemp(s->value.get(), v.ptr);
    return;
  }
  if (s->target->kind != HExpr::VarRef || !s->target->sym)
    return;
  // Self-append peephole first: X = X || lit... appends in place when the
  // target qualifies; anything else falls through to generic concat+assign.
  if (emitSelfAppend(s))
    return;
  // Constant-offset concatenation store: A = B || C into a fixed-length
  // target stores each leaf at a constant offset, with no temporary buffer
  // (ADR-242).
  if (emitConcatConstStore(s))
    return;
  Val v = emitExpr(s->value.get());
  storeTo(s->target->sym, v, s->loc);
  freeConcatTemp(s->value.get(), v.ptr);
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
    llvm::Value* addr = b_.CreateCall(runtimeFn("pli_ctl_alloc"), {ctlKeyOf(sym), total});
    ctlNoteAlloc(sym, addr);
  } else {
    llvm::Value* addr = b_.CreateCall(runtimeFn("pli_ctl_alloc_dims"),
                                      {ctlKeyOf(sym), total, i64((long long)rank)});
    ctlNoteAlloc(sym, addr);
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
    llvm::Value* addr =
        b_.CreateCall(runtimeFn("pli_ctl_alloc_dims"), {ctlKeyOf(sym), sz, i64((long long)rank)});
    ctlNoteAlloc(sym, addr);
    emitCtlNDDescDims(sym);
  } else {
    llvm::Value* addr = b_.CreateCall(runtimeFn("pli_ctl_alloc"), {ctlKeyOf(sym), sz});
    ctlNoteAlloc(sym, addr);
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
  // Destroy the procedure's AUTOMATIC AREA regions (rule (20)), one per slot.
  // Any blocks still allocated in them are released with the region.
  for (llvm::Value* slot : areaSlots_)
    b_.CreateCall(runtimeFn("pli_area_destroy"), {b_.CreateLoad(b_.getPtrTy(), slot)});
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
    const auto& bbounds = (i < s->allocBounds.size()) ? s->allocBounds[i] : noBounds;
    if (!bsym->controlled && bsym->ty.isArray() &&
        (bsym->ty.isDynamic() || !bbounds.empty())) {
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
        // A per-ALLOCATE extent overrides the DECLARE bound (rule (89)); sema
        // admits a single bound on a 1-D BASED array.
        llvm::Value* ub = i64(d.ub);
        if (k == 0 && !bbounds.empty() && bbounds[0].ub)
          ub = toI64(emitExpr(bbounds[0].ub.get()), s->loc);
        else if (k == 0 && bsym->dynUb)
          ub = toI64(emitExpr(bsym->dynUb), s->loc);
        llvm::Value* lb =
            (k == 0 && d.lbDyn && bsym->dynLb) ? toI64(emitExpr(bsym->dynLb), s->loc) : i64(d.lb);
        if (k == 0) {
          // Later references check against the ALLOCATE-time extent, which
          // may differ from the entry-evaluated one when the bound changed.
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
      llvm::Value* baddr = b_.CreateCall(runtimeFn("pli_ctl_alloc"), {ctlKeyOf(bsym), sz});
      ctlNoteAlloc(bsym, baddr);
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
    // A resumed AREA condition leaves the allocation undone (ADR-323): the
    // region returns null, and neither the SET locator nor the based
    // variable's own locator is overwritten.
    llvm::BasicBlock* storesDone = nullptr;
    if (areaE) {
      llvm::Value* refused =
          b_.CreateICmpEQ(p, llvm::ConstantPointerNull::get(b_.getPtrTy()), "arefused");
      llvm::BasicBlock* okBB = llvm::BasicBlock::Create(ctx_, "aalloc", curFn_);
      storesDone = llvm::BasicBlock::Create(ctx_, "aalloc.done", curFn_);
      b_.CreateCondBr(refused, storesDone, okBB);
      b_.SetInsertPoint(okBB);
    }
    // The SET target is any assignable locator reference — a scalar, an array
    // element (rule (22) arrays of OFFSET), or a member — so share the
    // reference store used by GET.
    if (set)
      storeGetTarget(set, Val{set->ty, p}, set->loc);
    // ALLOCATE of a BASED variable also sets the variable's own BASED locator
    // (rule (88)), so references through the based name address the new cell.
    if (bsym->basedBase && (!set || bsym->basedBase != set->sym))
      b_.CreateStore(p, addressOf(bsym->basedBase));
    if (storesDone) {
      b_.CreateBr(storesDone);
      b_.SetInsertPoint(storesDone);
    }
  }
}

// FREE (rule 90): release the heap storage addressed by a based pointer — the
// explicit locator when given, else the based variable's own BASED pointer —
// or pop a CONTROLLED generation.
void IRGen::emitFree(HStmt* s) {
  for (size_t i = 0; i < s->freeBase.size(); ++i) {
    Symbol* bsym = s->freeBase[i]->sym;
    if (bsym && bsym->controlled) {
      llvm::Value* top = b_.CreateCall(runtimeFn("pli_ctl_free_addr"), {ctlKeyOf(bsym)},
                                      "ctlafterfree");
      ctlNoteFree(bsym, top);
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

// A10: FILE identity across separately compiled units. The marker global's
// address is the identity; an EXTERNAL file's marker is a common symbol so
// every unit links the same address, while a private file gets its own.
llvm::GlobalVariable* IRGen::fileMarker(Symbol* f) {
  auto it = fileMarkers_.find(f);
  if (it != fileMarkers_.end())
    return it->second;
  bool ext = f->external;
  std::string extName = f->irName.size() > 1 ? f->irName.substr(1) : f->name;
  std::string name = ext ? "pli_file_marker_" + extName
                         : "pli_file_marker_" + std::to_string(n_++);
  auto* gv = new llvm::GlobalVariable(
      mod_, b_.getInt8Ty(), false,
      ext ? llvm::GlobalValue::CommonLinkage : llvm::GlobalValue::InternalLinkage,
      llvm::Constant::getNullValue(b_.getInt8Ty()), name);
  fileMarkers_[f] = gv;
  return gv;
}

// A10: resolve the file's process-wide slot on first use. The runtime caches
// marker address -> slot, so every statement naming this FILE variable and
// both sides of its condition key agree, whatever unit emitted them.
llvm::Value* IRGen::fileSlotValue(Symbol* f) {
  llvm::Value* key = b_.CreateBitCast(fileMarker(f), b_.getPtrTy(), "fkey");
  return b_.CreateCall(runtimeFn("pli_file_slot"), {key}, "fslot");
}

// A10: ON ENDFILE(f)/UNDEFINEDFILE(f) key = base - runtime slot.
llvm::Value* IRGen::fileCondKeyValue(HStmt* s) {
  long long base = 0;
  if (s->condName == "UNDEFINEDFILE")
    base = Stmt::kUndefinedfileBaseCondKey;
  else if (s->condName == "ENDFILE")
    base = Stmt::kEndfileBaseCondKey;
  else
    return i64(s->condKey);
  if (!s->fileSym)
    return i64(s->condKey);
  return b_.CreateSub(i64(base), fileSlotValue(s->fileSym), "fcondkey");
}

// OPEN (rules 100,101): open the FILE variable's slot against the TITLE name.
// The slot is a compile-time constant on the symbol; mode 0 = INPUT, 1 =
// OUTPUT, 2 = UPDATE. A RECORD SEQUENTIAL open (rules (101),(112)) uses the
// binary record runtime.
void IRGen::emitOpen(HStmt* s) {
  Symbol* f = s->fileSym;
  llvm::Value* name = globalString(s->openTitle);
  long long mode = s->openUpdate ? 2 : (s->openInput ? 0 : 1);
   b_.CreateCall(
       runtimeFn(s->openRecord ? "pli_file_open_record" : "pli_file_open"),
       {fileSlotValue(f), name, i64((long long)s->openTitle.size()), i64(mode),
        i64(f->fileNonprint ? 1 : 0)});
}

// WRITE (rules (112),(113)): append one fixed-size binary record — FIXED as 8
// bytes, FLOAT as 8 bytes, BIT(1) as 1 byte, CHAR(n) as n raw bytes.
void IRGen::emitRecordWrite(HStmt* s) {
  llvm::Value* slot = fileSlotValue(s->fileSym);
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

// REWRITE (rule (112)): replace the record most recently read or written.
// The runtime checks that a current record exists and that the new image has
// the same size, then writes it in place.
void IRGen::emitRecordRewrite(HStmt* s) {
  llvm::Value* slot = fileSlotValue(s->fileSym);
  Val v = emitExpr(s->value.get());
  switch (v.ty.k) {
  case TK::FixedBin:
  case TK::FixedDec:
    b_.CreateCall(runtimeFn("pli_record_rewrite_fixed"), {slot, toI64(v)});
    break;
  case TK::Float:
    b_.CreateCall(runtimeFn("pli_record_rewrite_float"), {slot, v.reg});
    break;
  case TK::Bit: {
    llvm::Value* bit = b_.CreateZExt(v.reg, b_.getInt8Ty(), "rbit");
    b_.CreateCall(runtimeFn("pli_record_rewrite_bit"), {slot, bit});
    break;
  }
  case TK::Char:
    b_.CreateCall(runtimeFn("pli_record_rewrite_char"), {slot, v.ptr, v.len});
    break;
  default:
    break; // other types are diagnosed by sema
  }
}

// DELETE (rule (112)): remove the record most recently read, shifting the
// rest of the file down so the next READ sees the following record.
void IRGen::emitRecordDelete(HStmt* s) {
  b_.CreateCall(runtimeFn("pli_record_delete"), {fileSlotValue(s->fileSym)});
}

// READ (rules (112),(113)): consume one fixed-size binary record into a plain
// scalar variable; a short read (EOF) raises ERROR in the runtime, since ON
// ENDFILE stays diagnosed.
void IRGen::emitRecordRead(HStmt* s) {
  llvm::Value* slot = fileSlotValue(s->fileSym);
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
  b_.CreateCall(runtimeFn("pli_file_close"), {fileSlotValue(s->fileSym)});
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
//
// The hoist is the compiler's answer to the MULTICS save_temps contract
// (ADR-269): a value cached across a control-flow edge is sound here only
// because the analysis proved the generation cannot change inside the loop —
// no call, no ALLOCATE, no FREE. A transformation that adds a call to the
// body must drop the hoist.
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
    if (sym->ctlDirect && !inHandler_ && curProc_ && curProc_->src == sym->owner) {
      // WS-K8 (ADR-257): the generation is in the compiler's slot, so the
      // hoisted read is a load and the length is the declared extent (a
      // statically bounded CONTROLLED array's bounds check uses it anyway).
      ctlAddrHoist_[sym] = b_.CreateLoad(b_.getPtrTy(), ctlAnchorSlot(sym), "ctlhoist.a");
      ctlLenHoist_[sym] = i64(ctlDeclaredExtent(sym));
    } else {
      ctlAddrHoist_[sym] = b_.CreateCall(runtimeFn("pli_ctl_addr"), {ctlKeyOf(sym)}, "ctlhoist.a");
      ctlLenHoist_[sym] = b_.CreateCall(runtimeFn("pli_ctl_len"), {ctlKeyOf(sym)}, "ctlhoist.l");
    }
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

  // P6.3: track DO control vars whose FROM is a non-negative constant and BY
  // is ascending (absent or positive) — the index stays >= FROM >= 0, so MOD
  // (index, k) with positive k can skip the sign-adjust chain. FROM is checked
  // against the emitted constant (sema may promote the literal to a TypeTag
  // HExpr for a wider control var, so HExpr::IntLit alone is unreliable).
  bool nonNegDo = false;
  if (!s->byNegConst && from.reg &&
      llvm::isa<llvm::ConstantInt>(from.reg) &&
      !llvm::cast<llvm::ConstantInt>(from.reg)->getValue().isNegative())
    nonNegDo = true;

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
  llvm::BasicBlock* upL = nullptr;
  llvm::BasicBlock* downL = nullptr;
  llvm::BasicBlock* testL =
      s->to ? llvm::BasicBlock::Create(ctx_, "do.test." + id, curFn_) : nullptr;


  // P3c+P3.3: emit a preheader block before the loop header (condL). condL is the
  // loop header (target of the backedge from stepL), so the preheader runs once
  // before the first iteration — the right place for trip-count assumes and loop
  // metadata setup.
  llvm::BasicBlock* preL =
      llvm::BasicBlock::Create(ctx_, "do.pre." + id, curFn_);
  branch(preL);
  startBlock(preL);

  // P3c: llvm.assume trip-count hint when FROM, TO, and BY are all compile-time
  // constants — gives the optimizer an explicit trip count instead of forcing it
  // to infer from phi+icmp. Sound because PL/I DO semantics are defined: BY=0
  // is diagnosed in sema (rule (86)).
  if (s->from && s->to &&
      s->from->kind == HExpr::IntLit && s->to->kind == HExpr::IntLit &&
      llvm::isa<llvm::ConstantInt>(by.reg)) {
    int64_t fromVal = s->from->ival;
    int64_t toVal = s->to->ival;
    int64_t byVal = llvm::cast<llvm::ConstantInt>(by.reg)->getSExtValue();
    int64_t maxTrip = 0;
    if (byVal > 0 && toVal >= fromVal)
      maxTrip = (toVal - fromVal) / byVal + 1;
    else if (byVal < 0 && fromVal >= toVal)
      maxTrip = (fromVal - toVal) / (-byVal) + 1;
    if (maxTrip > 0) {
      // intent: trip = ceil(|to-from| / |by|) + 1, computed from LLVM values;
      // constant-folded for const bounds, but the @llvm.assume still records
      // the trip-count bound for passes like LoopVectorize.
      llvm::Value* span64 = b_.CreateSub(
          b_.CreateSExt(tv.reg, b_.getInt64Ty(), "trip.tv64"),
          b_.CreateSExt(from.reg, b_.getInt64Ty(), "trip.from64"),
          "trip.span");
      // The step magnitude is |by| — the sign of BY, not of the span. The
      // select arms used to be swapped, which made the step always negative
      // and the assumption trivially true (trip = 1) instead of tight.
      llvm::Value* by64 = b_.CreateSExt(by.reg, b_.getInt64Ty(), "trip.by64");
      llvm::Value* absStep = b_.CreateSelect(
          b_.CreateICmpSGE(by64, b_.getInt64(0), "step.is"), by64,
          b_.CreateNeg(by64, "step.abs"), "trip.step");
      llvm::Value* absSpan =
          b_.CreateSelect(b_.CreateICmpSGE(span64, b_.getInt64(0), "span.pos"),
                           span64, b_.CreateNeg(span64, "span.neg"), "trip.absspan");
      // trip = floor(|to - from| / |by|) + 1 — the number of times the DO
      // runs, matching the maxTrip computed above. A ceiling here would
      // overshoot by one and the assumption would be false.
      llvm::Value* q = b_.CreateUDiv(absSpan, absStep, "trip.q");
      llvm::Value* trip = b_.CreateAdd(q, b_.getInt64(1), "trip");
      b_.CreateAssumption(b_.CreateICmpULE(trip, b_.getInt64(maxTrip)));
    }
  }

  // P3.3: build loop ID metadata for mustprogress; parallel_access added on the
  // backedge only when P2 proved the body disjoint (see aliasScopeOf_).
  // The self-referential node is completed and attached at the backedge.
  llvm::MDBuilder mdb(ctx_);
  llvm::SmallVector<llvm::Metadata*, 4> loopProps;
  loopProps.push_back(mdb.createString("llvm.loop.mustprogress"));
  loopID_ = llvm::MDNode::getDistinct(ctx_, {});
  loopMDs_ = loopProps;

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
    // P1.1: collapse the runtime sign test into one canonical branch when BY
    // is a compile-time integer constant (omitted BY defaults to +1).  Float
    // and variable-BY DO loops keep the sign-test path below.
    bool oneBranch = ct.k != TK::Float && llvm::isa<llvm::ConstantInt>(by.reg);
    if (oneBranch) {
      bool descending = s->byNegConst ||
                          llvm::cast<llvm::ConstantInt>(by.reg)->isNegative();
      llvm::Value* cmp = descending
                             ? b_.CreateICmpSGE(cv.reg, tv.reg, "docmp")
                             : b_.CreateICmpSLE(cv.reg, tv.reg, "docmp");
      b_.CreateCondBr(cmp, testL, endL);
      startBlock(testL);
    } else {
      // Float or variable-BY: runtime sign test + up/down branches.
      upL = llvm::BasicBlock::Create(ctx_, "do.up." + id, curFn_);
      downL = llvm::BasicBlock::Create(ctx_, "do.down." + id, curFn_);
      llvm::Value* bv = by.reg;
      llvm::Value* zero =
          ct.k == TK::Float ? flt(0.0) : llvm::Constant::getNullValue(llvmTy(ct));
      llvm::Value* neg =
          ct.k == TK::Float ? b_.CreateFCmpOLT(bv, zero, "negstep")
                            : b_.CreateICmpSLT(bv, zero, "negstep");
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
  }

  if (s->cond) { // WHILE clause
    Val w = emitExpr(s->cond.get());
    b_.CreateCondBr(toI1(w, s->loc), bodyL, endL);
  } else {
    branch(bodyL);
  }

  if (nonNegDo)
    nonNegDoIndices_.insert(ctl);
  startBlock(bodyL);
  loopStack_.push_back({s->labels, endL, stepL});
  for (auto& b : s->body)
    emitStmt(b.get());
  loopStack_.pop_back();
  if (nonNegDo)
    nonNegDoIndices_.erase(ctl);
  branch(stepL);

  startBlock(stepL);
  Val cv = loadSym(ctl, ct);
  llvm::Value* bv = by.reg;
  llvm::Value* nx;
  if (ct.k == TK::Float) {
    nx = b_.CreateFAdd(cv.reg, bv, "next");
  } else if (!sizeChecks() && !subChecks()) {
    // P3.1: overflow checks are off, so the step cannot trap; mark nsw to
    // unlock indvars/LSR closed forms like C loop counters (rule (86) DO).
    nx = b_.CreateNSWAdd(cv.reg, bv, "next");
  } else {
    nx = b_.CreateAdd(cv.reg, bv, "next");
  }
  Val nv;
  nv.ty = ct;
  nv.reg = nx;
  storeTo(ctl, nv, s->loc);

  // P3.3: attach llvm.loop metadata to the backedge (header->latch->header).
  // The loop ID is self-referential: MDNode with itself as operand 0.
  llvm::UncondBrInst* backedge = b_.CreateBr(condL);
  if (loopID_) {
    llvm::SmallVector<llvm::Metadata*, 4> ops;
    ops.push_back(loopID_); // self-reference slot
    for (llvm::Metadata* prop : loopMDs_)
      ops.push_back(prop);
    llvm::MDNode* loopID = llvm::MDNode::getDistinct(ctx_, ops);
    loopID->replaceOperandWith(0, loopID);
    backedge->setMetadata(llvm::LLVMContext::MD_loop, loopID);
  }

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
  deferCall("pli_put_data_end", {});
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
  deferCall("pli_put_data_name", {nameVal, i64((long long)name.size())});
  Val v = emitExpr(t);
  switch (v.ty.k) {
  case TK::Char:
    deferCall("pli_put_list_char", {v.ptr, v.len});
    break;
  case TK::Float:
    deferCall("pli_put_list_float", {v.reg});
    break;
  case TK::Bit: {
    llvm::Value* bit = b_.CreateZExt(v.reg, b_.getInt8Ty(), "bit");
    deferCall("pli_put_list_bit", {bit});
    break;
  }
  case TK::Complex:
    // Complex DATA output (CM5): the same real+imaginary list image, printed
    // directly after the NAME= prefix.
    deferCall("pli_put_list_complex", {b_.CreateExtractValue(v.cpxVal(), 0, "dcpx.re"),
                                       b_.CreateExtractValue(v.cpxVal(), 1, "dcpx.im")});
    break;
  case TK::FixedBin:
  case TK::FixedDec:
    if (v.ty.k == TK::FixedDec && v.ty.scale > 0) {
      if (v.ty.intBits() == 128) // ADR-191: >18 digit DECIMAL needs wide I/O
        emitWideDecOut("pli_put_list_decfixed128", v.reg, v.ty.scale);
      else {
        // The library routine does the conversion (ADR-254): the scaled
        // decimal becomes the runtime's fixed-point form.
        if (remarkLibraryCalls_)
          d_.remark(curLoc_, "the library routine does this conversion: the decimal output");
        deferCall("pli_put_list_decfixed", {toI64(v), i64(v.ty.scale)});
      }
    } else if (v.ty.k == TK::FixedDec && v.ty.intBits() == 128) {
      emitWideDecOut("pli_put_list_decfixed128", v.reg, 0);
    } else if (v.ty.k == TK::FixedBin && v.ty.scale > 0) {
      // The stored integer is value*2^q; the runtime prints the point
      // (ADR-284).
      deferCall("pli_put_list_fixed_scaled", {toI64(v), i64(v.ty.scale)});
    } else {
      deferCall("pli_put_list_fixed", {toI64(v)});
    }
    break;
  default:
    break;
  }
}

// ---------------------------------------------------------------------------
// Output statements and the output lock (ADR-233)
// ---------------------------------------------------------------------------

// Record one stream call. A program with a task defers it to the end of the
// statement (so the whole record is written or read under one lock); a
// program without one emits it in place.
void IRGen::deferCall(const char* fn, std::vector<llvm::Value*> args, bool capture,
                      std::function<void(const std::vector<llvm::Value*>&, size_t)> post) {
  llvm::Function* f = runtimeFn(fn);
  if (!hasAsync_) {
    llvm::Value* r = b_.CreateCall(f, args);
    if (capture)
      lastPutResult_ = r;
    callResults_.push_back(r);
    if (post)
      post(callResults_, callResults_.size() - 1);
    return;
  }
  pendingCalls_.push_back({f, std::move(args), capture, std::move(post)});
}

// Write the deferred calls of the statement between the two bracket calls
// (the lock) and run their post actions after it. The lock is taken here,
// after the statement's expressions have been evaluated, so an item
// expression that blocks on another task never holds it. Returns the result
// of the last captured call (the STRING close reports the characters
// written).
llvm::Value* IRGen::flushCalls(const char* beginFn, const char* endFn) {
  if (!hasAsync_) {
    pendingCalls_.clear();
    callResults_.clear();
    return lastPutResult_;
  }
  b_.CreateCall(runtimeFn(beginFn), {});
  std::vector<llvm::Value*> results;
  results.reserve(pendingCalls_.size());
  for (auto& p : pendingCalls_) {
    llvm::Value* r = b_.CreateCall(p.fn, p.args);
    results.push_back(r);
    if (p.capture)
      lastPutResult_ = r;
  }
  b_.CreateCall(runtimeFn(endFn), {});
  // The post actions run after the lock is released: they store into the
  // program's own variables, and their target expressions must not run while
  // the lock is held.
  for (size_t i = 0; i < pendingCalls_.size(); ++i)
    if (pendingCalls_[i].post)
      pendingCalls_[i].post(results, i);
  pendingCalls_.clear();
  callResults_.clear();
  return lastPutResult_;
}

void IRGen::emitPut(HStmt* s) {
  // ADR-233: the statement's expressions are evaluated first and the writes
  // are deferred to the end of the statement, so the whole record reaches the
  // stream under one lock and a blocking item expression never holds it.
  lastPutResult_ = nullptr;
  // FILE ( f ) (rule 105): route the list-directed output through the named
  // file's stream instead of SYSPRINT. Select before page/skip so all output
  // goes to the named file.
  if (s->fileSym)
    deferCall("pli_put_select", {fileSlotValue(s->fileSym)});
  if (s->page)
    deferCall("pli_put_page", {});
  if (s->skip) {
    llvm::Value* n = i64(1);
    if (s->skipCount) {
      Val v = emitExpr(s->skipCount.get());
      n = toI64(v);
    }
    deferCall("pli_put_skip", {n});
  }
  // LINE ( n ) stream option (rule (105)): start the output at line n.
  if (s->line) {
    llvm::Value* n = s->lineCount ? toI64(emitExpr(s->lineCount.get())) : i64(1);
    deferCall("pli_put_edit_line", {n});
  }
  // STRING (rule 105) sink: route the list-directed output into the character
  // variable instead of SYSPRINT.
  llvm::Value *sdata = nullptr, *slen = nullptr;
  bool isVarying = false;
  llvm::Value* slenPrefix = nullptr; // for varying: pointer to length prefix
  if (s->stringTarget) {
    HExpr* st = s->stringTarget.get();
    // A locator-qualified target P->S.STR (rule 105) sinks into the block the
    // locator points at, not the structure's declared BASED pointer.
    auto stringBase = [&]() -> llvm::Value* {
      if (st->locPtr)
        return st->memberPath.empty()
                   ? emitExpr(st->locPtr.get()).reg
                   : locatorMemberAddr(st->sym, st->memberPath, emitExpr(st->locPtr.get()).reg);
      return st->memberPath.empty() ? addressOf(st->sym)
                                    : memberAddr(st->sym, st->memberPath, st->loc);
    };
    if (st->ty.isChar() && st->ty.varying) {
      // VARYING string: data is at struct index 1, length prefix at index 0
      llvm::Value* base = stringBase();
      // The leaf's own VARYING layout: data at field 1, length at field 0.
      sdata = b_.CreateStructGEP(llvmTy(st->ty), base, 1, "vdata");
      slen = i64(st->ty.len); // capacity
      slenPrefix = b_.CreateStructGEP(llvmTy(st->ty), base, 0, "vlen");
      isVarying = true;
    } else {
      sdata = stringBase();
      slen = i64(st->ty.len);
    }
    deferCall("pli_string_put_open", {sdata, slen});
  }
  if (s->edit) {
    emitPutEditItems(s);
  } else if (s->data) {
    emitPutDataItems(s);
  } else {
    for (auto& item : s->items) {
      Val v = emitExpr(item.get());
      switch (v.ty.k) {
      case TK::Char:
        deferCall("pli_put_list_char", {v.ptr, v.len});
        break;
      case TK::Float:
        deferCall("pli_put_list_float", {v.reg});
        break;
      case TK::Bit: {
        llvm::Value* bit = b_.CreateZExt(v.reg, b_.getInt8Ty(), "bit");
        deferCall("pli_put_list_bit", {bit});
        break;
      }
      case TK::FixedBin:
      case TK::FixedDec:
        if (v.ty.k == TK::FixedDec && v.ty.scale > 0) {
          if (v.ty.intBits() == 128) // ADR-191: >18 digit DECIMAL needs wide I/O
            emitWideDecOut("pli_put_list_decfixed128", v.reg, v.ty.scale);
          else {
            // The library routine does the conversion (ADR-254).
            if (remarkLibraryCalls_)
              d_.remark(curLoc_, "the library routine does this conversion: the decimal output");
            deferCall("pli_put_list_decfixed", {toI64(v), i64(v.ty.scale)});
          }
        } else if (v.ty.k == TK::FixedDec && v.ty.intBits() == 128) {
          emitWideDecOut("pli_put_list_decfixed128", v.reg, 0);
        } else if (v.ty.k == TK::FixedBin && v.ty.scale > 0) {
          // The stored integer is value*2^q; the runtime prints the point
          // (ADR-284).
          deferCall("pli_put_list_fixed_scaled", {toI64(v), i64(v.ty.scale)});
        } else {
          deferCall("pli_put_list_fixed", {toI64(v)});
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
        deferCall("pli_put_list_complex", {b_.CreateExtractValue(v.cpxVal(), 0, "cpx.re"),
                                           b_.CreateExtractValue(v.cpxVal(), 1, "cpx.im")});
        break;
      case TK::Task:
      case TK::Event:
        d_.error(s->loc, "a TASK/EVENT value cannot be written with PUT LIST in this stage",
                 "(110)");
        break;
      case TK::Label:
        d_.error(s->loc, "a LABEL value cannot be written with PUT LIST in this stage", "(110)");
        break;
      }
    }
  }
  if (s->stringTarget)
    deferCall("pli_string_put_close", {sdata, slen}, /*capture=*/true);
  if (s->copy)
    deferCall("pli_copy", {});
  if (s->fileSym)
    deferCall("pli_put_unselect", {});
  // Write the record: under the output lock when the program has a task.
  llvm::Value* written = flushCalls("pli_out_begin", "pli_out_end");
  if (s->stringTarget && isVarying) {
    // Store the actual length written into the varying string's length prefix
    b_.CreateStore(b_.CreateTrunc(written, b_.getInt32Ty(), "vlen32"), slenPrefix);
  }
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
        // The reader returns an i64; a wide DECIMAL slot is i128.
        v.reg = fitTo(raw, llvmTy(ty), "gdec");
        v.ty = ty;
      } else if (ty.k == TK::FixedBin && ty.scale > 0) {
        // The reader returns value*2^q, the target's stored form (ADR-284).
        v.reg = b_.CreateCall(runtimeFn("pli_get_list_fixed_scaled"), {i64(ty.scale)});
        v.ty = Type::fixedBin(63, ty.scale);
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
      llvm::Value* n = b_.CreateCall(runtimeFn("pli_get_list_char"),
                                     {buf, i64(ty.len)}, "gdch.n");
      v.ptr = buf;
      // VARYING stores the assigned length; a fixed target stays padded to len.
      v.len = ty.varying ? n : i64(ty.len);
      v.ty = ty;
      break;
    }
    case TK::Complex: {
      // Complex DATA input (CM5): read both parts through element pointers,
      // then load the pair into the target.
      llvm::Value* pair = entryAlloca(llvmTy(ty), "gdcx");
      b_.CreateCall(runtimeFn("pli_get_list_complex"),
                    {b_.CreateStructGEP(llvmTy(ty), pair, 0, "gdcxr"),
                     b_.CreateStructGEP(llvmTy(ty), pair, 1, "gdcxi")});
      v.cpx = b_.CreateLoad(llvmTy(ty), pair, "gdcx");
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
  // ADR-233: the statement's expressions are evaluated first and the reads are
  // deferred, so the whole record is read under the input lock and a blocking
  // item expression never holds it. The stores run after the lock is released.
  // STRING (rule 105) source: route the list-directed input from the character
  // variable instead of SYSIN.
  llvm::Value *sdata = nullptr, *slen = nullptr;
  if (s->stringTarget) {
    HExpr* st = s->stringTarget.get();
    // A locator-qualified source P->S.STR (rule 105) reads the block the
    // locator points at, not the structure's declared BASED pointer.
    sdata = st->locPtr ? (st->memberPath.empty()
                              ? emitExpr(st->locPtr.get()).reg
                              : locatorMemberAddr(st->sym, st->memberPath,
                                                  emitExpr(st->locPtr.get()).reg))
                       : (st->memberPath.empty()
                              ? addressOf(st->sym)
                              : memberAddr(st->sym, st->memberPath, s->loc));
    // GET STRING takes a NONVARYING source (rule (105)); sema diagnoses a
    // VARYING one, so the whole capacity is the read length here.
    slen = i64(st->ty.len);
  }
  // FILE ( f ) (rule 105): route the list-directed input through the named
  // file's stream instead of SYSIN.
  if (s->data) {
    // The DATA form's reads drive its control flow (the name that comes next
    // picks the item), so its calls cannot be deferred: the statement takes
    // the input lock around the read loop instead.
    if (hasAsync_)
      b_.CreateCall(runtimeFn("pli_in_begin"), {});
    if (s->stringTarget)
      b_.CreateCall(runtimeFn("pli_string_get_open"), {sdata, slen});
    if (s->fileSym)
      b_.CreateCall(runtimeFn("pli_get_select"), {fileSlotValue(s->fileSym)});
    // LINE ( n ) stream option (rule (105)): resume input at line n.
    if (s->line) {
      llvm::Value* n = s->lineCount ? toI64(emitExpr(s->lineCount.get())) : i64(1);
      b_.CreateCall(runtimeFn("pli_get_edit_skip"), {n});
    }
    emitGetDataItems(s);
    if (s->fileSym)
      b_.CreateCall(runtimeFn("pli_get_unselect"), {});
    if (s->stringTarget)
      b_.CreateCall(runtimeFn("pli_string_get_close"), {});
    if (hasAsync_)
      b_.CreateCall(runtimeFn("pli_in_end"), {});
    return;
  }
  if (s->stringTarget)
    deferCall("pli_string_get_open", {sdata, slen});
  if (s->fileSym)
    deferCall("pli_get_select", {fileSlotValue(s->fileSym)});
  // LINE ( n ) stream option (rule (105)): resume input at line n.
  if (s->line) {
    llvm::Value* n = s->lineCount ? toI64(emitExpr(s->lineCount.get())) : i64(1);
    deferCall("pli_get_edit_skip", {n});
  }
  if (s->edit) {
    emitGetEditItems(s);
  } else {
    for (auto& item : s->items) {
      HExpr* t = item.get();
      const Type& ty = t->ty;
      SourceLoc loc = s->loc;
      switch (ty.k) {
      case TK::FixedBin:
      case TK::FixedDec:
        if (ty.k == TK::FixedDec && ty.scale > 0) {
          // Already scaled by the reader; truncate to the target width
          // without rescaling, then the store passes it through.
          deferCall("pli_get_list_decfixed", {i64(ty.scale)}, false,
                    [this, t, loc](const std::vector<llvm::Value*>& r, size_t i) {
                      Val v;
                      v.reg = fitTo(r[i], llvmTy(t->ty), "gdec");
                      v.ty = t->ty;
                      storeGetTarget(t, v, loc);
                    });
        } else if (ty.k == TK::FixedBin && ty.scale > 0) {
          // The reader returns value*2^q, the target's stored form (ADR-284).
          deferCall("pli_get_list_fixed_scaled", {i64(ty.scale)}, false,
                    [this, t, loc](const std::vector<llvm::Value*>& r, size_t i) {
                      Val v;
                      v.reg = r[i];
                      v.ty = Type::fixedBin(63, t->ty.scale);
                      storeGetTarget(t, v, loc);
                    });
        } else {
          deferCall("pli_get_list_fixed", {}, false,
                    [this, t, loc](const std::vector<llvm::Value*>& r, size_t i) {
                      Val v;
                      v.reg = r[i];
                      v.ty = Type::fixedBin(63, 0);
                      storeGetTarget(t, v, loc);
                    });
        }
        break;
      case TK::Float:
        deferCall("pli_get_list_float", {}, false,
                  [this, t, loc](const std::vector<llvm::Value*>& r, size_t i) {
                    Val v;
                    v.reg = r[i];
                    v.ty = Type::flt(6);
                    storeGetTarget(t, v, loc);
                  });
        break;
      case TK::Bit: {
        deferCall("pli_get_list_bit", {}, false,
                  [this, t, loc](const std::vector<llvm::Value*>& r, size_t i) {
                    Val v;
                    v.reg = b_.CreateTrunc(r[i], b_.getInt1Ty(), "gbit");
                    v.ty = Type::bit();
                    storeGetTarget(t, v, loc);
                  });
        break;
      }
      case TK::Char: {
        if (!ty.picture.empty()) {
          // Numeric character data (ADR-292): read an arithmetic value at
          // the picture's decimal scale, then let convertToPicture edit it.
          const pli::PictureSpec spec = pli::classifyPicture(ty.picture);
          const int frac = spec.fracDigits;
          int prec = spec.intDigits + frac;
          if (prec < 1)
            prec = 1;
          if (prec > 18) {
            // A wide picture reads a 128-bit decimal value (B1).
            llvm::Value* slot = entryAlloca(b_.getInt128Ty(), "glw");
            deferCall("pli_get_list_decfixed_wide", {i64(frac), slot}, false,
                      [this, t, loc, prec, frac, slot](const std::vector<llvm::Value*>&, size_t) {
                        Val v;
                        v.reg = b_.CreateLoad(b_.getInt128Ty(), slot, "glwv");
                        v.ty = Type::fixedDec(prec, frac);
                        storeGetTarget(t, v, loc);
                      });
          } else {
            deferCall("pli_get_list_decfixed", {i64(frac)}, false,
                      [this, t, loc, prec, frac](const std::vector<llvm::Value*>& r, size_t i) {
                        Val v;
                        v.reg = r[i];
                        v.ty = Type::fixedDec(prec, frac);
                        storeGetTarget(t, v, loc);
                      });
          }
          break;
        }
        // Read into a reusable entry buffer, then assign into the target.
        llvm::Value* buf = entryAlloca(llvm::ArrayType::get(b_.getInt8Ty(), ty.len), "gch");
        deferCall("pli_get_list_char", {buf, i64(ty.len)}, false,
                  [this, t, buf, loc](const std::vector<llvm::Value*>& r, size_t i) {
                    Val v;
                    v.ptr = buf;
                    // VARYING takes the input length; fixed stays padded to len.
                    v.len = t->ty.varying ? r[i] : i64(t->ty.len);
                    v.ty = t->ty;
                    storeGetTarget(t, v, loc);
                  });
        break;
      }
      case TK::Complex: {
        // Read both parts through element pointers, then load the pair.
        llvm::Value* pair = entryAlloca(llvmTy(ty), "gcx");
        deferCall("pli_get_list_complex",
                  {b_.CreateStructGEP(llvmTy(ty), pair, 0, "gcxr"),
                   b_.CreateStructGEP(llvmTy(ty), pair, 1, "gcxi")},
                  false, [this, t, pair, loc](const std::vector<llvm::Value*>&, size_t) {
                    Val v;
                    v.cpx = b_.CreateLoad(llvmTy(t->ty), pair, "gcx");
                    v.ty = t->ty;
                    storeGetTarget(t, v, loc);
                  });
        break;
      }
      default:
        d_.error(item->loc, "GET LIST of this type is not implemented in this stage", "(110)");
        continue;
      }
    }
  }
  if (s->stringTarget)
    deferCall("pli_string_get_close", {});
  if (s->fileSym)
    deferCall("pli_get_unselect", {});
  flushCalls("pli_in_begin", "pli_in_end");
}

// Store an input value into a data-list reference (rules (109),(110)): the same
// target-addressing as an assignment's left-hand side.
void IRGen::storeGetTarget(HExpr* t, const Val& v, SourceLoc loc) {
  const Type& ty = t->ty;
  if (t->kind == HExpr::Subscript && t->sym) {
    if (!t->memberPath.empty()) {
      const Type& arr = memberType(t->sym, t->memberPath);
      llvm::Value *ub = nullptr, *lb = nullptr;
      // A locator-qualified member P->S.A(i) (rules 124,126) stores through
      // the locator value, not the declared BASED pointer.
      llvm::Value* base =
          t->locPtr
              ? locatorMemberAddr(t->sym, t->memberPath, emitExpr(t->locPtr.get()).reg)
              : (arr.isDynamic() ? dynamicMemberBase(t->sym, t->memberPath, loc, ub, lb)
                                 : memberAddr(t->sym, t->memberPath, loc));
      llvm::Value* addr = arrayElementAddr(arr, base, t->args, loc, ub, lb);
      Val cv = convert(v, ty, loc);
      if (ty.isChar())
        storeCharTo(addr, ty, cv, loc, packedMemberPath(t->sym->ty, t->memberPath));
      else
        storeScalarTo(addr, ty, cv, packedMemberPath(t->sym->ty, t->memberPath));
      return;
    }
    if (t->sym->definedBase && t->sym->hasIsubOverlay()) {
      llvm::Value* addr = definedSubElementAddr(t->sym, t->args, loc);
      Val cv = convert(v, ty, loc);
      if (ty.isChar())
        storeCharTo(addr, ty, cv, loc);
      else
        storeScalarTo(addr, ty, cv);
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
      Val cv = convert(v, ty, loc);
      if (ty.isChar())
        storeCharTo(addr, ty, cv, loc, packedMemberPath(t->sym->ty, t->memberPath));
      else
        storeScalarTo(addr, ty, cv, packedMemberPath(t->sym->ty, t->memberPath));
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
      deferCall("pli_put_edit_float_e", {dbl, w, d});
    else
      deferCall("pli_put_edit_float", {dbl, w, d});
  };
  for (auto& f : s->formats) {
    switch (f.kind) {
    case HFormatItem::X: {
      llvm::Value* w = f.w ? toI64(emitExpr(f.w.get())) : defW;
      deferCall("pli_put_edit_x", {w});
      break;
    }
    case HFormatItem::Skip: {
      llvm::Value* n = f.w ? toI64(emitExpr(f.w.get())) : i64(1);
      deferCall("pli_put_edit_skip", {n});
      break;
    }
    case HFormatItem::Page:
      deferCall("pli_put_edit_page", {});
      break;
    case HFormatItem::Line: {
      llvm::Value* n = f.w ? toI64(emitExpr(f.w.get())) : i64(1);
      deferCall("pli_put_edit_line", {n});
      break;
    }
    case HFormatItem::Column: {
      // Column positioning (rule (48)): pad blanks to 1-based column n.
      llvm::Value* n = f.w ? toI64(emitExpr(f.w.get())) : i64(1);
      deferCall("pli_put_edit_column", {n});
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
      deferCall("pli_put_edit_char", {v.ptr, v.len, w});
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
      deferCall("pli_put_edit_bit", {ptr, i64(nbits), w});
      break;
    }
    case HFormatItem::C: {
      // Complex output (rule (51)): one COMPLEX item through 1-2 inner real
      // formats; a single sub serves both parts.
      if (di >= s->items.size())
        break;
      HExpr* item = s->items[di++].get();
      Val v = emitExpr(item);
      llvm::Value* re = b_.CreateExtractValue(v.cpxVal(), 0, "cpx.re");
      llvm::Value* im = b_.CreateExtractValue(v.cpxVal(), 1, "cpx.im");
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
        deferCall("pli_put_edit_float", {v.reg, w, d});
      } else if (v.ty.k == TK::FixedBin && v.ty.scale > 0) {
        // F(w,d) needs the true value: a scaled binary is divided by 2^q
        // first (ADR-284). The FLOAT path keeps the common d digits; a
        // point-aware fixed formatter remains a B11 refinement.
        Val fv = convert(v, Type::flt(6), item->loc);
        deferCall("pli_put_edit_float", {fv.reg, w, d});
      } else {
        llvm::Value* scale = v.ty.k == TK::FixedDec ? i64(v.ty.scale) : i64(0);
        deferCall("pli_put_edit_fixed", {toI64(v), scale, w, d});
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
      deferCall("pli_put_edit_float_e", {v.reg, w, d});
      break;
    }
    case HFormatItem::P: {
      // P(picture): picture-directed output (rules (53),(146)-(148));
      // formats a numeric value via pli_put_edit_pic against the picture string.
      if (di >= s->items.size())
        break;
      HExpr* item = s->items[di++].get();
      Val v = emitExpr(item);
      const pli::PictureSpec spec = pli::classifyPicture(f.pic);
      llvm::Value* val = nullptr;
      llvm::Value* signSrc = nullptr;
      bool binaryScaled = false;
      int scale = 0;
      if (spec.sterling) {
        if (v.ty.k == TK::FixedDec && v.ty.intBits() == 128) {
          d_.error(item->loc, "wide FIXED DECIMAL output through a sterling PICTURE is not "
                              "implemented",
                   "(53)");
          break;
        }
        if (!v.ty.isFixed() && v.ty.k != TK::Float) {
          d_.error(item->loc, "P edit item requires a numeric value", "(53)");
          break;
        }
        if (v.ty.isFixed() && v.ty.scale != 0) {
          d_.error(item->loc, "sterling PICTURE output requires a whole-pence value", "(53)");
          break;
        }
        val = v.ty.k == TK::Float ? b_.CreateFPToSI(v.reg, b_.getInt64Ty(), "picsf")
                                  : toI64(v);
        llvm::Value* sign = b_.CreateSelect(b_.CreateICmpSLT(val, i64(0), "picsgn"),
                                            i32(-1), i32(1), "picst");
        facts_.recordBuiltin("picture", "runtime",
                             "the sterling picture conversion needs the library routine");
        if (remarkLibraryCalls_)
          d_.remark(curLoc_, "the library routine does this conversion: the picture conversion");
        deferCall("pli_put_edit_pic_sterling",
                  {val, sign, globalString(spec.expanded),
                   i64((long long)spec.expanded.size())});
        break;
      }
      if (v.ty.k == TK::FixedDec && v.ty.intBits() == 128) {
        // Wide DECIMAL output uses the 128-bit formatter core (B1).
        llvm::Value* lo = b_.CreateTrunc(v.reg, b_.getInt64Ty(), "picw.lo");
        llvm::Value* hi = b_.CreateTrunc(b_.CreateLShr(v.reg, (uint64_t)64, "picw.shr"),
                                         b_.getInt64Ty(), "picw.hi");
        llvm::Value* sign = b_.CreateSelect(b_.CreateICmpSLT(v.reg, i128(0), "picwneg"),
                                            i32(-1), i32(1), "picwsgn");
        facts_.recordBuiltin("picture", "runtime",
                             "the wide picture conversion needs the library routine");
        if (remarkLibraryCalls_)
          d_.remark(curLoc_, "the library routine does this conversion: the picture conversion");
        deferCall("pli_put_edit_pic_wide",
                  {lo, hi, i64(v.ty.scale), sign, globalString(spec.expanded),
                   i64((long long)spec.expanded.size())});
        break;
      }
      if (v.ty.isFixed()) {
        val = toI64(v);
        signSrc = val;
        scale = v.ty.scale;
        if (v.ty.k == TK::FixedBin && v.ty.scale > 0) {
          // A binary scale holds raw/2^q; the formatter works in decimal.
          // Truncate at the picture's fractional positions in i128 and hand
          // the formatter that decimal value (ADR-288). The sign comes from
          // the raw value so a negative fraction below one keeps its sign.
          binaryScaled = true;
          const int d = spec.valid ? spec.fracDigits : 0;
          llvm::Type* wideTy = b_.getInt128Ty();
          llvm::APInt p10(128, 1);
          for (int i = 0; i < d; ++i)
            p10 *= llvm::APInt(128, 10);
          llvm::Value* wide = b_.CreateSExt(val, wideTy, "picw");
          wide = b_.CreateMul(wide, llvm::ConstantInt::get(wideTy, p10), "picsc");
          llvm::APInt pow2 = llvm::APInt(128, 1).shl(v.ty.scale);
          wide = b_.CreateSDiv(wide, llvm::ConstantInt::get(wideTy, pow2), "picdv");
          val = b_.CreateTrunc(wide, b_.getInt64Ty(), "picdec");
          scale = d;
        }
      } else if (v.ty.k == TK::Float) {
        // Convert float to fixed-decimal via truncation.
        val = b_.CreateFPToSI(v.reg, b_.getInt64Ty(), "picf");
        signSrc = val;
      } else {
        d_.error(item->loc, "P edit item requires a numeric value", "(53)");
        break;
      }
      // B4: a constant value through a foldable constant picture is formatted
      // at compile time (rules (146)-(148)); the lowering policy still wins,
      // so --experimental-lowering=picture:runtime keeps the interpreter and
      // the differential oracle keeps both sides (ADR-274). A scaled binary
      // stays on the runtime path: its sign is not recoverable from the
      // truncated decimal value when the fraction is below one.
      if (!binaryScaled && !runtimePath("picture") && spec.foldable) {
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(val)) {
          std::string text;
          if (pli::foldPictureConversion(ci->getSExtValue(), scale, spec, text)) {
            facts_.recordBuiltin("picture", "llvm");
            deferCall("pli_put_edit_char", {globalString(text), i64((long long)text.size()),
                                            i64((long long)text.size())});
            break;
          }
        }
      }
      facts_.recordBuiltin("picture", "runtime",
                           "the picture conversion needs the library routine");
      // The library-call remark (ADR-254): the picture conversion is done by
      // the library routine, so the user is told where it happens.
      if (remarkLibraryCalls_)
        d_.remark(curLoc_, "the library routine does this conversion: the picture conversion");
      // sign derived from the value: -1 if negative, +1 if non-negative.
      llvm::Value* neg = b_.CreateICmpSLT(signSrc, i64(0), "picsign");
      llvm::Value* sign = b_.CreateSelect(neg, i32(-1), i32(1), "picst");
      deferCall("pli_put_edit_pic",
                {val, i64(scale), sign, globalString(f.pic), i64((long long)f.pic.size())});
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
      deferCall("pli_get_edit_x", {w});
      break;
    }
    case HFormatItem::Skip: {
      llvm::Value* n = f.w ? toI64(emitExpr(f.w.get())) : i64(1);
      deferCall("pli_get_edit_skip", {n});
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
      deferCall("pli_get_edit_column", {n});
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
      SourceLoc loc = s->loc;
      llvm::Value* w = f.w ? toI64(emitExpr(f.w.get())) : defW;
      llvm::Value* buf = entryAlloca(llvm::ArrayType::get(b_.getInt8Ty(), ty.len), "gech");
      deferCall("pli_get_edit_char", {buf, i64(ty.len), w}, false,
                [this, t, buf, loc](const std::vector<llvm::Value*>& r, size_t i) {
                  Val v;
                  v.ptr = buf;
                  // VARYING takes the field width; fixed stays padded to len.
                  v.len = t->ty.varying ? r[i] : i64(t->ty.len);
                  v.ty = t->ty;
                  storeGetTarget(t, v, loc);
                });
      break;
    }
    case HFormatItem::F:
    case HFormatItem::E: {
      if (di >= s->items.size())
        break;
      HExpr* t = s->items[di++].get();
      SourceLoc loc = s->loc;
      llvm::Value* w = f.w ? toI64(emitExpr(f.w.get())) : defW;
      if (f.s)
        (void)toI64(emitExpr(f.s.get())); // scale evaluated for effects (50)
      deferCall("pli_get_edit_num", {w}, false,
                [this, t, loc](const std::vector<llvm::Value*>& r, size_t i) {
                  Val v;
                  v.reg = r[i];
                  v.ty = Type::flt(6);
                  storeGetTarget(t, v, loc);
                });
      break;
    }
    case HFormatItem::B: {
      // Bit-string input (rule (52)): read a w-character '0'/'1' field into
      // a temp packed buffer, then store through the normal BIT path.
      if (di >= s->items.size())
        break;
      HExpr* t = s->items[di++].get();
      SourceLoc loc = s->loc;
      long long nbits = t->ty.isBit() && t->ty.len > 0 ? t->ty.len : 1;
      long long nbytes = (nbits + 7) / 8;
      llvm::Value* tmp =
          entryAlloca(llvm::ArrayType::get(b_.getInt8Ty(), (unsigned)nbytes), "gebit");
      llvm::Value* ptr = b_.CreateInBoundsGEP(
          llvm::ArrayType::get(b_.getInt8Ty(), (unsigned)nbytes), tmp, {i64(0), i64(0)}, "gebitp");
      llvm::Value* w = f.w ? toI64(emitExpr(f.w.get())) : i64(nbits);
      deferCall("pli_get_edit_bit", {ptr, i64(nbits), w}, false,
                [this, t, tmp, ptr, nbits, nbytes, loc](const std::vector<llvm::Value*>&, size_t) {
                  Val v;
                  v.ty = Type::bit((int)nbits);
                  if (nbits == 1) {
                    // The runtime packs into the top bit; BIT(1) travels as the low bit.
                    llvm::Value* byte = b_.CreateLoad(b_.getInt8Ty(), ptr, "gb8");
                    llvm::Value* low =
                        b_.CreateLShr(byte, llvm::ConstantInt::get(b_.getInt8Ty(), 7), "gb7");
                    v.reg = b_.CreateTrunc(low, b_.getInt1Ty(), "gb1");
                  } else {
                    v.reg = b_.CreateLoad(llvm::ArrayType::get(b_.getInt8Ty(), (unsigned)nbytes),
                                          tmp, "gb");
                  }
                  storeGetTarget(t, v, loc);
                });
      break;
    }
    case HFormatItem::C: {
      // Complex input (rule (51)): read the real/imaginary parts through the
      // inner real widths, then store the {double,double} pair.
      if (di >= s->items.size())
        break;
      HExpr* t = s->items[di++].get();
      SourceLoc loc = s->loc;
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
      // The two halves are read in order; the store waits for the second one.
      deferCall("pli_get_edit_num", {w1});
      deferCall("pli_get_edit_num", {w2}, false,
                [this, t, loc](const std::vector<llvm::Value*>& r, size_t i) {
                  llvm::Value* cpx = llvm::UndefValue::get(llvmTy(Type::complexTy()));
                  cpx = b_.CreateInsertValue(cpx, r[i - 1], 0, "cpx.re");
                  cpx = b_.CreateInsertValue(cpx, r[i], 1, "cpx.im");
                  Val v;
                  v.cpx = cpx;
                  v.ty = Type::complexTy();
                  storeGetTarget(t, v, loc);
                });
      break;
    }
    case HFormatItem::P: {
      // P input (ADR-291): read the picture-width field and return the value
      // at the picture's assumed decimal point; the normal conversion path
      // then rescales it into a numeric target or formats a PICTURE target.
      if (di >= s->items.size())
        break;
      HExpr* t = s->items[di++].get();
      SourceLoc loc = s->loc;
      const pli::PictureSpec spec = pli::classifyPicture(f.pic);
      if (!spec.valid || spec.width <= 0) {
        d_.error(loc, "invalid PICTURE format for input", "(53)");
        break;
      }
      if (spec.sterling) {
        deferCall("pli_get_edit_pic_sterling",
                  {i64(spec.width), globalString(spec.expanded),
                   i64((long long)spec.expanded.size())},
                  false, [this, t, spec, loc](const std::vector<llvm::Value*>& r, size_t i) {
                    Val v;
                    v.reg = r[i];
                    int p = spec.intDigits > 0 ? spec.intDigits : 1;
                    v.ty = Type::fixedDec(p, 0);
                    storeGetTarget(t, v, loc);
                  });
        break;
      }
      {
        int p = spec.intDigits + spec.fracDigits;
        if (p > 18) {
          // Wide P input writes the 128-bit value through a temp (B1).
          llvm::Value* slot = entryAlloca(b_.getInt128Ty(), "gpicw");
          deferCall("pli_get_edit_pic_wide", {i64(spec.width), i64(spec.fracDigits), slot},
                    false, [this, t, spec, loc, slot](const std::vector<llvm::Value*>&, size_t) {
                      Val v;
                      v.reg = b_.CreateLoad(b_.getInt128Ty(), slot, "gpicwv");
                      int prec = spec.intDigits + spec.fracDigits;
                      v.ty = Type::fixedDec(prec > 0 ? prec : 1, spec.fracDigits);
                      storeGetTarget(t, v, loc);
                    });
        } else {
          deferCall("pli_get_edit_pic", {i64(spec.width), i64(spec.fracDigits)}, false,
                    [this, t, spec, loc, p](const std::vector<llvm::Value*>& r, size_t i) {
                      Val v;
                      v.reg = r[i];
                      v.ty = Type::fixedDec(p > 0 ? p : 1, spec.fracDigits);
                      storeGetTarget(t, v, loc);
                    });
        }
      }
      break;
    }
    }
  }
}

// Evaluate a call actual once (rule 78): reuse the value argAddr recorded for
// this call site, or evaluate the expression and record it. The hidden
// CHAR(*) length argument (argLen) reads the record back instead of running
// the actual a second time.
Val IRGen::materializeActual(HExpr* a) {
  auto it = actuals_.find(a);
  if (it != actuals_.end())
    return it->second;
  Val v = emitExpr(a);
  actuals_.emplace(a, v);
  return v;
}

// Fit an integer value to the destination integer type: widen with a sign
// extension, narrow with a truncation. A direct SExt/Trunc is invalid IR when
// the widths do not line up (an i64 runtime result into an i128 wide-DECIMAL
// slot, or an i128 value into an i64 slot).
llvm::Value* IRGen::fitTo(llvm::Value* v, llvm::Type* dst, const char* name) {
  unsigned src = v->getType()->getIntegerBitWidth();
  unsigned d = dst->getIntegerBitWidth();
  if (src == d)
    return v;
  return d > src ? b_.CreateSExt(v, dst, name) : b_.CreateTrunc(v, dst, name);
}

// Address of one call argument for a by-reference parameter (rule 4).
llvm::Value* IRGen::argAddr(HExpr* a, const Type& pty, llvm::Type** dummyTy) {
  if (dummyTy)
    *dummyTy = nullptr;
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
    llvm::AllocaInst* dst = entryAlloca(llvmTy(pty), "sv");
    Val av = materializeActual(a);
    llvm::Value* sz = i64(mod_.getDataLayout().getTypeStoreSize(llvmTy(pty)));
    b_.CreateMemCpy(dst, llvm::MaybeAlign(), av.ptr, llvm::MaybeAlign(), sz);
    if (dummyTy)
      *dummyTy = dst->getAllocatedType();
    return dst;
  }
  bool direct = a->kind == HExpr::VarRef && a->sym && a->sym->kind != Symbol::ProcName &&
                a->memberPath.empty() && a->sym->ty.k == pty.k &&
                a->sym->ty.len == pty.len && a->sym->ty.prec == pty.prec &&
                a->sym->ty.varying == pty.varying;
  // P4.1: a member leaf with identical layout rides by reference too — same
  // type, aligned, addressable — skipping the dummy copy (rules (34),(38)).
  // Locator-qualified, DEFINED-overlay and struct leaves keep the old paths.
  bool memberDirect =
      !pty.isStruct() && !(pty.isChar() && pty.starLen) && a->kind == HExpr::VarRef && a->sym &&
      a->sym->kind != Symbol::ProcName && !a->memberPath.empty() && !a->locPtr &&
      !a->sym->definedBase && a->ty == pty && !packedMemberPath(a->sym->ty, a->memberPath);
  // P4.1 for a fixed array element (rule 4, A8): `CALL w(a(i))` passes the
  // element itself when its storage is directly addressable and its layout
  // matches the dummy exactly. Packed members, DEFINED overlays, CONTROLLED
  // and runtime-extent arrays keep the copy path; an element of a fixed
  // array of the same type needs no temporary.
  bool elemDirect =
      !pty.isStruct() && !(pty.isChar() && pty.starLen) && a->kind == HExpr::Subscript &&
      a->sym && a->sym->kind != Symbol::ProcName && a->memberPath.empty() && !a->locPtr &&
      !a->sym->definedBase && !a->sym->controlled && !a->sym->ty.isDynamic() &&
      a->ty == pty;
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
      Val av0 = materializeActual(a);
      if (av0.ty.isChar()) {
        // The temp is a varying struct {i32 len, [cap x i8] data}. The data
        // area must hold the live length, so size it from the value's
        // DECLARED capacity (the data area a varying value carries): the live
        // length never exceeds it. A fixed 256-byte cap overflowed the stack
        // for a live length above it.
        unsigned cap = av0.ty.len > 0 ? (unsigned)av0.ty.len : 1;
        llvm::Type* st = llvm::StructType::get(
            ctx_, {b_.getInt32Ty(), llvm::ArrayType::get(b_.getInt8Ty(), cap)});
        llvm::AllocaInst* tmp = entryAlloca(st, "vstar");
        if (dummyTy)
          *dummyTy = st;
        markTempLifetime(tmp);
        llvm::Value* lp = b_.CreateStructGEP(st, tmp, 0, "vlp");
        llvm::Value* srcLen = av0.len ? av0.len : i64(av0.ty.len);
        b_.CreateStore(b_.CreateTrunc(srcLen, b_.getInt32Ty(), "vl32"), lp);
        llvm::Value* dp = b_.CreateStructGEP(st, tmp, 1, "vdp");
        b_.CreateMemCpy(dp, llvm::MaybeAlign(), av0.ptr, llvm::MaybeAlign(), srcLen);
        // The concat's heap buffer has been copied: release it (the call
        // argument path had no free, so every call leaked one buffer).
        freeConcatTemp(a, av0.ptr);
        return tmp;
      }
    }
    // Non-variable character argument (expression/literal): evaluate the
    // expression into a temp buffer of the result's declared capacity, then
    // pass the buffer pointer and its live length via argLen().
    Val av = materializeActual(a);
    if (av.ty.isChar()) {
      // Size the temp from the value's declared capacity — the live length
      // (a runtime value for a varying actual) never exceeds it. The old
      // fixed 256-byte cap overflowed the stack for a longer actual.
      unsigned cap = av.ty.len > 0 ? (unsigned)av.ty.len : 1;
      llvm::AllocaInst* buf = entryAlloca(llvm::ArrayType::get(b_.getInt8Ty(), cap), "cbuf");
      if (dummyTy)
        *dummyTy = buf->getAllocatedType();
      markTempLifetime(buf);
      b_.CreateMemCpy(buf, llvm::MaybeAlign(), av.ptr, llvm::MaybeAlign(), av.len);
      // The concat's heap buffer has been copied: release it (the call
      // argument path had no free, so every call leaked one buffer).
      freeConcatTemp(a, av.ptr);
      return buf;
    }
    d_.error(a->loc,
             "an adjustable-length CHARACTER parameter takes a character variable in this stage",
             "(18)");
    return llvm::Constant::getNullValue(b_.getPtrTy());
  }
  // A whole STATIC AREA array (rule (20)) rides by reference: the callee reads
  // the element slots directly, so materialise every region first.
  if (direct && a->sym->isStatic && a->sym->ty.isArray() &&
      a->sym->ty.elementType().isArea()) {
    llvm::Value* base = addressOf(a->sym);
    llvm::ArrayType* at =
        llvm::ArrayType::get(b_.getPtrTy(), (unsigned)arrayExtent(a->sym->ty));
    for (long long i = 0, n = arrayExtent(a->sym->ty); i < n; ++i) {
      llvm::Value* slot = b_.CreateInBoundsGEP(at, base, {i64(0), i64(i)}, "ar.slot");
      b_.CreateCall(runtimeFn("pli_area_lazy"), {slot, i64(a->sym->areaSize)});
    }
    return base;
  }
  if (direct)
    return addressOf(a->sym);
  if (memberDirect)
    return memberAddr(a->sym, a->memberPath, a->loc);
  if (elemDirect) {
    llvm::Value* elem = arrayElementAddr(a->sym->ty, addressOf(a->sym), a->args, a->loc,
                                         dynUb_.count(a->sym) ? dynUb_[a->sym] : nullptr,
                                         dynLb_.count(a->sym) ? dynLb_[a->sym] : nullptr);
    // An element of a STATIC AREA array carries its own lazily created region
    // (rule (20)), like the whole-array path above: create it before the
    // callee can allocate from it.
    if (a->sym->isStatic && a->sym->ty.elementType().isArea())
      b_.CreateCall(runtimeFn("pli_area_lazy"), {elem, i64(a->sym->areaSize)});
    return elem;
  }
  llvm::AllocaInst* addr = entryAlloca(llvmTy(pty), "dummy");
  if (dummyTy)
    *dummyTy = addr->getAllocatedType();
  // ADR-246: the dummy is live only until the call returns.
  markTempLifetime(addr);
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
// ADR-246: mark a stack buffer live from this point. The marker goes in the
// block that uses the buffer — the alloca itself stays in the entry block,
// but a marker there would describe a range that ends before the loop's
// next iteration uses the buffer.
void IRGen::markTempLifetime(llvm::Value* buf) {
  // LLVM 23's lifetime intrinsics take the pointer alone (the size is
  // inferred from the allocation): @llvm.lifetime.start.p0(ptr %buf).
  b_.CreateIntrinsic(llvm::Intrinsic::lifetime_start, {b_.getPtrTy()}, {buf});
  pendingTempLifetimes_.push_back(buf);
}

// ADR-246: end the marked lifetimes — the buffers are dead after this point,
// so LLVM's stack colouring can give them one shared slot.
void IRGen::endTempLifetimes() {
  for (llvm::Value* buf : pendingTempLifetimes_)
    b_.CreateIntrinsic(llvm::Intrinsic::lifetime_end, {b_.getPtrTy()}, {buf});
  pendingTempLifetimes_.clear();
}

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
  // ADR-246: the argument buffers are dead once the call has returned and
  // the copy-backs are done, so their stack slots are free again.
  endTempLifetimes();
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
  // ADR-246: the NUL-terminated copy is live only until the call returns.
  markTempLifetime(buf);
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
  // A character expression: reuse the value argAddr already materialized for
  // this call, so the actual is evaluated exactly once (rule 78).
  Val av = materializeActual(a);
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
    llvm::Value* link = addressOf(v);
    // A bare BASED variable (rule (25)) has no storage of its own, so
    // addressOf yields null; it is only ever reached through a locator
    // link (passed separately), so its own slot carries an explicit null.
    if (!link && v->isBased())
      link = llvm::Constant::getNullValue(b_.getPtrTy());
    args.push_back(link);
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
  // Evaluate each actual once for this call (rule 78): the scope keeps the
  // materialized values alive across the argument loop and the hidden length
  // arguments that follow it.
  ActualScope scope(*this);
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
    // A by-value C entry (rules (34),(38)) takes scalar/pointer values, as
    // does a by-value internal parameter (P6.1b, ADR-215).
    if (calleeSym->entryByValue)
      args.push_back(marshalArg(a, pty, s->loc));
    else if (i < calleeParams.size() && isByValueParam(calleeParams[i]))
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
  // Hidden extent/length args when CALLing a plain-EXTERNAL PL/I entry
  // (no local Proc/ENTRY statement): driven by the ENTRY declaration's
  // parameter types.
  if (!en && !callee)
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
  // ABNORMAL(label) (rule (38)): a CALL through this entry returns to the
  // label instead of the statement after the CALL, so branch there now and
  // leave the following statement's code unreachable. Sema resolved the label
  // and rejected an asynchronous call.
  if (!calleeSym->abnormalLabel.empty()) {
    // Leaving enclosing BEGIN blocks reverts the ON units they established,
    // exactly as a GO TO to the same label does (rule (91)).
    if (llvm::Value* depth = onExitDepth(calleeSym->abnormalLabel))
      b_.CreateCall(runtimeFn("pli_on_reset_error"), {depth});
    auto lb = labelBlocks_.find(calleeSym->abnormalLabel);
    if (lb != labelBlocks_.end())
      b_.CreateBr(lb->second);
    else
      d_.error(s->loc,
               "an ABNORMAL entry can only be called from the procedure that declares it",
               "(38)");
  }
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
  if (!callee && !en) {
    // External entry async (rule 79, ADR-298): marshal args for the
    // external procedure using its declared entryParams Types, the same
    // ABI rules as a synchronous external call. Function-async is diagnosed
    // by sema, so no hidden sret is needed here.
    llvm::Function* calleeF = calleeFn(calleeSym);
    std::vector<Type> paramTypes = calleeSym->entryParams;
    std::vector<llvm::Value*> callArgs;
    std::vector<llvm::Type*> dummyTys; // A9: copied actual carries its bytes in the ctx
    ActualScope scope(*this);
    for (size_t i = 0; i < s->args.size() && i < paramTypes.size(); ++i) {
      HExpr* a = s->args[i].get();
      if (a->kind == HExpr::Star) {
        callArgs.push_back(llvm::Constant::getNullValue(b_.getPtrTy()));
        dummyTys.push_back(nullptr);
        continue;
      }
      llvm::Type* dty = nullptr;
      llvm::Value* arg = calleeSym->entryByValue ? marshalArg(a, paramTypes[i], s->loc)
                                                 : argAddr(a, paramTypes[i], &dty);
      callArgs.push_back(arg);
      dummyTys.push_back(dty);
    }
    // Hidden extent/length args for `*`-extent array and CHAR(*) parameters
    // (rules (13),(18)), exactly as the synchronous external path marshals
    // them; without these the wrapper call would not match calleeFn.
    for (size_t i = 0; i < paramTypes.size(); ++i) {
      const Type& t = paramTypes[i];
      if (!((t.isArray() && !t.dims.empty() && t.dims[0].adj) || (t.isChar() && t.starLen)))
        continue;
      HExpr* a =
          (i >= s->args.size() || s->args[i]->kind == HExpr::Star) ? nullptr : s->args[i].get();
      llvm::Value* ext = hiddenAdjustArg(a, t, t.controlled);
      if (!ext) {
        SourceLoc loc = (i < s->args.size() && s->args[i]) ? s->args[i]->loc : s->loc;
        d_.error(loc, "a '*' extent parameter takes a fixed or dynamic-bound array in this stage",
                 "(13)");
        ext = i64(0);
      }
      callArgs.push_back(ext);
      dummyTys.push_back(nullptr);
    }
    appendStaticLinks(nullptr, callArgs);
    while (dummyTys.size() < callArgs.size())
      dummyTys.push_back(nullptr);
    emitAsyncSpawn(s, calleeF, callArgs, dummyTys);
    return;
  }
  llvm::Function* calleeF = calleeFn(calleeSym);
  std::vector<Symbol*> calleeParams =
      en ? en->entryParamSyms : (callee ? callee->paramSyms : std::vector<Symbol*>());

  // Marshal the callee arguments exactly as a synchronous call would.
  std::vector<llvm::Value*> callArgs;
  std::vector<llvm::Type*> dummyTys; // A9: copied actual carries its bytes in the ctx
  Type rty = en ? (en->entryIsFunction ? en->entryRetTy : Type::voidTy())
                : (callee ? callee->retTy : Type::voidTy());
  if (rty.isStruct() || rty.isChar()) {
    callArgs.push_back(rty.isChar() ? sretAlloc(rty) : entryAlloca(llvmTy(rty), "sret"));
    dummyTys.push_back(nullptr);
  }
  // Evaluate each actual once for this call (rule 78).
  ActualScope scope(*this);
  for (size_t i = 0; i < s->args.size(); ++i) {
    HExpr* a = s->args[i].get();
    Type pty;
    if (i < calleeParams.size())
      pty = calleeParams[i]->ty;
    else
      break;
    if (a->kind == HExpr::Star) {
      callArgs.push_back(llvm::Constant::getNullValue(b_.getPtrTy()));
      dummyTys.push_back(nullptr);
      continue;
    }
    llvm::Type* dty = nullptr;
    callArgs.push_back(argAddr(a, pty, &dty));
    dummyTys.push_back(dty);
  }
  if (en || callee)
    for (size_t i = s->args.size(); i < calleeParams.size(); ++i) {
      callArgs.push_back(llvm::Constant::getNullValue(b_.getPtrTy()));
      dummyTys.push_back(nullptr);
    }
  for (size_t i = 0; i < calleeParams.size(); ++i) {
    if (isAdjustable(calleeParams[i])) {
      HExpr* a =
          (i >= s->args.size() || s->args[i]->kind == HExpr::Star) ? nullptr : s->args[i].get();
      llvm::Value* ext = hiddenAdjustArg(a, calleeParams[i]->ty, calleeParams[i]->controlled);
      if (!ext)
        ext = i64(0); // diagnosed by sema through the sync path (13)
      callArgs.push_back(ext);
      dummyTys.push_back(nullptr);
    }
  }
  appendStaticLinks(callee, callArgs);
  while (dummyTys.size() < callArgs.size())
    dummyTys.push_back(nullptr);
  emitAsyncSpawn(s, calleeF, callArgs, dummyTys);
}

/* The common tail of emitAsyncCall: build the context layout, the per-site
 * wrapper, and spawn the task (with priority if PRIORITY was given). */
void IRGen::emitAsyncSpawn(HStmt* s, llvm::Function* calleeF,
                           std::vector<llvm::Value*> callArgs,
                           const std::vector<llvm::Type*>& dummyTys) {
  // PRIORITY (rule 79, ADR-298): evaluated and passed to the spawn
  // function so the task starts with that priority. A bare CALL (no
  // task option) reaches here only when PRIORITY is the sole option.
  llvm::Value* prioVal = i32(0);
  if (s->priorityExpr)
    prioVal = b_.CreateTrunc(toI64(emitExpr(s->priorityExpr.get())), b_.getInt32Ty(), "prio");

  llvm::Value* eventAddr = llvm::Constant::getNullValue(b_.getPtrTy());
  if (s->eventRef)
    eventAddr = taskEventAddr(s->eventRef.get(), s->loc);
  llvm::Value* taskAddr = nullptr;
  if (s->taskRef)
    taskAddr = taskEventAddr(s->taskRef.get(), s->loc);

  // Context layout: [event addr, ...callArgs in order]. A copied actual (A9)
  // stores its bytes inline so every task spawned from one call site sees the
  // value as it was at spawn time; a by-reference actual still stores its
  // pointer into the caller's frame.
  std::vector<llvm::Type*> fields = {b_.getPtrTy()};
  for (size_t k = 0; k < callArgs.size(); ++k)
    fields.push_back(k < dummyTys.size() && dummyTys[k] ? dummyTys[k]
                                                        : callArgs[k]->getType());
  llvm::StructType* ctxTy = llvm::StructType::get(ctx_, fields);

  // Per-site wrapper: unpack the context, call the procedure, free the
  // context, then complete the event. Completing last means a WAIT that
  // returns sees a task with no work left on its heap (the OS-thread model
  // may exit the process as soon as the waiter resumes).
  llvm::FunctionType* wft = llvm::FunctionType::get(b_.getPtrTy(), {b_.getPtrTy()}, false);
  llvm::Function* wrap = llvm::Function::Create(wft, llvm::Function::InternalLinkage,
                                                "pli_task_wrap_" + std::to_string(n_++), &mod_);
  llvm::BasicBlock* savedBB = b_.GetInsertBlock();
  llvm::BasicBlock* wbb = llvm::BasicBlock::Create(ctx_, "entry", wrap);
  b_.SetInsertPoint(wbb);
  // FINISH (rule 94; ADR-322): record the procedure this task was created
  // for. Its exit raises the condition; no other procedure's does.
  b_.CreateCall(runtimeFn("pli_task_finish_target"),
                {b_.CreateBitCast(calleeF, b_.getPtrTy(), "finprog")});
  llvm::Value* ctx = wrap->arg_begin();
  std::vector<llvm::Value*> wargs;
  for (size_t k = 0; k < callArgs.size(); ++k) {
    llvm::Value* fp = b_.CreateStructGEP(ctxTy, ctx, (unsigned)(k + 1), "carg");
    llvm::Type* dty = k < dummyTys.size() ? dummyTys[k] : nullptr;
    if (dty) {
      // Rebuild the copied actual on this task's own stack: the task must not
      // share the caller-frame dummy that the next spawn would overwrite.
      llvm::IRBuilder<> ab(wbb, wbb->begin());
      llvm::AllocaInst* dslot = ab.CreateAlloca(dty, nullptr, "task.dummy");
      b_.CreateMemCpy(dslot, llvm::MaybeAlign(), fp, llvm::MaybeAlign(),
                      i64((long long)mod_.getDataLayout().getTypeStoreSize(dty)));
      wargs.push_back(b_.CreateBitCast(dslot, b_.getPtrTy(), "tdarg"));
    } else {
      wargs.push_back(b_.CreateLoad(fields[k + 1], fp, "cval"));
    }
  }
  b_.CreateCall(calleeF, wargs);
  llvm::Value* evp = b_.CreateStructGEP(ctxTy, ctx, 0, "cev");
  llvm::Value* ev = b_.CreateLoad(b_.getPtrTy(), evp, "ev");
  b_.CreateCall(runtimeFn("pli_free"), {ctx});
  b_.CreateCall(runtimeFn("pli_event_complete"), {ev});
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
    llvm::Type* dty = k < dummyTys.size() ? dummyTys[k] : nullptr;
    if (dty)
      b_.CreateMemCpy(fp, llvm::MaybeAlign(), callArgs[k], llvm::MaybeAlign(),
                      i64((long long)mod_.getDataLayout().getTypeStoreSize(dty)));
    else
      b_.CreateStore(callArgs[k], fp);
  }
  if (s->eventRef)
    b_.CreateCall(runtimeFn("pli_event_reset"), {eventAddr});
  if (taskAddr)
    b_.CreateCall(runtimeFn("pli_task_note"), {taskAddr});
  // Load the task id (if any) so pli_task_spawn_p can store the priority in
  // the handle's registry entry. A bare TASK has no handle (id = 0).
  llvm::Value* taskId = i64(0);
  if (taskAddr) {
    llvm::Value* idVal = b_.CreateLoad(b_.getInt32Ty(), taskAddr, "tid");
    taskId = b_.CreateSExt(idVal, b_.getInt64Ty(), "tid64");
  }
  llvm::Value* fnPtr = b_.CreateBitCast(wrap, b_.getPtrTy(), "twrap");
  // Always the priority-aware entry: it stores the handle's fiber so a later
  // PRIORITY(t) can update a live task, and with no option it still inherits
  // the spawning task's priority (absolute = self + 0) (ADR-298).
  b_.CreateCall(runtimeFn("pli_task_spawn_p"), {fnPtr, ctxRaw, taskId, prioVal});
  pendingVarWrites_.clear(); // no sync point to copy a varying arg back
  // A9: copied actuals now live in the heap context, so their caller-frame
  // buffers are dead once the context is filled. By-reference actuals are not
  // lifetime-marked and stay live for the task (ADR-246).
  endTempLifetimes();
}

// Expand one WAIT operand into its event addresses (rule (82)). A whole
// static EVENT array contributes every element; a scalar, an element, or a
// member is one address. A dynamic/controlled array is diagnosed instead of
// silently waiting for only its first element.
void IRGen::appendWaitAddrs(HExpr* e, std::vector<llvm::Value*>& out, SourceLoc loc) {
  const Type& ty = e->ty;
  if (e->kind == HExpr::VarRef && ty.isArray()) {
    bool dynamic = e->sym && e->sym->controlled;
    for (const auto& d : ty.dims)
      if (d.dyn || d.lbDyn)
        dynamic = true;
    if (dynamic) {
      d_.error(loc, "WAIT over a dynamic EVENT array is not implemented in this stage", "(82)");
      return;
    }
    llvm::Value* base = taskEventAddr(e, loc);
    if (!base)
      return;
    long long n = arrayExtent(ty);
    llvm::Type* at = llvmTy(ty);
    for (long long i = 0; i < n; ++i) {
      llvm::Value* ep = b_.CreateInBoundsGEP(at, base, {i64(0), i64(i)}, "wev");
      out.push_back(ep);
    }
    return;
  }
  llvm::Value* addr = taskEventAddr(e, loc);
  if (addr)
    out.push_back(addr);
}

void IRGen::emitWait(HStmt* s) {
  // WAIT (rule (82)): no count waits for every event; WAIT(evs)(k) waits for
  // any k of them through pli_wait_n. An EVENT array operand expands to its
  // elements, so WAIT(ev) waits for all of them.
  std::vector<llvm::Value*> addrs;
  for (auto& e : s->waitEvents)
    appendWaitAddrs(e.get(), addrs, s->loc);
  if (!s->waitCount) {
    for (llvm::Value* addr : addrs)
      b_.CreateCall(runtimeFn("pli_event_wait"), {addr});
    return;
  }
  size_t n = addrs.size();
  if (n == 0)
    return;
  llvm::Value* arr = entryAlloca(llvm::ArrayType::get(b_.getPtrTy(), (unsigned)n), "wait.evs");
  for (size_t i = 0; i < n; ++i) {
    llvm::Value* ep = b_.CreateInBoundsGEP(llvm::ArrayType::get(b_.getPtrTy(), (unsigned)n), arr,
                                           {i64(0), i64((long long)i)}, "we");
    b_.CreateStore(addrs[i], ep);
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
    long long stride = 1;
    bool flatIsZero = true;
    llvm::Value* oob = subChecks() ? b_.getInt1(false) : nullptr;
    for (size_t k = nAxes; k-- > 0;) {
      const Dim& dk = arr.dims[k];
      llvm::Value* i = toI64(emitExpr(idxs[k].get()));
      llvm::Value* lb = k == 0 && dk.lbDyn ? dynLb : i64(dk.lb);
      llvm::Value* ub = k == 0 && dk.dyn ? dynUb : i64(dk.ub);
      // Bounds-check comparisons (lo/hi/oob) are dead when SUBSCRIPTRANGE
      // is off — emit none. ClampIndex is the slip resume (rule 94).
      llvm::Value* idx = i;
      if (subChecks()) {
        llvm::Value* lo = b_.CreateICmpSLT(i, lb, "lo");
        llvm::Value* hi = b_.CreateICmpSGT(i, ub, "hi");
        oob = b_.CreateOr(oob, b_.CreateOr(lo, hi, "oob"), "oob");
        idx = clampIndex(i, lb, ub);
      }
      // intent: row-major flat offset = sum_k (idx_k - lb_k) * stride_k
      llvm::Value* off = b_.CreateSub(idx, lb, "off");
      // skipMul: stride 1 needs no multiply (strength reduction P1.2)
      llvm::Value* contrib = (stride == 1) ? off
          : b_.CreateMul(off, i64(stride), "scaled");
      // skipAdd: initial accumulator is 0 — skip the add (strength reduction)
      if (flatIsZero) {
        flat = contrib;
      } else {
        flat = b_.CreateAdd(flat, contrib, "flat");
      }
      flatIsZero = false;
      stride *= (dk.ub - dk.lb + 1); // later axes are fixed; only axis 0 is runtime
    }
    if (subChecks()) {
      std::string id = std::to_string(n_++);
      llvm::BasicBlock* failL = llvm::BasicBlock::Create(ctx_, "sub.fail." + id, curFn_);
      llvm::BasicBlock* okL = llvm::BasicBlock::Create(ctx_, "sub.ok." + id, curFn_);
      b_.CreateCondBr(oob, failL, okL);
      startBlock(failL);
      emitCondTrap(Stmt::kSubscriptrangeCondKey, "pli_subscript_oob", okL);
      startBlock(okL);
    }
    return b_.CreateInBoundsGEP(llvmTy(el), base, {flat}, "aelem");
  }

  // Emit every index and OR the per-axis out-of-bounds flags into one check,
  // accumulating the row-major flat offset (last axis is contiguous).
  llvm::Value* flat = i64(0);
  long long stride = 1;
  bool flatIsZero = true;
  llvm::Value* oob = subChecks() ? b_.getInt1(false) : nullptr;
  for (size_t k = nAxes; k-- > 0;) {
    const long long lb = arr.dims[k].lb, ub = arr.dims[k].ub;
    llvm::Value* i = toI64(emitExpr(idxs[k].get()));
    // Bounds-check comparisons are dead when SUBSCRIPTRANGE is off — emit none.
    llvm::Value* idx = i;
    if (subChecks()) {
      llvm::Value* lo = b_.CreateICmpSLT(i, i64(lb), "lo");
      llvm::Value* hi = b_.CreateICmpSGT(i, i64(ub), "hi");
      oob = b_.CreateOr(oob, b_.CreateOr(lo, hi, "oob"), "oob");
      idx = clampIndex(i, i64(lb), i64(ub));
    }
    // intent: row-major flat offset = sum_k (idx_k - lb_k) * stride_k
    llvm::Value* off = b_.CreateSub(idx, i64(lb), "off");
    // skipMul: stride 1 needs no multiply (strength reduction P1.2)
    llvm::Value* contrib = (stride == 1) ? off
        : b_.CreateMul(off, i64(stride), "scaled");
    // skipAdd: initial accumulator is 0 — skip the add (strength reduction)
    if (flatIsZero) {
      flat = contrib;
    } else {
      flat = b_.CreateAdd(flat, contrib, "flat");
    }
    flatIsZero = false;
    stride *= (ub - lb + 1);
  }

  if (subChecks()) {
    std::string id = std::to_string(n_++);
    llvm::BasicBlock* failL = llvm::BasicBlock::Create(ctx_, "sub.fail." + id, curFn_);
    llvm::BasicBlock* okL = llvm::BasicBlock::Create(ctx_, "sub.ok." + id, curFn_);
    b_.CreateCondBr(oob, failL, okL);

    startBlock(failL);
    emitCondTrap(Stmt::kSubscriptrangeCondKey, "pli_subscript_oob", okL);

    startBlock(okL);
  }
  llvm::Type* arrTy = llvm::ArrayType::get(llvmTy(el), (unsigned)arrayExtent(arr));
  return b_.CreateInBoundsGEP(arrTy, base, {i64(0), flat}, "aelem");
}

// GEP to a struct member at field index `f` in struct type `cur`. For a UNION
// (ADR-202), all members share offset 0 — returns `addr` unchanged. For regular
// structs, emits CreateStructGEP to the sequential field offset.
llvm::Value* IRGen::structMemberGEP(const Type& cur, llvm::Value* addr, unsigned f, const char* name) {
  if (cur.isUnion)
    return addr; // union member: offset 0 (ADR-202)
  return b_.CreateStructGEP(llvmTy(cur), addr, f, name);
}

// Address of a qualified member S.A.B (rule 124): descend the recorded field
// indices one struct at a time with CreateStructGEP (the same API the
// varying-string addressing uses), so each GEP selects one field of the
// current struct type.
llvm::Value* IRGen::memberAddr(Symbol* base, const std::vector<unsigned>& path, SourceLoc) {
  llvm::Value* addr = addressOf(base);
  const Type* cur = &base->ty;
  for (unsigned f : path) {
    addr = structMemberGEP(*cur, addr, f, "mem");
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
    addr = structMemberGEP(*cur, addr, f, "mem");
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
     addr = structMemberGEP(*cur, addr, f, "lmem");
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
     // For UNION (ADR-202), all members are at offset 0
     llvm::Value* d = structMemberGEP(dst, dstBase, (unsigned)i, "bnm.d");
     llvm::Value* s = structMemberGEP(src, srcBase, (unsigned)j, "bnm.s");
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
        emitCondTrap(Stmt::kSubscriptrangeCondKey, "pli_subscript_oob", abL);
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
     llvm::Value* mem = structMemberGEP(ty, base, (unsigned)i, "init.mem");
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

// Stamp !alias.scope={own} + !noalias={all other function scopes} on one
// memory instruction for symbol s. Skips symbols without a scope (BASED,
// CONTROLLED, STATIC, DEFINED overlays, parameters, temps): those accesses
// stay unannotated and fall back to BasicAA, which is always sound.
void IRGen::annotateAlias(llvm::Instruction* inst, Symbol* s) {
  if (!inst || !s)
    return;
  auto it = aliasScopeOf_.find(s);
  if (it == aliasScopeOf_.end())
    return;
  llvm::MDNode* own = it->second;
  inst->setMetadata(llvm::LLVMContext::MD_alias_scope, llvm::MDNode::get(ctx_, {own}));
  if (aliasScopes_.size() > 1) {
    std::vector<llvm::Metadata*> others;
    others.reserve(aliasScopes_.size() - 1);
    for (llvm::MDNode* sc : aliasScopes_)
      if (sc != own)
        others.push_back(sc);
    inst->setMetadata(llvm::LLVMContext::MD_noalias, llvm::MDNode::get(ctx_, others));
  }
}

// Stamp !range on a FIXED BINARY(p) load (P3b): the declared precision bounds
// the value to a signed p-bit range, narrower than the i32/i64 storage. LLVM
// can then fold downstream overflow checks and narrow induction variables.
// Guard: only when sizeChecks() is off — at -O2/-O3 the optimizer already
// assumes no signed overflow, making the range fact consistent; with traps on
// the trap blocks own overflow control and the range would be unsound.
void IRGen::stampFixedBinRange(llvm::LoadInst* load, const Type& ty) {
  if (!load || ty.k != TK::FixedBin || sizeChecks())
    return;
  if (!load->getType()->isIntegerTy())
    return; // a whole array is copied; there is no scalar range to stamp
  unsigned typeWidth = load->getType()->getIntegerBitWidth();
  unsigned prec = ty.prec;
  if (prec <= 0 || prec >= typeWidth)
    return; // range fills the type — no narrowing
  // Signed p-bit range: [-(2^(p-1)), 2^(p-1)-1]; LLVM range is half-open.
  llvm::APInt lo = llvm::APInt::getSignedMinValue(prec).sextOrTrunc(typeWidth);
  llvm::APInt hi = llvm::APInt::getSignedMaxValue(prec).sextOrTrunc(typeWidth);
  hi += 1; // half-open upper bound, widened before increment to avoid prec-bit overflow
  llvm::MDBuilder mdb(load->getContext());
  load->setMetadata(llvm::LLVMContext::MD_range, mdb.createRange(lo, hi));
}

Val IRGen::loadSym(Symbol* sym, const Type& ty) {
  Val v;
  v.ty = ty;
  llvm::Value* addr = addressOf(sym);
  if (ty.isChar()) {
    // ADR-245: a fetch is an address plus a length, never a wider load —
    // the value cannot depend on a byte outside the declared extent
    // (MULTICS padded_ref).
    if (ty.varying) {
      llvm::Value* dp = b_.CreateStructGEP(llvmTy(ty), addr, 1, "vdata");
      llvm::Value* lp = b_.CreateStructGEP(llvmTy(ty), addr, 0, "vlenp");
      llvm::LoadInst* l32 = b_.CreateLoad(b_.getInt32Ty(), lp, "l32");
      annotateAlias(l32, sym);
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
   llvm::LoadInst* r = b_.CreateLoad(llvmTy(ty), addr, "ld");
  annotateAlias(r, sym);
  stampFixedBinRange(r, ty);
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

void IRGen::storeScalarTo(llvm::Value* addr, const Type& ty, const Val& v, bool packed,
                          Symbol* aliasSym) {
  if (ty.isComplex()) {
    // A complex value (QR2.2/CM5) is stored as the {double,double} struct.
    llvm::StoreInst* si = b_.CreateStore(v.cpxVal(), addr);
    if (packed)
      si->setAlignment(llvm::Align(1));
    annotateAlias(si, aliasSym);
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
  annotateAlias(si, aliasSym);
}

void IRGen::storeCharTo(llvm::Value* addr, const Type& dt, const Val& v, SourceLoc loc,
                        bool packed) {
  // ADR-245: the store is bounded by the declared extent — it writes the
  // target's own length and not one byte more, so a neighbouring variable
  // is untouched (MULTICS padded_for_store_ref).
  if (!v.ty.isChar()) {
    // A numeric value into a PICTURE target is picture-directed (ADR-290);
    // the formatted text is then an ordinary CHARACTER copy.
    if (dt.isChar() && !dt.picture.empty() && v.ty.isNumeric()) {
      Val cv = convert(v, dt, loc);
      storeCharTo(addr, dt, cv, loc, packed);
      return;
    }
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
        // Always reload fresh addr+len — may have changed in expBB. This is
        // the MULTICS save_temps contract (ADR-269): a transformation that
        // creates a new join must re-materialize the values it cached
        // across that edge, and the generation's address is the one value
        // here that can change.
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
  storeScalarTo(addr, dt, cv, false, sym);
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
    emitCondTrap(Stmt::kSubscriptrangeCondKey, "pli_subscript_oob", okL);
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
    emitCondTrap(Stmt::kSubscriptrangeCondKey, "pli_subscript_oob", okL);
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
  if (el.isArea() && sym->isStatic) {
    // A STATIC AREA array (rule (20)): every element owns a program-lifetime
    // region, created when that element is first used.
    llvm::Value* addr = arrayElementAddr(sym->ty, addressOf(sym), idxs, loc);
    v.reg = b_.CreateCall(runtimeFn("pli_area_lazy"), {addr, i64(sym->areaSize)}, "arealz");
    return v;
  }
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
     stampFixedBinRange(cast<llvm::LoadInst>(r), el);
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
        emitCondTrap(Stmt::kSubscriptrangeCondKey, "pli_subscript_oob", okL);
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

  // P4.2 bulk fast path: both views walk a trailing-axis run with the same
  // total, so the gather is one contiguous copy (rules (12),(126)). memmove
  // stays overlap-safe for self/overlaying slices; LLVM expands it inline.
  auto suffixRun = [](const View& vw) {
    const size_t n = vw.arr->dims.size();
    if (vw.stars.size() > n)
      return false;
    const size_t off = n - vw.stars.size();
    for (size_t j = 0; j < vw.stars.size(); ++j)
      if (vw.stars[j] != off + j)
        return false;
    return true;
  };
  if (!broadcast && tv.total == sv.total && tv.stars.size() == sv.stars.size() &&
      suffixRun(tv) && suffixRun(sv) && sv.arr->elementType() == el && !el.isChar() &&
      !el.isStruct() && !tv.arr->isDynamic() && !sv.arr->isDynamic() && !tv.packed &&
      !sv.packed && !t->locPtr && !x->locPtr && t->sym && x->sym && !t->sym->controlled &&
      !x->sym->controlled) {
    llvm::Value* dstPtr =
        b_.CreateInBoundsGEP(tgtArrTy, tv.base, {i64(0), tv.fixedFlat}, "csd");
    llvm::Value* srcPtr =
        b_.CreateInBoundsGEP(srcArrTy, sv.base, {i64(0), sv.fixedFlat}, "css");
    llvm::Value* nbytes = b_.CreateMul(
        i64(total), i64(mod_.getDataLayout().getTypeStoreSize(llvmTy(el))), "csn");
    b_.CreateMemMove(dstPtr, llvm::MaybeAlign(), srcPtr, llvm::MaybeAlign(), nbytes);
    return;
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
// array element selected by one subscript per base axis — a compile-time
// constant or a runtime expression that IBM re-evaluates at every reference to
// Y. Y has no storage of its own, so its address is a GEP into the base.
llvm::Value* IRGen::definedConstAddr(Symbol* sym) {
  Symbol* base = sym->definedBase;
  const Type& bty = base->ty;
  const size_t n = bty.dims.size();
  // One base index per base axis; a runtime subscript is bounds-checked
  // (rule 126) and clamped on a resumed SUBSCRIPTRANGE slip.
  std::vector<llvm::Value*> idx(n);
  for (size_t k = 0; k < n; ++k) {
    HExpr* he = k < sym->definedIdx.size() ? sym->definedIdx[k] : nullptr;
    if (!he) {
      idx[k] = i64(sym->definedConst[k]); // compile-time checked in sema
      continue;
    }
    llvm::Value* raw = toI64(emitExpr(he));
    const Dim& dd = bty.dims[k];
    const int ilb = dd.lb, iub = dd.ub;
    llvm::Value* oob = b_.CreateOr(b_.CreateICmpSLT(raw, i64(ilb), "lo"),
                                   b_.CreateICmpSGT(raw, i64(iub), "hi"), "oob");
    // A handled slip resumes with the base index clamped into range (rule 94);
    // (NOSUBSCRIPTRANGE) trusts the raw index and emits no check.
    idx[k] = subChecks() ? clampIndex(raw, i64(ilb), i64(iub)) : raw;
    if (subChecks()) {
      std::string id = std::to_string(n_++);
      llvm::BasicBlock* failL = llvm::BasicBlock::Create(ctx_, "def.fail." + id, curFn_);
      llvm::BasicBlock* okL = llvm::BasicBlock::Create(ctx_, "def.ok." + id, curFn_);
      b_.CreateCondBr(oob, failL, okL);
      startBlock(failL);
      emitCondTrap(Stmt::kSubscriptrangeCondKey, "pli_subscript_oob", okL);
      startBlock(okL);
    }
  }
  llvm::Value* flat = i64(0);
  long long stride = 1;
  for (size_t k = n; k-- > 0;) {
    flat = b_.CreateAdd(
        flat,
        b_.CreateMul(b_.CreateSub(idx[k], i64(bty.dims[k].lb), "o"), i64(stride), "s"), "f");
    stride *= (bty.dims[k].ub - bty.dims[k].lb + 1);
  }
  llvm::Type* arrTy = llvm::ArrayType::get(llvmTy(bty.elementType()), (unsigned)arrayExtent(bty));
  return b_.CreateInBoundsGEP(arrTy, addressOf(base), {i64(0), flat}, "defined");
}

// Address of a subscripted iSUB-DEFINED array element Y(i0,i1,...) (rules
// 134,126): Y is a live overlay of the base array X, so Y's element is the base
// element selected by one index per base axis — the mapped item subscript for
// an iSUB slot, the fixed constant otherwise. Each iSUB slot's base index is
// bounds-checked; fixed subscripts were compile-time checked.
llvm::Value* IRGen::definedSubElementAddr(Symbol* y, const std::vector<HExprP>& idxs, SourceLoc) {
  Symbol* base = y->definedBase;
  const Type& bty = base->ty;
  const size_t n = bty.dims.size();
  // Evaluate the item's subscripts once, in order: one dummy may drive several
  // base axes (X(1SUB,1SUB) overlays a diagonal), so no expression may run
  // twice.
  std::vector<llvm::Value*> yidx;
  yidx.reserve(idxs.size());
  for (const auto& ix : idxs)
    yidx.push_back(toI64(emitExpr(ix.get())));
  // One base index per base axis (rule 134 index arithmetic): an iSUB slot is
  // m*item_index + c, checked against the base axis it addresses.
  std::vector<llvm::Value*> bidx(n);
  for (size_t k = 0; k < n; ++k) {
    const int j = y->definedSlotAxis[k];
    if (j < 0) {
      bidx[k] = i64(y->definedConst[k]);
      continue;
    }
    const Dim& dd = bty.dims[k];
    const int ilb = dd.lb, iub = dd.ub;
    llvm::Value* raw =
        b_.CreateAdd(b_.CreateMul(yidx[(size_t)j], i64(y->definedSlotMult[k]), "m"),
                     i64(y->definedSlotAdd[k]), "c");
    llvm::Value* oob = b_.CreateOr(b_.CreateICmpSLT(raw, i64(ilb), "lo"),
                                   b_.CreateICmpSGT(raw, i64(iub), "hi"), "oob");
    // A handled slip resumes with the base index clamped into range (rule 94);
    // (NOSUBSCRIPTRANGE) trusts the raw index and emits no check.
    bidx[k] = subChecks() ? clampIndex(raw, i64(ilb), i64(iub)) : raw;
    if (subChecks()) {
      std::string id = std::to_string(n_++);
      llvm::BasicBlock* failL = llvm::BasicBlock::Create(ctx_, "def.fail." + id, curFn_);
      llvm::BasicBlock* okL = llvm::BasicBlock::Create(ctx_, "def.ok." + id, curFn_);
      b_.CreateCondBr(oob, failL, okL);
      startBlock(failL);
      emitCondTrap(Stmt::kSubscriptrangeCondKey, "pli_subscript_oob", okL);
      startBlock(okL);
    }
  }
  llvm::Value* flat = i64(0);
  long long stride = 1;
  for (size_t k = n; k-- > 0;) {
    flat = b_.CreateAdd(
        flat,
        b_.CreateMul(b_.CreateSub(bidx[k], i64(bty.dims[k].lb), "o"), i64(stride), "s"), "f");
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
    if (dst.isChar() && !dst.picture.empty() && v.ty.isNumeric())
      return convertToPicture(v, dst, loc);
    d_.error(loc,
             "conversion between " + v.ty.desc() + " and " + dst.desc() +
                 " is not implemented in this stage",
             "(86)");
    out.reg = i64(0);
    return out;
  }

  // A locator value is passed through unchanged between POINTER/OFFSET/AREA
  // targets (rules (15),(22),(20)): assignment copies the address, no numeric
  // conversion.
  if (v.ty.isLocator() && dst.isLocator())
    return v;

  // A whole-array value is an aggregate: sema required identical element
  // types for an array assignment (ADR-250), so the bytes copy unchanged and
  // there is no scalar slot to rescale.
  if (v.ty.isArray() && dst.isArray())
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
  if (v.ty.isLabel() && dst.isLabel())
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
    re.reg = b_.CreateExtractValue(v.cpxVal(), 0, "cpx.re");
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
      // A BIT(1) rides as i1, not as a byte array: widen it through the
      // integer path so the byte copy below never extracts from a scalar.
      if (v.ty.len == 1) {
        out.reg = unpackBitValue(b_.CreateZExt(v.reg, b_.getInt64Ty(), "b1w"), dst.len, loc);
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
    } else if (llvmTy(dst)->getIntegerBitWidth() == 128) {
      // A genuinely wide DECIMAL target (ADR-191) holds 20+ digits in i128;
      // a bit-string source is an unsigned magnitude that fits i64.
      out.reg = b_.CreateSExt(iv, llvmTy(dst), "cvt");
    } else {
      out.reg = b_.CreateTrunc(iv, llvmTy(dst), "cvt");
    }
    // A BINARY target narrower than its slot checks the declared precision.
    if (!dstFloat && dst.k == TK::FixedBin)
      out.reg = fixedBinPrecTrap(out.reg, dst.prec);
    return out;
  }
  if (srcFloat && dstFloat)
    return v;
  if (srcFloat && !dstFloat) { // FLOAT -> FIXED truncates toward zero
    // A FIXED DECIMAL target holds the value scaled by 10^q (ADR-006), a
    // FIXED BINARY target by 2^q (ADR-284), so scale the float up first.
    llvm::Value* f = v.reg;
    if (dst.k == TK::FixedDec && dst.scale > 0) {
      double sc = dst.scale <= 18 ? (double)pliPow10(dst.scale)
                                  : pli_to_double(pliPow10_128(dst.scale));
      f = b_.CreateFMul(f, flt(sc), "fsc");
    } else if (dst.k == TK::FixedBin && dst.scale > 0)
      f = b_.CreateFMul(f, flt((double)(1LL << dst.scale)), "fsc");
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
    // Use fptosi.sat (via emitFixedOfFloatLLVM) for 64-bit targets to avoid
    // LLVM 23 constant-folding CreateFPToSI(3.5, i64) to 4 instead of 3.
    if (llvmTy(dst)->getIntegerBitWidth() == 64) {
      out.reg = emitFixedOfFloatLLVM(f);
    } else {
      out.reg = b_.CreateFPToSI(f, llvmTy(dst), "cvt");
    }
    // A binary target narrower than its slot checks the declared precision.
    if (dst.k == TK::FixedBin)
      out.reg = fixedBinPrecTrap(out.reg, dst.prec);
    return out;
  }
  if (!srcFloat && dstFloat) {
    // A FIXED DECIMAL source holds the value scaled by 10^q, a FIXED BINARY
    // source by 2^q; divide back to the true value before converting.
    llvm::Value* f = b_.CreateSIToFP(v.reg, b_.getDoubleTy(), "cvt");
    if (v.ty.k == TK::FixedDec && v.ty.scale > 0) {
      double sc = v.ty.scale <= 18 ? (double)pliPow10(v.ty.scale)
                                   : pli_to_double(pliPow10_128(v.ty.scale));
      f = b_.CreateFDiv(f, flt(sc), "fds");
    } else if (v.ty.k == TK::FixedBin && v.ty.scale > 0)
      // A power of two is exact, so the multiply is the C5 exponent patch
      // (2^-q) rather than a divide (ADR-284, closing D2).
      f = b_.CreateFMul(f, flt(std::ldexp(1.0, -v.ty.scale)), "fds");
    out.reg = f;
    return out;
  }
  // FIXED -> FIXED. DECIMAL scale is 10^q, BINARY scale is 2^q (ADR-006,
  // ADR-284). A same-base rescale multiplies/divides by the base's power;
  // a cross-base conversion first recovers the value, then applies the
  // target scale. BINARY targets check declared precision; a DECIMAL target
  // checks magnitude after the rescale.
  if (v.ty.isFixed() && dst.isFixed()) {
    const bool srcBin = v.ty.k == TK::FixedBin;
    const bool dstBin = dst.k == TK::FixedBin;
    const bool srcDec = v.ty.k == TK::FixedDec;
    const bool dstDec = dst.k == TK::FixedDec;

    // Exact wide cross-base rescale (ADR-289): compute raw*2^q / 10^s in
    // i256, where the product of an i128 raw and 10^38 still fits. A scale
    // beyond the 128-bit decimal range (10^s no longer fits i128) keeps the
    // scaled-FLOAT path as the documented limit. Narrow cases stay exact
    // below.
    if ((srcBin && dstDec && (dst.intBits() == 128 || dst.prec > 18) && dst.scale > 38) ||
        (srcDec && dstBin && v.ty.intBits() == 128 && dst.scale > 0 && v.ty.scale > 38)) {
      Val f = convert(v, Type::flt(6), loc);
      return convert(f, dst, loc);
    }

    // Same-base same-scale copies only need a width change and a precision
    // check; every other pair goes through the rescale path.
    bool rescale = (srcDec && dstDec && v.ty.scale != dst.scale) ||
                   (srcBin && dstBin && v.ty.scale != dst.scale) ||
                   (srcDec && dstBin) || (srcBin && dstDec);
    int dqDec = (srcDec && dstDec) ? dst.scale - v.ty.scale : 0;
    bool decFits = srcDec && v.ty.prec + std::max(dqDec, 0) <= dst.prec;
    bool needDec = dstDec && dst.prec <= 18 && !decFits;
    long long limit = dstDec && dst.prec <= 18 ? pliPow10(dst.prec) : 0;

    // Fit an i128/i64 intermediate into the destination slot.
    auto fitFixed = [&](llvm::Value* r) -> llvm::Value* {
      unsigned db = (unsigned)dst.intBits();
      unsigned rb = r->getType()->getIntegerBitWidth();
      if (rb == db)
        return r;
      if (rb < db)
        return b_.CreateSExt(r, llvmTy(dst), "cvt");
      return b_.CreateTrunc(r, llvmTy(dst), "cvt");
    };

    if (!rescale) {
      if (needDec) {
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
      llvm::Value* sv = v.reg;
      if (dstBin)
        sv = fixedBinPrecTrap(sv, dst.prec);
      out.reg = fitFixed(sv);
      return out;
    }

    // A scale-up can exceed the source slot, so every rescaled path works
    // in i128; 2^q fits there for q < 63 and 10^q for the non-wide DECIMAL
    // targets handled below.
    if (srcBin && dstBin) {
      llvm::Value* r = b_.CreateSExt(v.reg, b_.getInt128Ty(), "res");
      int dq = dst.scale - v.ty.scale;
      if (dq > 0)
        r = b_.CreateMul(r, i128(pli_from_i64(1LL << dq)), "res");
      else if (dq < 0)
        r = b_.CreateSDiv(r, i128(pli_from_i64(1LL << -dq)), "res");
      r = fixedBinPrecTrap(r, dst.prec);
      out.reg = fitFixed(r);
      return out;
    }

    if (srcDec && dstBin) {
      if (v.ty.intBits() == 128) {
        // Wide DECIMAL source: raw*2^q can exceed i128, so apply the target
        // scale in i256 and divide by 10^s there; truncation matches the
        // narrow path (ADR-289).
        llvm::Type* wide = b_.getIntNTy(256);
        llvm::Value* r = b_.CreateSExt(v.reg, wide, "res");
        if (dst.scale > 0)
          r = b_.CreateMul(r, intPowConst(wide, 2, dst.scale), "res");
        if (v.ty.scale > 0)
          r = b_.CreateSDiv(r, intPowConst(wide, 10, v.ty.scale), "res");
        r = fixedBinPrecTrap(r, dst.prec);
        out.reg = fitFixed(r);
        return out;
      }
      // value*2^q: a non-wide DECIMAL's raw integer times 2^q fits i128
      // (< 10^18 * 2^62).
      llvm::Value* r = b_.CreateSExt(v.reg, b_.getInt128Ty(), "res");
      if (dst.scale > 0)
        r = b_.CreateMul(r, i128(pli_from_i64(1LL << dst.scale)), "res");
      if (v.ty.scale > 0)
        r = b_.CreateSDiv(r, i128(pliPow10_128(v.ty.scale)), "res");
      r = fixedBinPrecTrap(r, dst.prec);
      out.reg = fitFixed(r);
      return out;
    }

    if (srcBin && dstDec) {
      if (dst.intBits() == 128) {
        // Wide DECIMAL target: raw*10^s can exceed i128, so scale in i256,
        // round half away from zero (Y33-6003), trap at the declared
        // precision, then narrow (ADR-289).
        llvm::Type* wide = b_.getIntNTy(256);
        llvm::Value* r = b_.CreateSExt(v.reg, wide, "res");
        if (dst.scale > 0)
          r = b_.CreateMul(r, intPowConst(wide, 10, dst.scale), "res");
        int q = v.ty.scale;
        if (q > 0) {
          llvm::Value* div = intPowConst(wide, 2, q);
          llvm::Value* sign = b_.CreateSelect(
              b_.CreateICmpSLT(r, llvm::ConstantInt::get(wide, 0), "sgn"),
              llvm::ConstantInt::get(wide, -1), llvm::ConstantInt::get(wide, 1));
          llvm::Value* adj = b_.CreateMul(intPowConst(wide, 2, q - 1), sign, "adj");
          r = b_.CreateSDiv(b_.CreateAdd(r, adj, "rn"), div, "res");
        }
        wideMagTrap(r, dst.prec);
        out.reg = fitFixed(r);
        return out;
      }
      // value*10^s = raw*10^s/2^q; the target rounds half away from zero
      // (Y33-6003).
      llvm::Value* r = b_.CreateSExt(v.reg, b_.getInt128Ty(), "res");
      if (dst.scale > 0)
        r = b_.CreateMul(r, i128(pli_from_i64(pliPow10(dst.scale))), "res");
      int q = v.ty.scale;
      if (q > 0) {
        llvm::Value* div = i128(pli_from_i64(1LL << q));
        llvm::Value* sign = b_.CreateSelect(b_.CreateICmpSLT(r, i128(0), "sgn"), i128(-1), i128(1));
        llvm::Value* adj = b_.CreateMul(i128(pli_from_i64(1LL << (q - 1))), sign, "adj");
        r = b_.CreateSDiv(b_.CreateAdd(r, adj, "rn"), div, "res");
      }
      if (needDec)
        magTrap(r, limit);
      out.reg = fitFixed(r);
      return out;
    }

    // DECIMAL -> DECIMAL. dq > 0 multiplies by 10^dq; dq < 0 rounds half
    // away from zero (ADR-006).
    bool wide = std::max(v.ty.intBits(), dst.intBits()) == 128;
    llvm::Value* r = b_.CreateSExt(v.reg, wide ? b_.getInt128Ty() : b_.getInt64Ty(), "res");
    if (dqDec > 0) {
      // A scale-up that wraps already exceeds any target: trap, so the
      // magnitude check below never reads a wrapped value.
      r = checkedArith(Tok::Star, r, wide ? i128(pliPow10_128(dqDec)) : i64(pliPow10(dqDec)));
    } else if (dqDec < 0) {
      int k = -dqDec;
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
    }
    if (needDec)
      magTrap(r, limit);
    out.reg = fitFixed(r);
    return out;
  }
  return out;
}

// Numeric value -> PICTURE variable (rules (19),(146)-(148), ADR-290): the
// picture is the target CHARACTER(width)'s metadata, so the assignment edits
// the value with the same formatter as P output. The scaled-binary truncation
// mirrors the P path: the formatter works in decimal, the sign comes from the
// raw binary value so a negative fraction below one keeps its sign.
Val IRGen::convertToPicture(const Val& v, const Type& dst, SourceLoc loc) {
  const pli::PictureSpec spec = pli::classifyPicture(dst.picture);
  int width = spec.valid && spec.width > 0 ? spec.width : 1;
  Val out;
  out.ty = dst;
  out.ptr = charTemp(width).ptr;
  out.len = i64(width);
  if (!spec.valid || spec.width <= 0 || !spec.numeric) {
    d_.error(loc, "invalid PICTURE target", "(19)");
    return out;
  }
  if (v.ty.k == TK::FixedDec && v.ty.intBits() == 128) {
    if (spec.sterling) {
      d_.error(loc, "assignment of a wide FIXED DECIMAL to a sterling PICTURE is not implemented",
               "(53)");
      return out;
    }
    // A wide DECIMAL is edited by the 128-bit formatter core (B1).
    llvm::Value* lo = b_.CreateTrunc(v.reg, b_.getInt64Ty(), "picw.lo");
    llvm::Value* hi = b_.CreateTrunc(b_.CreateLShr(v.reg, (uint64_t)64, "picw.shr"),
                                     b_.getInt64Ty(), "picw.hi");
    llvm::Value* sign = b_.CreateSelect(b_.CreateICmpSLT(v.reg, i128(0), "picwneg"),
                                        b_.getInt32(-1), b_.getInt32(1), "picwsgn");
    b_.CreateCall(runtimeFn("pli_pic_format_wide"),
                  {lo, hi, i64(v.ty.scale), sign, globalString(spec.expanded),
                   i64((long long)spec.expanded.size()), out.ptr, out.len});
    return out;
  }
  llvm::Value* val = nullptr;
  llvm::Value* signSrc = nullptr;
  int scale = 0;
  if (v.ty.isFixed()) {
    val = toI64(v);
    signSrc = val;
    scale = v.ty.scale;
    if (v.ty.k == TK::FixedBin && v.ty.scale > 0) {
      // A binary scale holds raw/2^q; hand the formatter the value truncated
      // at the picture's decimal positions (ADR-288's P rule).
      const int d = spec.fracDigits;
      if (d > 18) {
        d_.error(loc, "a scaled FIXED BINARY PICTURE assignment supports at most 18 "
                      "fractional positions",
                 "(53)");
        return out;
      }
      llvm::Type* wideTy = b_.getInt128Ty();
      llvm::Value* wide = b_.CreateSExt(val, wideTy, "picw");
      wide = b_.CreateMul(wide, intPowConst(wideTy, 10, d), "picsc");
      wide = b_.CreateSDiv(wide, intPowConst(wideTy, 2, v.ty.scale), "picdv");
      val = b_.CreateTrunc(wide, b_.getInt64Ty(), "picdec");
      scale = d;
    }
  } else if (v.ty.k == TK::Float) {
    val = b_.CreateFPToSI(v.reg, b_.getInt64Ty(), "picf");
    signSrc = val;
  } else {
    d_.error(loc, "a PICTURE assignment requires a numeric value", "(53)");
    return out;
  }
  if (spec.sterling) {
    if (scale != 0) {
      d_.error(loc, "a sterling PICTURE assignment requires a whole-pence value", "(53)");
      return out;
    }
    llvm::Value* sign = b_.CreateSelect(b_.CreateICmpSLT(signSrc, i64(0), "picsgn"),
                                        b_.getInt32(-1), b_.getInt32(1), "picst");
    b_.CreateCall(runtimeFn("pli_pic_format_sterling"),
                  {val, sign, globalString(spec.expanded),
                   i64((long long)spec.expanded.size()), out.ptr, out.len});
    return out;
  }
  llvm::Value* neg = b_.CreateICmpSLT(signSrc, i64(0), "picsign");
  llvm::Value* sign = b_.CreateSelect(neg, b_.getInt32(-1), b_.getInt32(1), "picst");
  b_.CreateCall(runtimeFn("pli_pic_format"),
                {val, i64(scale), sign, globalString(spec.expanded),
                 i64((long long)spec.expanded.size()), out.ptr, out.len});
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
  // A wide DECIMAL (i128, ADR-191): narrow with a truncation. An SExt into
  // i64 is invalid IR, and the value does not fit the 64-bit path anyway.
  if (v.ty.intBits() == 128)
    return b_.CreateTrunc(v.reg, b_.getInt64Ty(), "i64");
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
    emitCharOfFloat(dst.ptr, dst.len, v.reg);
  } else {
    llvm::Value* iv = toI64(convert(v, Type::fixedBin(31, 0), e->loc));
    emitCharOfFixed(dst.ptr, dst.len, iv);
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
          // Branch-then-compute: sdiv only runs in computeBB where bi != 0,
          // eliminating the safe-divisor select from the hot path (T1a).
          llvm::Value* dz = b_.CreateICmpEQ(bi, i64(0), "zdiv");
          ZdivCheck zd = beginZerodivide(dz, i64(0), Stmt::kZerodivideCondKey);
          q = b_.CreateSDiv(ai, bi, "bin");
          q = endZerodivide(zd, q);
        }
        v.reg = convert(Val{Type::fixedBin(63, 0), q}, e->convTo, e->loc).reg;
        return v;
      }
    }
    return convert(emitExpr(e->a.get()), e->convTo, e->loc);
  }
  case HExpr::IntLit:
    v.ty = e->ty;
    // A >18-digit integer constant is genuinely wide (rule 135, ADR-191):
    // emit the full i128 wideIval like a wide DECIMAL literal, not the
    // saturated i64 ival.
    if (e->ty.intBits() == 128)
      v.reg = i128(e->wideIval);
    else
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
  case HExpr::TypeTag:
    // A ': ... :' designator (ADR-206/207/208) is consumed by its enquiry
    // built-in; reaching the value emitter means a lowering bug.
    d_.error(e->loc, "a ': ... :' designator is only valid in an enquiry built-in", "(123)");
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
      // A locator-qualified member P->S.A(i) (rules 124,126) addresses the
      // array through the locator value, not the declared BASED pointer.
      llvm::Value* base =
          e->locPtr
              ? locatorMemberAddr(e->sym, e->memberPath, emitExpr(e->locPtr.get()).reg)
              : (arr.isDynamic() ? dynamicMemberBase(e->sym, e->memberPath, e->loc, ub, lb)
                                 : memberAddr(e->sym, e->memberPath, e->loc));
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
    if (e->sym->definedBase && e->sym->hasIsubOverlay()) {
      llvm::Value* addr = definedSubElementAddr(e->sym, e->args, e->loc);
      const Type& el = e->ty;
      llvm::Value* r = b_.CreateLoad(llvmTy(el), addr, "defl");
      v.ty = el;
      v.reg = el.isBit() && el.len == 1 ? b_.CreateTrunc(r, b_.getInt1Ty(), "b1") : r;
      return v;
    }
    return loadArrayElement(e->sym, e->args, e->loc, e->locPtr.get());
  case HExpr::VarRef:
    if (e->isLabel) {
      // A label constant in value position (rule (21)): its LABEL value is the
      // address of the statement, the block address `ADDR(label)` yields, so
      // `L = LBL;` and an element of a label array can hold it.
      v.ty = e->ty;
      llvm::BasicBlock* bb = lookupLabelBlock(e->name);
      v.reg = (bb && curFn_) ? (llvm::Value*)llvm::BlockAddress::get(curFn_, bb)
                             : llvm::ConstantPointerNull::get(b_.getPtrTy());
      return v;
    }
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
      stampFixedBinRange(r, leaf);
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
      // Evaluate each actual once for this call (rule 78).
      ActualScope scope(*this);
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
      // convention as for a locally-defined procedure.
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
    // Evaluate each actual once for this call (rule 78).
    ActualScope scope(*this);
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
      // P6.1b (ADR-215): a by-value internal parameter marshals like a
      // by-value C entry; anything else keeps the address form.
      if (i < calleeParams.size() && isByValueParam(calleeParams[i]))
        args.push_back(marshalArg(a, pty, e->loc));
      else
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
    // A conjunction of two masked bit tests on the same value is one masked
    // test (ADR-243); the merge is tried before the two tests are emitted.
    if (op == Tok::Amp && emitCombinedBitTest(e, v))
      return v;
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
      // Up to 64 bits, pack the operands into one integer and do the logic
      // there: pack is a bijection on n-bit strings, so pack(a & b) is
      // pack(a) & pack(b). This keeps the operation in a single register
      // instead of one AND/OR per storage byte. Wider strings keep the
      // byte-array path.
      if (a.ty.len <= 64 && optimize_) {
        llvm::Value* x = packBitValue(a.reg, a.ty.len, e->loc);
        llvm::Value* y = packBitValue(b.reg, b.ty.len, e->loc);
        // Keep the operation in the narrowest register that holds the string:
        // a 32-bit string in i32 lets the loop vectorizer use 32-bit lanes,
        // the same width a C uint32_t uses.
        if (a.ty.len <= 32) {
          x = b_.CreateTrunc(x, b_.getInt32Ty(), "and32x");
          y = b_.CreateTrunc(y, b_.getInt32Ty(), "and32y");
        }
        llvm::Value* r = op == Tok::Amp ? b_.CreateAnd(x, y, "and") : b_.CreateOr(x, y, "or");
        v.ty = a.ty;
        v.reg = unpackBitValue(r, a.ty.len, e->loc);
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

  // A single masked bit test, (X & M) = E, compares packed integers rather
  // than materialising the byte-array AND (rules (115)-(117)).
  if ((op == Tok::Eq || op == Tok::Ne) && emitMaskedBitTest(e, v))
    return v;

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
        b_.CreateFCmp(llvm::CmpInst::FCMP_OEQ, b_.CreateExtractValue(ac.cpxVal(), 0, "cpx.er"),
                      b_.CreateExtractValue(bc.cpxVal(), 0, "cpx.er"), "cpx.er");
    llvm::Value* ei =
        b_.CreateFCmp(llvm::CmpInst::FCMP_OEQ, b_.CreateExtractValue(ac.cpxVal(), 1, "cpx.ei"),
                      b_.CreateExtractValue(bc.cpxVal(), 1, "cpx.ei"), "cpx.ei");
    llvm::Value* r = b_.CreateAnd(er, ei, "cpx.eq");
    if (op == Tok::Ne)
      r = b_.CreateNot(r, "cpx.ne");
    v.ty = Type::bit(1);
    v.reg = r;
    return v;
  }

  // A BIT operand is compared as a 64-bit integer, not a 32-bit one: the
  // packed value of a BIT(n) wider than 32 bits does not fit in 32, and
  // truncating it would compare only the low 32 bits (ADR-275).
  Type common = isCmp ? arithResultType(a.ty.isBit() ? Type::fixedBin(63, 0) : a.ty,
                                        b.ty.isBit() ? Type::fixedBin(63, 0) : b.ty)
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
    llvm::Value* ar = b_.CreateExtractValue(av.cpxVal(), 0, "cpx.ar");
    llvm::Value* ai = b_.CreateExtractValue(av.cpxVal(), 1, "cpx.ai");
    llvm::Value* br = b_.CreateExtractValue(bv.cpxVal(), 0, "cpx.br");
    llvm::Value* bi = b_.CreateExtractValue(bv.cpxVal(), 1, "cpx.bi");
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
      llvm::Value* lnMag = b_.CreateFMul(emitUnaryMath("pli_log", "llvm.log.f64", mag2, "cpx.ln"),
                                         llvm::ConstantFP::get(b_.getDoubleTy(), 0.5), "cpx.lnmag");
      llvm::Value* theta = emitBinaryMath("pli_atan2", "llvm.atan2.f64", ai, ar, "cpx.arg");
      llvm::Value* wr = b_.CreateFSub(b_.CreateFMul(br, lnMag), b_.CreateFMul(bi, theta), "cpx.wr");
      llvm::Value* wi = b_.CreateFAdd(b_.CreateFMul(br, theta), b_.CreateFMul(bi, lnMag), "cpx.wi");
      llvm::Value* ew = emitUnaryMath("pli_exp", "llvm.exp.f64", wr, "cpx.ew");
      llvm::Value* cosw = emitUnaryMath("pli_cos", "llvm.cos.f64", wi, "cpx.cos");
      llvm::Value* sinw = emitUnaryMath("pli_sin", "llvm.sin.f64", wi, "cpx.sin");
      rr = b_.CreateFMul(ew, cosw, "cpx.rr");
      ri = b_.CreateFMul(ew, sinw, "cpx.ri");
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
    // conversions instead (full widening stays D1/QR2). A FIXED DECIMAL result
    // longer than 31 digits raises FIXEDOVERFLOW (ADR-334).
    if (!flt && !isCmp && common.isFixed())
      r = checkedArith(op, av.reg, bv.reg, common.k == TK::FixedDec);
    else
      r = flt ? b_.CreateFAdd(av.reg, bv.reg, "bin") : b_.CreateAdd(av.reg, bv.reg, "bin");
    break;
  case Tok::Minus:
    if (!flt && !isCmp && common.isFixed())
      r = checkedArith(op, av.reg, bv.reg, common.k == TK::FixedDec);
    else
      r = flt ? b_.CreateFSub(av.reg, bv.reg, "bin") : b_.CreateSub(av.reg, bv.reg, "bin");
    break;
  case Tok::Star:
    if (!flt && !isCmp && common.isFixed())
      r = checkedArith(op, av.reg, bv.reg, common.k == TK::FixedDec);
    else
      r = flt ? b_.CreateFMul(av.reg, bv.reg, "bin") : b_.CreateMul(av.reg, bv.reg, "bin");
    break;
  case Tok::Slash: {
    if (flt) {
      // FLOAT division (ADR-014): fdiv, zero-divide resumes with 0.0.
      llvm::Value* fzero = llvm::ConstantFP::get(b_.getDoubleTy(), 0.0);
      llvm::Value* dz = b_.CreateFCmpOEQ(bv.reg, fzero, "zdiv");
      ZdivCheck zd = beginZerodivide(dz, fzero, Stmt::kZerodivideCondKey);
      llvm::Value* div = b_.CreateFDiv(av.reg, bv.reg, "bin");
      r = endZerodivide(zd, div);
    } else {
      // Integer division (P3, ADR-190): sdiv truncates toward zero, matching
      // PL/I's FIXED division semantics (rule 121). Operands widened so
      // CreateSDiv is well-defined (32-bit sdiv has narrower range); widened to
      // i64 normally, i128 for a wide (>18 digit) DECIMAL common (ADR-191).
      // ZERODIVIDE (rule 94): a zero divisor traps, resuming with 0 — guarded
      // by branching before the sdiv so the divisor is provably non-zero in the
      // compute block (no safe-divisor select on the hot path, rule 94).
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
        // Branch-then-compute: sdiv only runs when the divisor is non-zero,
        // eliminating the safe-divisor select from the hot path (T1a).
        llvm::Value* dz = b_.CreateICmpEQ(bi, z0, "zdiv");
        ZdivCheck zd = beginZerodivide(dz, z0, Stmt::kZerodivideCondKey);
        q = b_.CreateSDiv(ai, bi, "bin");
        q = endZerodivide(zd, q);
      }
      if (wide)
        r = q; // already at common's i128 width and scale
      else
        r = convert(Val{Type::fixedBin(63, 0), q}, common, e->loc).reg;
    }
    break;
  }
  case Tok::Power:
    // P4 fast path: x**2 with a constant integer exponent 2 is a single fmul,
    // exact for every finite base (a negative base still squares positive).
    if (e->b->kind == HExpr::IntLit && e->b->ival == 2) {
      r = b_.CreateFMul(av.reg, av.reg, "sq");
      break;
    }
    r = b_.CreateCall(
        intrinsicFn("llvm.pow.f64", b_.getDoubleTy(), {b_.getDoubleTy(), b_.getDoubleTy()}),
        {av.reg, bv.reg}, "bin");
    break;
  default:
    r = b_.CreateAdd(i64(0), i64(0));
    break;
  }
  // UNDERFLOW and OVERFLOW (rules 94; ADR-326/333): a floating-point operator
  // result is checked here; `*`, `/` and `**` pass their operands so a flush
  // to exact zero counts as underflow too.
  if (flt && !isCmp) {
    const bool mulDiv = op == Tok::Star || op == Tok::Slash || op == Tok::Power;
    r = applyUnderflow(Val{common, r}, mulDiv ? av.reg : nullptr,
                       mulDiv ? bv.reg : nullptr)
            .reg;
    r = applyOverflow(Val{common, r}).reg;
  }
  v.ty = common;
  v.reg = r;
  return v;
}
// Math transcendental helpers (W4b): emit LLVM intrinsic in llvm mode or
// pli_* call in runtime mode, gated by useRuntimeCall.
llvm::Value* IRGen::emitUnaryMath(const std::string& plifn, const std::string& intrinsic,
                                  llvm::Value* x, const llvm::Twine& name) {
  if (useRuntimeCall(plifn))
    return b_.CreateCall(runtimeFn(plifn), {x}, name);
  return b_.CreateCall(intrinsicFn(intrinsic, b_.getDoubleTy(), {b_.getDoubleTy()}), {x}, name);
}
llvm::Value* IRGen::emitBinaryMath(const std::string& plifn, const std::string& intrinsic,
                                   llvm::Value* x, llvm::Value* y, const llvm::Twine& name) {
  if (useRuntimeCall(plifn))
    return b_.CreateCall(runtimeFn(plifn), {x, y}, name);
  return b_.CreateCall(
      intrinsicFn(intrinsic, b_.getDoubleTy(), {b_.getDoubleTy(), b_.getDoubleTy()}), {x, y}, name);
}
// Emit a built-in function call (SUBSTR, INDEX, ABS, ...). Returns true
// if `e` is one of the recognised built-ins, filling `result`; false if
// it is a user function procedure (handled in emitExpr's general path).
// Extracted from the emitExpr Call case so each built-in is a
// self-contained block.
bool IRGen::emitBuiltin(HExpr* e, Val& result) {
  if (!e)
    return false;
  // The call site's location, for a remark raised inside this lowering
  // (ADR-254).
  curLoc_ = e->loc;
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
    // NULL (rule 123, Appendix 1): the null POINTER value; with an enquiry
    // designator (EXT, ADR-206) the typed null ENTRY/LABEL value instead.
    // Every one of these lowers to an opaque pointer, so one null serves all.
    v.ty = e->ty;
    v.reg = llvm::ConstantPointerNull::get(b_.getPtrTy());
    result = v;
    return true;
  }
  // DECLARESIZE (EXT, ADR-207): the sema-folded static storage byte count.
  if (e->name == "DECLARESIZE") {
    v.ty = e->ty;
    v.reg = llvm::ConstantInt::get(llvmTy(v.ty), (uint64_t)e->ival, false);
    result = v;
    return true;
  }
  // EPSILON/MAXVAL/MINVAL (EXT, ADR-208): double constants. Every FLOAT
  // lowers to a double, so one set of values serves every designator:
  // EPSILON is the double epsilon, MAXVAL the largest finite double,
  // MINVAL the smallest positive normalized double (Fortran TINY).
  if (e->name == "EPSILON" || e->name == "MAXVAL" || e->name == "MINVAL") {
    v.ty = e->ty;
    v.reg = flt(e->name == "EPSILON" ? DBL_EPSILON
                : e->name == "MAXVAL" ? DBL_MAX
                                      : DBL_MIN);
    result = v;
    return true;
  }
  // OFFSETADD/OFFSETSUBTRACT/OFFSETDIFF (Y33-6003, rule (22); ADR-311): an
  // OFFSET is an absolute address (ADR-170), so moving it is a non-inbounds
  // i8 GEP and the distance is a pointer-to-integer subtraction. The count
  // converts through the ordinary numeric path to FIXED BIN(31,0), so a
  // scaled decimal reconciles its scale before it is used as a byte count.
  if (e->name == "OFFSETADD" || e->name == "OFFSETSUBTRACT" ||
      e->name == "OFFSETDIFF") {
    llvm::Value* x = emitExpr(e->args[0].get()).reg;
    if (e->name == "OFFSETDIFF") {
      llvm::Value* y = emitExpr(e->args[1].get()).reg;
      llvm::Value* xa = b_.CreatePtrToInt(x, b_.getInt64Ty(), "offx");
      llvm::Value* ya = b_.CreatePtrToInt(y, b_.getInt64Ty(), "offy");
      v.ty = e->ty;
      v.reg = b_.CreateTrunc(b_.CreateSub(xa, ya, "offdiff"), b_.getInt32Ty(), "offd");
      result = v;
      return true;
    }
    Val count = convert(emitExpr(e->args[1].get()), Type::fixedBin(31, 0), e->loc);
    llvm::Value* n = toI64(count, e->loc);
    if (e->name == "OFFSETSUBTRACT")
      n = b_.CreateNeg(n, "offneg");
    v.ty = e->ty;
    v.reg = b_.CreateGEP(b_.getInt8Ty(), x, {n}, "off");
    result = v;
    return true;
  }
  // OFFSET(x, y) (rule (22); ADR-319): an OFFSET holds an absolute address
  // (ADR-170), so the conversion validates that x addresses a generation in
  // the region y and returns the same address.
  if (e->name == "OFFSET") {
    llvm::Value* p = emitExpr(e->args[0].get()).reg;
    llvm::Value* a = emitExpr(e->args[1].get()).reg;
    v.ty = e->ty;
    v.reg = b_.CreateCall(runtimeFn("pli_area_offset"), {a, p}, "off");
    result = v;
    return true;
  }
   if (e->name == "ADDR") {
     HExpr* a = e->args[0].get();
      if (a->kind == HExpr::VarRef && a->isLabel) {
        v.ty = e->ty;
        llvm::BasicBlock* bb = lookupLabelBlock(a->name);
        if (!bb || !curFn_) {
          v.reg = llvm::ConstantPointerNull::get(b_.getPtrTy());
        } else {
          v.reg = llvm::BlockAddress::get(curFn_, bb);
        }
        result = v;
        return true;
      }
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
      if (a->sym->definedBase && a->sym->hasIsubOverlay()) {
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
    // STRINGRANGE (rules (60)-(63),(94)): an invalid argument triple raises
    // before the clamped extraction when the condition is enabled here.
    if (strRangeChecks())
      emitStringrangeCheck(start, len, s.len);
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
      llvm::Value* re = b_.CreateExtractValue(a.cpxVal(), 0, "abre");
      llvm::Value* im = b_.CreateExtractValue(a.cpxVal(), 1, "abim");
      llvm::Value* m =
          b_.CreateFAdd(b_.CreateFMul(re, re, "abq1"), b_.CreateFMul(im, im, "abq2"), "abq");
      // Gate on the lowering policy like the math builtins (W4b).
      r = emitUnaryMath("pli_sqrt", "llvm.sqrt.f64", m, "abss");
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
  // EXPONENT (rule (123), Appendix 1): the binary exponent e such that
  // x = f * 2^e, 0.5 <= |f| < 1; 0 for 0.0 or NaN. Uses llvm.frexp.f64.
  if (e->name == "EXPONENT") {
    Val a = emitExpr(e->args[0].get());
    Val f = convert(a, Type::flt(6), e->loc);
    // frexp returns {mantissa, exponent}; EXPONENT is the exponent part.
    llvm::StructType* st = llvm::StructType::get(ctx_, {b_.getDoubleTy(), b_.getInt32Ty()});
    llvm::Value* pair =
        b_.CreateCall(intrinsicFn("llvm.frexp.f64.i32", st, {b_.getDoubleTy()}), {f.reg}, "exp");
    // frexp yields i32; EXPONENT wants FIXED BIN(63), widen to i64.
    llvm::Value* exp = b_.CreateExtractValue(pair, 1, "exp");
    llvm::Value* exp64 = b_.CreateSExt(exp, b_.getInt64Ty(), "exp64");
    v.ty = e->ty;
    v.reg = exp64;
    result = v;
    return true;
  }
  // Scalar math built-ins (QR2.7, Appendix 1, <math.h> analogues): FLOOR,
  // CEIL, SQRT, EXP, LOG, SIN, COS, TAN, LOG2, LOG10, ATAN, SINH, COSH, TANH,
  // ATANH, ERF, ERFC, ASIN, ACOS, CBRT, and the degree trig variants SIND,
  // COSD, TAND, ATAND. All variants map to LLVM intrinsics on LLVM 22+
  // (intrinsicFn) with a pli_* musl-port fallback for degree variants.
  // W4b: useRuntimeCall gates intrinsics vs pli_* so --experimental-lowering=runtime
  // forces runtime calls for differential testing.
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
    bool useRT = useRuntimeCall(kMathFn[ix]);
    if (intrinsic && !useRT)
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
    if (useRuntimeCall("pli_atan2"))
      v.reg = b_.CreateCall(runtimeFn("pli_atan2"), {y.reg, x.reg}, "atan2");
    else
      v.reg = b_.CreateCall(
          intrinsicFn("llvm.atan2.f64", b_.getDoubleTy(), {b_.getDoubleTy(), b_.getDoubleTy()}),
          {y.reg, x.reg}, "atan2");
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
    v.reg = b_.CreateExtractValue(z.cpxVal(), e->name == "REAL" ? 0 : 1, "part");
    result = v;
    return true;
  }
  if (e->name == "CONJG") {
    Val z = emitExpr(e->args[0].get());
    llvm::Value* re = b_.CreateExtractValue(z.cpxVal(), 0, "cgr");
    llvm::Value* im = b_.CreateExtractValue(z.cpxVal(), 1, "cgi");
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
      // Compute in the value's own width: a wide DECIMAL is i128, and
      // narrowing it to i64 first would drop the high digits (and an SExt
      // into i64 is invalid IR).
      llvm::Type* wt = llvmTy(a.ty);
      llvm::Value* p = fitTo(i64(pliPow10(a.ty.scale)), wt, "truncp");
      llvm::Value* t = b_.CreateSDiv(a.reg, p, "trunci");
      v.ty = a.ty;
      v.reg = b_.CreateMul(t, p, "truncd");
    } else if (a.ty.k == TK::FixedBin && a.ty.scale > 0) {
      // Drop the binary fraction toward zero, keeping the scaled type and
      // its q-bit zero tail: trunc(1.5) at scale 2 is 1.0, raw 4 (ADR-284).
      llvm::Type* wt = llvmTy(a.ty);
      llvm::Value* p = fitTo(i64(1LL << a.ty.scale), wt, "truncp");
      llvm::Value* t = b_.CreateSDiv(a.reg, p, "trunci");
      v.ty = a.ty;
      v.reg = b_.CreateMul(t, p, "truncd");
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
      ZdivCheck zd = beginZerodivide(dz, flt(0.0), Stmt::kZerodivideCondKey);
      llvm::Value* m = b_.CreateCall(runtimeFn("pli_mod_dd"), {av.reg, bv.reg}, "mod");
      v.reg = endZerodivide(zd, m);
    } else if (common.intBits() == 128) {
      // A wide DECIMAL (i128, ADR-191) keeps its own width: the i64 path
      // below drops the high digits, and an SExt into i64 is invalid IR.
      llvm::Type* wt = llvmTy(common);
      llvm::Value* zero = llvm::Constant::getNullValue(wt);
      llvm::Value* bi = bv.reg;
      // ZERODIVIDE (rule 94): a zero divisor traps, resuming with 0.
      llvm::Value* dz = b_.CreateICmpEQ(bi, zero, "zdiv");
      ZdivCheck zd = beginZerodivide(dz, zero, Stmt::kZerodivideCondKey);
      // PL/I MOD takes the sign of the divisor; srem gives the sign of the
      // dividend, so add the divisor back when the signs differ.
      llvm::Value* r = b_.CreateSRem(av.reg, bi, "mod");
      llvm::Value* adj = b_.CreateAdd(r, bi, "modadj");
      llvm::Value* nz = b_.CreateICmpNE(r, zero, "modnz");
      llvm::Value* rNeg = b_.CreateICmpSLT(r, zero, "modneg");
      llvm::Value* bNeg = b_.CreateICmpSLT(bi, zero, "modbneg");
      llvm::Value* signDiff = b_.CreateICmpNE(rNeg, bNeg, "modsd");
      llvm::Value* m = b_.CreateSelect(nz, b_.CreateSelect(signDiff, adj, r), r, "mods");
      v.reg = endZerodivide(zd, m);
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
          // intent: srem gives sign-of-dividend; PL/I MOD takes sign-of-divisor.
          // When the dividend is known non-negative and the divisor is positive,
          // srem already yields the correct non-negative result — skip the
          // adjust/select chain entirely (P1.3).
          llvm::SimplifyQuery SQ(mod_.getDataLayout());
          // intent: isKnownNonNegative with no DominatorTree/AssumptionCache
          // cannot see through load(sext) at IRGen time. Check if the dividend
          // is a constant, or derived from a DO index whose FROM is known
          // non-negative (P6.3). Otherwise fall back to isKnownNonNegative.
          bool avNonNeg = false;
          if (auto* avConst = llvm::dyn_cast<llvm::ConstantInt>(avi))
            avNonNeg = !avConst->getValue().isNegative();
          else if (isDerivedFromNonNegDoIndex(avi))
            avNonNeg = true;
          else
            avNonNeg = llvm::isKnownNonNegative(avi, SQ);
          if (cv.isPowerOf2()) {
            // intent: MOD(x, 2^k) for a positive power-of-two divisor is
            // x & (2^k - 1) for every sign: the mask already gives PL/I's
            // non-negative remainder, so no adjust/select chain is needed.
            uint64_t mask = cv.getZExtValue() - 1;
            m = b_.CreateAnd(avi, i64(mask), "modand");
          } else if (avNonNeg && cv.isStrictlyPositive()) {
            // intent: for a non-negative dividend and positive divisor, the
            // remainder is already correct.
            m = b_.CreateSRem(avi, divisor, "mod");
          } else {
            // General case: adjust when sign(r) != sign(divisor).
            llvm::Value* r = b_.CreateSRem(avi, divisor, "mod");
            llvm::Value* adj = b_.CreateAdd(r, divisor, "modadj");
            llvm::Value* nz = b_.CreateICmpNE(r, i64(0), "modnz");
            // Adjust when remainder is nonzero and sign(r) != sign(divisor).
            bool bNeg = cv.isNegative();
            llvm::Value* needAdj =
                bNeg ? b_.CreateICmpUGE(r, i64(0), "modpos") : b_.CreateICmpSLT(r, i64(0), "modneg");
            m = b_.CreateSelect(nz, b_.CreateSelect(needAdj, adj, r), r, "mods");
          }
        }
        } else {
        // General path: pli_mod_ll is AlwaysInline (Task 5), so LLVM can
        // inline and fold the body when the divisor is known at -O2+.
        llvm::Value* bi = toI64(bv);
        llvm::Value* dz = b_.CreateICmpEQ(bi, i64(0), "zdiv");
        ZdivCheck zd = beginZerodivide(dz, i64(0), Stmt::kZerodivideCondKey);
        m = b_.CreateCall(runtimeFn("pli_mod_ll"), {avi, bi});
        m = endZerodivide(zd, m);
      }
      // Keep the value's LLVM width in sync with e->ty for later compares.
      // The remainder must not be narrowed to 32 bits: a 64-bit common type
      // can hold a remainder that needs the full width.
      v.reg = common.intBits() == 32 ? b_.CreateTrunc(m, b_.getInt32Ty(), "mod32")
                                     : fitTo(m, llvmTy(common), "modw");
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
    // FIXED overflow (QR1.2): like Binary `*`, a wrapped product traps; a
    // FIXED DECIMAL product longer than 31 digits raises FIXEDOVERFLOW.
    llvm::Value* r = common.k == TK::Float ? b_.CreateFMul(av.reg, bv.reg, "mul")
                     : common.isFixed()    ? checkedArith(Tok::Star, av.reg, bv.reg,
                                                          common.k == TK::FixedDec)
                                           : b_.CreateMul(av.reg, bv.reg, "mul");
    v.ty = common;
    v.reg = r;
    if (common.k == TK::Float) {
      v.reg = applyUnderflow(v, av.reg, bv.reg).reg;
      v.reg = applyOverflow(v).reg;
    }
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
    ZdivCheck zd = beginZerodivide(dz, flt(0.0), Stmt::kZerodivideCondKey);
    llvm::Value* div = b_.CreateFDiv(av.reg, bv.reg, "div");
    v.reg = applyUnderflow(Val{common, endZerodivide(zd, div)}, av.reg, bv.reg).reg;
    v.reg = applyOverflow(Val{common, v.reg}).reg;
    result = v;
    return true;
  }
  // ADD/SUBTRACT (rule (123), Appendix 1): a+b / a-b in the common type, or
  // FIXED BINARY(p,s) when a precision/scale override is given. FIXED overflow
  // is checked (QR1.2); FLOAT goes through plain fadd/fsub.
  if (e->name == "ADD" || e->name == "SUBTRACT") {
    Val a = emitExpr(e->args[0].get());
    Val b = emitExpr(e->args[1].get());
    const Type& common = e->ty;
    Val av = convert(a, common, e->loc);
    Val bv = convert(b, common, e->loc);
    v.ty = common;
    if (common.k == TK::Float) {
      v.reg = e->name == "ADD" ? b_.CreateFAdd(av.reg, bv.reg, "add")
                               : b_.CreateFSub(av.reg, bv.reg, "sub");
    } else {
      Tok op = e->name == "ADD" ? Tok::Plus : Tok::Minus;
      v.reg = checkedArith(op, av.reg, bv.reg);
    }
    result = v;
    return true;
  }
  if (e->name == "ROUND") {
    // A FIXED DECIMAL argument rounded to a constant digit count stays exact:
    // the result is a decimal with that scale, and the conversion rounds half
    // away from zero (ADR-006) — the same rounding an assignment uses, and
    // the same one MULTICS's digit move produces (ADR-276).
    if (e->args[0]->ty.k == TK::FixedDec && e->ty.k == TK::FixedDec) {
      Val x = emitExpr(e->args[0].get());
      Val r = convert(x, e->ty, e->loc);
      v.ty = e->ty;
      v.reg = r.reg;
      result = v;
      return true;
    }
    Val x = emitExpr(e->args[0].get());
    Val n = emitExpr(e->args[1].get());
    Val xd = convert(x, Type::flt(6), e->loc);
    // The float path always produces a FLOAT, whatever the expression's type
    // says: a value typed as a decimal would be stored raw (ADR-276).
    v.ty = Type::flt(6);
    v.reg = b_.CreateCall(runtimeFn("pli_round"), {xd.reg, toI64(n)}, "round");
    result = v;
    return true;
  }
  if (e->name == "REPEAT") {
    // A constant string repeated a constant number of times is itself a
    // constant string (ADR-241): the text is built at compile time, so the
    // result is a constant instead of a runtime loop. The lowering policy
    // still wins: a forced runtime call keeps the library path, so the
    // differential oracle keeps both sides.
    HExpr* src = e->args[0].get();
    HExpr* cnt = e->args[1].get();
    if (src->kind == HExpr::CharLit && cnt->kind == HExpr::IntLit && cnt->ival > 0 &&
        !runtimePath("repeat")) {
      std::string text = pli::foldRepeatText(src->sval, cnt->ival);
      if (!text.empty() && (long long)text.size() == (long long)e->ty.len) {
        facts_.recordBuiltin("repeat", "llvm");
        Val out;
        out.ty = e->ty;
        out.ptr = globalString(text);
        out.len = i64(text.size());
        result = out;
        return true;
      }
    }
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
    llvm::Value* r;
    // A constant alphabet uses the membership table instead of a scan of the
    // alphabet (ADR-241). The lowering policy still wins: a forced runtime
    // call keeps the library path.
    const char* op = e->args.size() == 3 ? "verify_from" : "verify";
    HExpr* alpha = e->args[1].get();
    if (alpha->kind == HExpr::CharLit && !useRuntimeCall(op)) {
      llvm::Value* tbl = searchTable(alpha->sval);
      if (e->args.size() == 3) {
        Val st = emitExpr(e->args[2].get());
        r = emitVerifyFromTable(s.ptr, s.len, tbl, toI64(st));
      } else {
        r = emitVerifyTable(s.ptr, s.len, tbl);
      }
    } else {
      // The alphabet is only materialized when the scan needs it.
      Val t = emitExpr(e->args[1].get());
      if (e->args.size() == 3) {
        Val st = emitExpr(e->args[2].get());
        r = emitVerifyFrom(s.ptr, s.len, t.ptr, t.len, toI64(st));
      } else {
        r = emitVerify(s.ptr, s.len, t.ptr, t.len);
      }
    }
    v.ty = e->ty;
    v.reg = b_.CreateTrunc(r, b_.getInt32Ty(), "ver32");
    result = v;
    return true;
  }
  if (e->name == "TRANSLATE") {
    // A constant (out, in) pair becomes a 256-byte table at compile time
    // (ADR-273): one lookup per character instead of a scan of `in`. The
    // lowering policy still wins, so the differential oracle keeps both sides.
    HExpr* outArg = e->args[1].get();
    HExpr* inArg = e->args[2].get();
    if (outArg->kind == HExpr::CharLit && inArg->kind == HExpr::CharLit &&
        !runtimePath("translate")) {
      Val s = emitExpr(e->args[0].get());
      Val dst = charTemp(e->ty.len);
      emitTranslateTable(dst.ptr, dst.len, s.ptr, s.len, translateTable(outArg->sval, inArg->sval));
      facts_.recordBuiltin("translate", "llvm");
      dst.len = i64(e->ty.len);
      result = dst;
      return true;
    }
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
    // A constant alphabet uses the membership table instead of a scan of the
    // alphabet (ADR-241).
    llvm::Value* r;
    HExpr* alpha = e->args[1].get();
    if (alpha->kind == HExpr::CharLit && !useRuntimeCall("search"))
      r = emitSearchTable(s.ptr, s.len, searchTable(alpha->sval), toI64(st));
    else
      r = emitSearch(s.ptr, s.len, t.ptr, t.len, toI64(st));
    v.ty = e->ty;
    v.reg = b_.CreateTrunc(r, b_.getInt32Ty(), "srch32");
    result = v;
    return true;
  }
  // RANK(c) (rule (123)): code point of the first character, 0 when empty.
  if (e->name == "RANK") {
    Val s = emitExpr(e->args[0].get());
    v.ty = e->ty;
    if (useRuntimeCall("pli_rank")) {
      llvm::Value* r = b_.CreateCall(runtimeFn("pli_rank"), {s.ptr, s.len});
      v.reg = b_.CreateTrunc(r, b_.getInt32Ty(), "rnk32");
    } else {
      // Direct LLVM: if slen > 0, load and zext the first byte; else 0.
      llvm::Value* isNonempty = b_.CreateICmpUGT(s.len, i64(0), "rank.nonempty");
      llvm::Value* zero = b_.CreateSub(s.len, s.len, "rank.zero"); // constant 0
      llvm::Value* first = b_.CreateLoad(b_.getInt8Ty(), s.ptr, "rank.byte");
      llvm::Value* rankval = b_.CreateZExt(first, b_.getInt64Ty(), "rank.val");
      llvm::Value* result64 = b_.CreateSelect(isNonempty, rankval, zero, "rank.sel");
      v.reg = b_.CreateTrunc(result64, b_.getInt32Ty(), "rnk32");
    }
    result = v;
    return true;
  }
  // COLLATE(n) (rule (123)): char with code (n mod 256), blank-pad the tail.
  if (e->name == "COLLATE") {
    Val n = emitExpr(e->args[0].get());
    Val dst = charTemp(e->ty.len);
    v.ty = e->ty;
    if (useRuntimeCall("pli_collate")) {
      b_.CreateCall(runtimeFn("pli_collate"), {dst.ptr, dst.len, toI64(n)});
    } else {
      // Direct LLVM: store byte (n & 0xFF) at dst[0], memset rest with spaces.
      llvm::Value* byte = b_.CreateTrunc(toI64(n), b_.getInt8Ty(), "collate.byte");
      b_.CreateStore(byte, dst.ptr);
      if (e->ty.len > 1) {
        llvm::Value* tailPtr = b_.CreateGEP(b_.getInt8Ty(), dst.ptr, i64(1), "collate.tail");
        llvm::Value* tailLen = i64(e->ty.len - 1);
        b_.CreateMemSet(tailPtr, b_.getInt8(' '), tailLen, llvm::MaybeAlign(), false);
      }
    }
    dst.len = i64(e->ty.len);
    result = dst;
    return true;
  }
  if (e->name == "DATE" || e->name == "TIME" || e->name == "DATETIME") {
    Val out = charTemp(e->ty.len);
    std::string fn = e->name == "DATE" ? "pli_date"
                    : e->name == "TIME" ? "pli_time" : "pli_datetime";
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
  // The p/q form converts the source directly so a scaled float is not
  // truncated before the target scale is applied.
  if (e->name == "FIXED") {
    Val a = emitExpr(e->args[0].get());
    const bool hasPrec = e->args.size() >= 2;
    if (a.ty.isComplex()) {
      // FIXED(complex) keeps the pair and truncates each part toward zero.
      bool rt = useRuntimeCall("fixed_of_float");
      auto truncPart = [&](llvm::Value* x, const char* nm) -> llvm::Value* {
        if (rt)
          return b_.CreateCall(runtimeFn("pli_fixed_of_float"), {x}, nm);
        return emitFixedOfFloatLLVM(x);
      };
      llvm::Value* cpx = a.cpxVal();
      llvm::Value* re = b_.CreateExtractValue(cpx, 0, "fxc.re");
      llvm::Value* im = b_.CreateExtractValue(cpx, 1, "fxc.im");
      llvm::Value* reD = b_.CreateSIToFP(truncPart(re, "fxc.reI"), b_.getDoubleTy(), "fxc.reD");
      llvm::Value* imD = b_.CreateSIToFP(truncPart(im, "fxc.imI"), b_.getDoubleTy(), "fxc.imD");
      llvm::Value* s = llvm::UndefValue::get(llvmTy(Type::complexTy()));
      s = b_.CreateInsertValue(s, reD, 0, "fxc.cpx.re");
      s = b_.CreateInsertValue(s, imD, 1, "fxc.cpx.im");
      v.ty = e->ty;
      v.cpx = s;
      result = v;
      return true;
    }
    if (a.ty.isChar()) {
      // FIXED(char) parses a decimal point; the p/q form rescales directly into
      // the declared target (CONVERSION trap on no digits, rule 94).
      v = emitFixedFromChar(a.ptr, a.len, e->ty);
      result = v;
      return true;
    }
    if (a.ty.k == TK::Float) {
      // With an explicit scale, convert the float at full precision; the
      // one-argument form keeps the historical truncating helper.
      if (hasPrec) {
        v = convert(a, e->ty, e->loc);
        result = v;
        return true;
      }
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
    // FIXED(numeric): a FIXED DECIMAL/BINARY source converts to the declared
    // FIXED type via convert(), which handles scale rescaling (ADR-006).
    v = convert(a, e->ty, e->loc);
    result = v;
    return true;
  }
  // BINARY/DECIMAL/FLOAT/FIXEDBIN/FIXEDDEC (rule (123), Appendix 1):
  // numeric conversion with optional precision/scale override. Fixed targets
  // parse fractional character text directly (CONVERSION trap, ADR-170);
  // numeric input goes through convert(), which already handles float->fixed
  // scaling (ADR-006) and fixed->float back-scaling.
  if (e->name == "BINARY" || e->name == "DECIMAL" || e->name == "FLOAT" ||
      e->name == "FIXEDBIN" || e->name == "FIXEDDEC") {
    Val a = emitExpr(e->args[0].get());
    if (a.ty.isChar()) {
      // Fixed targets parse a decimal point and rescale directly.
      if (e->ty.isFixed()) {
        v = emitFixedFromChar(a.ptr, a.len, e->ty);
        result = v;
        return true;
      }
      // FLOAT(char) parses the same fractional text as the fixed targets.
      v = emitFloatFromChar(a.ptr, a.len, e->ty);
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
    if (a.ty.isComplex()) {
      // CHAR(complex): real image, signed imaginary image, I; the runtime
      // helper matches the list-directed complex rendering.
      Val dst = charTemp(e->ty.len);
      llvm::Value* cpx = a.cpxVal();
      llvm::Value* re = b_.CreateExtractValue(cpx, 0, "cpx.re");
      llvm::Value* im = b_.CreateExtractValue(cpx, 1, "cpx.im");
      b_.CreateCall(runtimeFn("pli_char_of_complex"), {dst.ptr, dst.len, re, im});
      dst.len = i64(e->ty.len);
      result = dst;
      return true;
    }
    Val dst = charTemp(e->ty.len);
    if (a.ty.k == TK::Float) {
      emitCharOfFloat(dst.ptr, dst.len, a.reg);
    } else {
      llvm::Value* iv = toI64(convert(a, Type::fixedBin(31, 0), e->loc));
      emitCharOfFixed(dst.ptr, dst.len, iv);
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
        emitCharOfFloat(cmd.ptr, cmd.len, a.reg);
      else {
        llvm::Value* iv = toI64(convert(a, Type::fixedBin(31, 0), e->loc));
        emitCharOfFixed(cmd.ptr, cmd.len, iv);
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
  // PRIORITY(t) (rule (79), ADR-298): read a task handle's priority as a
  // relative value (t minus self). The handle may be a scalar, an array
  // element t(i), or a member S.T; the runtime computes the difference.
  if (e->name == "PRIORITY") {
    HExpr* a = e->args[0].get();
    llvm::Value* idVal = i64(0);
    if (llvm::Value* idAddr = taskEventAddr(a, e->loc)) {
      llvm::Value* id32 = b_.CreateLoad(b_.getInt32Ty(), idAddr, "tid");
      idVal = b_.CreateSExt(id32, b_.getInt64Ty(), "tid64");
    }
    llvm::Value* pri = b_.CreateCall(runtimeFn("pli_task_priority_of"), {idVal}, "prio");
    v.ty = e->ty;
    // pli_task_priority_of returns i32 (ABI), FIXED BIN(15) is i32 — same width.
    v.reg = pri;
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
  // extent and reduce — SUM/PROD over numeric elements, ANY/ALL/EVERY/SOME over
  // BIT elements. The accumulator and counter live in entry allocas so they
  // survive the emitted loop's basic blocks. EVERY ≡ ALL, SOME ≡ ANY.
  if (e->name == "SUM" || e->name == "PROD" || e->name == "ANY" || e->name == "ALL" ||
      e->name == "EVERY" || e->name == "SOME") {
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
    const bool isBit = e->name == "ANY" || e->name == "ALL" ||
                       e->name == "EVERY" || e->name == "SOME";
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
    else if (e->name == "ANY" || e->name == "SOME")
      accInit = b_.getInt1(false);
    else
      accInit = b_.getInt1(true); // ALL or EVERY
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
    else if (e->name == "ANY" || e->name == "SOME")
      nv = b_.CreateOr(av, ev.reg, "rdo");
    else
      nv = b_.CreateAnd(av, ev.reg, "rda"); // ALL or EVERY
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
  // COPY (rule (123) Appendix 1): identical to REPEAT; reuse the same runtime
  // helper which has matching signature and semantics.
  if (e->name == "COPY") {
    Val s = emitExpr(e->args[0].get());
    Val n = emitExpr(e->args[1].get());
    Val out = charTemp(e->ty.len);
    emitRepeat(out.ptr, out.len, s.ptr, s.len, toI64(n));
    out.len = i64(e->ty.len);
    result = out;
    return true;
  }
  // ISOCHAR (IBM extension, ADR-193): fill dst[i] = (char)(lo + i).
  if (e->name == "ISOCHAR") {
    Val lo = emitExpr(e->args[0].get());
    Val hi = emitExpr(e->args[1].get());
    Val out = charTemp(e->ty.len);
    v.ty = e->ty;
    if (useRuntimeCall("pli_isochar")) {
      b_.CreateCall(runtimeFn("pli_isochar"),
                    {out.ptr, out.len, b_.CreateTrunc(toI64(lo), b_.getInt32Ty(), "isochar.lo"),
                     b_.CreateTrunc(toI64(hi), b_.getInt32Ty(), "isochar.hi")});
    } else {
      // Direct LLVM: fill dst[i] = (char)(lo + i) for i in [0, min(n, dstlen)),
      // blank-pad the tail. n = (lo <= hi) ? (hi - lo + 1) : 0.
      llvm::Value* lo64 = toI64(lo);
      llvm::Value* hi64 = toI64(hi);
      llvm::Value* loLeHi = b_.CreateICmpSLE(lo64, hi64, "isochar.lo_le_hi");
      llvm::Value* nRange = b_.CreateSub(hi64, lo64, "isochar.n_range");
      nRange = b_.CreateAdd(nRange, i64(1), "isochar.n");
      llvm::Value* zero = b_.CreateSub(lo64, lo64, "isochar.zero"); // 0
      llvm::Value* n = b_.CreateSelect(loLeHi, nRange, zero, "isochar.n_sel");
      llvm::Value* dstLen = i64(e->ty.len);
      llvm::Value* nClamped =
          b_.CreateSelect(b_.CreateICmpULT(n, dstLen), n, dstLen, "isochar.n_clamped");
      // Scalar loop: for i in [0, nClamped) store (char)(lo + i).
      llvm::BasicBlock* entryBB = b_.GetInsertBlock();
      llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(ctx_, "isochar.loop", curFn_);
      llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(ctx_, "isochar.body", curFn_);
      llvm::BasicBlock* afterBB = llvm::BasicBlock::Create(ctx_, "isochar.after", curFn_);
      b_.CreateBr(loopBB);
      b_.SetInsertPoint(loopBB);
      llvm::PHINode* iPhi = b_.CreatePHI(b_.getInt64Ty(), 2, "isochar.i");
      iPhi->addIncoming(i64(0), entryBB);
      llvm::Value* cond = b_.CreateICmpULT(iPhi, nClamped, "isochar.cond");
      b_.CreateCondBr(cond, bodyBB, afterBB);
      b_.SetInsertPoint(bodyBB);
      llvm::Value* code = b_.CreateAdd(lo64, iPhi, "isochar.code");
      llvm::Value* c = b_.CreateTrunc(code, b_.getInt8Ty(), "isochar.char");
      llvm::Value* elemPtr = b_.CreateGEP(b_.getInt8Ty(), out.ptr, iPhi, "isochar.ptr");
      b_.CreateStore(c, elemPtr);
      iPhi->addIncoming(b_.CreateAdd(iPhi, i64(1), "isochar.inc"), bodyBB);
      b_.CreateBr(loopBB);
      b_.SetInsertPoint(afterBB);
      // Blank-pad the tail (if any).
      if (e->ty.len > 0) {
        llvm::Value* tailPtr = b_.CreateGEP(b_.getInt8Ty(), out.ptr, nClamped, "isochar.pad");
        llvm::Value* padLen = b_.CreateSub(dstLen, nClamped, "isochar.padlen");
        b_.CreateMemSet(tailPtr, b_.getInt8(' '), padLen, llvm::MaybeAlign(), false);
      }
    }
    out.len = i64(e->ty.len);
    result = out;
    return true;
  }
  // BOOL (rule (123) Appendix 1): LLVM select on a BIT(1) condition.
  if (e->name == "BOOL") {
    Val c = emitExpr(e->args[0].get());
    Val t = emitExpr(e->args[1].get());
    Val f = emitExpr(e->args[2].get());
    v.ty = e->ty;
    if (e->ty.isChar()) {
      // Character branches: select the pointer/length pair.
      llvm::Value* selPtr = b_.CreateSelect(c.reg, t.ptr, f.ptr, "bool.ptr");
      llvm::Value* selLen = b_.CreateSelect(c.reg, t.len, f.len, "bool.len");
      v.ptr = selPtr;
      v.len = selLen;
    } else if (e->ty.isBit()) {
      v.reg = b_.CreateSelect(c.reg, t.reg, f.reg, "bool.sel");
    } else {
      v.reg = b_.CreateSelect(c.reg, t.reg, f.reg, "bool.sel");
    }
    result = v;
    return true;
  }
  // ONKEY (rule (123)): read whether a SIGNAL is currently being handled.
  if (e->name == "ONKEY") {
    v.ty = e->ty;
    // pli_onkey returns i32 (ABI), PL/I BIT(1) is i1 — truncate.
    v.reg = b_.CreateTrunc(b_.CreateCall(runtimeFn("pli_onkey"), {}, "onkey"),
                           llvmTy(v.ty), "onkey1");
    result = v;
    return true;
  }
  // ONSOURCE (rule (123)): read the current SIGNAL source procedure key.
  if (e->name == "ONSOURCE") {
    v.ty = e->ty;
    // pli_on_source returns i32 (ABI), FIXED BIN(15) is i32 — types match.
    v.reg = b_.CreateCall(runtimeFn("pli_on_source"), {}, "onsrc");
    result = v;
    return true;
  }
  // IDENTICAL (rule (123), ADR-199): IDENTICAL(a, b) -> BIT(1); 1 when a and b
  // hold identical values (element-wise for arrays, member-wise for
  // structures so padding bytes never participate), 0 otherwise. Static type
  // identity is required first; aggregates AND-fold into an i1 accumulator in
  // direct LLVM loops, like the reduction loop above but over pairs.
  if (e->name == "IDENTICAL") {
    HExpr* A = e->args[0].get();
    HExpr* B = e->args[1].get();
    // Differing static types are never identical (rule (123) Appendix 1).
    if (A->ty != B->ty) {
      v.ty = e->ty;
      v.reg = b_.getInt1(false);
      result = v;
      return true;
    }
    // One element-pair comparison between two addresses of element type el:
    // character elements compare with blank extension (rule (117)), wide BIT
    // byte by byte (packed big-endian), COMPLEX part-wise, FLOAT ordered.
    auto eqElem = [&](llvm::Value* epA, llvm::Value* epB, const Type& el,
                      bool packA, bool packB) -> llvm::Value* {
      if (el.isChar()) {
        llvm::Value* spA =
            b_.CreateBitCast(epA, llvm::PointerType::get(ctx_, 0), "ideq.ca");
        llvm::Value* spB =
            b_.CreateBitCast(epB, llvm::PointerType::get(ctx_, 0), "ideq.cb");
        return b_.CreateICmpEQ(emitCmpChar(spA, i64(el.len), spB, i64(el.len)), i32(0),
                               "ideq.c");
      }
      llvm::LoadInst* rlA = b_.CreateLoad(llvmTy(el), epA, "ideql.a");
      llvm::LoadInst* rlB = b_.CreateLoad(llvmTy(el), epB, "ideql.b");
      if (packA)
        rlA->setAlignment(llvm::Align(1));
      if (packB)
        rlB->setAlignment(llvm::Align(1));
      if (el.isBit() && el.len == 1)
        return b_.CreateICmpEQ(b_.CreateTrunc(rlA, b_.getInt1Ty(), "ideq.a1"),
                               b_.CreateTrunc(rlB, b_.getInt1Ty(), "ideq.b1"), "ideq.b");
      if (el.isBit()) {
        llvm::Value* w = b_.getInt1(true);
        for (int k = 0; k < bitBytes(el.len); ++k)
          w = b_.CreateAnd(w, b_.CreateICmpEQ(b_.CreateExtractValue(rlA, (unsigned)k, "ideq.ax"),
                                             b_.CreateExtractValue(rlB, (unsigned)k, "ideq.bx"),
                                             "ideq.bk"),
                           "ideq.ba");
        return w;
      }
      if (el.isComplex()) {
        llvm::Value* er = b_.CreateFCmp(llvm::CmpInst::FCMP_OEQ,
                                        b_.CreateExtractValue(rlA, 0, "ideq.ar"),
                                        b_.CreateExtractValue(rlB, 0, "ideq.br"), "ideq.er");
        llvm::Value* ei = b_.CreateFCmp(llvm::CmpInst::FCMP_OEQ,
                                        b_.CreateExtractValue(rlA, 1, "ideq.ai"),
                                        b_.CreateExtractValue(rlB, 1, "ideq.bi"), "ideq.ei");
        return b_.CreateAnd(er, ei, "ideq.cx");
      }
      if (el.k == TK::Float)
        return b_.CreateFCmp(llvm::CmpInst::FCMP_OEQ, rlA, rlB, "ideq.f");
      return b_.CreateICmpEQ(rlA, rlB, "ideq.i");
    };
    // A counted 0..bound loop running body(c) per index (rules (12),(13)).
    auto countedLoop = [&](llvm::Value* bound, const std::string& tag, auto&& body) {
      std::string lid = std::to_string(n_++);
      llvm::AllocaInst* ctr = entryAlloca(b_.getInt64Ty(), tag + ".i." + lid);
      b_.CreateStore(i64(0), ctr);
      llvm::BasicBlock* condL = llvm::BasicBlock::Create(ctx_, tag + ".cond." + lid, curFn_);
      llvm::BasicBlock* bodyL = llvm::BasicBlock::Create(ctx_, tag + ".body." + lid, curFn_);
      llvm::BasicBlock* stepL = llvm::BasicBlock::Create(ctx_, tag + ".step." + lid, curFn_);
      llvm::BasicBlock* endL = llvm::BasicBlock::Create(ctx_, tag + ".end." + lid, curFn_);
      branch(condL);
      startBlock(condL);
      llvm::Value* c = b_.CreateLoad(b_.getInt64Ty(), ctr, tag + "c");
      b_.CreateCondBr(b_.CreateICmpSLT(c, bound, tag + "cmp"), bodyL, endL);
      startBlock(bodyL);
      body(c);
      branch(stepL);
      startBlock(stepL);
      b_.CreateStore(b_.CreateAdd(c, i64(1), tag + "inc"), ctr);
      branch(condL);
      startBlock(endL);
    };
    // Aggregate-array case: both sides unsubscripted arrays. The fold starts
    // from the size-equality bit over min(nA, nB) elements, so a live-bound
    // mismatch on dynamic arrays yields false without overrunning (12),(13).
    if (A->ty.isArray() && B->ty.isArray()) {
      if (!A->sym || !B->sym) {
        v.ty = e->ty;
        v.reg = b_.getInt1(false);
        result = v;
        return true;
      }
      const bool mA = !A->memberPath.empty();
      const bool mB = !B->memberPath.empty();
      const Type& arrA = mA ? memberType(A->sym, A->memberPath) : A->sym->ty;
      const Type& arrB = mB ? memberType(B->sym, B->memberPath) : B->sym->ty;
      const Type& el = arrA.elementType();
      // Resolve one side's storage base, element count, and layout.
      auto side = [&](HExpr* X, const Type& arr, bool isMember, llvm::Value*& base,
                      llvm::Value*& n, llvm::Type*& arrTy, bool& dyn, bool& packed) {
        llvm::Value *ub = nullptr, *lb = nullptr;
        base = isMember ? (arr.isDynamic() ? dynamicMemberBase(X->sym, X->memberPath, e->loc, ub, lb)
                                           : memberAddr(X->sym, X->memberPath, e->loc))
                        : addressOf(X->sym);
        dyn = arr.isArray() && arr.isDynamic();
        packed = isMember && !dyn && packedMemberPath(X->sym->ty, X->memberPath);
        arrTy = nullptr;
        if (dyn) {
          const Dim& d0 = arr.dims[0];
          llvm::Value *mub = nullptr, *mlb = nullptr;
          if (isMember) {
            auto it = memberDyn_.find(MemberDyn{X->sym, X->memberPath});
            mub = it != memberDyn_.end() ? it->second.ub : nullptr;
            mlb = it != memberDyn_.end() ? it->second.lb : nullptr;
          }
          llvm::Value* rlb = d0.lbDyn ? (isMember ? (mlb ? mlb : i64(d0.lb))
                                                   : (dynLb_.count(X->sym) ? dynLb_[X->sym] : i64(d0.lb)))
                                      : i64(d0.lb);
          llvm::Value* rub = isMember ? (mub ? mub : i64(d0.ub))
                                      : (dynUb_.count(X->sym) ? dynUb_[X->sym] : i64(d0.ub));
          n = b_.CreateAdd(b_.CreateSub(rub, rlb, "idn1"), i64(1), "idn");
          long long rest = 1;
          for (size_t k = 1; k < arr.dims.size(); ++k)
            rest *= (arr.dims[k].ub - arr.dims[k].lb + 1);
          if (rest != 1)
            n = b_.CreateMul(n, i64(rest), "idnall");
        } else {
          n = i64(arrayExtent(arr));
          arrTy = llvm::ArrayType::get(llvmTy(el), (unsigned)arrayExtent(arr));
        }
      };
      llvm::Value *baseA, *baseB, *nA, *nB;
      llvm::Type *arrTyA, *arrTyB;
      bool dynA, dynB, packA, packB;
      side(A, arrA, mA, baseA, nA, arrTyA, dynA, packA);
      side(B, arrB, mB, baseB, nB, arrTyB, dynB, packB);
      std::string id = std::to_string(n_++);
      llvm::Value* sizeEq = b_.CreateICmpEQ(nA, nB, "ideq.n");
      llvm::Value* bound =
          b_.CreateSelect(b_.CreateICmpSLT(nA, nB, "ideq.min"), nA, nB, "ideq.b");
      llvm::AllocaInst* acc = entryAlloca(b_.getInt1Ty(), "ideq.acc." + id);
      b_.CreateStore(sizeEq, acc);
      countedLoop(bound, "ideq", [&](llvm::Value* c) {
        llvm::Value* epA = dynA ? b_.CreateInBoundsGEP(llvmTy(el), baseA, {c}, "ideqp.a")
                                : b_.CreateInBoundsGEP(arrTyA, baseA, {i64(0), c}, "ideqp.a");
        llvm::Value* epB = dynB ? b_.CreateInBoundsGEP(llvmTy(el), baseB, {c}, "ideqp.b")
                                : b_.CreateInBoundsGEP(arrTyB, baseB, {i64(0), c}, "ideqp.b");
        llvm::Value* eq = eqElem(epA, epB, el, packA, packB);
        b_.CreateStore(b_.CreateAnd(b_.CreateLoad(b_.getInt1Ty(), acc, "ideqacc"), eq, "ideq.a"),
                       acc);
      });
      v.ty = e->ty;
      v.reg = b_.CreateLoad(b_.getInt1Ty(), acc, "ideqres");
      result = v;
      return true;
    }
    // Structure case: both sides whole structures of one static type; fold an
    // AND over member values, never padding bytes (which carry no value).
    // Unions and dynamic/aggregate member arrays are diagnosed in sema.
    if (A->ty.isStruct() && !A->ty.isArray()) {
      Val a = emitExpr(A);
      Val b = emitExpr(B);
      if (!a.ptr || !b.ptr) {
        v.ty = e->ty;
        v.reg = b_.getInt1(false);
        result = v;
        return true;
      }
      const Type& rootA = A->sym ? A->sym->ty : A->ty;
      const Type& rootB = B->sym ? B->sym->ty : B->ty;
      std::string id = std::to_string(n_++);
      llvm::AllocaInst* acc = entryAlloca(b_.getInt1Ty(), "ideq.sacc." + id);
      b_.CreateStore(b_.getInt1(true), acc);
      // Recursive member fold; paths stay absolute from each side's root so
      // packed (UNALIGNED) loads use align 1 (ADR-169).
      auto fold = [&](auto&& self, llvm::Value* pA, llvm::Value* pB, const Type& ty,
                      std::vector<unsigned>& pathA,
                      std::vector<unsigned>& pathB) -> void {
        for (unsigned i = 0; i < ty.members.size(); ++i) {
          const Type& mt = ty.members[i]->ty;
          llvm::Value* fA = structMemberGEP(ty, pA, i, "ideq.fa");
          llvm::Value* fB = structMemberGEP(ty, pB, i, "ideq.fb");
          pathA.push_back(i);
          pathB.push_back(i);
          if (mt.isStruct()) {
            self(self, fA, fB, mt, pathA, pathB);
          } else if (mt.isArray()) {
            // Fixed array members compare element-wise (anything else nested
            // is diagnosed in sema).
            const Type& mel = mt.elementType();
            llvm::ArrayType* elArrTy =
                llvm::ArrayType::get(llvmTy(mel), (unsigned)arrayExtent(mt));
            bool pkA = packedMemberPath(rootA, pathA);
            bool pkB = packedMemberPath(rootB, pathB);
            countedLoop(i64(arrayExtent(mt)), "ideq.m", [&](llvm::Value* c) {
              llvm::Value* qA =
                  b_.CreateInBoundsGEP(elArrTy, fA, {i64(0), c}, "ideq.qa");
              llvm::Value* qB =
                  b_.CreateInBoundsGEP(elArrTy, fB, {i64(0), c}, "ideq.qb");
              b_.CreateStore(b_.CreateAnd(b_.CreateLoad(b_.getInt1Ty(), acc, "ideqmacc"),
                                          eqElem(qA, qB, mel, pkA, pkB), "ideq.ma"),
                             acc);
            });
          } else {
            bool pkA = packedMemberPath(rootA, pathA);
            bool pkB = packedMemberPath(rootB, pathB);
            b_.CreateStore(b_.CreateAnd(b_.CreateLoad(b_.getInt1Ty(), acc, "ideqsacc"),
                                        eqElem(fA, fB, mt, pkA, pkB), "ideq.sa"),
                           acc);
          }
          pathA.pop_back();
          pathB.pop_back();
        }
      };
      std::vector<unsigned> pathA = A->memberPath;
      std::vector<unsigned> pathB = B->memberPath;
      fold(fold, a.ptr, b.ptr, A->ty, pathA, pathB);
      v.ty = e->ty;
      v.reg = b_.CreateLoad(b_.getInt1Ty(), acc, "ideqsres");
      result = v;
      return true;
    }
    // Scalar case: one comparison in the operand's own domain (rule (117)).
    Val a = emitExpr(A);
    Val b = emitExpr(B);
    v.ty = e->ty;
    if (a.ty.isChar() && b.ty.isChar()) {
      v.reg = b_.CreateICmpEQ(emitCmpChar(a.ptr, a.len, b.ptr, b.len), i32(0), "ideq.c");
    } else if (a.ty.isComplex() || b.ty.isComplex()) {
      Val ac = convert(a, Type::complexTy(), e->loc);
      Val bc = convert(b, Type::complexTy(), e->loc);
      llvm::Value* er = b_.CreateFCmp(llvm::CmpInst::FCMP_OEQ,
                                      b_.CreateExtractValue(ac.cpxVal(), 0, "ideq.cr"),
                                      b_.CreateExtractValue(bc.cpxVal(), 0, "ideq.cr"), "ideq.er");
      llvm::Value* ei = b_.CreateFCmp(llvm::CmpInst::FCMP_OEQ,
                                      b_.CreateExtractValue(ac.cpxVal(), 1, "ideq.ci"),
                                      b_.CreateExtractValue(bc.cpxVal(), 1, "ideq.ci"), "ideq.ei");
      v.reg = b_.CreateAnd(er, ei, "ideq.cx");
    } else if (a.ty.isBit() && b.ty.isBit() && a.ty.len > 1) {
      // Wide BIT scalars compare byte by byte (packed big-endian).
      int nBytes = bitBytes(a.ty.len);
      llvm::Value* eq = b_.getInt1(true);
      for (int k = 0; k < nBytes; ++k)
        eq = b_.CreateAnd(eq, b_.CreateICmpEQ(b_.CreateExtractValue(a.reg, (unsigned)k, "ideq.wa"),
                                             b_.CreateExtractValue(b.reg, (unsigned)k, "ideq.wb"),
                                             "ideq.wk"),
                          "ideq.w");
      v.reg = eq;
    } else if (a.ty.k == TK::Float || b.ty.k == TK::Float) {
      Type common = arithResultType(a.ty.isBit() ? Type::fixedBin(31, 0) : a.ty,
                                   b.ty.isBit() ? Type::fixedBin(31, 0) : b.ty);
      Val av = convert(a, common, e->loc);
      Val bv = convert(b, common, e->loc);
      v.reg = b_.CreateFCmp(llvm::CmpInst::FCMP_OEQ, av.reg, bv.reg, "ideq.f");
    } else {
      Type common = arithResultType(a.ty.isBit() ? Type::fixedBin(31, 0) : a.ty,
                                   b.ty.isBit() ? Type::fixedBin(31, 0) : b.ty);
      Val av = convert(a, common, e->loc);
      Val bv = convert(b, common, e->loc);
      v.reg = b_.CreateICmpEQ(av.reg, bv.reg, "ideq.i");
    }
    result = v;
    return true;
  }
  // ONFILE (rule (123)): the file of the current I/O condition; blank when no
  // file condition is active (the only reachable state while ON ENDFILE/KEY
  // stays diagnosed, matching IBM's out-of-context null).
  if (e->name == "ONFILE") {
    Val out = charTemp(e->ty.len);
    b_.CreateCall(runtimeFn("pli_onfile"), {out.ptr, out.len});
    out.len = i64(e->ty.len);
    result = out;
    return true;
  }
  // CURRENTSIZE (rule (123)): the live size of a variable's buffer or element.
  if (e->name == "CURRENTSIZE") {
    Val a = emitExpr(e->args[0].get());
    v.ty = e->ty;
    if (a.ty.isChar()) {
      // Character argument: return the string length.
      v.reg = a.len;
    } else {
      // Numeric/bit/float: return the element's byte width from its type.
      int bits = a.ty.intBits();
      v.reg = i64((long long)(bits / 8));
    }
    result = v;
    return true;
  }
  // EMPTY (rule (123)): the null AREA handle.
  if (e->name == "EMPTY") {
    v.ty = e->ty;
    v.reg = b_.CreateCall(runtimeFn("pli_empty_area"), {}, "empty");
    result = v;
    return true;
  }
  // FILEOPEN (rule (123)): query whether file x is open.
  if (e->name == "FILEOPEN") {
    Val a = emitExpr(e->args[0].get());
    v.ty = e->ty;
    llvm::Value* open = b_.CreateCall(runtimeFn("pli_file_open_slot"), {toI64(a)}, "fopen");
    v.reg = b_.CreateTrunc(open, b_.getInt1Ty(), "fopen1");
    result = v;
    return true;
  }
  // LINENO (rule (123)): query the current line number of file x.
  if (e->name == "LINENO") {
    Val a = emitExpr(e->args[0].get());
    v.ty = e->ty;
    // pli_lineno returns i64 (ABI), FIXED BIN(31) is i32 — truncate.
    v.reg = b_.CreateTrunc(b_.CreateCall(runtimeFn("pli_lineno"), {toI64(a)}, "lineno"),
                           llvmTy(v.ty), "lineno32");
    result = v;
    return true;
  }
  // PAGENO (rule (123)): query the current page number of file x.
  if (e->name == "PAGENO") {
    Val a = emitExpr(e->args[0].get());
    v.ty = e->ty;
    // pli_pageno returns i64 (ABI), FIXED BIN(31) is i32 — truncate.
    v.reg = b_.CreateTrunc(b_.CreateCall(runtimeFn("pli_pageno"), {toI64(a)}, "pageno"),
                           llvmTy(v.ty), "pageno32");
    result = v;
    return true;
  }
  // UNSPEC (rule (123), ADR-196): UNSPEC(x) returns the raw storage bytes of x
  // as a BIT string. Materializes the storage bytes as a byte array value.
  if (e->name == "UNSPEC") {
    HExpr* a = e->args[0].get();
    v.ty = e->ty;
    llvm::Value* addr;
    llvm::Type* llvmSrcTy;
    if (a->kind == HExpr::VarRef && a->sym) {
      addr = addressOf(a->sym);
      llvmSrcTy = llvmTy(a->sym->ty);
    } else {
      Val av = emitExpr(a);
      addr = av.reg;
      llvmSrcTy = llvmTy(av.ty);
    }
    unsigned sz = (unsigned)mod_.getDataLayout().getTypeStoreSize(llvmSrcTy).getFixedValue();
    llvm::ArrayType* arrTy = llvm::ArrayType::get(b_.getInt8Ty(), sz);
    llvm::Value* bytePtr = b_.CreateBitCast(addr, llvm::PointerType::get(ctx_, 0), "unspec_ptr");
    v.reg = b_.CreateLoad(arrTy, bytePtr, "unspec_val");
    result = v;
    return true;
  }
  return false;
}
