// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// irgen_decisions.cpp — the fact store and its report (ADR-260).
//
// The store is deliberately dumb: it records what the compiler decided and
// prints it. It makes no decision of its own, so the report always agrees
// with the code that produced it.
#include "irgen_decisions.h"

#include "hir.h"
#include "sema.h"

#include <algorithm>
#include <functional>
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
  bool mayUnwind = false;  // makes a call whose unwinding is not known
  bool mayNotReturn = false; // may not return: a stop, or an endless loop
  std::set<int> raiseKeys; // the conditions it can raise
  std::string note;        // why the effect is not stronger

  bool operator==(const Bits& o) const {
    return readsOther == o.readsOther && writesOther == o.writesOther &&
           readsArg == o.readsArg && writesArg == o.writesArg && opaque == o.opaque &&
           io == o.io && hasOnUnit == o.hasOnUnit && mayUnwind == o.mayUnwind &&
           mayNotReturn == o.mayNotReturn && raiseKeys == o.raiseKeys;
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

  // The whole-program fixpoint: a procedure's effect includes what the
  // procedures it calls do. Every bit is monotone, so this terminates.
  for (size_t round = 0; round <= prog.procs.size(); ++round) {
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
    if (!changed)
      break;
  }

  // A procedure that can raise a condition the unit handles may dispatch to
  // that handler, and the handler runs inside the procedure's frame: whatever
  // the handler does — write memory, or unwind — the procedure does too.
  // ON ERROR catches every condition, so it covers them all.
  for (auto& [proc, b] : bits) {
    (void)proc;
    for (int k : b.raiseKeys)
      if (handlerKeys.count(k) || handlerKeys.count(0)) {
        b.writesOther = true;
        b.mayUnwind = true;
        break;
      }
  }

  // A procedure whose name escapes may be called from anywhere, so nothing may
  // be claimed about it.
  for (const HProc* e : escaped)
    bits[e].opaque = true;

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

} // namespace pli
