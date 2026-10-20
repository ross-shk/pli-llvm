// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// irgen_decisions.h — the compiler's PL/I-specific decisions, and the report
// `--print-facts` prints.
//
// ADR-260: the decision layer lives outside irgen.cpp (the plan's C2). What
// lives here answers "what did the compiler decide, and why" without touching
// LLVM, so a decision can be read in a test without a backend.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace pli {

// One built-in lowering decision: "llvm" means the inline LLVM expansion won
// over the pli_* runtime call, "runtime" means the library call.
struct BuiltinFact {
  std::string op;   // "assign_char"
  std::string path; // "llvm" | "runtime"
  unsigned count = 0;
};

// The facts the compiler computed for one procedure.
struct ProcFacts {
  std::string name;
  std::vector<BuiltinFact> builtins;
};

// The compiler's fact store: filled while lowering, printed by --print-facts.
// A fact is a decision the compiler already made, not a new decision — the
// decisions themselves stay where the plan put them.
class FactStore {
public:
  // Module-level facts: the lowering policy and which checks are enabled.
  void setLoweringMode(const std::string& mode) { loweringMode_ = mode; }
  void setCheck(const std::string& kind, bool on);
  // Per-procedure facts: beginProc opens a procedure, recordBuiltin adds to it.
  void beginProc(const std::string& name);
  void recordBuiltin(const std::string& op, const std::string& path);
  // The report: one fact per line, module facts first.
  std::string report() const;

private:
  std::string loweringMode_ = "auto";
  std::vector<std::pair<std::string, bool>> checks_;
  std::vector<ProcFacts> procs_;
  // Index into procs_ of the procedure being lowered; -1 when none is open.
  int cur_ = -1;
};

// The text of a REPEAT of a constant string a constant number of times
// (ADR-241): the repeated text is a compile-time constant, so the result
// needs neither a runtime buffer nor a copy. Empty when the repetition is
// not a foldable constant.
std::string foldRepeatText(const std::string& src, long long count);

// The bytes of the membership table of a constant character alphabet (ADR-241):
// entry c is 1 when the byte value c occurs in the alphabet, 0 otherwise. The
// table replaces a scan of the alphabet, so building it is a decision, not
// codegen — it lives here (the plan's C2).
std::string searchTableBytes(const std::string& alphabet);

// The bytes of the translation table of constant `out`/`in` strings (ADR-264):
// entry c is the character that a byte with value c maps to. `out` is padded
// with blanks to the length of `in`, the PL/I rule the inline scan already
// uses, so a match past the end of `out` maps to a blank; a character absent
// from `in` maps to itself. The first occurrence in `in` decides.
std::string translateTableBytes(const std::string& out, const std::string& in);

// WS-K3: the classification of one PICTURE specification (rules (146)-(148)).
// The classifier expands repetition factors (rule (147)) and separates the
// trailing F(n) scaling factor (rule (146)); `foldable` is true only for the
// numeric subset the constant folder below reproduces, so every other picture
// keeps the runtime interpreter.
struct PictureSpec {
  bool valid = false;     // parses as a picture specification
  bool numeric = false;   // numeric rather than character picture
  bool foldable = false;  // the simple numeric subset B4 folds
  bool hasRepeat = false; // a repetition factor was expanded
  bool hasScale = false;  // a trailing F(n) scaling factor was present
  int width = 0;          // output positions (V and F(n) emit none)
  int intDigits = 0;      // 9/Z positions before the decimal point
  int fracDigits = 0;     // 9/Z positions after the decimal point
  int point = -1;         // expanded index of V/., -1 when none
  int scale = 0;          // F(n) scaling factor
  std::string expanded;   // repetition factors expanded, F(n) removed
};

// Classify a picture string: expand repetitions, record the digit layout and
// the scaling factor, and decide whether the constant folder can reproduce it.
PictureSpec classifyPicture(const std::string& pic);

// B4: format a constant numeric value through a classified picture at compile
// time. `value` is scaled by 10^`scale`, exactly the pair the runtime
// `pli_put_edit_pic` receives. Returns false when the picture is not foldable
// or the value cannot be represented, leaving the caller on the runtime path.
bool foldPictureConversion(long long value, int scale, const PictureSpec& spec, std::string& out);

} // namespace pli