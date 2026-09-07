#include "diag.h"
#include <cstdio>
#include <sstream>

void Diags::emit(const char *level, SourceLoc loc, const std::string &msg,
                 const std::string &rule) {
  std::string suffix;
  if (!rule.empty()) suffix = "  [TR 25.084 rule " + rule + "]";
  if (loc.line > 0)
    fprintf(stderr, "%s:%d:%d: %s: %s%s\n", file_.c_str(), loc.line, loc.col,
            level, msg.c_str(), suffix.c_str());
  else
    fprintf(stderr, "%s: %s: %s%s\n", file_.c_str(), level, msg.c_str(),
            suffix.c_str());

  // Echo the source line plus a caret, clang-style.
  if (src_ && loc.line > 0) {
    std::istringstream is(*src_);
    std::string line;
    for (int i = 0; i < loc.line && std::getline(is, line); ++i) {
    }
    if (!line.empty() || loc.line > 0) {
      fprintf(stderr, "  %s\n", line.c_str());
      int c = loc.col > 0 ? loc.col - 1 : 0;
      std::string caret(c, ' ');
      caret += '^';
      fprintf(stderr, "  %s\n", caret.c_str());
    }
  }
}

void Diags::error(SourceLoc loc, const std::string &msg, const std::string &rule) {
  ++nerr_;
  emit("error", loc, msg, rule);
}

void Diags::warn(SourceLoc loc, const std::string &msg, const std::string &rule) {
  ++nwarn_;
  emit("warning", loc, msg, rule);
}

void Diags::note(SourceLoc loc, const std::string &msg) {
  emit("note", loc, msg, "");
}
