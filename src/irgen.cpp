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
llvm::Type *IRGen::llvmTy(const Type &t) {
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
        llvm::Type *data = llvm::ArrayType::get(b_.getInt8Ty(), t.len);
        return llvm::StructType::get(b_.getInt32Ty(), data);
      }
      return llvm::ArrayType::get(b_.getInt8Ty(), t.len);
    }
    case TK::Void:
      return b_.getVoidTy();
  }
  return b_.getInt32Ty();
}

llvm::Value *IRGen::i32(int v) { return b_.getInt32(v); }
llvm::Value *IRGen::i64(long long v) { return b_.getInt64(v); }
llvm::Value *IRGen::flt(double d) {
  return llvm::ConstantFP::get(b_.getDoubleTy(), d);
}

llvm::AllocaInst *IRGen::entryAlloca(llvm::Type *ty, const llvm::Twine &name) {
  llvm::IRBuilder<> ab(&curFn_->getEntryBlock(), curFn_->getEntryBlock().begin());
  return ab.CreateAlloca(ty, nullptr, name);
}

static bool blockTerminated(llvm::BasicBlock *bb) {
  return bb && !bb->empty() && bb->back().isTerminator();
}

void IRGen::startBlock(llvm::BasicBlock *bb) {
  if (b_.GetInsertBlock() && !blockTerminated(b_.GetInsertBlock())) {
    // The current block is open (not terminated): fall through into bb.
    b_.CreateBr(bb);
  }
  b_.SetInsertPoint(bb);
}

void IRGen::newBlock() {
  llvm::BasicBlock *bb = llvm::BasicBlock::Create(ctx_, "", curFn_);
  b_.SetInsertPoint(bb);
}

void IRGen::branch(llvm::BasicBlock *target) {
  if (!blockTerminated(b_.GetInsertBlock())) {
    b_.CreateBr(target);
  }
}

llvm::GlobalVariable *IRGen::globalString(const std::string &s) {
  std::string want = s.empty() ? " " : s;
  auto *init = llvm::ConstantDataArray::getString(ctx_, want, false);
  for (auto *g : strLits_)
    if (g->getInitializer() == init) return g;
  auto *g = new llvm::GlobalVariable(
      mod_, init->getType(), true, llvm::GlobalValue::PrivateLinkage, init,
      "str." + std::to_string(strLits_.size()));
  strLits_.push_back(g);
  return g;
}

// Get (or create) a declaration for a runtime `pli_*` function.
llvm::Function *IRGen::runtimeFn(const std::string &name, llvm::Type *ret,
                                 std::vector<llvm::Type *> args, bool vararg) {
  llvm::FunctionType *ft = llvm::FunctionType::get(ret, args, vararg);
  llvm::Function *f = mod_.getFunction(name);
  if (f) return f;
  return llvm::Function::Create(ft, llvm::Function::ExternalLinkage, name, &mod_);
}

// Resolve the LLVM function a call targets. External C entries (rule (38)) have
// no PL/I body, so no function is pre-declared; declare it on demand. Every
// PL/I argument is passed by reference, so all parameters are pointers.
llvm::Function *IRGen::calleeFn(Symbol *sym) {
  Proc *callee = sym->proc;
  Stmt *en = sym->entry;
  std::string name;
  if (en)
    name = entryIrName(callee, en).substr(1);
  else if (callee)
    name = callee->irName.substr(1);
  else if (sym->isEntry)
    name = sym->irName.substr(1);
  else
    return nullptr;

  llvm::Function *f = mod_.getFunction(name);
  if (f) return f;
  if (!sym->isEntry) return f;  // internal callee missing a declaration is a bug

  std::vector<llvm::Type *> pt;
  for (size_t i = 0; i < sym->entryParams.size(); ++i) pt.push_back(b_.getPtrTy());
  llvm::FunctionType *ft = llvm::FunctionType::get(b_.getVoidTy(), pt, false);
  return llvm::Function::Create(ft, llvm::Function::ExternalLinkage, name, &mod_);
}

// ---------------------------------------------------------------------------
// module
// ---------------------------------------------------------------------------
std::string IRGen::run(Program &prog) {
  // Reject unsupported signatures before constructing a partial module.
  for (auto &p : prog.procs) {
    if (p->isFunction && p->retTy.isChar())
      d_.error(p->loc, "character-valued functions are not implemented in this stage", "(34)");
  }
  if (prog.mainProc && !prog.mainProc->params.empty())
    d_.error(prog.mainProc->loc,
             "parameters on the MAIN procedure are not implemented in this stage", "(2)");
  if (!d_.ok()) return "";

  emitGlobals();

  // Pre-declare every procedure's functions/aliases so a call site resolves
  // regardless of the order the (flattened) procedures are emitted in.
  for (auto &p : prog.procs) declareProc(p.get());
  for (auto &p : prog.procs) emitProc(p.get());

  // C entry point: initialise the runtime, invoke the MAIN procedure,
  // terminate normally (this is where FINISH would be raised, see M5).
  if (prog.mainProc) {
    llvm::Function *main =
        llvm::Function::Create(llvm::FunctionType::get(b_.getInt32Ty(), false),
                               llvm::Function::ExternalLinkage, "main", &mod_);
    llvm::BasicBlock *bb = llvm::BasicBlock::Create(ctx_, "entry", main);
    b_.SetInsertPoint(bb);
    b_.CreateCall(runtimeFn("pli_rt_init", b_.getVoidTy(), {}), {});
    llvm::Function *mfn = mod_.getFunction(prog.mainProc->irName.substr(1));
    if (!mfn) {
      mfn = llvm::Function::Create(llvm::FunctionType::get(b_.getVoidTy(), false),
                                   llvm::Function::InternalLinkage,
                                   prog.mainProc->irName.substr(1), &mod_);
      llvm::BasicBlock *mb = llvm::BasicBlock::Create(ctx_, "entry", mfn);
      b_.SetInsertPoint(mb);
      b_.CreateRetVoid();
    }
    b_.SetInsertPoint(bb);
    b_.CreateCall(mfn, {});
    b_.CreateCall(runtimeFn("pli_rt_fini", b_.getVoidTy(), {}), {});
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
  if (!triple_.empty()) mod_.setTargetTriple(llvm::Triple(triple_));
  mod_.print(os, nullptr);
  return ir;
}

void IRGen::emitGlobals() {
  for (Symbol *s : sema_.storage()) {
    if (!s->isStatic || s->kind != Symbol::Var) continue;
    const Type &t = s->ty;
    const Expr *ini = s->initExpr;  // INITIAL constant, rule (26)
    llvm::Constant *init = nullptr;
    switch (t.k) {
      case TK::FixedBin:
      case TK::FixedDec: {
        long long v = 0;
        if (ini) v = ini->kind == Expr::FltLit ? (long long)ini->fval
                   : ini->kind == Expr::BitLit ? (!ini->sval.empty() && ini->sval[0] == '1')
                   : ini->ival;
        init = llvm::ConstantInt::get(llvmTy(t), v, true);
        break;
      }
      case TK::Float: {
        double v = 0;
        if (ini) v = ini->kind == Expr::FltLit ? ini->fval : (double)ini->ival;
        init = llvm::ConstantFP::get(b_.getDoubleTy(), v);
        break;
      }
      case TK::Bit: {
        int v = 0;
        if (ini) v = ini->kind == Expr::BitLit ? (!ini->sval.empty() && ini->sval[0] == '1')
                   : (ini->ival != 0 || ini->fval != 0);
        init = llvm::ConstantInt::get(b_.getInt8Ty(), v);
        break;
      }
      case TK::Char: {
        std::string text(t.len, ' ');
        if (ini) {
          for (int i = 0; i < t.len && i < (int)ini->sval.size(); ++i) text[i] = ini->sval[i];
        }
        llvm::Constant *data = llvm::ConstantDataArray::getString(ctx_, text, false);
        if (t.varying) {
          size_t cur = ini ? std::min<size_t>(ini->sval.size(), (size_t)t.len) : 0;
          init = llvm::ConstantStruct::get(
              llvm::cast<llvm::StructType>(llvmTy(t)),
              llvm::ConstantInt::get(b_.getInt32Ty(), cur), data);
        } else {
          init = data;
        }
        break;
      }
      case TK::Void: continue;
    }
    auto *g = new llvm::GlobalVariable(mod_, llvmTy(t), false,
                                       llvm::GlobalValue::InternalLinkage, init,
                                       s->irName.substr(1));
    symAddr_[s] = g;
  }
}

llvm::Value *IRGen::addressOf(Symbol *sym) {
  // rule (8): an enclosing variable is reached through this frame's static
  // link; otherwise it is this frame's own storage (globals / allocas /
  // parameters). Both are recorded in symAddr_.
  return symAddr_.count(sym) ? symAddr_[sym] : nullptr;
}

void IRGen::allocaLocals(Proc *p) {
  for (Symbol *s : p->localSyms) {
    if (s->kind != Symbol::Var) continue;
    llvm::Value *a = entryAlloca(llvmTy(s->ty), s->irName.substr(1));
    symAddr_[s] = a;
    if (s->ty.isChar()) {  // blank fill
      std::string blanks(s->ty.len, ' ');
      llvm::Value *g = globalString(blanks);
      if (s->ty.varying) {
        llvm::Value *lenp = b_.CreateStructGEP(llvmTy(s->ty), a, 0, "lenp");
        b_.CreateStore(i32(0), lenp);
      } else {
        b_.CreateCall(runtimeFn("pli_assign_char", b_.getVoidTy(),
                                {b_.getPtrTy(), b_.getInt64Ty(), b_.getPtrTy(), b_.getInt64Ty()}),
                      {a, i64(s->ty.len), g, i64(0)});
      }
    }
  }
}

// INITIAL attribute on AUTOMATIC variables (rule 26): runs on every
// activation.
static void collectDeclStmts(Stmt *s, std::vector<Stmt *> &out) {
  if (!s) return;
  if (s->kind == Stmt::Declare) { out.push_back(s); return; }
  if (s->kind == Stmt::Begin || s->kind == Stmt::Group)
    for (auto &b : s->body) collectDeclStmts(b.get(), out);
  else {
    for (auto &b : s->body) collectDeclStmts(b.get(), out);
    collectDeclStmts(s->thenS.get(), out);
    collectDeclStmts(s->elseS.get(), out);
  }
}

void IRGen::emitInitials(Proc *p) {
  std::vector<Stmt *> decls;
  for (auto &st : p->body) collectDeclStmts(st.get(), decls);
  for (Stmt *st : decls) {
    for (auto &item : st->decls) {
      Expr *e = item.sym ? item.sym->initExpr : nullptr;
      if (!e) continue;
      Val v;
      v.ty = item.sym->ty;
      switch (item.sym->ty.k) {
        case TK::Float: {
          double d = e->kind == Expr::FltLit ? e->fval : (double)e->ival;
          v.reg = flt(d);
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
        default: {  // Fixed
          long long val = e->kind == Expr::FltLit ? (long long)e->fval : e->ival;
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
void IRGen::collectGotoBlocks(Stmt *s) {
  if (!s) return;
  if (!s->labels.empty() && s->kind != Stmt::Entry) {
    std::string blk = "L" + std::to_string(n_++);
    llvm::BasicBlock *bb = llvm::BasicBlock::Create(ctx_, blk, curFn_);
    for (const std::string &l : s->labels) labelBlocks_[l] = bb;
  }
  if (s->thenS) collectGotoBlocks(s->thenS.get());
  if (s->elseS) collectGotoBlocks(s->elseS.get());
  for (auto &b : s->body) collectGotoBlocks(b.get());
}

// Pre-create a procedure's functions and aliases so a call site resolves
// regardless of emission order. Plain procedures get one function (filled by
// emitPlainProc); multi-entry procedures get the shared impl (filled by
// emitMultiEntryProc) plus a fully-built tail-calling thunk per entry name.
void IRGen::declareProc(Proc *p) {
  llvm::Type *ret = p->isFunction ? llvmTy(p->retTy) : b_.getVoidTy();
  std::vector<Stmt *> entries;
  for (auto &st : p->body)
    if (st && st->kind == Stmt::Entry) entries.push_back(st.get());
  auto aliasFor = [&](const std::string &en, llvm::Function *target) {
    std::string alias = "PLI_" + (p->parent ? p->parent->name + "$" : std::string()) + en;
    llvm::GlobalAlias::create(llvm::GlobalValue::InternalLinkage, alias, target);
  };

  if (entries.empty()) {
    std::vector<llvm::Type *> pt;
    for (size_t i = 0; i < p->paramSyms.size(); ++i) pt.push_back(b_.getPtrTy());
    for (size_t i = 0; i < p->env.size(); ++i) pt.push_back(b_.getPtrTy());  // links
    llvm::FunctionType *ft = llvm::FunctionType::get(ret, pt, false);
    llvm::Function *fn = llvm::Function::Create(ft, llvm::Function::InternalLinkage,
                                                p->irName.substr(1), &mod_);
    for (const auto &en : p->entryNames) aliasFor(en, fn);
    return;
  }

  // Multi-entry: the union of every entry point's parameters.
  std::vector<Symbol *> uni;
  auto push = [&](Symbol *s) {
    if (std::find(uni.begin(), uni.end(), s) == uni.end()) uni.push_back(s);
  };
  for (Symbol *s : p->paramSyms) push(s);
  for (Stmt *e : entries)
    for (Symbol *s : e->entryParamSyms) push(s);

  // Shared implementation (body filled by emitMultiEntryProc).
  std::vector<llvm::Type *> pt;
  for (size_t i = 0; i < uni.size(); ++i) pt.push_back(b_.getPtrTy());
  for (size_t i = 0; i < p->env.size(); ++i) pt.push_back(b_.getPtrTy());
  pt.push_back(b_.getInt64Ty());  // the entry selector
  llvm::FunctionType *ift = llvm::FunctionType::get(ret, pt, false);
  llvm::Function *impl = llvm::Function::Create(ift, llvm::Function::InternalLinkage,
                                                p->irName.substr(1) + ".impl", &mod_);

  // A thunk marshals one entry's arguments and tail-calls the shared impl.
  int thunkN = 0;
  auto thunk = [&](const std::vector<Symbol *> &mine, llvm::Value *selv) {
    std::vector<llvm::Type *> sig;
    for (size_t i = 0; i < mine.size(); ++i) sig.push_back(b_.getPtrTy());
    for (size_t i = 0; i < p->env.size(); ++i) sig.push_back(b_.getPtrTy());  // links
    llvm::FunctionType *tft = llvm::FunctionType::get(ret, sig, false);
    llvm::Function *tf = llvm::Function::Create(tft, llvm::Function::InternalLinkage,
                                                "entry.thunk." + std::to_string(thunkN++), &mod_);
    llvm::BasicBlock *tb = llvm::BasicBlock::Create(ctx_, "entry", tf);
    b_.SetInsertPoint(tb);
    std::vector<llvm::Value *> args;
    size_t targ = 0;
    std::unordered_map<Symbol *, llvm::Value *> mineAddr;
    for (Symbol *s : mine) mineAddr[s] = tf->getArg(targ++);
    std::vector<llvm::Value *> links;
    for (size_t i = 0; i < p->env.size(); ++i) links.push_back(tf->getArg(targ++));
    for (Symbol *u : uni)
      args.push_back(mineAddr.count(u) ? mineAddr[u]
                                       : llvm::UndefValue::get(b_.getPtrTy()));
    for (auto *l : links) args.push_back(l);
    args.push_back(selv);
    llvm::CallInst *call = b_.CreateCall(ift, impl, args);
    call->setTailCall(true);
    if (ret->isVoidTy()) b_.CreateRetVoid();
    else b_.CreateRet(call);
    return tf;
  };

  llvm::Function *t0 = thunk(p->paramSyms, i64(0));
  t0->setName(p->irName.substr(1));
  for (const auto &en : p->entryNames) aliasFor(en, t0);
  for (size_t i = 0; i < entries.size(); ++i) {
    llvm::Function *tf = thunk(entries[i]->entryParamSyms, i64(i + 1));
    tf->setName(entryIrName(p, entries[i]).substr(1));
  }
}

void IRGen::emitProc(Proc *p) {
  curProc_ = p;
  labelBlocks_.clear();
  symAddr_.clear();
  // Re-seed globals: static storage resolves the same in every procedure.
  for (Symbol *s : sema_.storage())
    if (s->isStatic && s->kind == Symbol::Var)
      symAddr_[s] = mod_.getGlobalVariable(s->irName.substr(1), true);

  llvm::Type *retLLVM = p->isFunction ? llvmTy(p->retTy) : b_.getVoidTy();

  // rule (56): ENTRY statements declare alternate entry points.
  std::vector<Stmt *> entries;
  for (auto &st : p->body)
    if (st && st->kind == Stmt::Entry) entries.push_back(st.get());

  if (entries.empty())
    emitPlainProc(p, retLLVM);
  else
    emitMultiEntryProc(p, entries, retLLVM);
  curProc_ = nullptr;
}

// One procedure, one LLVM function: the ordinary path (no ENTRY statements).
// The function and its entry-namelist aliases were pre-created by declareProc.
void IRGen::emitPlainProc(Proc *p, llvm::Type *retLLVM) {
  llvm::Function *fn = mod_.getFunction(p->irName.substr(1));
  curFn_ = fn;
  // The entry block must be the function's first block (so it is the real
  // entry, and the allocas it holds dominate every reachable block).
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx_, "entry", fn);
  for (auto &st : p->body) collectGotoBlocks(st.get());

  // Parameter arguments become their symbols' addresses (PL/I by reference);
  // the trailing args are the static links (rule (8)).
  size_t ai = 0;
  for (Symbol *s : p->paramSyms) symAddr_[s] = fn->getArg(ai++);
  for (size_t i = 0; i < p->env.size(); ++i) symAddr_[p->env[i]] = fn->getArg(ai++);

  b_.SetInsertPoint(entry);

  allocaLocals(p);
  emitInitials(p);  // INITIAL attribute on AUTOMATIC variables (rule 26)

  for (auto &st : p->body) emitStmt(st.get());

  if (!blockTerminated(b_.GetInsertBlock())) {
    if (p->isFunction)
      b_.CreateRet(llvm::Constant::getNullValue(retLLVM));  // fall-off: return a zero value
    else
      b_.CreateRetVoid();
  }
  curFn_ = nullptr;
}

// rule (56): a procedure with ENTRY statements. The shared implementation
// function (pre-created by declareProc) carries the whole body split into
// segments; each entry point's thunk was built by declareProc.
void IRGen::emitMultiEntryProc(Proc *p, const std::vector<Stmt *> &entries, llvm::Type *retLLVM) {
  llvm::Function *impl = mod_.getFunction(p->irName.substr(1) + ".impl");
  curFn_ = impl;
  // The entry block must be the function's first block (so it is the real
  // entry and its allocas dominate every reachable segment block).
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx_, "entry", impl);
  for (auto &st : p->body) collectGotoBlocks(st.get());

  std::vector<Symbol *> uni;
  auto push = [&](Symbol *s) {
    if (std::find(uni.begin(), uni.end(), s) == uni.end()) uni.push_back(s);
  };
  for (Symbol *s : p->paramSyms) push(s);
  for (Stmt *e : entries)
    for (Symbol *s : e->entryParamSyms) push(s);

  size_t ai = 0;
  for (Symbol *s : uni) symAddr_[s] = impl->getArg(ai++);
  for (size_t i = 0; i < p->env.size(); ++i) symAddr_[p->env[i]] = impl->getArg(ai++);
  llvm::Value *sel = impl->getArg(ai++);

  b_.SetInsertPoint(entry);

  allocaLocals(p);
  emitInitials(p);

  // Entry selector: dispatch to the segment each call entered through.
  std::vector<llvm::BasicBlock *> segs;
  for (size_t i = 0; i <= entries.size(); ++i) {
    llvm::BasicBlock *bb = llvm::BasicBlock::Create(ctx_, "e.seg." + std::to_string(i), impl);
    segs.push_back(bb);
  }
  for (size_t i = 0; i < entries.size(); ++i)
    for (const std::string &label : entries[i]->labels)
      labelBlocks_[label] = segs[i + 1];
  llvm::SwitchInst *sw = b_.CreateSwitch(sel, segs[0], entries.size());
  for (size_t i = 0; i < entries.size(); ++i)
    sw->addCase(llvm::ConstantInt::get(b_.getInt64Ty(), i + 1), segs[i + 1]);

  // Segment 0 is the procedure's own start; each ENTRY begins the next segment.
  size_t seg = 0;
  startBlock(segs[0]);
  for (auto &st : p->body) {
    if (st && st->kind == Stmt::Entry) {
      ++seg;
      startBlock(segs[seg]);  // fall through from the previous segment
      continue;
    }
    emitStmt(st.get());
  }
  if (!blockTerminated(b_.GetInsertBlock())) {
    if (p->isFunction) b_.CreateRet(llvm::Constant::getNullValue(retLLVM));
    else b_.CreateRetVoid();
  }
  curFn_ = nullptr;
}

std::string IRGen::entryIrName(Proc *p, Stmt *e) {
  return "@PLI_" + (p->parent ? p->parent->name + "$" : std::string()) + p->name +
         "$entry$" + e->name;
}

// ---------------------------------------------------------------------------
// statements
// ---------------------------------------------------------------------------
void IRGen::emitStmt(Stmt *s) {
  if (!s) return;
  if (!s->labels.empty()) {
    startBlock(labelBlocks_[s->labels.front()]);
  } else if (blockTerminated(b_.GetInsertBlock())) {
    newBlock();  // unreachable code (e.g. after STOP): start a fresh block
  }
  switch (s->kind) {
    case Stmt::Null:
    case Stmt::Declare:
    case Stmt::Entry:  // segment marker; handled by emitMultiEntryProc
      break;
    case Stmt::Assign: emitAssign(s); break;
    case Stmt::If: emitIf(s); break;
    case Stmt::Group:
    case Stmt::Begin:  // a block executes its body as a group (rule (68))
      for (auto &b : s->body) emitStmt(b.get());
      break;
    case Stmt::DoWhile: emitDoWhile(s); break;
    case Stmt::DoIter: emitDoIter(s); break;
    case Stmt::Put: emitPut(s); break;
    case Stmt::CallS: emitCall(s); break;
    case Stmt::Return: {
      if (curProc_->isFunction) {
        Val v = emitExpr(s->value.get());
        Val rv = convert(v, curProc_->retTy, s->loc);
        llvm::Value *reg = rv.reg;
        if (curProc_->retTy.isBit()) {  // BIT returns are held in i8
          reg = b_.CreateZExt(reg, b_.getInt8Ty(), "retz");
        }
        b_.CreateRet(reg);
      } else {
        b_.CreateRetVoid();
      }
      break;
    }
    case Stmt::Stop:
      b_.CreateCall(runtimeFn("pli_stop", b_.getVoidTy(), {}), {});
      b_.CreateUnreachable();
      break;
    case Stmt::Leave:
      break;
    case Stmt::Goto:
      b_.CreateBr(labelBlocks_[s->name]);
      break;
  }
}

void IRGen::emitAssign(Stmt *s) {
  if (!s->target) return;
  if (s->target->kind == Expr::Call && s->target->name == "SUBSTR") {
    Expr *t = s->target.get();
    Val sv = emitExpr(t->args[0].get());
    Symbol *sym = t->args[0]->sym;
    Val start = emitExpr(t->args[1].get());
    Val len = emitExpr(t->args[2].get());
    Val rhs = emitExpr(s->value.get());
    b_.CreateCall(runtimeFn("pli_substr_assign", b_.getVoidTy(),
                            {b_.getPtrTy(), b_.getInt64Ty(), b_.getInt64Ty(),
                             b_.getInt64Ty(), b_.getPtrTy(), b_.getInt64Ty()}),
                  {sv.ptr, i64(sym->ty.len), toI64(start), toI64(len), rhs.ptr, rhs.len});
    return;
  }
  if (s->target->kind != Expr::VarRef || !s->target->sym) return;
  Val v = emitExpr(s->value.get());
  storeTo(s->target->sym, v, s->loc);
}

void IRGen::emitIf(Stmt *s) {
  Val c = emitExpr(s->cond.get());
  llvm::Value *cond = toI1(c, s->loc);
  std::string id = std::to_string(n_++);
  llvm::BasicBlock *thenL = llvm::BasicBlock::Create(ctx_, "if.then." + id, curFn_);
  llvm::BasicBlock *endL = llvm::BasicBlock::Create(ctx_, "if.end." + id, curFn_);
  llvm::BasicBlock *elseL = s->elseS ? llvm::BasicBlock::Create(ctx_, "if.else." + id, curFn_) : nullptr;
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

void IRGen::emitDoWhile(Stmt *s) {
  std::string id = std::to_string(n_++);
  llvm::BasicBlock *condL = llvm::BasicBlock::Create(ctx_, "do.cond." + id, curFn_);
  llvm::BasicBlock *bodyL = llvm::BasicBlock::Create(ctx_, "do.body." + id, curFn_);
  llvm::BasicBlock *endL = llvm::BasicBlock::Create(ctx_, "do.end." + id, curFn_);
  branch(condL);
  startBlock(condL);
  Val c = emitExpr(s->cond.get());
  b_.CreateCondBr(toI1(c, s->loc), bodyL, endL);
  startBlock(bodyL);
  for (auto &b : s->body) emitStmt(b.get());
  branch(condL);
  startBlock(endL);
}

// DO v = e1 [TO e2] [BY e3] [WHILE(e4)];                    rules (71)-(73)
void IRGen::emitDoIter(Stmt *s) {
  Symbol *ctl = s->sym;
  if (!ctl) return;
  const Type &ct = ctl->ty;
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
    by.reg = ct.k == TK::Float ? flt(1.0)
                               : llvm::ConstantInt::get(llvmTy(ct), 1, true);
  }
  byAddr = entryAlloca(llvmTy(ct), "do.by." + id);
  storeScalarTo(byAddr, ct, by);

  llvm::BasicBlock *condL = llvm::BasicBlock::Create(ctx_, "do.cond." + id, curFn_);
  llvm::BasicBlock *bodyL = llvm::BasicBlock::Create(ctx_, "do.body." + id, curFn_);
  llvm::BasicBlock *stepL = llvm::BasicBlock::Create(ctx_, "do.step." + id, curFn_);
  llvm::BasicBlock *endL = llvm::BasicBlock::Create(ctx_, "do.end." + id, curFn_);
  llvm::BasicBlock *upL = s->to
      ? llvm::BasicBlock::Create(ctx_, "do.up." + id, curFn_) : nullptr;
  llvm::BasicBlock *downL = s->to
      ? llvm::BasicBlock::Create(ctx_, "do.down." + id, curFn_) : nullptr;
  llvm::BasicBlock *testL = s->to
      ? llvm::BasicBlock::Create(ctx_, "do.test." + id, curFn_) : nullptr;

  branch(condL);
  startBlock(condL);

  if (s->to) {
    Val cv = loadSym(ctl, ct);
    llvm::Value *tv = b_.CreateLoad(llvmTy(ct), toAddr, "to");
    llvm::Value *bv = b_.CreateLoad(llvmTy(ct), byAddr, "by");
    llvm::Value *zero = ct.k == TK::Float ? flt(0.0)
                                          : llvm::Constant::getNullValue(llvmTy(ct));
    llvm::Value *neg;
    if (ct.k == TK::Float)
      neg = b_.CreateFCmpOLT(bv, zero, "negstep");
    else
      neg = b_.CreateICmpSLT(bv, zero, "negstep");
    b_.CreateCondBr(neg, downL, upL);

    startBlock(upL);
    llvm::Value *cu = ct.k == TK::Float ? b_.CreateFCmpOLE(cv.reg, tv, "cmpup")
                                        : b_.CreateICmpSLE(cv.reg, tv, "cmpup");
    b_.CreateCondBr(cu, testL, endL);

    startBlock(downL);
    llvm::Value *cd = ct.k == TK::Float ? b_.CreateFCmpOGE(cv.reg, tv, "cmpdn")
                                        : b_.CreateICmpSGE(cv.reg, tv, "cmpdn");
    b_.CreateCondBr(cd, testL, endL);

    startBlock(testL);
  }

  if (s->cond) {  // WHILE clause
    Val w = emitExpr(s->cond.get());
    b_.CreateCondBr(toI1(w, s->loc), bodyL, endL);
  } else {
    branch(bodyL);
  }

  startBlock(bodyL);
  for (auto &b : s->body) emitStmt(b.get());
  branch(stepL);

  startBlock(stepL);
  Val cv = loadSym(ctl, ct);
  llvm::Value *bv = b_.CreateLoad(llvmTy(ct), byAddr, "by");
  llvm::Value *nx = ct.k == TK::Float ? b_.CreateFAdd(cv.reg, bv, "next")
                                      : b_.CreateAdd(cv.reg, bv, "next");
  Val nv;
  nv.ty = ct;
  nv.reg = nx;
  storeTo(ctl, nv, s->loc);
  branch(condL);

  startBlock(endL);
}

void IRGen::emitPut(Stmt *s) {
  if (s->page)
    b_.CreateCall(runtimeFn("pli_put_page", b_.getVoidTy(), {}), {});
  if (s->skip) {
    llvm::Value *n = i64(1);
    if (s->skipCount) {
      Val v = emitExpr(s->skipCount.get());
      n = toI64(v);
    }
    b_.CreateCall(runtimeFn("pli_put_skip", b_.getVoidTy(), {b_.getInt64Ty()}), {n});
  }
  for (auto &item : s->items) {
    Val v = emitExpr(item.get());
    switch (v.ty.k) {
      case TK::Char:
        b_.CreateCall(runtimeFn("pli_put_list_char", b_.getVoidTy(),
                                {b_.getPtrTy(), b_.getInt64Ty()}),
                      {v.ptr, v.len});
        break;
      case TK::Float:
        b_.CreateCall(runtimeFn("pli_put_list_float", b_.getVoidTy(),
                                {b_.getDoubleTy()}),
                      {v.reg});
        break;
      case TK::Bit: {
        llvm::Value *bit = b_.CreateZExt(v.reg, b_.getInt8Ty(), "bit");
        b_.CreateCall(runtimeFn("pli_put_list_bit", b_.getVoidTy(), {b_.getInt8Ty()}),
                      {bit});
        break;
      }
      case TK::FixedBin:
      case TK::FixedDec:
        b_.CreateCall(runtimeFn("pli_put_list_fixed", b_.getVoidTy(),
                                {b_.getInt64Ty()}),
                      {toI64(v)});
        break;
      case TK::Void:
        break;
    }
  }
}

void IRGen::emitCall(Stmt *s) {
  if (!s->sym) return;
  Symbol *calleeSym = s->sym;
  Proc *callee = calleeSym->proc;
  Stmt *en = calleeSym->entry;
  llvm::Function *calleeF = calleeFn(calleeSym);
  std::vector<Symbol *> calleeParams =
      en ? en->entryParamSyms : (callee ? callee->paramSyms : std::vector<Symbol *>());

  std::vector<llvm::Value *> args;
  for (size_t i = 0; i < s->args.size(); ++i) {
    Expr *a = s->args[i].get();
    Type pty;
    if (en || callee) {
      if (i < calleeParams.size()) pty = calleeParams[i]->ty;
      else break;
    } else {
      if (i < calleeSym->entryParams.size()) pty = calleeSym->entryParams[i];
      else break;
    }
    llvm::Value *addr;
    bool direct = a->kind == Expr::VarRef && a->sym && a->sym->kind != Symbol::ProcName &&
                  a->sym->ty.k == pty.k && a->sym->ty.len == pty.len &&
                  a->sym->ty.prec == pty.prec && a->sym->ty.varying == pty.varying;
    if (direct) {
      addr = addressOf(a->sym);
    } else {
      addr = entryAlloca(llvmTy(pty), "dummy");
      Val v = emitExpr(a);
      if (pty.isChar()) {
        Val cv = v;
        if (pty.varying) {
          llvm::Value *dp = b_.CreateStructGEP(llvmTy(pty), addr, 1, "vdata");
          llvm::Value *ln = b_.CreateCall(runtimeFn("pli_assign_varying", b_.getInt64Ty(),
                                                    {b_.getPtrTy(), b_.getInt64Ty(),
                                                     b_.getPtrTy(), b_.getInt64Ty()}),
                                          {dp, i64(pty.len), cv.ptr, cv.len});
          llvm::Value *lp = b_.CreateStructGEP(llvmTy(pty), addr, 0, "vlenp");
          b_.CreateStore(b_.CreateTrunc(ln, b_.getInt32Ty(), "l32"), lp);
        } else {
          b_.CreateCall(runtimeFn("pli_assign_char", b_.getVoidTy(),
                                  {b_.getPtrTy(), b_.getInt64Ty(), b_.getPtrTy(), b_.getInt64Ty()}),
                        {addr, i64(pty.len), cv.ptr, cv.len});
        }
      } else {
        Val cv = convert(v, pty, a->loc);
        storeScalarTo(addr, pty, cv);
      }
    }
    args.push_back(addr);
  }
  // rule (8): pass the callee's static links (enclosing automatic variables).
  if (callee) {
    for (Symbol *v : callee->env) {
      // "cousin" call: resolve via addressOf; for a non-adjacent variable
      // so diagnose it (same rule as before).
      if (v->owner != curProc_ &&
          std::find(curProc_->env.begin(), curProc_->env.end(), v) == curProc_->env.end()) {
        d_.error(callee->loc,
                 "a sibling internal procedure reaching a non-adjacent enclosing variable is not implemented in this stage", "(8)");
        continue;
      }
      args.push_back(addressOf(v));
    }
  }
  b_.CreateCall(calleeF, args);
}

// ---------------------------------------------------------------------------
// loads / stores
// ---------------------------------------------------------------------------
Val IRGen::loadSym(Symbol *sym, const Type &ty) {
  Val v;
  v.ty = ty;
  llvm::Value *addr = addressOf(sym);
  if (ty.isChar()) {
    if (ty.varying) {
      llvm::Value *dp = b_.CreateStructGEP(llvmTy(ty), addr, 1, "vdata");
      llvm::Value *lp = b_.CreateStructGEP(llvmTy(ty), addr, 0, "vlenp");
      llvm::Value *l32 = b_.CreateLoad(b_.getInt32Ty(), lp, "l32");
      v.ptr = dp;
      v.len = b_.CreateSExt(l32, b_.getInt64Ty(), "l64");
    } else {
      v.ptr = addr;
      v.len = i64(ty.len);
    }
    return v;
  }
  llvm::Value *r = b_.CreateLoad(llvmTy(ty), addr, "ld");
  if (ty.isBit()) {
    v.reg = b_.CreateTrunc(r, b_.getInt1Ty(), "b1");
  } else {
    v.reg = r;
  }
  return v;
}

void IRGen::storeScalarTo(llvm::Value *addr, const Type &ty, const Val &v) {
  llvm::Value *val = v.reg;
  if (ty.isBit()) {
    val = b_.CreateZExt(val, b_.getInt8Ty(), "z8");
  }
  b_.CreateStore(val, addr);
}

void IRGen::storeTo(Symbol *sym, const Val &v, SourceLoc loc) {
  const Type &dt = sym->ty;
  llvm::Value *addr = addressOf(sym);
  if (dt.isChar()) {
    if (!v.ty.isChar()) {
      d_.error(loc, "conversion from " + v.ty.desc() + " to " + dt.desc() +
               " is not implemented in this stage", "(86)");
      return;
    }
    if (dt.varying) {
      llvm::Value *dp = b_.CreateStructGEP(llvmTy(dt), addr, 1, "vdata");
      llvm::Value *ln = b_.CreateCall(runtimeFn("pli_assign_varying", b_.getInt64Ty(),
                                                {b_.getPtrTy(), b_.getInt64Ty(),
                                                 b_.getPtrTy(), b_.getInt64Ty()}),
                                      {dp, i64(dt.len), v.ptr, v.len});
      llvm::Value *lp = b_.CreateStructGEP(llvmTy(dt), addr, 0, "vlenp");
      b_.CreateStore(b_.CreateTrunc(ln, b_.getInt32Ty(), "l32"), lp);
    } else {
      b_.CreateCall(runtimeFn("pli_assign_char", b_.getVoidTy(),
                              {b_.getPtrTy(), b_.getInt64Ty(), b_.getPtrTy(), b_.getInt64Ty()}),
                    {addr, i64(dt.len), v.ptr, v.len});
    }
    return;
  }
  Val cv = convert(v, dt, loc);
  storeScalarTo(addr, dt, cv);
}

// ---------------------------------------------------------------------------
// conversions (the M0 subset of the PL/I conversion rules)
// ---------------------------------------------------------------------------
Val IRGen::convert(const Val &v, const Type &dst, SourceLoc loc) {
  Val out;
  out.ty = dst;
  if (v.ty.isChar() || dst.isChar()) {
    if (v.ty.isChar() && dst.isChar()) return v;
    d_.error(loc, "conversion between " + v.ty.desc() + " and " + dst.desc() +
             " is not implemented in this stage", "(86)");
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
  if (srcBit) {  // BIT -> arithmetic
    if (dstFloat) {
      llvm::Value *z = b_.CreateZExt(v.reg, b_.getInt32Ty(), "z");
      out.reg = b_.CreateSIToFP(z, b_.getDoubleTy(), "cvt");
    } else {
      out.reg = b_.CreateZExt(v.reg, llvmTy(dst), "cvt");
    }
    return out;
  }
  if (srcFloat && dstFloat) return v;
  if (srcFloat && !dstFloat) {  // FLOAT -> FIXED truncates toward zero
    out.reg = b_.CreateFPToSI(v.reg, llvmTy(dst), "cvt");
    return out;
  }
  if (!srcFloat && dstFloat) {
    out.reg = b_.CreateSIToFP(v.reg, b_.getDoubleTy(), "cvt");
    return out;
  }
  // FIXED -> FIXED: adjust width
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

llvm::Value *IRGen::toI1(const Val &v, SourceLoc loc) {
  if (v.ty.isBit()) return v.reg;
  if (v.ty.k == TK::Float)
    return b_.CreateFCmpUNE(v.reg, flt(0.0), "tst");
  if (v.ty.isNumeric())
    return b_.CreateICmpNE(v.reg, llvm::Constant::getNullValue(llvmTy(v.ty)), "tst");
  d_.error(loc, "value of type " + v.ty.desc() + " cannot be used as a condition", "(75)");
  return b_.getInt1(false);
}

llvm::Value *IRGen::toI64(const Val &v) {
  if (v.ty.k == TK::Float)
    return b_.CreateFPToSI(v.reg, b_.getInt64Ty(), "i64");
  if (v.ty.isBit())
    return b_.CreateZExt(v.reg, b_.getInt64Ty(), "i64");
  if (v.ty.intBits() == 64) return v.reg;
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
Val IRGen::emitExpr(Expr *e) {
  Val v;
  if (!e) return v;
  switch (e->kind) {
    case Expr::IntLit:
      v.ty = e->ty;
      v.reg = llvm::ConstantInt::get(llvmTy(e->ty), e->ival, true);
      return v;
    case Expr::FltLit:
      v.ty = e->ty;
      v.reg = flt(e->fval);
      return v;
    case Expr::CharLit: {
      v.ty = e->ty;
      v.ptr = globalString(e->sval);
      v.len = i64(e->sval.size());
      return v;
    }
    case Expr::BitLit:
      v.ty = Type::bit(1);
      v.reg = b_.getInt1(!e->sval.empty() && e->sval[0] == '1');
      return v;
    case Expr::VarRef:
      if (!e->sym) { v.ty = e->ty; v.reg = i64(0); return v; }
      return loadSym(e->sym, e->sym->ty);
    case Expr::Call: {
      if (e->name == "SUBSTR") {
        Val s = emitExpr(e->args[0].get());
        Val start = emitExpr(e->args[1].get());
        Val len = emitExpr(e->args[2].get());
        Val out = charTemp(e->ty.len);
        b_.CreateCall(runtimeFn("pli_substr", b_.getVoidTy(),
                                {b_.getPtrTy(), b_.getInt64Ty(), b_.getPtrTy(),
                                 b_.getInt64Ty(), b_.getInt64Ty(), b_.getInt64Ty()}),
                      {out.ptr, out.len, s.ptr, s.len, toI64(start), toI64(len)});
        out.len = i64(e->ty.len);
        return out;
      }
      if (e->name == "INDEX") {
        Val a = emitExpr(e->args[0].get());
        Val b = emitExpr(e->args[1].get());
        llvm::Value *r = b_.CreateCall(runtimeFn("pli_index", b_.getInt64Ty(),
                                                 {b_.getPtrTy(), b_.getInt64Ty(),
                                                  b_.getPtrTy(), b_.getInt64Ty()}),
                                       {a.ptr, a.len, b.ptr, b.len});
        v.ty = e->ty;
        v.reg = b_.CreateTrunc(r, b_.getInt32Ty(), "idx32");
        return v;
      }
      if (e->name == "ABS") {
        Val a = emitExpr(e->args[0].get());
        const Type &at = a.ty;
        llvm::Value *r;
        if (at.k == TK::Float) {
          r = b_.CreateCall(runtimeFn("llvm.fabs.f64", b_.getDoubleTy(), {b_.getDoubleTy()}),
                            {a.reg}, "abs");
        } else {
          llvm::Value *neg = b_.CreateSub(llvm::Constant::getNullValue(llvmTy(at)), a.reg, "absneg");
          llvm::Value *cmp = b_.CreateICmpSLT(a.reg, llvm::Constant::getNullValue(llvmTy(at)), "abscmp");
          r = b_.CreateSelect(cmp, neg, a.reg, "abs");
        }
        v.ty = e->ty;
        v.reg = r;
        return v;
      }
      if (e->name == "LENGTH") {
        Val a = emitExpr(e->args[0].get());
        v.ty = e->ty;
        v.reg = b_.CreateTrunc(a.len, b_.getInt32Ty(), "len32");
        return v;
      }
      if (e->name == "TRUNC") {
        Val a = emitExpr(e->args[0].get());
        if (a.ty.k == TK::Float) {
          llvm::Value *i = b_.CreateFPToSI(a.reg, b_.getInt64Ty(), "trunci");
          v.ty = e->ty;
          v.reg = b_.CreateSIToFP(i, b_.getDoubleTy(), "truncd");
        } else {
          v = a;
        }
        return v;
      }
      if (e->name == "PRECISION") {
        Val a = emitExpr(e->args[0].get());
        v = convert(a, e->ty, e->loc);
        return v;
      }
      if (e->name == "MIN" || e->name == "MAX") {
        Val a = emitExpr(e->args[0].get());
        Val b = emitExpr(e->args[1].get());
        const Type &common = e->ty;
        Val av = convert(a, common, e->loc);
        Val bv = convert(b, common, e->loc);
        llvm::Value *cmp = common.k == TK::Float
            ? b_.CreateFCmpOLT(av.reg, bv.reg, "mincmp")
            : b_.CreateICmpSLT(av.reg, bv.reg, "mincmp");
        llvm::Value *r = e->name == "MIN"
            ? b_.CreateSelect(cmp, av.reg, bv.reg, "min")
            : b_.CreateSelect(cmp, bv.reg, av.reg, "max");
        v.ty = common;
        v.reg = r;
        return v;
      }
      if (e->name == "MOD") {
        Val a = emitExpr(e->args[0].get());
        Val b = emitExpr(e->args[1].get());
        const Type &common = e->ty;
        Val av = convert(a, common, e->loc);
        Val bv = convert(b, common, e->loc);
        v.ty = common;
        if (common.k == TK::Float) {
          v.reg = b_.CreateCall(runtimeFn("pli_mod_dd", b_.getDoubleTy(),
                                          {b_.getDoubleTy(), b_.getDoubleTy()}),
                                {av.reg, bv.reg}, "mod");
        } else {
          llvm::Value *r = b_.CreateCall(runtimeFn("pli_mod_ll", b_.getInt64Ty(),
                                                   {b_.getInt64Ty(), b_.getInt64Ty()}),
                                         {toI64(av), toI64(bv)});
          v.reg = b_.CreateTrunc(r, b_.getInt32Ty(), "mod32");
        }
        return v;
      }
      if (e->name == "MULTIPLY") {
        Val a = emitExpr(e->args[0].get());
        Val b = emitExpr(e->args[1].get());
        const Type &common = e->ty;
        Val av = convert(a, common, e->loc);
        Val bv = convert(b, common, e->loc);
        llvm::Value *r = common.k == TK::Float ? b_.CreateFMul(av.reg, bv.reg, "mul")
                                               : b_.CreateMul(av.reg, bv.reg, "mul");
        v.ty = common;
        v.reg = r;
        return v;
      }
      if (e->name == "DIVIDE") {
        Val a = emitExpr(e->args[0].get());
        Val b = emitExpr(e->args[1].get());
        const Type &common = e->ty;
        Val av = convert(a, common, e->loc);
        Val bv = convert(b, common, e->loc);
        v.ty = common;
        v.reg = b_.CreateFDiv(av.reg, bv.reg, "div");
        return v;
      }
      if (e->name == "ROUND") {
        Val x = emitExpr(e->args[0].get());
        Val n = emitExpr(e->args[1].get());
        Val xd = convert(x, Type::flt(6), e->loc);
        v.ty = e->ty;
        v.reg = b_.CreateCall(runtimeFn("pli_round", b_.getDoubleTy(),
                                        {b_.getDoubleTy(), b_.getInt64Ty()}),
                              {xd.reg, toI64(n)}, "round");
        return v;
      }
      if (e->name == "REPEAT") {
        Val s = emitExpr(e->args[0].get());
        Val n = emitExpr(e->args[1].get());
        Val out = charTemp(e->ty.len);
        b_.CreateCall(runtimeFn("pli_repeat", b_.getVoidTy(),
                                {b_.getPtrTy(), b_.getInt64Ty(), b_.getPtrTy(),
                                 b_.getInt64Ty(), b_.getInt64Ty()}),
                      {out.ptr, out.len, s.ptr, s.len, toI64(n)});
        out.len = i64(e->ty.len);
        return out;
      }
      if (e->name == "VERIFY") {
        Val s = emitExpr(e->args[0].get());
        Val t = emitExpr(e->args[1].get());
        llvm::Value *r = b_.CreateCall(runtimeFn("pli_verify", b_.getInt64Ty(),
                                                 {b_.getPtrTy(), b_.getInt64Ty(),
                                                  b_.getPtrTy(), b_.getInt64Ty()}),
                                       {s.ptr, s.len, t.ptr, t.len});
        v.ty = e->ty;
        v.reg = b_.CreateTrunc(r, b_.getInt32Ty(), "ver32");
        return v;
      }
      if (e->name == "TRANSLATE") {
        Val s = emitExpr(e->args[0].get());
        Val out = emitExpr(e->args[1].get());
        Val in = emitExpr(e->args[2].get());
        Val dst = charTemp(e->ty.len);
        b_.CreateCall(runtimeFn("pli_translate", b_.getVoidTy(),
                                {b_.getPtrTy(), b_.getInt64Ty(), b_.getPtrTy(),
                                 b_.getInt64Ty(), b_.getPtrTy(), b_.getInt64Ty(),
                                 b_.getPtrTy(), b_.getInt64Ty()}),
                      {dst.ptr, dst.len, s.ptr, s.len, out.ptr, out.len, in.ptr, in.len});
        dst.len = i64(e->ty.len);
        return dst;
      }
      if (e->name == "HIGH" || e->name == "LOW") {
        Val n = emitExpr(e->args[0].get());
        Val out = charTemp(e->ty.len);
        std::string fn = e->name == "HIGH" ? "pli_high" : "pli_low";
        b_.CreateCall(runtimeFn(fn, b_.getVoidTy(), {b_.getPtrTy(), b_.getInt64Ty()}),
                      {out.ptr, toI64(n)});
        out.len = i64(e->ty.len);
        return out;
      }
      if (e->name == "DATE" || e->name == "TIME") {
        Val out = charTemp(e->ty.len);
        std::string fn = e->name == "DATE" ? "pli_date" : "pli_time";
        b_.CreateCall(runtimeFn(fn, b_.getVoidTy(), {b_.getPtrTy(), b_.getInt64Ty()}),
                      {out.ptr, out.len});
        out.len = i64(e->ty.len);
        return out;
      }
      if (!e->sym || !e->sym->proc) { v.ty = e->ty; v.reg = i64(0); return v; }
      Stmt *en = e->sym->entry;
      Proc *callee = e->sym->proc;
      Type rty = en ? (en->entryIsFunction ? en->entryRetTy : Type::voidTy()) : callee->retTy;
      llvm::Function *calleeFn = en
          ? mod_.getFunction(entryIrName(callee, en).substr(1))
          : mod_.getFunction(callee->irName.substr(1));
      std::vector<Symbol *> calleeParams = en ? en->entryParamSyms : callee->paramSyms;
      if (rty.isChar()) { v.ty = e->ty; v.reg = i64(0); return v; }  // diagnosed in emitProc
      std::vector<llvm::Value *> args;
      for (size_t i = 0; i < e->args.size(); ++i) {
        Expr *a = e->args[i].get();
        Type pty;
        if (i < calleeParams.size()) pty = calleeParams[i]->ty;
        else break;
        llvm::Value *addr;
        bool direct = a->kind == Expr::VarRef && a->sym && a->sym->kind != Symbol::ProcName &&
                      a->sym->ty.k == pty.k && a->sym->ty.len == pty.len &&
                      a->sym->ty.prec == pty.prec && a->sym->ty.varying == pty.varying;
        if (direct) {
          addr = addressOf(a->sym);
        } else {
          addr = entryAlloca(llvmTy(pty), "dummy");
          Val av = emitExpr(a);
          if (pty.isChar()) {
            Val cv = av;
            if (pty.varying) {
              llvm::Value *dp = b_.CreateStructGEP(llvmTy(pty), addr, 1, "vdata");
              llvm::Value *ln = b_.CreateCall(runtimeFn("pli_assign_varying", b_.getInt64Ty(),
                                                        {b_.getPtrTy(), b_.getInt64Ty(),
                                                         b_.getPtrTy(), b_.getInt64Ty()}),
                                              {dp, i64(pty.len), cv.ptr, cv.len});
              llvm::Value *lp = b_.CreateStructGEP(llvmTy(pty), addr, 0, "vlenp");
              b_.CreateStore(b_.CreateTrunc(ln, b_.getInt32Ty(), "l32"), lp);
            } else {
              b_.CreateCall(runtimeFn("pli_assign_char", b_.getVoidTy(),
                                      {b_.getPtrTy(), b_.getInt64Ty(), b_.getPtrTy(), b_.getInt64Ty()}),
                            {addr, i64(pty.len), cv.ptr, cv.len});
            }
          } else {
            Val cv = convert(av, pty, a->loc);
            storeScalarTo(addr, pty, cv);
          }
        }
        args.push_back(addr);
      }
      if (callee) {
        for (Symbol *v : callee->env) {
          if (v->owner != curProc_ &&
              std::find(curProc_->env.begin(), curProc_->env.end(), v) == curProc_->env.end()) {
            d_.error(callee->loc,
                     "a sibling internal procedure reaching a non-adjacent enclosing variable is not implemented in this stage", "(8)");
            continue;
          }
          args.push_back(addressOf(v));
        }
      }
      llvm::CallInst *call = b_.CreateCall(calleeFn, args, "fres");
      v.ty = rty;
      if (rty.isBit()) {
        v.reg = b_.CreateTrunc(call, b_.getInt1Ty(), "fb");
      } else {
        v.reg = call;
      }
      return v;
    }
    case Expr::Unary: {
      Val a = emitExpr(e->a.get());
      if (e->op == Tok::Not) {
        llvm::Value *bb = toI1(a, e->loc);
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
    case Expr::Binary: break;
  }

  // ---- binary operators ----
  const Tok op = e->op;

  if (op == Tok::Concat) {                                     // rule (119)
    Val a = emitExpr(e->a.get());
    Val b = emitExpr(e->b.get());
    if (!a.ty.isChar() || !b.ty.isChar()) { v.ty = e->ty; v.reg = i64(0); return v; }
    Val out = charTemp(e->ty.len);
    b_.CreateCall(runtimeFn("pli_concat", b_.getVoidTy(),
                            {b_.getPtrTy(), b_.getPtrTy(), b_.getInt64Ty(),
                             b_.getPtrTy(), b_.getInt64Ty()}),
                  {out.ptr, a.ptr, a.len, b.ptr, b.len});
    out.len = b_.CreateAdd(a.len, b.len, "clen");
    return out;
  }

  if (op == Tok::Amp || op == Tok::Bar) {                       // rules (116),(115)
    Val a = emitExpr(e->a.get());
    llvm::Value *ab = toI1(a, e->loc);
    Val b = emitExpr(e->b.get());
    llvm::Value *bb = toI1(b, e->loc);
    llvm::Value *r = op == Tok::Amp ? b_.CreateAnd(ab, bb, "and")
                                    : b_.CreateOr(ab, bb, "or");
    v.ty = Type::bit(1);
    v.reg = r;
    return v;
  }

  const bool isCmp = op == Tok::Eq || op == Tok::Ne || op == Tok::Lt || op == Tok::Le ||
                     op == Tok::Gt || op == Tok::Ge || op == Tok::Ngt || op == Tok::Nlt;

  Val a = emitExpr(e->a.get());
  Val b = emitExpr(e->b.get());

  if (isCmp && a.ty.isChar() && b.ty.isChar()) {                // rule (117)
    llvm::Value *c = b_.CreateCall(runtimeFn("pli_cmp_char", b_.getInt32Ty(),
                                             {b_.getPtrTy(), b_.getInt64Ty(),
                                              b_.getPtrTy(), b_.getInt64Ty()}),
                                   {a.ptr, a.len, b.ptr, b.len}, "scmp");
    llvm::CmpInst::Predicate pred = llvm::CmpInst::ICMP_EQ;
    switch (op) {
      case Tok::Eq: pred = llvm::CmpInst::ICMP_EQ; break;
      case Tok::Ne: pred = llvm::CmpInst::ICMP_NE; break;
      case Tok::Lt: pred = llvm::CmpInst::ICMP_SLT; break;
      case Tok::Le: pred = llvm::CmpInst::ICMP_SLE; break;
      case Tok::Gt: pred = llvm::CmpInst::ICMP_SGT; break;
      case Tok::Ge: pred = llvm::CmpInst::ICMP_SGE; break;
      case Tok::Ngt: pred = llvm::CmpInst::ICMP_SLE; break;
      case Tok::Nlt: pred = llvm::CmpInst::ICMP_SGE; break;
      default: break;
    }
    v.ty = Type::bit(1);
    v.reg = b_.CreateICmp(pred, c, i32(0), "cmp");
    return v;
  }

  Type common = isCmp ? arithResultType(a.ty.isBit() ? Type::fixedBin(31, 0) : a.ty,
                                        b.ty.isBit() ? Type::fixedBin(31, 0) : b.ty)
                      : e->ty;
  if (!isCmp && (op == Tok::Slash || op == Tok::Power)) common = Type::flt(e->ty.prec);
  Val av = convert(a, common, e->loc);
  Val bv = convert(b, common, e->loc);
  const bool flt = common.k == TK::Float;

  if (isCmp) {
    llvm::Value *r;
    if (flt) {
      llvm::FCmpInst::Predicate pred = llvm::CmpInst::FCMP_OEQ;
      switch (op) {
        case Tok::Eq: pred = llvm::CmpInst::FCMP_OEQ; break;
        case Tok::Ne: pred = llvm::CmpInst::FCMP_UNE; break;
        case Tok::Lt: pred = llvm::CmpInst::FCMP_OLT; break;
        case Tok::Le: pred = llvm::CmpInst::FCMP_OLE; break;
        case Tok::Gt: pred = llvm::CmpInst::FCMP_OGT; break;
        case Tok::Ge: pred = llvm::CmpInst::FCMP_OGE; break;
        case Tok::Ngt: pred = llvm::CmpInst::FCMP_OLE; break;
        case Tok::Nlt: pred = llvm::CmpInst::FCMP_OGE; break;
        default: break;
      }
      r = b_.CreateFCmp(pred, av.reg, bv.reg, "cmp");
    } else {
      llvm::CmpInst::Predicate pred = llvm::CmpInst::ICMP_EQ;
      switch (op) {
        case Tok::Eq: pred = llvm::CmpInst::ICMP_EQ; break;
        case Tok::Ne: pred = llvm::CmpInst::ICMP_NE; break;
        case Tok::Lt: pred = llvm::CmpInst::ICMP_SLT; break;
        case Tok::Le: pred = llvm::CmpInst::ICMP_SLE; break;
        case Tok::Gt: pred = llvm::CmpInst::ICMP_SGT; break;
        case Tok::Ge: pred = llvm::CmpInst::ICMP_SGE; break;
        case Tok::Ngt: pred = llvm::CmpInst::ICMP_SLE; break;
        case Tok::Nlt: pred = llvm::CmpInst::ICMP_SGE; break;
        default: break;
      }
      r = b_.CreateICmp(pred, av.reg, bv.reg, "cmp");
    }
    v.ty = Type::bit(1);
    v.reg = r;
    return v;
  }

  llvm::Value *r;
  switch (op) {
    case Tok::Plus:  r = flt ? b_.CreateFAdd(av.reg, bv.reg, "bin") : b_.CreateAdd(av.reg, bv.reg, "bin"); break;
    case Tok::Minus: r = flt ? b_.CreateFSub(av.reg, bv.reg, "bin") : b_.CreateSub(av.reg, bv.reg, "bin"); break;
    case Tok::Star:  r = flt ? b_.CreateFMul(av.reg, bv.reg, "bin") : b_.CreateMul(av.reg, bv.reg, "bin"); break;
    case Tok::Slash: r = b_.CreateFDiv(av.reg, bv.reg, "bin"); break;
    case Tok::Power: r = b_.CreateCall(runtimeFn("llvm.pow.f64", b_.getDoubleTy(),
                                                 {b_.getDoubleTy(), b_.getDoubleTy()}),
                                       {av.reg, bv.reg}, "bin"); break;
    default: r = b_.CreateAdd(i64(0), i64(0)); break;
  }
  v.ty = common;
  v.reg = r;
  return v;
}
