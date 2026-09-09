// diag.h — diagnostics with source locations and spec rule references.
//
// Diagnostics cite TR 25.084 production numbers where a rule is implicated,
// e.g. "expected THEN [TR 25.084 rule (75)]". This keeps the implementation
// traceable to the formal definition.
#pragma once
#include <string>
#include <vector>

struct SourceLoc {
  int line = 0;
  int col = 0;
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
  std::string file_;
  const std::string* src_ = nullptr;
  int nerr_ = 0;
  int nwarn_ = 0;
  int mute_ = 0;
  int mnerr_ = 0;
};
