// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
#include "parser.h"
#include <cerrno>
#include <climits>
#include <cstdlib>

// Bounded non-negative integer literal for precisions, scales, lengths and
// levels (static-analysis P2): atoi() is undefined on overflow, so parse
// with strtol and diagnose out-of-range input instead of wrapping silently.
// Returns 0 after diagnosing; callers must not emit code once diagnostics
// failed (the driver skips sema/codegen then, as for every parse error).
static int boundedNonNeg(const std::string& text, SourceLoc loc, Diags& d, const char* what,
                         const char* rule) {
  errno = 0;
  char* end = nullptr;
  long v = strtol(text.c_str(), &end, 10);
  if (errno == ERANGE || end == text.c_str() || *end != '\0' || v < 0 || v > INT_MAX) {
    d.error(loc, std::string(what) + " is out of range", rule);
    return 0;
  }
  return (int)v;
}

// A dimension bound must fit the int a Dim carries (rules (12),(13)): a
// 64-bit bound truncated to int silently declared a zero-element array. The
// bound itself may be negative (a(-3:3) is a valid axis); only the range
// matters.
static bool dimBoundFits(long long v) {
  return v >= INT_MIN && v <= INT_MAX;
}

// Parse an integer literal (rule 135): a digit string with no point or
// exponent. Sets ival via strtoll (saturating, as before — narrow paths keep
// their exact historical behavior) and wideIval to the full value (ADR-191:
// an integer constant's precision is its digit count, so a >18-digit literal
// is genuinely wide and needs i128 storage like a wide DECIMAL literal).
static void parseIntConstant(const std::string& text, Expr& e, int base) {
  PliI128 v = 0;
  for (char c : text) {
    if (c < '0' || c > '9' || c - '0' >= base)
      continue; // radix suffixes and separators are not value digits
    v = v * base + (c - '0');
  }
  e.wideIval = v;
  e.ival = strtoll(text.c_str(), nullptr, base);
}
// point and no exponent, e.g. "3.14". Sets ival to the value scaled by 10^q,
// decScale to the fraction digit count q, and decPrec to the digit count p.
// Sterling constant (rule (145)): convert pounds.shillings.pence[.fraction]£
// to fixed-point pence, exactly as the standard defines the value. The
// pence fraction becomes the decimal scale of the resulting DECIMAL literal.
static bool parseSterlingConstant(const std::string& text, Expr& e) {
  size_t d1 = text.find('.');
  if (d1 == std::string::npos)
    return false;
  size_t d2 = text.find('.', d1 + 1);
  if (d2 == std::string::npos)
    return false;
  std::string poundsS = text.substr(0, d1);
  std::string shillS = text.substr(d1 + 1, d2 - d1 - 1);
  std::string penceS = text.substr(d2 + 1);
  size_t d3 = penceS.find('.');
  std::string penceIntS = d3 == std::string::npos ? penceS : penceS.substr(0, d3);
  std::string penceFracS = d3 == std::string::npos ? "" : penceS.substr(d3 + 1);
  auto digits = [](const std::string& s) {
    return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
  };
  if (!digits(poundsS) || !digits(shillS) || !digits(penceIntS) ||
      (d3 != std::string::npos && !digits(penceFracS)))
    return false;
  // Both subfields are bounded by the constant grammar, so their text is
  // at most two digits; rejecting longer text avoids an out-of-range parse.
  if (shillS.size() > 2 || penceIntS.size() > 2 || penceFracS.size() > 38)
    return false;
  long long shill = std::stoll(shillS);
  long long penceInt = std::stoll(penceIntS);
  if (shill >= 20 || penceInt >= 12)
    return false;

  int scale = (int)penceFracS.size();
  // 240 and 12 widen the pounds field by at most three decimal digits.
  size_t poundsSig = poundsS.find_first_not_of('0');
  size_t poundsDigits = poundsSig == std::string::npos ? 1 : poundsS.size() - poundsSig;
  if (poundsDigits + (size_t)scale + 3 > 38)
    return false;

  PliI128 pounds = 0;
  for (char c : poundsS)
    pounds = pounds * 10 + (c - '0');
  PliI128 penceFrac = 0;
  for (char c : penceFracS)
    penceFrac = penceFrac * 10 + (c - '0');
  PliI128 value = pounds * 240 + shill * 12 + penceInt;
  for (int i = 0; i < scale; ++i)
    value = value * 10;
  value += penceFrac;

  PliU128 mag = pli_mag_s(value);
  int prec = 0;
  do {
    pli_divmod10(&mag);
    ++prec;
  } while (!pli_iszero_u(mag));
  if (prec > 38)
    return false;
  e.kind = Expr::DecLit;
  e.wideIval = value;
  e.ival = pli_to_i64(value);
  e.decScale = scale;
  e.decPrec = prec;
  return true;
}

static void parseDecConstant(const std::string& text, Expr& e) {
  size_t dot = text.find('.');
  std::string intPart = dot == std::string::npos ? text : text.substr(0, dot);
  std::string fracPart = dot == std::string::npos ? "" : text.substr(dot + 1);
  // Accumulate in i128 (ADR-191): a 31-digit scaled value exceeds i64, so the
  // old long long accumulator wrapped wide literals at parse time. PliI128
  // keeps this portable (MSVC lacks __int128; clang-cl supports it natively).
  PliI128 scaled = 0;
  for (char c : intPart)
    scaled = scaled * 10 + (c - '0');
  for (char c : fracPart)
    scaled = scaled * 10 + (c - '0');
  e.wideIval = scaled;
  e.ival = pli_to_i64(scaled);
  e.decScale = (int)fracPart.size();
  e.decPrec = (int)(intPart.size() + fracPart.size());
}

// True when a bare word is a scalar type/attribute keyword (rules (16)-(19)),
// so `RETURNS(fixed)`/`RETURNS(float)` mean a scalar descriptor rather than a
// structure template reference (rule 127).
static bool isScalarTypeWord(const std::string& w) {
  return w == "FIXED" || w == "FLOAT" || w == "BINARY" || w == "BIN" || w == "DECIMAL" ||
         w == "DEC" || w == "CHARACTER" || w == "CHAR" || w == "BIT" || w == "VARYING" ||
         w == "VARYINGZ" || w == "VAR" || w == "VARZ" || w == "ALIGNED" || w == "UNALIGNED" ||
         w == "POINTER" || w == "AREA" || w == "OFFSET";
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

// The five enquiry built-ins (Phase F, ADR-206/207/208) are the only calls
// whose argument list may hold a ': ... :' designator.
static bool isEnquiryName(const std::string& w) {
  return w == "NULL" || w == "DECLARESIZE" || w == "EPSILON" || w == "MAXVAL" ||
         w == "MINVAL";
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
  // Skip one general reference tail: (.|-> Word)* and
  // balanced (...) groups interleaved (rules 124,126).
  // A locator may itself be subscripted, e.g.
  // s.handles(i) -> view.fd = (rule 124 left recursion).
  for (;;) {
    for (;;) {
      bool moved = false;
      while (j + 1 < t_.size() && (t_[j].kind == Tok::Dot || t_[j].kind == Tok::Arrow) &&
             t_[j + 1].kind == Tok::Word) {
        j += 2;
        moved = true;
      }
      if (j < t_.size() && t_[j].kind == Tok::LParen) {
        int depth = 0;
        size_t k = j;
        for (; k < t_.size(); ++k) {
          if (t_[k].kind == Tok::LParen)
            ++depth;
          else if (t_[k].kind == Tok::RParen) {
            if (--depth == 0) {
              ++k;
              break;
            }
          } else if (t_[k].kind == Tok::Semi || t_[k].kind == Tok::Eof)
            return false;
        }
        j = k;
        continue;
      }
      if (!moved)
        break;
    }
    // Multiple assignment targets: A, B, C = (rule 86).
    if (j + 1 < t_.size() && t_[j].kind == Tok::Comma && t_[j + 1].kind == Tok::Word) {
      j += 2;
      continue;
    }
    break;
  }
  return j < t_.size() && t_[j].kind == Tok::Eq;
}

// The words keywordStatement() dispatches on. Only a word with a keyword
// spelling can be ambiguous with the keyword reading, so the speculative
// probe (ADR-004 step 3) is gated on this.
static bool stmtKeywordSpelling(const std::string& w) {
  static const char* const kws[] = {
      "DECLARE", "DCL",   "IF",   "DO",    "BEGIN",   "PUT",    "CALL",   "RETURN",   "STOP",
      "EXIT",    "GET",   "GO",   "GOTO",  "ON",      "SIGNAL", "REVERT", "ALLOCATE", "FREE",
      "OPEN",    "CLOSE", "READ", "WRITE", "REWRITE", "DELETE", "WAIT",   "DELAY",
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
    // DEFINE ALIAS at file scope (extension, ADR-114): `define:` with a
    // colon stays a procedure definition; without one it is an alias.
    if (cur().isWord("DEFINE") && peek().isWord("ALIAS")) {
      if (StmtP d = parseDefineAlias())
        prog_->defines.push_back(std::move(d));
    } else if (cur().isWord("DEFINE") && peek().isWord("CONSTANT")) {
      // DEFINE CONSTANT at file scope is not implemented in this stage
      // (ADR-205): constants live in a procedure frame, like VALUE does.
      d_.error(cur().loc,
               "DEFINE CONSTANT at file scope is not implemented in this stage (ADR-205)", "");
      resync();
    } else {
      parseExternalProcedure();
    }
    if (i_ == before) {
      d_.error(cur().loc, "expected an external procedure", "(1)");
      break;
    }
  }
  return std::move(prog_);
}

// define-alias-statement (extension, ADR-114): DEFINE ALIAS name attrs;
// the remainder parses as one declaration item, so attribute spellings and
// defaults match DECLARE exactly. Only scalar data attributes survive:
// dimensions, INITIAL/VALUE, LIKE/DEFINED/BASED, ENTRY/FILE, and aliases
// of aliases are diagnosed, never silently dropped.
StmtP Parser::parseDefineAlias() {
  advance(); // DEFINE
  if (!atWord("ALIAS")) {
    d_.error(cur().loc, "expected ALIAS after DEFINE (ADR-114)", "");
    resync();
    return nullptr;
  }
  advance(); // ALIAS
  DeclItem item;
  if (!parseDeclItem(item)) {
    resync();
    return nullptr;
  }
  if (!item.typeRef.empty()) {
    d_.error(item.loc, "aliases of aliases are not implemented in this stage (ADR-114)", "");
    resync();
    return nullptr;
  }
  if (!item.ty.dims.empty() || !item.dynBounds.empty() || !item.dynLbBounds.empty()) {
    d_.error(item.loc, "array aliases are not implemented in this stage (ADR-114)", "");
    resync();
    return nullptr;
  }
  if (item.level != 0 || item.init || item.initCall || !item.initItems.empty() || item.valueInit ||
      !item.like.empty() || !item.definedBase.empty() || !item.basedBase.empty() ||
      item.basedNoPtr || item.isEntry || item.fileAttr || !item.entryParams.empty()) {
    d_.error(item.loc, "only scalar data attributes are implemented in DEFINE ALIAS (ADR-114)", "");
    resync();
    return nullptr;
  }
  auto st = std::make_unique<Stmt>();
  st->kind = Stmt::DefineAlias;
  st->loc = item.loc;
  st->name = item.name;
  st->aliasTy = item.ty;
  if (!eat(Tok::Semi)) {
    d_.error(cur().loc, "expected ';' after the DEFINE ALIAS statement (ADR-114)", "");
    resync();
    return nullptr;
  }
  return st;
}

// define-constant-statement (extension, ADR-205): DEFINE CONSTANT name const;
// a textual named constant. The value parses as a general expression so a
// later constant expression stays possible, but sema only accepts a bare
// literal there (mirroring VALUE's single-constant fold).
StmtP Parser::parseDefineConstant() {
  SourceLoc loc = cur().loc;
  advance(); // DEFINE
  advance(); // CONSTANT (the dispatch checked the spelling)
  if (!at(Tok::Word)) {
    d_.error(cur().loc, "expected a constant name after DEFINE CONSTANT (ADR-205)", "");
    resync();
    return nullptr;
  }
  std::string name = cur().text;
  advance();
  ExprP v = parseExpr();
  if (!v) {
    resync();
    return nullptr;
  }
  auto st = std::make_unique<Stmt>();
  st->kind = Stmt::DefineConstant;
  st->loc = loc;
  st->name = name;
  st->value = std::move(v);
  if (!eat(Tok::Semi)) {
    d_.error(cur().loc, "expected ';' after the DEFINE CONSTANT statement (ADR-205)", "");
    resync();
    return nullptr;
  }
  return st;
}

// package-block (extension, ADR-109): name: PACKAGE [EXPORTS (a, ...)];
// declarations and procedures, closed by END [name]. Member procedures
// parse exactly like nested procedures (hoisted with parent set), so END
// matching and multiple closure come from parseBody; sema resolves EXPORTS
// and diagnoses package-level data. Errors continue into the body rather
// than resyncing, so following members keep their package scope.
void Parser::parsePackage(const std::string& name, SourceLoc loc) {
  advance(); // PACKAGE
  Proc* pkg = startProc(name, loc, nullptr);
  pkg->isPackage = true;
  if (atWord("EXPORTS")) {
    advance();
    if (!eat(Tok::LParen)) {
      d_.error(cur().loc, "expected '(' after EXPORTS (ADR-109)", "");
    } else {
      if (!at(Tok::RParen)) {
        for (;;) {
          if (at(Tok::Word)) {
            pkg->exports.push_back(cur().text);
            advance();
          } else {
            d_.error(cur().loc, "expected an exported procedure name (ADR-109)", "");
            break;
          }
          if (!eat(Tok::Comma))
            break;
        }
      }
      if (!eat(Tok::RParen))
        d_.error(cur().loc, "expected ')' after the EXPORTS list (ADR-109)", "");
    }
  }
  if (!eat(Tok::Semi))
    d_.error(cur().loc, "expected ';' after the PACKAGE statement (ADR-109)", "");
  EndInfo e = parseBody(pkg, pkg->body, name);
  if (e.present && !e.label.empty())
    d_.error(e.loc, "END label '" + e.label + "' does not match any open block", "(7)");
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

// Applies a PROCEDURE statement's condition prefix to the body it scopes
// (rules (60)-(63),(95); ADR-320): the entries of procCond_ are 0 for the
// default, 1 when the prefix disables the check, and 2 when CHECK re-enables
// it. Restores the enclosing state when the body ends.
struct ProcCondGuard {
  int* state;
  int saved[7];
  ProcCondGuard(int* s, const Stmt& pre) : state(s) {
    for (int i = 0; i < 7; ++i)
      saved[i] = s[i];
    if (pre.noSize)
      s[0] = 1;
    else if (pre.chkSize)
      s[0] = 2;
    if (pre.noSub)
      s[1] = 1;
    else if (pre.chkSub)
      s[1] = 2;
    if (pre.noZdiv)
      s[2] = 1;
    else if (pre.chkZdiv)
      s[2] = 2;
    if (pre.noConv)
      s[3] = 1;
    else if (pre.chkConv)
      s[3] = 2;
    if (pre.noUnder)
      s[4] = 1;
    else if (pre.chkUnder)
      s[4] = 2;
    if (pre.noOver)
      s[5] = 1;
    else if (pre.chkOver)
      s[5] = 2;
    if (pre.noFOver)
      s[6] = 1;
    else if (pre.chkFOver)
      s[6] = 2;
  }
  ~ProcCondGuard() {
    for (int i = 0; i < 7; ++i)
      state[i] = saved[i];
  }
};

// procedure ::= [prefixlist] entry-namelist PROCEDURE [(parameterlist)]
//               [procedure-optionslist] sentencelist                rule (2)
void Parser::parseExternalProcedure() {
  // procedure ::= [prefixlist] entry-namelist PROCEDURE ... (rule (2)): the
  // prefix list belongs to the PROCEDURE statement and scopes its body.
  auto pre = std::make_unique<Stmt>();
  pre->loc = cur().loc;
  if (!parseCondPrefixes(pre.get())) {
    resync();
    return;
  }
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
  if (atStmtKeyword("PACKAGE")) {
    // Package block (extension, ADR-109): member procedures nest under the
    // package scope; EXPORTS controls linkage (resolved in sema).
    if (!entryNames.empty())
      d_.error(cur().loc, "only a single name is allowed on a PACKAGE statement (ADR-109)", "");
    parsePackage(name, loc);
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
  // Apply the procedure statement's condition prefix to its body.
  int outerStr = procStrRange_;
  if (pre->strRange != 0)
    procStrRange_ = pre->strRange;
  ProcCondGuard condGuard(procCond_, *pre);
  parseProcBody(p);
  procStrRange_ = outerStr;
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
    // RECURSIVE ::= the procedure may be invoked recursively (rule (5)).
    // Recorded for the sema cycle check; codegen needs no change since
    // every activation already owns its AUTOMATIC storage.
    if (eatWord("RECURSIVE")) {
      p->isRecursive = true;
      continue;
    }
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
          if (p->retTy.isBit() && p->retTy.len > 1) {
            // Function results ride in i8 registers below; never declare a
            // wide bit result that codegen would mistarget (invariant 2).
            d_.error(p->loc, "RETURNS of a BIT(n>1) value is not implemented in this stage",
                     "(34)");
            p->retTy = Type::bit(1);
          } else if (p->retTy.starLen && !p->retTy.varying) {
            // RETURNS of adjustable non-VARYING CHARACTER stays out (rule
            // (18)/(34)); CHAR(*) VARYING is served with a caller-sized max.
            d_.error(p->loc, "RETURNS of CHAR(*) is not implemented in this stage", "(34)");
            p->retTy = Type::chr(1);
          }
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
  if (blockDepth_ >= kMaxBlockDepth) {
    d_.error(cur().loc, "block nesting is too deep", "(68)");
    return {};
  }
  DepthGuard g(blockDepth_);
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

// condition-prefix-list (rules (60)-(63)): `(cond [, cond]*) :` precedes the
// statement it governs; a PROCEDURE statement's prefix covers its body.
bool Parser::parseCondPrefixes(Stmt* st) {
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
    if (!at(Tok::Colon)) {
      d_.error(l, "condition prefix must end with ':'", "(60)");
      return false;
    }
    advance();
    // Prefix elements (rules (60)-(63)): the plain forms are no-ops because
    // SIZE, SUBSCRIPTRANGE, ZERODIVIDE, CONVERSION, UNDERFLOW, OVERFLOW and
    // FIXEDOVERFLOW are on by default, the NO... forms disable them for the
    // statement's scope, and STRINGRANGE is IBM's opt-in condition (ADR-297).
    // CHECK(...)/NOCHECK(...)
    // (rules (95),(96), ADR-320) name conditions — or ALL — and turn them on
    // or off regardless of what an enclosing prefix disabled. Any other
    // condition keeps the unenforced warning.
    bool anyWord = false, allHandled = true;
    bool checkMode = false, noCheckMode = false, named = false;
    // Apply one state to one implemented check: 1 enables (and clears a
    // disable), -1 disables (and clears an enable); `which` -1 means ALL.
    auto setCond = [&](int which, int state) {
      for (int c = (which < 0 ? 0 : which); c <= (which < 0 ? 7 : which); ++c) {
        switch (c) {
        case 0: st->noSize = state < 0; st->chkSize = state > 0; break;
        case 1: st->noSub = state < 0; st->chkSub = state > 0; break;
        case 2: st->noZdiv = state < 0; st->chkZdiv = state > 0; break;
        case 3: st->noConv = state < 0; st->chkConv = state > 0; break;
        case 4: st->noUnder = state < 0; st->chkUnder = state > 0; break;
        case 5: st->strRange = state; break;
        case 6: st->noOver = state < 0; st->chkOver = state > 0; break;
        default: st->noFOver = state < 0; st->chkFOver = state > 0; break;
        }
      }
    };
    for (size_t k = save; k < i_; ++k) {
      if (t_[k].kind != Tok::Word)
        continue;
      anyWord = true;
      std::string w = t_[k].text;
      if (w == "CHECK") {
        checkMode = true;
        noCheckMode = false;
        named = false;
        continue;
      }
      if (w == "NOCHECK") {
        noCheckMode = true;
        checkMode = false;
        named = false;
        continue;
      }
      // Classify the word: a condition name, its NO... form, or ALL.
      int state = 0;
      if (w == "NOSIZE" || w == "NOSUBSCRIPTRANGE" || w == "NOZERODIVIDE" ||
          w == "NOCONVERSION" || w == "NOSTRINGRANGE" || w == "NOUNDERFLOW" ||
          w == "NOOVERFLOW" || w == "NOFIXEDOVERFLOW") {
        w = w.substr(2);
        state = -1;
      }
      int which;
      if (w == "SIZE")
        which = 0;
      else if (w == "SUBSCRIPTRANGE")
        which = 1;
      else if (w == "ZERODIVIDE")
        which = 2;
      else if (w == "CONVERSION")
        which = 3;
      else if (w == "UNDERFLOW")
        which = 4;
      else if (w == "STRINGRANGE")
        which = 5;
      else if (w == "OVERFLOW")
        which = 6;
      else if (w == "FIXEDOVERFLOW")
        which = 7;
      else if (w == "ALL")
        which = -1;
      else {
        allHandled = false;
        continue;
      }
      // CHECK/NOCHECK turn a condition on/off; a plain prefix only disables
      // (the checks are on by default), except STRINGRANGE, which it enables.
      if (state == 0) {
        if (checkMode)
          state = 1;
        else if (noCheckMode)
          state = -1;
        else if (which < 0) { // (ALL) is not a prefix element
          allHandled = false;
          continue;
        } else
          state = which == 5 ? 1 : 0;
      }
      if (checkMode || noCheckMode)
        named = true;
      if (state != 0)
        setCond(which, state);
    }
    // NOCHECK with no list disables every implemented check (rule (96)).
    if (noCheckMode && !named)
      setCond(-1, -1);
    if (!anyWord || !allHandled)
      d_.warn(l, "condition prefixes are parsed but not yet enforced", "(60)");
  }
  return true;
}

StmtP Parser::parseStatement(Proc* owner) {
  auto st = std::make_unique<Stmt>();
  st->loc = cur().loc;
  // A PROCEDURE-level prefix covers every statement in its body.
  st->strRange = procStrRange_;
  st->noSize = procCond_[0] == 1;
  st->noSub = procCond_[1] == 1;
  st->noZdiv = procCond_[2] == 1;
  st->noConv = procCond_[3] == 1;
  st->noUnder = procCond_[4] == 1;
  st->noOver = procCond_[5] == 1;
  st->noFOver = procCond_[6] == 1;

  // condition prefix list                                          rule (60)
  if (!parseCondPrefixes(st.get()))
    return nullptr;
  // A prefix governs its statement and every body nested in it (rules
  // (60)-(63)) — including ON-unit bodies, which codegen emits outside the
  // enclosing group. The guard restores the outer scope on return.
  struct RangeGuard {
    int& state;
    int saved;
    ~RangeGuard() { state = saved; }
  } rangeGuard{procStrRange_, procStrRange_};
  if (st->strRange != 0)
    procStrRange_ = st->strRange;

  // label prefixes                                    rules (64),(65)
  // A plain identifier labels the statement with a label constant; an element
  // of a label array (initial-label, AR(1):) labels it through that element.
  while (at(Tok::Word)) {
    if (peek().kind == Tok::Colon) {
      st->labels.push_back(cur().text);
      advance();
      advance();
      continue;
    }
    if (peek().kind != Tok::LParen)
      break;
    // Try `ident ( signed-integer {, signed-integer} ) :`; anything else is the
    // statement itself (a call or an assignment), so put the tokens back.
    size_t save = i_;
    std::string name = cur().text;
    advance();
    advance(); // (
    std::vector<long long> subs;
    bool ok = true;
    while (ok) {
      long long sign = 1;
      if (at(Tok::Plus)) {
        advance();
      } else if (at(Tok::Minus)) {
        sign = -1;
        advance();
      }
      if (!at(Tok::Number) || cur().isFloat || cur().sterling || cur().imaginary) {
        ok = false;
        break;
      }
      subs.push_back(sign * std::strtoll(cur().text.c_str(), nullptr, 10));
      advance();
      if (eat(Tok::Comma))
        continue;
      break;
    }
    if (ok && at(Tok::RParen) && peek().kind == Tok::Colon) {
      advance(); // )
      advance(); // :
      LabelElem e;
      e.name = name;
      e.subs = std::move(subs);
      st->labelElems.push_back(std::move(e));
      continue;
    }
    i_ = save;
    break;
  }

  // nested package (extension, ADR-109): packages nest nothing; a labelled
  // PACKAGE inside a body is diagnosed, never silently accepted.
  if (atStmtKeyword("PACKAGE") && !st->labels.empty()) {
    d_.error(cur().loc, "nested packages are not implemented in this stage (ADR-109)", "");
    resync();
    return nullptr;
  }

  // nested procedure                                          rules (2),(8)
  if ((atStmtKeyword("PROCEDURE") || atStmtKeyword("PROC")) && !st->labels.empty()) {
    advance();
    Proc* p = startProc(st->labels.front(), st->loc, owner);
    parseProcOptions(p);
    // The prefixlist scopes the nested body the same way it scopes an
    // external procedure's body (rules (2),(60)).
    int outerStr = procStrRange_;
    if (st->strRange != 0)
      procStrRange_ = st->strRange;
    ProcCondGuard condGuard(procCond_, *st);
    parseProcBody(p);
    procStrRange_ = outerStr;
    return nullptr;   // procedures are hoisted into Program::procs
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
    if (s) {
      // Condition prefixes (rules (60)-(63), ADR-110) ride with labels.
      s->noSize = st->noSize;
      s->noSub = st->noSub;
      s->noZdiv = st->noZdiv;
      s->noConv = st->noConv;
      s->noUnder = st->noUnder;
      s->noOver = st->noOver;
      s->noFOver = st->noFOver;
      s->chkSize = st->chkSize;
      s->chkSub = st->chkSub;
      s->chkZdiv = st->chkZdiv;
      s->chkConv = st->chkConv;
      s->chkUnder = st->chkUnder;
      s->chkOver = st->chkOver;
      s->chkFOver = st->chkFOver;
      s->strRange = st->strRange;
      s->labelElems = st->labelElems;
      return s;
    }
  }

  StmtP k = keywordStatement(owner, st->labels, /*probe=*/false);
  if (k) {
    k->labels = st->labels;
    k->labelElems = st->labelElems;
    k->noSize = st->noSize;
    k->noSub = st->noSub;
    k->noZdiv = st->noZdiv;
    k->noConv = st->noConv;
    k->noUnder = st->noUnder;
    k->noOver = st->noOver;
    k->noFOver = st->noFOver;
    k->chkSize = st->chkSize;
    k->chkSub = st->chkSub;
    k->chkZdiv = st->chkZdiv;
    k->chkConv = st->chkConv;
    k->chkUnder = st->chkUnder;
    k->chkOver = st->chkOver;
    k->chkFOver = st->chkFOver;
    k->strRange = st->strRange;
    return k;
  }

  auto s = parseAssignment();
  if (s) {
    s->labels = st->labels;
    s->labelElems = st->labelElems;
    s->noSize = st->noSize;
    s->noSub = st->noSub;
    s->noZdiv = st->noZdiv;
    s->noConv = st->noConv;
    s->noUnder = st->noUnder;
    s->noOver = st->noOver;
    s->noFOver = st->noFOver;
    s->chkSize = st->chkSize;
    s->chkSub = st->chkSub;
    s->chkZdiv = st->chkZdiv;
    s->chkConv = st->chkConv;
    s->chkUnder = st->chkUnder;
    s->chkOver = st->chkOver;
    s->chkFOver = st->chkFOver;
    s->strRange = st->strRange;
  }
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
  if (kw("DEFINE")) {
    // DEFINE ALIAS (ADR-114) and DEFINE CONSTANT (ADR-205) share the DEFINE
    // head; anything else falls into the alias parse for its diagnostic.
    // peek() only reads ahead, so it is safe in both probe and real modes.
    if (peek().isWord("CONSTANT"))
      return parseDefineConstant();
    return parseDefineAlias();
  } // extensions (ADR-114, ADR-205)
  if (kw("IF")) {
    return parseIf(owner);
  } // rules (74),(75)
  if (kw("DO")) {
    return parseDo(owner, labels);
  } // rules (69)-(73)
  if (kw("SELECT")) {
    return parseSelect(owner);
  } // extension (ADR-104)
  if (kw("LEAVE")) {
    return parseLoopExit(false);
  } // extension (ADR-105)
  if (kw("ITERATE")) {
    return parseLoopExit(true);
  } // extension (ADR-105)
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
  if (kw("FORMAT")) {
    return parseFormatStmt(labels);
  } // rule (44)
  if (kw("PUT")) {
    return parsePut();
  } // rules (104)-(109)
  if (kw("DISPLAY")) {
    return parseDisplay();
  } // rule (114)
  if (kw("CALL")) {
    return parseCall();
  } // rules (78)-(80)
  if (kw("WAIT")) {
    return parseWait();
  } // rule (82)
  if (kw("DELAY")) {
    return parseDelay();
  } // rule (83)
  if (kw("RETURN")) { // rule (81)
    auto st = std::make_unique<Stmt>();
    st->loc = cur().loc;
    advance();
    st->kind = Stmt::Return;
    if (eat(Tok::LParen)) { // RETURN(value) — function value, rule (81)
      st->value = parseExprOrPlaceholder();
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
     } else if (at(Tok::LParen)) {
       // GO TO ( expr ) — indirect jump through a LABEL variable (rule (77),
       // rule (21)): the operand is a label variable holding a block address.
       advance(); // (
       st->gotoTarget = parseExprOrPlaceholder();
       expect(Tok::RParen, "(77)");
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
   } else if (at(Tok::LParen)) {
     advance(); // (
     st->gotoTarget = parseExprOrPlaceholder();
     expect(Tok::RParen, "(77)");
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
  if (kw("READ") || kw("WRITE") || kw("REWRITE") || kw("DELETE"))
    return parseRecordIO(); // rules (112),(113): sequential record transfer/update
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

// One condition (rule 94). ERROR, SIZE, SUBSCRIPTRANGE, ZERODIVIDE,
// CONVERSION, STRINGRANGE, FINISH, AREA, UNDERFLOW, OVERFLOW and
// FIXEDOVERFLOW are served; every other condition is diagnosed with its rule
// number, never silently accepted.
std::string Parser::parseCondition(std::string* fileArg) {
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
  if (w == "SIZE") {
    advance();
    return "SIZE";
  }
  if (w == "SUBSCRIPTRANGE" || w == "ZERODIVIDE") {
    advance();
    return w;
  }
  if (w == "CONVERSION") {
    advance();
    return w;
  }
  if (w == "STRINGRANGE") {
    // IBM's opt-in condition (rules (60)-(63),(94)): detection is enabled by
    // a (STRINGRANGE) prefix, but ON/SIGNAL/REVERT are always accepted.
    advance();
    return w;
  }
  if (w == "FINISH") {
    // FINISH (rule 94): raised in a task when the task's procedure returns
    // (ADR-322); ON/SIGNAL/REVERT are accepted and the runtime raises it.
    advance();
    return w;
  }
  if (w == "AREA") {
    // AREA (rule 94): raised by the region runtime when an ALLOCATE cannot be
    // satisfied or a pointer does not belong to the region (ADR-323).
    advance();
    return w;
  }
  if (w == "UNDERFLOW") {
    // UNDERFLOW (rule 94): a floating-point result below the smallest normal
    // is set to zero and the condition is raised (ADR-326).
    advance();
    return w;
  }
  if (w == "OVERFLOW") {
    // OVERFLOW (rule 94): a floating-point result larger in magnitude than the
    // double range; the implicit action is a message plus ERROR (ADR-333).
    advance();
    return w;
  }
  if (w == "FIXEDOVERFLOW") {
    // FIXEDOVERFLOW (rule 94): a FIXED DECIMAL arithmetic result longer than
    // the 31-digit maximum; the implicit action is a message plus ERROR
    // (ADR-334).
    advance();
    return w;
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
    if (name == "ERROR" || name == "SIZE" || name == "SUBSCRIPTRANGE" || name == "ZERODIVIDE" ||
        name == "CONVERSION" || name == "STRINGRANGE" || name == "FINISH" ||
        name == "AREA" || name == "UNDERFLOW" || name == "OVERFLOW" ||
        name == "FIXEDOVERFLOW") {
      d_.error(nl, name + " is not a valid programmer-named condition", "(99)");
      return "";
    }
    return name;
  }
  if (w == "ENDFILE" || w == "UNDEFINEDFILE") {
    // ENDFILE(file) and UNDEFINEDFILE(file) (rule 97) are file conditions:
    // each names its file, and sema keys the handler per file slot
    // (Stmt::kEndfileBaseCondKey / kUndefinedfileBaseCondKey).
    std::string cond = w;
    advance();
    if (!at(Tok::LParen)) {
      d_.error(l, cond + " requires a FILE ( f ) reference", "(97)");
      return "";
    }
    advance(); // (
    if (!at(Tok::Word)) {
      d_.error(cur().loc, "expected a FILE variable after " + cond + "(", "(97)");
      return "";
    }
    if (fileArg)
      *fileArg = cur().text;
    advance();
    expect(Tok::RParen, "(97)");
    return cond;
  }
  if (w == "ENDPAGE" || w == "KEY" || w == "NAME" ||
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
  st->condName = parseCondition(&st->fileIdent);
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
  st->condName = parseCondition(&st->fileIdent);
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
  st->condName = parseCondition(&st->fileIdent);
  if (st->condName.empty()) {
    resync();
    return nullptr;
  }
  // SIGNAL ... SET ONCODE(expr) (rule (93)): the expression value is stored
  // as ONCODE before the unit runs.
  if (atWord("SET")) {
    advance();
    if (!atWord("ONCODE")) {
      d_.error(cur().loc, "expected ONCODE after SET in a SIGNAL statement", "(93)");
      resync();
      return nullptr;
    }
    advance();
    if (!expect(Tok::LParen, "(93)")) {
      resync();
      return nullptr;
    }
    st->oncodeExpr = parseExprOrPlaceholder();
    if (!expect(Tok::RParen, "(93)")) {
      resync();
      return nullptr;
    }
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
      parseDeclTail(base);
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
        item.entryByValue = base.entryByValue;
        item.controlled = base.controlled;
        item.like = base.like;
        item.picture = base.picture;
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

// The scale factor of a FIXED attribute must fit the precision (rule (16)):
// FIXED DECIMAL(p,q) takes q <= p decimal digits; FIXED BINARY(p,q) takes
// q < p binary digits (the sign bit takes one of the p). A scale wider than
// the precision cannot be represented, and the runtime's fixed-point
// formatters sized their buffers from the scale alone.
bool Parser::fixedScaleFits(const AttrBag& bag, bool binary) {
  if (bag.prec <= 0)
    return true;
  return binary ? bag.scale < bag.prec : bag.scale <= bag.prec;
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
      n1 = boundedNonNeg(cur().text, cur().loc, d_, "precision", rule);
      advance();
    }
    if (eat(Tok::Comma) && at(Tok::Number)) {
      n2 = boundedNonNeg(cur().text, cur().loc, d_, "precision", rule);
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
    if (at(Tok::LParen) && peek().kind == Tok::Star) {
      // A '*' length (rule (18)) is an adjustable extent; recorded for sema
      // to diagnose where it is not served, never silently defaulted.
      advance();
      advance();
      bag.starLen = true;
      expect(Tok::RParen, rule);
    } else if (at(Tok::LParen)) {
      // A constant length `CHAR(n)` folds to the number; a runtime length
      // `CHAR(expr)` is an adjustable extent whose value the caller supplies
      // at call time (rule (18)), valid only on a parameter (checked in sema).
      advance();
      if (at(Tok::Number) && !cur().isFloat) {
        a = boundedNonNeg(cur().text, cur().loc, d_, "precision", rule);
        advance();
        expect(Tok::RParen, rule);
        if (a > 0)
          bag.slen = a;
      } else {
        // A runtime length `CHAR(expr)` (rule (18)): an adjustable length whose
        // value the caller supplies at call time, like `CHAR(*)`; the expr is
        // typed for validation in sema.
        ExprP len = parseExprOrPlaceholder();
        expect(Tok::RParen, rule);
        bag.slenExpr = std::move(len);
        bag.starLen = true;
      }
    }
    return true;
  }
  if (w == "BIT") {
    bag.bit = true;
    advance();
    if (at(Tok::LParen) && peek().kind == Tok::Star) {
      // Same adjustable-extent marker as CHARACTER (rule (18)).
      advance();
      advance();
      bag.starLen = true;
      expect(Tok::RParen, rule);
    } else if (at(Tok::LParen)) {
      parenNums(a, b);
      if (a > 0)
        bag.slen = a;
    }
    return true;
  }
  if (w == "VARYINGZ" || w == "VARZ") {
    // VARYINGZ (IBM Enterprise PL/I extension, ADR-168): a NUL-terminated
    // varying CHARACTER string. Reuse VARYING storage/operations and mark the
    // extra flag; the by-value C ABI boundary NUL-terminates. `VARZ` is the
    // IBM abbreviation, mirroring VARYING/VAR.
    bag.varying = true;
    bag.varyingz = true;
    advance();
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
  if (w == "CONTROLLED" || w == "CTL") {
    bag.controlled = true;
    advance();
    return true;
  }
  return false;
}

// declaration ::= [integer] identifier [dimension] [attribute•••]  rule (11)
bool Parser::parseDeclItem(DeclItem& item) {
  if (at(Tok::Number) && !cur().isFloat) {
    item.level = boundedNonNeg(cur().text, cur().loc, d_, "level number", "(11)");
    if (item.level > kMaxStructureLevel) {
      d_.error(cur().loc, "a level number must not exceed 49", "(11)");
      item.level = 0;
    }
    advance();
  }
  if (!at(Tok::Word)) {
    d_.error(cur().loc, "expected a name in DECLARE", "(11)");
    return false;
  }
  item.name = cur().text;
  item.loc = cur().loc;
  advance();
  parseDeclTail(item);
  return true;
}

// Dimension + attribute tail of a declaration (rule 11), shared verbatim by a
// factored declaration list (DECLARE (A, B) FIXED): the dimension attribute
// first, then the attribute bag, ending by building item.ty / dims / init.
void Parser::parseDeclTail(DeclItem& item) {
  // Dimension attribute (rules (12),(13)): a leading parenthesised group after
  // the name is a dimension when a bound-pair ':' is present or an attribute
  // keyword follows; otherwise it is a precision/length (M0 scalar behaviour).
  std::vector<Dim> arrDims;
  std::vector<ExprP> arrDyn;
  std::vector<ExprP> arrDynLb;
  tryParseDimension(arrDims, arrDyn, arrDynLb);

  // Attribute bag (rules 14-32).
  AttrBag bag;
  SourceLoc staticLoc; // where STATIC was written, for the deferred no-op warning
  ExprP init;

  for (;;) {
    if (parseScalarAttr(bag, "(16)"))
      continue;
    if (at(Tok::Word)) {
      const std::string& w = cur().text;
      if (w == "INITIAL" || w == "INIT") {
        if (item.valueInit)
          d_.error(cur().loc, "VALUE cannot be combined with INITIAL or a second VALUE (ADR-108)",
                   "");
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
                    // A '*' argument omits an OPTIONAL parameter (extension),
                    // as in a CALL statement (rule (78)).
                    if (at(Tok::Star)) {
                      auto star = std::make_unique<Expr>();
                      star->kind = Expr::Star;
                      star->loc = cur().loc;
                      call->args.push_back(std::move(star));
                      advance();
                    } else {
                      call->args.push_back(parseExprOrPlaceholder());
                    }
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
            // Copy (not move) the pointer: member-INIT gathering reads
            // initItems, and a moved-from unique_ptr would leave a null.
            if (item.initItems.size() == 1 && item.initItems[0].kind == InitItem::Value &&
                item.initItems[0].value)
              init = cloneExpr(item.initItems[0].value.get());
          }
        }
        continue;
      }
      if (w == "TYPE") {
        // TYPE <alias> (extension, ADR-114): the declaration takes its type
        // from a defined alias. Combining it with explicit data attributes
        // is diagnosed at the end of the item (either order); dimensions
        // are kept from this declaration (arrays of aliased elements).
        advance();
        if (!at(Tok::Word)) {
          d_.error(cur().loc, "expected an alias name after TYPE (ADR-114)", "");
        } else {
          item.typeRef = cur().text;
          advance();
        }
        continue;
      }
      if (w == "VALUE") {
        // VALUE(const) (extension, ADR-108): a named constant. The single
        // constant is stored like a scalar INITIAL; combining it with
        // INITIAL (or a second VALUE) is diagnosed, never merged silently.
        advance();
        if (!eat(Tok::LParen)) {
          d_.error(cur().loc, "expected '(' after VALUE (ADR-108)", "");
          continue;
        }
        ExprP v = parseExpr();
        if (!eat(Tok::RParen)) {
          d_.error(cur().loc, "expected ')' after the VALUE constant (ADR-108)", "");
          continue;
        }
        if (item.init || item.initCall || !item.initItems.empty() || item.valueInit)
          d_.error(item.loc, "VALUE cannot be combined with INITIAL or a second VALUE (ADR-108)",
                   "");
        else
          item.valueInit = std::move(v);
        continue;
      }
      if (w == "OPTIONAL") {
        // OPTIONAL (extension): the parameter may be omitted at a call
        // (passed '*' or left out when trailing). Validity (parameters
        // only) is checked in sema, which sees the parameter list.
        advance();
        item.optional = true;
        continue;
      }
      if (w == "VARIABLE") {
        // ENTRY ... VARIABLE (IBM Enterprise PL/I extension, ADR-171): the
        // declared name is an assignable procedure value (an entry variable)
        // rather than a procedure constant. Validity (ENTRY only, scalar,
        // no INITIAL) is checked in sema.
        advance();
        item.entryVariable = true;
        continue;
      }
      if (w == "ALIGNED") {
        // ALIGNED (rule (15)) is the default LLVM struct layout; an explicit
        // ALIGNED re-aligns a member inside an UNALIGNED structure (ADR-169).
        bag.aligned = true;
        advance();
        continue;
      }
      if (w == "UNALIGNED") {
        // UNALIGNED (IBM Enterprise PL/I extension, ADR-169): pack a structure's
        // members with no inter-member padding to match a C packed struct.
        bag.unaligned = true;
        advance();
        continue;
      }
      if (w == "STATIC" || w == "AUTOMATIC" || w == "AUTO" || w == "INTERNAL" || w == "INT") {
        // STATIC is the only storage attribute with an effect: a static AREA
        // keeps its region for the whole program (rule (20)). It still has no
        // effect on other variables, but the warning waits until the whole
        // declaration is read so an AREA attribute can suppress it.
        if (w == "STATIC") {
          bag.staticAttr = true;
          staticLoc = cur().loc;
        } else {
          d_.warn(cur().loc, "attribute " + w + " is accepted but has no effect in this stage",
                  "(15)");
        }
        advance();
        continue;
      }
      if (w == "EXTERNAL" || w == "EXT") {
        // EXTERNAL (rule (42)): the variable is a module-wide (linkage) name,
        // shared with other object files. The extended external-name form
        // EXTERNAL('symbol') gives the exact, case-sensitive C symbol for
        // interlanguage calls (see z/OS ILC).
        item.external = true;
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
      if (w == "OPTIONS") {
        // ENTRY OPTIONS (rule (34)): LINKAGE(SYSTEM) and BYVALUE both select
        // C-ABI by-value scalar marshalling, validated against ENTRY in sema
        // (order-independent with the ENTRY keyword itself).
        advance();
        if (expect(Tok::LParen, "(34)")) {
          for (;;) {
            if (atWord("BYVALUE")) {
              item.entryByValue = true;
              advance();
            } else if (atWord("LINKAGE")) {
              advance();
              if (expect(Tok::LParen, "(34)")) {
                if (!atWord("SYSTEM"))
                  d_.error(cur().loc, "LINKAGE other than SYSTEM is not implemented in this stage",
                           "(34)");
                else {
                  advance();
                  item.entryByValue = true;
                }
                expect(Tok::RParen, "(34)");
              }
            } else {
              std::string opt = cur().kind == Tok::Word ? cur().text : tokName(cur().kind);
              d_.warn(cur().loc, "ignoring unsupported entry option '" + opt + "'", "(34)");
              advance();
            }
            if (!eat(Tok::Comma))
              break;
          }
          expect(Tok::RParen, "(34)");
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
          if (item.entryRetTy.starLen && !item.entryRetTy.varying) {
            d_.error(item.loc, "RETURNS of CHAR(*) is not implemented in this stage", "(34)");
            item.entryRetTy = Type::chr(1);
          }
          expect(Tok::RParen, "(34)");
        }
        continue;
      }
      if (w == "USES" || w == "SETS") {
        // entry-name-attribute { USES | SETS } ( {,• uses-sets-item••• } )
        // (rules (34),(38)): interface metadata naming the variables the entry
        // uses or sets. It feeds interprocedural analysis, which this compiler
        // does not perform, so the list rides the symbol without changing
        // generated code; the item shape is still checked (a name, a level
        // number, or '*').
        bool isUses = (w == "USES");
        advance();
        if (expect(Tok::LParen, "(34)")) {
          auto& list = isUses ? item.uses : item.sets;
          for (;;) {
            std::string text;
            if (at(Tok::Star)) {
              text = "*";
              advance();
            } else if (at(Tok::Number)) {
              if (cur().isFloat || cur().sterling || cur().imaginary)
                d_.error(cur().loc, "a USES/SETS level must be an integer constant", "(38)");
              text = cur().text;
              advance();
            } else if (at(Tok::Word)) {
              // uses-sets-item ::= unsubscripted-reference | integer | * (38):
              // the reference parser rejects a subscript below.
              ExprP ref = parsePrimary();
              if (!ref)
                break;
              if (ref->kind != Expr::VarRef || ref->locPtr) {
                d_.error(ref->loc, "a USES/SETS item must be an unsubscripted reference",
                         "(38)");
              } else {
                text = ref->name;
                for (const std::string& q : ref->path)
                  text += "." + q;
              }
            } else {
              d_.error(cur().loc, "expected a name, a level number, or '*' in USES/SETS",
                       "(38)");
              break;
            }
            if (!text.empty())
              list.push_back(text);
            if (!eat(Tok::Comma))
              break;
          }
          expect(Tok::RParen, "(34)");
        }
        continue;
      }
      if (w == "ABNORMAL" || w == "NORMAL") {
        // entry-name-attribute { ABNORMAL ( label-name ) | NORMAL } (rule (38)):
        // ABNORMAL names the label control passes to when a CALL through this
        // entry returns, in place of the statement after the CALL. NORMAL names
        // the ordinary return point and cancels an inherited ABNORMAL.
        bool isAbnormal = (w == "ABNORMAL");
        advance();
        if (isAbnormal) {
          if (expect(Tok::LParen, "(38)")) {
            if (at(Tok::Word)) {
              item.abnormalLabel = cur().text;
              advance();
            } else {
              d_.error(cur().loc, "ABNORMAL requires a label name", "(38)");
            }
            expect(Tok::RParen, "(38)");
          }
        } else {
          item.normal = true;
        }
        continue;
      }
      if (w == "REDUCIBLE" || w == "IRREDUCIBLE") {
        // entry-name-attribute { REDUCIBLE | IRREDUCIBLE } (rule (34)): an
        // optimization hint about repeating a call with the same arguments.
        // The declaration is a hint, not a constraint, so a later attribute
        // overrides an earlier one; validity (an ENTRY declaration) is
        // checked in sema.
        item.reducible = (w == "REDUCIBLE") ? 1 : -1;
        advance();
        continue;
      }
      if (w == "LIKE") {
        // like-attribute ::= LIKE unsubscripted-reference (rule 43): the
        // declared item takes the structure shape of the referenced structure.
        // A qualified template LIKE S.A.B narrows to a minor structure; the
        // dotted path is resolved in sema.
        advance();
        if (at(Tok::Word)) {
          item.like = cur().text;
          advance();
          while (at(Tok::Dot)) {
            advance();
            if (at(Tok::Word)) {
              item.like += "." + cur().text;
              advance();
            } else {
              d_.error(cur().loc, "expected a member name after '.' in LIKE reference", "(43)");
              break;
            }
          }
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
              // One base subscript (rules 126,134): an iSUB dummy (bare or
              // affine `[m*] kSUB [+/- c]`), or an expression evaluated at each
              // reference to the defined item.
              DefinedSub ds;
              auto parseInt = [&](bool neg) -> long long {
                long long v = 0;
                if (at(Tok::Number)) {
                  v = strtoll(cur().text.c_str(), nullptr, 10);
                  advance();
                } else {
                  d_.error(cur().loc, "expected an integer in the iSUB offset", "(134)");
                }
                return neg ? -v : v;
              };
              bool isubForm =
                  at(Tok::Isub) ||
                  (at(Tok::Number) && peek(1).kind == Tok::Star && peek(2).kind == Tok::Isub);
              if (isubForm) {
                // The optional multiplier of an affine `m * kSUB` dummy.
                if (at(Tok::Number)) {
                  ds.mult = strtoll(cur().text.c_str(), nullptr, 10);
                  advance();
                  eat(Tok::Star);
                }
                // kSUB: the dummy names the item's k-th subscript (rule 134).
                ds.isub = true;
                ds.axis = (int)strtol(cur().text.c_str(), nullptr, 10) - 1;
                advance();
                // optional affine offset (only meaningful after an iSUB)
                if (at(Tok::Plus) || at(Tok::Minus)) {
                  bool neg = cur().kind == Tok::Minus;
                  advance();
                  ds.add = parseInt(neg);
                }
              } else {
                // A constant or runtime index expression; sema types it and
                // resolves the bounds.
                ds.expr = parseExpr();
                if (!ds.expr) {
                  d_.error(cur().loc, "expected a DEFINED base subscript", "(24)");
                  advance(); // skip the offending token to keep parsing the list
                }
              }
              item.definedSubs.push_back(std::move(ds));
              if (!eat(Tok::Comma))
                break;
            }
          }
          expect(Tok::RParen, "(126)");
        }
        // POSITION (rule 24, `POSITION | POS`): the 1-based position in the
        // base at which the defined item starts (IBM string-overlay defining).
        // The argument is an integer constant; sema checks the fit.
        if (at(Tok::Word) && (cur().text == "POSITION" || cur().text == "POS")) {
          advance();
          long long pos = 1;
          if (eat(Tok::LParen)) {
            if (at(Tok::Number)) {
              pos = strtoll(cur().text.c_str(), nullptr, 10);
              advance();
            } else {
              d_.error(cur().loc, "POSITION requires an integer constant", "(24)");
              advance();
            }
            expect(Tok::RParen, "(24)");
          } else {
            d_.error(cur().loc, "expected '(' after POSITION", "(24)");
          }
          item.hasPos = true;
          item.pos = pos;
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
      if (w == "TASK") {
        // task-attribute (rules (15),(79), QR2.8): a task name for CALL TASK.
        advance();
        bag.task = true;
        continue;
      }
      if (w == "EVENT") {
        // event-attribute (rules (15),(79),(82), QR2.8): an event name for
        // CALL EVENT / WAIT synchronisation.
        advance();
        bag.event = true;
        continue;
      }
      if (w == "BASED") {
        // based-attribute ::= BASED [ ( reference ) ]  rule (25): the declared
        // item overlays storage addressed by a locator, so it has no storage of
        // its own. Bare BASED declares an unconnected based variable whose
        // references must be explicitly locator-qualified (rules (25),(124)).
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
          item.basedNoPtr = true;
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
      if (w == "CONTROLLED" || w == "CTL") {
        // controlled-attribute (rule (15), ADR-140): storage managed by an
        // explicit generation stack; consumed like the other storage classes.
        advance();
        bag.controlled = true;
        continue;
      }
      if (w == "CONDITION") {
        // condition-attribute (rules (9),(99)): DECLARE name CONDITION; — the
        // condition name is use-declared here, registered in prog_.condNames so
        // subsequent SIGNAL CONDITION(name) / ON CONDITION(name) resolve it.
        advance();
        item.isCondition = true;
        continue;
      }
      if (w == "AREA") {
        // area-attribute ::= AREA [ ( expression | * ) ]  rule (20): a region
        // from which BASED storage is allocated and freed out of order.
        advance();
        bag.area = true;
        if (eat(Tok::LParen)) {
          if (at(Tok::Star)) {
            // AREA(*) (rule (20)): an adjustable size, valid on a parameter
            // (the size comes from the argument). Sema rejects it elsewhere.
            bag.areaStar = true;
            advance();
          } else {
            bag.areaSize = parseExprOrPlaceholder();
          }
          expect(Tok::RParen, "(20)");
        }
        continue;
      }
      if (w == "OFFSET") {
        // offset-attribute ::= OFFSET [ ( reference ) ]  rule (22): a locator
        // into an AREA. The optional reference names the associated AREA.
        advance();
        bag.offset = true;
        if (eat(Tok::LParen)) {
          if (at(Tok::Word)) {
            bag.offsetRef = cur().text;
            advance();
          } else {
            d_.error(cur().loc, "expected an AREA name after OFFSET(", "(22)");
          }
          expect(Tok::RParen, "(22)");
        }
        continue;
      }
      if (w == "PICTURE" || w == "PIC") {
        // picture-attribute ::= PICTURE ( 'picture-string' )  rules (19),(146)-(148)
        // PICTURE names a declared variable whose type is implied by the picture.
        advance();
        if (eat(Tok::LParen)) {
          if (at(Tok::CharLit)) {
            item.picture = cur().sval;
            bag.picture = true;
            advance();
          } else {
            d_.error(cur().loc, "PICTURE requires a character-string literal in parentheses", "(19)");
          }
          expect(Tok::RParen, "(19)");
        } else {
          d_.error(cur().loc, "PICTURE requires ( 'picture-string' )", "(19)");
        }
        continue;
      }
      if (w == "LABEL") {
        // LABEL (rule (21)): a label variable — a variable that can hold a
        // block address and be the target of an indirect GO TO.
        advance();
        item.labelAttr = true;
        continue;
      }
      if (w == "CELL" || w == "GENERIC" || w == "BUILTIN") {
        // CELL, GENERIC, BUILTIN are not Subset-G attributes; diagnosed.
        d_.error(cur().loc, "attribute " + w + " is not a Subset-G attribute", "(15)");
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
      if (w == "RESERVED") {
        // RESERVED (TR 25.084 rule (15)): attribute acceptance only — a
        // reserved-name variable has no structural effect in this stage.
        advance();
        item.reserved = true;
        continue;
      }
      if (w == "UNION") {
        // UNION (ADR-202): overlapping minor structures (Subset-G extension).
        // The first occurrence starts the overlay; subsequent members share
        // offset 0. Implemented via the DEFINED overlay path.
        advance();
        item.unionAttr = true;
        continue;
      }
      if (w == "NONPRINT" || w == "NOPRINT") {
        // NONPRINT (ADR-204): stream writes to this file are discarded.
        advance();
        item.nonprint = true;
        continue;
      }
      break; // not an attribute: next declaration item or end
    }
    if (at(Tok::LParen)) { // bare precision or dimension
      int a = -1, b = 0;
      SourceLoc l = cur().loc;
      eat(Tok::LParen);
      if (at(Tok::Number)) {
        a = boundedNonNeg(cur().text, cur().loc, d_, "precision or length", "(16)");
        advance();
      }
      if (eat(Tok::Comma) && at(Tok::Number)) {
        b = boundedNonNeg(cur().text, cur().loc, d_, "precision or length", "(16)");
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
  if (!item.typeRef.empty() &&
      (bag.fixed || bag.floating || bag.binary || bag.decimal || bag.character || bag.bit ||
       bag.varying || bag.pointer || bag.complex || bag.file || bag.task || bag.event || bag.area ||
       bag.offset))
    d_.error(item.loc, "TYPE cannot be combined with explicit data attributes (ADR-114)", "");
  if (bag.fixed && bag.floating)
    d_.error(item.loc, "FIXED and FLOAT are conflicting attributes", "(16)");
  if (bag.binary && bag.decimal)
    d_.error(item.loc, "BINARY and DECIMAL are conflicting attributes", "(16)");
  if (bag.character && bag.bit)
    d_.error(item.loc, "CHARACTER and BIT are conflicting attributes", "(18)");
  if ((bag.character || bag.bit) && (bag.fixed || bag.floating || bag.binary || bag.decimal))
    d_.error(item.loc, "string and arithmetic attributes cannot be combined", "(15)");
  // PICTURE (rules (19),(146)-(148)) defines the item's own CHARACTER type:
  // it cannot share the declaration with another data attribute.
  if (bag.picture &&
      (bag.fixed || bag.floating || bag.binary || bag.decimal || bag.character || bag.bit ||
       bag.pointer || bag.complex || bag.file || bag.task || bag.event || bag.area ||
       bag.offset || bag.varying))
    d_.error(item.loc, "PICTURE cannot be combined with another data attribute", "(19)");
  if (bag.picture && (!item.like.empty() || !item.typeRef.empty()))
    d_.error(item.loc, "PICTURE cannot be combined with LIKE or TYPE", "(19)");
  if (bag.varying && !bag.character && !bag.bit)
    d_.error(item.loc, "VARYING requires CHARACTER or BIT", "(15)");
  if (bag.varyingz && !bag.character)
    d_.error(item.loc, "VARYINGZ requires CHARACTER", "(15)");
  if (bag.pointer &&
      (bag.character || bag.bit || bag.fixed || bag.floating || bag.binary || bag.decimal))
    d_.error(item.loc, "POINTER cannot be combined with a data attribute", "(15)");
  if (bag.file && (bag.character || bag.bit || bag.fixed || bag.floating || bag.binary ||
                   bag.decimal || bag.pointer))
    d_.error(item.loc, "FILE cannot be combined with a data attribute", "(15)");
  // COMPLEX is an arithmetic modifier (rule (16)): it combines with the
  // FLOAT/FIXED and DECIMAL/BINARY base attributes; string, POINTER and FILE
  // remain incompatible.
  if (bag.complex && (bag.character || bag.bit || bag.pointer || bag.file))
    d_.error(item.loc, "COMPLEX cannot be combined with a non-arithmetic data attribute", "(15)");
  if ((bag.task || bag.event) &&
      (bag.character || bag.bit || bag.fixed || bag.floating || bag.binary || bag.decimal ||
       bag.pointer || bag.complex || bag.file || bag.varying))
    d_.error(item.loc, "TASK/EVENT cannot be combined with a data attribute", "(15)");
  if (bag.task && bag.event)
    d_.error(item.loc, "TASK and EVENT are conflicting attributes", "(15)");
  if (bag.aligned && bag.unaligned)
    d_.error(item.loc, "ALIGNED and UNALIGNED are conflicting attributes", "(15)");
  // AREA/OFFSET are locator/storage attributes (rules (15),(20),(22)): they do
  // not combine with a computational or string attribute, nor with each other.
  if ((bag.area || bag.offset) &&
      (bag.character || bag.bit || bag.fixed || bag.floating || bag.binary || bag.decimal ||
       bag.pointer || bag.complex || bag.file || bag.varying || bag.task || bag.event))
    d_.error(item.loc, "AREA/OFFSET cannot be combined with another data attribute", "(15)");
  if (bag.area && bag.offset)
    d_.error(item.loc, "AREA and OFFSET are conflicting attributes", "(15)");

  if (bag.task) {
    // A TASK name (rules (15),(79), QR2.8): an opaque handle for CALL TASK.
    item.ty = Type::taskTy();
  } else if (bag.event) {
    // An EVENT name (rules (15),(79),(82), QR2.8): a completion flag.
    item.ty = Type::eventTy();
  } else if (item.labelAttr) {
    // LABEL (rule (21)): a label variable holds a block address for GO TO.
    item.ty = Type::labelTy();
  } else if (bag.file) {
    // A FILE variable carries no computational value (rules 39,40); the parser
    // records the flag and sema assigns it a runtime slot at OPEN/CLOSE.
    item.fileAttr = true;
    item.ty = Type::voidTy();
  } else if (bag.area) {
    // An AREA variable (rule (20)): a runtime region for BASED allocation.
    item.ty = Type::areaTy();
  } else if (bag.offset) {
    // An OFFSET variable (rule (22)): an opaque locator into an AREA.
    item.ty = Type::offsetTy();
  } else if (bag.pointer) {
    item.ty = Type::ptr();
  } else if (bag.complex) {
    item.ty = Type::complexTy();
  } else if (bag.character) {
    // A `CHAR(*)` or `CHAR(expr)` adjustable length (rule (18)): recorded on the
    // type for sema to validate as a parameter-only form and for codegen to
    // size at call time. VARYING stays diagnosed there (deferred).
    item.ty = Type::chr(bag.slen > 0 ? bag.slen : 1, bag.varying);
    item.ty.varyingz = bag.varyingz;
    if (bag.starLen)
      item.ty.starLen = true;
    item.slenExpr = std::move(bag.slenExpr);
  } else if (bag.bit) {
    int n = bag.slen > 0 ? bag.slen : 1;
    item.ty = Type::bit(n);
    if (bag.starLen)
      d_.error(item.loc, "a '*' string length is not implemented in this stage", "(18)");
  } else if (bag.floating) {
    item.ty = Type::flt(bag.prec > 0 ? bag.prec : (bag.binary ? 21 : 6));
  } else {
    // FIXED is the default scale attribute; DECIMAL the default base. The
    // scale factor q is kept as a static property (ADR-006, rule (16)).
    bool binary = bag.binary && !bag.decimal;
    if (!fixedScaleFits(bag, binary))
      d_.error(item.loc,
               binary ? "a FIXED BINARY scale factor must be less than the precision"
                      : "a FIXED DECIMAL scale factor must not exceed the precision",
               "(16)");
    if (binary)
      item.ty = Type::fixedBin(bag.prec > 0 ? bag.prec : 15, bag.scale);
    else
      item.ty = Type::fixedDec(bag.prec > 0 ? bag.prec : 5, bag.scale);
  }
  item.ty.dims = arrDims;
  item.controlled = bag.controlled;
  item.aligned = bag.aligned;
  item.unaligned = bag.unaligned;
  item.area = bag.area;
  item.areaSize = std::move(bag.areaSize);
  item.areaStar = bag.areaStar;
  item.sawStatic = bag.staticAttr;
  // A static AREA is real storage; every other STATIC is still automatic
  // (rule (15)), so the no-op warning is emitted here.
  if (bag.staticAttr && !item.area)
    d_.warn(staticLoc, "attribute STATIC is accepted but has no effect in this stage", "(15)");
  item.offset = bag.offset;
  item.offsetRef = std::move(bag.offsetRef);
  if (item.ty.isBit() && item.ty.len > 1 && !item.ty.dims.empty()) {
    // Element access assumes single-bit storage below; never lay out a wide
    // bit array that codegen would mistarget (invariant 2).
    d_.error(item.loc, "arrays of BIT(n>1) are not implemented in this stage", "(12)");
  }
  item.dynBounds = std::move(arrDyn);
  item.dynLbBounds = std::move(arrDynLb);
  item.init = std::move(init);
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
          if (!dimBoundFits(fv)) {
            d_.error(first->loc, "a dimension bound is out of range", "(12)");
            i_ = save;
            return false;
          }
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
          if (!dimBoundFits(uv)) {
            d_.error(ubE->loc, "a dimension bound is out of range", "(12)");
            i_ = save;
            return false;
          }
          d.ub = (int)uv;
        } else {
          d.dyn = true;
          db.push_back(std::move(ubE));
        }
      } else { // bare extent (ub): lower bound defaults to 1
        if (firstConst) {
          if (!dimBoundFits(fv)) {
            d_.error(first->loc, "a dimension bound is out of range", "(12)");
            i_ = save;
            return false;
          }
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
  // POINTER, BASED and CONTROLLED ride here too, so `DCL H(4) POINTER`,
  // `DCL X(4) BASED(P)` and `DCL Y(4) CONTROLLED` form arrays rather
  // than precision-4 scalars (rules (12),(15),(25)). TASK and EVENT ride
  // here as well, so `DCL EV(2) EVENT` is an array of flags (rule (15)).
  auto isAttrWord = [&](const std::string& w) {
    return w == "FIXED" || w == "FLOAT" || w == "BINARY" || w == "BIN" || w == "DECIMAL" ||
           w == "DEC" || w == "CHARACTER" || w == "CHAR" || w == "BIT" || w == "VARYING" ||
           w == "VARYINGZ" || w == "VAR" || w == "VARZ" || w == "STATIC" || w == "AUTOMATIC" ||
           w == "AUTO" || w == "ALIGNED" || w == "UNALIGNED" || w == "INTERNAL" || w == "INITIAL" ||
           w == "INIT" || w == "VALUE" || w == "TYPE" || w == "EXTERNAL" || w == "EXT" ||
           w == "OPTIONAL" || w == "POINTER" || w == "PTR" || w == "BASED" || w == "CONTROLLED" ||
           w == "CTL" || w == "AREA" || w == "OFFSET" || w == "VARIABLE" || w == "TASK" ||
           w == "EVENT" || w == "PICTURE" || w == "PIC" || w == "LABEL" || w == "COMPLEX" ||
           w == "CPLX" || w == "REAL" || w == "LIKE" || w == "FILE" || w == "ENTRY";
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

// descriptor-param ::= [ dimension ] attribute••• [ dimension ]   rules (36),(12),(13)
// Parse a single ENTRY parameter type, using the same scalar-attribute
// accumulator as parseDeclItem, restricted to the scalar computational types.
// A single-axis '*' dimension is an adjustable extent whose bound the caller
// supplies at call time, passed as a hidden i64 extent argument (rule (13)).
// TR 25.084 rule (36) and IBM Enterprise PL/I put the dimension first
// (`(*) FIXED BIN(31)`); the wishlist order (`FIXED BIN(31)(*)`) is accepted
// too so existing libnet declarations compile unchanged.
bool Parser::parseDescriptorType(Type& out) {
  // One leading/trailing '*' dimension group in either position: `(*)`, or a
  // leading '*' with constant later bounds `(*, n[, m...])` (rules (13),(36)).
  // Fixed-first, non-first '*', and non-constant bounds diagnose (36).
  bool hasStar = false;
  std::vector<Dim> starDims;
  auto isSingleStar = [&]() {
    return at(Tok::LParen) && peek().kind == Tok::Star && peek(2).kind == Tok::RParen;
  };
  auto consumeSingleStar = [&]() {
    advance();
    advance();
    advance();
  };
  auto parseStarGroup = [&](std::vector<Dim>& dims) -> bool {
    if (!at(Tok::LParen))
      return false;
    size_t save = i_;
    SourceLoc l = cur().loc;
    advance();
    std::vector<Dim> axes;
    bool foundStar = false;
    bool ok = true;
    // One constant bound from plain numbers (descriptors carry no
    // expressions in this stage); rejects floats and suffixed numbers.
    auto constNum = [&](long& v) -> bool {
      bool neg = eat(Tok::Minus);
      if (!at(Tok::Number) || cur().isFloat || cur().binaryRadix || cur().imaginary || cur().hasExp)
        return false;
      v = boundedNonNeg(cur().text, cur().loc, d_, "dimension", "(36)");
      advance();
      if (neg)
        v = -v;
      return true;
    };
    for (;;) {
      if (eat(Tok::Star)) {
        Dim d;
        d.dyn = true;
        d.adj = true; // the bound is supplied by the caller, not an expression
        axes.push_back(d);
        foundStar = true;
      } else {
        long lb = 0, ub = 0;
        if (!constNum(ub)) {
          ok = false;
          break;
        }
        Dim d;
        if (eat(Tok::Colon)) {
          d.lb = (int)ub;
          if (!constNum(lb)) {
            ok = false;
            break;
          }
          d.ub = (int)lb;
        } else {
          d.ub = (int)ub; // bare extent: lower bound defaults to 1
        }
        axes.push_back(d);
      }
      if (!eat(Tok::Comma))
        break;
    }
    ok = ok && eat(Tok::RParen);
    // Served: a lone '*' or a leading '*' with constant later bounds.
    bool restConst = true;
    for (size_t k = 1; k < axes.size(); ++k)
      if (axes[k].adj)
        restConst = false;
    if (ok && foundStar && !axes.empty() && axes[0].adj && restConst) {
      dims = axes;
      return true;
    }
    // Anything else: rewind, skip, diagnose (36).
    i_ = save;
    int depth = 0;
    do {
      if (at(Tok::LParen))
        ++depth;
      else if (at(Tok::RParen))
        --depth;
      advance();
    } while (depth > 0 && !at(Tok::Eof));
    if (foundStar)
      d_.error(l,
               "only a leading '*' with constant later bounds is supported in an ENTRY "
               "descriptor in this stage",
               "(36)");
    else
      d_.error(l, "only a '*' dimension is supported in an ENTRY descriptor in this stage", "(36)");
    return false;
  };
  // Leading dimension (spec order, rule (36)): `(*) FIXED ...`.
  if (at(Tok::LParen)) {
    std::vector<Dim> leadDims;
    if (parseStarGroup(leadDims)) {
      hasStar = true;
      starDims = leadDims;
    }
  }
  // A POINTER descriptor (rule (38)): C `void*` parameters ride as the
  // pointer value itself under LINKAGE(SYSTEM)/BYVALUE.
  if (atWord("POINTER") || atWord("PTR")) {
    advance();
    if (isSingleStar()) {
      if (hasStar)
        d_.error(cur().loc, "duplicate '*' dimension in an ENTRY descriptor", "(36)");
      else {
        consumeSingleStar();
        hasStar = true;
        Dim d;
        d.dyn = true;
        d.adj = true;
        starDims.push_back(d);
      }
    } else if (at(Tok::LParen)) {
      SourceLoc l = cur().loc;
      int depth = 0;
      do {
        if (at(Tok::LParen))
          ++depth;
        else if (at(Tok::RParen))
          --depth;
        advance();
      } while (depth > 0 && !at(Tok::Eof));
      d_.error(l, "only a '*' dimension is supported in an ENTRY descriptor in this stage", "(36)");
    }
    if (hasStar) {
      // Array of POINTER with caller-supplied extent (rules (13),(36)).
      out = Type::ptr();
      for (const auto& d : starDims)
        out.dims.push_back(d);
      return true;
    }
    out = Type::ptr();
    return true;
  }
  // An AREA parameter (rule (20)): the runtime region pointer, passed like any
  // other variable. The descriptor's size — AREA(*) or an expression — is not
  // part of the call ABI: the argument is a region pointer and the callee owns
  // no copy, so the parenthesized size is consumed and has no effect.
  if (atWord("AREA")) {
    advance();
    if (at(Tok::LParen)) {
      int depth = 0;
      do {
        if (at(Tok::LParen))
          ++depth;
        else if (at(Tok::RParen))
          --depth;
        advance();
      } while (depth > 0 && !at(Tok::Eof));
    }
    out = Type::areaTy();
    return true;
  }
  // An OFFSET parameter (rule (22)): an opaque locator, passed by reference.
  if (atWord("OFFSET")) {
    advance();
    if (at(Tok::LParen) && peek().kind == Tok::Word) {
      advance();
      advance();
      if (at(Tok::RParen))
        advance();
    } else if (at(Tok::LParen)) {
      int depth = 0;
      do {
        if (at(Tok::LParen))
          ++depth;
        else if (at(Tok::RParen))
          --depth;
        advance();
      } while (depth > 0 && !at(Tok::Eof));
    }
    out = Type::offsetTy();
    return true;
  }
  // An ENTRY parameter (IBM ENTRY VARIABLE extension, ADR-171): a procedure
  // value passed as a function pointer. The nested descriptor and result type
  // are parsed for syntax and discarded; the Type only marks the value as a
  // procedure value so argument checks and the ABI agree across modules.
  if (atWord("ENTRY") || atWord("ENT")) {
    advance();
    if (at(Tok::LParen)) {
      std::vector<Type> nested;
      parseEntryParams(nested);
    }
    if (atWord("RETURNS")) {
      advance();
      if (expect(Tok::LParen, "(38)")) {
        Type rt;
        parseDescriptorType(rt);
        expect(Tok::RParen, "(38)");
      }
    }
    // An entry parameter may carry VARIABLE (ADR-171); it does not change the
    // by-reference ABI, so it is accepted and ignored here.
    if (atWord("VARIABLE"))
      advance();
    out = Type::entryTy();
    return true;
  }
  AttrBag bag;
  while (parseScalarAttr(bag, "(38)")) {
  }
  // Trailing dimension (wishlist order): `FIXED BIN(31)(*)`.
  if (isSingleStar()) {
    if (hasStar)
      d_.error(cur().loc, "duplicate '*' dimension in an ENTRY descriptor", "(36)");
    else {
      consumeSingleStar();
      hasStar = true;
      Dim d;
      d.dyn = true;
      d.adj = true;
      starDims.push_back(d);
    }
  } else if (at(Tok::LParen)) {
    SourceLoc l = cur().loc;
    int depth = 0;
    do {
      if (at(Tok::LParen))
        ++depth;
      else if (at(Tok::RParen))
        --depth;
      advance();
    } while (depth > 0 && !at(Tok::Eof));
    d_.error(l, "only a '*' dimension is supported in an ENTRY descriptor in this stage", "(36)");
  }
  if (bag.character) {
    if (hasStar) {
      d_.error(cur().loc, "arrays of CHARACTER are not implemented in this stage", "(12)");
      out = Type::chr(bag.slen > 0 ? bag.slen : 1, bag.varying);
      out.varyingz = bag.varyingz;
      if (bag.starLen)
        out.starLen = true;
      out.controlled = bag.controlled;
      return true;
    }
    out = Type::chr(bag.slen > 0 ? bag.slen : 1, bag.varying);
    out.varyingz = bag.varyingz;
    if (bag.starLen)
      out.starLen = true;
    // CONTROLLED dummy association visible to cross-module callers (15).
    out.controlled = bag.controlled;
  } else if (bag.bit) {
    if (bag.varyingz)
      d_.error(cur().loc, "VARYINGZ requires CHARACTER", "(15)");
    out = Type::bit(bag.slen > 0 ? bag.slen : 1);
    if (hasStar && bag.slen > 1) {
      d_.error(cur().loc, "arrays of BIT(n>1) are not implemented in this stage", "(12)");
      return true;
    }
    if (bag.starLen)
      d_.error(cur().loc, "a '*' string length is not implemented in this stage", "(18)");
    if (hasStar) {
      for (const auto& d : starDims)
        out.dims.push_back(d);
    }
    out.controlled = bag.controlled;
  } else if (bag.floating) {
    out = Type::flt(bag.prec > 0 ? bag.prec : (bag.binary ? 21 : 6));
    for (const auto& d : starDims)
      out.dims.push_back(d);
    out.controlled = bag.controlled;
  } else if (bag.binary) {
    if (!fixedScaleFits(bag, true))
      d_.error(cur().loc, "a FIXED BINARY scale factor must be less than the precision", "(16)");
    out = Type::fixedBin(bag.prec > 0 ? bag.prec : 15, bag.scale);
    for (const auto& d : starDims)
      out.dims.push_back(d);
    out.controlled = bag.controlled;
  } else {
    if (!fixedScaleFits(bag, false))
      d_.error(cur().loc, "a FIXED DECIMAL scale factor must not exceed the precision", "(16)");
    out = Type::fixedDec(bag.prec > 0 ? bag.prec : 5, bag.scale);
    for (const auto& d : starDims)
      out.dims.push_back(d);
    out.controlled = bag.controlled;
  }
  if (hasStar && bag.varying) {
    d_.error(cur().loc, "adjustable-length VARYING arrays are not implemented in this stage",
             "(12)");
  }
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

// Deep copy of a parsed expression for SELECT value lists (extension,
// ADR-104): each WHEN value compares against its own copy of the SELECT
// expression. Only parse-time state is copied; sema resolves the copies.
ExprP Parser::cloneExpr(const Expr* e) {
  if (!e)
    return nullptr;
  auto c = std::make_unique<Expr>();
  c->kind = e->kind;
  c->loc = e->loc;
  c->ty = e->ty;
  c->ival = e->ival;
  c->wideIval = e->wideIval;
  c->fval = e->fval;
  c->decScale = e->decScale;
  c->decPrec = e->decPrec;
  c->sval = e->sval;
  c->name = e->name;
  c->path = e->path;
  c->memberPath = e->memberPath;
  c->op = e->op;
  c->a = cloneExpr(e->a.get());
  c->b = cloneExpr(e->b.get());
  c->locPtr = cloneExpr(e->locPtr.get());
  for (auto& a : e->args)
    c->args.push_back(cloneExpr(a.get()));
  return c;
}

// select-statement (extension, ADR-104): SELECT [( expr )]; WHEN ...; END;
// desugars in the parser to an IF-chain, so sema and codegen are untouched:
//   SELECT; WHEN (p) S; OTHERWISE T; END;  ->  IF p THEN S; ELSE T;
//   SELECT (E); WHEN (a, b) S; ...         ->  IF E=a | E=b THEN S; ...
// WHEN bodies are single statements (groups included). A SELECT with no
// WHEN, a WHEN after OTHERWISE, a second OTHERWISE, and mixing value
// lists with a bare SELECT (or a bare predicate with SELECT(expr)) are
// diagnosed, never silently accepted.
StmtP Parser::parseSelect(Proc* owner) {
  advance(); // SELECT
  ExprP selExpr;
  if (eat(Tok::LParen)) {
    selExpr = parseExprOrPlaceholder();
    if (!eat(Tok::RParen)) {
      d_.error(cur().loc, "expected ')' after the SELECT expression (ADR-104)", "");
      resync();
      return nullptr;
    }
  }
  if (!eat(Tok::Semi)) {
    d_.error(cur().loc, "expected ';' after SELECT (ADR-104)", "");
    resync();
    return nullptr;
  }
  StmtP head;
  Stmt* tail = nullptr; // open IF node awaiting its elseS
  bool sawWhen = false, sawOtherwise = false;
  for (;;) {
    if (at(Tok::Eof)) {
      d_.error(cur().loc, "unexpected end of file: missing END for SELECT (ADR-104)", "");
      return head;
    }
    if (pendingEnd_.present)
      return head; // multiple closure from a WHEN body
    bool isWhen = cur().isWord("WHEN") && !looksLikeAssignment();
    bool isOtherwise = cur().isWord("OTHERWISE") && !looksLikeAssignment();
    if (atStmtKeyword("END")) {
      SourceLoc l = cur().loc;
      advance(); // END
      std::string lab;
      if (at(Tok::Word)) {
        lab = cur().text;
        advance();
      }
      if (!eat(Tok::Semi)) {
        d_.error(cur().loc, "expected ';' after END", "(7)");
        resync();
        return head;
      }
      if (!lab.empty()) {
        EndInfo e;
        e.present = true;
        e.loc = l;
        e.label = lab;
        pendingEnd_ = e; // multiple closure: closes an outer block
        return head;
      }
      if (!sawWhen)
        d_.error(l, "SELECT requires at least one WHEN (ADR-104)", "");
      return head;
    }
    if (!isWhen && !isOtherwise) {
      d_.error(cur().loc, "expected WHEN, OTHERWISE, or END in SELECT (ADR-104)", "");
      resync();
      return head;
    }
    if (isWhen) {
      if (sawOtherwise) {
        d_.error(cur().loc, "WHEN after OTHERWISE (ADR-104)", "");
        resync();
        return head;
      }
      SourceLoc l = cur().loc;
      advance(); // WHEN
      if (!eat(Tok::LParen)) {
        d_.error(cur().loc, "expected '(' after WHEN (ADR-104)", "");
        resync();
        return head;
      }
      std::vector<ExprP> vals;
      vals.push_back(parseExpr());
      while (eat(Tok::Comma))
        vals.push_back(parseExpr());
      if (!eat(Tok::RParen)) {
        d_.error(cur().loc, "expected ')' after the WHEN values (ADR-104)", "");
        resync();
        return head;
      }
      ExprP pred;
      if (!selExpr) {
        if (vals.size() != 1) {
          d_.error(cur().loc, "WHEN with multiple values needs SELECT ( expr ) (ADR-104)", "");
          resync();
          return head;
        }
        pred = std::move(vals[0]);
      } else {
        for (auto& v : vals) {
          auto eq = std::make_unique<Expr>();
          eq->kind = Expr::Binary;
          eq->op = Tok::Eq;
          eq->loc = v->loc;
          eq->a = cloneExpr(selExpr.get());
          eq->b = std::move(v);
          if (!pred) {
            pred = std::move(eq);
          } else {
            auto orE = std::make_unique<Expr>();
            orE->kind = Expr::Binary;
            orE->op = Tok::Bar;
            orE->loc = eq->loc;
            orE->a = std::move(pred);
            orE->b = std::move(eq);
            pred = std::move(orE);
          }
        }
      }
      StmtP body = parseStatement(owner);
      auto ifs = std::make_unique<Stmt>();
      ifs->kind = Stmt::If;
      ifs->loc = l;
      ifs->cond = std::move(pred);
      ifs->thenS = std::move(body);
      if (!head) {
        head = std::move(ifs);
        tail = head.get();
      } else {
        tail->elseS = std::move(ifs);
        tail = tail->elseS.get();
      }
      sawWhen = true;
      if (pendingEnd_.present)
        return head; // multiple closure from the WHEN body
      continue;
    }
    // OTHERWISE (exactly one; a second is diagnosed above via sawOtherwise).
    if (sawOtherwise) {
      d_.error(cur().loc, "only one OTHERWISE is allowed in SELECT (ADR-104)", "");
      resync();
      return head;
    }
    advance(); // OTHERWISE
    sawOtherwise = true;
    StmtP body = parseStatement(owner);
    if (tail)
      tail->elseS = std::move(body);
    // Without a preceding WHEN the rejection surfaces at END (!sawWhen).
    if (pendingEnd_.present)
      return head; // multiple closure from the OTHERWISE body
  }
}

// LEAVE [label]; / ITERATE [label]; (extension, ADR-105): loop exit and
// continue. The label (when present) names the target iterative DO-group;
// scope validation happens in sema, which sees the enclosing loops.
StmtP Parser::parseLoopExit(bool isIterate) {
  auto st = std::make_unique<Stmt>();
  st->kind = isIterate ? Stmt::Iterate : Stmt::Leave;
  st->loc = cur().loc;
  advance(); // LEAVE / ITERATE
  if (at(Tok::Word)) {
    st->name = cur().text;
    advance();
  }
  if (!eat(Tok::Semi)) {
    d_.error(cur().loc,
             std::string("expected ';' after ") +
                 (isIterate ? "ITERATE (ADR-105)" : "LEAVE (ADR-105)"),
             "");
    resync();
    return nullptr;
  }
  return st;
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
    if (atWord("UNTIL")) {
      d_.error(cur().loc, "DO with both WHILE and UNTIL is not implemented in this stage (ADR-106)",
               "");
      resync();
      return nullptr;
    }
    expect(Tok::Semi, "(71)");
  } else if (atWord("UNTIL")) {
    // DO UNTIL (expr) (extension, ADR-106): post-test loop; the same shape
    // as WHILE but the condition is tested after the body.
    advance();
    st->kind = Stmt::DoWhile;
    st->until = true;
    if (!eat(Tok::LParen)) {
      d_.error(cur().loc, "expected '(' after UNTIL (ADR-106)", "");
      resync();
      return nullptr;
    }
    st->cond = parseExpr();
    if (!eat(Tok::RParen)) {
      d_.error(cur().loc, "expected ')' after the UNTIL condition (ADR-106)", "");
      resync();
      return nullptr;
    }
    if (atWord("WHILE")) {
      d_.error(cur().loc, "DO with both UNTIL and WHILE is not implemented in this stage (ADR-106)",
               "");
      resync();
      return nullptr;
    }
    if (!eat(Tok::Semi)) {
      d_.error(cur().loc, "expected ';' after UNTIL ( expr ) (ADR-106)", "");
      resync();
      return nullptr;
    }
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
      if (atWord("UNTIL")) {
        d_.error(cur().loc,
                 "DO with an iterative specification and UNTIL is not implemented in this stage "
                 "(ADR-106)",
                 "");
        resync();
        return nullptr;
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

// display-statement ::= DISPLAY ( expression ) [ REPLY ( reference ) ]  rule (114)
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
  if (atWord("REPLY")) {
    advance();
    if (eat(Tok::LParen)) {
      // REPLY takes a reference (scalar character variable).
      st->replyTarget = parseExpr();
      expect(Tok::RParen, "(114)");
    } else {
      d_.error(cur().loc, "expected '(' after REPLY", "(114)");
      return nullptr;
    }
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
      // Data-directed transmission (rule (106)): `DATA ( item, ... )`; each
      // item must name a variable (checked in sema) so it prints as NAME=value.
      advance();
      st->data = true;
      sawData = true;
      if (expect(Tok::LParen, "(106)")) {
        if (!at(Tok::RParen)) {
          for (;;) {
            st->items.push_back(parseExpr());
            if (!eat(Tok::Comma))
              break;
          }
        }
        expect(Tok::RParen, "(106)");
      }
      continue;
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
    if (atWord("LINE")) {
      advance();
      st->line = true;
      if (eat(Tok::LParen)) {
        st->lineCount = parseExpr();
        expect(Tok::RParen, "(105)");
      }
      continue;
    }
    if (atWord("COPY")) {
      advance();
      st->copy = true;
      continue;
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
// List-directed, edit-directed, and data-directed input read scalar values
// from SYSIN (or FILE/STRING); COPY/LINE/PAGE stay unimplemented.
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
    if (atWord("DATA")) {
      // Data-directed transmission (rule (106)): `DATA ( item, ... )`; each
      // item must name a variable (checked in sema) to receive its pair.
      advance();
      st->data = true;
      if (expect(Tok::LParen, "(106)")) {
        if (!at(Tok::RParen)) {
          for (;;) {
            st->items.push_back(parseExpr());
            if (!eat(Tok::Comma))
              break;
          }
        }
        expect(Tok::RParen, "(106)");
      }
      continue;
    }
    if (atWord("LINE")) {
      advance();
      st->line = true;
      if (eat(Tok::LParen)) {
        st->lineCount = parseExpr();
        expect(Tok::RParen, "(105)");
      }
      continue;
    }
    if (atWord("COPY")) {
      advance();
      st->copy = true;
      continue;
    }
    if (atWord("PAGE")) {
      d_.error(cur().loc, "GET option PAGE is not implemented in this stage", "(105)");
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
// control format (X spacing, SKIP/LINE/PAGE line control, COL positioning),
// each with an optional parenthesised width and, for F, a fractional-digit
// count. An iteration group (n)(item...) unrolls inline here, nesting by
// recursion; only an integer literal repeats a group.
bool Parser::parseFormatItem(Stmt* st) { return parseFormatItemInto(st->formats); }

// Deep-copy a format width/decimals tree (iteration groups fan out to one
// copy per use).
static ExprP cloneFormatExpr(const Expr* e) {
  if (!e)
    return nullptr;
  auto c = std::make_unique<Expr>();
  c->kind = e->kind;
  c->loc = e->loc;
  c->ty = e->ty;
  c->ival = e->ival;
  c->wideIval = e->wideIval;
  c->fval = e->fval;
  c->decScale = e->decScale;
  c->decPrec = e->decPrec;
  c->sval = e->sval;
  c->name = e->name;
  c->path = e->path;
  c->memberPath = e->memberPath;
  c->sym = e->sym;
  if (e->locPtr)
    c->locPtr = cloneFormatExpr(e->locPtr.get());
  c->op = e->op;
  if (e->a)
    c->a = cloneFormatExpr(e->a.get());
  if (e->b)
    c->b = cloneFormatExpr(e->b.get());
  for (const auto& a : e->args)
    c->args.push_back(cloneFormatExpr(a.get()));
  return c;
}

static FormatItem cloneFormatItem(const FormatItem& f) {
  FormatItem c;
  c.kind = f.kind;
  c.w = cloneFormatExpr(f.w.get());
  c.d = cloneFormatExpr(f.d.get());
  c.s = cloneFormatExpr(f.s.get());
  c.remote = f.remote;
  for (const auto& sb : f.subs)
    c.subs.push_back(cloneFormatItem(sb));
  return c;
}

bool Parser::parseFormatItemInto(std::vector<FormatItem>& out) {
  if (cur().kind == Tok::LParen) {
    advance();
    if (cur().kind != Tok::Number || cur().isFloat) {
      d_.error(cur().loc, "a format repetition count must be an integer literal", "(48)");
      return false;
    }
    int n = boundedNonNeg(cur().text, cur().loc, d_, "repetition count", "(48)");
    advance();
    if (!expect(Tok::RParen, "(48)"))
      return false;
    if (!eat(Tok::LParen)) {
      d_.error(cur().loc, "expected '(' to open the repeated format group", "(48)");
      return false;
    }
    std::vector<FormatItem> group;
    if (!at(Tok::RParen)) {
      for (;;) {
        if (!parseFormatItemInto(group))
          return false;
        if (!eat(Tok::Comma))
          break;
      }
    }
    if (!expect(Tok::RParen, "(48)"))
      return false;
    for (int i = 0; i < n; ++i)
      for (const FormatItem& gi : group)
        out.push_back(cloneFormatItem(gi));
    return true;
  }
  FormatItem fi;
  if (!parseSingleFormatItem(fi))
    return false;
  out.push_back(std::move(fi));
  return true;
}

bool Parser::parseSingleFormatItem(FormatItem& fi) {
  if (cur().kind != Tok::Word) {
    d_.error(cur().loc, "expected a format item", "(48)");
    return false;
  }
  const std::string& w = cur().text;
  // Picture-directed P items use PICTURE (rule 53, rules 146-148).
  if (w == "P") {
    advance(); // consume P
    if (eat(Tok::LParen)) {
      if (at(Tok::CharLit)) {
        fi.kind = FormatItem::P;
        fi.pic = cur().sval;
        advance();
      } else {
        d_.error(cur().loc, "P requires a picture string literal in parentheses", "(53)");
      }
      expect(Tok::RParen, "(53)");
    } else {
      d_.error(cur().loc, "P requires ( 'picture-string' )", "(53)");
    }
    return true;
  }
  if (w == "A") {
    fi.kind = FormatItem::A;
  } else if (w == "B") {
    // Bit-string format (rule (52)): B[(w)] transmits a BIT item as '0'/'1'.
    fi.kind = FormatItem::B;
  } else if (w == "C") {
    // Complex format (rule (51)): C(real[,real]) transmits one COMPLEX item
    // through 1-2 inner real (F/E) formats for the real/imaginary parts.
    fi.kind = FormatItem::C;
  } else if (w == "R") {
    // Remote format (rule (55)): R(label) splices the FORMAT statement's list.
    fi.kind = FormatItem::Remote;
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
  } else if (w == "COL" || w == "COLUMN") {
    // Column positioning (rule (48)): COL(n) and COLUMN(n) both start the
    // next item at 1-based column n.
    fi.kind = FormatItem::Column;
  } else {
    d_.error(cur().loc, "'" + w + "' is not a format item", "(48)");
    return false;
  }
  advance(); // the format descriptor word
  if (fi.kind == FormatItem::Remote) {
    // R(reference) (rule (55)): the label of a FORMAT statement in this block.
    if (!expect(Tok::LParen, "(55)"))
      return false;
    if (cur().kind != Tok::Word) {
      d_.error(cur().loc, "R requires a FORMAT statement label in parentheses", "(55)");
      return false;
    }
    fi.remote = cur().text;
    advance();
    expect(Tok::RParen, "(55)");
    return true;
  }
  if (fi.kind == FormatItem::C) {
    // C(real[,real]): each part is an F/E real format with (w[,d]).
    if (!expect(Tok::LParen, "(51)"))
      return false;
    for (;;) {
      FormatItem inner;
      if (!parseSingleFormatItem(inner))
        return false;
      if (inner.kind != FormatItem::F && inner.kind != FormatItem::E) {
        d_.error(cur().loc, "a C format part must be an F or E real format", "(51)");
        return false;
      }
      fi.subs.push_back(std::move(inner));
      if (!eat(Tok::Comma))
        break;
      if ((int)fi.subs.size() >= 2)
        break;
    }
    if (fi.subs.size() > 2) {
      d_.error(cur().loc, "a C format takes at most two real formats", "(51)");
      return false;
    }
    expect(Tok::RParen, "(51)");
    return true;
  }
  if ((fi.kind == FormatItem::Column || fi.kind == FormatItem::B) && cur().kind != Tok::LParen &&
      fi.kind == FormatItem::Column) {
    d_.error(cur().loc, "COL requires a column number in parentheses", "(48)");
    return false;
  }
  if (eat(Tok::LParen)) { // optional ( width [, decimals [, scale ] ] )
    fi.w = parseExpr();
    if ((fi.kind == FormatItem::F || fi.kind == FormatItem::E) && eat(Tok::Comma)) {
      fi.d = parseExpr();
      if (eat(Tok::Comma)) { // the third F operand is a scale factor (rule (50))
        fi.s = parseExpr();
      }
    }
    expect(Tok::RParen, "(48)");
  } else if (fi.kind == FormatItem::Column) {
    d_.error(cur().loc, "COL requires a column number in parentheses", "(48)");
    return false;
  }
  return true;
}

// format-sentence ::= [ labellist ] FORMAT formatlist ;   rule (44)
// A FORMAT statement stores its format list under its labels for R(ref) use;
// it emits no code. At least one label is required (the R target).
StmtP Parser::parseFormatStmt(const std::vector<std::string>& labels) {
  auto st = std::make_unique<Stmt>();
  st->loc = cur().loc;
  st->labels = labels;
  advance(); // FORMAT
  st->kind = Stmt::Format;
  if (labels.empty())
    d_.error(st->loc, "a FORMAT statement requires a label for R reference", "(44)");
  if (!expect(Tok::LParen, "(45)"))
    return nullptr;
  if (!at(Tok::RParen)) {
    for (;;) {
      if (!parseFormatItem(st.get()))
        return nullptr;
      if (!eat(Tok::Comma))
        break;
    }
  }
  if (!expect(Tok::RParen, "(45)"))
    return nullptr;
  expect(Tok::Semi, "(44)");
  return st;
}

// call-statement ::= CALL identifier [argumentlist] ... ;          rule (78)
// call-optionslist ::= TASK[(ref)] | EVENT(ref) | PRIORITY(expr)  rule (79)
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
        // A '*' argument omits an OPTIONAL parameter (extension): recorded
        // as a Star node; sema validates it against the callee. It is not
        // an expression; parseExpr would reject it.
        if (at(Tok::Star)) {
          auto star = std::make_unique<Expr>();
          star->kind = Expr::Star;
          star->loc = cur().loc;
          st->args.push_back(std::move(star));
          advance();
        } else {
          st->args.push_back(parseExpr());
        }
        if (!eat(Tok::Comma))
          break;
      }
    }
    expect(Tok::RParen, "(80)");
  }
  // Task options (rule (79), QR2.8): TASK[(ref)], EVENT(ref),
  // PRIORITY(expr) in any order, blank-separated. Any combination makes the
  // CALL asynchronous; sema validates the referenced TASK/EVENT variables.
  for (;;) {
    if (atWord("TASK")) {
      if (st->hasTaskOpt) {
        d_.error(cur().loc, "duplicate TASK option on CALL", "(79)");
        advance();
      } else {
        SourceLoc l = cur().loc;
        advance();
        st->hasTaskOpt = true;
        if (eat(Tok::LParen)) {
          if (!at(Tok::RParen)) {
            st->taskRef = parsePrimary();
            if (!st->taskRef) {
              resync();
              return nullptr;
            }
          } else {
            // Bare TASK() — like a bare TASK with no name.
            (void)l;
          }
          expect(Tok::RParen, "(79)");
        }
      }
      continue;
    }
    if (atWord("EVENT")) {
      if (st->eventRef) {
        d_.error(cur().loc, "duplicate EVENT option on CALL", "(79)");
        advance();
      } else {
        advance();
        if (!expect(Tok::LParen, "(79)")) {
          resync();
          return nullptr;
        }
        st->eventRef = parsePrimary();
        if (!st->eventRef) {
          resync();
          return nullptr;
        }
        expect(Tok::RParen, "(79)");
      }
      continue;
    }
    if (atWord("PRIORITY")) {
      if (st->priorityExpr) {
        d_.error(cur().loc, "duplicate PRIORITY option on CALL", "(79)");
        advance();
      } else {
        advance();
        if (!expect(Tok::LParen, "(79)")) {
          resync();
          return nullptr;
        }
        st->priorityExpr = parseExpr();
        expect(Tok::RParen, "(79)");
      }
      continue;
    }
    break;
  }
  expect(Tok::Semi, "(78)");
  return st;
}

// wait-statement ::= WAIT(ev,...)[(count)] ;                        rule (82)
StmtP Parser::parseWait() {
  auto st = std::make_unique<Stmt>();
  st->kind = Stmt::Wait;
  st->loc = cur().loc;
  advance(); // WAIT
  if (!expect(Tok::LParen, "(82)")) {
    resync();
    return nullptr;
  }
  if (!at(Tok::RParen)) {
    for (;;) {
      ExprP ev = parsePrimary();
      if (!ev) {
        resync();
        return nullptr;
      }
      st->waitEvents.push_back(std::move(ev));
      if (!eat(Tok::Comma))
        break;
    }
  }
  if (!expect(Tok::RParen, "(82)")) {
    resync();
    return nullptr;
  }
  // Optional count: WAIT(evs)(n) waits for any n of the events.
  if (at(Tok::LParen)) {
    advance();
    st->waitCount = parseExpr();
    expect(Tok::RParen, "(82)");
  }
  expect(Tok::Semi, "(82)");
  return st;
}

// delay-statement ::= DELAY(expr) ;                                 rule (83)
StmtP Parser::parseDelay() {
  auto st = std::make_unique<Stmt>();
  st->kind = Stmt::Delay;
  st->loc = cur().loc;
  advance(); // DELAY
  if (!expect(Tok::LParen, "(83)")) {
    resync();
    return nullptr;
  }
  st->value = parseExpr();
  expect(Tok::RParen, "(83)");
  expect(Tok::Semi, "(83)");
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

// ALLOCATE {based-allocate-item|controlled-allocate-item},...;  rules (87)-(89)
// based-allocate-item ::= identifier ( SET ( reference ) [ IN ( reference ) ]
//                                     | IN ( reference ) [ SET ( reference ) ] )  rule (88)
// controlled-allocate-item ::= [integer] identifier dimension-attribute
//                              [ {string-attribute|...} ]  rule (89)
// Serves the SET option (heap-allocate the based structure's storage and store
// the address in the locator) and the IN (AREA) option (rules (20),(88):
// allocate the block from the named region). Rule (89) serves a scalar
// CHARACTER length at ALLOCATE: either a dimension
// `(expr)` giving the maximum or a string-attribute `CHAR(expr)`, with an
// optional trailing VARYING/CHARACTER confirmation.
StmtP Parser::parseAllocate() {
  auto st = std::make_unique<Stmt>();
  st->kind = Stmt::Allocate;
  st->loc = cur().loc;
  advance(); // ALLOCATE
  for (;;) {
    // Rule (89) level number: an integer before the identifier (structures).
    if (at(Tok::Number) && !cur().isFloat && peek().kind == Tok::Word) {
      boundedNonNeg(cur().text, cur().loc, d_, "level number", "(89)");
      advance();
    }
    if (at(Tok::Word)) {
      auto base = std::make_unique<Expr>();
      base->kind = Expr::VarRef;
      base->name = cur().text;
      base->loc = cur().loc;
      base->allocBaseRef = true; // named based variable of the ALLOCATE item (rule (88))
      advance();
      bool paren = eat(Tok::LParen); // optional: identifier [(] SET/IN ( ref ) [)]
      // A SET/IN option always carries `(...)`, so a bare variable named
      // `set`/`in` as a dimension length (e.g. `ALLOCATE x (set)`) must not
      // take this path: require `(` after the keyword. The two options may
      // appear in either order (rule (88)); the closing paren is only expected
      // when an opening paren was present.
      auto atOpt = [&](const char* kw) { return atWord(kw) && peek().kind == Tok::LParen; };
      if (atOpt("SET") || atOpt("IN")) {
        ExprP set;
        ExprP areaRef;
        for (;;) {
          if (atOpt("SET")) {
            // SET ( reference ): store the block address in the locator.
            if (set)
              d_.error(cur().loc, "duplicate SET option in ALLOCATE", "(88)");
            advance();
            expect(Tok::LParen, "(88)");
            set = parsePrimary();
            if (!set) {
              resync();
              return nullptr;
            }
            expect(Tok::RParen, "(88)");
            continue;
          }
          if (atOpt("IN")) {
            // IN ( area ): allocate the block from the named AREA (rule (88)).
            if (areaRef)
              d_.error(cur().loc, "duplicate IN option in ALLOCATE", "(88)");
            advance();
            expect(Tok::LParen, "(88)");
            areaRef = parsePrimary();
            if (!areaRef) {
              resync();
              return nullptr;
            }
            expect(Tok::RParen, "(88)");
            continue;
          }
          break;
        }
        if (paren)
          expect(Tok::RParen, "(88)");
        st->allocBase.push_back(std::move(base));
        st->allocSet.push_back(std::move(set));
        st->allocArea.push_back(std::move(areaRef));
        st->allocBounds.emplace_back();
        st->allocCharLen.push_back(nullptr);
        st->allocVarying.push_back(0);
        st->allocHasChar.push_back(0);
      } else if (paren) {
        // A dimension-attribute (rule 89): one bound-pair per axis —
        // a single extent `(n)`, an explicit `lb:ub` pair, or `*`
        // (reuse the previous generation's extent for that axis).
        // Multi-axis `(m,n,...)` sizes N-D CONTROLLED generations.
        std::vector<Stmt::AllocBound> bounds;
        for (;;) {
          Stmt::AllocBound b;
          if (at(Tok::Star)) {
            advance();
            b.star = true;
          } else {
            ExprP first = parseExpr();
            if (!first) {
              resync();
              return nullptr;
            }
            if (eat(Tok::Colon)) {
              ExprP second = parseExpr();
              if (!second) {
                resync();
                return nullptr;
              }
              b.lb = std::move(first);
              b.ub = std::move(second);
            } else {
              b.ub = std::move(first);
            }
          }
          bounds.push_back(std::move(b));
          if (!eat(Tok::Comma))
            break;
        }
        expect(Tok::RParen, "(89)");
        // Trailing string-attributes (rule 89): [CHAR[(expr|*)]] [VARYING].
        ExprP charLen;
        char hasChar = 0;
        char varying = 0;
        for (;;) {
          if (atWord("CHARACTER") || atWord("CHAR")) {
            hasChar = 1;
            advance();
            if (at(Tok::LParen)) {
              advance();
              if (at(Tok::Star)) {
                advance();
              } else {
                ExprP cl = parseExpr();
                if (!cl) {
                  resync();
                  return nullptr;
                }
                if (charLen)
                  d_.error(cur().loc, "duplicate CHARACTER length in ALLOCATE", "(89)");
                else
                  charLen = std::move(cl);
              }
              expect(Tok::RParen, "(89)");
            }
            continue;
          }
          if (atWord("VARYING") || atWord("VAR")) {
            varying = 1;
            advance();
            continue;
          }
          if (atWord("BIT") || atWord("CELL") || atWord("AREA") || atWord("INITIAL") ||
              atWord("INIT")) {
            d_.error(cur().loc,
                     std::string("ALLOCATE ") + cur().text + " is not implemented in this stage",
                     "(89)");
            resync();
            return nullptr;
          }
          break;
        }
        // SET/IN may follow a dimension (rule (88)): ALLOCATE x(n) SET(p).
        ExprP setOpt;
        ExprP areaOpt;
        while (atOpt("SET") || atOpt("IN")) {
          if (atOpt("SET")) {
            if (setOpt)
              d_.error(cur().loc, "duplicate SET option in ALLOCATE", "(88)");
            advance();
            expect(Tok::LParen, "(88)");
            setOpt = parsePrimary();
            if (!setOpt) {
              resync();
              return nullptr;
            }
            expect(Tok::RParen, "(88)");
            continue;
          }
          if (areaOpt)
            d_.error(cur().loc, "duplicate IN option in ALLOCATE", "(88)");
          advance();
          expect(Tok::LParen, "(88)");
          areaOpt = parsePrimary();
          if (!areaOpt) {
            resync();
            return nullptr;
          }
          expect(Tok::RParen, "(88)");
        }
        st->allocBase.push_back(std::move(base));
        st->allocSet.push_back(std::move(setOpt));
        st->allocArea.push_back(std::move(areaOpt));
        st->allocBounds.push_back(std::move(bounds));
        st->allocCharLen.push_back(std::move(charLen));
        st->allocVarying.push_back(varying);
        st->allocHasChar.push_back(hasChar);
      } else {
        // No dimension: bare ALLOCATE X or ALLOCATE X with trailing
        // CHAR[(expr|*)] / VARYING in either order (rule 89).
        ExprP charLen;
        char hasChar = 0;
        char varying = 0;
        bool sawAttr = false;
        for (;;) {
          if (atWord("CHARACTER") || atWord("CHAR")) {
            hasChar = 1;
            sawAttr = true;
            advance();
            if (at(Tok::LParen)) {
              advance();
              if (at(Tok::Star)) {
                advance();
              } else {
                ExprP cl = parseExpr();
                if (!cl) {
                  resync();
                  return nullptr;
                }
                if (charLen)
                  d_.error(cur().loc, "duplicate CHARACTER length in ALLOCATE", "(89)");
                else
                  charLen = std::move(cl);
              }
              expect(Tok::RParen, "(89)");
            }
            continue;
          }
          if (atWord("VARYING") || atWord("VAR")) {
            varying = 1;
            sawAttr = true;
            advance();
            continue;
          }
          if (atWord("BIT") || atWord("CELL") || atWord("AREA") || atWord("INITIAL") ||
              atWord("INIT")) {
            d_.error(cur().loc,
                     std::string("ALLOCATE ") + cur().text + " is not implemented in this stage",
                     "(89)");
            resync();
            return nullptr;
          }
          break;
        }
        // Bare ALLOCATE X (rule 87) when no trailing attribute appeared:
        // only CONTROLLED storage is served without SET; BASED without SET
        // is diagnosed in sema so the cite stays attached to the storage kind.
        (void)sawAttr;
        st->allocBase.push_back(std::move(base));
        st->allocSet.push_back(nullptr);
        st->allocArea.push_back(nullptr);
        st->allocBounds.emplace_back();
        st->allocCharLen.push_back(std::move(charLen));
        st->allocVarying.push_back(varying);
        st->allocHasChar.push_back(hasChar);
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
// Serves the plain based-variable form, the locator-qualified form, and the
// IN (AREA) option (returning the block to its region).
StmtP Parser::parseFree() {
  auto st = std::make_unique<Stmt>();
  st->kind = Stmt::Free;
  st->loc = cur().loc;
  advance(); // FREE
  // Rule (90) parenthesises the whole item list: FREE ( X, Y ); an
  // unparenthesised list stays accepted for source that predates it.
  bool outer = eat(Tok::LParen);
  for (;;) {
    ExprP areaRef;
    // [reference ->] identifier (rule (90)): the locator is any pointer-valued
    // reference (rule (124) left recursion), so a subscripted element, a
    // qualified member or a nested locator chain is parsed like any reference;
    // sema checks that the operand ends in a plain based-variable name.
    ExprP base = parsePrimary();
    if (!base) {
      resync();
      return nullptr;
    }
    st->freeBase.push_back(std::move(base));
    if (atWord("IN") && peek().kind == Tok::LParen) {
      // IN ( area ) (rule (90)): return the block to the named AREA.
      advance();
      expect(Tok::LParen, "(90)");
      areaRef = parsePrimary();
      if (!areaRef) {
        resync();
        return nullptr;
      }
      expect(Tok::RParen, "(90)");
    }
    st->freeArea.push_back(std::move(areaRef));
    if (!eat(Tok::Comma))
      break;
  }
  if (outer)
    expect(Tok::RParen, "(90)");
  expect(Tok::Semi, "(90)");
  return st;
}

// read-statement ::= READ FILE ( f ) INTO ( reference ) ;    rules (112),(113)
// write-statement ::= WRITE FILE ( f ) FROM ( expression ) ; sequential slice:
// one fixed-size binary record per statement on a RECORD SEQUENTIAL file.
// IGNORE/KEYTO/KEY/NOLOCK/SET/KEYFROM/EVENT stay diagnosed with rule (113).
StmtP Parser::parseRecordIO() {
  auto st = std::make_unique<Stmt>();
  st->loc = cur().loc;
  const bool isRead = atWord("READ");
  const bool isRewrite = atWord("REWRITE");
  const bool isDelete = atWord("DELETE");
  advance(); // READ / WRITE / REWRITE / DELETE
  st->kind = isRead ? Stmt::Read
                    : isRewrite ? Stmt::Rewrite : isDelete ? Stmt::Delete : Stmt::Write;
  if (atWord("FILE")) {
    advance();
    if (eat(Tok::LParen)) {
      if (at(Tok::Word)) {
        st->fileIdent = cur().text;
        advance();
      } else {
        d_.error(cur().loc, "expected a FILE variable after FILE(", "(113)");
      }
      expect(Tok::RParen, "(113)");
    } else {
      d_.error(cur().loc, "expected '(' after FILE", "(113)");
    }
  } else {
    d_.error(cur().loc,
             "READ/WRITE/REWRITE/DELETE require a FILE ( f ) option in this stage", "(113)");
  }
  // DELETE removes the current record and takes no data clause; READ needs a
  // receiver and WRITE/REWRITE a source.
  if (!isDelete) {
    const char* want = isRead ? "INTO" : "FROM";
    if (atWord(want)) {
      advance();
      if (eat(Tok::LParen)) {
        if (isRead)
          st->target = parseExpr();
        else
          st->value = parseExpr();
        expect(Tok::RParen, "(113)");
      } else {
        d_.error(cur().loc, std::string("expected '(' after ") + want, "(113)");
      }
    } else {
      d_.error(cur().loc,
               std::string(isRead
                               ? "READ requires an INTO ( reference ) option; "
                               : "WRITE/REWRITE require a FROM ( expression ) option; ") +
                   "it is not implemented otherwise in this stage",
               "(113)");
    }
  }
  while (!at(Tok::Semi) && !at(Tok::Eof)) {
    d_.error(cur().loc,
             "record option '" +
                 (cur().kind == Tok::Word ? cur().text : std::string(tokName(cur().kind))) +
                 "' is not implemented in this stage",
             "(113)");
    resync();
    return nullptr;
  }
  expect(Tok::Semi, "(112)");
  return st;
}

// open-statement ::= OPEN {, open-optionslist};    rules (100),(101)
// This stage serves the stream forms: FILE ( f ) plus TITLE ('name') and the
// INPUT/OUTPUT/STREAM/PRINT file-attributes (rule 40); and the sequential
// record form with RECORD SEQUENTIAL [INPUT|OUTPUT|UPDATE] (rules
// (101),(112)). KEYED/DIRECT and the IDENT/LINESIZE/PAGESIZE/ENVIRONMENT
// options stay unimplemented.
StmtP Parser::parseOpen() {
  auto st = std::make_unique<Stmt>();
  st->kind = Stmt::Open;
  st->loc = cur().loc;
  advance(); // OPEN
  bool sawFile = false;
  bool sawStream = false;
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
    if (atWord("OUTPUT")) {
      advance();
      continue;
    }
    if (atWord("UPDATE")) {
      // UPDATE (rules (101),(112)): read and write the same sequential
      // record file in place, for REWRITE/DELETE.
      st->openUpdate = true;
      advance();
      continue;
    }
    if (atWord("STREAM") || atWord("PRINT")) {
      sawStream = true;
      advance();
      continue;
    }
    if (atWord("RECORD") || atWord("SEQUENTIAL")) {
      // Sequential record organisation (rules (101),(112)): the only record
      // form served in this stage; DIRECT/KEYED stay diagnosed below.
      st->openRecord = true;
      advance();
      continue;
    }
    if (atWord("KEYED") || atWord("ENVIRONMENT") || atWord("IDENT") ||
        atWord("LINESIZE") || atWord("PAGESIZE")) {
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
  if (st->openRecord && sawStream)
    d_.error(st->loc, "OPEN cannot combine RECORD with STREAM/PRINT in this stage", "(101)");
  if (st->openUpdate && (!st->openRecord || sawStream))
    d_.error(st->loc,
             "OPEN ... UPDATE requires the RECORD SEQUENTIAL organisation in this stage",
             "(101)");
  else if (st->openUpdate && st->openInput)
    d_.error(st->loc, "OPEN cannot combine INPUT with UPDATE", "(101)");
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
ExprP Parser::parseExprOrPlaceholder() {
  SourceLoc loc = cur().loc;
  ExprP e = parseExpr();
  if (e)
    return e;
  auto ph = std::make_unique<Expr>();
  ph->kind = Expr::IntLit;
  ph->loc = loc;
  ph->ival = 0;
  return ph;
}

ExprP Parser::parseExpr(int minPrec) {
  if (exprDepth_ >= kMaxExprDepth) {
    d_.error(cur().loc, "expression nesting is too deep", "(123)");
    return nullptr;
  }
  DepthGuard g(exprDepth_);
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
  if (exprDepth_ >= kMaxExprDepth) {
    d_.error(cur().loc, "expression nesting is too deep", "(122)");
    return nullptr;
  }
  DepthGuard g(exprDepth_);
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
bool Parser::decodeHexLiteral(const std::string& hex, SourceLoc loc, std::string& out) {
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    return -1;
  };
  if (hex.size() % 2 != 0) {
    d_.error(loc, "a hex X literal needs an even digit count", "(143)");
    return false;
  }
  std::string bytes;
  for (size_t i = 0; i < hex.size(); i += 2) {
    int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      d_.error(loc, "a hex X literal needs hex digits", "(143)");
      return false;
    }
    bytes.push_back((char)(hi * 16 + lo));
  }
  out = bytes;
  return true;
}

ExprP Parser::parseTypeDesignator(const char* rule) {
  auto t = std::make_unique<Expr>();
  t->kind = Expr::TypeTag;
  t->loc = cur().loc;
  advance(); // :
  std::string raw;
  for (;;) {
    if (at(Tok::Eof) || at(Tok::Semi)) {
      d_.error(cur().loc, "expected ':' to close the type designator", rule);
      break;
    }
    if (eat(Tok::Colon))
      break;
    if (!cur().text.empty()) {
      if (!raw.empty())
        raw += ' ';
      raw += cur().text;
    }
    advance();
  }
  t->sval = raw;
  return t;
}

ExprP Parser::parsePrimary() {
  auto e = std::make_unique<Expr>();
  e->loc = cur().loc;

  // replicated-string-constant ::= ( integer ) simple-string-constant (129)
  // Expand `(3)'AB'` into `'ABABAB'` at parse time; also covers bit strings
  // and hex X literals (decoded to bytes below).
  if (at(Tok::LParen) && peek().kind == Tok::Number && !peek().isFloat &&
      peek(2).kind == Tok::RParen &&
      (peek(3).kind == Tok::CharLit || peek(3).kind == Tok::BitLit ||
       peek(3).kind == Tok::HexLit)) {
    advance(); // (
    long long count = strtoll(cur().text.c_str(), nullptr, 10);
    SourceLoc cl = cur().loc;
    advance(); // integer
    advance(); // )
    const Token& lit = cur();
    SourceLoc ll = lit.loc;
    advance(); // simple-string-constant
    // A replicated constant is expanded at parse time: bound the result so a
    // short source line cannot ask for gigabytes of memory.
    if (count < 0 || count > kMaxReplicatedChars / std::max<size_t>(1, lit.sval.size())) {
      d_.error(cl, "a replicated string constant is too large", "(129)");
      e->kind = Expr::CharLit;
      e->sval.clear();
      return e;
    }
    if (lit.kind == Tok::HexLit) {
      // Replicated hex (rule (129)+(143)): decode once, repeat the bytes.
      std::string one;
      if (!decodeHexLiteral(lit.sval, ll, one))
        return e;
      std::string out;
      for (long long i = 0; i < count; ++i)
        out += one;
      e->kind = Expr::CharLit;
      e->sval = out;
      return e;
    }
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
    if (t.sterling) {
      if (!parseSterlingConstant(t.text, *e))
        d_.error(t.loc, "invalid sterling constant", "(145)");
      advance();
      return e;
    }
    if (t.imaginary) {
      // Imaginary constant 2i (rule 139): 0 + value*i as a double pair part.
      e->kind = Expr::ComplexLit;
      if (t.isFloat)
        e->fval = strtod(t.text.c_str(), nullptr);
      else if (t.binaryRadix)
        e->fval = (double)strtoll(t.text.c_str(), nullptr, 2);
      else
        e->fval = (double)strtoll(t.text.c_str(), nullptr, 10);
    } else if (t.isFloat) {
      if (t.binaryRadix)
        d_.error(e->loc, "a binary radix suffix is not valid on a fractional constant", "(136)");
      if (t.hasExp) { // exponent form is a FLOAT constant (rule 135)
        e->kind = Expr::FltLit;
        e->fval = strtod(t.text.c_str(), nullptr);
      } else { // a bare fractional literal is an exact FIXED DECIMAL constant
        e->kind = Expr::DecLit;
        parseDecConstant(t.text, *e);
      }
    } else if (t.binaryRadix) {
      e->kind = Expr::IntLit;
      parseIntConstant(t.text, *e, 2);
    } else {
      e->kind = Expr::IntLit;
      parseIntConstant(t.text, *e, 10);
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
  if (at(Tok::HexLit)) {
    // Hex X literal (rule (143)): decode hex pairs to bytes; validation with
    // (143) keeps malformed digits out of every downstream stage.
    std::string out;
    SourceLoc ll = cur().loc;
    std::string hex = cur().sval;
    advance();
    if (!decodeHexLiteral(hex, ll, out))
      return e;
    e->kind = Expr::CharLit;
    e->sval = out;
    return e;
  }
  if (at(Tok::BitLit)) {
    e->kind = Expr::BitLit;
    e->sval = cur().sval;
    advance();
    return e;
  }
  if (at(Tok::Word)) {
    // One basic reference: WORD (.WORD)* [(args)] (.WORD)*
    // rules (124)-(126). The pre-subscript dots are member
    // qualifiers (S.A), the parens are subscripts or a call,
    // the post-subscript dots reach into an array element
    // (arr(i).x). Sema resolves the path and reclassifies.
    auto parseBasic = [this]() -> ExprP {
      auto b = std::make_unique<Expr>();
      b->kind = Expr::VarRef;
      b->loc = cur().loc;
      b->name = cur().text;
      advance();
      while (eat(Tok::Dot)) {
        if (at(Tok::Word)) {
          b->path.push_back(cur().text);
          advance();
        } else {
          d_.error(cur().loc, "expected a member name after '.'", "(124)");
          break;
        }
      }
      if (at(Tok::LParen)) {
        b->kind = Expr::Call;
        advance();
        if (!at(Tok::RParen)) {
          for (;;) {
            if (at(Tok::Star)) {
              auto star = std::make_unique<Expr>();
              star->kind = Expr::Star;
              star->loc = cur().loc;
              b->args.push_back(std::move(star));
              advance();
            } else if (at(Tok::Colon) && isEnquiryName(b->name)) {
              // A ': ... :' designator (ADR-206/207/208) names a declaration
              // or type for an enquiry built-in; sema interprets the text.
              b->args.push_back(parseTypeDesignator("(126)"));
            } else {
              b->args.push_back(parseExpr());
            }
            if (!eat(Tok::Comma))
              break;
          }
        }
        expect(Tok::RParen, "(126)");
      }
      while (eat(Tok::Dot)) {
        if (at(Tok::Word)) {
          b->path.push_back(cur().text);
          advance();
        } else {
          d_.error(cur().loc, "expected a member name after '.'", "(124)");
          break;
        }
      }
      return b;
    };
    ExprP lhs = parseBasic();
    // Locator qualification [reference ->] basic-reference
    // rule (124, left-recursive): the locator is any
    // pointer-valued reference (e.g. s.handles(i)), not
    // only a plain pointer variable. Chain for a -> b -> c.
    while (at(Tok::Arrow)) {
      advance(); // ->
      if (!at(Tok::Word)) {
        d_.error(cur().loc, "expected a based variable after '->'", "(124)");
        break;
      }
      ExprP rhs = parseBasic();
      rhs->locPtr = std::move(lhs);
      lhs = std::move(rhs);
    }
    return lhs;
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
    return k == Tok::Number || k == Tok::CharLit || k == Tok::BitLit || k == Tok::HexLit ||
           k == Tok::Word || k == Tok::Minus || k == Tok::Plus || k == Tok::Star ||
           k == Tok::LParen;
  };
  if (at(Tok::Star)) {
    advance();
    return InitItem{InitItem::Repeat, nullptr, 0, {}};
  }
  if (at(Tok::LParen)) {
    // A leading '(' is an iteration factor '( n )' when a value or sublist
    // follows it; a parenthesised constant '( n )' when ',' or ')' follows;
    // otherwise it is a group '( item, ... )'. Decide without consuming.
    // A replicated literal '( n )'X (rule (129)+(143)) is an iteration
    // factor over the hex constant, not a parenthesised number.
    bool iterHex =
        peek().kind == Tok::Number && peek(2).kind == Tok::RParen && peek(3).kind == Tok::HexLit;
    bool iter = iterHex || (peek().kind == Tok::Number && peek(2).kind == Tok::RParen &&
                            (peek(3).kind == Tok::LParen || valueStart(peek(3).kind)));
    bool parenConst = !iterHex && peek().kind == Tok::Number && peek(2).kind == Tok::RParen &&
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
