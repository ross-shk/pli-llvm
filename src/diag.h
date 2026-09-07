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

  void error(SourceLoc loc, const std::string &msg, const std::string &rule = "");
  void warn(SourceLoc loc, const std::string &msg, const std::string &rule = "");
  void note(SourceLoc loc, const std::string &msg);

  int errorCount() const { return nerr_; }
  int warnCount() const { return nwarn_; }
  bool ok() const { return nerr_ == 0; }

  // Source text is retained so diagnostics can echo the offending line.
  void setSource(const std::string *src) { src_ = src; }

private:
  void emit(const char *level, SourceLoc loc, const std::string &msg,
            const std::string &rule);
  std::string file_;
  const std::string *src_ = nullptr;
  int nerr_ = 0;
  int nwarn_ = 0;
};
