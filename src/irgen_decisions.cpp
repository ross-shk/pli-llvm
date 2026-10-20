// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// irgen_decisions.cpp — the fact store and its report (ADR-260).
//
// The store is deliberately dumb: it records what the compiler decided and
// prints it. It makes no decision of its own, so the report always agrees
// with the code that produced it.
#include "irgen_decisions.h"

#include <algorithm>
#include <cctype>
#include <limits>

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

namespace {

// The picture characters of rule (148). The classifier validates against this
// set before it looks at the layout, so a malformed string cannot reach the
// folder.
bool isPictureChar(char c) {
  static const std::string chars = "ABCDEGHIKMPSTVYZ$.,*/1236789XFR+-";
  return chars.find(c) != std::string::npos;
}

// The characters that make a picture numeric rather than character. A picture
// with none of them is a character picture (A/X), which P output cannot use.
bool isNumericPictureChar(char c) {
  static const std::string chars = "9Z123V.,$+-SY*EK";
  return chars.find(c) != std::string::npos;
}

// The subset the constant folder reproduces byte for byte: the digits, the
// assumed decimal point, the signs, and the A/X blank positions. Everything
// else (repetition, F(n), currency, exponent, sterling, CR/DB) stays on the
// runtime interpreter.
bool isFoldablePictureChar(char c) {
  static const std::string chars = "9ZV.+-SXA";
  return chars.find(c) != std::string::npos;
}

} // namespace

PictureSpec classifyPicture(const std::string& pic) {
  PictureSpec s;
  if (pic.empty())
    return s;

  for (size_t i = 0; i < pic.size();) {
    long long repeat = 1;
    bool repeated = false;
    // rule (147): "(integer) picture-character" repeats the character; n = 0
    // ignores it.
    if (pic[i] == '(') {
      size_t j = i + 1;
      long long n = 0;
      bool digits = false;
      while (j < pic.size() && std::isdigit(static_cast<unsigned char>(pic[j]))) {
        digits = true;
        n = n * 10 + (pic[j] - '0');
        if (n > 1000000)
          return s; // a picture this large is not a picture we want to expand
        ++j;
      }
      if (!digits || j >= pic.size() || pic[j] != ')')
        return s;
      repeat = n;
      repeated = true;
      i = j + 1;
      if (i >= pic.size())
        return s;
    }

    char c = pic[i];
    // rule (146): the trailing F(+n)/F(-n) scaling factor moves the assumed
    // decimal point; it emits no character.
    if (c == 'F') {
      if (repeated || i + 1 >= pic.size() || pic[i + 1] != '(')
        return s;
      size_t j = i + 2;
      int sign = 1;
      if (j < pic.size() && (pic[j] == '+' || pic[j] == '-')) {
        if (pic[j] == '-')
          sign = -1;
        ++j;
      }
      long long n = 0;
      bool digits = false;
      while (j < pic.size() && std::isdigit(static_cast<unsigned char>(pic[j]))) {
        digits = true;
        n = n * 10 + (pic[j] - '0');
        if (n > 1000000)
          return s;
        ++j;
      }
      if (!digits || j >= pic.size() || pic[j] != ')' || j + 1 != pic.size())
        return s;
      s.hasScale = true;
      s.scale = sign * static_cast<int>(n);
      i = j + 1;
      continue;
    }

    if (!isPictureChar(c))
      return s;
    if (repeated)
      s.hasRepeat = true;
    // Expand the repetition now: every later step (width, digit positions,
    // folding) sees one character per output position.
    for (long long k = 0; k < repeat; ++k) {
      s.expanded.push_back(c);
      if (c != 'V')
        ++s.width;
    }
    ++i;
  }

  if (s.expanded.empty())
    return s;

  // The first V or '.' is the assumed decimal point; digits after it are
  // fractional positions (the rule the runtime formatter uses).
  for (size_t i = 0; i < s.expanded.size(); ++i)
    if (s.expanded[i] == 'V' || s.expanded[i] == '.') {
      s.point = static_cast<int>(i);
      break;
    }
  for (size_t i = 0; i < s.expanded.size(); ++i) {
    char c = s.expanded[i];
    if (c != '9' && c != 'Z')
      continue;
    if (s.point >= 0 && static_cast<int>(i) > s.point)
      ++s.fracDigits;
    else
      ++s.intDigits;
  }

  for (char c : s.expanded)
    if (isNumericPictureChar(c)) {
      s.numeric = true;
      break;
    }

  // The runtime walks at most 127 picture characters (its output buffer), so
  // an expanded picture past that cap cannot be reproduced exactly.
  bool foldable = s.numeric && !s.hasRepeat && !s.hasScale && s.width > 0 &&
                  s.expanded.size() <= 127 && s.intDigits + s.fracDigits > 0;
  for (char c : s.expanded)
    if (!isFoldablePictureChar(c))
      foldable = false;
  s.foldable = foldable;
  s.valid = true;
  return s;
}

bool foldPictureConversion(long long value, int scale, const PictureSpec& spec, std::string& out) {
  if (!spec.valid || !spec.foldable || scale < 0)
    return false;
  // The runtime negates in signed arithmetic, so LLONG_MIN has no magnitude
  // there either; keep that value on the runtime path rather than disagree.
  if (value == std::numeric_limits<long long>::min())
    return false;

  const bool isneg = value < 0;
  unsigned long long av = isneg ? static_cast<unsigned long long>(-(value + 1)) + 1
                                : static_cast<unsigned long long>(value);
  long long digits[64];
  long long ndig = 0;
  if (av == 0) {
    digits[0] = 0;
    ndig = 1;
  } else {
    while (av > 0 && ndig < 64) {
      digits[ndig++] = static_cast<long long>(av % 10);
      av /= 10;
    }
    for (long long i = 0; i < ndig / 2; ++i) {
      long long t = digits[i];
      digits[i] = digits[ndig - 1 - i];
      digits[ndig - 1 - i] = t;
    }
  }

  const long long val_int = ndig > scale ? ndig - scale : 0;
  const long long val_frac = scale < ndig ? scale : 0;
  std::string text;
  text.reserve(static_cast<size_t>(spec.width));
  int led_zero = 1;
  long long frac_idx = 0;

  for (size_t pi = 0; pi < spec.expanded.size(); ++pi) {
    char c = spec.expanded[pi];
    const bool is_fractional = spec.point >= 0 && static_cast<long long>(pi) > spec.point;

    if (c == 'V')
      continue; // no output character

    if (c == '9' || c == 'Z') {
      if (is_fractional) {
        if (frac_idx < val_frac) {
          text.push_back(static_cast<char>('0' + digits[val_int + frac_idx]));
          led_zero = 0;
        } else {
          text.push_back('0'); // zero-pad if the value has fewer fractional digits
        }
        ++frac_idx;
      } else {
        long long int_idx = 0;
        for (size_t j = 0; j < pi; ++j)
          if (spec.expanded[j] == 'Z' || spec.expanded[j] == '9')
            ++int_idx;
        long long src_idx = int_idx - (spec.intDigits - val_int);
        if (src_idx < 0) {
          text.push_back(c == '9' ? '0' : ' '); // Z -> blank, 9 -> zero
        } else if (src_idx < val_int) {
          char d = static_cast<char>('0' + digits[src_idx]);
          if (c == 'Z' && d == '0' && led_zero) {
            text.push_back(' '); // leading zero suppression
          } else {
            led_zero = 0;
            text.push_back(d);
          }
        } else {
          text.push_back(c == '9' ? '0' : ' ');
        }
      }
      continue;
    }
    if (c == '.') {
      text.push_back('.');
      continue;
    }
    if (c == '+') {
      text.push_back(isneg ? '-' : '+');
      continue;
    }
    if (c == '-' || c == 'S') {
      text.push_back(isneg ? '-' : ' ');
      continue;
    }
    if (c == 'X' || c == 'A') {
      text.push_back(' ');
      continue;
    }
    text.push_back(c); // unreachable for a foldable picture
  }

  // The width is the contract the runtime writes; a mismatch means the
  // classifier and the formatter disagree, so keep the runtime path.
  if (static_cast<int>(text.size()) != spec.width)
    return false;
  out = std::move(text);
  return true;
}

} // namespace pli
