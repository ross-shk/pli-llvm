// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// irgen_decisions.cpp — the fact store and its report (ADR-260).
//
// The store is deliberately dumb: it records what the compiler decided and
// prints it. It makes no decision of its own, so the report always agrees
// with the code that produced it.
#include "irgen_decisions.h"

#include <sstream>

#include "hir.h"
#include "sema.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <set>

namespace pli {

// Record one check's state; a repeated kind overwrites, so the report shows
// the effective setting rather than every assignment that led to it.
void FactStore::setCheck(const std::string& kind, bool on) {
  for (auto& c : checks_)
    if (c.first == kind) {
      c.second = on;
      return;
    }
  checks_.emplace_back(kind, on);
}

// Open a procedure for recording; a second beginProc for the same name
// continues the existing entry so a procedure emitted in pieces stays one.
void FactStore::beginProc(const std::string& name) {
  for (size_t i = 0; i < procs_.size(); ++i)
    if (procs_[i].name == name) {
      cur_ = static_cast<int>(i);
      return;
    }
  procs_.push_back(ProcFacts{name, {}});
  cur_ = static_cast<int>(procs_.size()) - 1;
}

// Record a procedure's verdict (ADR-264).
void FactStore::setEffects(const std::string& name, const std::string& effect,
                           const std::string& note, bool nounwind, bool willReturn) {
  for (auto& p : procs_)
    if (p.name == name) {
      p.effect = effect;
      p.effectNote = note;
      p.nounwind = nounwind;
      p.willReturn = willReturn;
      return;
    }
  ProcFacts f;
  f.name = name;
  f.effect = effect;
  f.effectNote = note;
  f.nounwind = nounwind;
  f.willReturn = willReturn;
  procs_.push_back(std::move(f));
}

// Record one built-in lowering decision, counting repeats of the same pair.
void FactStore::recordBuiltin(const std::string& op, const std::string& path) {
  if (cur_ < 0)
    return;
  ProcFacts& p = procs_[static_cast<size_t>(cur_)];
  for (auto& b : p.builtins)
    if (b.op == op && b.path == path) {
      b.count++;
      return;
    }
  p.builtins.push_back(BuiltinFact{op, path, 1});
}

// The report, one fact per line. The module facts come first, then one block
// per procedure with the facts it produced.
std::string FactStore::report() const {
  std::string out = "; pli facts\n";
  out += "module\n";
  out += "  lowering: " + loweringMode_ + "\n";
  if (!checks_.empty()) {
    out += "  checks:";
    for (const auto& c : checks_)
      out += " " + c.first + (c.second ? " on" : " off");
    out += "\n";
  }
  for (const auto& p : procs_) {
    if (p.builtins.empty() && p.effect.empty())
      continue;
    out += "proc " + p.name + "\n";
    if (!p.effect.empty()) {
      out += "  effect: " + p.effect;
      if (!p.effectNote.empty())
        out += " (" + p.effectNote + ")";
      out += "\n";
    }
    if (p.nounwind || p.willReturn) {
      out += "  control:";
      if (p.nounwind)
        out += " nounwind";
      if (p.willReturn)
        out += " willreturn";
      out += "\n";
    }
    for (const auto& b : p.builtins)
      out += "  builtin " + b.op + " -> " + b.path + " x" + std::to_string(b.count) + "\n";
  }
  return out;
}

// --- WS-A4: the PL/I alias taxonomy (ADR-265) -----------------------------

AliasClass aliasClass(const Symbol* s) {
  if (!s)
    return AliasClass::Unknown;
  // A BASED variable's storage is wherever its locator points.
  if (s->isBased())
    return AliasClass::Unknown;
  // A DEFINED overlay shares its base's storage.
  if (s->definedBase)
    return AliasClass::Shares;
  // CONTROLLED storage lives on the generation stack.
  if (s->controlled)
    return AliasClass::Shares;
  // Module-wide storage: an EXTERNAL variable, or a STATIC one.
  if (s->external || s->isStatic)
    return AliasClass::Shares;
  // A parameter is the caller's storage, or a copy of it.
  if (s->kind == Symbol::Param)
    return AliasClass::Shares;
  // A procedure name is a procedure value, and an ENTRY VARIABLE holds one
  // that can be called from anywhere (ADR-171).
  if (s->kind == Symbol::ProcName || s->isEntryVar)
    return AliasClass::Shares;
  // A plain AUTOMATIC local owns its storage.
  return AliasClass::Disjoint;
}

// --- WS-A1: the memory effects of a PL/I procedure (ADR-264) --------------
//
// The analysis walks each procedure's HIR body once and records every memory
// access it makes, grouped by where the access lands. The verdict reaches LLVM
// only as a function attribute (the plan's C1: no custom pass) and the whole
// computation lives here, outside irgen.cpp (the plan's C2).

namespace {

// Where a memory access lands, seen from the caller of the procedure.
enum class Reach {
  Frame, // the procedure's own automatic storage — invisible outside the call
  Arg,   // reachable from one of the procedure's pointer arguments
  Other, // anything else: a global, an escaped pointer, unknown memory
};

// The wider of two reaches wins: Other > Arg > Frame.
Reach wider(Reach a, Reach b) {
  if (a == Reach::Other || b == Reach::Other)
    return Reach::Other;
  if (a == Reach::Arg || b == Reach::Arg)
    return Reach::Arg;
  return Reach::Frame;
}

// The operators that compute a new fixed-point value and can overflow.
bool isArithOp(Tok op) {
  return op == Tok::Plus || op == Tok::Minus || op == Tok::Star || op == Tok::Power;
}

// What a built-in call does to memory.
enum class BuiltinEffect {
  Constant,  // touches no memory at all
  Pure,      // a function of its arguments: reads their storage, writes nothing
  Allocates, // allocates: writes memory outside the frame
  Escapes,   // hands out the address of its argument
  Unknown,   // anything the table does not cover
};

// The built-ins the compiler knows, grouped by what they do to memory. A name
// in none of the groups is Unknown: the calling procedure then claims nothing.
BuiltinEffect builtinEffect(const std::string& op) {
  static const std::set<std::string> constants = {
      "DECLARESIZE", "EPSILON", "MAXVAL", "MINVAL", "NULL",  "OMITTED", "PRESENT",
      "PRECISION",   "REAL",    "IMAG",   "EXPONENT", "ABS",  "SIGN",    "MAX",
      "MIN",         "TRUNC",   "ROUND",  "CEIL",     "FLOOR", "ACOS",    "ASIN",
      "ATAN",        "ATAN2",   "ATAND",  "ATANH",    "CBRT", "COS",     "COSD",
      "COSH",        "ERF",     "ERFC",   "EXP",      "LOG",  "LOG10",   "LOG2",
      "SIN",         "SIND",    "SINH",   "SQRT",     "TAN",  "TAND",    "TANH",
      "COMPLEX",     "CONJG"};
  static const std::set<std::string> readers = {"INDEX",  "LENGTH", "SUBSTR",   "VERIFY",
                                                "SEARCH", "TRANSLATE", "MOD",  "ADD",
                                                "SUBTRACT", "MULTIPLY", "DIVIDE"};
  static const std::set<std::string> allocators = {"REPEAT", "REVERSE"};
  static const std::set<std::string> escapers = {"ADDR", "BYADDR"};
  if (constants.count(op))
    return BuiltinEffect::Constant;
  if (readers.count(op))
    return BuiltinEffect::Pure;
  if (allocators.count(op))
    return BuiltinEffect::Allocates;
  if (escapers.count(op))
    return BuiltinEffect::Escapes;
  return BuiltinEffect::Unknown;
}

// The facts one procedure accumulates about itself. Every bit is monotone (it
// only ever turns on), so the whole-program fixpoint terminates.
struct Bits {
  bool readsOther = false;
  bool writesOther = false;
  bool readsArg = false;
  bool writesArg = false;
  bool opaque = false;     // effects unknown: nothing may be claimed
  bool io = false;         // does input or output
  bool hasOnUnit = false;  // establishes an ON unit (rule 91)
  bool nameEscapes = false; // its name is assigned or passed as an argument
  bool mayUnwind = false;  // makes a call whose unwinding is not known
  bool mayNotReturn = false; // may not return: a stop, or an endless loop
  std::set<int> raiseKeys; // the conditions it can raise
  std::string note;        // why the effect is not stronger

  bool operator==(const Bits& o) const {
    return readsOther == o.readsOther && writesOther == o.writesOther &&
           readsArg == o.readsArg && writesArg == o.writesArg && opaque == o.opaque &&
           io == o.io && hasOnUnit == o.hasOnUnit && nameEscapes == o.nameEscapes &&
           mayUnwind == o.mayUnwind && mayNotReturn == o.mayNotReturn &&
           raiseKeys == o.raiseKeys;
  }
};

// Record an access of the given reach.
void noteReach(Bits& b, Reach r, bool isWrite) {
  if (r == Reach::Other) {
    if (isWrite)
      b.writesOther = true;
    else
      b.readsOther = true;
  } else if (r == Reach::Arg) {
    if (isWrite)
      b.writesArg = true;
    else
      b.readsArg = true;
  }
  // A Frame access is invisible outside the call: it constrains nothing.
}

// Why the effect is not stronger: the first fact that stops the claim.
std::string effectNote(const Bits& b) {
  if (b.hasOnUnit)
    return "establishes an ON unit";
  if (b.nameEscapes)
    return "its name escapes";
  if (b.io)
    return "does input or output";
  if (b.opaque)
    return "calls something whose effects are unknown";
  if (b.writesOther)
    return "writes memory outside its arguments";
  if (b.readsOther)
    return "reads memory outside its arguments";
  if (b.writesArg)
    return "writes its arguments";
  if (b.readsArg)
    return "reads its arguments";
  return "touches no memory";
}

// One procedure's body, walked once to find every memory access it makes.
class EffectWalker {
public:
  EffectWalker(const HProc* p, const std::unordered_set<const Symbol*>* byValue,
               const EffectOptions* opts, const std::map<const Proc*, const HProc*>* bySrc,
               std::set<int>* handlerKeys)
      : p_(p), byValue_(byValue), opts_(opts), bySrc_(bySrc), handlerKeys_(handlerKeys) {}

  Bits run();
  // The procedures whose names escaped while walking this body.
  const std::set<const HProc*>& escaped() const { return escaped_; }
  // The internal procedures this one calls, with the reach of what it passed.
  const std::vector<std::pair<const HProc*, Reach>>& calls() const { return calls_; }

private:
  Reach reachOf(Symbol* s) const;
  Reach reachOfExpr(HExpr* e) const;
  Reach callArgReach(const HProc* callee, const std::vector<HExprP>& args) const;
  void note(Reach r, bool isWrite) { noteReach(b_, r, isWrite); }
  void walkStmt(HStmt* st);
  void walkExpr(HExpr* e, bool writeCtx);
  void walkCallExpr(HExpr* e, bool writeCtx);
  void walkCallStmt(HStmt* st);
  void walkArgs(const std::vector<HExprP>& args);
  void markEscape(Symbol* s);
  void raiseSize() {
    if (opts_->sizeChecks)
      b_.raiseKeys.insert(Stmt::kSizeCondKey);
  }

  const HProc* p_;
  const std::unordered_set<const Symbol*>* byValue_;
  const EffectOptions* opts_;
  const std::map<const Proc*, const HProc*>* bySrc_;
  std::set<int>* handlerKeys_;
  Bits b_;
  std::set<const HProc*> escaped_;
  std::vector<std::pair<const HProc*, Reach>> calls_;
};

// Where a symbol's storage lives, from the caller's point of view.
Reach EffectWalker::reachOf(Symbol* s) const {
  if (!s)
    return Reach::Other;
  // A procedure name is not storage.
  if (s->kind == Symbol::ProcName)
    return Reach::Frame;
  // Module-wide or generation-stack storage: visible to everyone.
  if (s->external || s->isStatic || s->controlled)
    return Reach::Other;
  // A BASED variable's address is the locator's value: it points anywhere.
  if (s->isBased())
    return Reach::Other;
  // A DEFINED overlay aliases its base, whose storage may be anywhere.
  if (s->definedBase)
    return Reach::Other;
  // Module-level storage (a package member) is a global.
  if (!s->owner || s->owner->isPackage)
    return Reach::Other;
  // A variable of an enclosing procedure arrives through a static link, which
  // is a pointer argument of this procedure.
  if (s->owner != p_->src)
    return Reach::Arg;
  // A by-value parameter's storage is this frame's own cell.
  if (s->kind == Symbol::Param)
    return byValue_->count(s) ? Reach::Frame : Reach::Arg;
  // A plain AUTOMATIC local of this procedure.
  return Reach::Frame;
}

// The reach of an expression used as a value: a variable's storage, or the
// frame when the expression computes a value of its own.
Reach EffectWalker::reachOfExpr(HExpr* e) const {
  if (!e)
    return Reach::Frame;
  if (e->kind == HExpr::VarRef || e->kind == HExpr::Subscript) {
    if (e->locPtr)
      return Reach::Other; // a locator-qualified reference reaches anywhere
    return reachOf(e->sym);
  }
  return Reach::Frame;
}

// The reach of what a call hands the callee. The by-reference actuals decide:
// a by-value actual is copied into the callee's own frame, so the callee never
// reaches the caller's storage through it.
Reach EffectWalker::callArgReach(const HProc* callee, const std::vector<HExprP>& args) const {
  Reach r = Reach::Frame;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i < callee->paramSyms.size() && byValue_->count(callee->paramSyms[i]))
      continue;
    r = wider(r, reachOfExpr(args[i].get()));
  }
  return r;
}

// A procedure's name that is assigned or handed to somebody else: the
// procedure can then be called — or run as an ON-unit handler — from a
// context the compiler cannot see.
void EffectWalker::markEscape(Symbol* s) {
  if (!s || s->kind != Symbol::ProcName || !s->proc)
    return;
  auto it = bySrc_->find(s->proc);
  if (it != bySrc_->end())
    escaped_.insert(it->second);
}

// Walk the arguments of a call: each one is read for the call.
void EffectWalker::walkArgs(const std::vector<HExprP>& args) {
  for (const auto& a : args)
    walkExpr(a.get(), false);
}

// A call to a built-in: the built-in's own effect decides.
void EffectWalker::walkCallExpr(HExpr* e, bool writeCtx) {
  Symbol* callee = e->sym;
  if (callee && callee->kind == Symbol::ProcName) {
    walkArgs(e->args);
    if (!callee->proc) {
      b_.opaque = true;
      return;
    }
    const HProc* target = bySrc_->count(callee->proc) ? bySrc_->at(callee->proc) : nullptr;
    if (!target) {
      b_.opaque = true; // a procedure with no HIR twin: nothing is known
      return;
    }
    calls_.emplace_back(target, callArgReach(target, e->args));
    return;
  }
  if (callee && (callee->isEntryVar || callee->isEntry)) {
    // An indirect call through an entry variable, or a C entry: unknown.
    walkArgs(e->args);
    b_.opaque = true;
    b_.mayUnwind = true;
    return;
  }
  walkArgs(e->args);
  // A built-in may lower to a runtime call, and the runtime's unwinding is
  // not part of the audited side table, so a built-in call is a call whose
  // unwinding the compiler does not know.
  b_.mayUnwind = true;
  switch (builtinEffect(e->name)) {
  case BuiltinEffect::Constant:
    break;
  case BuiltinEffect::Pure: {
    // The built-in reads its arguments' storage and writes nothing.
    Reach r = Reach::Frame;
    for (const auto& a : e->args)
      r = wider(r, reachOfExpr(a.get()));
    note(r, false);
    if (e->name == "MOD" || e->name == "DIVIDE") {
      if (opts_->zerodivideChecks)
        b_.raiseKeys.insert(Stmt::kZerodivideCondKey);
    }
    if (e->name == "ADD" || e->name == "SUBTRACT" || e->name == "MULTIPLY")
      raiseSize();
    break;
  }
  case BuiltinEffect::Allocates:
    b_.writesOther = true;
    break;
  case BuiltinEffect::Escapes: {
    // The address of the argument leaves the procedure: the argument's storage
    // becomes reachable from outside, so the frame is no longer private.
    Reach r = Reach::Frame;
    for (const auto& a : e->args)
      r = wider(r, reachOfExpr(a.get()));
    if (r == Reach::Frame)
      b_.opaque = true; // a local's address escaped: the frame is not private
    else
      note(r, false);
    break;
  }
  case BuiltinEffect::Unknown:
    b_.opaque = true;
    break;
  }
  // A built-in on the left of an assignment (SUBSTR(x,1,3) = 'a') writes the
  // storage of its first argument.
  if (writeCtx && !e->args.empty())
    note(reachOfExpr(e->args[0].get()), true);
}

// A CALL statement: to an internal procedure, an entry variable, or a C entry.
void EffectWalker::walkCallStmt(HStmt* st) {
  Symbol* callee = st->sym;
  walkArgs(st->args);
  if (callee && callee->kind == Symbol::ProcName && callee->proc) {
    const HProc* target = bySrc_->count(callee->proc) ? bySrc_->at(callee->proc) : nullptr;
    if (!target) {
      b_.opaque = true;
      return;
    }
    calls_.emplace_back(target, callArgReach(target, st->args));
    return;
  }
  // An entry variable, an external C entry, or a task call: the target's
  // effects are not known here.
  b_.opaque = true;
  b_.mayUnwind = true;
}

// Walk one expression. `writeCtx` marks the expression as the target of an
// assignment, which is the only way a PL/I expression writes memory.
void EffectWalker::walkExpr(HExpr* e, bool writeCtx) {
  if (!e)
    return;
  switch (e->kind) {
  case HExpr::IntLit:
  case HExpr::FltLit:
  case HExpr::DecLit:
  case HExpr::CharLit:
  case HExpr::BitLit:
  case HExpr::ComplexLit:
  case HExpr::TypeTag:
  case HExpr::Star:
    return;
  case HExpr::VarRef: {
    Symbol* s = e->sym;
    if (!s)
      return;
    if (s->kind == Symbol::ProcName) {
      // The procedure's own name: handing it out lets somebody else call it.
      markEscape(s);
      return;
    }
    if (e->locPtr) {
      // A locator-qualified reference reaches through the locator.
      walkExpr(e->locPtr.get(), false);
      note(Reach::Other, writeCtx);
      return;
    }
    note(reachOf(s), writeCtx);
    return;
  }
  case HExpr::Subscript:
    for (auto& a : e->args)
      walkExpr(a.get(), false);
    if (e->locPtr) {
      // A locator-qualified subscript reaches through the locator, which
      // points anywhere (as for a plain locator-qualified reference).
      walkExpr(e->locPtr.get(), false);
      note(Reach::Other, writeCtx);
    } else if (e->sym) {
      note(reachOf(e->sym), writeCtx);
    }
    if (opts_->subscriptChecks)
      b_.raiseKeys.insert(Stmt::kSubscriptrangeCondKey);
    return;
  case HExpr::Binary:
    walkExpr(e->a.get(), false);
    walkExpr(e->b.get(), false);
    // Fixed-point arithmetic can overflow (SIZE); a division can divide by
    // zero (ZERODIVIDE). A comparison or a logical operation computes no new
    // value, so it cannot overflow — the code generator emits no trap for it.
    if (e->a && e->a->ty.isFixed() && isArithOp(e->op))
      raiseSize();
    if (e->op == Tok::Slash && opts_->zerodivideChecks)
      b_.raiseKeys.insert(Stmt::kZerodivideCondKey);
    return;
  case HExpr::Unary:
    walkExpr(e->a.get(), false);
    // Negating the most negative value overflows.
    if (e->a && e->a->ty.isFixed() && e->op == Tok::Minus)
      raiseSize();
    return;
  case HExpr::Convert:
    walkExpr(e->a.get(), false);
    if (opts_->conversionChecks)
      b_.raiseKeys.insert(Stmt::kConversionCondKey);
    // A conversion may lower to a runtime helper (char to fixed does), whose
    // unwinding is not part of the audited side table.
    b_.mayUnwind = true;
    return;
  case HExpr::Call:
    walkCallExpr(e, writeCtx);
    return;
  }
  // An expression kind this walker does not model: claim nothing about it.
  b_.opaque = true;
  b_.mayUnwind = true;
  b_.mayNotReturn = true;
}

// Walk one statement.
void EffectWalker::walkStmt(HStmt* st) {
  if (!st)
    return;
  switch (st->kind) {
  case HStmt::Null:
  case HStmt::Leave:
  case HStmt::Iterate:
  case HStmt::Format:
  case HStmt::Entry:
    return;
  case HStmt::Stop:
    // The program ends here: the procedure never returns to its caller.
    b_.mayNotReturn = true;
    return;
  case HStmt::Goto:
    // A GO TO can jump backwards, so the procedure may not return.
    b_.mayNotReturn = true;
    return;
  case HStmt::Declare:
    for (auto& d : st->decls) {
      walkExpr(d.init.get(), false);
      walkExpr(d.initCall.get(), false);
      walkExpr(d.dynLenBound.get(), false);
    }
    return;
  case HStmt::Assign:
    // The targets are written, the value is read.
    walkExpr(st->target.get(), true);
    for (auto& t : st->extraTargets)
      walkExpr(t.get(), true);
    walkExpr(st->value.get(), false);
    if (st->target && st->target->ty.isChar())
      raiseSize(); // a truncation raises SIZE
    return;
  case HStmt::If:
    walkExpr(st->cond.get(), false);
    walkStmt(st->thenS.get());
    walkStmt(st->elseS.get());
    return;
  case HStmt::Group:
  case HStmt::Begin:
    for (auto& b : st->body)
      walkStmt(b.get());
    return;
  case HStmt::DoWhile:
    // A DO WHILE has no bound: it may never end.
    b_.mayNotReturn = true;
    walkExpr(st->cond.get(), false);
    for (auto& b : st->body)
      walkStmt(b.get());
    return;
  case HStmt::DoIter:
    // A DO loop ends unless its increment can be zero at run time: a zero
    // increment never reaches the limit. An omitted BY is one.
    if (st->by && !(st->by->kind == HExpr::IntLit && st->by->ival != 0))
      b_.mayNotReturn = true;
    walkExpr(st->from.get(), false);
    walkExpr(st->to.get(), false);
    walkExpr(st->by.get(), false);
    for (auto& b : st->body)
      walkStmt(b.get());
    return;
  case HStmt::Return:
    walkExpr(st->value.get(), false);
    return;
  case HStmt::On:
    // An ON unit may run at any point in the procedure and may write anything
    // the establishing block can see, so nothing may be claimed about it.
    b_.hasOnUnit = true;
    b_.opaque = true;
    if (handlerKeys_)
      handlerKeys_->insert(st->condKey);
    walkStmt(st->unit.get());
    return;
  case HStmt::CallS:
    walkCallStmt(st);
    return;
  case HStmt::Put:
  case HStmt::Get:
  case HStmt::Display:
  case HStmt::Read:
  case HStmt::Write:
  case HStmt::Open:
  case HStmt::Close:
  case HStmt::Allocate:
  case HStmt::Free:
  case HStmt::Revert:
  case HStmt::Signal:
  case HStmt::Wait:
  case HStmt::Delay:
    // I/O, storage and the condition machinery all reach outside the frame.
    walkExpr(st->target.get(), false);
    walkExpr(st->value.get(), false);
    walkExpr(st->cond.get(), false);
    walkExpr(st->from.get(), false);
    walkExpr(st->to.get(), false);
    walkExpr(st->by.get(), false);
    for (auto& i : st->items)
      walkExpr(i.get(), false);
    walkExpr(st->stringTarget.get(), true);
    walkExpr(st->oncodeExpr.get(), false);
    b_.io = true;
    b_.opaque = true;
    return;
  }
  // A statement kind this walker does not model: claim nothing about it.
  b_.opaque = true;
  b_.mayUnwind = true;
  b_.mayNotReturn = true;
}

// The body of one procedure.
Bits EffectWalker::run() {
  for (auto& st : p_->body)
    walkStmt(st.get());
  // A structure- or character-valued function returns through a hidden result
  // pointer (rules (34), (127)): the callee writes through that pointer, so it
  // writes its argument memory.
  if (p_->isFunction && (p_->retTy.isStruct() || p_->retTy.isChar()))
    b_.writesArg = true;
  return b_;
}

} // namespace

// The text of a REPEAT of a constant string a constant number of times
// (ADR-241). The decision lives here, outside irgen.cpp (the plan's C2), so
// it can be read without a backend.
std::string foldRepeatText(const std::string& src, long long count) {
  if (src.empty() || count <= 0)
    return {};
  // The result type is the source length times the count (sema sized it), so
  // the two agree; the guard only keeps the text bounded.
  if (count > (1LL << 20) || (long long)src.size() * count > (1LL << 20))
    return {};
  std::string text;
  text.reserve(src.size() * (size_t)count);
  for (long long k = 0; k < count; ++k)
    text += src;
  return text;
}

// The membership table of a constant alphabet (ADR-241). A byte is a
// character code in PL/I, so the table is indexed by the full 256-value range
// and the scan it replaces is exactly "is this byte in the alphabet".
std::string searchTableBytes(const std::string& alphabet) {
  std::string bytes(256, '\0');
  for (unsigned char c : alphabet)
    bytes[c] = 1;
  return bytes;
}

// The translate table of constant `out`/`in` strings (ADR-273). The inline
// scan walks `in` for the first occurrence of the character and answers with
// `out` at that index, a blank when the index is past the end of `out`, and
// the character itself when it does not occur — the table below is that
// function evaluated once per byte value.
std::string translateTableBytes(const std::string& out, const std::string& in) {
  std::string bytes(256, '\0');
  bool mapped[256] = {false};
  for (int c = 0; c < 256; ++c)
    bytes[(size_t)c] = static_cast<char>(c);
  for (size_t j = 0; j < in.size(); ++j) {
    unsigned char c = static_cast<unsigned char>(in[j]);
    // The first occurrence decides, which is where the scan stops.
    if (mapped[c])
      continue;
    mapped[c] = true;
    bytes[c] = j < out.size() ? out[j] : ' ';
  }
  return bytes;
}

namespace {

// The picture characters of rule (148). The classifier validates against this
// set before it looks at the layout, so a malformed string cannot reach the
// folder.
bool isPictureChar(char c) {
  static const std::string chars = "ABCDEGHIKMPSTVYZ$.,*/1236789XFR+-";
  return chars.find(c) != std::string::npos;
}

// The characters that make a picture numeric rather than character. A picture
// with none of them is a character picture (A/X), which P output cannot use.
bool isNumericPictureChar(char c) {
  static const std::string chars = "9Z123V.,$+-SY*EK";
  return chars.find(c) != std::string::npos;
}

// The subset the constant folder reproduces byte for byte: the digits, the
// assumed decimal point, the signs, and the A/X blank positions. Everything
// else (repetition, F(n), currency, exponent, sterling, CR/DB) stays on the
// runtime interpreter.
bool isFoldablePictureChar(char c) {
  static const std::string chars = "9ZV.+-SXA";
  return chars.find(c) != std::string::npos;
}

} // namespace

// Analyze every procedure in the program.
std::map<const HProc*, ProcEffects> analyzeEffects(const HProgram& prog,
                                                    const std::unordered_set<const Symbol*>& byValue,
                                                    const EffectOptions& opts) {
  // Map each AST procedure to its HIR twin, so a call site finds the callee.
  std::map<const Proc*, const HProc*> bySrc;
  for (const auto& p : prog.procs)
    if (p->src)
      bySrc[p->src] = p.get();

  // The condition keys the unit establishes a handler for. A handler may run
  // inside any procedure that raises that condition, and it may write memory
  // outside that procedure's frame.
  std::set<int> handlerKeys;

  // Walk every body once: the accesses it makes and the procedures it calls.
  std::map<const HProc*, Bits> bits;
  std::map<const HProc*, std::vector<std::pair<const HProc*, Reach>>> calls;
  std::set<const HProc*> escaped;
  for (const auto& p : prog.procs) {
    EffectWalker w(p.get(), &byValue, &opts, &bySrc, &handlerKeys);
    bits[p.get()] = w.run();
    calls[p.get()] = w.calls();
    escaped.insert(w.escaped().begin(), w.escaped().end());
  }

  // A procedure that can reach itself may never return: a recursion has no
  // proven bound. Mark the procedures on a cycle; the fixpoint below carries
  // the mark to their callers. A back edge always lands on a procedure that
  // is still being visited, so marking that one is enough to mark the cycle.
  {
    std::map<const HProc*, std::vector<const HProc*>> edges;
    for (const auto& p : prog.procs)
      for (const auto& [callee, reach] : calls[p.get()])
        edges[p.get()].push_back(callee);
    enum Colour { White, Grey, Black };
    std::map<const HProc*, Colour> colour;
    std::function<void(const HProc*)> visit = [&](const HProc* n) {
      colour[n] = Grey;
      for (const HProc* c : edges[n]) {
        Colour& cc = colour[c];
        if (cc == Grey)
          bits[c].mayNotReturn = true; // a back edge: c is on the cycle
        else if (cc == White)
          visit(c);
      }
      colour[n] = Black;
    };
    for (const auto& p : prog.procs)
      if (colour[p.get()] == White)
        visit(p.get());
  }

  // A procedure whose name escapes may be called from anywhere, so nothing may
  // be claimed about it — and its callers inherit that, so the mark goes in
  // before the fixpoint, not after it. The separate flag keeps the reported
  // reason honest: the procedure itself calls nothing.
  for (const HProc* e : escaped) {
    bits[e].opaque = true;
    bits[e].nameEscapes = true;
  }

  // The whole-program fixpoint: a procedure's effect includes what the
  // procedures it calls do, and what the handlers it can dispatch to do.
  // Every bit is monotone, so this terminates. The handler rule runs inside
  // each round for the same reason as the escape mark: a handler runs in the
  // raising procedure's frame, so its effects are that procedure's effects,
  // and the callers must see them too.
  for (size_t round = 0; round <= prog.procs.size() + 1; ++round) {
    bool changed = false;
    for (const auto& p : prog.procs) {
      const HProc* self = p.get();
      Bits next = bits[self];
      for (const auto& [callee, reach] : calls[self]) {
        auto it = bits.find(callee);
        if (it == bits.end())
          continue;
        const Bits& cb = it->second;
        if (cb.opaque)
          next.opaque = true;
        if (cb.readsOther)
          next.readsOther = true;
        if (cb.writesOther)
          next.writesOther = true;
        if (cb.readsArg)
          noteReach(next, reach, false);
        if (cb.writesArg)
          noteReach(next, reach, true);
        next.raiseKeys.insert(cb.raiseKeys.begin(), cb.raiseKeys.end());
        if (cb.mayUnwind)
          next.mayUnwind = true;
        if (cb.mayNotReturn)
          next.mayNotReturn = true;
      }
      if (!(next == bits[self])) {
        bits[self] = next;
        changed = true;
      }
    }
    // A procedure that can raise a condition the unit handles may dispatch to
    // that handler: whatever the handler does to memory, or to control flow,
    // the procedure does too. ON ERROR catches every condition, so it covers
    // them all.
    for (auto& [proc, b] : bits) {
      (void)proc;
      for (int k : b.raiseKeys)
        if (handlerKeys.count(k) || handlerKeys.count(0)) {
          if (!b.writesOther) {
            b.writesOther = true;
            changed = true;
          }
          if (!b.mayUnwind) {
            b.mayUnwind = true;
            changed = true;
          }
          break;
        }
    }
    if (!changed)
      break;
  }

  // The verdict: the strongest promise the facts support.
  std::map<const HProc*, ProcEffects> out;
  for (const auto& p : prog.procs) {
    const Bits& b = bits[p.get()];
    ProcEffects pe;
    if (b.opaque)
      pe.effect = MemEffect::Unknown;
    else if (b.writesOther)
      pe.effect = MemEffect::Unknown;
    else if (b.readsOther)
      pe.effect = MemEffect::Read;
    else if (b.writesArg)
      pe.effect = MemEffect::ArgReadWrite;
    else if (b.readsArg)
      pe.effect = MemEffect::ArgRead;
    else
      pe.effect = MemEffect::None;
    pe.note = effectNote(b);
    // `nounwind`: the procedure cannot unwind. Every call it makes must be one
    // whose unwinding is known. The trap path does not disqualify it: the
    // condition machinery is part of the audited runtime table, the abort
    // functions are `noreturn`, and a condition that is handled is treated as
    // an unwinding call above, because the handler runs inside this frame.
    pe.nounwind = !b.opaque && !b.mayUnwind;
    // `willreturn`: the procedure always returns. A trap path does not return
    // when no handler is established — it aborts — so a procedure that can
    // raise a condition is excluded. So is a procedure whose effects are
    // unknown: the unknown call may not return.
    pe.willReturn = !b.opaque && !b.mayNotReturn && b.raiseKeys.empty();
    out[p.get()] = pe;
  }
  return out;
}

// --- WS-A2: the published-effects file (ADR-267) ----------------------------
//
// The file is the only channel that carries a procedure's promise to a caller
// that cannot see its body. The format is deliberately dull: one line per
// procedure, the name, then the flags — a name, a memory-effect token, and
// the two control promises. A line the reader does not understand is an
// error, not a silent skip: a promise that is dropped must not look like one
// that was never made.

// The token for each rung of ADR-264's ladder.
static const char* effectToken(MemEffect e) {
  switch (e) {
  case MemEffect::None:
    return "none";
  case MemEffect::ArgRead:
    return "argmem-read";
  case MemEffect::ArgReadWrite:
    return "argmem-readwrite";
  case MemEffect::Read:
    return "read";
  case MemEffect::Unknown:
    break;
  }
  return "readwrite"; // may read and write any memory: no promise at all
}

const char* const kPublishedEffectsHeader = "# pli published effects v1";

std::string formatEffectLine(const std::string& name, const ProcEffects& e,
                              const std::string& digest) {
  std::string out = name;
  out += ' ';
  out += digest;
  out += ' ';
  out += effectToken(e.effect);
  if (e.nounwind)
    out += " nounwind";
  if (e.willReturn)
    out += " willreturn";
  return out;
}

bool parseEffectLine(const std::string& line, std::string& name, ProcEffects& e,
                     std::string& digest) {
  std::istringstream in(line);
  std::string tok;
  if (!(in >> tok) || tok.empty())
    return false;
  name = tok;
  if (!(in >> tok) || tok.empty())
    return false;
  digest = tok; // the signature the promise was published for
  e = ProcEffects();
  bool sawEffect = false;
  while (in >> tok) {
    if (tok == "none") {
      e.effect = MemEffect::None;
      sawEffect = true;
    } else if (tok == "argmem-read") {
      e.effect = MemEffect::ArgRead;
      sawEffect = true;
    } else if (tok == "argmem-readwrite") {
      e.effect = MemEffect::ArgReadWrite;
      sawEffect = true;
    } else if (tok == "read") {
      e.effect = MemEffect::Read;
      sawEffect = true;
    } else if (tok == "readwrite") {
      e.effect = MemEffect::Unknown;
      sawEffect = true;
    } else if (tok == "nounwind") {
      e.nounwind = true;
    } else if (tok == "willreturn") {
      e.willReturn = true;
    } else {
      return false; // an unknown flag: the line is not a promise we can keep
    }
  }
  return sawEffect;
}

PictureSpec classifyPicture(const std::string& pic) {
  PictureSpec s;
  if (pic.empty())
    return s;

  for (size_t i = 0; i < pic.size();) {
    long long repeat = 1;
    bool repeated = false;
    // rule (147): "(integer) picture-character" repeats the character; n = 0
    // ignores it.
    if (pic[i] == '(') {
      size_t j = i + 1;
      long long n = 0;
      bool digits = false;
      while (j < pic.size() && std::isdigit(static_cast<unsigned char>(pic[j]))) {
        digits = true;
        n = n * 10 + (pic[j] - '0');
        if (n > 1000000)
          return s; // a picture this large is not a picture we want to expand
        ++j;
      }
      if (!digits || j >= pic.size() || pic[j] != ')')
        return s;
      repeat = n;
      repeated = true;
      i = j + 1;
      if (i >= pic.size())
        return s;
    }

    char c = pic[i];
    // rule (146): the trailing F(+n)/F(-n) scaling factor moves the assumed
    // decimal point; it emits no character.
    if (c == 'F') {
      if (repeated || i + 1 >= pic.size() || pic[i + 1] != '(')
        return s;
      size_t j = i + 2;
      int sign = 1;
      if (j < pic.size() && (pic[j] == '+' || pic[j] == '-')) {
        if (pic[j] == '-')
          sign = -1;
        ++j;
      }
      long long n = 0;
      bool digits = false;
      while (j < pic.size() && std::isdigit(static_cast<unsigned char>(pic[j]))) {
        digits = true;
        n = n * 10 + (pic[j] - '0');
        if (n > 1000000)
          return s;
        ++j;
      }
      if (!digits || j >= pic.size() || pic[j] != ')' || j + 1 != pic.size())
        return s;
      s.hasScale = true;
      s.scale = sign * static_cast<int>(n);
      i = j + 1;
      continue;
    }

    if (!isPictureChar(c))
      return s;
    if (repeated)
      s.hasRepeat = true;
    // Expand the repetition now: every later step (width, digit positions,
    // folding) sees one character per output position.
    for (long long k = 0; k < repeat; ++k) {
      s.expanded.push_back(c);
      if (c != 'V')
        ++s.width;
    }
    ++i;
  }

  if (s.expanded.empty())
    return s;

  // The first V or '.' is the assumed decimal point; digits after it are
  // fractional positions (the rule the runtime formatter uses).
  for (size_t i = 0; i < s.expanded.size(); ++i)
    if (s.expanded[i] == 'V' || s.expanded[i] == '.') {
      s.point = static_cast<int>(i);
      break;
    }
  for (size_t i = 0; i < s.expanded.size(); ++i) {
    char c = s.expanded[i];
    if (c != '9' && c != 'Z')
      continue;
    if (s.point >= 0 && static_cast<int>(i) > s.point)
      ++s.fracDigits;
    else
      ++s.intDigits;
  }

  for (char c : s.expanded)
    if (isNumericPictureChar(c)) {
      s.numeric = true;
      break;
    }

  // The runtime walks at most 127 picture characters (its output buffer), so
  // an expanded picture past that cap cannot be reproduced exactly.
  bool foldable = s.numeric && !s.hasRepeat && !s.hasScale && s.width > 0 &&
                  s.expanded.size() <= 127 && s.intDigits + s.fracDigits > 0;
  for (char c : s.expanded)
    if (!isFoldablePictureChar(c))
      foldable = false;
  s.foldable = foldable;
  s.valid = true;
  return s;
}

bool foldPictureConversion(long long value, int scale, const PictureSpec& spec, std::string& out) {
  if (!spec.valid || !spec.foldable || scale < 0)
    return false;
  // The runtime negates in signed arithmetic, so LLONG_MIN has no magnitude
  // there either; keep that value on the runtime path rather than disagree.
  if (value == std::numeric_limits<long long>::min())
    return false;

  const bool isneg = value < 0;
  unsigned long long av = isneg ? static_cast<unsigned long long>(-(value + 1)) + 1
                                : static_cast<unsigned long long>(value);
  long long digits[64];
  long long ndig = 0;
  if (av == 0) {
    digits[0] = 0;
    ndig = 1;
  } else {
    while (av > 0 && ndig < 64) {
      digits[ndig++] = static_cast<long long>(av % 10);
      av /= 10;
    }
    for (long long i = 0; i < ndig / 2; ++i) {
      long long t = digits[i];
      digits[i] = digits[ndig - 1 - i];
      digits[ndig - 1 - i] = t;
    }
  }

  const long long val_int = ndig > scale ? ndig - scale : 0;
  const long long val_frac = scale < ndig ? scale : 0;
  std::string text;
  text.reserve(static_cast<size_t>(spec.width));
  int led_zero = 1;
  long long frac_idx = 0;

  for (size_t pi = 0; pi < spec.expanded.size(); ++pi) {
    char c = spec.expanded[pi];
    const bool is_fractional = spec.point >= 0 && static_cast<long long>(pi) > spec.point;

    if (c == 'V')
      continue; // no output character

    if (c == '9' || c == 'Z') {
      if (is_fractional) {
        if (frac_idx < val_frac) {
          text.push_back(static_cast<char>('0' + digits[val_int + frac_idx]));
          led_zero = 0;
        } else {
          text.push_back('0'); // zero-pad if the value has fewer fractional digits
        }
        ++frac_idx;
      } else {
        long long int_idx = 0;
        for (size_t j = 0; j < pi; ++j)
          if (spec.expanded[j] == 'Z' || spec.expanded[j] == '9')
            ++int_idx;
        long long src_idx = int_idx - (spec.intDigits - val_int);
        if (src_idx < 0) {
          text.push_back(c == '9' ? '0' : ' '); // Z -> blank, 9 -> zero
        } else if (src_idx < val_int) {
          char d = static_cast<char>('0' + digits[src_idx]);
          if (c == 'Z' && d == '0' && led_zero) {
            text.push_back(' '); // leading zero suppression
          } else {
            led_zero = 0;
            text.push_back(d);
          }
        } else {
          text.push_back(c == '9' ? '0' : ' ');
        }
      }
      continue;
    }
    if (c == '.') {
      text.push_back('.');
      continue;
    }
    if (c == '+') {
      text.push_back(isneg ? '-' : '+');
      continue;
    }
    if (c == '-' || c == 'S') {
      text.push_back(isneg ? '-' : ' ');
      continue;
    }
    if (c == 'X' || c == 'A') {
      text.push_back(' ');
      continue;
    }
    text.push_back(c); // unreachable for a foldable picture
  }

  // The width is the contract the runtime writes; a mismatch means the
  // classifier and the formatter disagree, so keep the runtime path.
  if (static_cast<int>(text.size()) != spec.width)
    return false;
  out = std::move(text);
  return true;
}

// WS-C1 (ADR-242): the constant-offset store plan. The result of the
// concatenation is the leftmost `cap` characters of the leaves laid end to
// end, blank-padded to `cap` when they are shorter. Each leaf therefore
// contributes min(its length, the room left in the target).
ConcatStorePlan planConcatStore(const std::vector<long long>& leafLen, long long cap) {
  ConcatStorePlan plan;
  if (cap <= 0)
    return plan;
  for (long long l : leafLen)
    if (l < 0)
      return plan;
  plan.sizes.reserve(leafLen.size());
  for (long long l : leafLen) {
    long long room = cap - plan.used;
    long long take = l < room ? l : room;
    plan.sizes.push_back(take);
    plan.used += take;
  }
  plan.pad = cap - plan.used;
  plan.ok = true;
  return plan;
}

// WS-C3 (ADR-243): the merged masked test. Two tests on the same value merge
// when the masks are disjoint and each expectation lies inside its own mask:
// then the conjunction holds exactly when the masked value equals the merged
// expectation. Overlapping masks would let one test decide a bit the other
// also decides, and an expectation outside its mask can never hold, so both
// keep the two separate tests.
BitTestMerge mergeBitTests(const std::string& mask1, const std::string& want1,
                           const std::string& mask2, const std::string& want2) {
  BitTestMerge merge;
  if (mask1.size() != mask2.size() || mask1.size() != want1.size() || mask1.size() != want2.size())
    return merge;
  std::string mask(mask1.size(), '0');
  std::string want(mask1.size(), '0');
  for (size_t i = 0; i < mask1.size(); ++i) {
    const bool m1 = mask1[i] == '1';
    const bool m2 = mask2[i] == '1';
    const bool w1 = want1[i] == '1';
    const bool w2 = want2[i] == '1';
    if (m1 && m2)
      return merge; // the masks overlap: the tests are not independent
    if ((w1 && !m1) || (w2 && !m2))
      return merge; // an expectation outside its mask: the test never holds
    mask[i] = (m1 || m2) ? '1' : '0';
    want[i] = (w1 || w2) ? '1' : '0';
  }
  merge.ok = true;
  merge.mask = std::move(mask);
  merge.want = std::move(want);
  return merge;
}

} // namespace pli
