// parser.h — recursive-descent parser for the M0 subset of TR 25.084.
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "ast.h"
#include "diag.h"
#include "token.h"

// Result of encountering an END clause (rule 7). `label` non-empty means the
// END names an *enclosing* block, i.e. multiple closure (TR §2.3.2.2).
struct EndInfo {
  bool present = false;
  std::string label;
  SourceLoc loc{};
};

class Parser {
public:
  Parser(std::vector<Token> toks, Diags &d) : t_(std::move(toks)), d_(d) {}
  std::unique_ptr<Program> parse();

private:
  // --- token access ---------------------------------------------------
  const Token &cur() const { return t_[i_]; }
  const Token &peek(size_t n = 1) const {
    size_t j = i_ + n;
    return j < t_.size() ? t_[j] : t_.back();
  }
  bool at(Tok k) const { return cur().kind == k; }
  bool atWord(const char *w) const { return cur().isWord(w); }
  void advance() { if (i_ + 1 < t_.size()) ++i_; }
  bool eat(Tok k) { if (at(k)) { advance(); return true; } return false; }
  bool eatWord(const char *w) { if (atWord(w)) { advance(); return true; } return false; }
  bool expect(Tok k, const char *rule = "");
  void resync();  // error recovery: skip to next ';'

  // --- contextual keyword recognition (ADR-004) -----------------------
  bool atStmtKeyword(const char *w) const;
  bool looksLikeAssignment() const;
  StmtP keywordStatement(Proc *owner, const std::vector<std::string> &labels,
                         bool probe);
  StmtP probeKeywordStatement(Proc *owner,
                              const std::vector<std::string> &labels);

  // --- grammar --------------------------------------------------------
  void parseExternalProcedure();
  Proc *startProc(const std::string &name, SourceLoc loc, Proc *parent);
  EndInfo parseProcBody(Proc *p);
  EndInfo parseBody(Proc *owner, std::vector<StmtP> &body, const std::string &ownName);
  StmtP parseStatement(Proc *owner);
  StmtP parseDeclare();
  StmtP parseIf(Proc *owner);
  StmtP parseDo(Proc *owner, const std::vector<std::string> &labels);
  StmtP parsePut();
  StmtP parseCall();
  StmtP parseAssignment();
  void parseProcOptions(Proc *p);
  bool parseDeclItem(DeclItem &item);
  bool parseDescriptorType(Type &out);  // one ENTRY parameter type (rule 38)
  bool parseEntryParams(std::vector<Type> &params);  // ENTRY ( ... )

  // --- expressions (rules 115-129) ------------------------------------
  ExprP parseExpr(int minPrec = 1);
  ExprP parseUnary();
  ExprP parsePower();
  ExprP parsePrimary();

  std::vector<Token> t_;
  Diags &d_;
  size_t i_ = 0;
  std::unique_ptr<Program> prog_ = std::make_unique<Program>();
  EndInfo pendingEnd_{};  // propagates multiple closure outward
};
