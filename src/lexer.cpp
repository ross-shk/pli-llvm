#include "lexer.h"
#include <cctype>
#include <cstdio>

const char *tokName(Tok t) {
  switch (t) {
    case Tok::Eof: return "end of file";
    case Tok::Word: return "identifier";
    case Tok::Number: return "numeric constant";
    case Tok::CharLit: return "character constant";
    case Tok::BitLit: return "bit constant";
    case Tok::Semi: return "';'";
    case Tok::Colon: return "':'";
    case Tok::Comma: return "','";
    case Tok::LParen: return "'('";
    case Tok::RParen: return "')'";
    case Tok::Dot: return "'.'";
    case Tok::Eq: return "'='";
    case Tok::Plus: return "'+'";
    case Tok::Minus: return "'-'";
    case Tok::Star: return "'*'";
    case Tok::Slash: return "'/'";
    case Tok::Power: return "'**'";
    case Tok::Concat: return "'||'";
    case Tok::Amp: return "'&'";
    case Tok::Bar: return "'|'";
    case Tok::Not: return "'not'";
    case Tok::Lt: return "'<'";
    case Tok::Le: return "'<='";
    case Tok::Gt: return "'>'";
    case Tok::Ge: return "'>='";
    case Tok::Ne: return "'not='";
    case Tok::Ngt: return "'not>'";
    case Tok::Nlt: return "'not<'";
    case Tok::Arrow: return "'->'";
  }
  return "token";
}

bool pliIsLetter(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '$' ||
         c == '#' || c == '@';
}

bool pliIsAlphameric(char c) {
  return pliIsLetter(c) || (c >= '0' && c <= '9') || c == '_';
}

void Lexer::bump() {
  if (p_ < s_.size()) {
    if (s_[p_] == '\n') {
      ++line_;
      col_ = 1;
    } else {
      ++col_;
    }
    ++p_;
  }
}

// Accepts the not-symbol in its three common spellings: the EBCDIC/Unicode
// glyph U+00AC (UTF-8 C2 AC), and the ASCII substitutes '^' and '~'.
bool Lexer::eatNot() {
  if (cur() == '^' || cur() == '~') {
    bump();
    return true;
  }
  if ((unsigned char)cur() == 0xC2 && (unsigned char)peek() == 0xAC) {
    bump();
    bump();
    return true;
  }
  return false;
}

void Lexer::skipSpaceAndComments() {
  for (;;) {
    while (cur() && (unsigned char)cur() <= ' ') bump();
    if (cur() == '/' && peek() == '*') {  // rule 150
      SourceLoc start = here();
      bump();
      bump();
      for (;;) {
        if (!cur()) {
          d_.error(start, "unterminated comment", "(150)");
          return;
        }
        if (cur() == '*' && peek() == '/') {
          bump();
          bump();
          break;
        }
        bump();
      }
      continue;
    }
    return;
  }
}

Token Lexer::lexWord() {
  Token t;
  t.kind = Tok::Word;
  t.loc = here();
  while (pliIsAlphameric(cur())) {
    t.text.push_back((char)toupper((unsigned char)cur()));
    bump();
  }
  return t;
}

// rules 135-139: integer, fixed-constant, float-constant, [B] radix suffix.
Token Lexer::lexNumber() {
  Token t;
  t.kind = Tok::Number;
  t.loc = here();
  while (isdigit((unsigned char)cur())) {
    t.text.push_back(cur());
    bump();
  }
  if (cur() == '.' && isdigit((unsigned char)peek())) {
    t.isFloat = true;
    t.text.push_back('.');
    bump();
    while (isdigit((unsigned char)cur())) {
      t.text.push_back(cur());
      bump();
    }
  }
  if (cur() == 'E' || cur() == 'e') {
    // Only an exponent if followed by [sign] digit.
    size_t save = p_;
    int sl = line_, sc = col_;
    std::string exp;
    exp.push_back('E');
    bump();
    if (cur() == '+' || cur() == '-') {
      exp.push_back(cur());
      bump();
    }
    if (isdigit((unsigned char)cur())) {
      while (isdigit((unsigned char)cur())) {
        exp.push_back(cur());
        bump();
      }
      t.text += exp;
      t.isFloat = true;
    } else {
      p_ = save;
      line_ = sl;
      col_ = sc;
    }
  }
  if (cur() == 'B' || cur() == 'b') {  // binary radix (rule 136)
    t.binaryRadix = true;
    bump();
  } else if (cur() == 'I' || cur() == 'i') {  // imaginary (rule 139)
    bump();
    d_.error(t.loc, "complex constants are not supported by this compiler stage",
             "(139)");
  }
  return t;
}

// rules 141/143: bit-string and character-string constants. '' denotes a
// contained quotation mark.
Token Lexer::lexString() {
  Token t;
  t.loc = here();
  bump();  // opening quote
  std::string v;
  for (;;) {
    if (!cur() || cur() == '\n') {
      d_.error(t.loc, "unterminated string constant", "(143)");
      break;
    }
    if (cur() == '\'') {
      if (peek() == '\'') {
        v.push_back('\'');
        bump();
        bump();
        continue;
      }
      bump();
      break;
    }
    v.push_back(cur());
    bump();
  }
  if (cur() == 'B' || cur() == 'b') {
    bump();
    t.kind = Tok::BitLit;
  } else {
    t.kind = Tok::CharLit;
  }
  t.sval = v;
  return t;
}

std::vector<Token> Lexer::run() {
  std::vector<Token> out;
  for (;;) {
    skipSpaceAndComments();
    if (!cur()) {
      Token t;
      t.kind = Tok::Eof;
      t.loc = here();
      out.push_back(t);
      return out;
    }

    SourceLoc loc = here();
    char c = cur();

    if (pliIsLetter(c)) {
      out.push_back(lexWord());
      continue;
    }
    if (isdigit((unsigned char)c) ||
        (c == '.' && isdigit((unsigned char)peek()))) {
      out.push_back(lexNumber());
      continue;
    }
    if (c == '\'') {
      out.push_back(lexString());
      continue;
    }

    Token t;
    t.loc = loc;
    auto one = [&](Tok k) {
      t.kind = k;
      bump();
    };
    switch (c) {
      case ';': one(Tok::Semi); break;
      case ':': one(Tok::Colon); break;
      case ',': one(Tok::Comma); break;
      case '(': one(Tok::LParen); break;
      case ')': one(Tok::RParen); break;
      case '.': one(Tok::Dot); break;
      case '+': one(Tok::Plus); break;
      case '&': one(Tok::Amp); break;
      case '=': one(Tok::Eq); break;
      case '*':
        bump();
        if (cur() == '*') {
          bump();
          t.kind = Tok::Power;
        } else {
          t.kind = Tok::Star;
        }
        break;
      case '/': one(Tok::Slash); break;
      case '|':
        bump();
        if (cur() == '|') {
          bump();
          t.kind = Tok::Concat;
        } else {
          t.kind = Tok::Bar;
        }
        break;
      case '-':
        bump();
        if (cur() == '>') {
          bump();
          t.kind = Tok::Arrow;
        } else {
          t.kind = Tok::Minus;
        }
        break;
      case '<':
        bump();
        if (cur() == '=') {
          bump();
          t.kind = Tok::Le;
        } else {
          t.kind = Tok::Lt;
        }
        break;
      case '>':
        bump();
        if (cur() == '=') {
          bump();
          t.kind = Tok::Ge;
        } else {
          t.kind = Tok::Gt;
        }
        break;
      default:
        if (eatNot()) {  // ¬ ¬= ¬> ¬<
          if (cur() == '=') {
            bump();
            t.kind = Tok::Ne;
          } else if (cur() == '>') {
            bump();
            t.kind = Tok::Ngt;
          } else if (cur() == '<') {
            bump();
            t.kind = Tok::Nlt;
          } else {
            t.kind = Tok::Not;
          }
          break;
        }
        // Encoding damage, not language: diagnose it readably.
        if ((unsigned char)cur() == 0xC3 && (unsigned char)peek() == 0x82 &&
            (unsigned char)peek(2) == 0xC2 && (unsigned char)peek(3) == 0xAC) {
          d_.error(loc, "not sign is doubly encoded (0xc3 0x82 0xc2 0xac); write it as '^'");
          bump(); bump(); bump(); bump();
          continue;
        }
        if ((unsigned char)cur() >= 0x80) {
          char buf[72];
          std::snprintf(buf, sizeof buf, "invalid character (0x%02x) in source",
                        (unsigned char)cur());
          d_.error(loc, buf);
          bump();
          continue;
        }
        d_.error(loc, std::string("invalid character '") + c + "' in source");
        bump();
        continue;
    }
    out.push_back(t);
  }
}
