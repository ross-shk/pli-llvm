#include "diag.h"
#include <cstdio>
#include <sstream>

void Diags::emit(const char *level, SourceLoc loc, const std::string &msg,
                 const std::string &rule, const std::string &replace) {
  std::string suffix;
  if (!rule.empty()) suffix = "  [TR 25.084 rule " + rule + "]";
  if (loc.line > 0)
    fprintf(stderr, "%s:%d:%d: %s: %s%s\n", file_.c_str(), loc.line, loc.col,
            level, msg.c_str(), suffix.c_str());
  else
    fprintf(stderr, "%s: %s: %s%s\n", file_.c_str(), level, msg.c_str(),
            suffix.c_str());

  // Echo the source line plus a caret, clang-style; rendered ASCII-safe so
  // the UTF-8 not sign reads as '^' on any terminal and the caret aligns.
  if (src_ && loc.line > 0) {
    std::istringstream is(*src_);
    std::string line;
    for (int i = 0; i < loc.line && std::getline(is, line); ++i) {
    }
    if (!line.empty() || loc.line > 0) {
      int target = loc.col > 0 ? loc.col - 1 : 0;
      int d = 0;
      for (int j = 0; j < target && j < (int)line.size(); ++j) {
        unsigned char b = (unsigned char)line[j];
        if (b == 0xC2 && j + 1 < (int)line.size() &&
            (unsigned char)line[j + 1] == 0xAC)
          ++j;  // two bytes, one display column
        ++d;
      }
      std::string show, caret(d, ' ');
      caret += '^';
      for (size_t j = 0; j < line.size(); ++j) {
        unsigned char b = (unsigned char)line[j];
        if (b == 0xC2 && j + 1 < line.size() &&
            (unsigned char)line[j + 1] == 0xAC) {
          show += '^';
          ++j;
        } else if (b >= 0x80) {
          show += '?';
        } else {
          show += (char)b;
        }
      }
      fprintf(stderr, "  %s\n", show.c_str());
      fprintf(stderr, "  %s\n", caret.c_str());

      // A fix-it is an insertion of `replace` at this location; render the
      // suggested text at the caret column so it is visible where it goes.
      if (!replace.empty()) {
        std::string pad(d, ' ');
        fprintf(stderr, "  %s%s\n", pad.c_str(), replace.c_str());
      }
    }
  }
}

void Diags::error(SourceLoc loc, const std::string &msg, const std::string &rule,
                  const std::string &replace) {
  if (muted()) { ++mnerr_; return; }
  ++nerr_;
  emit("error", loc, msg, rule, replace);
}

void Diags::warn(SourceLoc loc, const std::string &msg, const std::string &rule,
                 const std::string &replace) {
  if (muted()) return;
  ++nwarn_;
  emit("warning", loc, msg, rule, replace);
}

void Diags::note(SourceLoc loc, const std::string &msg) {
  if (muted()) return;
  emit("note", loc, msg, "", "");
}
