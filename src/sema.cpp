#include "sema.h"
#include <algorithm>
#include <functional>

// FIXED op FIXED -> FIXED with the wider precision and the larger scale;
// anything involving FLOAT is FLOAT. This is the *common* result type for
// +,-,comparison (and MIN/MAX/MOD), where a mixed-scale operand is rescaled
// up to the common scale (ADR-006). The product (`*`, MULTIPLY) uses
// mulResultType instead, whose scale is the sum of the operand scales.
Type arithResultType(const Type& a, const Type& b) {
  if (a.k == TK::Float || b.k == TK::Float)
    return Type::flt(std::max(a.k == TK::Float ? a.prec : 6, b.k == TK::Float ? b.prec : 6));
  int bits = std::max(a.intBits(), b.intBits());
  return Type::fixedBin(bits == 64 ? 63 : 31, std::max(a.scale, b.scale));
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

bool Sema::run(Program& prog, bool compileOnly) {
  prog_ = &prog;

  // Pass 1: every procedure name is visible in its parent's scope, so that
  // CALL can be resolved regardless of textual order. External procedures
  // share a program-level scope, so siblings can call each other.
  rootScope_ = new Scope();
  scopes_.push_back(std::unique_ptr<Scope>(rootScope_));
  for (auto& p : prog.procs) {
    p->irName = "@PLI_" + p->name;
    // A function procedure's symbol carries its result type so that a
    // function reference (rule (123)) types as the returned value.
    Scope* outer = p->parent ? scopeFor(p->parent) : rootScope_;
    Type symTy = p->isFunction ? p->retTy : Type::voidTy();
    Symbol* s = declare(outer, p->name, symTy, p->loc, Symbol::ProcName, false);
    s->proc = p.get();
    p->irName = "@PLI_" + (p->parent ? p->parent->name + "$" : std::string()) + p->name;
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

  // Pass 2: declarations, resolution and typing, procedure by procedure.
  for (auto& p : prog.procs)
    processProc(p.get());

  // Pass 3: bottom-up static-link environments (rule (8)); a proc's env is the
  // set of enclosing variables its whole subtree accesses.
  for (auto& p : prog.procs)
    if (!p->parent)
      computeEnv(p.get());

  if (!prog.mainProc) {
    if (!prog.procs.empty()) {
      // A relocatable object (`-c`) may be a library with no entry point; the
      // caller links it against its own `main` (e.g. a C driver).
      if (!compileOnly) {
        d_.warn(prog.procs.front()->loc,
                "no procedure has OPTIONS(MAIN); using '" + prog.procs.front()->name +
                    "' as the program entry point",
                "(5)");
        prog.mainProc = prog.procs.front().get();
        prog.mainProc->isMain = true;
      }
    } else {
      d_.error({}, "translation unit contains no procedure", "(1)");
    }
  }
  return d_.ok();
}

void Sema::processProc(Proc* p) {
  Scope* sc = scopeFor(p);

  // STORAGE (M1): every procedure's variables are AUTOMATIC (stack), so each
  // activation gets its own copy — this makes external procedures reentrant.
  // Internal procedures reach enclosing automatic storage through a static
  // link (ADR-027); the M0 ADR-010 deviation (external vars as globals) is
  // removed. Explicit STATIC is accepted and treated as AUTOMATIC for now.
  bool isStatic = false;

  beginScopes_.clear();
  collectDecls(p->body, sc, p, isStatic);

  // Parameters: a DECLARE inside the procedure supplies their attributes;
  // otherwise the implicit rule applies. Parameters are always by reference.
  resolveParams(sc, p, p->params, p->paramSyms);

  // rule (56) ENTRY statements: each entry point declares its own parameters,
  // by reference, in the same scope (so a name shared with the procedure's own
  // parameter list refers to the same variable).
  for (auto& st : p->body) {
    if (st && st->kind == Stmt::Entry) {
      resolveParams(sc, p, st->params, st->entryParamSyms);
      Type rt = st->entryIsFunction ? st->entryRetTy : Type::voidTy();
      Type prt = p->isFunction ? p->retTy : Type::voidTy();
      if (rt != prt)
        d_.error(st->loc,
                 "ENTRY result type differs from the procedure's; a mixed return type is not "
                 "implemented in this stage",
                 "(56)");
    }
  }

  procLabels_.clear();
  for (auto& s : p->body)
    collectLabels(s.get());
  for (auto& s : p->body)
    checkStmt(s.get(), sc, p);
}

// Parameters are always by reference; a DECLARE supplies their attributes,
// otherwise the implicit rule applies. A name already resolved as a parameter
// (e.g. an ENTRY parameter reusing a procedure parameter) is reused, so the
// two spellings refer to one variable.
void Sema::resolveParams(Scope* sc, Proc* p, const std::vector<std::string>& names,
                         std::vector<Symbol*>& out) {
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

void Sema::collectDecls(std::vector<StmtP>& body, Scope* sc, Proc* p, bool isStatic) {
  for (auto& s : body) {
    if (!s)
      continue;
    if (s->kind == Stmt::Declare) {
      // Build the level-numbered structure hierarchy (rule 11): a member item
      // belongs to the nearest preceding item with a strictly smaller level.
      std::vector<DeclItem*> items;
      std::vector<int> parentOf;
      std::vector<std::vector<int>> children;
      for (auto& item : s->decls) {
        if (item.isEntry) {
          // External C entry: a ProcName symbol with no PL/I body. The C
          // symbol is the EXTERNAL('name') override when given, else the
          // upper-cased PL/I identifier (rules (34),(38); z/OS ILC naming).
          // Not storage.
          Symbol* sym = declare(sc, item.name, Type::voidTy(), item.loc, Symbol::ProcName, false);
          sym->isEntry = true;
          sym->entryParams = item.entryParams;
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
      // children; a leaf keeps its parsed scalar/array type.
      std::function<Type(int)> buildType = [&](int idx) -> Type {
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
          Symbol* tpl = lookup(sc, it.like);
          if (!tpl || tpl->kind != Symbol::Var || !tpl->ty.isStruct()) {
            d_.error(it.loc, "LIKE reference '" + it.like + "' is not a structure in this scope",
                     "(43)");
            return Type::voidTy();
          }
          return tpl->ty; // deep copy via Type's copy constructor
        }
        if (children[idx].empty())
          return it.ty;
        std::vector<Member> ms;
        for (int c : children[idx])
          ms.push_back({items[c]->name, buildType(c)});
        return Type::structTy(std::move(ms));
      };
      // Declare each top-level item (no parent) as a variable; members are
      // reached by qualification and get no standalone symbol or storage.
      for (int idx = 0; idx < (int)items.size(); ++idx) {
        if (parentOf[idx] != -1)
          continue;
        DeclItem& item = *items[idx];
        item.ty = buildType(idx);
        item.sym = declare(sc, item.name, item.ty, item.loc, Symbol::Var, isStatic);
        item.sym->owner = p; // which procedure's frame holds this variable
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
          } else if (item.ty.isStruct() || base->ty.isStruct()) {
            d_.error(item.loc, "DEFINED on a structure is not implemented in this stage", "(24)");
          } else if (!(item.ty == base->ty)) {
            d_.error(item.loc,
                     "DEFINED requires the item and base to have the same type (" + item.ty.desc() +
                         " vs " + base->ty.desc() + ")",
                     "(24)");
          } else {
            item.sym->definedBase = base;
            isDefined = true;
          }
        }
        // Only scalar (numeric/BIT) element arrays are served in this stage;
        // character element arrays are diagnosed, never silently miscompiled
        // (invariant 2). INITIAL on an array (rule 26) expands its itemlist
        // (with iteration factors and '*') into one value per element.
        if (item.ty.isArray() && !isDefined) {
          if (item.ty.elementType().isChar())
            d_.error(item.loc, "arrays of CHARACTER are not implemented in this stage", "(12)");
          if (!item.initItems.empty()) {
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
        if (item.ty.isStruct() && !item.initItems.empty())
          d_.error(item.loc, "INITIAL on a structure is not implemented in this stage", "(26)");
        // Record AUTOMATIC variables so codegen allocates them (STATIC ones
        // become LLVM globals via emitGlobals). This must cover variables of
        // BEGIN blocks too, hence the Proc* here.
        if (item.sym->kind == Symbol::Var && !item.sym->isStatic && !item.sym->definedBase)
          p->localSyms.push_back(item.sym);
        if (item.init) {
          // M0 accepts a literal (optionally signed) as INITIAL value.
          if (Expr* folded = foldInitialConstant(item.init.get(), item.ty, item.loc))
            item.sym->initExpr = folded; // consumed by code generation
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
      } else if (s->thenS->kind == Stmt::Declare) {
        d_.error(s->thenS->loc, "DECLARE cannot be the body of an IF statement", "(74)");
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
      } else if (s->elseS->kind == Stmt::Declare) {
        d_.error(s->elseS->loc, "DECLARE cannot be the body of an IF statement", "(74)");
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

bool Sema::checkAssignable(const Type& dst, const Type& src, SourceLoc loc, const char* what) {
  if (dst.isStruct() || src.isStruct()) {
    // Whole-structure assignment (rule 127): a copy between two structures of
    // identical shape (same members, recursively). Anything else — a shape
    // mismatch, or mixing a structure with a non-structure — is diagnosed.
    if (dst.isStruct() && src.isStruct() && dst == src)
      return true;
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
  if (dst.isBit() && (src.isBit() || src.isNumeric()))
    return true;
  if (dst.isChar() && src.isChar())
    return true;
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
  if (lit->kind != Expr::IntLit && lit->kind != Expr::FltLit && lit->kind != Expr::CharLit &&
      lit->kind != Expr::BitLit) {
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
    n *= (long long)(d.second - d.first + 1);
  return n;
}

void Sema::checkStmt(Stmt* s, Scope* sc, Proc* p) {
  if (!s)
    return;
  switch (s->kind) {
  case Stmt::Null:
    break;
  case Stmt::Declare:
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
        if (!s->value->ty.isVoid() && !t->ty.isVoid())
          checkAssignable(t->ty, s->value->ty, t->loc, "assignment");
        return true;
      };
      bool ok = checkOne(s->target.get());
      for (auto& t : s->extraTargets)
        ok = checkOne(t.get()) && ok;
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
    // Cross-section assignment (rule 126): B = A(i, *) — the right-hand side is
    // a reduced-dim array value produced by a '*' subscript. The target must be
    // a whole array of exactly that reduced shape.
    {
      bool isCross = false;
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
  case Stmt::Group:
    for (auto& b : s->body)
      checkStmt(b.get(), sc, p);
    break;
  case Stmt::Begin: {
    // Descend with the block's own scope (rule (68)); see collectDecls.
    auto it = beginScopes_.find(s);
    Scope* bsc = it != beginScopes_.end() ? it->second : sc;
    for (auto& b : s->body)
      checkStmt(b.get(), bsc, p);
    break;
  }
  case Stmt::DoWhile:
    typeExpr(s->cond.get(), sc, p);
    for (auto& b : s->body)
      checkStmt(b.get(), sc, p);
    break;
  case Stmt::DoIter: {
    Symbol* sym = lookup(sc, s->name);
    if (!sym) {
      sym = implicitDeclare(sc, s->name, s->loc, false);
      sym->owner = p;
      p->localSyms.push_back(sym); // implicit vars are AUTOMATIC storage
    }
    s->sym = sym;
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
    for (auto& b : s->body)
      checkStmt(b.get(), sc, p);
    break;
  }
  case Stmt::Put: {
    typeExpr(s->skipCount.get(), sc, p);
    for (auto& it : s->items) {
      typeExpr(it.get(), sc, p);
      if (it->ty.isVoid())
        d_.error(it->loc, "invalid data list item", "(110)");
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
    const size_t expect = en       ? en->params.size()
                          : callee ? callee->params.size()
                                   : sym->entryParams.size();
    if (s->args.size() != expect) {
      d_.error(s->loc,
               "'" + s->name + "' expects " + std::to_string(expect) + " argument(s), " +
                   std::to_string(s->args.size()) + " given",
               "(78)");
      break;
    }
    if (callee)
      for (size_t i = 0; i < s->args.size(); ++i)
        if (i < calleeParams.size())
          checkAssignable(calleeParams[i]->ty, s->args[i]->ty, s->args[i]->loc, "argument");
    break;
  }
  case Stmt::Return: {
    // rule (81): RETURN(value) supplies a function procedure's result;
    // a plain RETURN ends a procedure.
    if (s->value) {
      if (!p->isFunction) {
        d_.error(s->loc, "RETURN with a value is only valid in a function procedure", "(81)");
        break;
      }
      typeExpr(s->value.get(), sc, p);
      if (!s->value->ty.isVoid())
        checkAssignable(p->retTy, s->value->ty, s->loc, "RETURN value");
    } else if (p->isFunction) {
      d_.error(s->loc, "a function procedure must RETURN a value", "(81)");
    }
    break;
  }
  case Stmt::Stop:
  case Stmt::Leave:
  case Stmt::Entry: // declaration-like; params/type resolved in processProc
    break;
  case Stmt::Goto:
    // GO TO target must be a label defined in this procedure (rules (64),(77)).
    if (procLabels_.find(s->name) == procLabels_.end())
      d_.error(s->loc, "'" + s->name + "' is not a label in this procedure", "(77)");
    break;
  }
}

// A constant subscript is range-checked at compile time (SUBSCRIPTRANGE, rule
// (126)); a runtime index is left to the generated bounds check in IRGen.
// Each constant subscript is checked against its own axis (rules (12),(13)).
void Sema::checkSubscriptBounds(Expr* e, Symbol* arr) {
  checkSubscriptBoundsDims(e, arr->ty.dims, arr->name);
}

void Sema::checkSubscriptBoundsDims(Expr* e, const std::vector<std::pair<int, int>>& dims,
                                    const std::string& name) {
  const size_t n = std::min(e->args.size(), dims.size());
  for (size_t k = 0; k < n; ++k) {
    Expr* idx = e->args[k].get();
    if (idx->kind != Expr::IntLit)
      continue;
    long long v = idx->ival;
    const auto& [lb, ub] = dims[k];
    if (v < lb || v > ub)
      d_.error(idx->loc,
               "subscript " + std::to_string(v) + " is out of bounds " + std::to_string(lb) + ":" +
                   std::to_string(ub) + " for array '" + name + "'",
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
  case Expr::FltLit:
    e->ty = Type::flt(6);
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
    if (sym->kind == Symbol::Var && sym->owner && sym->owner != p && isDescendantOf(p, sym->owner))
      addEnv(p->directUses, sym);
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
        // A member-array cross-section S.A(i, *) (rules 124,126): reduced-dim
        // array value.
        if (nStar > 1) {
          d_.error(e->loc,
                   "a cross-section with more than one '*' is not implemented in this stage",
                   "(126)");
          e->ty = Type::voidTy();
          break;
        }
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
      if (bs->owner && bs->owner != p && isDescendantOf(p, bs->owner))
        addEnv(p->directUses, bs);
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
        // A cross-section A(*, ...) (rule 126): a reduced-dim array value.
        if (nStar > 1) {
          d_.error(e->loc,
                   "a cross-section with more than one '*' is not implemented in this stage",
                   "(126)");
          e->ty = Type::voidTy();
          break;
        }
        e->ty = reduced;
      } else {
        e->ty = arr->ty.elementType();
      }
      // rule (8)/(42): an array of an enclosing procedure is reached through
      // this procedure's static link.
      if (arr->owner && arr->owner != p && isDescendantOf(p, arr->owner))
        addEnv(p->directUses, arr);
      checkSubscriptBounds(e, arr);
      break;
    }
    // Built-ins are typed in typeBuiltin; a non-builtin call (a user
    // function procedure) falls through to the general path below.
    if (typeBuiltin(e))
      break;
    Symbol* sym = lookup(sc, e->name);
    if (!sym || sym->kind != Symbol::ProcName) {
      d_.error(e->loc, "'" + e->name + "' is not a function procedure", "(123)");
      e->ty = Type::voidTy();
      break;
    }
    if (!sym->proc || !(sym->proc->isFunction || (sym->entry && sym->entry->entryIsFunction))) {
      d_.error(e->loc, "'" + e->name + "' is a procedure and returns no value", "(123)");
      e->ty = Type::voidTy();
      break;
    }
    e->sym = sym;
    Proc* callee = sym->proc;
    Stmt* en = sym->entry; // rule (56): an ENTRY name uses the entry's params
    std::vector<Symbol*> calleeParams = en ? en->entryParamSyms : callee->paramSyms;
    const size_t expect = en ? en->params.size() : callee->params.size();
    if (e->args.size() != expect) {
      d_.error(e->loc,
               "'" + e->name + "' expects " + std::to_string(expect) + " argument(s), " +
                   std::to_string(e->args.size()) + " given",
               "(78)");
      e->ty = Type::voidTy();
      break;
    }
    for (size_t i = 0; i < e->args.size(); ++i)
      if (i < calleeParams.size())
        checkAssignable(calleeParams[i]->ty, e->args[i]->ty, e->args[i]->loc, "argument");
    e->ty = en ? (en->entryIsFunction ? en->entryRetTy : Type::voidTy()) : callee->retTy;
    break;
  }
  case Expr::Unary: {
    typeExpr(e->a.get(), sc, p);
    const Type& t = e->a->ty;
    if (e->op == Tok::Not) {
      if (!t.isBit() && !t.isNumeric())
        d_.error(e->loc, "operand of NOT must be a bit or arithmetic value", "(122)");
      e->ty = Type::bit(1);
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
      if (!A.isNumeric() || !B.isNumeric()) {
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
      if (!A.isNumeric() || !B.isNumeric()) {
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
      if (!A.isNumeric() || !B.isNumeric()) {
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
      if ((!A.isBit() && !A.isNumeric()) || (!B.isBit() && !B.isNumeric()))
        d_.error(e->loc, "logical operator requires bit operands", "(116)");
      e->ty = Type::bit(1);
      break;
    case Tok::Eq:
    case Tok::Ne:
    case Tok::Lt:
    case Tok::Le:
    case Tok::Gt:
    case Tok::Ge:
    case Tok::Ngt:
    case Tok::Nlt:
      if (A.isChar() != B.isChar())
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
// Type a built-in function call; return true if `e` is one of the
// recognised built-ins (typed or diagnosed here). Extracted from the
// typeExpr Call case so each built-in is a self-contained block.
bool Sema::typeBuiltin(Expr* e) {
  // Names matching none of these are user function procedures and are
  // handled in typeExpr's general function-call path.
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
      d_.error(e->args[2]->loc, "SUBSTR length must be a constant in this stage", "(123)");
      e->ty = Type::voidTy();
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
    // TRUNC of a scaled FIXED value must remove its fractional digits,
    // which the scaled representation does not yet do (invariant 2).
    if (e->args[0]->ty.isFixed() && e->args[0]->ty.scale != 0) {
      d_.error(e->args[0]->loc, "TRUNC of a scaled FIXED value is not implemented in this stage",
               "(16)");
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
  // VERIFY built-in (M2): verify(s, t) yields a FIXED BINARY position.
  if (e->name == "VERIFY") {
    if (e->args.size() != 2) {
      d_.error(e->loc, "VERIFY expects 2 arguments (string, set)", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    if (!e->args[0]->ty.isChar() || !e->args[1]->ty.isChar()) {
      d_.error(e->loc, "VERIFY arguments must be character strings", "(123)");
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
  // DIM of a fixed-size single-axis array. With constant bounds these fold
  // to compile-time values; the argument must be an unsubscripted array.
  if (e->name == "LBOUND" || e->name == "HBOUND" || e->name == "DIM") {
    if (e->args.size() != 1) {
      d_.error(e->loc, e->name + " takes one array argument in this stage", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    Expr* a = e->args[0].get();
    bool isArr =
        (a->kind == Expr::VarRef && a->sym &&
         (a->sym->kind == Symbol::Var || a->sym->kind == Symbol::Param) && a->sym->ty.isArray());
    if (!isArr) {
      d_.error(a->loc, e->name + " argument must be an array in this stage", "(123)");
      e->ty = Type::voidTy();
      return true;
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
    bool isArr =
        (a->kind == Expr::VarRef && a->sym &&
         (a->sym->kind == Symbol::Var || a->sym->kind == Symbol::Param) && a->sym->ty.isArray());
    if (!isArr) {
      d_.error(a->loc, e->name + " argument must be an array in this stage", "(123)");
      e->ty = Type::voidTy();
      return true;
    }
    const Type& el = a->sym->ty.elementType();
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
  return false;
}
