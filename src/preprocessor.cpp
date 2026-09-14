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

fs::path normalized(const fs::path& path) {
  std::error_code ec;
  fs::path result = fs::weakly_canonical(path, ec);
  return ec ? fs::absolute(path).lexically_normal() : result;
}

} // namespace

bool Preprocessor::run(const fs::path& input, std::string& output) {
  active_.clear();
  ppVars_.clear();
  output.clear();
  return expand(input, output);
}

void Preprocessor::addIncludeDir(const fs::path& dir) {
  includeDirs_.push_back(dir);
}

namespace {

// Integer expressions for %IF and %assignment (ADR-083): literals,
// %DECLARE variables, parens, +-*/ (unary minus), comparisons, and & | ¬
// (any of ¬ ^ ~ spellings). Comparisons yield 0/1; nonzero is true.
struct PPCondParser {
  const std::string& text;
  size_t pos = 0;
  const std::map<std::string, long long>& vars;
  std::string error;

  void skipWs() {
    while (pos < text.size() &&
           (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r' || text[pos] == '\n' ||
            text[pos] == '\f'))
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
    if (pos < text.size() &&
        (std::isalpha((unsigned char)text[pos]) || text[pos] == '_' || text[pos] == '$' ||
         text[pos] == '#' || text[pos] == '@')) {
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
// past it, or npos after diagnosing. With active == false the directive is
// skipped without effects; newlines stay caller-preserved.
size_t Preprocessor::processDirective(const fs::path& path, const std::string& source, size_t i,
                                      std::string& output, int& line, int& col, bool active) {
  static const size_t npos = std::string::npos;
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
  if (name == "IF")
    return processIf(path, source, i, wordEnd, output, line, col, active);
  if (name == "THEN" || name == "ELSE") {
    error(path, directiveLine, directiveCol, "%" + name + " without %IF");
    return npos;
  }

  bool quoted = false;
  size_t end = wordEnd;
  for (; end < source.size(); ++end) {
    if (source[end] == '\'') {
      if (quoted && end + 1 < source.size() && source[end + 1] == '\'') {
        ++end;
        continue;
      }
      quoted = !quoted;
    } else if (source[end] == ';' && !quoted) {
      break;
    }
  }
  if (end == source.size()) {
    error(path, directiveLine, directiveCol, "unterminated preprocessor directive");
    return npos;
  }

  bool ok = false;
  if (name == "INCLUDE") {
    if (active)
      ok = handleInclude(path, source.substr(wordEnd, end - wordEnd), directiveLine,
                         directiveCol, output);
    else
      ok = true;
  } else if (name == "DECLARE" || name == "DCL") {
    if (active)
      ok = handleDeclare(path, source.substr(wordEnd, end - wordEnd), directiveLine,
                         directiveCol);
    else
      ok = true;
  } else if (name == "ACTIVATE" || name == "DEACTIVATE") {
    if (active)
      ok = handleActivation(path, name, directiveLine, directiveCol);
    else
      ok = true;
  } else if (name == "DO" || name == "END") {
    if (active)
      ok = handleDo(path, name, directiveLine, directiveCol);
    else
      ok = true;
  } else if (name == "GO" || name == "GOTO") {
    if (active)
      ok = handleGoto(path, name, directiveLine, directiveCol);
    else
      ok = true;
  } else if (name == "PROCEDURE" || name == "PROC" || name == "RETURN") {
    if (active)
      ok = handleProcedure(path, name, directiveLine, directiveCol);
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
// each arm is exactly one directive.
size_t Preprocessor::processIf(const fs::path& path, const std::string& source, size_t i,
                               size_t wordEnd, std::string& output, int& line, int& col,
                               bool active) {
  static const size_t npos = std::string::npos;
  int directiveLine = line;
  int directiveCol = col;
  // Find %THEN: the first %word in the expression span (expressions hold
  // no directives). Comments cannot hide one: the main loop strips them
  // before directives are ever scanned... except inside this raw span, so
  // skip /* */ here too.
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
    if (source[t] == '%') {
      size_t ws = t + 1;
      while (ws < source.size() &&
             (source[ws] == ' ' || source[ws] == '\t' || source[ws] == '\f'))
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
  // One directive per arm, at the caller's activity gated by the condition.
  // thenAt points at '%': re-scan to the end of the THEN word itself, since
  // spaces may sit between them.
  size_t arm = thenAt + 1;
  while (arm < source.size() &&
         (source[arm] == ' ' || source[arm] == '\t' || source[arm] == '\f'))
    ++arm;
  while (arm < source.size() && (std::isalnum((unsigned char)source[arm]) || source[arm] == '_' ||
                                 source[arm] == '$' || source[arm] == '#' || source[arm] == '@'))
    ++arm;
  while (arm < source.size() &&
         (source[arm] == ' ' || source[arm] == '\t' || source[arm] == '\r' ||
          source[arm] == '\n' || source[arm] == '\f'))
    ++arm;
  if (arm >= source.size() || source[arm] != '%') {
    error(path, directiveLine, directiveCol, "expected a directive after %THEN");
    return npos;
  }
  size_t next = processDirective(path, source, arm, output, line, col, active && cond != 0);
  if (next == npos)
    return npos;
  // An optional %ELSE arm takes the inverted condition.
  size_t e = next;
  while (e < source.size() &&
         (source[e] == ' ' || source[e] == '\t' || source[e] == '\r' || source[e] == '\n' ||
          source[e] == '\f'))
    ++e;
  if (e < source.size() && source[e] == '%') {
    size_t ws = e + 1;
    while (ws < source.size() &&
           (source[ws] == ' ' || source[ws] == '\t' || source[ws] == '\f'))
      ++ws;
    size_t we = ws;
    while (we < source.size() && (std::isalnum((unsigned char)source[we]) || source[we] == '_' ||
                                  source[we] == '$' || source[we] == '#' || source[we] == '@'))
      ++we;
    if (upper(source.substr(ws, we - ws)) == "ELSE") {
      size_t arm2 = we;
      while (arm2 < source.size() && (source[arm2] == ' ' || source[arm2] == '\t' ||
                                      source[arm2] == '\r' || source[arm2] == '\n' ||
                                      source[arm2] == '\f'))
        ++arm2;
      if (arm2 >= source.size() || source[arm2] != '%') {
        error(path, directiveLine, directiveCol, "expected a directive after %ELSE");
        return npos;
      }
      next = processDirective(path, source, arm2, output, line, col, active && cond == 0);
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

  int line = 1;
  int col = 1;
  bool inString = false;
  bool inComment = false;
  for (size_t i = 0; i < source.size();) {
    char c = source[i];
    if (inComment) {
      output += c;
      if (c == '*' && i + 1 < source.size() && source[i + 1] == '/') {
        output += '/';
        i += 2;
        col += 2;
        inComment = false;
        continue;
      }
    } else if (inString) {
      output += c;
      if (c == '\'' && i + 1 < source.size() && source[i + 1] == '\'') {
        output += '\'';
        i += 2;
        col += 2;
        continue;
      }
      if (c == '\'')
        inString = false;
    } else if (c == '/' && i + 1 < source.size() && source[i + 1] == '*') {
      output += "/*";
      i += 2;
      col += 2;
      inComment = true;
      continue;
    } else if (c == '\'') {
      output += c;
      inString = true;
    } else if (c == '%') {
      size_t start = i;
      size_t next = processDirective(path, source, i, output, line, col, true);
      if (next == std::string::npos) {
        active_.pop_back();
        return false;
      }
      // Preserve directive newlines so following root-source diagnostics remain useful.
      for (size_t j = start; j < next; ++j) {
        if (source[j] == '\n')
          output += '\n';
      }
      while (i < next) {
        if (source[i] == '\n') {
          ++line;
          col = 1;
        } else {
          ++col;
        }
        ++i;
      }
      continue;
    } else {
      output += c;
    }

    ++i;
    if (c == '\n') {
      ++line;
      col = 1;
    } else {
      ++col;
    }
  }

  active_.pop_back();
  return true;
}

bool Preprocessor::handleInclude(const fs::path& input, const std::string& operand, int line,
                                 int col, std::string& output) {
  std::string name = trim(operand);
  if (name.size() >= 2 && name.front() == '\'' && name.back() == '\'')
    name = name.substr(1, name.size() - 2);
  if (name.empty() || name.find_first_of(" \t\r\n,()") != std::string::npos)
    return error(input, line, col, "%INCLUDE requires one file or member name");

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
  return expand(include, output);
}

bool Preprocessor::handleDeclare(const fs::path& input, const std::string& operand, int line,
                                 int col) {
  // %DECLARE a, b (ADR-083): integer preprocessor variables, default 0.
  // Redeclaration keeps the current value (include-twice idempotent).
  std::stringstream ss(operand);
  std::string item;
  bool any = false;
  while (std::getline(ss, item, ',')) {
    std::string name = upper(trim(item));
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
      ppVars_[name] = 0;
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

bool Preprocessor::handleActivation(const fs::path& input, const std::string& name, int line,
                                    int col) {
  return unsupported(input, name, line, col);
}

bool Preprocessor::handleConditional(const fs::path& input, const std::string& name, int line,
                                     int col) {
  return unsupported(input, name, line, col);
}

bool Preprocessor::handleDo(const fs::path& input, const std::string& name, int line, int col) {
  return unsupported(input, name, line, col);
}

bool Preprocessor::handleGoto(const fs::path& input, const std::string& name, int line, int col) {
  return unsupported(input, name, line, col);
}

bool Preprocessor::handleProcedure(const fs::path& input, const std::string& name, int line,
                                   int col) {
  return unsupported(input, name, line, col);
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
