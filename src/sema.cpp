#include "sema.h"
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>

// True when a structure type contains a dynamic (runtime-extent) array member
// (rule 13); forward-declared here because resolveStructReturn (rule 127) and
// LIKE/assignment (rules 43,127) both use it.
static bool hasDynamicMember(const Type& ty);

// FIXED op FIXED -> FIXED with the wider precision and the larger scale;
// anything involving FLOAT is FLOAT. This is the *common* result type for
// +,-,comparison (and MIN/MAX/MOD), where a mixed-scale operand is rescaled
// up to the common scale (ADR-006). The product (`*`, MULTIPLY) uses
// mulResultType instead, whose scale is the sum of the operand scales.
Type arithResultType(const Type& a, const Type& b) {
  if (a.k == TK::Float || b.k == TK::Float)
    return Type::flt(std::max(a.k == TK::Float ? a.prec : 6, b.k == TK::Float ? b.prec : 6));
  int bits = std::max(a.intBits(), b.intBits());
  int p = bits == 64 ? 63 : 31;
  // A common DECIMAL type only when both operands are DECIMAL: FIXED BINARY
  // scale is 2-based (2^q) and must not be rescaled by powers of ten.
  bool dec = a.k == TK::FixedDec && b.k == TK::FixedDec;
  return dec ? Type::fixedDec(p, std::max(a.scale, b.scale))
             : Type::fixedBin(p, std::max(a.scale, b.scale));
}

// Product of two FIXED operands: the scale of the result is the sum of the
// operand scales, since the stored integers multiply directly (ADR-006).
Type mulResultType(const Type& a, const Type& b) {
  if (a.k == TK::Float || b.k == TK::Float)
    return arithResultType(a, b);
  Type t = arithResultType(a, b);
  t.scale = a.scale + b.scale;
  return t;
}

// Cross-section (rule 126): a subscript list containing at least one '*'. The
// result is an array whose rank and bounds are those of the '*' axes, in axis
// order; each non-'*' axis is collapsed by its fixed index. Returns true and
// fills `reduced` (the reduced-dim array type) and `nStar` when `e` is a
// cross-section, else returns false.
bool crossSectionType(const Expr* e, const Type& full, Type& reduced, int& nStar) {
  bool isCross = false;
  nStar = 0;
  Type r = full.elementType();
  r.dims.clear();
  for (size_t k = 0; k < e->args.size() && k < full.dims.size(); ++k) {
    if (e->args[k]->kind == Expr::Star) {
      isCross = true;
      ++nStar;
      r.dims.push_back(full.dims[k]);
    }
  }
  if (!isCross)
    return false;
  reduced = r;
  return true;
}

// Complex arithmetic (QR2.2/CM5): an operator serves complex operands when
// both sides are complex or numeric with at least one side complex; the
// result is always a complex value.
bool complexArith(const Type& a, const Type& b) {
  bool ac = a.isComplex() || a.isNumeric();
  bool bc = b.isComplex() || b.isNumeric();
  return ac && bc && (a.isComplex() || b.isComplex());
}

Scope* Sema::scopeFor(Proc* p) {
  auto it = procScopes_.find(p);
  if (it != procScopes_.end())
    return it->second;
  auto sc = std::make_unique<Scope>();
  sc->parent = p->parent ? scopeFor(p->parent) : rootScope_;
  Scope* raw = sc.get();
  scopes_.push_back(std::move(sc));
  procScopes_[p] = raw;
  return raw;
}

Symbol* Sema::lookup(Scope* sc, const std::string& n) {
  for (Scope* s = sc; s; s = s->parent) {
    auto it = s->tab.find(n);
    if (it != s->tab.end())
      return it->second;
  }
  return nullptr;
}

Symbol* Sema::declare(Scope* sc, const std::string& n, Type t, SourceLoc l, Symbol::Kind k,
                      bool isStatic) {
  auto it = sc->tab.find(n);
  if (it != sc->tab.end()) {
    d_.error(l, "'" + n + "' is already declared in this block", "(9)");
    d_.note(it->second->loc, "previous declaration was here");
    return it->second;
  }
  auto sym = std::make_unique<Symbol>();
  sym->name = n;
  sym->ty = t;
  sym->loc = l;
  sym->kind = k;
  sym->isStatic = isStatic;
  // The irName must be unique module-wide: a name declared in a nested
  // BEGIN block shadows an outer one of the same spelling (rule (68)), and the
  // two symbols still need distinct storage. Disambiguate with a numeric suffix.
  std::string base = (k == Symbol::Param) ? "%" + n + ".ptr"
                     : isStatic           ? "@pli_g_" + n
                                          : "%" + n + ".addr";
  std::string ir = base;
  for (int d = 1; !irNames_.insert(ir).second; ++d)
    ir = base + "$" + std::to_string(d);
  sym->irName = ir;
  Symbol* raw = sym.get();
  owned_.push_back(std::move(sym));
  sc->tab[n] = raw;
  sc->order.push_back(raw);
  if (k != Symbol::ProcName)
    storage_.push_back(raw);
  return raw;
}

// Implicit declaration (Y33-6003: an identifier beginning with I through N is
// FIXED BINARY REAL with default precision; any other initial letter gives
// FLOAT DECIMAL REAL). TR 25.084 defines only the syntax, not these defaults.
Symbol* Sema::implicitDeclare(Scope* sc, const std::string& n, SourceLoc l, bool isStatic) {
  char c = n.empty() ? 'X' : n[0];
  Type t = (c >= 'I' && c <= 'N') ? Type::fixedBin(15, 0) : Type::flt(6);
  Symbol* s = declare(sc, n, t, l, Symbol::Var, isStatic);
  s->implicit = true;
  d_.warn(l, "'" + n + "' is not declared; implicitly " + t.desc(), "(130)");
  return s;
}

// Extension (ADR-109): resolve PACKAGE EXPORTS against hoisted members,
// marking listed member procedures external. Runs before pass 1 assigns
// linkage, so exports keep their upper-cased external names.
void Sema::resolvePackageExports() {
  for (auto& p : prog_->procs) {
    if (!p->isPackage)
      continue;
    for (const auto& en : p->exports) {
      Proc* m = nullptr;
      for (auto& q : prog_->procs)
        if (q->parent == p.get() && q->name == en) {
          m = q.get();
          break;
        }
      if (!m) {
        d_.error(p->loc,
                 "EXPORTS name '" + en + "' is not a procedure of package '" + p->name +
                     "' (ADR-109)",
                 "");
        continue;
      }
      if (m->isMain) {
        d_.error(p->loc, "cannot EXPORT the MAIN procedure '" + en + "' (ADR-109)", "");
        continue;
      }
      m->isExternal = true;
    }
  }
}

// Extension (ADR-109): a package contributes scope and linkage only; DECLARE
// statements inside a PACKAGE body are collected into rootScope_ during pass
// 1b so they become module-scope globals accessible by member procedures.
void Sema::processPackage(Proc* p) {
  for (auto& b : p->body) {
    if (!b || b->kind == Stmt::Null || b->kind == Stmt::Declare)
      continue; // DECLAREs handled in pass 1b; procedure bodies already hoisted
    d_.error(b->loc, "only DECLARE and PROCEDURE may appear in a PACKAGE body (ADR-109)", "");
  }
}

bool Sema::run(Program& prog, bool compileOnly) {
  prog_ = &prog;

  // Pass 1: every procedure name is visible in its parent's scope, so that
  // CALL can be resolved regardless of textual order. External procedures
  // share a program-level scope, so siblings can call each other.
  rootScope_ = new Scope();
  scopes_.push_back(std::unique_ptr<Scope>(rootScope_));

  // Extension (ADR-109): resolve PACKAGE EXPORTS before pass 1 assigns
  // linkage, so listed members keep their upper-cased external names.
  resolvePackageExports();
  // Extension (ADR-114): file-scope DEFINE ALIAS registers in the root
  // scope before any procedure resolves TYPE references against it.
  for (auto& d : prog.defines)
    if (d && d->kind == Stmt::DefineAlias)
      defineAlias(rootScope_, d->name, d->aliasTy, d->loc);

  for (auto& p : prog.procs) {
    if (p->isPackage)
      continue; // packages declare no symbol and need no irName
    // Rule (42): a top-level non-MAIN procedure is externally linked under
    // its upper-cased name (already upper-cased by the lexer), so another
    // translation unit's ENTRY reference resolves at link time. The MAIN
    // procedure keeps its module-private name: only its own `main` shim
    // invokes it. Nested procedures stay module-private unless exported.
    p->isExternal = p->isExternal || (!p->parent && !p->isMain);
    p->irName = p->isExternal
                    ? "@" + p->name
                    : "@PLI_" + (p->parent ? p->parent->name + "$" : std::string()) + p->name;
    // A function procedure's symbol carries its result type so that a
    // function reference (rule (123)) types as the returned value.
    Scope* outer = p->parent ? scopeFor(p->parent) : rootScope_;
    Type symTy = p->isFunction ? p->retTy : Type::voidTy();
    Symbol* s = declare(outer, p->name, symTy, p->loc, Symbol::ProcName, false);
    s->proc = p.get();
    // rule (3) entry-namelist: every extra name is another entry point to
    // the same procedure body, so each resolves to this Proc.
    for (const auto& en : p->entryNames) {
      Symbol* es = declare(outer, en, symTy, p->loc, Symbol::ProcName, false);
      es->proc = p.get();
    }
    // rule (56) ENTRY statements: each labelled entry is callable by name.
    for (auto& st : p->body) {
      if (st && st->kind == Stmt::Entry) {
        Type et = st->entryIsFunction ? st->entryRetTy : Type::voidTy();
        Symbol* es = declare(outer, st->name, et, st->loc, Symbol::ProcName, false);
        es->proc = p.get();
        es->entry = st.get();
      }
    }
  }

  // Extension (ADR-109): collect package-level data BEFORE member proc processing
  // so LIKE references to module-level structures (e.g. CONN_REC) resolve from
  // member procedures. Package-level symbols are registered in rootScope_ as
  // static storage (module-scope globals).
  for (auto& p : prog.procs) {
    if (!p->isPackage) continue;
    beginScopes_.clear();
    collectDecls(p->body, rootScope_, p.get(), true);
    resolvePendingBased(p.get());
  }

  // Pass 1b: collect every procedure's declarations into its own scope before
  // typing any body, so that a structure-valued function's RETURNS name (rule
  // 127) resolves against an already-declared enclosing template. Runs after
  // pass 1 (procedure names are declared), so INITIAL CALL (rule 27) can resolve
  // its function. processProc (pass 2) skips re-collecting (declsCollected_).
  // Parameters resolve here too, so a caller's argument checks (rules (34),
  // (78)) see every callee's descriptors regardless of procedure order
  // (ADR-094); processProc skips re-resolving them.
  for (auto& p : prog.procs) {
    if (p->isPackage) continue;
    beginScopes_.clear();
    collectDecls(p->body, scopeFor(p.get()), p.get(), false);
    // BASED bases resolve before params so a bare parameter base becomes a
    // POINTER parameter (rule 25), not an implicit arithmetic parameter.
    resolvePendingBased(p.get());
    resolveProcParams(p.get());
  }
  flushPendingBased();
  declsCollected_ = true;
  for (auto& p : prog.procs)
    resolveStructReturn(p.get());

  // Pass 2: declarations, resolution and typing, procedure by procedure.
  for (auto& p : prog.procs) {
    if (p->isPackage) {
      processPackage(p.get());
      continue;
    }
    processProc(p.get());
  }

  // Rule (5): every procedure in a static call cycle must carry RECURSIVE.
  // Runs here so CALL and function-reference callees are already resolved.
  checkRecursion();

  // Pass 3: bottom-up static-link environments (rule (8)); a proc's env is the
  // set of enclosing variables its whole subtree accesses.
  for (auto& p : prog.procs)
    if (!p->parent)
      computeEnv(p.get());

  if (!prog.mainProc) {
    // A package block is scope and linkage only, never an entry point.
    Proc* entry = nullptr;
    for (auto& p : prog.procs)
      if (!p->isPackage) {
        entry = p.get();
        break;
      }
    if (entry) {
      // A relocatable object (`-c`) may be a library with no entry point; the
      // caller links it against its own `main` (e.g. a C driver).
      if (!compileOnly) {
        d_.warn(entry->loc,
                "no procedure has OPTIONS(MAIN); using '" + entry->name +
                    "' as the program entry point",
                "(5)");
        prog.mainProc = entry;
        prog.mainProc->isMain = true;
      }
    } else if (prog.procs.empty()) {
      d_.error({}, "translation unit contains no procedure", "(1)");
    } else if (!compileOnly) {
      // Packages only: a library needs `-c`; an executable has no entry point.
      d_.error({}, "translation unit holds only PACKAGE blocks and no entry point (ADR-109)",
               "(1)");
    }
  }
  return d_.ok();
}

// True when any statement in `body` contains kind `k` (defined below).
static bool stmtsHaveKind(const std::vector<StmtP>& body, Stmt::Kind k);
// True when a declaration subtree carries a per-member INITIAL (rule (26)):
// any descendant with its own init/initItems/initCall/valueInit.
static bool structHasMemberInit(const std::vector<DeclItem*>& items,
                                const std::vector<std::vector<int>>& children, int idx);

// Extension (ADR-114): find a DEFINE ALIAS type by lexical scope chain.
static const Type* lookupAlias(Scope* sc, const std::string& name) {
  for (Scope* s = sc; s; s = s->parent) {
    auto it = s->aliases.find(name);
    if (it != s->aliases.end())
      return &it->second;
  }
  return nullptr;
}

void Sema::processProc(Proc* p) {
  Scope* sc = scopeFor(p);

  // STORAGE (M1): every procedure's variables are AUTOMATIC (stack), so each
  // activation gets its own copy — this makes external procedures reentrant.
  // Internal procedures reach enclosing automatic storage through a static
  // link (ADR-027); the M0 ADR-010 deviation (external vars as globals) is
  // removed. Explicit STATIC is accepted and treated as AUTOMATIC for now.
  bool isStatic = false;

  if (!declsCollected_) {
    beginScopes_.clear();
    collectDecls(p->body, sc, p, isStatic);
    resolvePendingBased(p);
  }

  // Parameters: a DECLARE inside the procedure supplies their attributes;
  // otherwise the implicit rule applies. Parameters are always by reference.
  // Already resolved in pass 1b when coming through run() (ADR-094).
  resolveProcParams(p);
  if (!declsCollected_)
    flushPendingBased();

  // Every function-valued entry point (the procedure's own RETURNS and each
  // ENTRY's RETURNS, rule (34)) must share one result type, which the shared
  // implementation returns. A non-function primary entry may coexist with
  // function ENTRYs (their segments carry a RETURN(value)).
  Type common = p->isFunction ? p->retTy : Type::voidTy();
  for (auto& st : p->body)
    if (st && st->kind == Stmt::Entry && st->entryIsFunction) {
      if (common.isVoid())
        common = st->entryRetTy;
      else if (st->entryRetTy != common)
        d_.error(st->loc,
                 "entries in one implementation must all return the same type; a mixed "
                 "return type is not implemented in this stage",
                 "(56)");
    }
  p->commonRetTy = common;

  procLabels_.clear();
  curEntry_ = nullptr;
  for (auto& s : p->body)
    collectLabels(s.get());
  for (auto& s : p->body) {
    if (s && s->kind == Stmt::Entry)
      curEntry_ = s.get(); // later statements belong to this segment
    checkStmt(s.get(), sc, p);
  }
  curEntry_ = nullptr;
  // Reductions over array expressions only expand in direct assignment;
  // anything left pending was never expanded and stays diagnosed (123).
  drainPendingReduces(p);
  // rule (91): unwinding handlers on a non-local exit is not implemented, so
  // GO TO in a procedure that establishes an ON-unit is diagnosed.
  if (stmtsHaveKind(p->body, Stmt::On) && stmtsHaveKind(p->body, Stmt::Goto))
    d_.error(p->loc, "GO TO in a procedure that establishes ON is not implemented in this stage",
             "(91)");
}

// Resolve a structure-valued function's RETURNS name (rule 127). The parser
// recorded the bare name of an enclosing structure variable; resolve it to a
// deep copy of that structure's type (mirroring a LIKE template, rule 43). A
// missing or non-structure reference, or a structure with a dynamic member, is
// diagnosed.
void Sema::resolveStructReturn(Proc* p) {
  if (p->returnsStructName.empty())
    return;
  Symbol* tpl = lookup(scopeFor(p), p->returnsStructName);
  if (!tpl || tpl->kind != Symbol::Var || !tpl->ty.isStruct()) {
    d_.error(p->loc,
             "RETURNS reference '" + p->returnsStructName + "' is not a structure in this scope",
             "(127)");
    p->retTy = Type::voidTy();
    return;
  }
  if (hasDynamicMember(tpl->ty))
    d_.error(p->loc, "RETURNS of a structure with a dynamic member is not implemented", "(13)");
  p->retTy = tpl->ty; // deep copy via Type's copy constructor
}

// Parameters are always by reference; a DECLARE supplies their attributes,
// otherwise the implicit rule applies. A name already resolved as a parameter
// (e.g. an ENTRY parameter reusing a procedure parameter) is reused, so the
// two spellings refer to one variable.
// Resolve one procedure's own parameters and its ENTRY statements' parameters
// (rules (34),(56)); resolving twice is a no-op so pass 1b can run ahead of
// pass 2 (ADR-094).
void Sema::resolveProcParams(Proc* p) {
  Scope* sc = scopeFor(p);
  if (p->paramSyms.empty())
    resolveParams(sc, p, p->params, p->paramSyms);
  // rule (56) ENTRY statements: each entry point declares its own parameters,
  // by reference, in the same scope (so a name shared with the procedure's own
  // parameter list refers to the same variable).
  for (auto& st : p->body)
    if (st && st->kind == Stmt::Entry && st->entryParamSyms.empty())
      resolveParams(sc, p, st->params, st->entryParamSyms);
}

void Sema::resolveParams(Scope* sc, Proc* p, const std::vector<std::string>& names,
                         std::vector<Symbol*>& out) {
  if (!out.empty())
    return; // already resolved (pass 1b ran ahead of pass 2)
  for (const std::string& pname : names) {
    Symbol* s = nullptr;
    auto it = sc->tab.find(pname);
    if (it != sc->tab.end()) {
      s = it->second;
      s->kind = Symbol::Param;
      s->isStatic = false;
      s->irName = "%" + pname + ".ptr";
      storage_.erase(std::remove(storage_.begin(), storage_.end(), s), storage_.end());
    } else {
      char c = pname.empty() ? 'X' : pname[0];
      Type t = (c >= 'I' && c <= 'N') ? Type::fixedBin(15, 0) : Type::flt(6);
      s = declare(sc, pname, t, p->loc, Symbol::Param, false);
      storage_.erase(std::remove(storage_.begin(), storage_.end(), s), storage_.end());
      d_.warn(p->loc, "parameter '" + pname + "' has no DECLARE; implicitly " + t.desc(), "(4)");
    }
    s->owner = p;
    out.push_back(s);
  }
}

// A name is a procedure-pointer-parameter base when it names the procedure's
// own parameter or one of its ENTRY statements' parameters (rules (34),(56)).
static bool isProcParamName(Proc* p, const std::string& n) {
  if (std::find(p->params.begin(), p->params.end(), n) != p->params.end())
    return true;
  for (auto& st : p->body)
    if (st && st->kind == Stmt::Entry &&
        std::find(st->params.begin(), st->params.end(), n) != st->params.end())
      return true;
  return false;
}

bool Sema::tryResolveBased(PendingBased& pb) {
  DeclItem* item = pb.item;
  if (!item || item->basedBase.empty() || item->sym->basedBase)
    return true; // nothing to do (already resolved)
  Symbol* base = lookup(pb.sc, item->basedBase);
  if (base && (base->kind == Symbol::Var || base->kind == Symbol::Param) &&
      base->ty.isPointer()) {
    item->sym->basedBase = base;
    return true;
  }
  if (!base && isProcParamName(pb.proc, item->basedBase)) {
    // BASED(P) directly on a procedure POINTER parameter (rule 25): the
    // parameter needs no separate DECLARE; it becomes a POINTER parameter.
    // Declared in the procedure scope (not the use-site BEGIN scope) so a
    // later explicit DECLARE and param resolution share one symbol.
    Scope* psc = scopeFor(pb.proc);
    Symbol* s = declare(psc, item->basedBase, Type::ptr(), item->loc, Symbol::Var, false);
    s->owner = pb.proc;
    item->sym->basedBase = s;
    return true;
  }
  return false;
}

void Sema::resolvePendingBased(Proc* p) {
  for (auto& pb : pendingBased_) {
    if (pb.proc != p)
      continue;
    tryResolveBased(pb);
  }
}

void Sema::flushPendingBased() {
  for (auto& pb : pendingBased_) {
    if (pb.item->sym->basedBase)
      continue;
    if (tryResolveBased(pb))
      continue;
    // A second lookup catches a POINTER declared in an enclosing procedure
    // whose DECLAREs were collected after this procedure's own pass.
    Symbol* base = lookup(pb.sc, pb.item->basedBase);
    if (base && (base->kind == Symbol::Var || base->kind == Symbol::Param) &&
        base->ty.isPointer()) {
      pb.item->sym->basedBase = base;
      continue;
    }
    d_.error(pb.item->loc,
             "BASED base '" + pb.item->basedBase + "' is not a POINTER variable in this scope",
             "(25)");
    // Mark as resolved (null stays) so one diagnostic is emitted even if
    // flush runs again from processProc's fallback path.
    pb.item->basedBase.clear();
  }
}

bool Sema::isDescendantOf(Proc* p, Proc* anc) {
  for (Proc* c = p; c; c = c->parent)
    if (c == anc)
      return true;
  return false;
}

void Sema::addEnv(std::vector<Symbol*>& env, Symbol* s) {
  if (std::find(env.begin(), env.end(), s) == env.end())
    env.push_back(s);
}

// Rule (8): a BASED/DEFINED reference needs its base storage too.
void Sema::noteStaticUse(Proc* p, Symbol* s) {
  if (!s || !p)
    return;
  // Package-level variables live in module scope and are addressed as globals;
  // they do not require static-link parameters even when accessed by members.
  if ((s->kind == Symbol::Var || s->kind == Symbol::Param) && s->owner && s->owner != p &&
      isDescendantOf(p, s->owner) && !s->owner->isPackage)
    addEnv(p->directUses, s);
  if (s->basedBase)
    noteStaticUse(p, s->basedBase);
  if (s->definedBase)
    noteStaticUse(p, s->definedBase);
}

// Bottom-up over the procedure tree. `p->env` is the ordered list of variables
// owned by a strict ancestor of `p` that `p` or any of its internal procedures
// accesses; codegen makes each a static-link parameter. A variable owned by
// `p` itself is not in the list — the caller passes its address directly.
void Sema::computeEnv(Proc* p) {
  for (auto& q : prog_->procs)
    if (q->parent == p)
      computeEnv(q.get());

  std::vector<Symbol*> env;
  for (Symbol* s : p->directUses)
    addEnv(env, s);
  for (auto& q : prog_->procs) {
    if (q->parent != p)
      continue;
    for (Symbol* s : q->env)
      if (s->owner != p)
        addEnv(env, s); // owned higher up: thread it through
  }
  p->env = env;
}

// Rule (5): collect the static call edges out of one expression. A Call whose
// symbol resolves to a PL/I procedure (including an alias or ENTRY name that
// maps to the same Proc) is an edge; builtin and C ENTRY calls have no body.
void Sema::collectExprCallees(const Expr* e, std::vector<Proc*>& out) {
  if (!e)
    return;
  if (e->locPtr)
    collectExprCallees(e->locPtr.get(), out);
  switch (e->kind) {
  case Expr::Call:
    if (e->sym && e->sym->proc && std::find(out.begin(), out.end(), e->sym->proc) == out.end())
      out.push_back(e->sym->proc);
    for (auto& a : e->args)
      collectExprCallees(a.get(), out);
    break;
  case Expr::Binary:
    collectExprCallees(e->a.get(), out);
    collectExprCallees(e->b.get(), out);
    break;
  case Expr::Unary:
    collectExprCallees(e->a.get(), out);
    break;
  case Expr::Subscript:
    for (auto& a : e->args)
      collectExprCallees(a.get(), out);
    break;
  default:
    break;
  }
}

// Rule (5): collect the static call edges out of one INITIAL item.
void Sema::collectInitItemCallees(const InitItem& item, std::vector<Proc*>& out) {
  collectExprCallees(item.value.get(), out);
  for (auto& sub : item.items)
    collectInitItemCallees(sub, out);
}

// Rule (5): collect the static call edges out of one statement, including the
// ON-unit body (conservative: a unit that re-enters its procedure needs
// RECURSIVE too) and INITIAL CALL / bound expressions in declarations.
void Sema::collectStmtCallees(const Stmt* s, std::vector<Proc*>& out) {
  if (!s)
    return;
  collectExprCallees(s->target.get(), out);
  collectExprCallees(s->value.get(), out);
  collectExprCallees(s->cond.get(), out);
  collectExprCallees(s->from.get(), out);
  collectExprCallees(s->to.get(), out);
  collectExprCallees(s->by.get(), out);
  collectExprCallees(s->skipCount.get(), out);
  collectExprCallees(s->stringTarget.get(), out);
  for (auto& it : s->items)
    collectExprCallees(it.get(), out);
  for (auto& t : s->extraTargets)
    collectExprCallees(t.get(), out);
  for (auto& a : s->args)
    collectExprCallees(a.get(), out);
  // Task options, WAIT/DELAY operands (rules (79),(82),(83)): may contain
  // function references in PRIORITY/DELAY/count expressions.
  collectExprCallees(s->taskRef.get(), out);
  collectExprCallees(s->eventRef.get(), out);
  collectExprCallees(s->priorityExpr.get(), out);
  collectExprCallees(s->value.get(), out);
  for (auto& e : s->waitEvents)
    collectExprCallees(e.get(), out);
  collectExprCallees(s->waitCount.get(), out);
  for (auto& f : s->formats) {
    collectExprCallees(f.w.get(), out);
    collectExprCallees(f.d.get(), out);
  }
  for (auto& b : s->allocBase)
    collectExprCallees(b.get(), out);
  for (auto& b : s->allocSet)
    collectExprCallees(b.get(), out);
  for (auto& b : s->freeBase)
    collectExprCallees(b.get(), out);
  if (s->kind == Stmt::CallS && s->sym && s->sym->proc &&
      std::find(out.begin(), out.end(), s->sym->proc) == out.end())
    out.push_back(s->sym->proc);
  if (s->kind == Stmt::Declare)
    for (auto& d : s->decls) {
      collectExprCallees(d.init.get(), out);
      collectExprCallees(d.initCall.get(), out);
      for (auto& b : d.dynBounds)
        collectExprCallees(b.get(), out);
      for (auto& b : d.dynLbBounds)
        collectExprCallees(b.get(), out);
      for (auto& item : d.initItems)
        collectInitItemCallees(item, out);
    }
  collectStmtCallees(s->thenS.get(), out);
  collectStmtCallees(s->elseS.get(), out);
  collectStmtCallees(s->unit.get(), out);
  for (auto& b : s->body)
    collectStmtCallees(b.get(), out);
}

// Rule (5): collect the static call edges out of one procedure body.
void Sema::collectCallees(const std::vector<StmtP>& body, std::vector<Proc*>& out) {
  for (auto& s : body)
    collectStmtCallees(s.get(), out);
}

// Rule (5): every procedure in a static call cycle must carry RECURSIVE.
// A self-edge through an entry-namelist alias or ENTRY name still maps to
// the same Proc, so it is direct recursion. One error per non-recursive
// cycle member, at the procedure definition where the fix goes. Calls
// across translation units cannot be seen and stay unchecked.
void Sema::checkRecursion() {
  std::unordered_map<Proc*, std::vector<Proc*>> adj;
  for (auto& p : prog_->procs) {
    std::vector<Proc*> out;
    collectCallees(p->body, out);
    adj[p.get()] = out;
  }
  for (auto& p : prog_->procs) {
    Proc* start = p.get();
    std::vector<Proc*> stack = adj[start];
    std::unordered_set<Proc*> seen;
    bool cycle = false;
    while (!stack.empty()) {
      Proc* q = stack.back();
      stack.pop_back();
      if (q == start) {
        cycle = true;
        break;
      }
      if (!seen.insert(q).second)
        continue;
      auto it = adj.find(q);
      if (it != adj.end())
        for (Proc* r : it->second)
          stack.push_back(r);
    }
    if (cycle && !start->isRecursive)
      d_.error(start->loc,
               "procedure '" + start->name +
                   "' is invoked recursively but lacks the RECURSIVE attribute; add "
                   "RECURSIVE to its PROCEDURE statement",
               "(5)");
  }
}

// True when a structure type contains a dynamic (runtime-extent) array member,
// at any nesting depth (rule (13)).
static bool hasDynamicMember(const Type& ty) {
  for (const auto& m : ty.members) {
    if (m->ty.isArray() && m->ty.isDynamic())
      return true;
    if (m->ty.isStruct() && hasDynamicMember(m->ty))
      return true;
  }
  return false;
}

void Sema::collectDecls(std::vector<StmtP>& body, Scope* sc, Proc* p, bool isStatic) {
  for (auto& s : body) {
    if (!s)
      continue;
    if (s->kind == Stmt::DefineAlias) {
      // DEFINE ALIAS (extension, ADR-114): register the parser-built type
      // in this scope's alias namespace (separate from variables).
      defineAlias(sc, s->name, s->aliasTy, s->loc);
      continue;
    }
    if (s->kind == Stmt::Declare) {
      // Build the level-numbered structure hierarchy (rule 11): a member item
      // belongs to the nearest preceding item with a strictly smaller level.
      // Indices below address s->decls directly (items[i] aliases s->decls[i]).
      std::vector<DeclItem*> items;
      std::vector<int> parentOf;
      std::vector<std::vector<int>> children;
      for (auto& item : s->decls) {
        if (item.entryIsFunction && !item.isEntry) {
          // RETURNS is an entry-name-attribute (rule (34)): it types an ENTRY
          // declaration, never a storage item. Never silently accepted.
          d_.error(item.loc, "RETURNS is only valid on an ENTRY declaration", "(34)");
          item.entryIsFunction = false;
        }
        if (item.entryByValue && !item.isEntry) {
          // OPTIONS(BYVALUE/LINKAGE) selects C-ABI marshalling on an ENTRY
          // declaration only; elsewhere it has no meaning. Never silently
          // accepted.
          d_.error(item.loc, "OPTIONS(BYVALUE/LINKAGE) is only valid on an ENTRY declaration",
                   "(34)");
          item.entryByValue = false;
        }
        if (item.isCondition) {
          // DECLARE name CONDITION; (rule 9): register the programmer-named
          // condition so subsequent SIGNAL/ON CONDITION(name) resolve it. Use
          // a ProcName kind symbol so lookup rejects it as a variable later,
          // mirroring how built-in conditions are checked at signal sites.
          prog_->condNames.push_back(item.name);
          Symbol* sym = declare(sc, item.name, Type::voidTy(), item.loc, Symbol::ProcName, false);
          sym->isCond = true;
          item.sym = sym;
          continue;
        }
        if (item.isEntry) {
          if (item.valueInit)
            d_.error(item.loc,
                     "VALUE on an ENTRY declaration is not implemented in this stage (ADR-108)",
                     "");
          // External C entry: a ProcName symbol with no PL/I body. The C
          // symbol is the EXTERNAL('name') override when given, else the
          // upper-cased PL/I identifier (rules (34),(38); z/OS ILC naming).
          // Not storage. A RETURNS(...) entry-name-attribute (rule (34))
          // makes it a function entry whose result type types a call.
          Type symTy = item.entryIsFunction ? item.entryRetTy : Type::voidTy();
          Symbol* sym = declare(sc, item.name, symTy, item.loc, Symbol::ProcName, false);
          sym->isEntry = true;
          sym->entryParams = item.entryParams;
          sym->entryIsFunction = item.entryIsFunction;
          sym->entryRetTy = item.entryRetTy;
          sym->entryByValue = item.entryByValue;
          sym->irName = "@" + (item.extName.empty() ? item.name : item.extName);
          item.sym = sym;
          entries_.push_back(sym);
          continue;
        }
        // Group the remaining (storage) items into a tree by level.
        int idx = (int)items.size();
        int parent = -1;
        for (int k = (int)items.size(); k-- > 0;)
          if (items[k]->level > 0 && items[k]->level < item.level) {
            parent = k;
            break;
          }
        items.push_back(&item);
        parentOf.push_back(parent);
        children.push_back({});
        if (parent >= 0)
          children[parent].push_back(idx);
      }
      // Construct the struct Type for an item from its (recursively built)
      // children; a leaf keeps its parsed scalar/array type. A dynamic array
      // member (rule 13) has its runtime bound exprs type-checked and captured
      // (with its field path) so codegen can size and address it.
      std::function<Type(int, std::vector<unsigned>, std::vector<DeclItem::DynMemberInfo>&)>
          buildType = [&](int idx, std::vector<unsigned> path,
                          std::vector<DeclItem::DynMemberInfo>& dynMs) -> Type {
        const DeclItem& it = *items[idx];
        // LIKE template (rule 43): the item takes the structure shape of an
        // already-declared structure variable (a deep copy of its type). A LIKE
        // item combined with its own members (the extend form) or a dimension
        // (an array of the template) is diagnosed, never silently dropped.
        if (!it.like.empty()) {
          if (!it.ty.dims.empty())
            d_.error(it.loc, "LIKE with a dimension is not implemented in this stage", "(43)");
          if (!children[idx].empty())
            d_.error(
                it.loc,
                "LIKE combined with members is not implemented in this stage; use a plain LIKE",
                "(43)");
          Symbol* tpl = nullptr;
          const Type* tplTy = nullptr;
          // A qualified template LIKE S.A.B (rule 43) narrows to a minor
          // structure: the head names an already-declared structure variable
          // and each further segment names a member of it.
          size_t dot = it.like.find('.');
          if (dot == std::string::npos) {
            tpl = lookup(sc, it.like);
            if (!tpl || tpl->kind != Symbol::Var || !tpl->ty.isStruct()) {
              d_.error(it.loc, "LIKE reference '" + it.like + "' is not a structure in this scope",
                       "(43)");
              return Type::voidTy();
            }
            tplTy = &tpl->ty;
          } else {
            tpl = lookup(sc, it.like.substr(0, dot));
            if (!tpl || tpl->kind != Symbol::Var || !tpl->ty.isStruct()) {
              d_.error(it.loc, "LIKE reference '" + it.like + "' is not a structure in this scope",
                       "(43)");
              return Type::voidTy();
            }
            tplTy = &tpl->ty;
            size_t pos = dot + 1;
            while (pos <= it.like.size()) {
              size_t next = it.like.find('.', pos);
              std::string seg =
                  it.like.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
              const Member* found = nullptr;
              if (!tplTy->isStruct()) {
                d_.error(it.loc,
                         "LIKE reference '" + it.like.substr(0, pos - 1) + "' is not a structure" +
                             ", so it cannot be qualified by '" + seg + "'",
                         "(43)");
                return Type::voidTy();
              }
              for (const auto& m : tplTy->members)
                if (m->name == seg) {
                  found = m.get();
                  break;
                }
              if (!found) {
                d_.error(it.loc, "LIKE reference '" + it.like + "' has no member '" + seg + "'",
                         "(43)");
                return Type::voidTy();
              }
              tplTy = &found->ty;
              if (next == std::string::npos)
                break;
              pos = next + 1;
            }
            if (!tplTy->isStruct()) {
              d_.error(it.loc, "LIKE reference '" + it.like + "' is not a structure", "(43)");
              return Type::voidTy();
            }
          }
          if (hasDynamicMember(*tplTy))
            d_.error(it.loc, "LIKE of a structure with a dynamic member is not implemented",
                     "(13)");
          return *tplTy; // deep copy via Type's copy constructor
        }
        if (children[idx].empty())
          return it.ty;
        std::vector<Member> ms;
        unsigned fi = 0;
        for (int c : children[idx]) {
          std::vector<unsigned> cp = path;
          cp.push_back(fi);
          Type ct = buildType(c, cp, dynMs);
          if (ct.isArray() && ct.isDynamic()) {
            // A dynamic array member (rule 13): resolve its bound expressions so
            // codegen can evaluate them at entry to size the member buffer.
            for (auto& b : items[c]->dynBounds)
              if (b)
                typeExpr(b.get(), sc, p);
            for (auto& b : items[c]->dynLbBounds)
              if (b)
                typeExpr(b.get(), sc, p);
            DeclItem::DynMemberInfo dm;
            dm.path = cp;
            dm.ub = items[c]->dynBounds.empty() ? nullptr : items[c]->dynBounds[0].get();
            dm.lb = items[c]->dynLbBounds.empty() ? nullptr : items[c]->dynLbBounds[0].get();
            dynMs.push_back(std::move(dm));
          }
          ms.push_back({items[c]->name, ct});
          ++fi;
        }
        Type st = Type::structTy(std::move(ms));
        st.dims = it.ty.dims; // an array of structures: keep the level item's dimension
        return st;
      };
      // Declare each top-level item (no parent) as a variable; members are
      // reached by qualification and get no standalone symbol or storage.
      for (int idx = 0; idx < (int)items.size(); ++idx) {
        if (parentOf[idx] != -1)
          continue;
        DeclItem& item = *items[idx];
        std::vector<DeclItem::DynMemberInfo> dynMs;
        item.ty = buildType(idx, {}, dynMs);
        if (!item.typeRef.empty()) {
          // TYPE <alias> (extension, ADR-114): the element type comes from
          // the alias, dimensions (if any) from this declaration.
          const Type* aty = lookupAlias(sc, item.typeRef);
          if (!aty)
            d_.error(item.loc, "unknown TYPE alias '" + item.typeRef + "' (ADR-114)", "");
          else {
            Type t = *aty;
            t.dims = item.ty.dims;
            item.ty = t;
          }
        }
        item.dynMembers = std::move(dynMs);
        item.sym = declare(sc, item.name, item.ty, item.loc, Symbol::Var, isStatic);
        item.sym->owner = p; // which procedure's frame holds this variable
        if (item.fileAttr) {
          // A FILE variable carries no runtime storage of its own: its identity
          // is the compile-time slot that OPEN/CLOSE/FILE( f ) pass to libpli.
          item.sym->fileAttr = true;
          item.sym->fileSlot = nextFileSlot_++;
          storage_.pop_back();
        }
        // TASK/EVENT names (rules (15),(79),(82), QR2.8): scalar handles only
        // in this stage; arrays, INITIAL/VALUE, LIKE/DEFINED/BASED stay diagnosed.
        if (item.ty.isTask() || item.ty.isEvent()) {
          const char* what = item.ty.isTask() ? "TASK" : "EVENT";
          if (item.ty.isArray())
            d_.error(item.loc,
                     std::string("array ") + what + " variables are not implemented in this stage",
                     "(15)");
          if (item.init || !item.initItems.empty() || item.initCall || item.valueInit)
            d_.error(item.loc,
                     std::string("INITIAL/VALUE on a ") + what +
                         " variable is not implemented in this stage",
                     "(26)");
          if (!item.like.empty() || !item.definedBase.empty() || !item.basedBase.empty())
            d_.error(item.loc,
                     std::string("LIKE/DEFINED/BASED on a ") + what +
                         " variable is not implemented in this stage",
                     "(15)");
        }
        // DEFINED (rule 24): the item overlays the storage of an already-
        // declared variable of identical type in this scope, so it needs no
        // storage of its own — references resolve to the base's address
        // (ADR-018). Unsupported forms are diagnosed, never silently dropped.
        bool isDefined = false;
        if (!item.definedBase.empty()) {
          Symbol* base = lookup(sc, item.definedBase);
          if (!base || base->kind != Symbol::Var) {
            d_.error(item.loc,
                     "DEFINED base '" + item.definedBase + "' is not a variable in this scope",
                     "(24)");
          } else if (base->owner != p) {
            d_.error(item.loc,
                     "DEFINED across an enclosing procedure is not implemented in this stage",
                     "(24)");
          } else if (item.definedSubs.empty()) {
            // Whole-base overlay (rule 24): the item shares the base's storage.
            if (item.ty.isStruct() || base->ty.isStruct()) {
              d_.error(item.loc, "DEFINED on a structure is not implemented in this stage", "(24)");
            } else if (!(item.ty == base->ty)) {
              d_.error(item.loc,
                       "DEFINED requires the item and base to have the same type (" +
                           item.ty.desc() + " vs " + base->ty.desc() + ")",
                       "(24)");
            } else {
              item.sym->definedBase = base;
              isDefined = true;
            }
          } else {
            // Subscripted base DEFINED X(...) (rules 126,134): each base
            // subscript is a constant index or a single iSUB dummy. With an
            // iSUB, the item is a 1-D array overlaying that axis of X; with
            // only constants, it is a scalar overlay of one element.
            if (!base->ty.isArray()) {
              d_.error(item.loc,
                       "DEFINED base '" + item.definedBase +
                           "' is not an array and cannot be subscripted",
                       "(126)");
            } else if (item.definedSubs.size() != base->ty.dims.size()) {
              d_.error(item.loc,
                       "DEFINED base '" + item.definedBase + "' has " +
                           std::to_string(base->ty.dims.size()) + " dimension(s) but takes " +
                           std::to_string(item.definedSubs.size()) + " subscript(s)",
                       "(126)");
            } else {
              int isubAxis = -1;
              std::vector<long long> cst(base->ty.dims.size(), 0);
              long long iSubMult = 1, iSubAdd = 0;
              bool ok = true;
              for (size_t k = 0; k < item.definedSubs.size(); ++k) {
                const DefinedSub& ds = item.definedSubs[k];
                if (ds.isub) {
                  if (isubAxis >= 0) {
                    d_.error(item.loc, "a DEFINED base may have only one iSUB", "(134)");
                    ok = false;
                  } else {
                    isubAxis = (int)k;
                    iSubMult = ds.mult;
                    iSubAdd = ds.add;
                  }
                } else if (ds.expr && ds.expr->kind == Expr::IntLit) {
                  cst[k] = ds.expr->ival;
                  const Dim& d = base->ty.dims[k];
                  if (cst[k] < d.lb || cst[k] > d.ub)
                    d_.error(ds.expr->loc,
                             "DEFINED base subscript " + std::to_string(cst[k]) +
                                 " is out of bounds " + std::to_string(d.lb) + ":" +
                                 std::to_string(d.ub),
                             "(126)");
                } else {
                  d_.error(item.loc, "DEFINED base subscripts must be constant integers", "(24)");
                  ok = false;
                }
              }
              if (ok && isubAxis >= 0) {
                // iSUB overlay: Y is a 1-D array over X's iSUB axis. With
                // index arithmetic Y(i) -> X(m*i + c), the affine image of the
                // overlay's index range must stay within the base axis bounds.
                const Dim& dd = base->ty.dims[isubAxis];
                long long lo = iSubMult * item.ty.dims[0].lb + iSubAdd;
                long long hi = iSubMult * item.ty.dims[0].ub + iSubAdd;
                if (iSubMult < 0)
                  std::swap(lo, hi);
                bool match = item.ty.isArray() && item.ty.dims.size() == 1 &&
                             item.ty.elementType() == base->ty.elementType() && lo >= dd.lb &&
                             hi <= dd.ub;
                if (match) {
                  item.sym->definedBase = base;
                  item.sym->definedIsubAxis = isubAxis;
                  item.sym->definedConst = cst;
                  item.sym->definedIsubMult = iSubMult;
                  item.sym->definedIsubAdd = iSubAdd;
                  isDefined = true;
                } else {
                  d_.error(item.loc,
                           "DEFINED iSUB base requires the item to be a 1-D array of the iSUB "
                           "axis's element type whose affine image fits the base axis",
                           "(134)");
                }
              } else if (ok) {
                // All-constant element overlay: Y is a scalar of X's element type.
                if (item.ty == base->ty.elementType()) {
                  item.sym->definedBase = base;
                  item.sym->definedIsubAxis = -1;
                  item.sym->definedConst = cst;
                  isDefined = true;
                } else {
                  d_.error(item.loc,
                           "DEFINED element base requires the item to match the base's element "
                           "type (" +
                               item.ty.desc() + " vs " + base->ty.elementType().desc() + ")",
                           "(24)");
                }
              }
            }
          }
        }
        // BASED (rule 25): the declared item overlays the storage addressed by
        // a POINTER variable, so it has no storage of its own. The base must be
        // a POINTER variable/parameter in scope; a based structure member is
        // addressed through the pointer value at every reference. Resolution
        // defers to after all DECLAREs are collected, so a POINTER declared
        // later in the same procedure (or a bare POINTER parameter) resolves.
        if (!item.basedBase.empty()) {
          Symbol* base = lookup(sc, item.basedBase);
          if (base && (base->kind == Symbol::Var || base->kind == Symbol::Param) &&
              base->ty.isPointer())
            item.sym->basedBase = base;
          else
            pendingBased_.push_back({&item, sc, p});
        }
        // CONTROLLED (rule (15), ADR-140): storage managed by an explicit
        // generation stack. This slice accepts the attribute and diagnoses
        // invalid combinations; allocation itself stays out (rules 87-90).
        if (item.controlled) {
          item.sym->controlled = true;
          item.sym->ctlSlot = nextCtlSlot_++;
          auto ctlErr = [&](const char* what, const char* rule) {
            d_.error(item.loc, std::string("CONTROLLED+") + what + " is not implemented", rule);
          };
          if (item.init || !item.initItems.empty() || item.initCall)
            ctlErr("INITIAL", "(26)");
          if (item.valueInit)
            ctlErr("VALUE", "(ADR-108)");
          if (!item.like.empty())
            ctlErr("LIKE", "(15)");
          if (!item.definedBase.empty())
            ctlErr("DEFINED", "(24)");
          if (!item.basedBase.empty())
            ctlErr("BASED", "(25)");
          if (item.ty.isDynamic())
            d_.error(item.loc,
                     "a CONTROLLED array with dynamic extent is not implemented in this stage",
                     "(13)");
        }
        // OPTIONAL (extension, ADR-119): only valid on a procedure parameter;
        // the flag rides the symbol into call checking. ENTRY-statement
        // parameters resolve by name as well (rule 56).
        if (item.optional) {
          bool isParam =
              std::find(p->params.begin(), p->params.end(), item.name) != p->params.end();
          if (!isParam)
            for (auto& st : p->body)
              if (st && st->kind == Stmt::Entry &&
                  std::find(st->params.begin(), st->params.end(), item.name) != st->params.end())
                isParam = true;
          if (!isParam)
            d_.error(item.loc, "OPTIONAL is only valid on a procedure parameter (ADR-119)", "");
          else
            item.sym->isOptional = true;
        }
        // Only scalar (numeric/BIT) element arrays are served in this stage;
        // character element arrays are diagnosed, never silently miscompiled
        // (invariant 2). INITIAL on an array (rule 26) expands its itemlist
        // (with iteration factors and '*') into one value per element.
        if (item.ty.isArray() && !isDefined) {
          if (item.ty.elementType().isChar())
            d_.error(item.loc, "arrays of CHARACTER are not implemented in this stage", "(12)");
          if (item.ty.isDynamic()) {
            // Dynamic (runtime-extent) arrays (rules (12),(13)): this stage
            // serves only a single-axis AUTOMATIC array whose lower and upper
            // bounds may be runtime expressions. A `*` adjustable extent is a
            // parameter-only form whose bound is supplied by the caller at call
            // time. Resolve the runtime bound expressions' symbols so codegen
            // can evaluate them at entry to size the buffer.
            bool hasStar = false;
            for (const auto& d : item.ty.dims)
              if (d.adj)
                hasStar = true;
            bool isParam =
                std::find(p->params.begin(), p->params.end(), item.name) != p->params.end();
            if (hasStar && !isParam)
              d_.error(item.loc, "a '*' adjustable extent is only valid on a parameter", "(13)");
            if (hasStar && isParam && item.ty.dims.size() != 1)
              d_.error(item.loc, "a '*' extent parameter must be single-axis in this stage",
                       "(13)");
            for (auto& b : item.dynBounds)
              if (b)
                typeExpr(b.get(), sc, p);
            for (auto& b : item.dynLbBounds)
              if (b)
                typeExpr(b.get(), sc, p);
            // A dynamic array may be multi-axis, but only the first axis may
            // have a runtime extent; later axes must be constant in this stage.
            for (size_t k = 1; k < item.ty.dims.size(); ++k)
              if (item.ty.dims[k].dyn || item.ty.dims[k].lbDyn)
                d_.error(item.loc,
                         "a dynamic array may only have a dynamic first axis in this stage",
                         "(13)");
            // A single-axis dynamic lower bound on a parameter is served:
            // the bound exprs name caller-supplied params read at entry.
            if (item.sym->isStatic)
              d_.error(item.loc, "a dynamic array must be AUTOMATIC in this stage", "(13)");
            if (!item.initItems.empty()) {
              // INITIAL on a dynamic array (rule (26)): the extent is runtime, so
              // the itemlist cannot be count-checked here; expand it into the flat
              // element list and store it into the runtime buffer at block entry.
              std::vector<Expr*> elems;
              expandInitItems(item.initItems, item.ty.elementType(), item.loc, elems);
              item.sym->initElems = std::move(elems);
            }
          } else if (!item.initItems.empty()) {
            std::vector<Expr*> elems;
            expandInitItems(item.initItems, item.ty.elementType(), item.loc, elems);
            long long n = elementCount(item.ty);
            if ((long long)elems.size() != n)
              d_.error(item.loc,
                       "INITIAL supplies " + std::to_string(elems.size()) +
                           " value(s) for an array of " + std::to_string(n) + " element(s)",
                       "(26)");
            else
              item.sym->initElems = std::move(elems);
          }
        }
        // Adjustable CHARACTER length (rule (18)): `CHAR(*)` or `CHAR(expr)` on
        // a scalar character variable. The length is caller-supplied at call
        // time, so it is a parameter-only form in this stage; VARYING and
        // non-parameter uses are diagnosed, never silently fixed-length.
        if (item.ty.isChar() && (item.ty.starLen || item.slenExpr)) {
          bool isParam =
              std::find(p->params.begin(), p->params.end(), item.name) != p->params.end();
          if (!isParam)
            for (auto& st : p->body)
              if (st && st->kind == Stmt::Entry &&
                  std::find(st->params.begin(), st->params.end(), item.name) != st->params.end())
                isParam = true;
          if (!isParam)
            d_.error(item.loc, "an adjustable CHARACTER length is only valid on a parameter",
                     "(18)");
          else if (item.ty.varying)
            d_.error(item.loc,
                     "CHARACTER(*) VARYING parameters are not implemented in this stage", "(18)");
          else if (item.ty.isArray())
            d_.error(item.loc,
                     "an adjustable CHARACTER length on an array is not implemented in this stage",
                     "(12)");
          else if (item.slenExpr) {
            // CHAR(expr): type the runtime length expression (it usually names a
            // caller parameter read at entry, mirroring a dynamic array bound).
            typeExpr(item.slenExpr.get(), sc, p);
            if (!item.slenExpr->ty.isNumeric())
              d_.error(item.slenExpr->loc, "CHARACTER length must be numeric", "(18)");
          }
        }
        if (item.ty.isStruct() &&
            (!item.initItems.empty() || structHasMemberInit(items, children, idx))) {
          // INITIAL on a structure (rule (26)): flatten the itemlist (iteration
          // factors, '*' and groups) and fold each value against its member's
          // type in declaration order. The count must match the scalar leaves.
          if (item.sym->isStatic) {
            d_.error(item.loc, "INITIAL on a static structure is not implemented in this stage",
                     "(26)");
          } else if (hasDynamicMember(item.ty)) {
            // INITIAL covering a dynamic member (rules (13),(26), ADR-092):
            // only a trailing top-level dynamic array member is served — the
            // itemlist fills the static leaves first, then one buffer element
            // per remaining value (no compile-time count check: the extent is
            // runtime). Any other dynamic layout is diagnosed, never silently
            // misfilled.
            // A member counts when it is itself a dynamic array or holds one
            // deeper: hasDynamicMember only sees dynamics nested inside its
            // argument, never a dynamic array passed directly.
            std::vector<unsigned> dynTops;
            for (unsigned i = 0; i < item.ty.members.size(); ++i) {
              const Type& mt = item.ty.members[i]->ty;
              if ((mt.isArray() && mt.isDynamic()) || hasDynamicMember(mt))
                dynTops.push_back(i);
            }
            const unsigned last = (unsigned)item.ty.members.size() - 1;
            const Type& tail = item.ty.members[last]->ty;
            if (dynTops.size() != 1 || dynTops[0] != last || !tail.isArray() ||
                tail.elementType().isStruct() || tail.elementType().isChar()) {
              d_.error(item.loc,
                       "INITIAL on a structure with a non-trailing or nested dynamic member is "
                       "not implemented in this stage",
                       "(26)");
            } else {
              std::vector<Expr*> raw;
              flattenInitItems(item.initItems, item.loc, raw);
              long long statics = 0;
              for (unsigned i = 0; i < last; ++i)
                statics += structureLeafCount(item.ty.members[i]->ty);
              if ((long long)raw.size() < statics)
                d_.error(item.loc,
                         "INITIAL supplies " + std::to_string(raw.size()) +
                             " value(s) for a structure needing " + std::to_string(statics) +
                             " static value(s) first",
                         "(26)");
              else {
                std::vector<Expr*> folded;
                size_t foldIdx = 0;
                for (unsigned i = 0; i < last; ++i)
                  foldStructInit(item.ty.members[i]->ty, raw, foldIdx, item.loc, folded);
                const Type& el = tail.elementType();
                while (idx < (int)raw.size())
                  if (Expr* f = foldInitialConstant(raw[idx++], el, item.loc))
                    folded.push_back(f);
                item.sym->initElems = std::move(folded);
              }
            }
          } else {
            // Per-member INITIAL (rule (26)): each member's own itemlist (or
            // scalar init) fills its own leaves in member order; a member
            // without one contributes no values. A structure-level itemlist
            // combined with member INITIALs stays diagnosed (single source).
            std::vector<Expr*> raw;
            if (!item.initItems.empty() || item.init || item.initCall || item.valueInit) {
              if (structHasMemberInit(items, children, idx))
                d_.error(item.loc,
                         "INITIAL on a structure combined with a member INITIAL is not "
                         "implemented in this stage",
                         "(26)");
              else if (!item.initItems.empty())
                flattenInitItems(item.initItems, item.loc, raw);
              else if (item.init)
                raw.push_back(item.init.get());
              else if (item.valueInit)
                raw.push_back(item.valueInit.get());
              else
                typeExpr(item.initCall.get(), sc, p); // diagnosed below like scalars
            } else {
              // Walk the declaration subtree in member order, expanding each
              // member's own INITIAL (recursing into nested structures). A
              // member without one contributes its zero-fill slot: the raw
              // list always covers every scalar leaf, so per-member INITIAL
              // never shifts its siblings (invariant 2).
              std::function<void(const Type&)> gatherEmpty = [&](const Type& mt) {
                if (mt.isStruct()) {
                  for (const auto& m : mt.members)
                    gatherEmpty(m->ty);
                  return;
                }
                if (mt.isArray()) {
                  long long cnt = elementCount(mt);
                  const Type& el = mt.elementType();
                  for (long long k = 0; k < cnt; ++k) {
                    if (el.isStruct())
                      gatherEmpty(el);
                    else
                      raw.push_back(nullptr); // zero-fill slot
                  }
                  return;
                }
                raw.push_back(nullptr); // zero-fill slot
              };
              std::function<void(int)> gather = [&](int c) {
                const DeclItem* mi = items[c];
                if (!children[c].empty()) {
                  for (int g : children[c])
                    gather(g);
                  return;
                }
                // The member's own type comes from the built parent type
                // (dimensions live there, not on the raw DeclItem).
                const Type* mt = nullptr;
                for (const auto& m : item.ty.members)
                  if (m->name == mi->name) {
                    mt = &m->ty;
                    break;
                  }
                if (!mi->initItems.empty()) {
                  std::vector<Expr*> sub;
                  flattenInitItems(mi->initItems, mi->loc, sub);
                  // A member array INITIAL fills its own elements (rule (26)):
                  // pad short lists with zero-fill slots so the member always
                  // contributes its full element count.
                  long long need = mt && mt->isArray() ? elementCount(*mt) : 1;
                  for (Expr* se : sub)
                    raw.push_back(se);
                  for (long long k = (long long)sub.size(); k < need; ++k)
                    raw.push_back(nullptr); // zero-fill slot
                } else if (mi->init) {
                  raw.push_back(mi->init.get());
                } else if (mi->valueInit) {
                  raw.push_back(mi->valueInit.get());
                } else if (mt) {
                  gatherEmpty(*mt);
                }
              };
              for (int c : children[idx])
                gather(c);
            }
            long long leaves = structureLeafCount(item.ty);
            if ((long long)raw.size() != leaves)
              d_.error(item.loc,
                       "INITIAL supplies " + std::to_string(raw.size()) +
                           " value(s) for a structure of " + std::to_string(leaves) + " member(s)",
                       "(26)");
            else {
              std::vector<Expr*> folded;
              size_t foldIdx = 0;
              foldStructInit(item.ty, raw, foldIdx, item.loc, folded);
              item.sym->initElems = std::move(folded);
            }
          }
        }
        // Record AUTOMATIC variables so codegen allocates them (STATIC ones
        // become LLVM globals via emitGlobals). This must cover variables of
        // BEGIN blocks too, hence the Proc* here. A DEFINED or BASED variable
        // has no storage of its own, so it is never allocated; neither is a
        // CONTROLLED variable, whose generations live on the runtime stack.
        // A BASED item is excluded by its syntactic attribute: its base
        // resolves after all DECLAREs are collected (deferred, rule 25).
        if (item.sym->kind == Symbol::Var && !item.sym->isStatic && !item.sym->definedBase &&
            item.basedBase.empty() && !item.sym->basedBase && !item.sym->fileAttr &&
            !item.sym->controlled)
          p->localSyms.push_back(item.sym);
        if (item.init) {
          // M0 accepts a literal (optionally signed) as INITIAL value.
          if (Expr* folded = foldInitialConstant(item.init.get(), item.ty, item.loc))
            item.sym->initExpr = folded; // consumed by code generation
        }
        if (item.initCall) {
          // INITIAL(CALL f(...)) (rule 27): type-check the call — this resolves
          // the function, its arguments, and its return type. Store the call on
          // the symbol only when it returns a value; a non-function (void) call
          // was already diagnosed in typeExpr.
          typeExpr(item.initCall.get(), sc, p);
          if (!item.initCall->ty.isVoid())
            item.sym->initCall = item.initCall.get(); // consumed by codegen
        }
        if (item.valueInit) {
          // VALUE(const) (extension, ADR-108): a scalar named constant. It
          // keeps AUTOMATIC storage initialized once from the folded
          // constant; every write position is diagnosed via checkValueTarget.
          if (item.sym->fileAttr)
            d_.error(item.loc,
                     "VALUE on a FILE variable is not implemented in this stage (ADR-108)", "");
          else if (!item.definedBase.empty() || !item.basedBase.empty())
            d_.error(item.loc,
                     "VALUE on a DEFINED/BASED variable is not implemented in this stage "
                     "(ADR-108)",
                     "");
          else if (item.ty.isArray() || item.ty.isStruct() || item.ty.isPointer() ||
                   item.ty.isComplex())
            d_.error(item.loc,
                     "VALUE on arrays, structures, pointers, and complex data is not implemented "
                     "in this stage (ADR-108)",
                     "");
          else {
            if (Expr* folded = foldInitialConstant(item.valueInit.get(), item.ty, item.loc))
              item.sym->initExpr = folded; // consumed by code generation
            item.sym->isValue = true;
          }
        }
      }
      continue;
    }
    if (s->kind == Stmt::Begin) {
      // A BEGIN block (rule (68)) opens a child scope: names declared inside
      // shadow outer ones and do not leak out.
      auto child = std::make_unique<Scope>();
      child->parent = sc;
      Scope* raw = child.get();
      scopes_.push_back(std::move(child));
      beginScopes_[s.get()] = raw;
      collectDecls(s->body, raw, p, isStatic);
      continue;
    }
    // Declarations in DO/IF bodies contribute to the enclosing scope.
    if (!s->body.empty())
      collectDecls(s->body, sc, p, isStatic);
    // THEN/ELSE branches also need BEGIN scope creation (rule (68)) and
    // the bare-DECLARE diagnostic (rule (74)).
    if (s->thenS) {
      if (s->thenS->kind == Stmt::Begin) {
        auto child = std::make_unique<Scope>();
        child->parent = sc;
        Scope* raw = child.get();
        scopes_.push_back(std::move(child));
        beginScopes_[s->thenS.get()] = raw;
        collectDecls(s->thenS->body, raw, p, isStatic);
      } else if (s->thenS->kind == Stmt::Declare || s->thenS->kind == Stmt::DefineAlias) {
        d_.error(s->thenS->loc, "DECLARE/DEFINE cannot be the body of an IF statement", "(74)");
      }
    }
    if (s->elseS) {
      if (s->elseS->kind == Stmt::Begin) {
        auto child = std::make_unique<Scope>();
        child->parent = sc;
        Scope* raw = child.get();
        scopes_.push_back(std::move(child));
        beginScopes_[s->elseS.get()] = raw;
        collectDecls(s->elseS->body, raw, p, isStatic);
      } else if (s->elseS->kind == Stmt::Declare || s->elseS->kind == Stmt::DefineAlias) {
        d_.error(s->elseS->loc, "DECLARE/DEFINE cannot be the body of an IF statement", "(74)");
      }
    }
  }
}

// Gather the labels (rule (64)) defined anywhere in this procedure so that a
// GO TO target (rule (77)) can be resolved. M1 treats every label of the
// procedure body as visible; non-local GO TO to an enclosing procedure is M5.
void Sema::collectLabels(Stmt* s) {
  if (!s)
    return;
  for (const std::string& l : s->labels)
    procLabels_.insert(l);
  if (s->thenS)
    collectLabels(s->thenS.get());
  if (s->elseS)
    collectLabels(s->elseS.get());
  for (auto& b : s->body)
    collectLabels(b.get());
}

// True when the statement subtree rooted at `s` contains kind `k` (through
// branches, blocks and ON-units).
static bool stmtHasKind(const Stmt* s, Stmt::Kind k) {
  if (!s)
    return false;
  if (s->kind == k)
    return true;
  if (stmtHasKind(s->thenS.get(), k) || stmtHasKind(s->elseS.get(), k) ||
      stmtHasKind(s->unit.get(), k))
    return true;
  for (auto& b : s->body)
    if (stmtHasKind(b.get(), k))
      return true;
  return false;
}

// True when any statement in `body` contains kind `k`.
static bool stmtsHaveKind(const std::vector<StmtP>& body, Stmt::Kind k) {
  for (auto& s : body)
    if (stmtHasKind(s.get(), k))
      return true;
  return false;
}

// True when a declaration subtree carries a per-member INITIAL (rule (26)):
// any descendant with its own init/initItems/initCall/valueInit.
static bool structHasMemberInit(const std::vector<DeclItem*>& items,
                                const std::vector<std::vector<int>>& children, int idx) {
  for (int c : children[idx]) {
    const DeclItem* it = items[c];
    if (it->init || it->initCall || it->valueInit || !it->initItems.empty())
      return true;
    if (structHasMemberInit(items, children, c))
      return true;
  }
  return false;
}

bool Sema::checkAssignable(const Type& dst, const Type& src, SourceLoc loc, const char* what) {
  if (dst.isStruct() || src.isStruct()) {
    // Whole-structure assignment (rule 127): a copy between two structures of
    // identical shape (same members, recursively). Anything else — a shape
    // mismatch, or mixing a structure with a non-structure — is diagnosed.
    if (dst.isStruct() && src.isStruct() && dst == src) {
      // A struct with a dynamic-array member holds a pointer to a runtime
      // buffer: plain assignment deep-copies each buffer (rule (13),
      // ADR-091), while by-value arguments and function results stay
      // diagnosed so they are never silently pointer-copied.
      if (hasDynamicMember(dst) && std::string(what) != "assignment")
        d_.error(loc,
                 std::string(what) +
                     ": a whole-structure value with a dynamic member is not implemented here",
                 "(13)");
      return true;
    }
    if (dst.isStruct() && src.isStruct())
      d_.error(loc,
               std::string(what) +
                   ": whole-structure assignment requires identical structure shapes",
               "(127)");
    else
      d_.error(loc,
               std::string(what) +
                   ": a whole structure cannot be mixed with a non-structure in this position",
               "(127)");
    return false;
  }
  if (dst.isNumeric() && (src.isNumeric() || src.isBit()))
    return true;
  // TASK/EVENT assignment (rules (15),(79),(82), QR2.8): same-type copies
  // (e.g. passing a TASK/EVENT by reference) are plain handle copies.
  if (dst.isTask() && src.isTask())
    return true;
  if (dst.isEvent() && src.isEvent())
    return true;
  // POINTER assignment (rule 15): copy the address; a pointer target takes a
  // pointer source (NULL, ADDR, or another pointer) unchanged.
  if (dst.isPointer() && src.isPointer())
    return true;
  if (dst.isBit() && (src.isBit() || src.isNumeric()))
    return true;
  if (dst.isChar() && src.isChar())
    return true;
  // Complex conversions (QR2.2/CM5): complex <-> complex passes through; a real
  // (numeric) value converts to complex with a zero imaginary part, and a
  // complex value converts to a real by taking the real part.
  if (dst.isComplex() || src.isComplex()) {
    if (dst.isComplex() && (src.isComplex() || src.isNumeric()))
      return true;
    if (dst.isNumeric() && src.isComplex())
      return true;
  }
  d_.error(loc,
           std::string(what) + ": conversion from " + src.desc() + " to " + dst.desc() +
               " is not implemented in this stage",
           "(86)");
  return false;
}

const Type* Sema::structLeafType(Expr* e) {
  if (e->kind != Expr::VarRef || !e->sym || e->sym->kind == Symbol::ProcName)
    return nullptr;
  if (!e->ty.isStruct())
    return nullptr;
  return &e->ty;
}

void Sema::checkByNameMatch(const Type& dst, const Type& src, SourceLoc loc) {
  // BY NAME (rule 86): walk the target structure's members; a member is copied
  // from the same-named member of the source when both are present. Nested
  // structures recurse by name, so the two layouts need not match.
  for (const auto& dm : dst.members) {
    const Member* sm = nullptr;
    for (const auto& m : src.members)
      if (m->name == dm->name) {
        sm = m.get();
        break;
      }
    if (!sm)
      continue; // name absent from the source: skipped, not an error
    if (dm->ty.isStruct() && sm->ty.isStruct()) {
      checkByNameMatch(dm->ty, sm->ty, loc);
    } else if (dm->ty.isStruct() || sm->ty.isStruct()) {
      d_.error(loc,
               "BY NAME assignment mixes a structure with a non-structure member '" + dm->name +
                   "'",
               "(86)");
    } else if (dm->ty.isArray() || sm->ty.isArray()) {
      if (!(dm->ty == sm->ty))
        d_.error(loc,
                 "BY NAME array member '" + dm->name + "' must have the same type on both sides",
                 "(86)");
    } else if (dm->ty.isChar() || sm->ty.isChar()) {
      d_.error(loc, "CHARACTER structure members are not implemented in this stage", "(11)");
    } else {
      checkAssignable(dm->ty, sm->ty, loc, "BY NAME assignment");
    }
  }
}

Expr* Sema::foldInitialConstant(Expr* e, const Type& ty, SourceLoc loc) {
  Expr* lit = e;
  if (lit->kind == Expr::Unary && lit->op == Tok::Minus)
    lit = lit->a.get();
  if (lit->kind != Expr::IntLit && lit->kind != Expr::DecLit && lit->kind != Expr::FltLit &&
      lit->kind != Expr::CharLit && lit->kind != Expr::BitLit) {
    d_.error(loc, "INITIAL requires a constant in this stage", "(26)");
    return nullptr;
  }
  if (lit != e) { // apply the leading unary minus once
    lit->ival = -lit->ival;
    lit->fval = -lit->fval;
  }
  bool strInit = lit->kind == Expr::CharLit;
  if (ty.isChar() != strInit) {
    d_.error(loc, "INITIAL value is not compatible with " + ty.desc(), "(26)");
    return nullptr;
  }
  return lit;
}

void Sema::expandInitItems(const std::vector<InitItem>& items, const Type& elemTy, SourceLoc loc,
                           std::vector<Expr*>& out) {
  for (const InitItem& it : items) {
    switch (it.kind) {
    case InitItem::Value:
      if (Expr* f = foldInitialConstant(it.value.get(), elemTy, loc))
        out.push_back(f);
      break;
    case InitItem::Repeat:
      if (out.empty())
        d_.error(loc, "'*' in INITIAL has no preceding value to repeat", "(29)");
      else
        out.push_back(out.back()); // repeat the last folded value
      break;
    case InitItem::Group:
      expandInitItems(it.items, elemTy, loc, out);
      break;
    case InitItem::Iter: {
      std::vector<Expr*> sub;
      expandInitItems(it.items, elemTy, loc, sub);
      for (long long k = 0; k < it.factor; ++k)
        for (Expr* s : sub)
          out.push_back(s);
      break;
    }
    }
  }
}

long long Sema::elementCount(const Type& ty) {
  long long n = 1;
  for (const auto& d : ty.dims)
    n *= (long long)(d.ub - d.lb + 1);
  return n;
}

// Rule (91): an ON-unit compiles to a handler function without the
// establishing frame, so constructs needing that frame are diagnosed:
// automatic-variable access, RETURN with a value, DECLARE and ENTRY.
// A nested ON is allowed: it establishes a handler when the outer unit runs.
void Sema::checkOnUnit(Stmt* u, Proc* p) {
  if (!u)
    return;
  std::function<void(Expr*)> checkExpr = [&](Expr* e) {
    if (!e)
      return;
    if ((e->kind == Expr::VarRef || e->kind == Expr::Subscript) && e->sym &&
        e->sym->kind != Symbol::ProcName && e->sym->owner) {
      d_.error(e->loc, "an ON-unit reaching an automatic variable is not implemented in this stage",
               "(91)");
    }
    if (e->a)
      checkExpr(e->a.get());
    if (e->b)
      checkExpr(e->b.get());
    for (auto& a : e->args)
      checkExpr(a.get());
  };
  std::function<void(Stmt*)> check = [&](Stmt* s) {
    if (!s)
      return;
    switch (s->kind) {
    case Stmt::On:
      // A nested ON establishes a new handler when the outer unit runs
      // (rule 91). Its own unit was already type-checked and validated
      // by checkStmt, so skip it here to avoid duplicate diagnostics.
      return;
    case Stmt::Return:
      // A bare RETURN ends the unit and resumes after the SIGNAL (rule 91);
      // RETURN with a value has no function to return from here.
      if (s->value)
        d_.error(s->loc, "RETURN with a value inside an ON-unit is not implemented in this stage",
                 "(91)");
      break;
    case Stmt::Leave:
      d_.error(s->loc, "LEAVE inside an ON-unit is not implemented in this stage", "(91)");
      break;
    case Stmt::Iterate:
      d_.error(s->loc, "ITERATE inside an ON-unit is not implemented in this stage", "(91)");
      break;
    case Stmt::Declare:
      d_.error(s->loc, "DECLARE inside an ON-unit is not implemented in this stage", "(91)");
      break;
    case Stmt::DefineAlias:
      d_.error(s->loc, "DEFINE ALIAS inside an ON-unit is not implemented in this stage", "(91)");
      break;
    case Stmt::Entry:
      d_.error(s->loc, "ENTRY inside an ON-unit is not implemented in this stage", "(91)");
      break;
    case Stmt::DoIter:
      // The control variable lives in the establishing frame.
      if (s->sym && s->sym->owner)
        d_.error(s->loc,
                 "an ON-unit reaching an automatic variable is not implemented in this stage",
                 "(91)");
      break;
    default:
      break;
    }
    checkExpr(s->target.get());
    for (auto& t : s->extraTargets)
      checkExpr(t.get());
    checkExpr(s->value.get());
    checkExpr(s->cond.get());
    checkExpr(s->from.get());
    checkExpr(s->to.get());
    checkExpr(s->by.get());
    checkExpr(s->skipCount.get());
    for (auto& it : s->items)
      checkExpr(it.get());
    for (auto& a : s->args)
      checkExpr(a.get());
    if (s->thenS)
      check(s->thenS.get());
    if (s->elseS)
      check(s->elseS.get());
    if (s->unit)
      check(s->unit.get());
    for (auto& b : s->body)
      check(b.get());
  };
  check(u);
}

long long Sema::structureLeafCount(const Type& ty) {
  long long n = 0;
  for (const auto& m : ty.members) {
    if (m->ty.isStruct()) {
      n += structureLeafCount(m->ty);
    } else if (m->ty.isArray()) {
      long long cnt = elementCount(m->ty);
      if (m->ty.elementType().isStruct())
        n += cnt * structureLeafCount(m->ty.elementType());
      else
        n += cnt;
    } else {
      n += 1;
    }
  }
  return n;
}

void Sema::flattenInitItems(const std::vector<InitItem>& items, SourceLoc loc,
                            std::vector<Expr*>& out) {
  for (const InitItem& it : items) {
    switch (it.kind) {
    case InitItem::Value:
      out.push_back(it.value.get());
      break;
    case InitItem::Repeat:
      if (out.empty())
        d_.error(loc, "'*' in INITIAL has no preceding value to repeat", "(29)");
      else
        out.push_back(out.back()); // repeat the last raw value
      break;
    case InitItem::Group:
      flattenInitItems(it.items, loc, out);
      break;
    case InitItem::Iter: {
      std::vector<Expr*> sub;
      flattenInitItems(it.items, loc, sub);
      for (long long k = 0; k < it.factor; ++k)
        for (Expr* s : sub)
          out.push_back(s);
      break;
    }
    }
  }
}

void Sema::foldStructInit(const Type& ty, const std::vector<Expr*>& vals, size_t& idx,
                          SourceLoc loc, std::vector<Expr*>& out) {
  for (const auto& m : ty.members) {
    if (m->ty.isStruct()) {
      foldStructInit(m->ty, vals, idx, loc, out);
    } else if (m->ty.isArray()) {
      long long cnt = elementCount(m->ty);
      const Type& el = m->ty.elementType();
      for (long long k = 0; k < cnt; ++k) {
        if (el.isStruct()) {
          foldStructInit(el, vals, idx, loc, out);
        } else if (idx < vals.size()) {
          // A null slot marks a per-member-INIT zero-fill (rule (26)): push
          // it through so irgen's positional walk stays aligned.
          Expr* ve = vals[idx++];
          if (!ve) {
            out.push_back(nullptr);
            continue;
          }
          if (Expr* f = foldInitialConstant(ve, el, loc))
            out.push_back(f);
        }
      }
    } else if (idx < vals.size()) {
      Expr* ve = vals[idx++];
      if (!ve) {
        out.push_back(nullptr);
        continue;
      }
      if (Expr* f = foldInitialConstant(ve, m->ty, loc))
        out.push_back(f);
    }
  }
}

// Rules (94),(99): resolve a condition to its dispatch key (0 = ERROR, the
// negative Stmt::k*CondKey constants are the fixed conditions, else a
// programmer-named condition index + 1).
int Sema::resolveCondKey(Stmt* s, Scope* sc) {
  if (s->condName == "ERROR")
    return 0;
  if (s->condName == "SIZE")
    return Stmt::kSizeCondKey;
  if (s->condName == "SUBSCRIPTRANGE")
    return Stmt::kSubscriptrangeCondKey;
  if (s->condName == "ZERODIVIDE")
    return Stmt::kZerodivideCondKey;
  auto& names = prog_->condNames;
  auto it = std::find(names.begin(), names.end(), s->condName);
  int key = it == names.end() ? (int)names.size() + 1 : (int)(it - names.begin()) + 1;
  if (it == names.end())
    names.push_back(s->condName);
  // A use-declared name must not collide with a declared entity, except for an
  // explicit DECLARE ... CONDITION which created it as a ProcName.
  if (Symbol* sym = lookup(sc, s->condName)) {
    if (!sym->isCond) {
      if (sym->kind == Symbol::ProcName)
        d_.error(s->loc, "'" + s->condName + "' is a procedure, not a condition name", "(99)");
      else
        d_.error(s->loc, "'" + s->condName + "' is a variable, not a condition name", "(99)");
    }
  }
  return key;
}

// Whole-array expressions (rules 86, 127; QR2.1): storage a DO desugar can
// address — a variable with its own storage. Parameters (hidden extents),
// DEFINED overlays, BASED storage, and dynamic structure members stay
// diagnosed in this stage.
bool Sema::wholeArrayStorageOk(Expr* e) {
  Symbol* s = e->sym;
  if (!s || s->kind != Symbol::Var || s->definedBase || s->basedBase)
    return false;
  if (!e->memberPath.empty() && e->ty.isDynamic())
    return false;
  return true;
}

// Pure predicate matching what rewriteWholeArrayRefs would expand: a
// whole-array VarRef outside any Call, or a cross-section value anywhere.
// Served reductions and array-parameter calls keep their whole form.
bool Sema::valueHasWholeArrayRef(Expr* e, bool underCall) {
  if (!e)
    return false;
  if (e->kind == Expr::VarRef && e->ty.isArray())
    return !underCall;
  if (e->kind == Expr::Subscript && e->ty.isArray())
    return true;
  if (e->kind == Expr::Call) {
    for (auto& a : e->args)
      if (valueHasWholeArrayRef(a.get(), true))
        return true;
    return false;
  }
  if (e->kind == Expr::Binary)
    return valueHasWholeArrayRef(e->a.get(), underCall) ||
           valueHasWholeArrayRef(e->b.get(), underCall);
  if (e->kind == Expr::Unary)
    return valueHasWholeArrayRef(e->a.get(), underCall);
  if (e->kind == Expr::Subscript) {
    for (auto& a : e->args)
      if (valueHasWholeArrayRef(a.get(), underCall))
        return true;
    return false;
  }
  return false;
}

// Rewrite whole-array VarRef uses to subscript calls over the index names.
// References under a Call keep their whole form (served reductions and
// array-parameter calls consume whole arrays); a nested cross-section value
// has no element-wise form here and is diagnosed. Returns false after a
// diagnostic.
bool Sema::rewriteWholeArrayRefs(ExprP& e, const std::vector<std::string>& idx, const Type& shape,
                                 bool underCall) {
  if (!e)
    return true;
  if (e->kind == Expr::VarRef && e->ty.isArray()) {
    if (underCall)
      return true;
    // Only the shape (bounds per axis) must match: element types convert on
    // the per-element store, as in a scalar assignment. Bounds must match
    // exactly since every side shares the target's indices.
    if (e->ty.dims != shape.dims) {
      d_.error(e->loc, "whole-array assignment requires identical array shapes in this stage",
               "(86)");
      return false;
    }
    if (!wholeArrayStorageOk(e.get())) {
      d_.error(e->loc,
               "whole-array assignment of parameters, DEFINED overlays, BASED storage, or "
               "dynamic members is not implemented in this stage",
               "(86)");
      return false;
    }
    auto sub = std::make_unique<Expr>();
    sub->kind = Expr::Call;
    sub->name = e->name;
    sub->path = e->path;
    sub->loc = e->loc;
    for (const std::string& in : idx) {
      auto a = std::make_unique<Expr>();
      a->kind = Expr::VarRef;
      a->name = in;
      a->loc = e->loc;
      sub->args.push_back(std::move(a));
    }
    e = std::move(sub);
    return true;
  }
  if (e->kind == Expr::Subscript && e->ty.isArray()) {
    d_.error(e->loc, "a cross-section inside an array expression is not implemented in this stage",
             "(126)");
    return false;
  }
  if (e->kind == Expr::Call) {
    for (auto& a : e->args)
      if (!rewriteWholeArrayRefs(a, idx, shape, true))
        return false;
    return true;
  }
  if (e->kind == Expr::Binary)
    return rewriteWholeArrayRefs(e->a, idx, shape, underCall) &&
           rewriteWholeArrayRefs(e->b, idx, shape, underCall);
  if (e->kind == Expr::Unary)
    return rewriteWholeArrayRefs(e->a, idx, shape, underCall);
  if (e->kind == Expr::Subscript) {
    for (auto& a : e->args)
      if (!rewriteWholeArrayRefs(a, idx, shape, underCall))
        return false;
    return true;
  }
  return true;
}

// Expand `T = <array expression>` into explicit DO loops over the target's
// axes (rules 86, 127). The loops carry live bounds, so dynamic extents work
// and a source/target extent mismatch traps through the per-element
// SUBSCRIPTRANGE checks; scalar subexpressions evaluate per iteration, as the
// written loop would. Returns true when rewritten (the caller then checks the
// statement as a DO group), false otherwise: identical static plain shapes
// and scalar broadcasts ride the existing paths, anything unsupported is
// diagnosed here.
bool Sema::expandWholeArrayAssign(Stmt* s, Scope* sc, Proc* p) {
  Expr* t = s->target.get();
  Expr* v = s->value.get();
  // A scalar target for an array-valued source mixes ranks (rule 86).
  // The gate only routes here when the value carries a whole-array
  // reference, so this always diagnoses.
  if (!t->ty.isArray()) {
    d_.error(s->loc, "cannot assign an array value to a scalar variable in this stage", "(86)");
    return false;
  }
  // Identical static plain shapes ride the aggregate-copy path below.
  if (v->kind == Expr::VarRef && v->ty.isArray() && v->ty == t->ty && t->memberPath.empty() &&
      v->memberPath.empty() && !t->ty.isDynamic() && wholeArrayStorageOk(t) &&
      wholeArrayStorageOk(v))
    return false;
  const size_t n = t->ty.dims.size();
  // Axes past the first are always static in this stage; guard anyway since a
  // live-bound loop over one would read the wrong axis.
  for (size_t k = 1; k < n; ++k)
    if (t->ty.dims[k].dyn || t->ty.dims[k].lbDyn) {
      d_.error(s->loc,
               "whole-array assignment over a dynamic non-first axis is not implemented in this "
               "stage",
               "(13)");
      return false;
    }
  if (!wholeArrayStorageOk(t)) {
    d_.error(s->loc,
             "whole-array assignment of parameters, DEFINED overlays, BASED storage, or dynamic "
             "members is not implemented in this stage",
             "(86)");
    return false;
  }
  // Fresh index per axis, pre-declared so no implicit-declaration warning fires.
  std::vector<std::string> idx(n);
  for (size_t k = 0; k < n; ++k) {
    std::string base = "PLI$WA" + std::to_string(k);
    while (lookup(sc, base))
      base += "X";
    idx[k] = base;
    Symbol* is = declare(sc, base, Type::fixedBin(31, 0), s->loc, Symbol::Var, false);
    is->owner = p;
    p->localSyms.push_back(is);
  }
  // Rewrite the value tree first: every whole reference becomes a subscript
  // over the fresh indices (mismatches diagnosed inside).
  if (!rewriteWholeArrayRefs(s->value, idx, t->ty, false))
    return false;
  // One bound expression per axis: constants for static axes, live
  // LBOUND/HBOUND for a dynamic first axis.
  auto boundExpr = [&](size_t k, bool upper) -> ExprP {
    const Dim& d = t->ty.dims[k];
    if (!d.dyn && !d.lbDyn) {
      auto e = std::make_unique<Expr>();
      e->kind = Expr::IntLit;
      e->ival = upper ? d.ub : d.lb;
      e->loc = s->loc;
      return e;
    }
    auto e = std::make_unique<Expr>();
    e->kind = Expr::Call;
    e->name = upper ? "HBOUND" : "LBOUND";
    e->loc = s->loc;
    auto a = std::make_unique<Expr>();
    a->kind = Expr::VarRef;
    a->name = t->name;
    a->path = t->path;
    a->loc = s->loc;
    e->args.push_back(std::move(a));
    return e;
  };
  // The inner assignment assigns one element from the rewritten value.
  auto inner = std::make_unique<Stmt>();
  inner->kind = Stmt::Assign;
  inner->loc = s->loc;
  inner->noSize = s->noSize;
  inner->noSub = s->noSub;
  inner->noZdiv = s->noZdiv;
  {
    auto tgt = std::make_unique<Expr>();
    tgt->kind = Expr::Call;
    tgt->name = t->name;
    tgt->path = t->path;
    tgt->loc = t->loc;
    for (const std::string& in : idx) {
      auto a = std::make_unique<Expr>();
      a->kind = Expr::VarRef;
      a->name = in;
      a->loc = t->loc;
      tgt->args.push_back(std::move(a));
    }
    inner->target = std::move(tgt);
  }
  inner->value = std::move(s->value);
  // Nest one DO loop per axis around it, outermost first.
  StmtP body = std::move(inner);
  for (size_t k = n; k-- > 0;) {
    auto loop = std::make_unique<Stmt>();
    loop->kind = Stmt::DoIter;
    loop->loc = s->loc;
    loop->name = idx[k];
    loop->from = boundExpr(k, false);
    loop->to = boundExpr(k, true);
    loop->body.push_back(std::move(body));
    body = std::move(loop);
  }
  // The statement becomes its outermost loop; the caller checks it as one.
  StmtP outer = std::move(body);
  s->kind = Stmt::DoIter;
  s->name = std::move(outer->name);
  s->from = std::move(outer->from);
  s->to = std::move(outer->to);
  s->by = std::move(outer->by);
  s->cond = std::move(outer->cond);
  s->body = std::move(outer->body);
  s->target.reset();
  s->value.reset();
  s->byName = false;
  return true;
}

// Deep-copy an expression tree (fixed cross-section indices fan out to one
// copy per transmitted element).
static ExprP cloneExpr(const Expr* e) {
  if (!e)
    return nullptr;
  auto c = std::make_unique<Expr>();
  c->kind = e->kind;
  c->loc = e->loc;
  c->ty = e->ty;
  c->ival = e->ival;
  c->fval = e->fval;
  c->decScale = e->decScale;
  c->decPrec = e->decPrec;
  c->sval = e->sval;
  c->name = e->name;
  c->path = e->path;
  c->memberPath = e->memberPath;
  c->sym = e->sym;
  if (e->locPtr)
    c->locPtr = cloneExpr(e->locPtr.get());
  c->op = e->op;
  if (e->a)
    c->a = cloneExpr(e->a.get());
  if (e->b)
    c->b = cloneExpr(e->b.get());
  for (auto& a : e->args)
    c->args.push_back(cloneExpr(a.get()));
  return c;
}

void Sema::checkDoIter(Stmt* s, Scope* sc, Proc* p) {
  Symbol* sym = lookup(sc, s->name);
  if (!sym) {
    sym = implicitDeclare(sc, s->name, s->loc, false);
    sym->owner = p;
    p->localSyms.push_back(sym); // implicit vars are AUTOMATIC storage
  }
  s->sym = sym;
  if (sym->isValue)
    d_.error(s->loc,
             "cannot assign to the VALUE constant '" + sym->name +
                 "' as a DO control variable (ADR-108)",
             "");
  if (!sym->ty.isNumeric())
    d_.error(s->loc, "DO control variable must be arithmetic, found " + sym->ty.desc(), "(72)");
  // A scaled FIXED control variable would miscompile the loop step and
  // comparison at the scaled representation (invariant 2).
  if (sym->ty.isFixed() && sym->ty.scale != 0)
    d_.error(s->loc, "a scaled FIXED DO control variable is not implemented in this stage",
             "(16)");
  typeExpr(s->from.get(), sc, p);
  typeExpr(s->to.get(), sc, p);
  typeExpr(s->by.get(), sc, p);
  typeExpr(s->cond.get(), sc, p);
  Stmt* save = curEntry_;
  loopStack_.push_back(s->labels);
  for (auto& b : s->body) {
    if (b && b->kind == Stmt::Entry)
      curEntry_ = b.get();
    checkStmt(b.get(), sc, p);
  }
  loopStack_.pop_back();
  curEntry_ = save;
}

// Expand whole-array and cross-section PUT/GET items into element subscript
// calls in row-major order (rules (104)-(110)). Only static plain-storage
// shapes expand (cross-sections need static star axes); the statement itself
// is retained, so SKIP/PAGE positioning and EDIT format pairing apply to the
// expanded list unchanged. New nodes are typed here. Returns false after a
// diagnostic, in which case the caller skips further item checks.
bool Sema::expandAggregateItems(Stmt* s, Scope* sc, Proc* p, bool isGet) {
  const char* rule = s->edit ? "(108)" : "(110)";
  const char* what = isGet ? "GET" : "PUT";
  std::vector<ExprP> out;
  for (auto& item : s->items) {
    Expr* it = item.get();
    bool whole = it->kind == Expr::VarRef && it->ty.isArray();
    bool cross = it->kind == Expr::Subscript && it->ty.isArray();
    if (!whole && !cross) {
      out.push_back(std::move(item));
      continue;
    }
    Symbol* sym = it->sym;
    if (!sym || sym->kind != Symbol::Var || sym->definedBase || sym->basedBase) {
      d_.error(it->loc,
               std::string(what) + " of a parameter, DEFINED overlay, or BASED array is not "
                                   "implemented in this stage",
               rule);
      return false;
    }
    // The full array type behind the item (through any member path).
    Type full;
    if (it->memberPath.empty()) {
      full = sym->ty;
    } else {
      const Type* cur = &sym->ty;
      for (unsigned f : it->memberPath) {
        if (!cur->isStruct() || f >= cur->members.size()) {
          d_.error(it->loc, std::string(what) + " of this item is not implemented in this stage",
                   rule);
          return false;
        }
        cur = &cur->members[f]->ty;
      }
      full = *cur;
    }
    // Axes to enumerate: every axis for a whole array, the star axes of a
    // cross-section. Enumerated axes must be static; anything else (dynamic
    // extents, dynamic members) stays diagnosed.
    std::vector<size_t> axes;
    if (whole) {
      for (size_t k = 0; k < full.dims.size(); ++k)
        axes.push_back(k);
    } else {
      for (size_t k = 0; k < it->args.size(); ++k)
        if (it->args[k]->kind == Expr::Star)
          axes.push_back(k);
      if (axes.empty()) {
        d_.error(it->loc, "a cross-section needs a '*' axis in this stage", "(126)");
        return false;
      }
    }
    for (size_t k : axes) {
      if (full.dims[k].dyn || full.dims[k].lbDyn) {
        d_.error(it->loc,
                 std::string(what) + " of a dynamic-extent array is not implemented in this stage",
                 rule);
        return false;
      }
    }
    if (isGet)
      checkValueTarget(it); // VALUE arrays cannot receive values (ADR-108)
    // Row-major odometer over the enumerated extents; the fixed axes of a
    // cross-section reuse a clone of the original index per element.
    std::vector<long long> ext;
    for (size_t k : axes)
      ext.push_back((long long)full.dims[k].ub - full.dims[k].lb + 1);
    std::vector<long long> coord(axes.size(), 0);
    bool done = axes.empty();
    while (!done) {
      auto el = std::make_unique<Expr>();
      el->kind = Expr::Call;
      el->name = it->name;
      el->path = it->path;
      el->loc = it->loc;
      for (size_t k = 0; k < full.dims.size(); ++k) {
        auto pos = std::find(axes.begin(), axes.end(), k);
        if (pos != axes.end()) {
          auto idx = std::make_unique<Expr>();
          idx->kind = Expr::IntLit;
          idx->ival = full.dims[k].lb + coord[pos - axes.begin()];
          idx->loc = it->loc;
          el->args.push_back(std::move(idx));
        } else if (cross) {
          el->args.push_back(cloneExpr(it->args[k].get()));
        }
      }
      typeExpr(el.get(), sc, p);
      out.push_back(std::move(el));
      done = true;
      for (size_t j = axes.size(); j-- > 0;) {
        if (++coord[j] < ext[j]) {
          done = false;
          break;
        }
        coord[j] = 0;
      }
    }
  }
  s->items = std::move(out);
  return true;
}

void Sema::checkStmt(Stmt* s, Scope* sc, Proc* p) {
  if (!s)
    return;
  switch (s->kind) {
  case Stmt::Null:
    break;
  case Stmt::Declare:
    break; // handled in collectDecls
  case Stmt::DefineAlias:
    break; // handled in collectDecls
  case Stmt::Assign: {
    typeExpr(s->value.get(), sc, p);
    typeExpr(s->target.get(), sc, p);
    // BY NAME assignment (rule 86): S = T BY NAME copies members of S from
    // the same-named members of T. Both sides must be whole structures; members
    // present in only one side are skipped, so the layouts need not match.
    if (s->byName) {
      if (!s->extraTargets.empty()) {
        d_.error(s->loc, "assignment BY NAME requires a single structure target", "(86)");
        break;
      }
      const Type* dst = structLeafType(s->target.get());
      const Type* src = structLeafType(s->value.get());
      if (!dst || !src) {
        if (!dst)
          d_.error(s->target->loc, "assignment BY NAME target must be a structure reference",
                   "(86)");
        if (!src)
          d_.error(s->value->loc,
                   "assignment BY NAME right-hand side must be a structure reference", "(86)");
        break;
      }
      checkByNameMatch(*dst, *src, s->loc);
      break;
    }
    // Multiple assignment (rule 86): a, b, c = e — the shared value is
    // converted once (to the first target's type) and stored to every target,
    // so all targets must be of that same type. SUBSTR and whole-structure
    // targets are not served in a multiple-assignment list.
    if (!s->extraTargets.empty()) {
      auto checkOne = [&](Expr* t) {
        typeExpr(t, sc, p);
        if (t->kind != Expr::VarRef && t->kind != Expr::Subscript) {
          d_.error(
              t->loc,
              "multiple-assignment target must be a scalar variable or array element in this stage",
              "(86)");
          return false;
        }
        if (t->kind == Expr::VarRef && t->sym && t->sym->kind == Symbol::ProcName) {
          d_.error(t->loc, "cannot assign to procedure '" + t->name + "'", "(86)");
          return false;
        }
        checkValueTarget(t); // VALUE constants cannot receive values (ADR-108)
        if (!s->value->ty.isVoid() && !t->ty.isVoid())
          checkAssignable(t->ty, s->value->ty, t->loc, "assignment");
        return true;
      };
      bool ok = checkOne(s->target.get());
      for (auto& t : s->extraTargets)
        ok = checkOne(t.get()) && ok;
      // Whole arrays in a multiple-assignment list stay diagnosed (rule 86):
      // neither an array target nor an array-valued source has an element-wise
      // form here.
      if (s->value->ty.isArray() || valueHasWholeArrayRef(s->value.get(), false)) {
        ok = false;
        d_.error(s->value->loc,
                 "multiple assignment of an array value is not implemented in this stage", "(86)");
      }
      if (s->target->ty.isArray()) {
        ok = false;
        d_.error(s->target->loc,
                 "multiple assignment to a whole array is not implemented in this stage", "(86)");
      }
      for (auto& t : s->extraTargets)
        if (t->ty.isArray()) {
          ok = false;
          d_.error(t->loc, "multiple assignment to a whole array is not implemented in this stage",
                   "(86)");
        }
      if (ok && !s->target->ty.isVoid()) {
        for (auto& t : s->extraTargets)
          if (!(t->ty == s->target->ty))
            d_.error(t->loc, "multiple-assignment targets must all have the same type", "(86)");
      }
      break;
    }
    // SUBSTR pseudo-variable (M2): substr(v, i, n) on the left of '=' — v
    // must be a modifiable character variable that the assignment overwrites.
    if (s->target->kind == Expr::Call && s->target->name == "SUBSTR") {
      Expr* t = s->target.get();
      if (t->args[0]->kind != Expr::VarRef || !t->args[0]->sym ||
          t->args[0]->sym->kind == Symbol::ProcName) {
        d_.error(t->args[0]->loc,
                 "SUBSTR assignment target must be a modifiable character variable", "(86)");
        break;
      }
      checkValueTarget(t->args[0].get()); // VALUE constants cannot receive values (ADR-108)
      if (!s->value->ty.isVoid())
        checkAssignable(t->ty, s->value->ty, s->loc, "assignment");
      break;
    }
    // Array element assignment: A(i) = e — a modifiable subscripted target.
    if (s->target->kind == Expr::Subscript) {
      if (!s->value->ty.isVoid() && !s->target->ty.isVoid())
        checkAssignable(s->target->ty, s->value->ty, s->loc, "assignment");
      break;
    }
    if (s->target->kind != Expr::VarRef) {
      d_.error(s->target->loc, "assignment target must be a variable reference in this stage",
               "(86)");
      break;
    }
    if (s->target->sym && s->target->sym->kind == Symbol::ProcName) {
      d_.error(s->target->loc, "cannot assign to procedure '" + s->target->name + "'", "(86)");
      break;
    }
    checkValueTarget(s->target.get()); // VALUE constants cannot receive values (ADR-108)
    // Reductions over array expressions (rule (123); QR2.1): expand pending
    // SUM/PROD/ANY/ALL calls in the value into static temps first, so the
    // whole-array path below sees bare reductions.
    if (s->target->kind == Expr::VarRef && expandReductionTemps(s, sc, p))
      break;
    // Cross-section assignment (rule 126): B = A(i, *) — the right-hand side is
    // a reduced-dim array value produced by a '*' subscript. Only a subscript
    // carries Star axes; a call's '*' arguments omit OPTIONALs (extension).
    {
      bool isCross = false;
      if (s->value->kind == Expr::Subscript)
        for (auto& a : s->value->args)
          if (a->kind == Expr::Star) {
            isCross = true;
            break;
          }
      if (isCross) {
        if (s->target->ty.isArray() && s->target->ty == s->value->ty)
          break;
        d_.error(s->loc,
                 "cross-section assignment requires the target to be an array of the "
                 "cross-section's shape",
                 "(126)");
        break;
      }
    }
    // Whole-array assignment (rules 86, 127; QR2.1): a whole-array target or
    // an array-valued source expands to explicit DO loops. Identical static
    // plain copies and scalar broadcasts ride the path below; anything
    // unsupported is diagnosed inside. Root cross-sections were claimed by
    // the block above, so they never reach the expander.
    if (s->target->kind == Expr::VarRef &&
        (s->target->ty.isArray() || valueHasWholeArrayRef(s->value.get(), false))) {
      if (expandWholeArrayAssign(s, sc, p))
        checkDoIter(s, sc, p);
      break;
    }
    if (!s->value->ty.isVoid() && !s->target->ty.isVoid())
      checkAssignable(s->target->ty, s->value->ty, s->loc, "assignment");
    break;
  }
  case Stmt::If: {
    typeExpr(s->cond.get(), sc, p);
    if (s->cond && !s->cond->ty.isBit() && !s->cond->ty.isNumeric())
      d_.error(s->cond->loc, "IF condition must yield a bit value", "(75)");
    checkStmt(s->thenS.get(), sc, p);
    checkStmt(s->elseS.get(), sc, p);
    break;
  }
  case Stmt::Group: {
    // A segment boundary (rule (56)) may sit inside a group body.
    Stmt* save = curEntry_;
    for (auto& b : s->body) {
      if (b && b->kind == Stmt::Entry)
        curEntry_ = b.get();
      checkStmt(b.get(), sc, p);
    }
    curEntry_ = save;
    break;
  }
  case Stmt::Begin: {
    // Descend with the block's own scope (rule (68)); see collectDecls.
    auto it = beginScopes_.find(s);
    Scope* bsc = it != beginScopes_.end() ? it->second : sc;
    Stmt* save = curEntry_;
    for (auto& b : s->body) {
      if (b && b->kind == Stmt::Entry)
        curEntry_ = b.get();
      checkStmt(b.get(), bsc, p);
    }
    curEntry_ = save;
    break;
  }
  case Stmt::DoWhile: {
    typeExpr(s->cond.get(), sc, p);
    Stmt* save = curEntry_;
    loopStack_.push_back(s->labels);
    for (auto& b : s->body) {
      if (b && b->kind == Stmt::Entry)
        curEntry_ = b.get();
      checkStmt(b.get(), sc, p);
    }
    loopStack_.pop_back();
    curEntry_ = save;
    break;
  }
  case Stmt::DoIter: {
    checkDoIter(s, sc, p);
    break;
  }
  case Stmt::Put: {
    typeExpr(s->skipCount.get(), sc, p);
    checkStringTarget(s, sc, p, false);
    checkFileTarget(s, sc);
    if (s->edit) {
      checkEditFormats(s, sc, p, false);
      break;
    }
    if (s->data) {
      // Data-directed output (rule (106)): each item prints as NAME=value,
      // so it must be a plain scalar variable reference. Subscripted,
      // qualified, structure, pointer, and complex items stay diagnosed.
      for (auto& it : s->items) {
        typeExpr(it.get(), sc, p);
        if (it->kind != Expr::VarRef || !it->sym || it->sym->kind == Symbol::ProcName ||
            !it->memberPath.empty()) {
          d_.error(it->loc, "a PUT DATA item must be a scalar variable", "(110)");
          continue;
        }
        if (it->ty.isVoid() || it->ty.isStruct() || it->ty.isPointer() || it->ty.isComplex() ||
            it->ty.isArray()) {
          d_.error(it->loc, "PUT DATA of this type is not implemented in this stage", "(110)");
        }
      }
      break;
    }
    for (auto& it : s->items)
      typeExpr(it.get(), sc, p);
    // Aggregate items transmit element-wise (rules (104),(105)).
    if (!expandAggregateItems(s, sc, p, false))
      break;
    for (auto& it : s->items) {
      if (it->ty.isVoid())
        d_.error(it->loc, "invalid data list item", "(110)");
      else if (it->ty.isBit() && it->ty.len > 1)
        d_.error(it->loc, "PUT LIST of a BIT(n>1) value is not implemented in this stage", "(110)");
    }
    break;
  }
  case Stmt::Get: {
    // List-directed input writes a value into each data-list item, so every
    // item must be an assignable scalar reference, not a constant or procedure
    // (rules (109),(110)).
    typeExpr(s->skipCount.get(), sc, p);
    checkStringTarget(s, sc, p, true);
    checkFileTarget(s, sc);
    if (s->edit) {
      checkEditFormats(s, sc, p, true);
      break;
    }
    if (s->data) {
      // Data-directed input (rule (106)): each NAME=value pair stores into
      // the named variable, so items must be plain scalar variable
      // references, mirroring PUT DATA (subscripted, qualified, structure,
      // pointer, complex, and array items stay diagnosed).
      for (auto& it : s->items) {
        typeExpr(it.get(), sc, p);
        if (it->kind != Expr::VarRef || !it->sym || it->sym->kind == Symbol::ProcName ||
            !it->memberPath.empty()) {
          d_.error(it->loc, "a GET DATA item must be a scalar variable", "(110)");
          continue;
        }
        if (it->ty.isVoid() || it->ty.isStruct() || it->ty.isPointer() || it->ty.isComplex() ||
            it->ty.isArray()) {
          d_.error(it->loc, "GET DATA of this type is not implemented in this stage", "(110)");
        }
      }
      break;
    }
    for (auto& it : s->items)
      typeExpr(it.get(), sc, p);
    // Aggregate items read element-wise (rules (109),(110)).
    if (!expandAggregateItems(s, sc, p, true))
      break;
    for (auto& it : s->items) {
      bool ref = (it->kind == Expr::VarRef && it->sym && it->sym->kind != Symbol::ProcName) ||
                 it->kind == Expr::Subscript;
      if (!ref) {
        d_.error(it->loc, "GET LIST item must be a variable to receive the value", "(110)");
        continue;
      }
      checkValueTarget(it.get()); // VALUE constants cannot receive values (ADR-108)
      if (it->ty.isVoid()) {
        d_.error(it->loc, "invalid data list item", "(110)");
        continue;
      }
      if (it->ty.isBit() && it->ty.len > 1) {
        d_.error(it->loc, "GET LIST of a BIT(n>1) value is not implemented in this stage", "(110)");
        continue;
      }
      if (it->ty.isStruct()) {
        d_.error(it->loc, "a whole structure cannot be read with GET LIST in this stage", "(110)");
        continue;
      }
      // A CHARACTER member or array element is read through storeArrayElement
      // /member paths, which are not served (mirrors assignment, rule (11)/(12)).
      if (it->ty.isChar() &&
          ((it->kind == Expr::VarRef && !it->memberPath.empty()) || it->kind == Expr::Subscript)) {
        d_.error(it->loc,
                 "GET LIST of a CHARACTER member or array element is not implemented in this "
                 "stage",
                 "(110)");
        continue;
      }
    }
    break;
  }
  case Stmt::CallS: {
    Symbol* sym = lookup(sc, s->name);
    if (!sym || sym->kind != Symbol::ProcName) {
      d_.error(s->loc, "'" + s->name + "' is not a known internal procedure", "(78)");
      break;
    }
    s->sym = sym;
    Proc* callee = sym->proc;
    Stmt* en = sym->entry; // rule (56): an ENTRY name uses the entry's params
    std::vector<Symbol*> calleeParams =
        en ? en->entryParamSyms : (callee ? callee->paramSyms : std::vector<Symbol*>());
    for (auto& a : s->args)
      typeExpr(a.get(), sc, p);
    // External entries carry their descriptor in entryParams (rule (38)).
    // Arity counts parameter NAMES: resolved symbols may still lag for a
    // nested callee (ADR-094), when per-argument checks are skipped as before.
    const size_t expect = en       ? en->params.size()
                          : callee ? callee->params.size()
                                   : sym->entryParams.size();
    if (callee || en) {
      // OPTIONAL parameters (extension, ADR-119): a '*' argument omits an
      // OPTIONAL parameter in any position, and trailing OPTIONALs may be
      // left out entirely. Anything else keeps the exact-count rule (78).
      if (s->args.size() > expect) {
        d_.error(s->loc,
                 "'" + s->name + "' expects " + std::to_string(expect) + " argument(s), " +
                     std::to_string(s->args.size()) + " given",
                 "(78)");
      } else {
        size_t i = 0;
        for (; i < s->args.size(); ++i) {
          if (s->args[i]->kind == Expr::Star) {
            if (i >= calleeParams.size() || !calleeParams[i]->isOptional)
              d_.error(s->args[i]->loc, "'*' omits a parameter that is not OPTIONAL (ADR-119)", "");
          } else if (i < calleeParams.size())
            checkAssignable(calleeParams[i]->ty, s->args[i]->ty, s->args[i]->loc, "argument");
        }
        for (; i < expect; ++i)
          if (i >= calleeParams.size() || !calleeParams[i]->isOptional) {
            d_.error(s->loc,
                     "'" + s->name + "' expects " + std::to_string(expect) + " argument(s), " +
                         std::to_string(s->args.size()) + " given",
                     "(78)");
            break;
          }
      }
    } else if (s->args.size() != expect) {
      d_.error(s->loc,
               "'" + s->name + "' expects " + std::to_string(expect) + " argument(s), " +
                   std::to_string(s->args.size()) + " given",
               "(78)");
    } else {
      // External entries carry Type descriptors with no OPTIONAL info, so a
      // '*' argument cannot be validated and stays diagnosed (rule (38)).
      for (auto& a : s->args)
        if (a->kind == Expr::Star)
          d_.error(a->loc, "'*' arguments to an external entry are not implemented (ADR-119)", "");
      checkByValueArgs(sym, s->args);
    }
    // Task options (rule (79), QR2.8): any option makes the CALL asynchronous.
    // TASK takes a TASK variable (or nothing), EVENT an EVENT variable,
    // PRIORITY a numeric expression (evaluated, best-effort ignored).
    if (s->hasTaskOpt || s->eventRef || s->priorityExpr) {
      if (!callee && !en)
        d_.error(s->loc, "CALL with TASK/EVENT/PRIORITY on an external entry is not implemented",
                 "(79)");
      if ((callee && callee->isFunction) || (en && en->entryIsFunction))
        d_.error(s->loc, "CALL with TASK/EVENT/PRIORITY on a function procedure is not implemented",
                 "(79)");
      if (s->taskRef) {
        typeExpr(s->taskRef.get(), sc, p);
        Expr* t = s->taskRef.get();
        if (t->kind != Expr::VarRef || !t->sym || t->sym->kind == Symbol::ProcName ||
            !t->sym->ty.isTask())
          d_.error(t->loc, "TASK option requires a TASK variable", "(79)");
        else if (!t->path.empty() || !t->memberPath.empty())
          d_.error(t->loc, "TASK of a structure member is not implemented in this stage", "(79)");
        else if (t->sym->ty.isArray())
          d_.error(t->loc, "TASK of an array element is not implemented in this stage", "(79)");
      }
      if (s->eventRef) {
        typeExpr(s->eventRef.get(), sc, p);
        Expr* e = s->eventRef.get();
        if (e->kind != Expr::VarRef || !e->sym || e->sym->kind == Symbol::ProcName ||
            !e->sym->ty.isEvent())
          d_.error(e->loc, "EVENT option requires an EVENT variable", "(79)");
        else if (!e->path.empty() || !e->memberPath.empty())
          d_.error(e->loc, "EVENT of a structure member is not implemented in this stage", "(79)");
        else if (e->sym->ty.isArray())
          d_.error(e->loc, "EVENT of an array element is not implemented in this stage", "(79)");
      }
      if (s->priorityExpr) {
        typeExpr(s->priorityExpr.get(), sc, p);
        if (!s->priorityExpr->ty.isNumeric() && !s->priorityExpr->ty.isVoid())
          d_.error(s->priorityExpr->loc, "PRIORITY expression must be numeric", "(79)");
      }
    }
    break;
  }
  case Stmt::Wait: {
    // WAIT (rule (82), QR2.8): every event must be an EVENT variable; the
    // optional count is a numeric expression (how many must complete).
    if (s->waitEvents.empty())
      d_.error(s->loc, "WAIT requires at least one event", "(82)");
    for (auto& e : s->waitEvents) {
      typeExpr(e.get(), sc, p);
      if (e->kind != Expr::VarRef || !e->sym || e->sym->kind == Symbol::ProcName ||
          !e->sym->ty.isEvent()) {
        d_.error(e->loc, "WAIT requires an EVENT variable", "(82)");
        continue;
      }
      if (!e->path.empty() || !e->memberPath.empty())
        d_.error(e->loc, "WAIT of a structure member is not implemented in this stage", "(82)");
      else if (e->sym->ty.isArray())
        d_.error(e->loc, "WAIT of an array element is not implemented in this stage", "(82)");
    }
    if (s->waitCount) {
      typeExpr(s->waitCount.get(), sc, p);
      if (!s->waitCount->ty.isNumeric() && !s->waitCount->ty.isVoid())
        d_.error(s->waitCount->loc, "WAIT count must be numeric", "(82)");
    }
    break;
  }
  case Stmt::Delay: {
    // DELAY (rule (83), QR2.8): suspend for N milliseconds; N is numeric.
    if (!s->value)
      d_.error(s->loc, "DELAY requires an expression", "(83)");
    else {
      typeExpr(s->value.get(), sc, p);
      if (!s->value->ty.isNumeric() && !s->value->ty.isVoid())
        d_.error(s->value->loc, "DELAY expression must be numeric", "(83)");
    }
    break;
  }
  case Stmt::Return: {
    // rule (81): RETURN(value) supplies a function procedure's result; a plain
    // RETURN ends a procedure. In a multi-entry procedure a segment belonging to
    // a function ENTRY (rule 56) may also RETURN(value), even if the primary
    // entry itself returns nothing. Every RETURN must also agree with the
    // shared implementation's single result type: a plain RETURN is only valid
    // when that type is void, and RETURN(value) only when it is not (rule 56).
    const bool inFuncEntry = curEntry_ && curEntry_->entryIsFunction;
    const bool isFunc = p->isFunction || inFuncEntry;
    const Type& rty = inFuncEntry ? curEntry_->entryRetTy : p->retTy;
    if (s->value) {
      if (!isFunc) {
        d_.error(s->loc, "RETURN with a value is only valid in a function procedure", "(81)");
        break;
      }
      if (p->commonRetTy.isVoid()) {
        d_.error(s->loc, "RETURN with a value is only valid in a function procedure", "(81)");
        break;
      }
      typeExpr(s->value.get(), sc, p);
      if (!s->value->ty.isVoid())
        checkAssignable(rty, s->value->ty, s->loc, "RETURN value");
    } else {
      // A bare RETURN in an ON-unit ends the unit and resumes after the
      // SIGNAL (rule 91): it answers neither the value nor the common-type
      // checks below. Elsewhere a function must RETURN a value (81).
      if (inUnit_)
        break;
      if (isFunc) {
        d_.error(s->loc, "a function procedure must RETURN a value", "(81)");
        break;
      }
      if (!p->commonRetTy.isVoid()) {
        d_.error(s->loc,
                 "a plain RETURN is not valid in a procedure whose ENTRYs return a value; "
                 "RETURN a value of the common result type",
                 "(56)");
      }
    }
    break;
  }
  case Stmt::Stop:
  case Stmt::Entry: // declaration-like; params/type resolved in processProc
    break;
  case Stmt::Leave:
  case Stmt::Iterate: {
    // Extension (ADR-105): LEAVE exits / ITERATE continues the innermost
    // enclosing iterative DO-group, or the named one. Plain groups and
    // BEGIN blocks are transparent (only iterative frames are tracked).
    const char* what = s->kind == Stmt::Leave ? "LEAVE" : "ITERATE";
    if (s->name.empty()) {
      if (loopStack_.empty())
        d_.error(s->loc, std::string(what) + " outside an iterative DO-group (ADR-105)", "");
    } else {
      bool found = false;
      for (auto it = loopStack_.rbegin(); it != loopStack_.rend(); ++it)
        if (std::find(it->begin(), it->end(), s->name) != it->end()) {
          found = true;
          break;
        }
      if (!found)
        d_.error(s->loc, "no enclosing iterative DO-group named '" + s->name + "' (ADR-105)", "");
    }
    break;
  }
  case Stmt::Display: {
    // Rule (114): DISPLAY takes one scalar value.
    typeExpr(s->value.get(), sc, p);
    if (s->value && (s->value->ty.isArray() || s->value->ty.isStruct()))
      d_.error(s->loc, "DISPLAY takes a scalar value", "(114)");
    else if (s->value && s->value->ty.isBit() && s->value->ty.len > 1)
      d_.error(s->loc, "DISPLAY of a BIT(n>1) value is not implemented in this stage", "(114)");
    break;
  }
  case Stmt::Read: {
    // Rules (112),(113): READ FILE ( f ) INTO ( reference ) — one fixed-size
    // binary record into a plain scalar variable (sequential slice).
    Symbol* rsym = lookup(sc, s->fileIdent);
    if (!rsym || rsym->kind == Symbol::ProcName || !rsym->fileAttr) {
      d_.error(s->loc, "'" + s->fileIdent + "' is not a FILE variable", "(112)");
      break;
    }
    s->fileSym = rsym;
    typeExpr(s->target.get(), sc, p);
    Expr* t = s->target.get();
    if (!t || t->kind != Expr::VarRef || !t->sym || t->sym->kind == Symbol::ProcName ||
        !t->memberPath.empty()) {
      d_.error(s->loc, "READ INTO requires a variable to receive the record", "(112)");
      break;
    }
    checkValueTarget(t); // VALUE constants cannot receive values (ADR-108)
    if (t->ty.isVoid() || t->ty.isArray() || t->ty.isStruct() || t->ty.isPointer() ||
        t->ty.isComplex() || (t->ty.isChar() && t->ty.varying))
      d_.error(s->loc, "READ INTO of this type is not implemented in this stage", "(112)");
    break;
  }
  case Stmt::Write: {
    // Rules (112),(113): WRITE FILE ( f ) FROM ( expression ) — one fixed-size
    // binary record from a scalar value (sequential slice).
    Symbol* rsym = lookup(sc, s->fileIdent);
    if (!rsym || rsym->kind == Symbol::ProcName || !rsym->fileAttr) {
      d_.error(s->loc, "'" + s->fileIdent + "' is not a FILE variable", "(112)");
      break;
    }
    s->fileSym = rsym;
    typeExpr(s->value.get(), sc, p);
    Expr* v = s->value.get();
    if (!v || v->ty.isVoid() || v->ty.isArray() || v->ty.isStruct() || v->ty.isPointer() ||
        v->ty.isComplex() || (v->ty.isChar() && v->ty.varying))
      d_.error(s->loc, "WRITE FROM of this type is not implemented in this stage", "(112)");
    break;
  }
  case Stmt::Allocate:
    // ALLOCATE (rules 87,88): heap-allocate each based structure and store its
    // address in the SET pointer target. The based variable must be fixed-size
    // (a runtime-extent based array is QR2.3), and the SET target a POINTER.
    for (size_t i = 0; i < s->allocBase.size(); ++i) {
      typeExpr(s->allocBase[i].get(), sc, p);
      Symbol* bsym = s->allocBase[i]->sym;
      if (!bsym || (!bsym->basedBase && !bsym->controlled)) {
        d_.error(s->allocBase[i]->loc,
                 "'" + s->allocBase[i]->name +
                     "' is not a BASED variable; ALLOCATE requires based storage",
                 "(88)");
        continue;
      }
      if (bsym->controlled) {
        // CONTROLLED allocation (rules 87-90, ADR-140): bare ALLOCATE pushes
        // a generation; a SET option is stack-managed anyway, so it warns
        // and is ignored rather than failing the call.
        if (i < s->allocSet.size() && s->allocSet[i])
          d_.warn(s->allocSet[i]->loc,
                  "SET on CONTROLLED ALLOCATE is ignored; generations are stack-managed", "(88)");
        continue;
      }
      if (bsym->ty.isArray() && bsym->ty.isDynamic())
        d_.error(s->allocBase[i]->loc,
                 "ALLOCATE of a dynamic-extent based array is not implemented in this stage",
                 "(89)");
      // BASED without SET was a parse error before bare ALLOCATE (rule 87)
      // opened the paren-less form for CONTROLLED; keep the cite attached
      // to the storage kind here.
      if (i >= s->allocSet.size() || !s->allocSet[i])
        d_.error(s->allocBase[i]->loc,
                 "ALLOCATE of BASED storage requires the SET ( reference ) option", "(88)");
      else {
        typeExpr(s->allocSet[i].get(), sc, p);
        if (s->allocSet[i]->kind != Expr::VarRef || !s->allocSet[i]->ty.isPointer())
          d_.error(s->allocSet[i]->loc, "the SET target of ALLOCATE must be a POINTER variable",
                   "(88)");
      }
    }
    break;
  case Stmt::Free:
    // FREE (rule 90): free the storage of each based variable, addressed either
    // by an explicit locator pointer or by the variable's own BASED pointer.
    for (auto& f : s->freeBase) {
      typeExpr(f.get(), sc, p);
      Symbol* bsym = f->sym;
      if (bsym && bsym->controlled) {
        // CONTROLLED FREE (rule 90, ADR-140): the plain form pops a
        // generation; a locator has no stack meaning and stays diagnosed.
        if (f->locPtr)
          d_.error(f->loc, "a locator FREE of CONTROLLED storage is not implemented", "(90)");
        continue;
      }
      if (!bsym || !bsym->basedBase)
        d_.error(f->loc, "'" + f->name + "' is not a BASED variable; FREE requires based storage",
                 "(90)");
      if (f->locPtr && !f->locPtr->ty.isPointer())
        d_.error(f->locPtr->loc, "the locator of '->' in FREE must be a POINTER", "(90)");
    }
    break;
  case Stmt::Open:
  case Stmt::Close:
    // OPEN/CLOSE FILE ( f ) (rules 100-103): f must be a declared FILE variable.
    checkFileTarget(s, sc);
    break;
  case Stmt::Goto:
    // GO TO target must be a label defined in this procedure (rules (64),(77)).
    if (procLabels_.find(s->name) == procLabels_.end())
      d_.error(s->loc, "'" + s->name + "' is not a label in this procedure", "(77)");
    break;
  case Stmt::On:
    // Rules (91),(94),(99): ERROR, SIZE, or a programmer-named condition;
    // the unit body only needs typing plus the establishing-frame checks.
    s->condKey = resolveCondKey(s, sc);
    if (!s->isSystem && s->unit) {
      bool savedUnit = inUnit_;
      inUnit_ = true;
      checkStmt(s->unit.get(), sc, p);
      checkOnUnit(s->unit.get(), p);
      inUnit_ = savedUnit;
    }
    break;
  case Stmt::Revert:
  case Stmt::Signal:
    // A REVERT with no established handler is a no-op reverting to SYSTEM.
    s->condKey = resolveCondKey(s, sc);
    // SIGNAL ... SET ONCODE(expr) (rule (93)): the code is numeric.
    if (s->oncodeExpr) {
      typeExpr(s->oncodeExpr.get(), sc, p);
      if (!s->oncodeExpr->ty.isVoid() && !s->oncodeExpr->ty.isNumeric())
        d_.error(s->oncodeExpr->loc, "SET ONCODE requires a numeric code expression", "(93)");
    }
    break;
  }
}

// The STRING ( reference ) stream option (rule 105): the target must be a
// NONVARYING CHARACTER variable, and PAGE/SKIP are stream-only, meaningless
// against a string sink/source.
// Extension (ADR-114): register a DEFINE ALIAS type in scope.
void Sema::defineAlias(Scope* sc, const std::string& name, const Type& ty, SourceLoc loc) {
  if (sc->aliases.count(name))
    d_.error(loc, "'" + name + "' is already declared in this block (ADR-114)", "");
  else
    sc->aliases[name] = ty;
}

// Extension (ADR-108): a VALUE named constant keeps its storage but no
// statement may receive a value into it.
void Sema::checkValueTarget(Expr* t) {
  if (t && t->kind == Expr::VarRef && t->sym && t->sym->isValue)
    d_.error(t->loc, "cannot assign to the VALUE constant '" + t->sym->name + "' (ADR-108)", "");
}

void Sema::checkStringTarget(Stmt* s, Scope* sc, Proc* p, bool isGet) {
  if (!s->stringTarget)
    return;
  typeExpr(s->stringTarget.get(), sc, p);
  Expr* t = s->stringTarget.get();
  if (t->kind != Expr::VarRef || !t->sym || t->sym->kind == Symbol::ProcName)
    d_.error(t->loc, "the STRING option requires a character variable", "(105)");
  else if (!t->ty.isChar() || t->ty.varying)
    d_.error(t->loc, "the STRING option requires a NONVARYING CHARACTER variable", "(105)");
  else if (!isGet)
    checkValueTarget(t); // PUT STRING formats into its target (ADR-108)
  if (s->page || s->skip)
    d_.error(s->loc, "PAGE/SKIP cannot be combined with the STRING option", "(105)");
}

// The FILE ( f ) stream option (rule 105) and OPEN/CLOSE FILE ( f ) (rules
// 100-103): f must be a declared FILE variable. FILE and STRING are mutually
// exclusive stream targets. Resolves s->fileIdent to s->fileSym.
void Sema::checkFileTarget(Stmt* s, Scope* sc) {
  if (s->fileIdent.empty())
    return;
  Symbol* sym = lookup(sc, s->fileIdent);
  if (!sym || sym->kind == Symbol::ProcName || !sym->fileAttr) {
    d_.error(s->loc, "'" + s->fileIdent + "' is not a FILE variable", "(105)");
    return;
  }
  s->fileSym = sym;
  if (s->stringTarget)
    d_.error(s->loc, "the FILE and STRING options cannot be combined", "(105)");
}

// Edit-directed transmission (rule (108)): type the format widths/decimals and
// the data items, then pair each data item with its data (A/F) format, skipping
// the control formats (X/SKIP/PAGE/LINE) that act without consuming data. For
// GET the paired item must be an assignable reference of a format-compatible
// type.
void Sema::checkEditFormats(Stmt* s, Scope* sc, Proc* p, bool isGet) {
  for (auto& it : s->items)
    typeExpr(it.get(), sc, p);
  for (auto& f : s->formats) {
    typeExpr(f.w.get(), sc, p);
    typeExpr(f.d.get(), sc, p);
  }
  // Aggregate items transmit element-wise; expansion runs before format
  // pairing so EDIT counts line up.
  if (!expandAggregateItems(s, sc, p, isGet))
    return;
  size_t dataIdx = 0;
  for (auto& f : s->formats) {
    if (isGet && f.kind == FormatItem::Column) {
      d_.error(s->loc, "COLUMN positioning on input is not implemented in this stage", "(108)");
      continue;
    }
    if (f.kind == FormatItem::X || f.kind == FormatItem::Skip || f.kind == FormatItem::Page ||
        f.kind == FormatItem::Line || f.kind == FormatItem::Column)
      continue; // a control format consumes no data item
    if (dataIdx >= s->items.size()) {
      d_.error(s->loc, "more data formats than data items in EDIT", "(108)");
      break;
    }
    Expr* it = s->items[dataIdx].get();
    if (f.kind == FormatItem::A && !it->ty.isChar())
      d_.error(it->loc, "an A format requires a CHARACTER item", "(52)");
    else if ((f.kind == FormatItem::F || f.kind == FormatItem::E) && !it->ty.isNumeric()) {
      const char* letter = f.kind == FormatItem::F ? "F" : "E";
      const char* rule = f.kind == FormatItem::F ? "(50)" : "(53)";
      d_.error(it->loc, "an " + std::string(letter) + " format requires a numeric item", rule);
    }
    if (isGet) {
      bool ref = (it->kind == Expr::VarRef && it->sym && it->sym->kind != Symbol::ProcName) ||
                 it->kind == Expr::Subscript;
      if (!ref)
        d_.error(it->loc, "GET EDIT item must be a variable to receive the value", "(110)");
      else if (it->ty.isStruct())
        d_.error(it->loc, "a whole structure cannot be read with GET EDIT in this stage", "(110)");
      else
        checkValueTarget(it); // VALUE constants cannot receive values (ADR-108)
    } else if (it->ty.isVoid()) {
      d_.error(it->loc, "invalid data list item", "(110)");
    }
    ++dataIdx;
  }
  size_t dataFormats = 0;
  for (auto& f : s->formats)
    if (f.kind == FormatItem::A || f.kind == FormatItem::F || f.kind == FormatItem::E)
      ++dataFormats;
  if (dataFormats < s->items.size())
    d_.error(s->loc, "more data items than data formats in EDIT", "(108)");
}

// A constant subscript is range-checked at compile time (SUBSCRIPTRANGE, rule
// (126)); a runtime index is left to the generated bounds check in IRGen.
// Each constant subscript is checked against its own axis (rules (12),(13)).
void Sema::checkSubscriptBounds(Expr* e, Symbol* arr) {
  checkSubscriptBoundsDims(e, arr->ty.dims, arr->name);
}

void Sema::checkSubscriptBoundsDims(Expr* e, const std::vector<Dim>& dims,
                                    const std::string& name) {
  const size_t n = std::min(e->args.size(), dims.size());
  for (size_t k = 0; k < n; ++k) {
    Expr* idx = e->args[k].get();
    if (idx->kind != Expr::IntLit)
      continue;
    const Dim& d = dims[k];
    if (d.dyn) // a dynamic axis has no compile-time upper bound to check
      continue;
    long long v = idx->ival;
    if (v < d.lb || v > d.ub)
      d_.error(idx->loc,
               "subscript " + std::to_string(v) + " is out of bounds " + std::to_string(d.lb) +
                   ":" + std::to_string(d.ub) + " for array '" + name + "'",
               "(126)");
  }
}

const Type* Sema::resolveMemberPath(Expr* e, const Type& base) {
  const Type* cur = &base;
  e->memberPath.clear();
  for (const std::string& mname : e->path) {
    if (!cur->isStruct()) {
      d_.error(e->loc,
               "'" + e->name + "' is not a structure, so it cannot be qualified by '" + mname + "'",
               "(124)");
      return nullptr;
    }
    int m = -1;
    for (size_t k = 0; k < cur->members.size(); ++k)
      if (cur->members[k]->name == mname) {
        m = (int)k;
        break;
      }
    if (m < 0) {
      d_.error(e->loc, "structure '" + e->name + "' has no member '" + mname + "'", "(124)");
      return nullptr;
    }
    e->memberPath.push_back((unsigned)m);
    cur = &cur->members[m]->ty;
  }
  return cur;
}

void Sema::typeExpr(Expr* e, Scope* sc, Proc* p) {
  if (!e)
    return;
  switch (e->kind) {
  case Expr::IntLit:
    e->ty = Type::fixedDec(e->ival > 99999 || e->ival < -99999 ? 15 : 5, 0);
    if (e->ty.intBits() == 32 && (e->ival > 2147483647LL || e->ival < -2147483648LL))
      e->ty = Type::fixedBin(63, 0);
    break;
  case Expr::DecLit:
    e->ty = Type::fixedDec(e->decPrec > 0 ? e->decPrec : 1, e->decScale);
    break;
  case Expr::FltLit:
    e->ty = Type::flt(6);
    break;
  case Expr::ComplexLit:
    e->ty = Type::complexTy();
    break;
  case Expr::CharLit:
    e->ty = Type::chr((int)e->sval.size());
    break;
  case Expr::BitLit:
    e->ty = Type::bit((int)std::max<size_t>(1, e->sval.size()));
    break;
  case Expr::Star:
    // A '*' subscript is a cross-section axis marker (rule 126), not a value;
    // its meaning is assigned by the enclosing subscript, so it stays void.
    e->ty = Type::voidTy();
    break;
  case Expr::VarRef: {
    Symbol* sym = lookup(sc, e->name);
    if (!sym) {
      sym = implicitDeclare(sc, e->name, e->loc, false);
      p->localSyms.push_back(sym); // implicit vars are AUTOMATIC storage
    }
    if (!sym->owner)
      sym->owner = p;
    e->sym = sym;
    e->ty = sym->ty;
    // A locator-qualified reference P -> X (rule 124): P is a POINTER, X a
    // based variable whose storage is addressed through P. The member path
    // below then resolves X.A against the based structure type.
    if (e->locPtr) {
      typeExpr(e->locPtr.get(), sc, p);
      if (!e->locPtr->ty.isPointer())
        d_.error(e->locPtr->loc, "the locator of '->' must be a POINTER", "(124)");
      if (!sym->basedBase)
        d_.error(e->loc,
                 "'" + e->name + "' is not a BASED variable; '->' requires a based reference",
                 "(124)");
    }
    // A qualified reference S.A.B (rule 124): resolve each member against
    // the structure type, recording the LLVM field index along the path.
    if (!e->path.empty()) {
      if (const Type* leaf = resolveMemberPath(e, sym->ty); leaf)
        e->ty = *leaf;
      else
        e->ty = Type::voidTy();
    }
    if (sym->kind == Symbol::ProcName) {
      d_.error(e->loc, "'" + e->name + "' is a procedure and cannot be used as a value", "(123)");
      e->ty = Type::voidTy();
    }
    // rule (8)/(42) static link: a reference to a variable of an enclosing
    // procedure is served through this procedure's static link.
    if (sym->kind == Symbol::Var)
      noteStaticUse(p, sym);
    break;
  }
  case Expr::Subscript:
    // Reclassified in the Call case; never re-dispatched here.
    break;
  case Expr::Call: {
    for (auto& a : e->args)
      typeExpr(a.get(), sc, p);
    // A qualified reference being subscripted, S.A(i) (rules 124,126): the
    // callee name is a structure and the path resolves to an array member.
    // Reclassify as a Subscript of the member's element type, carrying the
    // base structure symbol and the resolved field path.
    if (!e->path.empty()) {
      Symbol* bs = lookup(sc, e->name);
      // An array of structures arr(i).x (rules 124,126): the subscripts index
      // the array to select one structure element, then the path resolves a
      // member of that element. Served for a scalar (or nested minor-structure)
      // member; a member array of an element is diagnosed unimplemented.
      if (bs && bs->kind == Symbol::Var && bs->ty.isArray() && bs->ty.elementType().isStruct()) {
        if (e->args.size() != bs->ty.dims.size()) {
          d_.error(e->loc,
                   "array of structures '" + e->name + "' has " +
                       std::to_string(bs->ty.dims.size()) + " dimension(s) and takes " +
                       std::to_string(bs->ty.dims.size()) + " subscript(s), " +
                       std::to_string(e->args.size()) + " given",
                   "(126)");
          e->ty = Type::voidTy();
          break;
        }
        e->kind = Expr::Subscript;
        e->sym = bs;
        const Type* leaf = resolveMemberPath(e, bs->ty.elementType());
        if (!leaf) {
          e->ty = Type::voidTy();
          break;
        }
        if (leaf->isArray())
          d_.error(e->loc,
                   "a member array of an array-of-structures element is not implemented in this "
                   "stage",
                   "(124)");
        e->ty = *leaf;
        // Rule (8): thread outer storage plus any BASED/DEFINED base.
        noteStaticUse(p, bs);
        break;
      }
      if (!bs || bs->kind != Symbol::Var || !bs->ty.isStruct()) {
        d_.error(e->loc,
                 "'" + e->name + "' is not a structure, so it cannot be subscripted by member",
                 "(124)");
        e->ty = Type::voidTy();
        break;
      }
      // Reclassify as a Subscript up front so an assignment target that fails
      // to resolve is still recognised as a (void) subscript, not a call.
      e->kind = Expr::Subscript;
      e->sym = bs;
      const Type* leaf = resolveMemberPath(e, bs->ty);
      if (!leaf || !leaf->isArray()) {
        if (leaf)
          d_.error(e->loc, "structure member is not an array and cannot be subscripted", "(126)");
        e->ty = Type::voidTy();
        break;
      }
      if (e->args.size() != leaf->dims.size()) {
        d_.error(e->loc,
                 "member array has " + std::to_string(leaf->dims.size()) +
                     " dimension(s) and takes " + std::to_string(leaf->dims.size()) +
                     " subscript(s), " + std::to_string(e->args.size()) + " given",
                 "(126)");
        e->ty = Type::voidTy();
        break;
      }
      int nStar = 0;
      Type reduced;
      if (crossSectionType(e, *leaf, reduced, nStar)) {
        // A member-array cross-section S.A(i, *) (rules 124,126): a reduced-dim
        // array value whose rank is the number of '*' axes (rule 126).
        e->ty = reduced;
      } else {
        e->ty = leaf->elementType();
      }
      std::string mname = e->name;
      for (const std::string& q : e->path)
        mname += "." + q;
      checkSubscriptBoundsDims(e, leaf->dims, mname);
      // rule (8)/(42): a structure of an enclosing procedure is reached
      // through this procedure's static link.
      noteStaticUse(p, bs);
      break;
    }
    // A subscripted reference (rule (126)): the callee name resolves to a
    // declared array. Reclassify as a Subscript of the element type; the
    // index (single axis in this stage) is checked against the bounds. A
    // procedure parameter array (rule (126)) is subscriptable the same way.
    if (Symbol* arr = lookup(sc, e->name);
        arr && (arr->kind == Symbol::Var || arr->kind == Symbol::Param) && arr->ty.isArray()) {
      if (e->args.size() != arr->ty.dims.size()) {
        d_.error(e->loc,
                 "array '" + e->name + "' has " + std::to_string(arr->ty.dims.size()) +
                     " dimension(s) and takes " + std::to_string(arr->ty.dims.size()) +
                     " subscript(s)",
                 "(126)");
        e->ty = Type::voidTy();
        break;
      }
      e->kind = Expr::Subscript;
      e->sym = arr;
      int nStar = 0;
      Type reduced;
      if (crossSectionType(e, arr->ty, reduced, nStar)) {
        // A cross-section A(*, ...) (rule 126): a reduced-dim array value whose
        // rank is the number of '*' axes.
        e->ty = reduced;
      } else {
        e->ty = arr->ty.elementType();
      }
      // rule (8)/(42): an array of an enclosing procedure is reached through
      // this procedure's static link.
      noteStaticUse(p, arr);
      checkSubscriptBounds(e, arr);
      break;
    }
    // Built-ins are typed in typeBuiltin; a non-builtin call (a user
    // function procedure) falls through to the general path below.
    if (typeBuiltin(e, p))
      break;
    Symbol* sym = lookup(sc, e->name);
    if (!sym || sym->kind != Symbol::ProcName) {
      d_.error(e->loc, "'" + e->name + "' is not a function procedure", "(123)");
      e->ty = Type::voidTy();
      break;
    }
    const bool isFunc = sym->proc
                            ? (sym->proc->isFunction || (sym->entry && sym->entry->entryIsFunction))
                            : (sym->isEntry && sym->entryIsFunction); // rule (34) external entry
    if (!isFunc) {
      d_.error(e->loc, "'" + e->name + "' is a procedure and returns no value", "(123)");
      e->ty = Type::voidTy();
      break;
    }
    e->sym = sym;
    Proc* callee = sym->proc;
    Stmt* en = sym->entry; // rule (56): an ENTRY name uses the entry's params
    // External entries (rule (34)) carry Type descriptors, not Symbol params, so
    // there is no per-argument type check against callee symbols.
    std::vector<Symbol*> calleeParams =
        en ? en->entryParamSyms : (callee ? callee->paramSyms : std::vector<Symbol*>());
    const size_t expect =
        en ? en->params.size() : (callee ? callee->params.size() : sym->entryParams.size());
    // OPTIONAL parameters (extension, ADR-119): as in the CALL statement,
    // '*' omits an OPTIONAL in any position and trailing OPTIONALs may be
    // left out. External entries keep the exact-count rule (38).
    // Arity counts parameter NAMES: resolved symbols may still lag for a
    // nested callee at INITIAL CALL time, when per-argument checks are
    // skipped exactly as before (ADR-094).
    if (callee || en) {
      const size_t nparams = en ? en->params.size() : callee->params.size();
      if (e->args.size() > nparams) {
        d_.error(e->loc,
                 "'" + e->name + "' expects " + std::to_string(nparams) + " argument(s), " +
                     std::to_string(e->args.size()) + " given",
                 "(78)");
        e->ty = Type::voidTy();
        break;
      }
      size_t i = 0;
      for (; i < e->args.size(); ++i) {
        if (e->args[i]->kind == Expr::Star) {
          if (i >= calleeParams.size() || !calleeParams[i]->isOptional)
            d_.error(e->args[i]->loc, "'*' omits a parameter that is not OPTIONAL (ADR-119)", "");
        } else if (i < calleeParams.size())
          checkAssignable(calleeParams[i]->ty, e->args[i]->ty, e->args[i]->loc, "argument");
      }
      for (; i < nparams; ++i)
        if (i >= calleeParams.size() || !calleeParams[i]->isOptional) {
          d_.error(e->loc,
                   "'" + e->name + "' expects " + std::to_string(nparams) + " argument(s), " +
                       std::to_string(e->args.size()) + " given",
                   "(78)");
          e->ty = Type::voidTy();
          break;
        }
    } else {
      if (e->args.size() != expect) {
        d_.error(e->loc,
                 "'" + e->name + "' expects " + std::to_string(expect) + " argument(s), " +
                     std::to_string(e->args.size()) + " given",
                 "(78)");
        e->ty = Type::voidTy();
        break;
      }
      for (auto& a : e->args)
        if (a->kind == Expr::Star)
          d_.error(a->loc, "'*' arguments to an external entry are not implemented (ADR-119)", "");
      checkByValueArgs(sym, e->args);
    }
    e->ty =
        en ? (en->entryIsFunction ? en->entryRetTy : Type::voidTy())
           : (callee ? callee->retTy : (sym->entryIsFunction ? sym->entryRetTy : Type::voidTy()));
    break;
  }
  case Expr::Unary: {
    typeExpr(e->a.get(), sc, p);
    const Type& t = e->a->ty;
    if (e->op == Tok::Not) {
      if (!t.isBit() && !t.isNumeric())
        d_.error(e->loc, "operand of NOT must be a bit or arithmetic value", "(122)");
      // NOT of a bit string is the string itself complemented (rule (122)).
      e->ty = t.isBit() ? Type::bit(t.len) : Type::bit(1);
    } else {
      if (!t.isNumeric()) {
        d_.error(e->loc, "operand of unary '-' must be arithmetic, found " + t.desc(), "(122)");
        e->ty = Type::fixedBin(31, 0);
      } else {
        e->ty = t;
      }
    }
    break;
  }
  case Expr::Binary: {
    typeExpr(e->a.get(), sc, p);
    typeExpr(e->b.get(), sc, p);
    const Type &A = e->a->ty, &B = e->b->ty;
    switch (e->op) {
    case Tok::Plus:
    case Tok::Minus:
      if (complexArith(A, B)) {
        e->ty = Type::complexTy();
      } else if (!A.isNumeric() || !B.isNumeric()) {
        d_.error(e->loc,
                 "arithmetic operator requires arithmetic operands (" + A.desc() + ", " + B.desc() +
                     ")",
                 "(120)");
        e->ty = Type::fixedBin(31, 0);
      } else {
        e->ty = arithResultType(A, B);
      }
      break;
    case Tok::Star:
      // The product's scale is the sum of the operand scales (ADR-006).
      if (complexArith(A, B)) {
        e->ty = Type::complexTy();
      } else if (!A.isNumeric() || !B.isNumeric()) {
        d_.error(e->loc,
                 "arithmetic operator requires arithmetic operands (" + A.desc() + ", " + B.desc() +
                     ")",
                 "(120)");
        e->ty = Type::fixedBin(31, 0);
      } else {
        e->ty = mulResultType(A, B);
      }
      break;
    case Tok::Slash:
    case Tok::Power:
      // Division and exponentiation are evaluated in floating point in M0;
      // PL/I's exact FIXED scale rules are M2 (ADR-006). Truncation on
      // assignment to a FIXED target preserves the usual observable result.
      if (complexArith(A, B)) {
        if (e->op == Tok::Power) {
          d_.error(e->loc, "complex exponentiation is not implemented in this stage", "(121)");
        }
        e->ty = Type::complexTy();
      } else if (!A.isNumeric() || !B.isNumeric()) {
        d_.error(e->loc, "operator requires arithmetic operands", "(121)");
        e->ty = Type::flt(6);
      } else {
        e->ty = Type::flt(std::max(6, std::max(A.prec, B.prec)));
      }
      break;
    case Tok::Concat:
      if (!A.isChar() || !B.isChar()) {
        d_.error(e->loc,
                 "concatenation of non-character data is not implemented "
                 "in this stage",
                 "(119)");
        e->ty = Type::chr(1);
      } else {
        e->ty = Type::chr(A.len + B.len);
      }
      break;
    case Tok::Amp:
    case Tok::Bar:
      if ((!A.isBit() && !A.isNumeric()) || (!B.isBit() && !B.isNumeric())) {
        d_.error(e->loc, "logical operator requires bit operands", "(116)");
        e->ty = Type::bit(1);
      } else if ((A.isBit() && A.len > 1) || (B.isBit() && B.len > 1)) {
        // Wide strings operate byte-wise (rule (116)); mixing lengths or
        // kinds stays diagnosed while padding rules are out of scope.
        if (!(A.isBit() && B.isBit()) || A.len != B.len)
          d_.error(e->loc, "logical operators require identical BIT lengths in this stage",
                   "(116)");
        e->ty = Type::bit(A.isBit() ? A.len : B.len);
      } else {
        e->ty = Type::bit(1);
      }
      break;
    case Tok::Eq:
    case Tok::Ne:
      // Equality/inequality of a POINTER is allowed only against another
      // POINTER (rules (15),(117)); mixed pointer/arithmetic comparison is
      // diagnosed rather than silently comparing an address as a number.
      if ((A.isPointer() || B.isPointer()) && !(A.isPointer() && B.isPointer()))
        d_.error(e->loc, "a POINTER can only be compared with a POINTER", "(117)");
      else if (A.isComplex() || B.isComplex()) {
        // Exact part-wise equality (CM5); a complex side against anything
        // but complex-or-numeric is diagnosed.
        if (!(A.isComplex() || A.isNumeric()) || !(B.isComplex() || B.isNumeric()))
          d_.error(e->loc, "cannot compare " + A.desc() + " with " + B.desc(), "(117)");
      } else if (A.isChar() != B.isChar())
        d_.error(e->loc, "cannot compare " + A.desc() + " with " + B.desc(), "(117)");
      e->ty = Type::bit(1);
      break;
    case Tok::Lt:
    case Tok::Le:
    case Tok::Gt:
    case Tok::Ge:
    case Tok::Ngt:
    case Tok::Nlt:
      // Ordered comparisons are not meaningful on addresses (rule (117)).
      if (A.isComplex() || B.isComplex())
        d_.error(e->loc, "ordered comparison of a COMPLEX value is not allowed", "(117)");
      else if (A.isPointer() || B.isPointer())
        d_.error(e->loc, "ordered comparison of a POINTER is not allowed", "(117)");
      else if (A.isChar() != B.isChar())
        d_.error(e->loc, "cannot compare " + A.desc() + " with " + B.desc(), "(117)");
      e->ty = Type::bit(1);
      break;
    default:
      e->ty = Type::fixedBin(31, 0);
      break;
    }
    break;
  }
  }
}
// Validate a reduction argument as an element-wise expression (rule (123)):
// every whole reference outside calls resolves to an identical static
// plain-storage shape (set in shapeOut). Silent: false simply keeps the
// existing diagnostic at the caller.
bool Sema::reduceArgShapeOk(Expr* e, Type& shape) {
  std::vector<Expr*> refs;
  std::function<bool(Expr*, bool)> walk = [&](Expr* x, bool underCall) -> bool {
    if (!x || x->ty.isVoid())
      return false;
    if (x->kind == Expr::VarRef && x->ty.isArray()) {
      if (!underCall)
        refs.push_back(x);
      return true;
    }
    if (x->kind == Expr::Subscript && x->ty.isArray())
      return false;
    if (x->kind == Expr::Call) {
      for (auto& a : x->args)
        if (!walk(a.get(), true))
          return false;
      return true;
    }
    if (x->kind == Expr::Binary)
      return walk(x->a.get(), underCall) && walk(x->b.get(), underCall);
    if (x->kind == Expr::Unary)
      return walk(x->a.get(), underCall);
    if (x->kind == Expr::Subscript) {
      for (auto& a : x->args)
        if (!walk(a.get(), underCall))
          return false;
      return true;
    }
    return true;
  };
  if (!walk(e, false) || refs.empty())
    return false;
  // Shapes must match per axis (bounds included — every side shares the
  // target's indices); element types convert per element and may differ.
  shape = refs[0]->ty;
  for (Expr* r : refs)
    if (r->ty.dims != shape.dims)
      return false;
  if (shape.isDynamic())
    return false;
  for (Expr* r : refs)
    if (!wholeArrayStorageOk(r))
      return false;
  return true;
}

// Drop a consumed or diagnosed reduction from the pending list.
void Sema::dropPendingReduce(Expr* call) {
  auto same = [&](const PendingReduce& pr) { return pr.call == call; };
  pendingReduces_.erase(std::remove_if(pendingReduces_.begin(), pendingReduces_.end(), same),
                        pendingReduces_.end());
}

// Expand pending reductions in an assignment value: one static temp per
// call holding the expression values, then the original with bare-temp
// arguments. The statement becomes a Group of the fills plus the original
// and is re-checked as one (each fill desugars through the whole-array
// path). Returns true when expanded.
bool Sema::expandReductionTemps(Stmt* s, Scope* sc, Proc* p) {
  std::vector<Expr*> found;
  std::function<void(Expr*)> scan = [&](Expr* e) {
    if (!e)
      return;
    if ((e->kind == Expr::Call) &&
        (e->name == "SUM" || e->name == "PROD" || e->name == "ANY" || e->name == "ALL")) {
      for (auto& pr : pendingReduces_)
        if (pr.call == e && pr.owner == p) {
          found.push_back(e);
          break;
        }
    }
    if (e->kind == Expr::Binary) {
      scan(e->a.get());
      scan(e->b.get());
    } else if (e->kind == Expr::Unary) {
      scan(e->a.get());
    } else if (e->kind == Expr::Call || e->kind == Expr::Subscript) {
      for (auto& a : e->args)
        scan(a.get());
    } else if (e->kind == Expr::VarRef && e->locPtr) {
      scan(e->locPtr.get());
    }
  };
  scan(s->value.get());
  if (found.empty())
    return false;
  std::vector<StmtP> fills;
  for (Expr* call : found) {
    Expr* arg = call->args[0].get();
    Type shape;
    if (!reduceArgShapeOk(arg, shape)) {
      d_.error(call->loc, "SUM over an array expression is not implemented in this stage", "(123)");
      dropPendingReduce(call);
      continue;
    }
    // The temp holds expression values: the argument's scalar type on the
    // resolved shape. Its assignment desugars through the whole-array path
    // when the Group below is checked.
    Type tty = arg->ty;
    tty.dims = shape.dims;
    std::string base = "PLI$WS";
    while (lookup(sc, base))
      base += "X";
    Symbol* ts = declare(sc, base, tty, call->loc, Symbol::Var, false);
    ts->owner = p;
    p->localSyms.push_back(ts);
    auto fill = std::make_unique<Stmt>();
    fill->kind = Stmt::Assign;
    fill->loc = call->loc;
    auto tgt = std::make_unique<Expr>();
    tgt->kind = Expr::VarRef;
    tgt->name = base;
    tgt->loc = call->loc;
    fill->target = std::move(tgt);
    fill->value = std::move(call->args[0]);
    auto tref = std::make_unique<Expr>();
    tref->kind = Expr::VarRef;
    tref->name = base;
    tref->loc = call->loc;
    call->args[0] = std::move(tref);
    dropPendingReduce(call);
    fills.push_back(std::move(fill));
  }
  // The statement becomes a Group of the fills plus the original assignment
  // (Assign fields only — every other field is default on an Assign node).
  auto inner = std::make_unique<Stmt>();
  inner->kind = Stmt::Assign;
  inner->loc = s->loc;
  inner->labels = std::move(s->labels);
  inner->target = std::move(s->target);
  inner->value = std::move(s->value);
  inner->extraTargets = std::move(s->extraTargets);
  inner->byName = s->byName;
  inner->noSize = s->noSize;
  inner->noSub = s->noSub;
  inner->noZdiv = s->noZdiv;
  s->kind = Stmt::Group;
  s->byName = false;
  for (auto& f : fills)
    s->body.push_back(std::move(f));
  s->body.push_back(std::move(inner));
  checkStmt(s, sc, p);
  return true;
}

// Backstop (rule (123)): reductions over array expressions only expand in
// direct assignment; diagnose anything left pending for this procedure.
void Sema::drainPendingReduces(Proc* p) {
  for (auto& pr : pendingReduces_)
    if (pr.owner == p)
      d_.error(pr.call->loc,
               pr.call->name + " over an array expression is not implemented in this position in "
                               "this stage",
               "(123)");
  pendingReduces_.erase(std::remove_if(pendingReduces_.begin(), pendingReduces_.end(),
                                       [&](const PendingReduce& pr) { return pr.owner == p; }),
                        pendingReduces_.end());
}

// By-value C entries (rule (34)): a POINTER parameter takes a pointer
// argument, and a structure parameter stays diagnosed (C struct-by-value is
// unimplemented; the by-reference copy would silently mismatch the ABI).
// Anything else rides checkAssignable, with scalar conversions handled by
// convert() at emission.
void Sema::checkByValueArgs(Symbol* sym, const std::vector<ExprP>& args) {
  if (!sym->entryByValue)
    return;
  for (size_t i = 0; i < args.size() && i < sym->entryParams.size(); ++i) {
    const Expr* a = args[i].get();
    if (!a || a->kind == Expr::Star || a->ty.isVoid())
      continue;
    const Type& pty = sym->entryParams[i];
    if (pty.isPointer() && !a->ty.isPointer()) {
      d_.error(a->loc, "a POINTER parameter of a by-value entry requires a pointer argument",
               "(38)");
      continue;
    }
    if (pty.isStruct()) {
      d_.error(a->loc, "a structure parameter of a by-value entry is not implemented", "(38)");
      continue;
    }
    checkAssignable(pty, a->ty, a->loc, "argument");
  }
}
// Type a built-in function call; return true if `e` is one of the
// recognised built-ins (typed or diagnosed here). Extracted from the
// typeExpr Call case so each built-in is a self-contained block.
bool Sema::typeBuiltin(Expr* e, Proc* p) {
  // Names matching none of these are user function procedures and are
  // handled in typeExpr's general function-call path.
  // NULL built-in (rule 123, Appendix 1): yields the null POINTER value.
  if (e->name == "NULL") {
    if (!e->args.empty())
      d_.error(e->loc, "NULL takes no arguments", "(123)");
    e->ty = Type::ptr();
    return true;
  }
  // ADDR built-in (rule 123, Appendix 1): yields the address of a variable as
  // a POINTER value.
  if (e->name == "ADDR") {
    if (e->args.size() != 1) {
      d_.error(e->loc, "ADDR takes one argument (a variable)", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = Type::ptr();
    return true;
  }
  // BYADDR marker (rule (38)): byaddr(s) passes a structure's own address so
  // callee writes are visible, opting out of the rule-127 by-value copy for
  // that call. Exactly one structure argument; the type rides through so
  // ordinary call-argument checking still applies. Anything but a direct
  // call argument is diagnosed at emission.
  if (e->name == "BYADDR") {
    if (e->args.size() != 1) {
      d_.error(e->loc, "BYADDR takes one argument (a structure)", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isStruct()) {
      d_.error(e->args[0]->loc, "BYADDR argument must be a structure", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = e->args[0]->ty;
    return true;
  }
  // SUBSTR built-in (M2): substr(s, i, n) yields a character string of
  // length n; the length must be a constant so the result type is sized.
  if (e->name == "SUBSTR") {
    if (e->args.size() != 3) {
      d_.error(e->loc, "SUBSTR expects 3 arguments (string, start, length)", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isChar()) {
      d_.error(e->args[0]->loc, "SUBSTR first argument must be a character string", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[1]->ty.isNumeric() || !e->args[2]->ty.isNumeric()) {
      d_.error(e->loc, "SUBSTR start and length must be numeric", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    int n = e->args[2]->kind == Expr::IntLit ? (int)e->args[2]->ival : -1;
    if (n < 0) {
      // A runtime length (rule (123)): the result is VARYING over the
      // source's static maximum, with the live length fixed at emission.
      e->ty = Type::chr(e->args[0]->ty.len, true);
      return true;
    }
    e->ty = Type::chr(n);
    return true;
  }
  // INDEX built-in (M2): index(s1, s2) yields a FIXED BINARY position.
  if (e->name == "INDEX") {
    if (e->args.size() != 2) {
      d_.error(e->loc, "INDEX expects 2 arguments (string, substring)", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isChar() || !e->args[1]->ty.isChar()) {
      d_.error(e->loc, "INDEX arguments must be character strings", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = Type::fixedBin(31, 0);
    return true;
  }
  // ABS built-in (M2): abs(x) preserves the numeric type of its argument.
  if (e->name == "ABS") {
    if (e->args.size() != 1) {
      d_.error(e->loc, "ABS expects 1 argument", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isNumeric()) {
      d_.error(e->args[0]->loc, "ABS argument must be numeric", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = e->args[0]->ty;
    return true;
  }
  // REVERSE/MAXLENGTH (rule (123), libnet heap-guard forms): reverse(s)
  // mirrors a string; maxlength(v) folds from the declared VARYING maximum
  // with no runtime (like DIM's axis fold).
  if (e->name == "REVERSE") {
    if (e->args.size() != 1) {
      d_.error(e->loc, "REVERSE expects 1 argument (string)", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isChar()) {
      d_.error(e->args[0]->loc, "REVERSE argument must be a character string", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = Type::chr(e->args[0]->ty.len);
    return true;
  }
  if (e->name == "MAXLENGTH") {
    if (e->args.size() != 1) {
      d_.error(e->loc, "MAXLENGTH expects 1 argument (varying string)", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isChar()) {
      d_.error(e->args[0]->loc, "MAXLENGTH argument must be a character string", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.varying && !e->args[0]->ty.starLen) {
      d_.error(e->args[0]->loc, "MAXLENGTH argument must be an adjustable-length or VARYING string", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = Type::fixedBin(31, 0);
    return true;
  }
  // LENGTH built-in (M2): length(s) yields a FIXED BINARY length.
  if (e->name == "LENGTH") {
    if (e->args.size() != 1) {
      d_.error(e->loc, "LENGTH expects 1 argument", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isChar()) {
      d_.error(e->args[0]->loc, "LENGTH argument must be a character string", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = Type::fixedBin(31, 0);
    return true;
  }
  // OMITTED/PRESENT (extension, ADR-119): test whether an OPTIONAL
  // parameter was omitted at the call (passed '*' or left out). Codegen
  // compares the by-reference slot against null.
  if (e->name == "OMITTED" || e->name == "PRESENT") {
    if (e->args.size() != 1) {
      d_.error(e->loc, e->name + " expects 1 argument", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    Expr* a = e->args[0].get();
    if (a->kind != Expr::VarRef || !a->sym ||
        (a->sym->kind != Symbol::Var && a->sym->kind != Symbol::Param) || !a->sym->isOptional) {
      d_.error(a->loc, e->name + " takes an OPTIONAL parameter (ADR-119)", "");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = Type::bit(1);
    return true;
  }
  // Scalar math built-ins (QR2.7, Appendix 1, <math.h> analogues): FLOOR,
  // CEIL, SQRT, EXP, LOG, SIN, COS, TAN, LOG2, LOG10, ATAN, SINH, COSH, TANH,
  // ATANH, ERF, ERFC, and the degree trig variants SIND, COSD, TAND, ATAND —
  // one numeric argument, FLOAT result.
  if (e->name == "FLOOR" || e->name == "CEIL" || e->name == "SQRT" || e->name == "EXP" ||
      e->name == "LOG" || e->name == "SIN" || e->name == "COS" || e->name == "TAN" ||
      e->name == "LOG2" || e->name == "LOG10" || e->name == "ATAN" || e->name == "SINH" ||
      e->name == "COSH" || e->name == "TANH" || e->name == "ATANH" || e->name == "ERF" ||
      e->name == "ERFC" || e->name == "SIND" || e->name == "COSD" || e->name == "TAND" ||
      e->name == "ATAND" || e->name == "ASIN" || e->name == "ACOS" || e->name == "CBRT") {
    if (e->args.size() != 1) {
      d_.error(e->loc, e->name + " expects 1 argument", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isNumeric()) {
      d_.error(e->args[0]->loc, e->name + " argument must be numeric", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = Type::flt(6);
    return true;
  }
  // ATAN2(y, x) (CM5, Appendix 1): C-order two-argument arctangent.
  if (e->name == "ATAN2") {
    if (e->args.size() != 2) {
      d_.error(e->loc, "ATAN2 expects 2 arguments (y, x)", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isNumeric() || !e->args[1]->ty.isNumeric()) {
      d_.error(e->loc, "ATAN2 arguments must be numeric", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = Type::flt(6);
    return true;
  }
  // Complex component/conjugate built-ins (QR2.2/CM5, Appendix 1): COMPLEX(a,b)
  // forms a complex value from a real and an imaginary part; REAL(z) and IMAG(z)
  // extract the real/imaginary part as a FLOAT; CONJG(z) returns the conjugate.
  if (e->name == "COMPLEX") {
    if (e->args.size() != 2) {
      d_.error(e->loc, "COMPLEX expects 2 arguments (real, imaginary)", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isNumeric() || !e->args[1]->ty.isNumeric()) {
      d_.error(e->loc, "COMPLEX arguments must be numeric", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = Type::complexTy();
    return true;
  }
  if (e->name == "REAL" || e->name == "IMAG") {
    if (e->args.size() != 1) {
      d_.error(e->loc, e->name + " expects 1 argument (a complex value)", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isComplex()) {
      d_.error(e->args[0]->loc, e->name + " argument must be a complex value", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = Type::flt(6);
    return true;
  }
  if (e->name == "CONJG") {
    if (e->args.size() != 1) {
      d_.error(e->loc, "CONJG expects 1 argument (a complex value)", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isComplex()) {
      d_.error(e->args[0]->loc, "CONJG argument must be a complex value", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = Type::complexTy();
    return true;
  }
  // TRUNC built-in (M2): trunc(x) preserves the numeric type of its arg.
  if (e->name == "TRUNC") {
    if (e->args.size() != 1) {
      d_.error(e->loc, "TRUNC expects 1 argument", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isNumeric()) {
      d_.error(e->args[0]->loc, "TRUNC argument must be numeric", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = e->args[0]->ty;
    return true;
  }
  // PRECISION built-in (M2): precision(x, p) — x with p digits/bits of
  // precision, keeping x's base type (M0 model; the value is unchanged and
  // only the declared precision differs, so the storage width may change).
  if (e->name == "PRECISION") {
    if (e->args.size() != 2) {
      d_.error(e->loc, "PRECISION expects 2 arguments in this stage", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isNumeric()) {
      d_.error(e->args[0]->loc, "PRECISION first argument must be numeric", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    int p = e->args[1]->kind == Expr::IntLit ? (int)e->args[1]->ival : -1;
    if (p <= 0) {
      d_.error(e->args[1]->loc, "PRECISION must be a positive integer constant", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    Type t = e->args[0]->ty;
    t.prec = p;
    e->ty = t;
    return true;
  }
  // MIN built-in (M2): min(a, b) — the smaller of two numerics, in their
  // common arithmetic type (two-argument form in this stage).
  if (e->name == "MIN") {
    if (e->args.size() != 2) {
      d_.error(e->loc, "MIN takes 2 arguments in this stage", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isNumeric() || !e->args[1]->ty.isNumeric()) {
      d_.error(e->loc, "MIN arguments must be numeric", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = arithResultType(e->args[0]->ty, e->args[1]->ty);
    return true;
  }
  // MAX built-in (M2): max(a, b) — the larger of two numerics, in their
  // common arithmetic type (two-argument form in this stage).
  if (e->name == "MAX") {
    if (e->args.size() != 2) {
      d_.error(e->loc, "MAX takes 2 arguments in this stage", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isNumeric() || !e->args[1]->ty.isNumeric()) {
      d_.error(e->loc, "MAX arguments must be numeric", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = arithResultType(e->args[0]->ty, e->args[1]->ty);
    return true;
  }
  // MOD built-in (M2): mod(a, b) — remainder with the divisor's sign, in
  // the common arithmetic type.
  if (e->name == "MOD") {
    if (e->args.size() != 2) {
      d_.error(e->loc, "MOD expects 2 arguments", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isNumeric() || !e->args[1]->ty.isNumeric()) {
      d_.error(e->loc, "MOD arguments must be numeric", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = arithResultType(e->args[0]->ty, e->args[1]->ty);
    return true;
  }
  // ROUND built-in (M2): round(x, n) — the result is a FLOAT value.
  if (e->name == "ROUND") {
    if (e->args.size() != 2) {
      d_.error(e->loc, "ROUND expects 2 arguments", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isNumeric() || !e->args[1]->ty.isNumeric()) {
      d_.error(e->loc, "ROUND arguments must be numeric", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = Type::flt(6);
    return true;
  }
  // REPEAT built-in (M2): repeat(s, n) — s repeated n times; the length
  // must be a constant so the result type is sized.
  if (e->name == "REPEAT") {
    if (e->args.size() != 2) {
      d_.error(e->loc, "REPEAT expects 2 arguments (string, count)", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isChar()) {
      d_.error(e->args[0]->loc, "REPEAT first argument must be a character string", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    int n = e->args[1]->kind == Expr::IntLit ? (int)e->args[1]->ival : -1;
    if (n < 0) {
      d_.error(e->args[1]->loc, "REPEAT count must be a constant in this stage", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = Type::chr(e->args[0]->ty.len * n);
    return true;
  }
  // VERIFY built-in (M2): verify(s, t[, start]) yields a FIXED BINARY
  // position — the 2-arg form searches from 1, the 3-arg merged-bridge form
  // from start (rule (123)).
  if (e->name == "VERIFY") {
    if (e->args.size() != 2 && e->args.size() != 3) {
      d_.error(e->loc, "VERIFY expects 2 or 3 arguments (string, set[, start])", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isChar() || !e->args[1]->ty.isChar()) {
      d_.error(e->loc, "VERIFY arguments must be (string, set[, start])", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (e->args.size() == 3 && !e->args[2]->ty.isNumeric()) {
      d_.error(e->args[2]->loc, "VERIFY start must be numeric", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = Type::fixedBin(31, 0);
    return true;
  }
  // TRANSLATE built-in (M2): translate(s, out, in) — same length as s.
  if (e->name == "TRANSLATE") {
    if (e->args.size() != 3) {
      d_.error(e->loc, "TRANSLATE expects 3 arguments (string, out, in)", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isChar() || !e->args[1]->ty.isChar() || !e->args[2]->ty.isChar()) {
      d_.error(e->loc, "TRANSLATE arguments must be character strings", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = Type::chr(e->args[0]->ty.len);
    return true;
  }
  // TRIM built-in (extension, ADR-107): trim(s[, pad]) — s minus
  // leading/trailing blanks (or pad-set characters); the result keeps
  // the input length, left-justified and blank-padded.
  if (e->name == "TRIM") {
    if (e->args.size() < 1 || e->args.size() > 2) {
      d_.error(e->loc, "TRIM expects 1 or 2 arguments (string[, pad])", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    for (auto& a : e->args)
      if (!a->ty.isChar()) {
        d_.error(a->loc, "TRIM arguments must be character strings", "(123)");
        e->ty = Type::voidTy();
        return true;
      }
    e->ty = Type::chr(e->args[0]->ty.len);
    return true;
  }
  // TALLY built-in (extension, ADR-107): tally(x, y) — non-overlapping
  // occurrences of y in x, case-sensitive; zero when absent or null.
  if (e->name == "TALLY") {
    if (e->args.size() != 2) {
      d_.error(e->loc, "TALLY expects 2 arguments (string, substring)", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isChar() || !e->args[1]->ty.isChar()) {
      d_.error(e->loc, "TALLY arguments must be character strings", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = Type::fixedBin(31, 0);
    return true;
  }
  // UPPERCASE built-in (Bridge B4, rule 123): uppercase(s) — s with
  // a-z folded to A-Z, other characters unchanged; same length as s.
  if (e->name == "UPPERCASE") {
    if (e->args.size() != 1) {
      d_.error(e->loc, "UPPERCASE expects 1 argument (string)", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isChar()) {
      d_.error(e->args[0]->loc, "UPPERCASE argument must be a character string", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = Type::chr(e->args[0]->ty.len);
    return true;
  }
  // LOWERCASE/CENTER/SEARCH/VERIFY/RANK/COLLATE (rule (123)): merged
  // from the builtins bridge (libc-native, blank-padded, 1-based positions).
  if (e->name == "LOWERCASE") {
    if (e->args.size() != 1) {
      d_.error(e->loc, "LOWERCASE expects 1 argument (string)", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isChar()) {
      d_.error(e->args[0]->loc, "LOWERCASE argument must be a character string", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = Type::chr(e->args[0]->ty.len);
    return true;
  }
  if (e->name == "CENTER") {
    if (e->args.size() != 2) {
      d_.error(e->loc, "CENTER expects 2 arguments (string, width)", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isChar() || !e->args[1]->ty.isNumeric()) {
      d_.error(e->loc, "CENTER arguments must be (string, width)", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    int n = e->args[1]->kind == Expr::IntLit ? (int)e->args[1]->ival : -1;
    if (n < 0) {
      d_.error(e->args[1]->loc, "CENTER width must be a constant in this stage", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = Type::chr(n);
    return true;
  }
  if (e->name == "SEARCH") {
    if (e->args.size() != 3) {
      d_.error(e->loc, "SEARCH expects 3 arguments (string, set, start)", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isChar() || !e->args[1]->ty.isChar() || !e->args[2]->ty.isNumeric()) {
      d_.error(e->loc, "SEARCH arguments must be (string, set, start)", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = Type::fixedBin(31, 0);
    return true;
  }
  if (e->name == "RANK") {
    if (e->args.size() != 1) {
      d_.error(e->loc, "RANK expects 1 argument (string)", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isChar()) {
      d_.error(e->args[0]->loc, "RANK argument must be a character string", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = Type::fixedBin(31, 0);
    return true;
  }
  if (e->name == "COLLATE") {
    if (e->args.size() != 1) {
      d_.error(e->loc, "COLLATE expects 1 argument (code)", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isNumeric()) {
      d_.error(e->args[0]->loc, "COLLATE argument must be numeric", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = Type::chr(1);
    return true;
  }
  // HIGH/LOW built-ins (M2): high(n)/low(n) — n copies of the top/bottom
  // collating character; n must be constant to size the result.
  if (e->name == "HIGH" || e->name == "LOW") {
    const char* nm = e->name == "HIGH" ? "HIGH" : "LOW";
    if (e->args.size() != 1) {
      d_.error(e->loc, std::string(nm) + " expects 1 argument", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    int n = e->args[0]->kind == Expr::IntLit ? (int)e->args[0]->ival : -1;
    if (n < 0) {
      d_.error(e->args[0]->loc, std::string(nm) + " length must be a constant in this stage",
               "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = Type::chr(n);
    return true;
  }
  // DATE/TIME built-ins (M2): date() -> CHARACTER(8) 'YYYYMMDD', time()
  // -> CHARACTER(6) 'HHMMSS'; both take no arguments.
  if (e->name == "DATE" || e->name == "TIME") {
    if (!e->args.empty()) {
      d_.error(e->loc, std::string(e->name == "DATE" ? "DATE" : "TIME") + " takes no arguments",
               "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = Type::chr(e->name == "DATE" ? 8 : 6);
    return true;
  }
  // SYSPARM built-in (rule (123)): sysparm() -> CHARACTER(n) holding the
  // SYSPARM compiler-option value verbatim (empty when none was given).
  if (e->name == "SYSPARM") {
    if (!e->args.empty()) {
      d_.error(e->loc, "SYSPARM takes no arguments", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = Type::chr((int)sysparm_.size());
    return true;
  }
  // ONCODE built-in (rules (91)-(94)): ONCODE() yields the current ERROR
  // code as FIXED BINARY(31): 1 inside an ERROR unit raised by SIGNAL,
  // 0 elsewhere. Takes no arguments.
  if (e->name == "ONCODE") {
    if (!e->args.empty()) {
      d_.error(e->loc, "ONCODE takes no arguments", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = Type::fixedBin(31, 0);
    return true;
  }
  // FIXED built-in (rule (123)): FIXED(char) parses decimal text with
  // truncation, FIXED(numeric) truncates toward zero (the PL/I
  // FIXED(x) conversion entry point used by libnet's `port =
  // fixed(substr(url, pos+1))`).
  if (e->name == "FIXED") {
    if (e->args.size() != 1) {
      d_.error(e->loc, "FIXED expects 1 argument", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    const Type& a = e->args[0]->ty;
    if (a.isChar() || a.isNumeric()) {
      e->ty = Type::fixedBin(31, 0);
      return true;
    }
    d_.error(e->args[0]->loc, "FIXED argument must be character or numeric", "(123)");
    e->ty = Type::voidTy();
    return true;
  }
  // CHAR built-in (rule (123)): CHAR(x) renders a scalar value as text
  // (the PL/I CHAR(x) conversion used by libnet's
  // `trim(char(moves))` and `'...' || char(port)` chains).
  if (e->name == "CHAR") {
    if (e->args.size() != 1) {
      d_.error(e->loc, "CHAR expects 1 argument", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    const Type& a = e->args[0]->ty;
    if (a.isChar()) {
      e->ty = Type::chr(a.len);
      return true;
    }
    if (a.isNumeric() || a.isBit()) {
      e->ty = Type::chr(24); // decimal or %.6g image, blank-padded
      return true;
    }
    d_.error(e->args[0]->loc, "CHAR argument must be a scalar value in this stage", "(123)");
    e->ty = Type::voidTy();
    return true;
  }
  // EVENT built-in (rules (79),(82), QR2.8): EVENT(ev) polls an event's
  // completion status as BIT(1) ('1'B complete, '0'B incomplete).
  if (e->name == "EVENT") {
    if (e->args.size() != 1) {
      d_.error(e->loc, "EVENT expects 1 argument (an EVENT variable)", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    Expr* a = e->args[0].get();
    if (a->kind != Expr::VarRef || !a->sym || a->sym->kind == Symbol::ProcName ||
        !a->sym->ty.isEvent()) {
      d_.error(a->loc, "EVENT argument must be an EVENT variable", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = Type::bit(1);
    return true;
  }
  // PRIORITY built-in/pseudo-variable (rule (79), QR2.8): task priorities
  // are best-effort ignored in this stage; diagnose uses, never accept.
  if (e->name == "PRIORITY") {
    d_.error(e->loc, "PRIORITY is not implemented in this stage", "(79)");
    e->ty = Type::voidTy();
    return true;
  }
  // MULTIPLY built-in (M2): multiply(a, b) — product of two numerics; for
  // FIXED operands the result scale is the sum of the operand scales.
  if (e->name == "MULTIPLY") {
    if (e->args.size() != 2) {
      d_.error(e->loc, "MULTIPLY expects 2 arguments in this stage", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isNumeric() || !e->args[1]->ty.isNumeric()) {
      d_.error(e->loc, "MULTIPLY arguments must be numeric", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = mulResultType(e->args[0]->ty, e->args[1]->ty);
    return true;
  }
  // DIVIDE built-in (M2): divide(a, b) — quotient of two numerics, computed
  // in floating point (M0 model, ADR-014; exact decimal division is M2).
  if (e->name == "DIVIDE") {
    if (e->args.size() != 2) {
      d_.error(e->loc, "DIVIDE expects 2 arguments in this stage", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isNumeric() || !e->args[1]->ty.isNumeric()) {
      d_.error(e->loc, "DIVIDE arguments must be numeric", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    e->ty = Type::flt(std::max(6, std::max(e->args[0]->ty.prec, e->args[1]->ty.prec)));
    return true;
  }
  // Array attribute built-ins (M2, rules (12),(13),(123)): LBOUND/HBOUND/
  // DIM/DIMENSION of an array. With constant bounds these fold to
  // compile-time values; the first argument must be an unsubscripted array.
  // An optional second integer-constant argument selects the axis (1-based);
  // without it LBOUND/HBOUND report the first axis and DIM the total count.
  if (e->name == "LBOUND" || e->name == "HBOUND" || e->name == "DIM" ||
      e->name == "DIMENSION") {
    if (e->args.size() != 1 && e->args.size() != 2) {
      d_.error(e->loc, e->name + " takes an array and an optional axis in this stage", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    Expr* a = e->args[0].get();
    // An unsubscripted array reference: a plain array variable/parameter
    // (A or x(k)) or a qualified structure member array (S.V). a->ty is the
    // resolved reference type, which is the array for an unsubscripted VarRef.
    bool isArr = a->kind == Expr::VarRef && a->sym && a->ty.isArray();
    if (!isArr) {
      d_.error(a->loc, e->name + " argument must be an array in this stage", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (e->args.size() == 2) {
      Expr* n = e->args[1].get();
      if (n->kind != Expr::IntLit) {
        d_.error(n->loc, e->name + " axis must be an integer constant in this stage", "(123)");
        e->ty = Type::voidTy();
        return true;
      }
      long long rank = (long long)a->ty.dims.size();
      if (n->ival < 1 || n->ival > rank) {
        d_.error(n->loc,
                 e->name + " axis " + std::to_string(n->ival) + " out of range (1.." +
                     std::to_string(rank) + ")",
                 "(123)");
        e->ty = Type::voidTy();
        return true;
      }
    }
    e->ty = Type::fixedBin(31, 0);
    return true;
  }
  // Array reduction built-ins (M2, rule (123)): SUM/PROD reduce a numeric
  // array to its element type; ANY/ALL reduce a BIT array to BIT(1). Each
  // takes a single unsubscripted array argument in this stage.
  if (e->name == "SUM" || e->name == "PROD" || e->name == "ANY" || e->name == "ALL") {
    if (e->args.size() != 1) {
      d_.error(e->loc, e->name + " takes one array argument in this stage", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    Expr* a = e->args[0].get();
    // An unsubscripted array reference, including a qualified structure member
    // array (S.V); a->ty is the resolved array reference type.
    bool isArr = a->kind == Expr::VarRef && a->sym && a->ty.isArray();
    if (isArr) {
      const Type& el = a->ty.elementType();
      // Reduction accumulators hold single bits; wider elements stay out.
      if (el.isBit() && el.len > 1) {
        d_.error(a->loc, e->name + " over a BIT(n>1) array is not implemented in this stage",
                 "(123)");
        e->ty = Type::voidTy();
        return true;
      }
      if (e->name == "ANY" || e->name == "ALL") {
        if (!el.isBit()) {
          d_.error(a->loc, e->name + " requires a BIT array in this stage", "(123)");
          e->ty = Type::voidTy();
          return true;
        }
        e->ty = Type::bit(1);
      } else {
        if (el.isChar() || !el.isNumeric()) {
          d_.error(a->loc, e->name + " requires a numeric array in this stage", "(123)");
          e->ty = Type::voidTy();
          return true;
        }
        e->ty = el;
      }
      return true;
    }
    // An element-wise expression over whole arrays of one static shape
    // (rule (123); QR2.1): validated here, expanded to a static temp in
    // direct assignment, diagnosed at the backstop anywhere else.
    Type shape;
    if (reduceArgShapeOk(a, shape)) {
      if ((e->name == "ANY" || e->name == "ALL") && !a->ty.isBit()) {
        d_.error(a->loc, e->name + " requires a BIT expression in this stage", "(123)");
        e->ty = Type::voidTy();
        return true;
      }
      if ((e->name == "SUM" || e->name == "PROD") && (a->ty.isChar() || !a->ty.isNumeric())) {
        d_.error(a->loc, e->name + " requires a numeric expression in this stage", "(123)");
        e->ty = Type::voidTy();
        return true;
      }
      pendingReduces_.push_back({e, p});
      e->ty = (e->name == "ANY" || e->name == "ALL") ? Type::bit(1) : a->ty;
      return true;
    }
    d_.error(a->loc, e->name + " argument must be an array in this stage", "(123)");
    e->ty = Type::voidTy();
    return true;
  }
  return false;
}
