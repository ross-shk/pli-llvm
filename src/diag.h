// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// diag.h — diagnostics with source locations and spec rule references.
//
// Diagnostics cite TR 25.084 production numbers where a rule is implicated,
// e.g. "expected THEN [TR 25.084 rule (75)]". This keeps the implementation
// traceable to the formal definition.
#pragma once
#include <map>
#include <string>
#include <vector>

struct SourceLoc {
  int line = 0;
  int col = 0;
};

// A location in the original (pre-flattened) source: which file and which
// line within that file. The preprocessor builds a flat-line table mapping each
// line of the single flattened source string back to a FileLoc so that
// diagnostics keep the real file:line across `%include` (ADR-219).
struct FileLoc {
  std::string path; // canonical path as resolved by the preprocessor
  int line = 0;     // 1-based line within that file
};

class Diags {
public:
  explicit Diags(std::string filename) : file_(std::move(filename)) {}

  void error(SourceLoc loc, const std::string& msg, const std::string& rule = "",
             const std::string& replace = "");
  void warn(SourceLoc loc, const std::string& msg, const std::string& rule = "",
            const std::string& replace = "");
  void note(SourceLoc loc, const std::string& msg);

  int errorCount() const { return nerr_; }
  int warnCount() const { return nwarn_; }
  bool ok() const { return nerr_ == 0; }

  // Source text is retained so diagnostics can echo the offending line.
  void setSource(const std::string* src) { src_ = src; }

  // Flat-line translation through `%include` (ADR-219). The line map maps a 1-based
  // flat output line (the value the lexer reports as loc.line) to the real
  // source file/line; fileSrc_ supplies each file's verbatim text for echoing.
  // rootPath is the canonical path of the root file, so its locations keep the
  // original `file_` spelling passed to the constructor.
  void setLineMap(const std::vector<FileLoc>* m) { lineMap_ = m; }
  void setFileSources(const std::map<std::string, std::string>* m) { fileSrc_ = m; }
  void setRootPath(const std::string& p) { rootCanon_ = p; }

  // Speculative-parse probes (ADR-004 step 3): while muted, diagnostics are
  // neither printed nor counted; a probe's errors tally separately so the
  // parser can detect a failed probe and rewind that tally when discarding
  // the interpretation.
  void mute() { ++mute_; }
  void unmute() {
    if (mute_ > 0)
      --mute_;
  }
  bool muted() const { return mute_ > 0; }
  int mutedErrors() const { return mnerr_; }
  void rewindMutedErrors(int to) { mnerr_ = to; }

private:
  void emit(const char* level, SourceLoc loc, const std::string& msg, const std::string& rule,
            const std::string& replace);
  // Resolve a flat SourceLoc (line = line in the flattened src) to the real
  // source file/line; returns null when no line map is configured or the line
  // is out of range (caller falls back to the flat loc).
  const FileLoc* resolveFileLoc(SourceLoc loc) const;
  std::string file_;
  const std::string* src_ = nullptr;
  const std::vector<FileLoc>* lineMap_ = nullptr;
  const std::map<std::string, std::string>* fileSrc_ = nullptr;
  std::string rootCanon_;
  int nerr_ = 0;
  int nwarn_ = 0;
  int mute_ = 0;
  int mnerr_ = 0;
};
