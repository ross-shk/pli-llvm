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
  output.clear();
  return expand(input, output);
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
        active_.pop_back();
        return error(path, directiveLine, directiveCol, "expected a preprocessor directive");
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
        active_.pop_back();
        return error(path, directiveLine, directiveCol, "unterminated preprocessor directive");
      }

      std::string name = upper(source.substr(wordStart, wordEnd - wordStart));
      bool ok = false;
      if (name == "INCLUDE") {
        ok = handleInclude(path, source.substr(wordEnd, end - wordEnd), directiveLine, directiveCol,
                           output);
      } else if (name == "DECLARE" || name == "DCL") {
        ok = handleDeclare(path, directiveLine, directiveCol);
      } else if (name == "ACTIVATE" || name == "DEACTIVATE") {
        ok = handleActivation(path, name, directiveLine, directiveCol);
      } else if (name == "IF" || name == "THEN" || name == "ELSE") {
        ok = handleConditional(path, name, directiveLine, directiveCol);
      } else if (name == "DO" || name == "END") {
        ok = handleDo(path, name, directiveLine, directiveCol);
      } else if (name == "GO" || name == "GOTO") {
        ok = handleGoto(path, name, directiveLine, directiveCol);
      } else if (name == "PROCEDURE" || name == "PROC" || name == "RETURN") {
        ok = handleProcedure(path, name, directiveLine, directiveCol);
      } else if (name == "REPLACE") {
        // Served downstream by the lexer's token substitution (ADR-077):
        // preserve the directive text verbatim, minus newlines (the generic
        // tail below re-adds those, keeping output line numbers aligned).
        for (size_t j = i; j <= end; ++j)
          if (source[j] != '\n')
            output += source[j];
        ok = true;
      } else {
        ok = handleAssignment(path, name, directiveLine, directiveCol);
      }
      if (!ok) {
        active_.pop_back();
        return false;
      }

      // Preserve directive newlines so following root-source diagnostics remain useful.
      for (size_t j = i; j <= end; ++j) {
        if (source[j] == '\n')
          output += '\n';
      }
      while (i <= end) {
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
  return expand(include, output);
}

bool Preprocessor::handleDeclare(const fs::path& input, int line, int col) {
  return unsupported(input, "DECLARE", line, col);
}

bool Preprocessor::handleAssignment(const fs::path& input, const std::string& name, int line,
                                    int col) {
  return unsupported(input, name, line, col);
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
