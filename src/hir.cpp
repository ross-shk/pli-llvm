// hir.cpp — HIR lowering (ADR-005) and printing.
//
// `lower` mirrors a typed AST into HIR node-for-node, and marks every implicit
// scalar conversion with an explicit `Convert` node. The set of sites matches
// exactly where IRGen applies a conversion, so lowering changes no runtime
// behaviour — it only makes the conversion visible in the HIR (and via
// `plic --print-hir`). Character and aggregate semantics stay in codegen.
#include "hir.h"

#include <ostream>

#include "sema.h"

namespace {

// True when IRGen's `convert` would emit a real instruction: a bit <-> scalar
// change, float <-> fixed, or a FIXED width or scale change. Character and
// void values are handled elsewhere, so they never need a Convert node.
bool convRequired(const Type& src, const Type& dst) {
  if (src.isBit() != dst.isBit())
    return true;
  if ((src.k == TK::Float) != (dst.k == TK::Float))
    return true;
  if (src.isNumeric() && dst.isNumeric() && src.k != TK::Float && dst.k != TK::Float)
    return src.intBits() != dst.intBits() || src.scale != dst.scale;
  return false;
}

// Wrap `e` in an explicit Convert to `dst` when a scalar conversion is needed.
HExprP convIf(HExprP e, const Type& dst) {
  if (!e || !convRequired(e->ty, dst))
    return e;
  auto c = std::make_unique<HExpr>();
  c->kind = HExpr::Convert;
  c->loc = e->loc;
  c->ty = dst;
  c->convTo = dst;
  c->a = std::move(e);
  return c;
}

HExprP lowerCallExpr(const Expr* e);
HExprP lowerSubscriptExpr(const Expr* e);
HExprP lowerExpr(const Expr* e);

// Mirror an expression node without re-dispatching the Call case (Call is
// handled by lowerCallExpr, which builds on this).
HExprP lowerExprBase(const Expr* e) {
  if (!e)
    return nullptr;
  auto h = std::make_unique<HExpr>();
  h->kind = static_cast<HExpr::Kind>(e->kind);
  h->loc = e->loc;
  h->ty = e->ty;
  h->ival = e->ival;
  h->fval = e->fval;
  h->decScale = e->decScale;
  h->decPrec = e->decPrec;
  h->sval = e->sval;
  h->name = e->name;
  h->path = e->path;
  h->memberPath = e->memberPath;
  h->sym = e->sym;
  h->op = e->op;
  h->a = lowerExpr(e->a.get());
  h->b = lowerExpr(e->b.get());
  for (const auto& a : e->args)
    h->args.push_back(lowerExpr(a.get()));
  return h;
}

HExprP lowerExpr(const Expr* e) {
  if (!e)
    return nullptr;
  if (e->kind == Expr::Call)
    return lowerCallExpr(e);
  if (e->kind == Expr::Subscript)
    return lowerSubscriptExpr(e);
  return lowerExprBase(e);
}

// Mirror a subscripted array reference A(i) (rule 126). The base symbol and
// element type come from sema; lowerExprBase already lowers the index args.
HExprP lowerSubscriptExpr(const Expr* e) {
  auto h = lowerExprBase(e);
  h->kind = HExpr::Subscript;
  return h;
}

// Lower a function-call expression, wrapping each argument that needs an
// implicit conversion to its parameter type. Built-ins get their operand
// conversions to the result/common type, mirroring IRGen's `convert` calls.
HExprP lowerCallExpr(const Expr* e) {
  auto h = lowerExprBase(e);
  HExpr* he = h.get();
  const std::string& n = e->name;

  if (n == "MIN" || n == "MAX" || n == "MOD" || n == "DIVIDE") {
    if (he->args.size() >= 2) {
      const Type& common = e->ty;
      he->args[0] = convIf(std::move(he->args[0]), common);
      he->args[1] = convIf(std::move(he->args[1]), common);
    }
  } else if (n == "MULTIPLY") {
    // A product's scale is the sum of the operand scales (ADR-006); match each
    // operand's width without rescaling it to the product scale first.
    if (he->args.size() >= 2) {
      const Type& common = e->ty;
      Type a0 = common;
      a0.scale = he->args[0]->ty.isFixed() ? he->args[0]->ty.scale : 0;
      Type a1 = common;
      a1.scale = he->args[1]->ty.isFixed() ? he->args[1]->ty.scale : 0;
      he->args[0] = convIf(std::move(he->args[0]), a0);
      he->args[1] = convIf(std::move(he->args[1]), a1);
    }
  } else if (n == "PRECISION") {
    if (!he->args.empty())
      he->args[0] = convIf(std::move(he->args[0]), e->ty);
  } else if (n == "ROUND") {
    if (!he->args.empty())
      he->args[0] = convIf(std::move(he->args[0]), Type::flt(6));
  } else if (e->sym && e->sym->proc) {
    // A user-defined function call: coerce each argument to its parameter type.
    for (size_t i = 0; i < he->args.size() && i < e->sym->proc->paramSyms.size(); ++i)
      he->args[i] = convIf(std::move(he->args[i]), e->sym->proc->paramSyms[i]->ty);
  }
  return h;
}

HStmtP lowerStmt(const Stmt* s, const Proc* owner) {
  if (!s)
    return nullptr;
  auto h = std::make_unique<HStmt>();
  h->kind = static_cast<HStmt::Kind>(s->kind);
  h->loc = s->loc;
  h->labels = s->labels;
  h->name = s->name;
  h->sym = s->sym;
  h->params = s->params;
  h->entryIsFunction = s->entryIsFunction;
  h->entryRetTy = s->entryRetTy;
  h->entryParamSyms = s->entryParamSyms;
  h->skip = s->skip;
  h->page = s->page;

  for (const auto& d : s->decls) {
    HDeclItem hd;
    hd.name = d.name;
    hd.ty = d.ty;
    hd.loc = d.loc;
    hd.level = d.level;
    hd.init = lowerExpr(d.init.get());
    hd.sym = d.sym;
    hd.isEntry = d.isEntry;
    hd.entryParams = d.entryParams;
    hd.extName = d.extName;
    for (const auto& b : d.dynBounds)
      hd.dynBounds.push_back(b ? lowerExpr(b.get()) : nullptr);
    for (const auto& b : d.dynLbBounds)
      hd.dynLbBounds.push_back(b ? lowerExpr(b.get()) : nullptr);
    // The lowered dynamic upper bound rides on the symbol so irgen's allocaLocals
    // can size the runtime buffer (rules (12),(13)). Safe to keep a raw pointer:
    // the HExpr is owned by this HProgram, which outlives IRGen.
    if (hd.sym && !hd.dynBounds.empty())
      hd.sym->dynUb = hd.dynBounds[0].get();
    if (hd.sym && !hd.dynLbBounds.empty())
      hd.sym->dynLb = hd.dynLbBounds[0].get();
    // Dynamic array members (rule 13): lower each member's bound exprs onto the
    // symbol so irgen can size and address the member buffer at entry. The raw
    // ub/lb pointers reference hd.dynMemberBounds, which owns the exprs.
    if (hd.sym)
      for (const auto& dm : d.dynMembers) {
        Symbol::DynMemberH mh;
        mh.path = dm.path;
        if (dm.ub) {
          hd.dynMemberBounds.push_back(lowerExpr(dm.ub));
          mh.ub = hd.dynMemberBounds.back().get();
        }
        if (dm.lb) {
          hd.dynMemberBounds.push_back(lowerExpr(dm.lb));
          mh.lb = hd.dynMemberBounds.back().get();
        }
        hd.sym->dynMembers.push_back(std::move(mh));
      }
    // The lowered INITIAL(CALL f(...)) expression (rule 27) rides on the symbol
    // so irgen's emitInitials can evaluate it at block entry; hd.initCall owns
    // the HExpr (freed with the HStmt), which outlives IRGen.
    if (hd.sym && d.initCall) {
      hd.initCall = lowerExpr(d.initCall.get());
      hd.sym->initCallH = hd.initCall.get();
    }
    h->decls.push_back(std::move(hd));
  }

  h->target = lowerExpr(s->target.get());
  for (const auto& t : s->extraTargets)
    h->extraTargets.push_back(lowerExpr(t.get()));
  h->byName = s->byName;
  h->cond = lowerExpr(s->cond.get());
  h->from = lowerExpr(s->from.get());
  h->to = lowerExpr(s->to.get());
  h->by = lowerExpr(s->by.get());
  h->thenS = lowerStmt(s->thenS.get(), owner);
  h->elseS = lowerStmt(s->elseS.get(), owner);
  for (const auto& b : s->body)
    h->body.push_back(lowerStmt(b.get(), owner));

  h->skipCount = lowerExpr(s->skipCount.get());
  for (const auto& it : s->items)
    h->items.push_back(lowerExpr(it.get()));

  // CALL statement arguments: coerce to the callee's parameter types.
  for (const auto& a : s->args) {
    HExprP ha = lowerExpr(a.get());
    // Callee parameter list: via the symbol's proc/entry, mirroring emitCall.
    if (s->sym) {
      if (Stmt* en = s->sym->entry) {
        if (h->args.size() < en->entryParamSyms.size())
          ha = convIf(std::move(ha), en->entryParamSyms[h->args.size()]->ty);
      } else if (Proc* callee = s->sym->proc) {
        if (h->args.size() < callee->paramSyms.size())
          ha = convIf(std::move(ha), callee->paramSyms[h->args.size()]->ty);
      }
    }
    h->args.push_back(std::move(ha));
  }

  // Statement-level conversions that IRGen applies inline, made explicit.
  switch (h->kind) {
  case HStmt::Assign:
    if (s->target && s->target->kind == Expr::VarRef && s->target->sym)
      h->value = convIf(lowerExpr(s->value.get()), s->target->sym->ty);
    else if (s->target && s->target->kind == Expr::Subscript && s->target->sym)
      // Array element target: convert to the element type (rule 126).
      h->value = convIf(lowerExpr(s->value.get()), s->target->ty);
    else
      h->value = lowerExpr(s->value.get());
    break;
  case HStmt::Return:
    if (owner && owner->isFunction)
      h->value = convIf(lowerExpr(s->value.get()), owner->retTy);
    else
      h->value = lowerExpr(s->value.get());
    break;
  case HStmt::DoIter:
    if (s->sym) {
      const Type& ct = s->sym->ty;
      h->from = convIf(std::move(h->from), ct);
      h->to = convIf(std::move(h->to), ct);
      h->by = convIf(std::move(h->by), ct);
    }
    break;
  default:
    if (!h->value)
      h->value = lowerExpr(s->value.get());
    break;
  }
  return h;
}

} // namespace

HProgram lower(const Program& prog) {
  HProgram out;
  // First pass: create every HProc and set parents, so parent pointers are valid.
  std::vector<HProc*> nodes(prog.procs.size(), nullptr);
  for (size_t i = 0; i < prog.procs.size(); ++i) {
    auto hp = std::make_unique<HProc>();
    hp->name = prog.procs[i]->name;
    hp->loc = prog.procs[i]->loc;
    hp->isMain = prog.procs[i]->isMain;
    hp->isFunction = prog.procs[i]->isFunction;
    hp->retTy = prog.procs[i]->retTy;
    hp->params = prog.procs[i]->params;
    hp->entryNames = prog.procs[i]->entryNames;
    hp->paramSyms = prog.procs[i]->paramSyms;
    hp->localSyms = prog.procs[i]->localSyms;
    hp->directUses = prog.procs[i]->directUses;
    hp->env = prog.procs[i]->env;
    hp->irName = prog.procs[i]->irName;
    hp->src = prog.procs[i].get();
    nodes[i] = hp.get();
    out.procs.push_back(std::move(hp));
  }
  for (size_t i = 0; i < prog.procs.size(); ++i)
    if (prog.procs[i]->parent)
      for (size_t j = 0; j < prog.procs.size(); ++j)
        if (prog.procs[j].get() == prog.procs[i]->parent) {
          nodes[i]->parent = nodes[j];
          break;
        }
  for (size_t i = 0; i < prog.procs.size(); ++i)
    for (auto& b : prog.procs[i]->body)
      nodes[i]->body.push_back(lowerStmt(b.get(), prog.procs[i].get()));
  for (size_t i = 0; i < prog.procs.size(); ++i)
    if (prog.mainProc && prog.procs[i].get() == prog.mainProc) {
      out.mainProc = nodes[i];
      break;
    }
  return out;
}

namespace {

void printType(std::ostream& os, const Type& t) { os << t.desc(); }

void printExpr(std::ostream& os, const HExpr* e, int ind) {
  if (!e) {
    os << "null";
    return;
  }
  (void)ind;
  switch (e->kind) {
  case HExpr::IntLit:
    os << "IntLit(" << e->ival << ":";
    printType(os, e->ty);
    os << ")";
    break;
  case HExpr::DecLit:
    os << "DecLit(" << e->ival << ",q" << e->decScale << ":";
    printType(os, e->ty);
    os << ")";
    break;
  case HExpr::FltLit:
    os << "FltLit(" << e->fval << ":";
    printType(os, e->ty);
    os << ")";
    break;
  case HExpr::CharLit:
    os << "CharLit(\"" << e->sval << "\":";
    printType(os, e->ty);
    os << ")";
    break;
  case HExpr::BitLit:
    os << "BitLit(\"" << e->sval << "\":";
    printType(os, e->ty);
    os << ")";
    break;
  case HExpr::VarRef:
    os << "VarRef(" << e->name << ":";
    printType(os, e->ty);
    os << ")";
    break;
  case HExpr::Convert:
    os << "Convert(";
    printExpr(os, e->a.get(), ind);
    os << " -> ";
    printType(os, e->convTo);
    os << ")";
    break;
  case HExpr::Unary: {
    const char* op = e->op == Tok::Not ? "not" : "neg";
    os << "Unary(" << op << " ";
    printExpr(os, e->a.get(), ind);
    os << ")";
    break;
  }
  case HExpr::Binary:
    os << "Binary(";
    printExpr(os, e->a.get(), ind);
    os << " ";
    switch (e->op) {
    case Tok::Plus:
      os << "+";
      break;
    case Tok::Minus:
      os << "-";
      break;
    case Tok::Star:
      os << "*";
      break;
    case Tok::Slash:
      os << "/";
      break;
    case Tok::Power:
      os << "**";
      break;
    case Tok::Concat:
      os << "||";
      break;
    case Tok::Amp:
      os << "&";
      break;
    case Tok::Bar:
      os << "|";
      break;
    case Tok::Eq:
      os << "=";
      break;
    case Tok::Ne:
      os << "¬=";
      break;
    case Tok::Lt:
      os << "<";
      break;
    case Tok::Le:
      os << "<=";
      break;
    case Tok::Gt:
      os << ">";
      break;
    case Tok::Ge:
      os << ">=";
      break;
    default:
      os << "?";
    }
    os << " ";
    printExpr(os, e->b.get(), ind);
    os << ":";
    printType(os, e->ty);
    os << ")";
    break;
  case HExpr::Subscript: {
    os << "Subscript(" << e->name;
    for (const auto& a : e->args) {
      os << " ";
      printExpr(os, a.get(), ind);
    }
    os << ":";
    printType(os, e->ty);
    os << ")";
    break;
  }
  case HExpr::Star:
    os << "*";
    break;
  case HExpr::Call: {
    os << "Call(" << e->name;
    if (e->sym && e->sym->proc)
      os << "->" << e->sym->proc->name;
    for (const auto& a : e->args) {
      os << " ";
      printExpr(os, a.get(), ind);
    }
    os << ")";
    break;
  }
  }
}

const char* stmtKind(HStmt::Kind k) {
  switch (k) {
  case HStmt::Null:
    return "Null";
  case HStmt::Declare:
    return "Declare";
  case HStmt::Assign:
    return "Assign";
  case HStmt::If:
    return "If";
  case HStmt::Group:
    return "Group";
  case HStmt::Begin:
    return "Begin";
  case HStmt::DoWhile:
    return "DoWhile";
  case HStmt::DoIter:
    return "DoIter";
  case HStmt::Put:
    return "Put";
  case HStmt::CallS:
    return "Call";
  case HStmt::Return:
    return "Return";
  case HStmt::Stop:
    return "Stop";
  case HStmt::Goto:
    return "Goto";
  case HStmt::Entry:
    return "Entry";
  case HStmt::Leave:
    return "Leave";
  }
  return "?";
}

void printStmt(std::ostream& os, const HStmt* s, int ind) {
  if (!s)
    return;
  const std::string pad(ind * 2, ' ');
  os << pad << stmtKind(s->kind);
  if (!s->labels.empty())
    os << " @" << s->labels.front();
  switch (s->kind) {
  case HStmt::Declare:
    for (const auto& d : s->decls)
      os << " " << d.name << ":" << d.ty.desc();
    break;
  case HStmt::Assign:
    os << " ";
    printExpr(os, s->target.get(), ind);
    for (const auto& t : s->extraTargets) {
      os << ",";
      printExpr(os, t.get(), ind);
    }
    os << " = ";
    printExpr(os, s->value.get(), ind);
    if (s->byName)
      os << " BY NAME";
    break;
  case HStmt::If:
    os << " cond=";
    printExpr(os, s->cond.get(), ind);
    break;
  case HStmt::DoWhile:
    os << " cond=";
    printExpr(os, s->cond.get(), ind);
    break;
  case HStmt::DoIter:
    os << " v=" << (s->sym ? s->sym->name : s->name) << " from=";
    printExpr(os, s->from.get(), ind);
    if (s->to) {
      os << " to=";
      printExpr(os, s->to.get(), ind);
    }
    if (s->by) {
      os << " by=";
      printExpr(os, s->by.get(), ind);
    }
    break;
  case HStmt::CallS:
    os << " " << (s->sym ? s->sym->name : s->name);
    for (const auto& a : s->args) {
      os << " ";
      printExpr(os, a.get(), ind);
    }
    break;
  case HStmt::Return:
    os << " ";
    printExpr(os, s->value.get(), ind);
    break;
  case HStmt::Goto:
    os << " " << s->name;
    break;
  case HStmt::Put: {
    os << " [";
    for (size_t i = 0; i < s->items.size(); ++i) {
      if (i)
        os << ", ";
      printExpr(os, s->items[i].get(), ind);
    }
    os << "]";
    break;
  }
  case HStmt::Entry:
    os << " " << s->name;
    break;
  default:
    break;
  }
  os << "\n";
  if (s->thenS)
    printStmt(os, s->thenS.get(), ind + 1);
  if (s->elseS) {
    os << pad << "else\n";
    printStmt(os, s->elseS.get(), ind + 1);
  }
  for (const auto& b : s->body)
    printStmt(os, b.get(), ind + 1);
}

} // namespace

void printHIR(const HProgram& p, std::ostream& os) {
  for (const auto& proc : p.procs) {
    os << (proc->isMain ? "main " : "") << "proc " << proc->name;
    if (proc->isFunction)
      os << " returns " << proc->retTy.desc();
    os << "\n";
    for (const auto& b : proc->body)
      printStmt(os, b.get(), 1);
    os << "end " << proc->name << "\n";
  }
}
