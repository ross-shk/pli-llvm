// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
#include "preprocessor.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

std::string upper(std::string value) {
  for (char& c : value)
    c = (char)std::toupper((unsigned char)c);
  return value;
}

std::string trim(const std::string& value) {
  size_t first = value.find_first_not_of(" \t\r\n\f");
  if (first == std::string::npos)
    return {};
  size_t last = value.find_last_not_of(" \t\r\n\f");
  return value.substr(first, last - first + 1);
}

bool isWordChar(char c) {
  return std::isalnum((unsigned char)c) || c == '_' || c == '$' || c == '#' || c == '@';
}

fs::path normalized(const fs::path& path) {
  std::error_code ec;
  fs::path result = fs::weakly_canonical(path, ec);
  return ec ? fs::absolute(path).lexically_normal() : result;
}

} // namespace

bool Preprocessor::run(const fs::path& input, std::string& output) {
  active_.clear();
  included_.clear();
  ppVars_.clear();
  ppActive_.clear();
  labels_.clear();
  gotoTarget_ = 0;
  scanDepth_ = 0;
  output.clear();
  lineMap_.clear();
  fileSrc_.clear();
  rootCanon_.clear();
  rootCanon_ = normalized(input).string();
  return expand(input, output);
}

void Preprocessor::addIncludeDir(const fs::path& dir) { includeDirs_.push_back(dir); }

namespace {

// Integer expressions for %IF and %assignment (ADR-083): literals,
// %DECLARE variables, parens, +-*/ (unary minus), comparisons, and & | ¬
// (any of ¬ ^ ~ spellings). Comparisons yield 0/1; nonzero is true.
struct PPCondParser {
  const std::string& text;
  size_t pos = 0;
  const std::map<std::string, long long>& vars;
  std::string error;
  // Parenthesis nesting: a deeply nested %IF expression is diagnosed
  // instead of overflowing the compiler's stack.
  int depth = 0;
  static constexpr int kMaxDepth = 128;
  struct DepthGuard {
    int& d;
    explicit DepthGuard(int& depth) : d(depth) { ++d; }
    ~DepthGuard() { --d; }
  };

  void skipWs() {
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r' ||
                                 text[pos] == '\n' || text[pos] == '\f'))
      ++pos;
  }
  bool atEnd() {
    skipWs();
    return pos >= text.size();
  }
  // A not-sign in any accepted spelling: ¬ (UTF-8 or single byte), ^, ~.
  bool matchNot() {
    if (pos < text.size() && (text[pos] == '^' || text[pos] == '~')) {
      ++pos;
      return true;
    }
    if (pos + 1 < text.size() && (unsigned char)text[pos] == 0xC2 &&
        (unsigned char)text[pos + 1] == 0xAC) {
      pos += 2;
      return true;
    }
    if (pos < text.size() && (unsigned char)text[pos] == 0xAC) {
      ++pos;
      return true;
    }
    return false;
  }
  long long parseOr() {
    long long v = parseAnd();
    for (;;) {
      skipWs();
      if (pos < text.size() && text[pos] == '|') {
        ++pos;
        long long rhs = parseAnd();
        v = (v != 0 || rhs != 0) ? 1 : 0;
      } else {
        return v;
      }
    }
  }
  long long parseAnd() {
    long long v = parseNot();
    for (;;) {
      skipWs();
      if (pos < text.size() && text[pos] == '&') {
        ++pos;
        long long rhs = parseNot();
        v = (v != 0 && rhs != 0) ? 1 : 0;
      } else {
        return v;
      }
    }
  }
  long long parseNot() {
    skipWs();
    // A not-equals operator starts with a not-sign too; try it first.
    size_t save = pos;
    if (matchNot()) {
      skipWs();
      if (pos < text.size() && text[pos] == '=') {
        pos = save; // rewind: this belongs to parseCmp
      } else {
        return parseNot() != 0 ? 0 : 1;
      }
    }
    return parseCmp();
  }
  long long parseCmp() {
    long long v = parseAdd();
    for (;;) {
      skipWs();
      if (pos < text.size() && text[pos] == '=') {
        ++pos;
        long long rhs = parseAdd();
        v = (v == rhs) ? 1 : 0;
      } else if (pos < text.size() && (text[pos] == '<' || text[pos] == '>')) {
        bool less = text[pos] == '<';
        ++pos;
        bool eq = false;
        if (pos < text.size() && text[pos] == '=') {
          eq = true;
          ++pos;
        }
        long long rhs = parseAdd();
        v = (less ? (eq ? v <= rhs : v < rhs) : (eq ? v >= rhs : v > rhs)) ? 1 : 0;
      } else {
        size_t save = pos;
        if (matchNot()) {
          skipWs();
          if (pos < text.size() && text[pos] == '=') {
            ++pos;
            long long rhs = parseAdd();
            v = (v != rhs) ? 1 : 0;
            continue;
          }
        }
        pos = save;
        return v;
      }
    }
  }
  long long parseAdd() {
    long long v = parseMul();
    for (;;) {
      skipWs();
      if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
        char op = text[pos++];
        long long rhs = parseMul();
        v = (op == '+') ? v + rhs : v - rhs;
      } else {
        return v;
      }
    }
  }
  long long parseMul() {
    long long v = parseUnary();
    for (;;) {
      skipWs();
      if (pos < text.size() && (text[pos] == '*' || text[pos] == '/')) {
        char op = text[pos++];
        long long rhs = parseUnary();
        if (op == '/') {
          if (rhs == 0) {
            error = "division by zero in preprocessor expression";
            return 0;
          }
          v /= rhs;
        } else {
          v *= rhs;
        }
      } else {
        return v;
      }
    }
  }
  long long parseUnary() {
    skipWs();
    if (pos < text.size() && text[pos] == '-') {
      ++pos;
      return -parseUnary();
    }
    return parsePrimary();
  }
  long long parsePrimary() {
    skipWs();
    if (pos < text.size() && text[pos] == '(') {
      if (depth >= kMaxDepth) {
        error = "preprocessor expression nesting is too deep";
        return 0;
      }
      DepthGuard g(depth);
      ++pos;
      long long v = parseOr();
      skipWs();
      if (pos >= text.size() || text[pos] != ')') {
        error = "expected ')' in preprocessor expression";
        return 0;
      }
      ++pos;
      return v;
    }
    if (pos < text.size() && std::isdigit((unsigned char)text[pos])) {
      long long v = 0;
      while (pos < text.size() && std::isdigit((unsigned char)text[pos]))
        v = v * 10 + (text[pos++] - '0');
      return v;
    }
    if (pos < text.size() && (std::isalpha((unsigned char)text[pos]) || text[pos] == '_' ||
                              text[pos] == '$' || text[pos] == '#' || text[pos] == '@')) {
      size_t start = pos;
      while (pos < text.size() && (std::isalnum((unsigned char)text[pos]) || text[pos] == '_' ||
                                   text[pos] == '$' || text[pos] == '#' || text[pos] == '@'))
        ++pos;
      std::string name = upper(text.substr(start, pos - start));
      auto it = vars.find(name);
      if (it == vars.end()) {
        error = "unknown preprocessor variable '%" + name + "'";
        return 0;
      }
      return it->second;
    }
    error = "expected a number, variable, or '(' in preprocessor expression";
    return 0;
  }
};

} // namespace

bool Preprocessor::evalCondExpr(const std::string& text, const fs::path& path, int line, int col,
                                long long& out) {
  PPCondParser p{trim(text), 0, ppVars_, {}};
  out = p.parseOr();
  if (!p.error.empty()) {
    error(path, line, col, p.error);
    return false;
  }
  if (!p.atEnd()) {
    error(path, line, col, "unexpected text in preprocessor expression");
    return false;
  }
  return true;
}

// Parse and run one %-directive at `i` (source[i] == '%'). Returns the index
// past it (npos after diagnosing). With active == false the directive is
// skipped without effects; newlines stay caller-preserved. Sets semPos to the
// last character position of the directive's own text so the caller can
// preserve its newlines (ADR-305).
size_t Preprocessor::processDirective(const fs::path& path, const std::string& source, size_t i,
                                      std::string& output, int& line, int& col, bool active,
                                      size_t& semPos) {
  static const size_t npos = std::string::npos;
  semPos = i;
  int directiveLine = line;
  int directiveCol = col;
  size_t wordStart = i + 1;
  while (wordStart < source.size() &&
         (source[wordStart] == ' ' || source[wordStart] == '\t' || source[wordStart] == '\f'))
    ++wordStart;
  size_t wordEnd = wordStart;
  while (wordEnd < source.size() &&
         (std::isalnum((unsigned char)source[wordEnd]) || source[wordEnd] == '_' ||
          source[wordEnd] == '$' || source[wordEnd] == '#' || source[wordEnd] == '@'))
    ++wordEnd;
  if (wordEnd == wordStart) {
    error(path, directiveLine, directiveCol, "expected a preprocessor directive");
    return npos;
  }

  std::string name = upper(source.substr(wordStart, wordEnd - wordStart));
  if (name == "IF") {
    size_t next = processIf(path, source, i, wordEnd, output, line, col, active, semPos);
    return next;
  }
  if (name == "THEN" || name == "ELSE") {
    error(path, directiveLine, directiveCol, "%" + name + " without %IF");
    semPos = wordEnd;
    return npos;
  }

  // %label: — label marker (target for %GOTO). collectLabels() already
  // recorded the target position; here we just skip the label prefix so
  // scanning resumes at the labeled directive (or past ';' if standalone).
  if (wordEnd < source.size() && source[wordEnd] == ':') {
    size_t target = wordEnd + 1;
    while (target < source.size() &&
           (source[target] == ' ' || source[target] == '\t' || source[target] == '\f' ||
            source[target] == '\n' || source[target] == '\r'))
      ++target;
    semPos = target - 1;
    if (target < source.size() && source[target] == '%')
      return target;
    size_t semi = findDirectiveEnd(source, target);
    if (semi != npos)
      return semi + 1;
    return target;
  }

  // %DO [v=e1 TO e2 [BY e3]] ; — find matching %END and dispatch the loop.
  if (name == "DO") {
    size_t doEnd = findDirectiveEnd(source, wordEnd);
    if (doEnd == npos) {
      error(path, directiveLine, directiveCol, "unterminated %DO (C28-6571-3 Ch.9)");
      semPos = wordEnd;
      return npos;
    }
    semPos = doEnd;
    size_t endSemi;
    size_t matchingEnd = findMatchingEnd(path, source, doEnd + 1, endSemi, line, col);
    if (matchingEnd == npos)
      return npos;
    if (active) {
      return handleDo(path, source, doEnd + 1, matchingEnd, endSemi, output, line, col, active,
                      source.substr(wordEnd, doEnd - wordEnd), directiveLine, directiveCol);
    }
    // Inactive %DO: skip past matching %END; preserving its newlines.
    for (size_t j = matchingEnd; j <= endSemi; ++j) {
      if (source[j] == '\n') {
        output += '\n';
        lineMap_.push_back({path.string(), line});
        ++line;
        col = 1;
      } else {
        ++col;
      }
    }
    return endSemi + 1;
  }

  // %END without %DO — only reached if findMatchingEnd didn't consume it.
  if (name == "END") {
    error(path, directiveLine, directiveCol, "%END without %DO (C28-6571-3 Ch.9)");
    semPos = wordEnd;
    return npos;
  }

  // %GOTO label ; / %GO TO label ; — resolve target and jump.
  if (name == "GO" || name == "GOTO") {
    size_t end = findDirectiveEnd(source, wordEnd);
    if (end == npos) {
      error(path, directiveLine, directiveCol, "unterminated %GOTO (C28-6571-3 Ch.9)");
      semPos = wordEnd;
      return npos;
    }
    semPos = end;
    if (active) {
      if (!handleGoto(path, name, source.substr(wordEnd, end - wordEnd), directiveLine, directiveCol))
        return npos;
      return gotoTarget_;
    }
    return end + 1;
  }

  // %PROCEDURE / %PROC / %RETURN — compile-time macros (Stage 2).
  if (name == "PROCEDURE" || name == "PROC" || name == "RETURN") {
    size_t end = findDirectiveEnd(source, wordEnd);
    if (end == npos) {
      error(path, directiveLine, directiveCol, "unterminated preprocessor directive");
      semPos = wordEnd;
      return npos;
    }
    semPos = end;
    if (active) {
      return handleProcedure(path, source, i, wordEnd, end, output, line, col, active);
    }
    return end + 1;
  }

  // Find ';' for standard directives.
  size_t end = findDirectiveEnd(source, wordEnd);
  if (end == npos) {
    error(path, directiveLine, directiveCol, "unterminated preprocessor directive");
    semPos = wordEnd;
    return npos;
  }
  semPos = end;
  bool ok = false;
  if (name == "INCLUDE") {
    if (active)
      ok = handleInclude(path, source.substr(wordEnd, end - wordEnd), directiveLine, directiveCol,
                         output);
    else
      ok = true;
  } else if (name == "XINCLUDE") {
    if (active)
      ok = handleXInclude(path, source.substr(wordEnd, end - wordEnd), directiveLine, directiveCol,
                          output);
    else
      ok = true;
  } else if (name == "DECLARE" || name == "DCL") {
    if (active)
      ok = handleDeclare(path, source.substr(wordEnd, end - wordEnd), directiveLine, directiveCol);
    else
      ok = true;
  } else if (name == "ACTIVATE" || name == "DEACTIVATE") {
    if (active)
      ok = handleActivation(path, name, source.substr(wordEnd, end - wordEnd), directiveLine,
                            directiveCol);
    else
      ok = true;
  } else if (name == "REPLACE") {
    if (active) {
      // Served downstream by the lexer's token substitution (ADR-077):
      // preserve the directive text verbatim, minus newlines (the caller
      // re-adds those, keeping output line numbers aligned).
      for (size_t j = i; j <= end; ++j)
        if (source[j] != '\n')
          output += source[j];
    }
    ok = true;
  } else {
    // Bare %NAME (extension, ADR-113): a reference expanding an activated
    // variable. Anything else stays an assignment attempt. The reference
    // ends at the name itself (mid-statement uses like `a(%N)` never reach
    // the directive-terminating `;`).
    size_t rs = wordEnd;
    while (rs < source.size() && (source[rs] == ' ' || source[rs] == '\t' || source[rs] == '\f'))
      ++rs;
    if (rs >= source.size() || source[rs] != '=') {
      if (active)
        ok = substituteRef(path, name, directiveLine, directiveCol, output);
      else
        ok = true;
      if (!ok)
        return npos;
      return wordEnd;
    }
    if (active)
      ok = handleAssignment(path, name, source.substr(wordEnd, end - wordEnd), directiveLine,
                            directiveCol);
    else
      ok = true;
  }
  if (!ok)
    return npos;
  return end + 1;
}

// %IF expr %THEN directive [%ELSE directive] (ADR-083). Arms are full
// directives run (or structurally skipped) recursively, so nesting works;
// each arm is exactly one directive. Sets semPos to the last character of
// the %IF...%THEN header (before the arm) so the caller preserves its
// newlines; arm text is emitted directly via emit()/processDirective.
size_t Preprocessor::processIf(const fs::path& path, const std::string& source, size_t,
                               size_t wordEnd, std::string& output, int& line, int& col,
                               bool active, size_t& semPos) {
  static const size_t npos = std::string::npos;
  int directiveLine = line;
  int directiveCol = col;
  // Find %THEN: the first %word in the expression span (expressions hold
  // no directives). Comments cannot hide one: the main loop strips them
  // before directives are ever scanned... except inside this raw span, so
  // skip comments and strings here too (ADR-156 covers //).
  size_t t = wordEnd;
  size_t thenAt = npos;
  while (t < source.size()) {
    if (source[t] == '/' && t + 1 < source.size() && source[t + 1] == '*') {
      t += 2;
      while (t + 1 < source.size() && !(source[t] == '*' && source[t + 1] == '/'))
        ++t;
      t += 2;
      continue;
    }
    if (source[t] == '/' && t + 1 < source.size() && source[t + 1] == '/') {
      // Line comment (ADR-156): hides %THEN and ';' to end of line.
      while (t < source.size() && source[t] != '\n')
        ++t;
      continue;
    }
    if (source[t] == '\'') {
      // Quoted text hides %THEN and ';' ('' is an escaped quote).
      ++t;
      while (t < source.size()) {
        if (source[t] == '\'' && t + 1 < source.size() && source[t + 1] == '\'') {
          t += 2;
          continue;
        }
        if (source[t] == '\'')
          break;
        ++t;
      }
      ++t;
      continue;
    }
    if (source[t] == '%') {
      size_t ws = t + 1;
      while (ws < source.size() && (source[ws] == ' ' || source[ws] == '\t' || source[ws] == '\f'))
        ++ws;
      size_t we = ws;
      while (we < source.size() && (std::isalnum((unsigned char)source[we]) || source[we] == '_' ||
                                     source[we] == '$' || source[we] == '#' || source[we] == '@'))
        ++we;
      if (upper(source.substr(ws, we - ws)) == "THEN") {
        thenAt = t;
        break;
      }
      error(path, directiveLine, directiveCol, "expected %THEN in %IF directive");
      return npos;
    }
    if (source[t] == ';') {
      error(path, directiveLine, directiveCol, "expected %THEN in %IF directive");
      return npos;
    }
    ++t;
  }
  if (thenAt == npos) {
    error(path, directiveLine, directiveCol, "expected %THEN in %IF directive");
    return npos;
  }
  long long cond = 0;
  if (active && !evalCondExpr(source.substr(wordEnd, thenAt - wordEnd), path, directiveLine,
                              directiveCol, cond))
    return npos;
  // One directive or statement per arm, at the caller's activity gated by the condition.
  // thenAt points at '%': re-scan to the end of the THEN word itself, since
  // spaces may sit between them.
  size_t arm = thenAt + 1;
  while (arm < source.size() && (source[arm] == ' ' || source[arm] == '\t' || source[arm] == '\f'))
    ++arm;
  while (arm < source.size() && (std::isalnum((unsigned char)source[arm]) || source[arm] == '_' ||
                                 source[arm] == '$' || source[arm] == '#' || source[arm] == '@'))
    ++arm;
  while (arm < source.size() && (source[arm] == ' ' || source[arm] == '\t' || source[arm] == '\r' ||
                                 source[arm] == '\n' || source[arm] == '\f'))
    ++arm;
  if (arm >= source.size()) {
    error(path, directiveLine, directiveCol, "expected a directive or statement after %THEN");
    return npos;
  }
  // semPos covers %IF expr and %THEN keyword + trailing whitespace; the
  // arm itself is emitted by the recursive processDirective/emit below.
      semPos = arm > 0 ? arm - 1 : 0;
  size_t next;
  if (source[arm] == '%') {
    // Directive arm
    size_t armSemPos;
    next = processDirective(path, source, arm, output, line, col, active && cond != 0, armSemPos);
  } else {
    // Statement arm: find the terminating semicolon (not in string/comment)
    size_t stmtEnd = arm;
    bool inStr = false, inCmt = false, inLineCmt = false;
    while (stmtEnd < source.size()) {
      char c = source[stmtEnd];
      if (inLineCmt) {
        if (c == '\n')
          inLineCmt = false;
      } else if (inCmt) {
        if (c == '*' && stmtEnd + 1 < source.size() && source[stmtEnd + 1] == '/') {
          inCmt = false;
          stmtEnd += 2;
          continue;
        }
      } else if (inStr) {
        if (c == '\'' && stmtEnd + 1 < source.size() && source[stmtEnd + 1] == '\'') {
          stmtEnd += 2;
          continue;
        }
        if (c == '\'')
          inStr = false;
      } else {
        if (c == '/' && stmtEnd + 1 < source.size() && source[stmtEnd + 1] == '*') {
          inCmt = true;
          stmtEnd += 2;
          continue;
        }
        if (c == '/' && stmtEnd + 1 < source.size() && source[stmtEnd + 1] == '/') {
          inLineCmt = true;
          stmtEnd += 2;
          continue;
        }
        if (c == '\'') {
          inStr = true;
        } else if (c == ';') {
          ++stmtEnd; // include the semicolon
          break;
        }
      }
      ++stmtEnd;
    }
    if (active && cond != 0) {
      emit(source.substr(arm, stmtEnd - arm), output, path, line, col);
    }
    next = stmtEnd;
  }
  if (next == npos)
    return npos;
  // An optional %ELSE arm takes the inverted condition.
  size_t e = next;
  while (e < source.size() && (source[e] == ' ' || source[e] == '\t' || source[e] == '\r' ||
                                source[e] == '\n' || source[e] == '\f'))
    ++e;
  if (e < source.size() && source[e] == '%') {
    size_t ws = e + 1;
    while (ws < source.size() && (source[ws] == ' ' || source[ws] == '\t' || source[ws] == '\f'))
      ++ws;
    size_t we = ws;
    while (we < source.size() && (std::isalnum((unsigned char)source[we]) || source[we] == '_' ||
                                 source[we] == '$' || source[we] == '#' || source[we] == '@'))
      ++we;
    if (upper(source.substr(ws, we - ws)) == "ELSE") {
      size_t arm2 = we;
      while (arm2 < source.size() &&
             (source[arm2] == ' ' || source[arm2] == '\t' || source[arm2] == '\r' ||
              source[arm2] == '\n' || source[arm2] == '\f'))
        ++arm2;
      if (arm2 >= source.size()) {
        error(path, directiveLine, directiveCol, "expected a directive or statement after %ELSE");
        return npos;
      }
      if (source[arm2] == '%') {
        size_t arm2SemPos;
        next = processDirective(path, source, arm2, output, line, col, active && cond == 0,
                                arm2SemPos);
      } else {
        // Statement arm for %ELSE
        size_t stmtEnd = arm2;
        bool inStr = false, inCmt = false, inLineCmt = false;
        while (stmtEnd < source.size()) {
          char c = source[stmtEnd];
          if (inLineCmt) {
            if (c == '\n')
              inLineCmt = false;
          } else if (inCmt) {
            if (c == '*' && stmtEnd + 1 < source.size() && source[stmtEnd + 1] == '/') {
              inCmt = false;
              stmtEnd += 2;
              continue;
            }
          } else if (inStr) {
            if (c == '\'' && stmtEnd + 1 < source.size() && source[stmtEnd + 1] == '\'') {
              stmtEnd += 2;
              continue;
            }
            if (c == '\'')
              inStr = false;
          } else {
            if (c == '/' && stmtEnd + 1 < source.size() && source[stmtEnd + 1] == '*') {
              inCmt = true;
              stmtEnd += 2;
              continue;
            }
            if (c == '/' && stmtEnd + 1 < source.size() && source[stmtEnd + 1] == '/') {
              inLineCmt = true;
              stmtEnd += 2;
              continue;
            }
            if (c == '\'') {
              inStr = true;
            } else if (c == ';') {
              ++stmtEnd; // include the semicolon
              break;
            }
          }
          ++stmtEnd;
        }
        if (active && cond == 0) {
          emit(source.substr(arm2, stmtEnd - arm2), output, path, line, col);
        }
        next = stmtEnd;
      }
      if (next == npos)
        return npos;
    }
  }
  return next;
}

bool Preprocessor::expand(const fs::path& input, std::string& output) {
  fs::path path = normalized(input);
  if (std::find(active_.begin(), active_.end(), path) != active_.end())
    return error(input, 0, 0, "recursive %INCLUDE of '" + path.string() + "'");

  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    return error(input, 0, 0, "cannot open included source '" + path.string() + "'");
  std::stringstream buffer;
  buffer << stream.rdbuf();
  std::string source = buffer.str();
  active_.push_back(path);
  // Keep verbatim source per file so Diags can echo the real line (ADR-219).
  fileSrc_[path.string()] = source;

  int line = 1;
  int col = 1;
  // %GOTO resolves labels in the current file's flat text (ADR-305).
  // Save outer file's labels across recursive %INCLUDE expansion, then
  // repopulate for this file.
  auto savedLabels = labels_;
  labels_.clear();
  collectLabels(source, 0, source.size());
  gotoTarget_ = 0;
  bool result = scan(source, 0, source.size(), output, line, col, path, true);
  labels_ = savedLabels;

  active_.pop_back();
  return result;
}

// Append text to output, recording one lineMap entry per \n so diagnostics
// stay attributed to the correct source file:line (ADR-219, ADR-305).
void Preprocessor::emit(const std::string& text, std::string& output, const fs::path& path,
                        int& line, int& col) {
  output += text;
  for (char c : text) {
    if (c == '\n') {
      lineMap_.push_back({path.string(), line});
      ++line;
      col = 1;
    } else {
      ++col;
    }
  }
}

// Scan a source range char-by-char, emitting non-directive text via emit()
// and dispatching %-directives to processDirective (ADR-305). String, comment
// and // comment nesting is tracked so directives inside them are inert.
// Depth-guarded to prevent runaway %DO/%GOTO nesting.
bool Preprocessor::scan(const std::string& source, size_t start, size_t end, std::string& output,
                        int& line, int& col, const fs::path& path, bool active) {
  if (++scanDepth_ > kMaxScanDepth) {
    --scanDepth_;
    return error(path, line, col, "preprocessor control-flow nesting is too deep (C28-6571-3 Ch.9)");
  }
  bool inString = false;
  bool inComment = false;
  bool inLineComment = false;
  for (size_t i = start; i < end;) {
    char c = source[i];
    if (inLineComment) {
      emit(std::string(1, c), output, path, line, col);
      ++i;
      if (c == '\n')
        inLineComment = false;
    } else if (inComment) {
      emit(std::string(1, c), output, path, line, col);
      ++i;
      if (c == '*' && i < end && source[i] == '/') {
        emit(std::string(1, '/'), output, path, line, col);
        ++i;
        inComment = false;
      }
    } else if (inString) {
      emit(std::string(1, c), output, path, line, col);
      ++i;
      if (c == '\'' && i < end && source[i] == '\'') {
        emit(std::string(1, '\''), output, path, line, col);
        ++i;
      } else if (c == '\'') {
        inString = false;
      }
    } else if (c == '/' && i + 1 < end && source[i + 1] == '*') {
      emit("/*", output, path, line, col);
      i += 2;
      inComment = true;
    } else if (c == '/' && i + 1 < end && source[i + 1] == '/') {
      emit("//", output, path, line, col);
      i += 2;
      inLineComment = true;
    } else if (c == '\'') {
      emit(std::string(1, c), output, path, line, col);
      ++i;
      inString = true;
    } else if (c == '%') {
      size_t semPos = i;
      size_t next = processDirective(path, source, i, output, line, col, active, semPos);
      if (next == std::string::npos) {
        --scanDepth_;
        return false;
      }
      // Preserve newlines from the directive's own text (i..semPos), not
      // from the jump target, so %DO body/%END and %GOTO target text is
      // tracked by their own scan/processDirective calls (ADR-305).
      for (size_t j = i; j <= semPos; ++j) {
        if (source[j] == '\n') {
          output += '\n';
          lineMap_.push_back({path.string(), line});
          ++line;
          col = 1;
        } else {
          ++col;
        }
      }
      i = next;
      // Boundary guard: %GOTO may land outside [start, end).
      if (i < start || i > end)
        break;
    } else {
      emit(std::string(1, c), output, path, line, col);
      ++i;
    }
  }
  --scanDepth_;
  return true;
}

// Pre-scan source[start..end) for %label: definitions (ADR-305).
// Records each label's target position (right after ':' + whitespace).
void Preprocessor::collectLabels(const std::string& source, size_t start, size_t end) {
  bool inString = false;
  bool inComment = false;
  bool inLineComment = false;
  for (size_t i = start; i < end;) {
    if (inLineComment) {
      if (source[i] == '\n')
        inLineComment = false;
      ++i;
    } else if (inComment) {
      if (source[i] == '*' && i + 1 < end && source[i + 1] == '/') {
        i += 2;
        inComment = false;
      } else {
        ++i;
      }
    } else if (inString) {
      if (source[i] == '\'' && i + 1 < end && source[i + 1] == '\'') {
        i += 2;
      } else if (source[i] == '\'') {
        inString = false;
        ++i;
      } else {
        ++i;
      }
    } else if (source[i] == '/' && i + 1 < end && source[i + 1] == '*') {
      i += 2;
      inComment = true;
    } else if (source[i] == '/' && i + 1 < end && source[i + 1] == '/') {
      while (i < end && source[i] != '\n')
        ++i;
    } else if (source[i] == '\'') {
      ++i;
      inString = true;
    } else if (source[i] == '%') {
      // Skip the directive name.
      size_t ws = i + 1;
      while (ws < end && (source[ws] == ' ' || source[ws] == '\t' || source[ws] == '\f'))
        ++ws;
      size_t we = ws;
      while (we < end && (std::isalnum((unsigned char)source[we]) || source[we] == '_' ||
                          source[we] == '$' || source[we] == '#' || source[we] == '@'))
        ++we;
      // If followed by ':', it's a %label: definition.
      size_t chk = we;
      while (chk < end && (source[chk] == ' ' || source[chk] == '\t' || source[chk] == '\f'))
        ++chk;
      if (chk < end && source[chk] == ':') {
        std::string labelName = upper(source.substr(ws, we - ws));
        size_t target = chk + 1;
        while (target < end && (source[target] == ' ' || source[target] == '\t' ||
                                source[target] == '\f' || source[target] == '\n' ||
                                source[target] == '\r'))
          ++target;
        labels_[labelName] = target;
        i = target;
      } else {
        // Not a label: skip to end of directive (semicolon or newline).
        i = (we < end) ? we : end;
      }
    } else {
      ++i;
    }
  }
}

// Find the ';' (outside quoted strings) that terminates a directive body
// starting just after `start` (ADR-305).
size_t Preprocessor::findDirectiveEnd(const std::string& source, size_t start) {
  bool quoted = false;
  for (size_t i = start; i < source.size(); ++i) {
    if (source[i] == '\'' && quoted && i + 1 < source.size() && source[i + 1] == '\'') {
      ++i; // skip escaped quote
      continue;
    }
    if (source[i] == '\'')
      quoted = !quoted;
    else if (source[i] == ';' && !quoted)
      return i;
  }
  return std::string::npos;
}

// Find the matching %END for a %DO starting at `start` (ADR-305).
// Returns the %END position in endPos, its ';' in endSemi.
size_t Preprocessor::findMatchingEnd(const fs::path& path, const std::string& source,
                                     size_t start, size_t& endSemi, int line, int col) {
  static const size_t npos = std::string::npos;
  endSemi = npos;
  int depth = 1;
  bool inString = false;
  bool inComment = false;
  bool inLineComment = false;
  for (size_t i = start; i < source.size();) {
    if (inLineComment) {
      if (source[i] == '\n')
        inLineComment = false;
      ++i;
    } else if (inComment) {
      if (source[i] == '*' && i + 1 < source.size() && source[i + 1] == '/') {
        i += 2;
        inComment = false;
      } else {
        ++i;
      }
    } else if (inString) {
      if (source[i] == '\'' && i + 1 < source.size() && source[i + 1] == '\'') {
        i += 2;
      } else if (source[i] == '\'') {
        inString = false;
        ++i;
      } else {
        ++i;
      }
    } else if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '*') {
      i += 2;
      inComment = true;
    } else if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '/') {
      while (i < source.size() && source[i] != '\n')
        ++i;
    } else if (source[i] == '\'') {
      ++i;
      inString = true;
    } else if (source[i] == '%') {
      // Parse the directive name.
      size_t ws = i + 1;
      while (ws < source.size() && (source[ws] == ' ' || source[ws] == '\t' || source[ws] == '\f'))
        ++ws;
      size_t we = ws;
      while (we < source.size() && (std::isalnum((unsigned char)source[we]) || source[we] == '_' ||
                                     source[we] == '$' || source[we] == '#' || source[we] == '@'))
        ++we;
      std::string directiveName = upper(source.substr(ws, we - ws));
      if (directiveName == "DO") {
        ++depth;
        // Skip to next ';' or end.
        size_t semi = findDirectiveEnd(source, we);
        if (semi == npos) {
          error(path, line, col, "unterminated %DO (C28-6571-3 Ch.9)");
          return npos;
        }
        i = semi + 1;
      } else if (directiveName == "END") {
        if (--depth == 0) {
          endSemi = findDirectiveEnd(source, we);
          if (endSemi == npos) {
            error(path, line, col, "unterminated %END (C28-6571-3 Ch.9)");
            return npos;
          }
          return i;
        }
        size_t semi = findDirectiveEnd(source, we);
        if (semi == npos) {
          error(path, line, col, "unterminated %END (C28-6571-3 Ch.9)");
          return npos;
        }
        i = semi + 1;
      } else {
        // Other directive: skip to next ';'.
        size_t semi = findDirectiveEnd(source, we);
        if (semi == npos) {
          error(path, line, col, "unterminated preprocessor directive");
          return npos;
        }
        i = semi + 1;
      }
    } else {
      ++i;
    }
  }
  error(path, line, col, "unterminated %DO (C28-6571-3 Ch.9)");
  return npos;
}

bool Preprocessor::handleInclude(const fs::path& input, const std::string& operand, int line,
                                 int col, std::string& output) {
  std::string name = trim(operand);
  if (name.empty())
    return error(input, line, col, "%INCLUDE requires one file or member name");
  return expandOneInclude(input, name, line, col, output, false);
}

// IBM Enterprise %XINCLUDE (ADR-157): like %INCLUDE, but each file is
// expanded only the first time it is requested. The operand is a
// comma-separated list; each item is a quoted string, a bare member name,
// or a parenthesized name. Items resolve with the same search order as
// %INCLUDE (including file's directory, -I, PLI_INCLUDE_PATH, default).
bool Preprocessor::handleXInclude(const fs::path& input, const std::string& operand, int line,
                                  int col, std::string& output) {
  // Split on commas outside quotes and parens ('' is an escaped quote).
  std::vector<std::string> items;
  std::string cur;
  bool quoted = false;
  int depth = 0;
  for (size_t k = 0; k < operand.size(); ++k) {
    char c = operand[k];
    if (c == '\'') {
      if (quoted && k + 1 < operand.size() && operand[k + 1] == '\'') {
        cur += "''";
        ++k;
        continue;
      }
      quoted = !quoted;
      cur += c;
      continue;
    }
    if (!quoted) {
      if (c == '(')
        ++depth;
      else if (c == ')' && depth > 0)
        --depth;
      else if (c == ',' && depth == 0) {
        items.push_back(cur);
        cur.clear();
        continue;
      }
    }
    cur += c;
  }
  items.push_back(cur);
  bool any = false;
  for (const std::string& item : items) {
    if (trim(item).empty())
      return error(input, line, col, "%XINCLUDE requires file or member names");
    any = true;
  }
  if (!any)
    return error(input, line, col, "%XINCLUDE requires file or member names");
  for (const std::string& item : items)
    if (!expandOneInclude(input, item, line, col, output, true))
      return false;
  return true;
}

bool Preprocessor::expandOneInclude(const fs::path& input, const std::string& rawName, int line,
                                    int col, std::string& output, bool once) {
  std::string name = trim(rawName);
  // IBM parenthesized form: (MEMBER) names the same member as MEMBER.
  if (name.size() >= 2 && name.front() == '(' && name.back() == ')')
    name = trim(name.substr(1, name.size() - 2));
  if (name.size() >= 2 && name.front() == '\'' && name.back() == '\'')
    name = name.substr(1, name.size() - 2);
  if (name.empty() || name.find_first_of(" \t\r\n,()") != std::string::npos)
    return error(input, line, col,
                 once ? "%XINCLUDE requires file or member names"
                      : "%INCLUDE requires one file or member name");

  fs::path include = input.parent_path() / name;
  if (!fs::exists(include) && include.extension().empty())
    include += ".inc";
  if (!fs::exists(include)) {
    // Search the configured directories in order; the first hit wins
    // (ADR-078). An absolute name resolves on its own in every candidate.
    for (const fs::path& dir : includeDirs_) {
      fs::path cand = dir / name;
      if (!fs::exists(cand) && cand.extension().empty())
        cand += ".inc";
      if (fs::exists(cand)) {
        include = cand;
        break;
      }
    }
  }
  if (fs::exists(include)) {
    fs::path canon = normalized(include);
    if (once) {
      // Include-once: skip files already pulled in or currently being
      // expanded (the latter also makes self-referential %XINCLUDE
      // cycle-safe, unlike %INCLUDE which diagnoses cycles).
      if (included_.count(canon) ||
          std::find(active_.begin(), active_.end(), canon) != active_.end())
        return true;
    }
    included_.insert(canon);
  }
  return expand(include, output);
}

bool Preprocessor::handleDeclare(const fs::path& input, const std::string& operand, int line,
                                 int col) {
  // %DECLARE a, b (ADR-083): integer preprocessor variables, default 0.
  // Also supports %DECLARE name VALUE(expr) for initialization (ADR-083).
  // Redeclaration keeps the current value (include-twice idempotent).
  std::stringstream ss(operand);
  std::string item;
  bool any = false;
  while (std::getline(ss, item, ',')) {
    std::string name = upper(trim(item));
    // Check for VALUE clause: name VALUE(expr)
    long long initValue = 0;
    bool hasValue = false;
    // The VALUE keyword must be a whole word: a name that merely contains
    // it ("values") is not a VALUE clause.
    size_t valuePos = std::string::npos;
    for (size_t i = 0; i + 5 <= name.size(); ++i) {
      if (name.compare(i, 5, "VALUE") != 0)
        continue;
      bool leftOk = i == 0 || name[i - 1] == ' ' || name[i - 1] == '\t';
      bool rightOk = i + 5 == name.size() || name[i + 5] == ' ' || name[i + 5] == '\t' ||
                     name[i + 5] == '(';
      if (leftOk && rightOk) {
        valuePos = i;
        break;
      }
    }
    if (valuePos != std::string::npos) {
      // Extract name and VALUE expression
      std::string beforeValue = trim(name.substr(0, valuePos));
      std::string afterValue = trim(name.substr(valuePos + 5)); // skip "VALUE"
      if (!beforeValue.empty()) {
        name = upper(beforeValue);
      } else {
        error(input, line, col, "expected a name before VALUE in %DECLARE");
        return false;
      }
      if (!afterValue.empty() && afterValue[0] == '(') {
        // Find matching closing parenthesis
        int depth = 0;
        size_t endPos = std::string::npos;
        for (size_t i = 0; i < afterValue.size(); ++i) {
          if (afterValue[i] == '(')
            ++depth;
          else if (afterValue[i] == ')') {
            if (--depth == 0) {
              endPos = i;
              break;
            }
          }
        }
        if (endPos != std::string::npos) {
          std::string valueExpr = afterValue.substr(1, endPos - 1);
          if (!evalCondExpr(valueExpr, input, line, col, initValue))
            return false;
          hasValue = true;
        } else {
          error(input, line, col, "unterminated VALUE expression in %DECLARE");
          return false;
        }
      } else {
        error(input, line, col, "expected '(' after VALUE in %DECLARE");
        return false;
      }
    }
    if (name.empty()) {
      error(input, line, col, "expected a name in %DECLARE");
      return false;
    }
    if (name == "CHARACTER") {
      error(input, line, col, "CHARACTER preprocessor variables are not implemented");
      return false;
    }
    for (char ch : name)
      if (!std::isalnum((unsigned char)ch) && ch != '_' && ch != '$' && ch != '#' && ch != '@') {
        error(input, line, col, "expected a name in %DECLARE");
        return false;
      }
    any = true;
    if (!ppVars_.count(name))
      ppVars_[name] = hasValue ? initValue : 0;
  }
  if (!any) {
    error(input, line, col, "expected a name in %DECLARE");
    return false;
  }
  return true;
}

bool Preprocessor::handleAssignment(const fs::path& input, const std::string& name,
                                    const std::string& operand, int line, int col) {
  // %X = expr (ADR-083): assign a declared preprocessor variable.
  std::string rest = trim(operand);
  if (rest.empty() || rest[0] != '=') {
    error(input, line, col, "expected = after %" + name + " (only assignment is implemented)");
    return false;
  }
  auto it = ppVars_.find(name);
  if (it == ppVars_.end()) {
    error(input, line, col, "%" + name + " is not a declared preprocessor variable");
    return false;
  }
  long long value = 0;
  if (!evalCondExpr(rest.substr(1), input, line, col, value))
    return false;
  it->second = value;
  return true;
}

bool Preprocessor::handleActivation(const fs::path& input, const std::string& name,
                                    const std::string& operand, int line, int col) {
  // %ACTIVATE a, b / %DEACTIVATE a, b (extension, ADR-113): gate reference
  // substitution; idempotent like %DECLARE redeclaration.
  std::stringstream ss(operand);
  std::string item;
  bool any = false;
  while (std::getline(ss, item, ',')) {
    std::string var = upper(trim(item));
    if (var.empty()) {
      error(input, line, col, "expected a name in %" + name);
      return false;
    }
    if (!ppVars_.count(var)) {
      error(input, line, col, "%" + var + " is not a declared preprocessor variable");
      return false;
    }
    any = true;
    if (name == "ACTIVATE")
      ppActive_.insert(var);
    else
      ppActive_.erase(var);
  }
  if (!any) {
    error(input, line, col, "expected a name in %" + name);
    return false;
  }
  return true;
}

bool Preprocessor::substituteRef(const fs::path& input, const std::string& name, int line, int col,
                                 std::string& output) {
  auto it = ppVars_.find(name);
  if (it == ppVars_.end())
    return error(input, line, col, "%" + name + " is not a declared preprocessor variable");
  if (!ppActive_.count(name))
    return error(input, line, col, "%" + name + " is not an active preprocessor variable");
  output += std::to_string(it->second);
  return true;
}

// %DO [v=e1 TO e2 [BY e3]] ; — repeat the body up to %END (ADR-305).
// The body is scan()'d once per iteration; the iteration variable is set
// in ppVars_ before each scan. Returns endSemi+1 (past %END;) on success.
size_t Preprocessor::handleDo(const fs::path& path, const std::string& source, size_t bodyStart,
                              size_t endPos, size_t endSemi, std::string& output, int& line,
                              int& col, bool active, const std::string& spec, int directiveLine,
                              int directiveCol) {
  static const size_t npos = std::string::npos;
  std::string trimmed = trim(spec);
  long long e1 = 0, e2 = 0, e3 = 1;
  std::string varName;
  bool iterate = false;

  if (!trimmed.empty()) {
    // Parse: var = start TO end [BY step]
    std::string up = upper(trimmed);
    size_t eq = up.find('=');
    if (eq == std::string::npos) {
      error(path, directiveLine, directiveCol,
            "expected 'var=start TO end [BY step]' in %DO, or bare %DO with no iteration");
      return npos;
    }
    varName = upper(trim(up.substr(0, eq)));
    std::string rest = trim(up.substr(eq + 1));
    // Find TO as a whole word (not part of another identifier).
    size_t toPos = std::string::npos;
    for (size_t pos = 0; pos + 2 <= rest.size(); ++pos) {
      if (rest.compare(pos, 2, "TO") != 0)
        continue;
      bool leftOk = pos == 0 || !isWordChar(rest[pos - 1]);
      bool rightOk = pos + 2 >= rest.size() || !isWordChar(rest[pos + 2]);
      if (leftOk && rightOk) {
        toPos = pos;
        break;
      }
    }
    if (toPos == std::string::npos) {
      error(path, directiveLine, directiveCol, "expected TO in %DO iteration spec");
      return npos;
    }
    std::string startExpr = rest.substr(0, toPos);
    std::string afterTo = trim(rest.substr(toPos + 2));
    // Optional BY (also a whole word).
    std::string endExpr = afterTo;
    std::string stepExpr = "1";
    size_t byPos = std::string::npos;
    for (size_t pos = 0; pos + 2 <= afterTo.size(); ++pos) {
      if (afterTo.compare(pos, 2, "BY") != 0)
        continue;
      bool leftOk = pos == 0 || !isWordChar(afterTo[pos - 1]);
      bool rightOk = pos + 2 >= afterTo.size() || !isWordChar(afterTo[pos + 2]);
      if (leftOk && rightOk) {
        byPos = pos;
        break;
      }
    }
    if (byPos != std::string::npos) {
      endExpr = trim(afterTo.substr(0, byPos));
      stepExpr = trim(afterTo.substr(byPos + 2));
    }
    if (!evalCondExpr(startExpr, path, directiveLine, directiveCol, e1) ||
        !evalCondExpr(endExpr, path, directiveLine, directiveCol, e2) ||
        !evalCondExpr(stepExpr, path, directiveLine, directiveCol, e3))
      return npos;
    if (e3 == 0) {
      error(path, directiveLine, directiveCol, "BY step is zero in %DO loop");
      return npos;
    }
    iterate = true;
  }

  if (!iterate) {
    // No iteration: scan body once.
    if (!scan(source, bodyStart, endPos, output, line, col, path, active))
      return npos;
  } else {
    // Iterative: scan body once per iteration.
    for (long long iter = e1; (e3 > 0) ? (iter <= e2) : (iter >= e2); iter += e3) {
      ppVars_[varName] = iter;
      std::string iterOutput;
      int iterLine = line;
      int iterCol = col;
      if (!scan(source, bodyStart, endPos, iterOutput, iterLine, iterCol, path, active))
        return npos;
      emit(iterOutput, output, path, line, col);
      line = iterLine;
      col = iterCol;
    }
  }

  return endSemi + 1;
}

// %label: / %GOTO label / %GO TO label — resolve to a flat position jump (ADR-305).
bool Preprocessor::handleGoto(const fs::path& path, const std::string& name,
                              const std::string& operand, int line, int col) {
  std::string label = upper(trim(operand));
  if (label.empty()) {
    return error(path, line, col, "%" + name + " requires a label name");
  }
  auto it = labels_.find(label);
  if (it == labels_.end()) {
    return error(path, line, col, "undefined label in %" + name + ": '" + trim(operand) + "'");
  }
  gotoTarget_ = it->second;
  return true;
}

// %PROCEDURE / %PROC / %RETURN — compile-time macros (Stage 2).
// Placeholder: not yet implemented, returns the position past the directive.
size_t Preprocessor::handleProcedure(const fs::path& path, const std::string& source, size_t,
                                     size_t wordEnd, size_t end, std::string&, int& line,
                                     int& col, bool) {
  std::string name = upper(source.substr(wordEnd, end - wordEnd));
  error(path, line, col, "%" + name + " is not implemented");
  return std::string::npos;
}

// %REPLACE — Stage 3: textual rescanning in the preprocessor.
bool Preprocessor::handleReplace(const fs::path& path, const std::string& source, size_t i,
                                 size_t wordEnd, size_t end, std::string& output, int& line,
                                 int& col, bool active) {
  // Currently served downstream by the lexer's token substitution (ADR-077).
  // Not called in Stage 1/2; reserved for Stage 3 migration.
  return true;
}

bool Preprocessor::unsupported(const fs::path& input, const std::string& name, int line, int col) {
  return error(input, line, col, "%" + name + " is not implemented");
}

bool Preprocessor::error(const fs::path& input, int line, int col, const std::string& message) {
  std::cerr << input.string();
  if (line > 0)
    std::cerr << ':' << line << ':' << col;
  std::cerr << ": error: " << message << "  [C28-6571-3 Chapter 9]\n";
  return false;
}
