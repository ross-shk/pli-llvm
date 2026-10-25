// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
#pragma once

#include "diag.h" // FileLoc

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class Preprocessor {
public:
  bool run(const std::filesystem::path& input, std::string& output);
  // Extra %INCLUDE search directories, tried in order after the including
  // file's own directory (ADR-078).
  void addIncludeDir(const std::filesystem::path& dir);

private:
  bool expand(const std::filesystem::path& input, std::string& output);
  bool handleInclude(const std::filesystem::path& input, const std::string& operand, int line,
                     int col, std::string& output);
  // IBM Enterprise %XINCLUDE (ADR-157): a comma-separated list of files or
  // members, each included only once per compilation.
  bool handleXInclude(const std::filesystem::path& input, const std::string& operand, int line,
                      int col, std::string& output);
  // Expand one include name; with once == true skip files already included
  // or currently being expanded. Shared by %INCLUDE and %XINCLUDE.
  bool expandOneInclude(const std::filesystem::path& input, const std::string& rawName, int line,
                        int col, std::string& output, bool once);
  // Append text to output, recording one lineMap entry per \n so diagnostics
  // stay attributed to the correct source file:line (ADR-219, ADR-305).
  void emit(const std::string& text, std::string& output, const std::filesystem::path& path,
            int& line, int& col);
  // Scan a source range char-by-char, emitting non-directive text via emit()
  // and dispatching %-directives to processDirective (ADR-305). String, comment
  // and // comment nesting is tracked so directives inside them are inert.
  bool scan(const std::string& source, size_t start, size_t end, std::string& output,
            int& line, int& col, const std::filesystem::path& path, bool active);
  // Pre-scan source[start..end) for %label: definitions (ADR-305).
  void collectLabels(const std::string& source, size_t start, size_t end);
  // Find the ';' (or comment-embedded ';') that terminates a directive body
  // starting just after `start`, skipping quoted strings (ADR-305).
  size_t findDirectiveEnd(const std::string& source, size_t start);
  // Find the matching %END for a %DO starting at `start`, returning its position
  // in `endPos` and the ';' position in `endSemi` (ADR-305).
  size_t findMatchingEnd(const std::filesystem::path& path, const std::string& source,
                         size_t start, size_t& endSemi, int line, int col);
  // Parse and run one %-directive at `i` (source[i] == '%'), returning the
  // index past it (npos after diagnosing). With active == false the directive
  // is skipped without effects; newlines stay caller-preserved. Sets semPos to
  // the last character position of the directive's own text so the caller can
  // preserve its newlines (ADR-305).
  size_t processDirective(const std::filesystem::path& path, const std::string& source, size_t i,
                          std::string& output, int& line, int& col, bool active, size_t& semPos);
  // %IF expr %THEN directive [%ELSE directive] (ADR-083): arms are full
  // directives run (or structurally skipped) recursively, so nesting works.
  size_t processIf(const std::filesystem::path& path, const std::string& source, size_t,
                   size_t wordEnd, std::string& output, int& line, int& col, bool active,
                   size_t& semPos);
  // Evaluate a %IF/%assignment integer expression (ADR-083); on failure
  // returns false after diagnosing at the directive position.
  bool evalCondExpr(const std::string& text, const std::filesystem::path& path, int line, int col,
                    long long& out);
  bool handleDeclare(const std::filesystem::path& input, const std::string& operand, int line,
                     int col);
  bool handleAssignment(const std::filesystem::path& input, const std::string& name,
                        const std::string& operand, int line, int col);
  bool handleActivation(const std::filesystem::path& input, const std::string& name,
                        const std::string& operand, int line, int col);
  // Bare %NAME (extension, ADR-113): expand an activated preprocessor
  // variable to its decimal value.
  bool substituteRef(const std::filesystem::path& input, const std::string& name, int line,
                     int col, std::string& output);
  // %DO [v=e1 TO e2 [BY e3]] ; — repeat the body up to %END (ADR-305).
  // Returns the position past %END (or npos on error).
  size_t handleDo(const std::filesystem::path& path, const std::string& source, size_t bodyStart,
                  size_t endPos, size_t endSemi, std::string& output, int& line, int& col,
                  bool active, const std::string& spec, int directiveLine, int directiveCol);
  // %label: / %GOTO label / %GO TO label — resolve to a flat position jump (ADR-305).
  bool handleGoto(const std::filesystem::path& path, const std::string& name,
                  const std::string& operand, int line, int col);
  // %PROCEDURE / %PROC / %RETURN — compile-time macros (Stage 2).
  size_t handleProcedure(const std::filesystem::path& path, const std::string& source, size_t i,
                         size_t wordEnd, size_t end, std::string& output, int& line, int& col,
                         bool active);
  // %REPLACE — Stage 3: textual rescanning in the preprocessor.
  bool handleReplace(const std::filesystem::path& path, const std::string& source, size_t i,
                     size_t wordEnd, size_t end, std::string& output, int& line, int& col,
                     bool active);
  bool unsupported(const std::filesystem::path& input, const std::string& name, int line, int col);
  bool error(const std::filesystem::path& input, int line, int col, const std::string& message);

  std::vector<std::filesystem::path> active_;
  std::vector<std::filesystem::path> includeDirs_;
  // Canonical paths already pulled in (via %INCLUDE or %XINCLUDE). Only
  // %XINCLUDE consults it; %INCLUDE always expands (ADR-157).
  std::set<std::filesystem::path> included_;
  // Preprocessor integer variables (%DECLARE), keyed uppercase (ADR-083).
  std::map<std::string, long long> ppVars_;
  // Activated variables (extension, ADR-113): only these expand as %NAME.
  std::set<std::string> ppActive_;
  // %label: → flat position past the ':' + whitespace (ADR-305).
  std::unordered_map<std::string, size_t> labels_;
  // Jump target set by %GOTO (ADR-305); scan() resumes there.
  size_t gotoTarget_ = 0;
  // Recursion guard for scan()/handleDo() nesting (ADR-305).
  int scanDepth_ = 0;
  static constexpr int kMaxScanDepth = 256;
  // Flat-line source map (ADR-219): lineMap_[n-1] gives the canonical file path
  // and 1-based line within that file for flat output line n, so a flat line
  // reported by the lexer maps back to the original source location across
  // %include flattening.
  std::vector<FileLoc> lineMap_;
  // Verbatim source text per canonical path, for echoing the cited line; the
  // root file is keyed by rootCanon_ (see below).
  std::map<std::string, std::string> fileSrc_;
  // Canonical path of the root file passed to run(); kept so Diags can render
  // root locations with the original `file_` spelling.
  std::string rootCanon_;

public:
  const std::vector<FileLoc>& lineMap() const { return lineMap_; }
  const std::map<std::string, std::string>& fileSrc() const { return fileSrc_; }
  const std::string& rootCanonical() const { return rootCanon_; }
};
