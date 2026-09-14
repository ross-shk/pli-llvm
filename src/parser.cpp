#include "parser.h"
#include <cstdlib>

// Parse an exact FIXED DECIMAL constant (rule 135): digits with a fraction
// point and no exponent, e.g. "3.14". Sets ival to the value scaled by 10^q,
// decScale to the fraction digit count q, and decPrec to the digit count p.
static void parseDecConstant(const std::string& text, Expr& e) {
  size_t dot = text.find('.');
  std::string intPart = dot == std::string::npos ? text : text.substr(0, dot);
  std::string fracPart = dot == std::string::npos ? "" : text.substr(dot + 1);
  long long scaled = 0;
  for (char c : intPart)
    scaled = scaled * 10 + (c - '0');
  for (char c : fracPart)
    scaled = scaled * 10 + (c - '0');
  e.ival = scaled;
  e.decScale = (int)fracPart.size();
  e.decPrec = (int)(intPart.size() + fracPart.size());
}

// True when a bare word is a scalar type/attribute keyword (rules (16)-(19)),
// so `RETURNS(fixed)`/`RETURNS(float)` mean a scalar descriptor rather than a
// structure template reference (rule 127).
static bool isScalarTypeWord(const std::string& w) {
  return w == "FIXED" || w == "FLOAT" || w == "BINARY" || w == "BIN" || w == "DECIMAL" ||
         w == "DEC" || w == "CHARACTER" || w == "CHAR" || w == "BIT" || w == "VARYING" ||
         w == "VAR" || w == "ALIGNED" || w == "UNALIGNED" || w == "POINTER";
}

// ---------------------------------------------------------------------------
// Operator mapping. Symbols, plus the 48-character-set operator words of
// TR 25.084 §2.3.3 (NOT AND OR GT LT GE LE NG NL NE CAT).
// ---------------------------------------------------------------------------
static Tok infixOp(const Token& t) {
  switch (t.kind) {
  case Tok::Bar:
  case Tok::Amp:
  case Tok::Concat:
  case Tok::Eq:
  case Tok::Ne:
  case Tok::Lt:
  case Tok::Le:
  case Tok::Gt:
  case Tok::Ge:
  case Tok::Ngt:
  case Tok::Nlt:
  case Tok::Plus:
  case Tok::Minus:
  case Tok::Star:
  case Tok::Slash:
  case Tok::Power:
    return t.kind;
  case Tok::Word:
    break;
  default:
    return Tok::Eof;
  }
  if (t.text == "AND")
    return Tok::Amp;
  if (t.text == "OR")
    return Tok::Bar;
  if (t.text == "CAT")
    return Tok::Concat;
  if (t.text == "GT")
    return Tok::Gt;
  if (t.text == "LT")
    return Tok::Lt;
  if (t.text == "GE")
    return Tok::Ge;
  if (t.text == "LE")
    return Tok::Le;
  if (t.text == "NE")
    return Tok::Ne;
  if (t.text == "NG")
    return Tok::Ngt;
  if (t.text == "NL")
    return Tok::Nlt;
  return Tok::Eof;
}

// Precedence per rules (115)-(122): | < & < comparison < || < +- < */ < **
static int precOf(Tok op) {
  switch (op) {
  case Tok::Bar:
    return 1;
  case Tok::Amp:
    return 2;
  case Tok::Eq:
  case Tok::Ne:
  case Tok::Lt:
  case Tok::Le:
  case Tok::Gt:
  case Tok::Ge:
  case Tok::Ngt:
  case Tok::Nlt:
    return 3;
  case Tok::Concat:
    return 4;
  case Tok::Plus:
  case Tok::Minus:
    return 5;
  case Tok::Star:
  case Tok::Slash:
    return 6;
  default:
    return 0; // ** handled in parsePower (right associative)
  }
}

bool Parser::expect(Tok k, const char* rule) {
  if (at(k)) {
    advance();
    return true;
  }
  d_.error(cur().loc,
           std::string("expected ") + tokName(k) + ", found " +
               (cur().kind == Tok::Word ? "'" + cur().text + "'" : tokName(cur().kind)),
           rule);
  return false;
}

void Parser::resync() {
  while (!at(Tok::Eof) && !at(Tok::Semi))
    advance();
  eat(Tok::Semi);
}

// ---------------------------------------------------------------------------
// Contextual keyword recognition.
//
// A word in statement-initial position is a keyword unless the statement is
// really an assignment to a variable that happens to bear that spelling.
// Bounded lookahead: WORD '='            -> assignment  (e.g. `IF = 5;`)
//                    WORD '(' ... ')' '=' -> assignment  (e.g. `IF(3) = 5;`)
//                    otherwise            -> keyword
// Where the two readings are ambiguous — `IF (X) = 1 THEN` is the IF
// statement of rule (74), whose condition is `(X) = 1` — step 3 of ADR-004
// speculatively parses the keyword reading and keeps it only if it succeeds
// (probeKeywordStatement).
// ---------------------------------------------------------------------------
bool Parser::looksLikeAssignment() const {
  size_t j = i_ + 1;
  if (j >= t_.size())
    return false;
  // Qualified / locator-qualified targets: A.B = , P->A =
  while (j + 1 < t_.size() && (t_[j].kind == Tok::Dot || t_[j].kind == Tok::Arrow) &&
         t_[j + 1].kind == Tok::Word)
    j += 2;
  // Multiple assignment targets: A, B, C = (rule 86) — skip further
  // references separated by commas (each possibly qualified).
  while (j + 1 < t_.size() && t_[j].kind == Tok::Comma && t_[j + 1].kind == Tok::Word) {
    j += 2;
    while (j + 1 < t_.size() && (t_[j].kind == Tok::Dot || t_[j].kind == Tok::Arrow) &&
           t_[j + 1].kind == Tok::Word)
      j += 2;
  }
  if (t_[j].kind == Tok::Eq)
    return true;
  if (t_[j].kind == Tok::LParen) {
    int depth = 0;
    for (; j < t_.size(); ++j) {
      if (t_[j].kind == Tok::LParen)
        ++depth;
      else if (t_[j].kind == Tok::RParen) {
        if (--depth == 0) {
          ++j;
          break;
        }
      } else if (t_[j].kind == Tok::Semi || t_[j].kind == Tok::Eof)
        return false;
    }
    return j < t_.size() && t_[j].kind == Tok::Eq;
  }
  return false;
}

// The words keywordStatement() dispatches on. Only a word with a keyword
// spelling can be ambiguous with the keyword reading, so the speculative
// probe (ADR-004 step 3) is gated on this.
static bool stmtKeywordSpelling(const std::string& w) {
  static const char* const kws[] = {
      "DECLARE",  "DCL",  "IF",   "DO",    "BEGIN", "PUT",   "CALL",    "RETURN",
      "STOP",     "EXIT", "GET",  "GO",    "GOTO",  "ON",    "SIGNAL",  "REVERT",
      "ALLOCATE", "FREE", "OPEN", "CLOSE", "READ",  "WRITE", "REWRITE", "DELETE",
  };
  for (const char* k : kws)
    if (w == k)
      return true;
  return false;
}

bool Parser::atStmtKeyword(const char* w) const {
  return cur().isWord(w) && !looksLikeAssignment();
}

// ---------------------------------------------------------------------------
// program ::= procedure•••                                          rule (1)
// ---------------------------------------------------------------------------
std::unique_ptr<Program> Parser::parse() {
  while (!at(Tok::Eof)) {
    size_t before = i_;
    parseExternalProcedure();
    if (i_ == before) {
      d_.error(cur().loc, "expected an external procedure", "(1)");
      break;
    }
  }
  return std::move(prog_);
}

Proc* Parser::startProc(const std::string& name, SourceLoc loc, Proc* parent) {
  auto p = std::make_unique<Proc>();
  p->name = name;
  p->loc = loc;
  p->parent = parent;
  Proc* raw = p.get();
  prog_->procs.push_back(std::move(p));
  return raw;
}

// procedure ::= [prefixlist] entry-namelist PROCEDURE [(parameterlist)]
//               [procedure-optionslist] sentencelist                rule (2)
void Parser::parseExternalProcedure() {
  if (!at(Tok::Word)) {
    d_.error(cur().loc, "expected procedure name", "(2)");
    resync();
    return;
  }
  std::string name = cur().text;
  SourceLoc loc = cur().loc;
  advance();
  // rule (3) entry-namelist: `a, b: PROCEDURE` — the first name is the primary,
  // the rest are additional entry points to the same procedure body.
  std::vector<std::string> entryNames;
  while (eat(Tok::Comma)) {
    if (at(Tok::Word)) {
      entryNames.push_back(cur().text);
      advance();
    } else {
      d_.error(cur().loc, "expected entry name", "(3)");
      resync();
      return;
    }
  }
  if (!expect(Tok::Colon, "(64)")) {
    resync();
    return;
  }
  if (!(atStmtKeyword("PROCEDURE") || atStmtKeyword("PROC"))) {
    d_.error(cur().loc, "expected PROCEDURE after entry name", "(2)", "PROCEDURE ");
    resync();
    return;
  }
  advance();
  Proc* p = startProc(name, loc, nullptr);
  p->entryNames = std::move(entryNames);
  parseProcOptions(p);
  parseProcBody(p);
}

void Parser::parseProcOptions(Proc* p) {
  // [ ( parameterlist ) ]                                            rule (4)
  if (eat(Tok::LParen)) {
    while (!at(Tok::RParen) && !at(Tok::Eof)) {
      if (at(Tok::Word)) {
        p->params.push_back(cur().text);
        advance();
      } else {
        d_.error(cur().loc, "expected parameter name", "(4)");
        advance();
      }
      if (!eat(Tok::Comma))
        break;
    }
    expect(Tok::RParen, "(4)");
  }
  // [ procedure-optionslist ]                                        rule (5)
  while (!at(Tok::Semi) && !at(Tok::Eof)) {
    if (atWord("OPTIONS")) {
      advance();
      if (expect(Tok::LParen, "(5)")) {
        int depth = 1;
        while (depth > 0 && !at(Tok::Eof)) {
          if (at(Tok::LParen))
            ++depth;
          else if (at(Tok::RParen))
            --depth;
          else if (atWord("MAIN"))
            p->isMain = true;
          advance();
        }
      }
      continue;
    }
    if (eatWord("RECURSIVE"))
      continue;
    if (atWord("RETURNS")) {
      // RETURNS(data-attributes) ::= the result type of a function
      // procedure (rules (5),(34)). Parse the type from the attribute words, or
      // — when the operand is a single name — treat it as a structure variable
      // whose shape the function returns (rule (127), resolved in sema).
      advance();
      if (expect(Tok::LParen, "(34)")) {
        p->isFunction = true;
        // A lone identifier that is not a scalar type keyword names a structure
        // template whose shape the function returns (rule 127).
        if (cur().kind == Tok::Word && peek().kind == Tok::RParen &&
            !isScalarTypeWord(cur().text)) {
          p->returnsStructName = cur().text;
          advance();
        } else {
          parseDescriptorType(p->retTy); // result type from the attribute words
        }
        expect(Tok::RParen, "(34)");
      }
      continue;
    }
    d_.warn(cur().loc,
            "ignoring unsupported procedure option '" +
                (cur().kind == Tok::Word ? cur().text : std::string(tokName(cur().kind))) + "'",
            "(5)");
    advance();
  }
  expect(Tok::Semi, "(2)");
  if (p->isMain && !prog_->mainProc)
    prog_->mainProc = p;
}

EndInfo Parser::parseProcBody(Proc* p) {
  EndInfo e = parseBody(p, p->body, p->name);
  if (e.present && !e.label.empty()) {
    // END names something that is not this procedure and not any enclosing
    // block we know about.
    d_.error(e.loc, "END label '" + e.label + "' does not match any open block", "(7)");
  }
  return e;
}

// sentencelist ::= [sentence•••] end-clause                     rules (6),(7)
//
// Implements multiple closure (TR §2.3.2.2): an END bearing a label that names
// an enclosing block closes every block in between.
EndInfo Parser::parseBody(Proc* owner, std::vector<StmtP>& body, const std::string& ownName) {
  for (;;) {
    if (at(Tok::Eof)) {
      d_.error(cur().loc, "unexpected end of file: missing END for '" + ownName + "'", "(7)",
               "END " + ownName + ";");
      return {};
    }
    if (atStmtKeyword("END")) {
      EndInfo e;
      e.present = true;
      e.loc = cur().loc;
      advance();
      if (at(Tok::Word)) {
        e.label = cur().text;
        advance();
      }
      expect(Tok::Semi, "(7)");
      if (e.label.empty() || e.label == ownName) {
        e.label.clear();
        return e;
      }
      return e; // closes an outer block -> propagate
    }
    StmtP s = parseStatement(owner);
    if (s)
      body.push_back(std::move(s));
    if (pendingEnd_.present) {
      EndInfo e = pendingEnd_;
      pendingEnd_ = {};
      if (e.label == ownName) {
        e.label.clear();
        return e;
      }
      return e;
    }
  }
}

StmtP Parser::parseStatement(Proc* owner) {
  auto st = std::make_unique<Stmt>();
  st->loc = cur().loc;

  // condition prefix list                                          rule (60)
  while (at(Tok::LParen)) {
    SourceLoc l = cur().loc;
    int depth = 0;
    size_t save = i_;
    while (!at(Tok::Eof)) {
      if (at(Tok::LParen))
        ++depth;
      else if (at(Tok::RParen)) {
        if (--depth == 0) {
          advance();
          break;
        }
      }
      advance();
    }
    if (at(Tok::Colon)) {
      advance();
      d_.warn(l, "condition prefixes are parsed but not yet enforced", "(60)");
    } else {
      i_ = save;
      break;
    }
  }

  // label prefixes                                                 rule (64)
  while (at(Tok::Word) && peek().kind == Tok::Colon) {
    st->labels.push_back(cur().text);
    advance();
    advance();
  }

  // nested procedure                                          rules (2),(8)
  if ((atStmtKeyword("PROCEDURE") || atStmtKeyword("PROC")) && !st->labels.empty()) {
    advance();
    Proc* p = startProc(st->labels.front(), st->loc, owner);
    parseProcOptions(p);
    parseProcBody(p);
    return nullptr; // procedures are hoisted into Program::procs
  }

  // ENTRY statement                                     rule (56)
  if (atStmtKeyword("ENTRY")) {
    if (st->labels.empty()) {
      d_.error(cur().loc, "ENTRY statement requires a label (the entry name)", "(56)");
      resync();
      return nullptr;
    }
    advance(); // ENTRY
    st->kind = Stmt::Entry;
    st->name = st->labels.front();
    // [ ( parameterlist ) ]
    if (eat(Tok::LParen)) {
      while (!at(Tok::RParen) && !at(Tok::Eof)) {
        if (at(Tok::Word)) {
          st->params.push_back(cur().text);
          advance();
        } else {
          d_.error(cur().loc, "expected parameter name", "(56)");
          advance();
        }
        if (!eat(Tok::Comma))
          break;
      }
      expect(Tok::RParen, "(56)");
    }
    // [ RETURNS ( data-attributes ) ]
    if (atWord("RETURNS")) {
      advance();
      if (expect(Tok::LParen, "(56)")) {
        st->entryIsFunction = true;
        parseDescriptorType(st->entryRetTy);
        expect(Tok::RParen, "(56)");
      }
    }
    expect(Tok::Semi, "(56)");
    return st;
  }

  if (eat(Tok::Semi)) {
    st->kind = Stmt::Null;
    return st;
  } // rule (67)

  // WORD ( ... ) = is ambiguous until parsed (ADR-004 step 3): it may be a
  // statement whose own grammar contains that shape (IF (X) = 1 THEN ...)
  // or an assignment to a variable bearing the keyword's spelling. Prefer
  // the keyword reading when a speculative parse of it succeeds; otherwise
  // fall through and let the word be an assignment target.
  if (cur().kind == Tok::Word && looksLikeAssignment() && stmtKeywordSpelling(cur().text)) {
    StmtP s = probeKeywordStatement(owner, st->labels);
    if (s)
      return s;
  }

  StmtP k = keywordStatement(owner, st->labels, /*probe=*/false);
  if (k) {
    k->labels = st->labels;
    return k;
  }

  auto s = parseAssignment();
  if (s)
    s->labels = st->labels;
  return s;
}

// ADR-004 step 3: try the keyword reading of an ambiguous leading word under
// muted diagnostics. Keep the resulting statement only when the reading parses
// cleanly (emitted no errors); otherwise rewind the token stream, any
// propagated END (multiple closure) and any procedure hoisted into the program
// during the probe, and report failure so the caller falls back to the
// assignment reading. A probe is grammar-only — sema has no symbol table to
// bias it — so when both readings are valid, the keyword reading wins.
StmtP Parser::probeKeywordStatement(Proc* owner, const std::vector<std::string>& labels) {
  size_t save = i_;
  EndInfo saveEnd = pendingEnd_;
  size_t saveProcs = prog_->procs.size();
  int before = d_.mutedErrors();
  d_.mute();
  StmtP s = keywordStatement(owner, labels, /*probe=*/true);
  bool clean = s && d_.mutedErrors() == before;
  d_.unmute();
  if (clean)
    return s;
  i_ = save;
  pendingEnd_ = saveEnd;
  while (prog_->procs.size() > saveProcs)
    prog_->procs.pop_back();
  d_.rewindMutedErrors(before);
  return nullptr;
}

// Statement-keyword dispatch (rules (57)-(59) and the keyed statements).
// With probe=false the keyword spelling is guarded by the bounded-lookahead
// of ADR-004 step 2 (atStmtKeyword); with probe=true it is taken on spelling
// alone so a speculative parse can decide whether the keyword reading holds.
// Returns nullptr when no keyword matched (caller tries rule (86) then).
StmtP Parser::keywordStatement(Proc* owner, const std::vector<std::string>& labels, bool probe) {
  auto kw = [&](const char* w) { return probe ? cur().isWord(w) : atStmtKeyword(w); };

  if (kw("DECLARE") || kw("DCL")) {
    advance();
    return parseDeclare();
  } // rule (9)
  if (kw("IF")) {
    return parseIf(owner);
  } // rules (74),(75)
  if (kw("DO")) {
    return parseDo(owner, labels);
  } // rules (69)-(73)
  if (kw("BEGIN")) { // rule (68)
    auto st = std::make_unique<Stmt>();
    st->loc = cur().loc;
    advance();
    expect(Tok::Semi, "(68)");
    st->kind = Stmt::Begin; // a block with its own scope (rule (68))
    EndInfo e = parseBody(owner, st->body, labels.empty() ? std::string() : labels.front());
    if (e.present && !e.label.empty())
      pendingEnd_ = e;
    return st;
  }
  if (kw("PUT")) {
    return parsePut();
  } // rules (104)-(109)
  if (kw("DISPLAY")) {
    return parseDisplay();
  } // rule (114)
  if (kw("CALL")) {
    return parseCall();
  } // rules (78)-(80)
  if (kw("RETURN")) { // rule (81)
    auto st = std::make_unique<Stmt>();
    st->loc = cur().loc;
    advance();
    st->kind = Stmt::Return;
    if (eat(Tok::LParen)) { // RETURN(value) — function value, rule (81)
      st->value = parseExpr();
      expect(Tok::RParen, "(81)");
    }
    expect(Tok::Semi, "(81)");
    return st;
  }
  if (kw("STOP") || kw("EXIT")) { // rules (84),(85)
    auto st = std::make_unique<Stmt>();
    st->loc = cur().loc;
    advance();
    st->kind = Stmt::Stop;
    expect(Tok::Semi, "(85)");
    return st;
  }
  if (kw("GET"))
    return parseGet();
  if (kw("GO") && peek().kind == Tok::Word && peek().isWord("TO")) {
    // GO TO label ;                                        rule (77)
    auto st = std::make_unique<Stmt>();
    st->loc = cur().loc;
    advance(); // GO
    advance(); // TO
    st->kind = Stmt::Goto;
    if (at(Tok::Word)) {
      st->name = cur().text;
      advance();
    } else
      d_.error(cur().loc, "expected a label after GO TO", "(77)");
    expect(Tok::Semi, "(77)");
    return st;
  }
  if (kw("GOTO")) {
    // GOTO label ;   (one-word spelling)                  rule (77)
    auto st = std::make_unique<Stmt>();
    st->loc = cur().loc;
    advance();
    st->kind = Stmt::Goto;
    if (at(Tok::Word)) {
      st->name = cur().text;
      advance();
    } else
      d_.error(cur().loc, "expected a label after GOTO", "(77)");
    expect(Tok::Semi, "(77)");
    return st;
  }
  if (kw("ON"))
    return parseOn(owner); // rule (91)
  if (kw("SIGNAL"))
    return parseSignal(); // rule (93)
  if (kw("REVERT"))
    return parseRevert(); // rule (92)
  if (kw("ALLOCATE"))
    return parseAllocate();
  if (kw("FREE"))
    return parseFree();
  if (kw("OPEN"))
    return parseOpen();
  if (kw("CLOSE"))
    return parseClose();
  if (kw("READ") || kw("WRITE") || kw("REWRITE") || kw("DELETE")) {
    d_.error(cur().loc, "file input/output is not implemented in this stage", "(100)");
    resync();
    return nullptr;
  }
  return nullptr;
}

// Consume a balanced (...) group at the current token (the opening paren).
void Parser::skipParen(const char* rule) {
  if (!at(Tok::LParen)) {
    d_.error(cur().loc, "expected '('", rule);
    return;
  }
  int depth = 0;
  while (!at(Tok::Eof)) {
    if (at(Tok::LParen))
      ++depth;
    else if (at(Tok::RParen) && --depth == 0) {
      advance();
      return;
    }
    advance();
  }
}

// One condition (rule 94). Only ERROR is served; every other condition is
// diagnosed with its rule number, never silently accepted.
std::string Parser::parseCondition() {
  if (!at(Tok::Word)) {
    d_.error(cur().loc, "expected a condition", "(94)");
    return "";
  }
  SourceLoc l = cur().loc;
  const std::string& w = cur().text;
  if (w == "ERROR") {
    advance();
    return "ERROR";
  }
  if (w == "FINISH" || w == "AREA") {
    advance();
    d_.error(l, w + " conditions are not implemented in this stage", "(94)");
    return "";
  }
  if (w == "CONVERSION" || w == "FIXEDOVERFLOW" || w == "OVERFLOW" || w == "SIZE" ||
      w == "SUBSCRIPTRANGE" || w == "STRINGRANGE" || w == "UNDERFLOW" || w == "ZERODIVIDE") {
    advance();
    d_.error(l, w + " conditions are not implemented in this stage", "(94)");
    return "";
  }
  if (w == "CHECK") {
    advance();
    if (at(Tok::LParen))
      skipParen("(95)");
    d_.error(l, "CHECK conditions are not implemented in this stage", "(95)");
    return "";
  }
  if (w == "CONDITION") {
    advance();
    // A programmer-named condition (rule 99): the name is use-declared,
    // like an implicit variable (ADR-011); sema assigns its dispatch key.
    if (!expect(Tok::LParen, "(99)"))
      return "";
    if (!at(Tok::Word)) {
      d_.error(cur().loc, "expected a condition name", "(99)");
      return "";
    }
    std::string name = cur().text;
    SourceLoc nl = cur().loc;
    advance();
    if (!expect(Tok::RParen, "(99)"))
      return "";
    if (name == "ERROR") {
      d_.error(nl, "ERROR is not a valid programmer-named condition", "(99)");
      return "";
    }
    return name;
  }
  if (w == "ENDFILE" || w == "ENDPAGE" || w == "KEY" || w == "UNDEFINEDFILE" || w == "NAME" ||
      w == "RECORD" || w == "TRANSMIT") {
    advance();
    if (at(Tok::LParen))
      skipParen("(97)");
    d_.error(l, w + " conditions are not implemented in this stage", "(97)");
    return "";
  }
  d_.error(l, "expected a condition", "(94)");
  return "";
}

// on-statement ::= ON condition [SNAP] {unconditional-statement | SYSTEM ; }
//                                                              rule (91)
StmtP Parser::parseOn(Proc* owner) {
  auto st = std::make_unique<Stmt>();
  st->loc = cur().loc;
  advance(); // ON
  st->kind = Stmt::On;
  st->condName = parseCondition();
  if (st->condName.empty()) {
    resync();
    return nullptr;
  }
  if (atWord("SNAP")) {
    SourceLoc l = cur().loc;
    advance();
    st->snap = true;
    d_.warn(l, "SNAP is accepted but has no effect in this stage", "(91)");
  }
  // ON ERROR SYSTEM; establishes the system action (rule 91).
  if (atWord("SYSTEM") && peek().kind == Tok::Semi) {
    advance();
    st->isSystem = true;
    expect(Tok::Semi, "(91)");
    return st;
  }
  StmtP u = parseStatement(owner);
  if (!u) {
    d_.error(cur().loc, "expected an unconditional statement as the ON-unit", "(91)");
    resync();
    return nullptr;
  }
  if (u->kind == Stmt::If) {
    d_.error(u->loc, "an ON-unit must be an unconditional statement", "(91)");
    resync();
    return nullptr;
  }
  st->unit = std::move(u);
  return st;
}

// revert-statement ::= REVERT condition ;                      rule (92)
StmtP Parser::parseRevert() {
  auto st = std::make_unique<Stmt>();
  st->loc = cur().loc;
  advance(); // REVERT
  st->kind = Stmt::Revert;
  st->condName = parseCondition();
  if (st->condName.empty()) {
    resync();
    return nullptr;
  }
  expect(Tok::Semi, "(92)");
  return st;
}

// signal-statement ::= SIGNAL condition ;                      rule (93)
StmtP Parser::parseSignal() {
  auto st = std::make_unique<Stmt>();
  st->loc = cur().loc;
  advance(); // SIGNAL
  st->kind = Stmt::Signal;
  st->condName = parseCondition();
  if (st->condName.empty()) {
    resync();
    return nullptr;
  }
  expect(Tok::Semi, "(93)");
  return st;
}

// declaration-sentence ::= [labellist] DECLARE declarationlist ;   rule (9)
StmtP Parser::parseDeclare() {
  auto st = std::make_unique<Stmt>();
  st->kind = Stmt::Declare;
  st->loc = cur().loc;
  for (;;) {
    if (at(Tok::LParen)) {
      // Factored declaration (rule 11): DECLARE (A, B, C) [dim] attrs... — every
      // name in the parenthesised list shares the dimension + attribute tail.
      // Parse the name list, parse the tail once, then clone it per name.
      SourceLoc floc = cur().loc;
      advance(); // (
      std::vector<std::pair<std::string, SourceLoc>> names;
      for (;;) {
        if (at(Tok::Word)) {
          names.push_back({cur().text, cur().loc});
          advance();
        } else {
          d_.error(cur().loc, "expected a name in factored declaration", "(11)");
          break;
        }
        if (!eat(Tok::Comma))
          break;
      }
      expect(Tok::RParen, "(11)");
      if (names.empty())
        d_.error(floc, "factored declaration has no names", "(11)");
      DeclItem base;
      if (!parseDeclTail(base)) {
        resync();
        return st;
      }
      if (base.init)
        d_.error(floc, "INITIAL in a factored declaration is not implemented in this stage",
                 "(26)");
      if (base.initCall)
        d_.error(floc, "INITIAL CALL in a factored declaration is not implemented in this stage",
                 "(27)");
      if (!base.dynBounds.empty())
        d_.error(floc, "dynamic bounds in a factored declaration are not implemented in this stage",
                 "(13)");
      for (auto& [n, nl] : names) {
        DeclItem item;
        item.name = n;
        item.loc = nl;
        item.level = base.level;
        item.ty = base.ty;
        item.isEntry = base.isEntry;
        item.extName = base.extName;
        item.entryParams = base.entryParams;
        item.like = base.like;
        st->decls.push_back(std::move(item));
      }
    } else {
      DeclItem item;
      if (!parseDeclItem(item)) {
        resync();
        return st;
      }
      st->decls.push_back(std::move(item));
    }
    if (!eat(Tok::Comma))
      break;
  }
  expect(Tok::Semi, "(9)");
  return st;
}

// Consume one scalar computational attribute word into `bag`. Shared by
// parseDeclItem (rule 11) and parseDescriptorType (rule 38); `rule` cites the
// TR production for the parenthesised precision group.
bool Parser::parseScalarAttr(AttrBag& bag, const char* rule) {
  if (!at(Tok::Word))
    return false;
  const std::string& w = cur().text;
  auto parenNums = [&](int& n1, int& n2) {
    if (!eat(Tok::LParen))
      return false;
    if (at(Tok::Number)) {
      n1 = atoi(cur().text.c_str());
      advance();
    }
    if (eat(Tok::Comma) && at(Tok::Number)) {
      n2 = atoi(cur().text.c_str());
      advance();
    }
    expect(Tok::RParen, rule);
    return true;
  };
  int a = -1, b = 0;
  if (w == "FIXED") {
    bag.fixed = true;
    advance();
    if (at(Tok::LParen)) {
      parenNums(a, b);
      if (a > 0) {
        bag.prec = a;
        bag.scale = b;
      }
    }
    return true;
  }
  if (w == "FLOAT") {
    bag.floating = true;
    advance();
    if (at(Tok::LParen)) {
      parenNums(a, b);
      if (a > 0)
        bag.prec = a;
    }
    return true;
  }
  if (w == "BINARY" || w == "BIN") {
    bag.binary = true;
    advance();
    if (at(Tok::LParen)) {
      parenNums(a, b);
      if (a > 0) {
        bag.prec = a;
        bag.scale = b;
      }
    }
    return true;
  }
  if (w == "DECIMAL" || w == "DEC") {
    bag.decimal = true;
    advance();
    if (at(Tok::LParen)) {
      parenNums(a, b);
      if (a > 0) {
        bag.prec = a;
        bag.scale = b;
      }
    }
    return true;
  }
  if (w == "CHARACTER" || w == "CHAR") {
    bag.character = true;
    advance();
    if (at(Tok::LParen)) {
      parenNums(a, b);
      if (a > 0)
        bag.slen = a;
    }
    return true;
  }
  if (w == "BIT") {
    bag.bit = true;
    advance();
    if (at(Tok::LParen)) {
      parenNums(a, b);
      if (a > 0)
        bag.slen = a;
    }
    return true;
  }
  if (w == "VARYING" || w == "VAR") {
    bag.varying = true;
    advance();
    return true;
  }
  if (w == "REAL") {
    advance();
    return true;
  }
  return false;
}

// declaration ::= [integer] identifier [dimension] [attribute•••]  rule (11)
bool Parser::parseDeclItem(DeclItem& item) {
  if (at(Tok::Number) && !cur().isFloat) {
    item.level = atoi(cur().text.c_str());
    advance();
  }
  if (!at(Tok::Word)) {
    d_.error(cur().loc, "expected a name in DECLARE", "(11)");
    return false;
  }
  item.name = cur().text;
  item.loc = cur().loc;
  advance();
  return parseDeclTail(item);
}

// Dimension + attribute tail of a declaration (rule 11), shared verbatim by a
// factored declaration list (DECLARE (A, B) FIXED): the dimension attribute
// first, then the attribute bag, ending by building item.ty / dims / init.
bool Parser::parseDeclTail(DeclItem& item) {
  // Dimension attribute (rules (12),(13)): a leading parenthesised group after
  // the name is a dimension when a bound-pair ':' is present or an attribute
  // keyword follows; otherwise it is a precision/length (M0 scalar behaviour).
  std::vector<Dim> arrDims;
  std::vector<ExprP> arrDyn;
  std::vector<ExprP> arrDynLb;
  tryParseDimension(arrDims, arrDyn, arrDynLb);

  // Attribute bag (rules 14-32).
  AttrBag bag;
  ExprP init;

  for (;;) {
    if (parseScalarAttr(bag, "(16)"))
      continue;
    if (at(Tok::Word)) {
      const std::string& w = cur().text;
      if (w == "INITIAL" || w == "INIT") {
        advance();
        if (expect(Tok::LParen, "(26)")) {
          if (atWord("CALL")) {
            // initial-call ::= CALL identifier ( argumentlist )  rule (27)
            advance(); // CALL
            auto call = std::make_unique<Expr>();
            if (at(Tok::Word)) {
              call->kind = Expr::Call;
              call->name = cur().text;
              call->loc = cur().loc;
              advance();
              if (at(Tok::LParen)) {
                advance();
                if (!at(Tok::RParen)) {
                  for (;;) {
                    call->args.push_back(parseExpr());
                    if (!eat(Tok::Comma))
                      break;
                  }
                }
                expect(Tok::RParen, "(27)");
              } else {
                d_.error(call->loc, "expected '(' after the INITIAL CALL function name", "(27)");
              }
            } else {
              d_.error(cur().loc, "expected a function name after INITIAL CALL", "(27)");
            }
            item.initCall = std::move(call);
            expect(Tok::RParen, "(26)"); // close INITIAL( ... )
          } else {
            item.initItems = parseInitialList();
            // Back-compat: a single plain value is also the M0 scalar item.init.
            if (item.initItems.size() == 1 && item.initItems[0].kind == InitItem::Value)
              init = std::move(item.initItems[0].value);
          }
        }
        continue;
      }
      if (w == "STATIC" || w == "AUTOMATIC" || w == "AUTO" || w == "ALIGNED" || w == "UNALIGNED" ||
          w == "INTERNAL") {
        d_.warn(cur().loc, "attribute " + w + " is accepted but has no effect in this stage",
                "(15)");
        advance();
        continue;
      }
      if (w == "EXTERNAL" || w == "EXT") {
        // Extended external-name form: EXTERNAL('symbol') gives the exact,
        // case-sensitive C symbol for interlanguage calls (see z/OS ILC).
        advance();
        if (at(Tok::LParen) && peek().kind == Tok::CharLit) {
          advance(); // (
          item.extName = cur().sval;
          advance(); // the quoted symbol
          if (at(Tok::RParen))
            advance();
        }
        continue;
      }
      if (w == "ENTRY") {
        // Declares an external entry (a C procedure, resolved at link time).
        // rule (38). The optional ( ... ) is the parameter descriptor list.
        item.isEntry = true;
        advance();
        if (at(Tok::LParen))
          parseEntryParams(item.entryParams);
        continue;
      }
      if (w == "RETURNS") {
        // entry-name-attribute RETURNS ( function-attribute ) (rule (34)): the
        // result type of an ENTRY-declared function. Valid only on an entry.
        advance();
        if (expect(Tok::LParen, "(34)")) {
          item.entryIsFunction = true;
          parseDescriptorType(item.entryRetTy);
          expect(Tok::RParen, "(34)");
        }
        continue;
      }
      if (w == "LIKE") {
        // like-attribute ::= LIKE unsubscripted-reference (rule 43): the
        // declared item takes the structure shape of the referenced structure.
        advance();
        if (at(Tok::Word)) {
          item.like = cur().text;
          advance();
          if (at(Tok::Dot)) // LIKE S.A.B: qualified template, diagnosed in sema
            d_.error(cur().loc, "LIKE with a qualified reference is not implemented in this stage",
                     "(43)");
        } else {
          d_.error(cur().loc, "expected a reference after LIKE", "(43)");
        }
        continue;
      }
      if (w == "DEFINED" || w == "DEF") {
        // defined-attribute ::= DEFINED basic-reference [ POSITION(integer) ]
        // (rule 24): the declared item overlays the storage of the base
        // reference. The base is resolved in sema; POSITION is diagnosed.
        advance();
        if (at(Tok::Word)) {
          item.definedBase = cur().text;
          advance();
        } else {
          d_.error(cur().loc, "expected a reference after DEFINED", "(24)");
        }
        // An optional subscripted base DEFINED X(...) (rules 126,134): each
        // subscript is a constant integer or an iSUB dummy (nSUB). Non-constant
        // index expressions are diagnosed; sema resolves the base.
        if (at(Tok::LParen)) {
          advance();
          if (!at(Tok::RParen)) {
            for (;;) {
              // One base subscript (rules 126,134): a constant integer, a bare
              // iSUB dummy, or an affine iSUB form `[m] [*] 1SUB [+/- c]`. A
              // non-affine base subscript is diagnosed.
              DefinedSub ds;
              auto parseInt = [&](bool neg) -> long long {
                long long v = 0;
                if (at(Tok::Number)) {
                  v = strtoll(cur().text.c_str(), nullptr, 10);
                  advance();
                } else {
                  d_.error(cur().loc,
                           "DEFINED base subscripts must be constant integers or an affine iSUB "
                           "expression",
                           "(24)");
                }
                return neg ? -v : v;
              };
              if (at(Tok::Number)) {
                long long c = strtoll(cur().text.c_str(), nullptr, 10);
                advance();
                if (eat(Tok::Star)) {
                  // m * 1SUB
                  if (at(Tok::Isub)) {
                    ds.isub = true;
                    ds.mult = c;
                    advance();
                  } else {
                    d_.error(cur().loc, "expected an iSUB dummy after '*'", "(134)");
                  }
                } else {
                  // a plain constant index (no iSUB)
                  ds.expr = std::make_unique<Expr>();
                  ds.expr->kind = Expr::IntLit;
                  ds.expr->loc = cur().loc;
                  ds.expr->ival = c;
                }
              } else if (at(Tok::Isub)) {
                // 1SUB (or 1SUB +/- c)
                ds.isub = true;
                ds.mult = 1;
                advance();
              } else {
                d_.error(cur().loc,
                         "DEFINED base subscripts must be constant integers or an affine iSUB "
                         "expression",
                         "(24)");
                advance(); // skip the offending token to keep parsing the list
              }
              // optional affine offset (only meaningful after an iSUB)
              if (ds.isub && (at(Tok::Plus) || at(Tok::Minus))) {
                bool neg = cur().kind == Tok::Minus;
                advance();
                ds.add = parseInt(neg);
              }
              item.definedSubs.push_back(std::move(ds));
              if (!eat(Tok::Comma))
                break;
            }
          }
          expect(Tok::RParen, "(126)");
        }
        while (at(Tok::Word) && (cur().text == "POSITION" || cur().text == "POS")) {
          d_.error(cur().loc, "POSITION on DEFINED is not implemented in this stage", "(24)");
          advance();
          if (at(Tok::LParen)) {
            int dd = 0;
            do {
              if (at(Tok::LParen))
                ++dd;
              else if (at(Tok::RParen))
                --dd;
              advance();
            } while (dd && !at(Tok::Eof));
          }
        }
        continue;
      }
      if (w == "POINTER" || w == "PTR") {
        advance();
        bag.pointer = true;
        continue;
      }
      if (w == "FILE") {
        // file-name-attribute ::= FILE (rules 39,40): a named file variable,
        // addressed by OPEN/CLOSE and the FILE ( f ) stream option.
        advance();
        bag.file = true;
        continue;
      }
      if (w == "BASED") {
        // based-attribute ::= BASED [ ( reference ) ]  rule (25): the declared
        // item overlays the storage addressed by a POINTER variable, so it has
        // no storage of its own. This stage requires the explicit BASED(P).
        advance();
        if (at(Tok::LParen)) {
          advance();
          if (at(Tok::Word)) {
            item.basedBase = cur().text;
            advance();
          } else {
            d_.error(cur().loc, "expected a POINTER reference after BASED(", "(25)");
          }
          expect(Tok::RParen, "(25)");
        } else {
          d_.error(cur().loc, "BASED without an explicit POINTER is not implemented in this stage",
                   "(25)");
        }
        continue;
      }
      if (w == "COMPLEX" || w == "CPLX") {
        // complex-attribute (QR2.2/CM5, rules (14),(15)): a value with real and
        // imaginary parts. Consumed like the other scalar data attributes.
        advance();
        bag.complex = true;
        continue;
      }
      if (w == "PICTURE" || w == "PIC" || w == "AREA" || w == "OFFSET" || w == "CONTROLLED" ||
          w == "CTL" || w == "LABEL" || w == "TASK" || w == "EVENT" || w == "CELL" ||
          w == "GENERIC" || w == "BUILTIN") {
        d_.error(cur().loc, "attribute " + w + " is not implemented in this stage", "(15)");
        advance();
        if (at(Tok::LParen)) {
          int d = 0;
          do {
            if (at(Tok::LParen))
              ++d;
            else if (at(Tok::RParen))
              --d;
            advance();
          } while (d && !at(Tok::Eof));
        }
        continue;
      }
      break; // not an attribute: next declaration item or end
    }
    if (at(Tok::LParen)) { // bare precision or dimension
      int a = -1, b = 0;
      SourceLoc l = cur().loc;
      eat(Tok::LParen);
      if (at(Tok::Number)) {
        a = atoi(cur().text.c_str());
        advance();
      }
      if (eat(Tok::Comma) && at(Tok::Number)) {
        b = atoi(cur().text.c_str());
        advance();
      }
      expect(Tok::RParen, "(16)");
      if (bag.character || bag.bit) {
        if (a > 0)
          bag.slen = a;
      } else if (a > 0) {
        bag.prec = a;
        bag.scale = b;
      } else
        d_.error(l, "arrays are not implemented in this stage", "(12)");
      continue;
    }
    break;
  }

  // Attribute -> type, applying the default rules of TR 25.084 rules (15)-(18).
  // Conflicting attributes are diagnosed first.
  if (bag.fixed && bag.floating)
    d_.error(item.loc, "FIXED and FLOAT are conflicting attributes", "(16)");
  if (bag.binary && bag.decimal)
    d_.error(item.loc, "BINARY and DECIMAL are conflicting attributes", "(16)");
  if (bag.character && bag.bit)
    d_.error(item.loc, "CHARACTER and BIT are conflicting attributes", "(18)");
  if ((bag.character || bag.bit) && (bag.fixed || bag.floating || bag.binary || bag.decimal))
    d_.error(item.loc, "string and arithmetic attributes cannot be combined", "(15)");
  if (bag.varying && !bag.character && !bag.bit)
    d_.error(item.loc, "VARYING requires CHARACTER or BIT", "(15)");
  if (bag.pointer &&
      (bag.character || bag.bit || bag.fixed || bag.floating || bag.binary || bag.decimal))
    d_.error(item.loc, "POINTER cannot be combined with a data attribute", "(15)");
  if (bag.file && (bag.character || bag.bit || bag.fixed || bag.floating || bag.binary ||
                   bag.decimal || bag.pointer))
    d_.error(item.loc, "FILE cannot be combined with a data attribute", "(15)");
  if (bag.complex && (bag.character || bag.bit || bag.fixed || bag.floating || bag.binary ||
                      bag.decimal || bag.pointer || bag.file))
    d_.error(item.loc, "COMPLEX cannot be combined with another data attribute", "(15)");

  if (bag.file) {
    // A FILE variable carries no computational value (rules 39,40); the parser
    // records the flag and sema assigns it a runtime slot at OPEN/CLOSE.
    item.fileAttr = true;
    item.ty = Type::voidTy();
  } else if (bag.pointer) {
    item.ty = Type::ptr();
  } else if (bag.complex) {
    item.ty = Type::complexTy();
  } else if (bag.character) {
    item.ty = Type::chr(bag.slen > 0 ? bag.slen : 1, bag.varying);
  } else if (bag.bit) {
    int n = bag.slen > 0 ? bag.slen : 1;
    item.ty = Type::bit(n);
    if (n != 1) {
      // Only BIT(1) is served; arbitrary-length bit strings are M2. Never
      // silently miscompile a wider bit value as a single bit (invariant 2).
      d_.error(item.loc,
               "BIT(" + std::to_string(n) + ") is not implemented in this stage; only BIT(1)",
               "(18)");
      item.ty = Type::bit(1);
    }
  } else if (bag.floating) {
    item.ty = Type::flt(bag.prec > 0 ? bag.prec : (bag.binary ? 21 : 6));
  } else {
    // FIXED is the default scale attribute; DECIMAL the default base. The
    // scale factor q is kept as a static property (ADR-006, rule (16)).
    if (bag.binary && !bag.decimal)
      item.ty = Type::fixedBin(bag.prec > 0 ? bag.prec : 15, bag.scale);
    else
      item.ty = Type::fixedDec(bag.prec > 0 ? bag.prec : 5, bag.scale);
  }
  item.ty.dims = arrDims;
  item.dynBounds = std::move(arrDyn);
  item.dynLbBounds = std::move(arrDynLb);
  item.init = std::move(init);
  return true;
}

// See parser.h. Disambiguates a leading (n) after a name from a precision: a
// bound-pair (lb:ub) is always a dimension; a bare (n) is a dimension only when
// followed by an attribute keyword (e.g. `DECLARE A(5) FIXED BINARY;`), else it
// stays a precision/length for M0 scalar declarations.
bool Parser::tryParseDimension(std::vector<Dim>& out, std::vector<ExprP>& dynBounds,
                               std::vector<ExprP>& dynLbBounds) {
  if (!at(Tok::LParen))
    return false;
  size_t save = i_;
  eat(Tok::LParen);

  // Fold a bound expression to a compile-time constant when it is a (possibly
  // negated) integer literal; false means it is a runtime expression (rule (13)).
  auto foldBound = [&](const ExprP& e, long long& v) -> bool {
    if (e->kind == Expr::IntLit) {
      v = e->ival;
      return true;
    }
    if (e->kind == Expr::Unary && e->op == Tok::Minus && e->a && e->a->kind == Expr::IntLit) {
      v = -e->a->ival;
      return true;
    }
    return false;
  };

  // One or more comma-separated bound-pairs (lb:ub) or bare extents (n|expr),
  // one per axis (rules (12),(13)); '*' is an adjustable extent. A colon group
  // or a dynamic bound makes the whole group a dimension; all-bare constant
  // extents are a dimension only when an attribute keyword follows.
  std::vector<Dim> axes;
  std::vector<ExprP> db;  // upper-bound expr of each dynamic upper bound
  std::vector<ExprP> dlb; // lower-bound expr of each dynamic lower bound
  bool anyColon = false;
  for (;;) {
    Dim d;
    if (at(Tok::Star)) { // rule (13) '*': adjustable extent (parameter only)
      advance();
      d.dyn = true;
      d.adj = true; // the bound is supplied by the caller, not an expression
    } else {
      ExprP first = parseExpr();
      if (!first) {
        i_ = save;
        return false;
      }
      long long fv;
      bool firstConst = foldBound(first, fv);
      if (eat(Tok::Colon)) {
        anyColon = true;
        if (firstConst) {
          d.lb = (int)fv;
        } else {
          d.lbDyn = true; // runtime lower bound (rule (13)); lb is a placeholder
          dlb.push_back(std::move(first));
        }
        ExprP ubE = parseExpr();
        long long uv;
        if (!ubE) {
          i_ = save;
          return false;
        }
        if (foldBound(ubE, uv)) {
          d.ub = (int)uv;
        } else {
          d.dyn = true;
          db.push_back(std::move(ubE));
        }
      } else { // bare extent (ub): lower bound defaults to 1
        if (firstConst) {
          d.ub = (int)fv;
        } else {
          d.dyn = true;
          db.push_back(std::move(first));
        }
      }
    }
    axes.push_back(d);
    if (!eat(Tok::Comma))
      break;
  }

  expect(Tok::RParen, "(12)");

  if (anyColon) {
    out = std::move(axes);
    dynBounds = std::move(db);
    dynLbBounds = std::move(dlb);
    return true;
  }
  // All bare constants (n): a dimension only when an attribute keyword follows.
  auto isAttrWord = [&](const std::string& w) {
    return w == "FIXED" || w == "FLOAT" || w == "BINARY" || w == "BIN" || w == "DECIMAL" ||
           w == "DEC" || w == "CHARACTER" || w == "CHAR" || w == "BIT" || w == "VARYING" ||
           w == "VAR" || w == "STATIC" || w == "AUTOMATIC" || w == "AUTO" || w == "ALIGNED" ||
           w == "UNALIGNED" || w == "INTERNAL" || w == "INITIAL" || w == "INIT" ||
           w == "EXTERNAL" || w == "EXT";
  };
  if (at(Tok::Word) && isAttrWord(cur().text)) {
    out = std::move(axes);
    dynBounds = std::move(db);
    dynLbBounds = std::move(dlb);
    return true;
  }
  // A bare extent (n) directly after a name is unambiguously a dimension when
  // more items follow in the same DECLARE — a comma — because a scalar precision
  // can never follow a name bare. This is the structure-array form `1 arr(3),
  // 2 x` (rule 11), where no attribute keyword follows the bound.
  if (at(Tok::Comma)) {
    out = std::move(axes);
    dynBounds = std::move(db);
    dynLbBounds = std::move(dlb);
    return true;
  }
  i_ = save;
  return false;
}

// descriptor-param ::= attribute•••                                rule (38)
// Parse a single ENTRY parameter type, using the same scalar-attribute
// accumulator as parseDeclItem, restricted to the scalar computational types.
bool Parser::parseDescriptorType(Type& out) {
  AttrBag bag;
  while (parseScalarAttr(bag, "(38)")) {
  }
  if (bag.character)
    out = Type::chr(bag.slen > 0 ? bag.slen : 1, bag.varying);
  else if (bag.bit)
    out = Type::bit(bag.slen > 0 ? bag.slen : 1);
  else if (bag.floating)
    out = Type::flt(bag.prec > 0 ? bag.prec : (bag.binary ? 21 : 6));
  else if (bag.binary)
    out = Type::fixedBin(bag.prec > 0 ? bag.prec : 15, bag.scale);
  else
    out = Type::fixedDec(bag.prec > 0 ? bag.prec : 5, bag.scale);
  return true;
}

// entry-parameterlist ::= ( descriptor-param [ , descriptor-param ]••• )
bool Parser::parseEntryParams(std::vector<Type>& params) {
  if (!expect(Tok::LParen, "(38)"))
    return false;
  while (!at(Tok::RParen) && !at(Tok::Eof)) {
    Type t;
    parseDescriptorType(t);
    params.push_back(t);
    if (!eat(Tok::Comma))
      break;
  }
  expect(Tok::RParen, "(38)");
  return true;
}

// if-statement ::= if-clause statement | if-clause balanced-statement
//                  ELSE statement                              rules (74),(75)
StmtP Parser::parseIf(Proc* owner) {
  auto st = std::make_unique<Stmt>();
  st->kind = Stmt::If;
  st->loc = cur().loc;
  advance(); // IF
  st->cond = parseExpr();
  if (!atWord("THEN")) {
    d_.error(cur().loc, "expected THEN", "(75)", "THEN ");
    resync();
    return nullptr;
  }
  advance();
  st->thenS = parseStatement(owner);
  if (pendingEnd_.present)
    return st;
  if (atWord("ELSE")) {
    advance();
    st->elseS = parseStatement(owner);
  }
  return st;
}

// group ::= simple-group | iterated-group                  rules (69)-(73)
//
// `labels` are the label prefixes of this DO statement; a labelled group can
// be closed by `END <label>;` from any depth (multiple closure).
StmtP Parser::parseDo(Proc* owner, const std::vector<std::string>& labels) {
  auto st = std::make_unique<Stmt>();
  st->loc = cur().loc;
  advance(); // DO

  if (eat(Tok::Semi)) {
    st->kind = Stmt::Group;
  } else if (atWord("WHILE")) {
    advance();
    st->kind = Stmt::DoWhile;
    if (expect(Tok::LParen, "(71)")) {
      st->cond = parseExpr();
      expect(Tok::RParen, "(71)");
    }
    expect(Tok::Semi, "(71)");
  } else if (at(Tok::Word)) {
    st->kind = Stmt::DoIter;
    st->name = cur().text;
    advance();
    if (!expect(Tok::Eq, "(72)")) {
      resync();
      return nullptr;
    }
    st->from = parseExpr(); // rule (73)
    for (;;) {
      if (atWord("TO")) {
        advance();
        st->to = parseExpr();
        continue;
      }
      if (atWord("BY")) {
        advance();
        st->by = parseExpr();
        continue;
      }
      if (atWord("WHILE")) {
        advance();
        if (expect(Tok::LParen, "(73)")) {
          st->cond = parseExpr();
          expect(Tok::RParen, "(73)");
        }
        continue;
      }
      break;
    }
    if (at(Tok::Comma)) {
      d_.error(cur().loc, "multiple DO specifications are not implemented in this stage", "(72)");
      resync();
      return nullptr;
    }
    expect(Tok::Semi, "(71)");
  } else {
    d_.error(cur().loc, "malformed DO statement", "(71)");
    resync();
    return nullptr;
  }

  EndInfo e = parseBody(owner, st->body, labels.empty() ? std::string() : labels.front());
  if (e.present && !e.label.empty())
    pendingEnd_ = e; // multiple closure
  return st;
}

// display-statement ::= DISPLAY ( expression )                  rule (114)
StmtP Parser::parseDisplay() {
  auto st = std::make_unique<Stmt>();
  st->kind = Stmt::Display;
  st->loc = cur().loc;
  advance(); // DISPLAY
  if (!expect(Tok::LParen, "(114)"))
    return nullptr;
  st->value = parseExpr();
  if (!expect(Tok::RParen, "(114)"))
    return nullptr;
  // The REPLY form stays diagnosed.
  if (atWord("REPLY")) {
    d_.error(cur().loc, "DISPLAY ... REPLY is not implemented in this stage", "(114)");
    resync();
    return nullptr;
  }
  expect(Tok::Semi, "(114)");
  return st;
}

// stream-io-statement ::= {GET | PUT} stream-optionslist ;    rules (104)-(109)
StmtP Parser::parsePut() {
  auto st = std::make_unique<Stmt>();
  st->kind = Stmt::Put;
  st->loc = cur().loc;
  advance(); // PUT
  bool sawData = false;
  while (!at(Tok::Semi) && !at(Tok::Eof)) {
    if (atWord("SKIP")) {
      advance();
      st->skip = true;
      if (eat(Tok::LParen)) {
        st->skipCount = parseExpr();
        expect(Tok::RParen, "(105)");
      }
      continue;
    }
    if (atWord("PAGE")) {
      advance();
      st->page = true;
      continue;
    }
    if (atWord("LIST")) {
      advance();
      sawData = true;
      if (expect(Tok::LParen, "(109)")) {
        if (!at(Tok::RParen)) {
          for (;;) {
            st->items.push_back(parseExpr());
            if (!eat(Tok::Comma))
              break;
          }
        }
        expect(Tok::RParen, "(109)");
      }
      continue;
    }
    if (atWord("FILE")) {
      advance();
      if (eat(Tok::LParen)) {
        if (at(Tok::Word)) {
          st->fileIdent = cur().text;
          advance();
        } else {
          d_.error(cur().loc, "expected a FILE variable after FILE(", "(105)");
        }
        expect(Tok::RParen, "(105)");
      }
      continue;
    }
    if (atWord("EDIT")) {
      // Edit-directed transmission (rule (108)): `EDIT ( (datalist) formatlist )`.
      if (!parseEditClause(st.get())) {
        resync();
        return nullptr;
      }
      continue;
    }
    if (atWord("DATA")) {
      d_.error(cur().loc, "DATA-directed output is not implemented in this stage", "(106)");
      resync();
      return nullptr;
    }
    if (atWord("STRING")) {
      // STRING ( reference ) option (rule 105): write list-directed output into
      // the character variable instead of SYSPRINT.
      advance();
      if (eat(Tok::LParen)) {
        st->stringTarget = parsePrimary();
        expect(Tok::RParen, "(105)");
      } else {
        d_.error(cur().loc, "expected '(' after STRING", "(105)");
      }
      continue;
    }
    if (atWord("LINE") || atWord("COPY")) {
      d_.error(cur().loc, "PUT option " + cur().text + " is not implemented in this stage",
               "(105)");
      resync();
      return nullptr;
    }
    d_.error(cur().loc, "unexpected token in PUT statement", "(105)");
    resync();
    return nullptr;
  }
  expect(Tok::Semi, "(104)");
  (void)sawData;
  return st;
}

// stream-io-statement ::= GET stream-optionslist ;    rules (104)-(109)
// CM3 serves the list-directed form: GET [SKIP] LIST (datalist); reading scalar
// values from SYSIN. FILE/STRING/EDIT/DATA/COPY/LINE/PAGE stay QR2.5.
StmtP Parser::parseGet() {
  auto st = std::make_unique<Stmt>();
  st->kind = Stmt::Get;
  st->loc = cur().loc;
  advance(); // GET
  while (!at(Tok::Semi) && !at(Tok::Eof)) {
    if (atWord("SKIP")) {
      advance();
      st->skip = true;
      if (eat(Tok::LParen)) {
        st->skipCount = parseExpr();
        expect(Tok::RParen, "(105)");
      }
      continue;
    }
    if (atWord("LIST")) {
      advance();
      if (expect(Tok::LParen, "(109)")) {
        if (!at(Tok::RParen)) {
          for (;;) {
            st->items.push_back(parseExpr());
            if (!eat(Tok::Comma))
              break;
          }
        }
        expect(Tok::RParen, "(109)");
      }
      continue;
    }
    if (atWord("FILE")) {
      advance();
      if (eat(Tok::LParen)) {
        if (at(Tok::Word)) {
          st->fileIdent = cur().text;
          advance();
        } else {
          d_.error(cur().loc, "expected a FILE variable after FILE(", "(105)");
        }
        expect(Tok::RParen, "(105)");
      }
      continue;
    }
    if (atWord("STRING")) {
      // STRING ( reference ) option (rule 105): read list-directed input from
      // the character variable instead of SYSIN.
      advance();
      if (eat(Tok::LParen)) {
        st->stringTarget = parsePrimary();
        expect(Tok::RParen, "(105)");
      } else {
        d_.error(cur().loc, "expected '(' after STRING", "(105)");
      }
      continue;
    }
    if (atWord("EDIT")) {
      // Edit-directed transmission (rule (108)): `EDIT ( (datalist) formatlist )`.
      if (!parseEditClause(st.get())) {
        resync();
        return nullptr;
      }
      continue;
    }
    if (atWord("DATA") || atWord("COPY") || atWord("LINE") || atWord("PAGE")) {
      d_.error(cur().loc, "GET option " + cur().text + " is not implemented in this stage",
               "(105)");
      resync();
      return nullptr;
    }
    d_.error(cur().loc, "unexpected token in GET statement", "(105)");
    resync();
    return nullptr;
  }
  expect(Tok::Semi, "(104)");
  return st;
}

// EDIT ( { ( datalist ) formatlist }••• )          rules (108),(45)
// Parses the edit-directed data specification into the statement's data items
// and its paired format list. A bare word inside the datalist is a data item
// (an expression); inside the formatlist it is a format descriptor (A/F/X/...).
bool Parser::parseEditClause(Stmt* st) {
  advance(); // EDIT
  for (;;) {
    // ( datalist )   rules (110),(111)
    if (!expect(Tok::LParen, "(110)"))
      return false;
    if (!at(Tok::RParen)) {
      for (;;) {
        st->items.push_back(parseExpr());
        if (!eat(Tok::Comma))
          break;
      }
    }
    expect(Tok::RParen, "(110)");
    // formatlist  ( {,• format•••} )   rules (45)-(54)
    if (!expect(Tok::LParen, "(45)"))
      return false;
    if (!at(Tok::RParen)) {
      for (;;) {
        if (!parseFormatItem(st))
          return false;
        if (!eat(Tok::Comma))
          break;
      }
    }
    expect(Tok::RParen, "(45)");
    // A further ( datalist ) formatlist group, comma-separated.
    if (!eat(Tok::Comma))
      break;
  }
  st->edit = true;
  return true;
}

// One format item (rules (46)-(54)): a data format (A character, F fixed) or a
// control format (X spacing, SKIP/LINE/PAGE line control), each with an optional
// parenthesised width and, for F, a fractional-digit count.
bool Parser::parseFormatItem(Stmt* st) {
  FormatItem fi;
  if (cur().kind != Tok::Word) {
    d_.error(cur().loc, "expected a format item", "(48)");
    return false;
  }
  const std::string& w = cur().text;
  // Format families recognised but not served by this slice (CM3).
  if (w == "B" || w == "C" || w == "P" || w == "COLUMN" || w == "R") {
    d_.error(cur().loc, "format item '" + w + "' is not implemented in this stage", "(48)");
    return false;
  }
  if (w == "A") {
    fi.kind = FormatItem::A;
  } else if (w == "F") {
    fi.kind = FormatItem::F;
  } else if (w == "E") {
    fi.kind = FormatItem::E;
  } else if (w == "X") {
    fi.kind = FormatItem::X;
  } else if (w == "SKIP") {
    fi.kind = FormatItem::Skip;
  } else if (w == "PAGE") {
    fi.kind = FormatItem::Page;
  } else if (w == "LINE") {
    fi.kind = FormatItem::Line;
  } else {
    d_.error(cur().loc, "'" + w + "' is not a format item", "(48)");
    return false;
  }
  advance();              // the format descriptor word
  if (eat(Tok::LParen)) { // optional ( width [, decimals ] )
    fi.w = parseExpr();
    if ((fi.kind == FormatItem::F || fi.kind == FormatItem::E) && eat(Tok::Comma)) {
      fi.d = parseExpr();
      if (eat(Tok::Comma)) { // the third F operand is a scale factor
        d_.error(cur().loc, "a scale factor on an F format item is not implemented in this stage",
                 "(50)");
        return false;
      }
    }
    expect(Tok::RParen, "(48)");
  }
  st->formats.push_back(std::move(fi));
  return true;
}

// call-statement ::= CALL identifier [argumentlist] ... ;          rule (78)
StmtP Parser::parseCall() {
  auto st = std::make_unique<Stmt>();
  st->kind = Stmt::CallS;
  st->loc = cur().loc;
  advance(); // CALL
  if (!at(Tok::Word)) {
    d_.error(cur().loc, "expected entry name after CALL", "(78)");
    resync();
    return nullptr;
  }
  st->name = cur().text;
  advance();
  if (eat(Tok::LParen)) { // rule (80)
    if (!at(Tok::RParen)) {
      for (;;) {
        st->args.push_back(parseExpr());
        if (!eat(Tok::Comma))
          break;
      }
    }
    expect(Tok::RParen, "(80)");
  }
  expect(Tok::Semi, "(78)");
  return st;
}

// assignment-statement ::= reference = expression ;                rule (86)
// assignment-statement ::= {,• reference•••} = expression [ , BY NAME ]
//                                                          rule (86)
// The comma-separated target list precedes '='; every target receives the value
// of the single RHS expression. The trailing ", BY NAME" is parsed into
// st->byName; sema restricts it to a single whole-structure target.
StmtP Parser::parseAssignment() {
  auto st = std::make_unique<Stmt>();
  st->kind = Stmt::Assign;
  st->loc = cur().loc;
  st->target = parsePrimary();
  if (!st->target) {
    resync();
    return nullptr;
  }
  while (at(Tok::Comma)) {
    advance();
    ExprP more = parsePrimary();
    if (!more) {
      resync();
      return nullptr;
    }
    st->extraTargets.push_back(std::move(more));
  }
  if (!at(Tok::Eq)) {
    d_.error(cur().loc, "expected '=' in assignment statement", "(86)", "= ");
    resync();
    return nullptr;
  }
  advance();
  st->value = parseExpr();
  if (at(Tok::Comma)) {
    advance();
    if (atWord("BY") && peek().isWord("NAME")) {
      st->byName = true;
      advance();
      advance();
    } else {
      d_.error(cur().loc, "malformed assignment: unexpected item after the right-hand side",
               "(86)");
    }
  }
  expect(Tok::Semi, "(86)");
  return st;
}

// ALLOCATE based-allocate-item{,...};  rule (87)
// based-allocate-item ::= identifier ( SET ( reference ) [ IN ( reference ) ]
//                                     | IN ( reference ) [ SET ( reference ) ] )  rule (88)
// CM2 serves the SET option: heap-allocate the based structure's storage and
// store the address in the pointer reference. The IN (AREA) option stays QR2.3.
StmtP Parser::parseAllocate() {
  auto st = std::make_unique<Stmt>();
  st->kind = Stmt::Allocate;
  st->loc = cur().loc;
  advance(); // ALLOCATE
  for (;;) {
    if (at(Tok::Word)) {
      auto base = std::make_unique<Expr>();
      base->kind = Expr::VarRef;
      base->name = cur().text;
      base->loc = cur().loc;
      advance();
      bool paren = eat(Tok::LParen); // optional: identifier ( SET ( ref ) )
      if (atWord("SET")) {
        advance();
        expect(Tok::LParen, "(88)");
        ExprP set = parsePrimary();
        if (!set) {
          resync();
          return nullptr;
        }
        expect(Tok::RParen, "(88)");
        if (paren)
          expect(Tok::RParen, "(88)");
        st->allocBase.push_back(std::move(base));
        st->allocSet.push_back(std::move(set));
      } else {
        d_.error(cur().loc, "ALLOCATE requires the SET ( reference ) option in this stage", "(88)");
        resync();
        return nullptr;
      }
    } else {
      d_.error(cur().loc, "expected an identifier after ALLOCATE", "(87)");
      resync();
      return nullptr;
    }
    if (!eat(Tok::Comma))
      break;
  }
  expect(Tok::Semi, "(87)");
  return st;
}

// FREE ( [reference ->] identifier [ IN ( reference ) ] ){,...};  rule (90)
// CM2 serves the plain based-variable form and the locator-qualified form. The
// IN (AREA) option stays QR2.3.
StmtP Parser::parseFree() {
  auto st = std::make_unique<Stmt>();
  st->kind = Stmt::Free;
  st->loc = cur().loc;
  advance(); // FREE
  for (;;) {
    bool paren = eat(Tok::LParen);
    if (at(Tok::Word)) {
      auto base = std::make_unique<Expr>();
      base->kind = Expr::VarRef;
      base->name = cur().text;
      base->loc = cur().loc;
      advance();
      if (at(Tok::Arrow)) {
        // [reference ->] identifier: the left word is the locator pointer, the
        // right is the based variable.
        auto loc = std::make_unique<Expr>();
        loc->kind = Expr::VarRef;
        loc->name = base->name;
        loc->loc = base->loc;
        advance(); // ->
        if (at(Tok::Word)) {
          base->name = cur().text;
          base->loc = cur().loc;
          base->locPtr = std::move(loc);
          advance();
        } else {
          d_.error(cur().loc, "expected a based variable after '->'", "(90)");
        }
      }
      st->freeBase.push_back(std::move(base));
    } else {
      d_.error(cur().loc, "expected an identifier after FREE", "(90)");
      resync();
      return nullptr;
    }
    if (atWord("IN")) {
      d_.error(cur().loc, "FREE ... IN ( AREA ) is not implemented in this stage", "(90)");
      resync();
      return nullptr;
    }
    if (paren)
      expect(Tok::RParen, "(90)");
    if (!eat(Tok::Comma))
      break;
  }
  expect(Tok::Semi, "(90)");
  return st;
}

// open-statement ::= OPEN {, open-optionslist};    rules (100),(101)
// This stage serves the stream forms: FILE ( f ) plus TITLE ('name') and the
// INPUT/OUTPUT/STREAM/PRINT file-attributes (rule 40). RECORD/KEYED/UPDATE and
// the IDENT/LINESIZE/PAGESIZE/ENVIRONMENT options stay unimplemented.
StmtP Parser::parseOpen() {
  auto st = std::make_unique<Stmt>();
  st->kind = Stmt::Open;
  st->loc = cur().loc;
  advance(); // OPEN
  bool sawFile = false;
  while (!at(Tok::Semi) && !at(Tok::Eof)) {
    if (atWord("FILE")) {
      advance();
      if (eat(Tok::LParen)) {
        if (at(Tok::Word)) {
          st->fileIdent = cur().text;
          advance();
        } else {
          d_.error(cur().loc, "expected a FILE variable after FILE(", "(101)");
        }
        expect(Tok::RParen, "(101)");
      } else {
        d_.error(cur().loc, "expected '(' after FILE", "(101)");
      }
      sawFile = true;
      continue;
    }
    if (atWord("TITLE")) {
      advance();
      if (eat(Tok::LParen)) {
        if (at(Tok::CharLit)) {
          st->openTitle = cur().sval;
          advance();
        } else {
          d_.error(cur().loc, "TITLE requires a character-string constant", "(101)");
        }
        expect(Tok::RParen, "(101)");
      }
      continue;
    }
    if (atWord("INPUT")) {
      st->openInput = true;
      advance();
      continue;
    }
    if (atWord("OUTPUT") || atWord("STREAM") || atWord("PRINT")) {
      advance();
      continue;
    }
    if (atWord("RECORD") || atWord("KEYED") || atWord("UPDATE") || atWord("ENVIRONMENT") ||
        atWord("IDENT") || atWord("LINESIZE") || atWord("PAGESIZE")) {
      d_.error(cur().loc, "OPEN option " + cur().text + " is not implemented in this stage",
               "(101)");
      resync();
      return nullptr;
    }
    d_.error(cur().loc, "unexpected token in OPEN statement", "(101)");
    resync();
    return nullptr;
  }
  if (!sawFile)
    d_.error(st->loc, "OPEN requires a FILE ( f ) option in this stage", "(101)");
  expect(Tok::Semi, "(100)");
  return st;
}

// close-statement ::= CLOSE {, close-optionslist};   rules (102),(103)
StmtP Parser::parseClose() {
  auto st = std::make_unique<Stmt>();
  st->kind = Stmt::Close;
  st->loc = cur().loc;
  advance(); // CLOSE
  bool sawFile = false;
  while (!at(Tok::Semi) && !at(Tok::Eof)) {
    if (atWord("FILE")) {
      advance();
      if (eat(Tok::LParen)) {
        if (at(Tok::Word)) {
          st->fileIdent = cur().text;
          advance();
        } else {
          d_.error(cur().loc, "expected a FILE variable after FILE(", "(103)");
        }
        expect(Tok::RParen, "(103)");
      }
      sawFile = true;
      continue;
    }
    if (atWord("IDENT")) {
      d_.error(cur().loc, "CLOSE option IDENT is not implemented in this stage", "(103)");
      resync();
      return nullptr;
    }
    d_.error(cur().loc, "unexpected token in CLOSE statement", "(103)");
    resync();
    return nullptr;
  }
  if (!sawFile)
    d_.error(st->loc, "CLOSE requires a FILE ( f ) option in this stage", "(103)");
  expect(Tok::Semi, "(102)");
  return st;
}

// ---------------------------------------------------------------------------
// Expressions, rules (115)-(129).
// ---------------------------------------------------------------------------
ExprP Parser::parseExpr(int minPrec) {
  ExprP lhs = parseUnary();
  if (!lhs)
    return nullptr;
  for (;;) {
    Tok op = infixOp(cur());
    int p = precOf(op);
    if (op == Tok::Eof || p == 0 || p < minPrec)
      break;
    SourceLoc loc = cur().loc;
    advance();
    ExprP rhs = parseExpr(p + 1); // all binary operators are left associative
    if (!rhs)
      return nullptr;
    auto e = std::make_unique<Expr>();
    e->kind = Expr::Binary;
    e->op = op;
    e->loc = loc;
    e->a = std::move(lhs);
    e->b = std::move(rhs);
    lhs = std::move(e);
  }
  return lhs;
}

// expression-one ::= primitive | {+|-|¬} expression-one
//                  | primitive ** expression-one                   rule (122)
ExprP Parser::parseUnary() {
  if (at(Tok::Plus) || at(Tok::Minus) || at(Tok::Not) ||
      (at(Tok::Word) && cur().text == "NOT" &&
       (peek().kind == Tok::Word || peek().kind == Tok::Number || peek().kind == Tok::LParen ||
        peek().kind == Tok::CharLit || peek().kind == Tok::BitLit))) {
    Tok op = at(Tok::Word) ? Tok::Not : cur().kind;
    SourceLoc loc = cur().loc;
    advance();
    ExprP operand = parseUnary();
    if (!operand)
      return nullptr;
    if (op == Tok::Plus)
      return operand; // unary plus is the identity
    auto e = std::make_unique<Expr>();
    e->kind = Expr::Unary;
    e->op = op;
    e->loc = loc;
    e->a = std::move(operand);
    return e;
  }
  return parsePower();
}

ExprP Parser::parsePower() {
  ExprP base = parsePrimary();
  if (!base)
    return nullptr;
  if (at(Tok::Power)) {
    SourceLoc loc = cur().loc;
    advance();
    ExprP exp = parseUnary(); // right associative, permits 2**-3
    if (!exp)
      return nullptr;
    auto e = std::make_unique<Expr>();
    e->kind = Expr::Binary;
    e->op = Tok::Power;
    e->loc = loc;
    e->a = std::move(base);
    e->b = std::move(exp);
    return e;
  }
  return base;
}

// primitive-expression ::= (expression) | reference | constant     rule (123)
ExprP Parser::parsePrimary() {
  auto e = std::make_unique<Expr>();
  e->loc = cur().loc;

  // replicated-string-constant ::= ( integer ) simple-string-constant (129)
  // Expand `(3)'AB'` into `'ABABAB'` at parse time; also covers bit strings.
  if (at(Tok::LParen) && peek().kind == Tok::Number && !peek().isFloat &&
      peek(2).kind == Tok::RParen &&
      (peek(3).kind == Tok::CharLit || peek(3).kind == Tok::BitLit)) {
    advance(); // (
    long long count = strtoll(cur().text.c_str(), nullptr, 10);
    advance(); // integer
    advance(); // )
    const Token& lit = cur();
    advance(); // simple-string-constant
    std::string out;
    for (long long i = 0; i < count; ++i)
      out += lit.sval;
    e->kind = lit.kind == Tok::CharLit ? Expr::CharLit : Expr::BitLit;
    e->sval = out;
    return e;
  }

  if (at(Tok::LParen)) {
    advance();
    ExprP inner = parseExpr();
    expect(Tok::RParen, "(123)");
    return inner;
  }
  if (at(Tok::Number)) {
    const Token& t = cur();
    if (t.isFloat) {
      if (t.hasExp) { // exponent form is a FLOAT constant (rule 135)
        e->kind = Expr::FltLit;
        e->fval = strtod(t.text.c_str(), nullptr);
      } else { // a bare fractional literal is an exact FIXED DECIMAL constant
        e->kind = Expr::DecLit;
        parseDecConstant(t.text, *e);
      }
    } else if (t.binaryRadix) {
      e->kind = Expr::IntLit;
      e->ival = strtoll(t.text.c_str(), nullptr, 2);
    } else {
      e->kind = Expr::IntLit;
      e->ival = strtoll(t.text.c_str(), nullptr, 10);
    }
    advance();
    return e;
  }
  if (at(Tok::CharLit)) {
    e->kind = Expr::CharLit;
    e->sval = cur().sval;
    advance();
    return e;
  }
  if (at(Tok::BitLit)) {
    e->kind = Expr::BitLit;
    e->sval = cur().sval;
    advance();
    return e;
  }
  if (at(Tok::Word)) {
    e->kind = Expr::VarRef;
    e->name = cur().text;
    advance();
    if (at(Tok::Arrow)) {
      // Locator-qualified reference P -> X (rule 124): the left reference is a
      // POINTER, the right a based variable X (or X.FIELD). e currently holds
      // the left P — move it into the locator, then parse the right-hand name.
      auto ptr = std::make_unique<Expr>();
      ptr->kind = Expr::VarRef;
      ptr->name = e->name;
      ptr->loc = e->loc;
      advance(); // ->
      if (at(Tok::Word)) {
        e->name = cur().text;
        e->loc = cur().loc;
        e->locPtr = std::move(ptr);
        advance();
      } else {
        d_.error(cur().loc, "expected a based variable after '->'", "(124)");
      }
      // fall through: the X.FIELD member path is collected below
    }
    // A qualified name S.A.B (rule 124): collect the member qualifiers after
    // the base name; sema resolves them against the structure type.
    while (eat(Tok::Dot)) {
      if (at(Tok::Word)) {
        e->path.push_back(cur().text);
        advance();
      } else {
        d_.error(cur().loc, "expected a member name after '.'", "(124)");
        break;
      }
    }
    if (at(Tok::LParen)) { // subscripts or function reference
      e->kind = Expr::Call;
      advance();
      if (!at(Tok::RParen)) {
        for (;;) {
          // A '*' subscript is a cross-section axis marker (rule 126) — the
          // reference selects every index along that axis. It is not an
          // expression; it is recorded as a Star node in the argument list.
          if (at(Tok::Star)) {
            auto star = std::make_unique<Expr>();
            star->kind = Expr::Star;
            star->loc = cur().loc;
            e->args.push_back(std::move(star));
            advance();
          } else {
            e->args.push_back(parseExpr());
          }
          if (!eat(Tok::Comma))
            break;
        }
      }
      expect(Tok::RParen, "(126)");
    }
    // A qualified member after a subscript/call group: `arr(i).x` (rule 124) —
    // the base is an array of structures, so the member qualifiers follow the
    // subscript. Sema resolves the path against the element structure type.
    while (eat(Tok::Dot)) {
      if (at(Tok::Word)) {
        e->path.push_back(cur().text);
        advance();
      } else {
        d_.error(cur().loc, "expected a member name after '.'", "(124)");
        break;
      }
    }
    return e;
  }

  // Double quotes are only a %REPLACE replacement operand (ADR-082).
  if (at(Tok::DqString)) {
    d_.error(cur().loc, "double-quoted strings are only valid as a %REPLACE operand (ADR-082)", "");
    return nullptr;
  }
  d_.error(cur().loc, std::string("expected an expression, found ") + tokName(cur().kind), "(123)");
  return nullptr;
}

// INITIAL itemlist (rules (28)-(31)): a comma-separated list of INITIAL items
// terminated by the enclosing ')'. Called with the opening '(' already consumed;
// consumes the closing ')'.
std::vector<InitItem> Parser::parseInitialList() {
  std::vector<InitItem> out;
  for (;;) {
    if (at(Tok::RParen)) {
      advance();
      break;
    }
    out.push_back(parseInitialItem());
    if (at(Tok::Comma)) {
      advance();
      continue;
    }
    expect(Tok::RParen, "(28)");
    break;
  }
  return out;
}

// One INITIAL item (rules (29)-(31)): a constant value, an iteration factor
// '( n )' over a value or sublist, a '*' repeat-last, or a parenthesised group.
InitItem Parser::parseInitialItem() {
  auto valueStart = [](Tok k) {
    return k == Tok::Number || k == Tok::CharLit || k == Tok::BitLit || k == Tok::Word ||
           k == Tok::Minus || k == Tok::Plus || k == Tok::Star || k == Tok::LParen;
  };
  if (at(Tok::Star)) {
    advance();
    return InitItem{InitItem::Repeat, nullptr, 0, {}};
  }
  if (at(Tok::LParen)) {
    // A leading '(' is an iteration factor '( n )' when a value or sublist
    // follows it; a parenthesised constant '( n )' when ',' or ')' follows;
    // otherwise it is a group '( item, ... )'. Decide without consuming.
    bool iter = peek().kind == Tok::Number && peek(2).kind == Tok::RParen &&
                (peek(3).kind == Tok::LParen || valueStart(peek(3).kind));
    bool parenConst = peek().kind == Tok::Number && peek(2).kind == Tok::RParen &&
                      (peek(3).kind == Tok::Comma || peek(3).kind == Tok::RParen);
    advance(); // (
    if (iter) {
      long long n = strtoll(cur().text.c_str(), nullptr, 10);
      advance(); // n
      advance(); // )
      InitItem it;
      it.kind = InitItem::Iter;
      it.factor = n;
      if (at(Tok::LParen)) {
        advance(); // (
        it.items = parseInitialList();
      } else {
        it.items.push_back(parseInitialItem());
      }
      return it;
    }
    if (parenConst) {
      InitItem v;
      v.kind = InitItem::Value;
      v.value = std::make_unique<Expr>();
      v.value->loc = cur().loc;
      v.value->kind = Expr::IntLit;
      v.value->ival = strtoll(cur().text.c_str(), nullptr, 10);
      advance(); // n
      advance(); // )
      return v;
    }
    InitItem g;
    g.kind = InitItem::Group;
    g.items = parseInitialList(); // consumes the group's ')'
    return g;
  }
  InitItem v;
  v.kind = InitItem::Value;
  v.value = parseExpr();
  return v;
}
