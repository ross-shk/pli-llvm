// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
#include "diag.h"
#include <cstdio>
#include <sstream>

void Diags::emit(const char* level, SourceLoc loc, const std::string& msg, const std::string& rule,
                 const std::string& replace) {
  std::string suffix;
  if (!rule.empty())
    suffix = "  [TR 25.084 rule " + rule + "]";

  // Translate the flat (in-the-concatenated-source) location back to the real
  // source file/line so includes don't shift or misfile diagnostics (ADR-219).
  const FileLoc* fl = resolveFileLoc(loc);
  const std::string* source = src_;
  if (fl && fileSrc_ && fileSrc_->count(fl->path))
    source = &fileSrc_->at(fl->path);
  std::string name = (fl && fl->path == rootCanon_) ? file_
                     : (fl ? fl->path : file_);
  int lineNum = fl ? fl->line : loc.line;
  int col = loc.col;

  if (lineNum > 0)
    fprintf(stderr, "%s:%d:%d: %s: %s%s\n", name.c_str(), lineNum, col, level, msg.c_str(),
            suffix.c_str());
  else
    fprintf(stderr, "%s: %s: %s%s\n", name.c_str(), level, msg.c_str(), suffix.c_str());

  // Echo the source line plus a caret, clang-style; rendered ASCII-safe so
  // the UTF-8 not sign reads as '^' on any terminal and the caret aligns.
  if (source && lineNum > 0) {
    std::istringstream is(*source);
    std::string ln;
    for (int i = 0; i < lineNum && std::getline(is, ln); ++i) {
    }
    if (!ln.empty() || lineNum > 0) {
      int target = col > 0 ? col - 1 : 0;
      int d = 0;
      for (int j = 0; j < target && j < (int)ln.size(); ++j) {
        unsigned char b = (unsigned char)ln[j];
        if (b == 0xC2 && j + 1 < (int)ln.size() && (unsigned char)ln[j + 1] == 0xAC)
          ++j; // two bytes, one display column
        ++d;
      }
      std::string show, caret(d, ' ');
      caret += '^';
      for (size_t j = 0; j < ln.size(); ++j) {
        unsigned char b = (unsigned char)ln[j];
        if (b == 0xC2 && j + 1 < ln.size() && (unsigned char)ln[j + 1] == 0xAC) {
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

const FileLoc* Diags::resolveFileLoc(SourceLoc loc) const {
  if (!lineMap_ || loc.line <= 0)
    return nullptr;
  if (loc.line > (int)lineMap_->size())
    return nullptr;
  return &(*lineMap_)[loc.line - 1];
}

void Diags::error(SourceLoc loc, const std::string& msg, const std::string& rule,
                  const std::string& replace) {
  if (muted()) {
    ++mnerr_;
    return;
  }
  ++nerr_;
  emit("error", loc, msg, rule, replace);
}

void Diags::warn(SourceLoc loc, const std::string& msg, const std::string& rule,
                 const std::string& replace) {
  if (muted())
    return;
  ++nwarn_;
  emit("warning", loc, msg, rule, replace);
}

void Diags::note(SourceLoc loc, const std::string& msg) {
  if (muted())
    return;
  emit("note", loc, msg, "", "");
}
