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
  Proc* startProc(const std::string& name, SourceLoc loc, Proc* parent);
  EndInfo parseProcBody(Proc* p);
  EndInfo parseBody(Proc* owner, std::vector<StmtP>& body, const std::string& ownName);
  StmtP parseStatement(Proc* owner);
  StmtP parseDeclare();
  StmtP parseIf(Proc* owner);
  StmtP parseDo(Proc* owner, const std::vector<std::string>& labels);
  StmtP parsePut();
  StmtP parseCall();
  StmtP parseAssignment();
  void parseProcOptions(Proc* p);
  bool parseDeclItem(DeclItem& item);
  // Parse the dimension + attribute tail shared by a declaration item and by a
  // factored declaration list (rule 11); builds item.ty / item.dims / item.init.
  bool parseDeclTail(DeclItem& item);
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
    int prec = -1, scale = 0, slen = -1;
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
