// parser.h — recursive-descent parser for the M0 subset of TR 25.084.
#pragma once
#include "ast.h"
#include "diag.h"
#include "token.h"
#include <memory>
#include <string>
#include <vector>

// Result of encountering an END clause (rule 7). `label` non-empty means the
// END names an *enclosing* block, i.e. multiple closure (TR §2.3.2.2).
struct EndInfo {
  bool present = false;
  std::string label;
  SourceLoc loc{};
};

class Parser {
public:
  Parser(std::vector<Token> toks, Diags& d) : t_(std::move(toks)), d_(d) {}
  std::unique_ptr<Program> parse();

private:
  // --- token access ---------------------------------------------------
  const Token& cur() const { return t_[i_]; }
  const Token& peek(size_t n = 1) const {
    size_t j = i_ + n;
    return j < t_.size() ? t_[j] : t_.back();
  }
  bool at(Tok k) const { return cur().kind == k; }
  bool atWord(const char* w) const { return cur().isWord(w); }
  void advance() {
    if (i_ + 1 < t_.size())
      ++i_;
  }
  bool eat(Tok k) {
    if (at(k)) {
      advance();
      return true;
    }
    return false;
  }
  bool eatWord(const char* w) {
    if (atWord(w)) {
      advance();
      return true;
    }
    return false;
  }
  bool expect(Tok k, const char* rule = "");
  void resync(); // error recovery: skip to next ';'

  // --- contextual keyword recognition (ADR-004) -----------------------
  bool atStmtKeyword(const char* w) const;
  bool looksLikeAssignment() const;
  StmtP keywordStatement(Proc* owner, const std::vector<std::string>& labels, bool probe);
  StmtP probeKeywordStatement(Proc* owner, const std::vector<std::string>& labels);

  // --- grammar --------------------------------------------------------
  void parseExternalProcedure();
  void parsePackage(const std::string& name, SourceLoc loc); // PACKAGE (extension, ADR-109)
  Proc* startProc(const std::string& name, SourceLoc loc, Proc* parent);
  EndInfo parseProcBody(Proc* p);
  EndInfo parseBody(Proc* owner, std::vector<StmtP>& body, const std::string& ownName);
  StmtP parseStatement(Proc* owner);
  StmtP parseDeclare();
  StmtP parseIf(Proc* owner);
  StmtP parseDo(Proc* owner, const std::vector<std::string>& labels);
  StmtP parseSelect(Proc* owner);        // SELECT (extension, ADR-104): desugars to IFs
  StmtP parseLoopExit(bool isIterate);   // LEAVE/ITERATE [label] (extension, ADR-105)
  StmtP parseDefineAlias();              // DEFINE ALIAS name attrs (extension, ADR-114)
  static ExprP cloneExpr(const Expr* e); // deep copy for SELECT value lists
  // Decode a hex X literal payload to bytes (rule (143)); false after a
  // diagnostic, in which case out is untouched.
  bool decodeHexLiteral(const std::string& hex, SourceLoc loc, std::string& out);
  StmtP parsePut();
  StmtP parseDisplay(); // display-statement (rule 114)
  StmtP parseGet();
  // Edit-directed transmission (rule (108)): parse `EDIT ( { ( datalist )
  // formatlist }••• )` into st->items/st->formats with st->edit set. Returns
  // true on success; on error emits a diagnostic, resyncs, and returns false.
  bool parseEditClause(Stmt* st);
  // Parse one format item (rules (46)-(54)) into st->formats; returns false
  // (after a diagnostic) on an unimplemented or malformed item. An
  // iteration group (n)(...) unrolls inline, nesting by recursion.
  bool parseFormatItem(Stmt* st);
  // Parse one format item into out (groups unroll here); single items land
  // in parseSingleFormatItem.
  bool parseFormatItemInto(std::vector<FormatItem>& out);
  // Parse one non-group format item into fi.
  bool parseSingleFormatItem(FormatItem& fi);
  StmtP parseCall();
  StmtP parseWait();  // WAIT (rule 82, QR2.8): WAIT(ev,...)[(count)];
  StmtP parseDelay(); // DELAY (rule 83, QR2.8): DELAY(expr);
  StmtP parseAssignment();
  StmtP parseOn(Proc* owner); // rule (91)
  StmtP parseRevert();        // rule (92)
  StmtP parseSignal();        // rule (93)
  // Parse one condition (rule 94); returns its name ("ERROR" or a rule (99)
  // programmer-named condition, otherwise "" after diagnosing the
  // unimplemented condition).
  std::string parseCondition();
  // Consume a balanced (...) group (e.g. an unimplemented CHECK list).
  void skipParen(const char* rule);
  StmtP parseAllocate(); // ALLOCATE (rule 87)
  StmtP parseFree();     // FREE (rule 90)
  StmtP parseOpen();     // OPEN (rules 100,101)
  StmtP parseClose();    // CLOSE (rules 102,103)
  StmtP parseRecordIO(); // READ/WRITE (rules (112),(113)): sequential slice
  void parseProcOptions(Proc* p);
  bool parseDeclItem(DeclItem& item);
  // Parse the dimension + attribute tail shared by a declaration item and by a
  // factored declaration list (rule 11); builds item.ty / item.dims / item.init.
  void parseDeclTail(DeclItem& item);
  // Try to parse a dimension (rules (12),(13)) at the current LParen. Consumes
  // tokens only when it is genuinely a dimension; returns false (with the token
  // stream restored) so the caller can treat the group as a precision/length.
  // Fills `out` with the per-axis Dim (a dynamic axis is marked `dyn`/`lbDyn`)
  // and `dynBounds` with the runtime upper-bound expression of each dynamic
  // upper bound, `dynLbBounds` with the runtime lower-bound expression of each
  // dynamic lower bound (null entries for constant axes). Supports (ub),
  // (lb:ub), and (expr)/(lb:expr).
  bool tryParseDimension(std::vector<Dim>& out, std::vector<ExprP>& dynBounds,
                         std::vector<ExprP>& dynLbBounds);
  bool parseDescriptorType(Type& out);              // one ENTRY parameter type (rule 38)
  bool parseEntryParams(std::vector<Type>& params); // ENTRY ( ... )

  // Shared scalar-computational attribute accumulator (rules 15-18). Both
  // parseDeclItem (rule 11) and parseDescriptorType (rule 38) consume the same
  // attribute words; only their conflict checking and type selection differ.
  struct AttrBag {
    bool fixed = false, floating = false, binary = false, decimal = false;
    bool character = false, bit = false, varying = false;
    bool pointer = false;
    bool complex = false; // COMPLEX (QR2.2/CM5): a real+imaginary pair
    bool file = false;    // FILE (rules 39,40): a named file variable
    bool task = false;    // TASK (rules (15),(79), QR2.8): a task name
    bool event = false;   // EVENT (rules (15),(79),(82), QR2.8): an event name
    int prec = -1, scale = 0, slen = -1;
    bool starLen = false; // '*' string length (rule (18)): adjustable extent
  };
  // Consume one attribute word (FIXED, FLOAT, BINARY, DECIMAL, CHARACTER,
  // BIT, VARYING, REAL) with its optional precision into `bag`. Returns true
  // if the current token was such an attribute. `rule` cites the TR production
  // for the precision parenthesised group (16 for a declaration, 38 for a
  // descriptor parameter).
  bool parseScalarAttr(AttrBag& bag, const char* rule);

  // --- expressions (rules 115-129) ------------------------------------
  ExprP parseExpr(int minPrec = 1);
  ExprP parseUnary();
  ExprP parsePower();
  ExprP parsePrimary();

  // --- INITIAL itemlist (rules 26-31) ---------------------------------
  // Parse a comma-separated list of INITIAL items, stopping at the enclosing
  // ')'. Returns the item tree for sema to expand.
  std::vector<InitItem> parseInitialList();
  // Parse one INITIAL item: a constant, an iteration factor (n) value/sublist,
  // a '*' repeat-last, or a parenthesised group.
  InitItem parseInitialItem();

  std::vector<Token> t_;
  Diags& d_;
  size_t i_ = 0;
  std::unique_ptr<Program> prog_ = std::make_unique<Program>();
  EndInfo pendingEnd_{}; // propagates multiple closure outward
};
