// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// irgen_decisions.cpp — the fact store and its report (ADR-260).
//
// The store is deliberately dumb: it records what the compiler decided and
// prints it. It makes no decision of its own, so the report always agrees
// with the code that produced it.
#include "irgen_decisions.h"

#include <algorithm>

namespace pli {

// Record one check's state; a repeated kind overwrites, so the report shows
// the effective setting rather than every assignment that led to it.
void FactStore::setCheck(const std::string& kind, bool on) {
  for (auto& c : checks_)
    if (c.first == kind) {
      c.second = on;
      return;
    }
  checks_.emplace_back(kind, on);
}

// Open a procedure for recording; a second beginProc for the same name
// continues the existing entry so a procedure emitted in pieces stays one.
void FactStore::beginProc(const std::string& name) {
  for (size_t i = 0; i < procs_.size(); ++i)
    if (procs_[i].name == name) {
      cur_ = static_cast<int>(i);
      return;
    }
  procs_.push_back(ProcFacts{name, {}});
  cur_ = static_cast<int>(procs_.size()) - 1;
}

// Record one built-in lowering decision, counting repeats of the same pair.
void FactStore::recordBuiltin(const std::string& op, const std::string& path) {
  if (cur_ < 0)
    return;
  ProcFacts& p = procs_[static_cast<size_t>(cur_)];
  for (auto& b : p.builtins)
    if (b.op == op && b.path == path) {
      b.count++;
      return;
    }
  p.builtins.push_back(BuiltinFact{op, path, 1});
}

// The report, one fact per line. The module facts come first, then one block
// per procedure with the facts it produced.
std::string FactStore::report() const {
  std::string out = "; pli facts\n";
  out += "module\n";
  out += "  lowering: " + loweringMode_ + "\n";
  if (!checks_.empty()) {
    out += "  checks:";
    for (const auto& c : checks_)
      out += " " + c.first + (c.second ? " on" : " off");
    out += "\n";
  }
  for (const auto& p : procs_) {
    if (p.builtins.empty())
      continue;
    out += "proc " + p.name + "\n";
    for (const auto& b : p.builtins)
      out += "  builtin " + b.op + " -> " + b.path + " x" + std::to_string(b.count) + "\n";
  }
  return out;
}

// The text of a REPEAT of a constant string a constant number of times
// (ADR-241). The decision lives here, outside irgen.cpp (the plan's C2), so
// it can be read without a backend.
std::string foldRepeatText(const std::string& src, long long count) {
  if (src.empty() || count <= 0)
    return {};
  // The result type is the source length times the count (sema sized it), so
  // the two agree; the guard only keeps the text bounded.
  if (count > (1LL << 20) || (long long)src.size() * count > (1LL << 20))
    return {};
  std::string text;
  text.reserve(src.size() * (size_t)count);
  for (long long k = 0; k < count; ++k)
    text += src;
  return text;
}

// The membership table of a constant alphabet (ADR-241). A byte is a
// character code in PL/I, so the table is indexed by the full 256-value range
// and the scan it replaces is exactly "is this byte in the alphabet".
std::string searchTableBytes(const std::string& alphabet) {
  std::string bytes(256, '\0');
  for (unsigned char c : alphabet)
    bytes[c] = 1;
  return bytes;
}

// The translate table of constant `out`/`in` strings (ADR-264). The inline
// scan walks `in` for the first occurrence of the character and answers with
// `out` at that index, a blank when the index is past the end of `out`, and
// the character itself when it does not occur — the table below is that
// function evaluated once per byte value.
std::string translateTableBytes(const std::string& out, const std::string& in) {
  std::string bytes(256, '\0');
  bool mapped[256] = {false};
  for (int c = 0; c < 256; ++c)
    bytes[(size_t)c] = static_cast<char>(c);
  for (size_t j = 0; j < in.size(); ++j) {
    unsigned char c = static_cast<unsigned char>(in[j]);
    // The first occurrence decides, which is where the scan stops.
    if (mapped[c])
      continue;
    mapped[c] = true;
    bytes[c] = j < out.size() ? out[j] : ' ';
  }
  return bytes;
}

} // namespace pli
