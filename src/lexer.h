// lexer.h — PL/I lexical analysis (TR 25.084 §3.2.1, §3.2.3).
#pragma once
#include <string>
#include <vector>
#include "token.h"

class Lexer {
public:
  Lexer(const std::string &src, Diags &d) : s_(src), d_(d) {}

  // Tokenizes the whole unit. Comments and spaces are consumed (rule 149/150).
  std::vector<Token> run();

private:
  char cur() const { return p_ < s_.size() ? s_[p_] : '\0'; }
  char peek(size_t n = 1) const { return p_ + n < s_.size() ? s_[p_ + n] : '\0'; }
  void bump();
  bool eatNot();  // consumes ¬ / ^ / ~ in any encoding
  SourceLoc here() const { return {line_, col_}; }
  void skipSpaceAndComments();
  Token lexWord();
  Token lexNumber();
  Token lexString();

  const std::string &s_;
  Diags &d_;
  size_t p_ = 0;
  int line_ = 1, col_ = 1;
};

// True for PL/I "letters": A-Z plus the alphabetic extenders $ # @ (rule 131).
bool pliIsLetter(char c);
// alphameric-character (rule 132): letter | digit | break character
bool pliIsAlphameric(char c);
