// token.h — token model.
//
// DESIGN: PL/I has no reserved words. `IF IF = THEN THEN THEN = ELSE;` is a
// legal statement. Therefore the lexer never classifies a word as a keyword:
// every word becomes Tok::Word carrying its (upper-cased) spelling, and the
// parser performs keyword recognition positionally, with bounded lookahead.
// See docs/DESIGN-DECISIONS.md ADR-004.
#pragma once
#include "diag.h"
#include <string>
#include <string_view>

enum class Tok {
  Eof,
  Word,     // identifier or contextual keyword
  Number,   // arithmetic constant
  Isub,     // iSUB dummy variable, 'integer SUB' with no blanks (rule 134)
  CharLit,  // '...'   (character-string constant)
  BitLit,   // '...'B  (bit-string constant)
  DqString, // "..."  (extension: quoted %REPLACE replacement, ADR-082)
  // Punctuation
  Semi,
  Colon,
  Comma,
  LParen,
  RParen,
  Dot,
  // Operators
  Eq, // =  (assignment symbol and equality comparison; context decides)
  Plus,
  Minus,
  Star,
  Slash,
  Power,  // + - * / **
  Concat, // ||
  Amp,
  Bar,
  Not, // & | ¬  (also ^ ~ for ¬ on ASCII keyboards)
  Lt,
  Le,
  Gt,
  Ge,
  Ne,
  Ngt,
  Nlt,     // < <= > >= ¬= ¬> ¬<
  Arrow,   // ->  locator qualification
  Percent, // %  (extension: preprocessor directives, ADR-077)
};

struct Token {
  Tok kind = Tok::Eof;
  std::string text; // upper-cased spelling for Word; digits for Number
  std::string sval; // decoded value for CharLit/BitLit
  SourceLoc loc{};
  bool binaryRadix = false; // Number had the B suffix (rule 136)
  bool imaginary = false;   // Number had the I suffix (rule 139)
  bool isFloat = false;
  bool hasExp = false; // Number had an E exponent (a FLOAT constant, rule 135)

  bool isWord(std::string_view w) const { return kind == Tok::Word && text == w; }
};

const char* tokName(Tok t);
