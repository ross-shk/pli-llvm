// types.h — PL/I data attributes reduced to a compiler type.
//
// The full attribute lattice of TR 25.084 rules (14)-(43) is much larger than
// this; M0 models the scalar computational types only. Aggregates, PICTURE,
// AREA/OFFSET, ENTRY/FILE/LABEL variables are M2-M4 (see IMPLEMENTATION-PLAN).
#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>

enum class TK {
  FixedBin, // FIXED BINARY(p,q)
  FixedDec, // FIXED DECIMAL(p,q)
  Float,    // FLOAT DECIMAL(p) / FLOAT BINARY(p)
  Char,     // CHARACTER(n) [VARYING]
  Bit,      // BIT(n)
  Struct,   // structure with level-numbered members (rule 11)
  Void,
};

// One level-numbered structure member (rule 11). Defined after Type: a member's
// `ty` can itself be a Struct, so the mutual reference is broken by storing
// members in Type as unique_ptr. Type stays copyable through a deep-copy
// constructor (see the out-of-line definitions below Member).
struct Member;

// One array axis (rules (12),(13)): a lower and upper bound. A dynamic (runtime)
// upper bound — a general expression, or a `*` adjustable extent — is marked by
// `dyn`; `ub` is then unused and the extent is only known at run time. A dynamic
// (runtime) lower bound is marked by `lbDyn`; `lb` is then a placeholder and the
// live lower bound is evaluated at entry from the symbol's lower-bound
// expression (`dynLb`, the mirror of `dynUb`).
struct Dim {
  int lb = 1;
  int ub = 1;
  bool dyn = false;   // the upper bound is a runtime value (rule (13))
  bool lbDyn = false; // the lower bound is a runtime value (rule (13))
  bool adj = false;   // '*' adjustable extent: the bound comes from the caller (rule (13))
  bool operator==(const Dim& o) const {
    return lb == o.lb && ub == o.ub && dyn == o.dyn && lbDyn == o.lbDyn && adj == o.adj;
  }
};

struct Type {
  TK k = TK::FixedBin;
  int prec = 15;        // precision: digits (DECIMAL) or bits (BINARY)
  int scale = 0;        // FIXED scale factor q
  int len = 1;          // CHARACTER/BIT length
  bool varying = false; // VARYING (rule 15)
  // Array dimension bounds per axis — rules (12),(13). Empty for a scalar.
  // `len`/`prec`/... describe the element type.
  std::vector<Dim> dims;
  // Structure members (rule 11), in declaration order. Only meaningful when
  // k == TK::Struct.
  std::vector<std::unique_ptr<Member>> members;

  // Copy/move/destroy are custom (deep-copy members) and defined below Member.
  Type() = default;
  Type(const Type& o);
  Type& operator=(const Type& o);
  Type(Type&& o) noexcept;
  Type& operator=(Type&& o) noexcept;
  ~Type() = default;

  bool operator==(const Type&) const; // defined below Member

  bool isArray() const { return !dims.empty(); }
  bool isStruct() const { return k == TK::Struct; }
  // True when any axis has a runtime (dynamic) extent (rule (13)).
  bool isDynamic() const {
    for (const auto& d : dims)
      if (d.dyn || d.lbDyn)
        return true;
    return false;
  }
  // The scalar type of one element (dims cleared).
  Type elementType() const {
    Type t = *this;
    t.dims.clear();
    return t;
  }

  static Type fixedBin(int p = 15, int q = 0) {
    Type t;
    t.k = TK::FixedBin;
    t.prec = p;
    t.scale = q;
    return t;
  }
  static Type fixedDec(int p = 5, int q = 0) {
    Type t;
    t.k = TK::FixedDec;
    t.prec = p;
    t.scale = q;
    return t;
  }
  static Type flt(int p = 6) {
    Type t;
    t.k = TK::Float;
    t.prec = p;
    return t;
  }
  static Type chr(int n, bool vary = false) {
    Type t;
    t.k = TK::Char;
    t.len = n;
    t.varying = vary;
    return t;
  }
  static Type bit(int n = 1) {
    Type t;
    t.k = TK::Bit;
    t.len = n;
    return t;
  }
  static Type voidTy() {
    Type t;
    t.k = TK::Void;
    return t;
  }
  // Build a structure type from its level-numbered members (rule 11).
  static Type structTy(std::vector<Member> m);

  bool isFixed() const { return k == TK::FixedBin || k == TK::FixedDec; }
  bool isNumeric() const { return isFixed() || k == TK::Float; }
  bool isChar() const { return k == TK::Char; }
  bool isBit() const { return k == TK::Bit; }
  bool isVoid() const { return k == TK::Void; }

  // Integer width chosen for FIXED values (M0 keeps FIXED scale 0 only).
  int intBits() const {
    if (k == TK::FixedBin)
      return prec <= 31 ? 32 : 64;
    if (k == TK::FixedDec)
      return prec <= 9 ? 32 : 64;
    return 32;
  }

  // Human readable attribute list, used in diagnostics.
  std::string desc() const {
    switch (k) {
    case TK::FixedBin:
      return "FIXED BINARY(" + std::to_string(prec) + "," + std::to_string(scale) + ")";
    case TK::FixedDec:
      return "FIXED DECIMAL(" + std::to_string(prec) + "," + std::to_string(scale) + ")";
    case TK::Float:
      return "FLOAT DECIMAL(" + std::to_string(prec) + ")";
    case TK::Char:
      return "CHARACTER(" + std::to_string(len) + ")" + (varying ? " VARYING" : "");
    case TK::Bit:
      return "BIT(" + std::to_string(len) + ")";
    case TK::Struct:
      return "STRUCT";
    case TK::Void:
      return "VOID";
    }
    return "?";
  }
};

// One level-numbered structure member (rule 11). Structures may nest: a
// member's `ty` can itself be a Struct.
struct Member {
  std::string name;
  Type ty{};
};

inline Type::Type(const Type& o) { *this = o; }

inline Type& Type::operator=(const Type& o) {
  if (this == &o)
    return *this;
  k = o.k;
  prec = o.prec;
  scale = o.scale;
  len = o.len;
  varying = o.varying;
  dims = o.dims;
  members.clear();
  members.reserve(o.members.size());
  for (const auto& m : o.members)
    members.push_back(std::make_unique<Member>(*m));
  return *this;
}

inline Type::Type(Type&& o) noexcept = default;
inline Type& Type::operator=(Type&& o) noexcept = default;

inline bool Type::operator==(const Type& o) const {
  if (k != o.k || prec != o.prec || scale != o.scale || len != o.len || varying != o.varying ||
      dims != o.dims)
    return false;
  if (members.size() != o.members.size())
    return false;
  for (size_t i = 0; i < members.size(); ++i)
    if (members[i]->name != o.members[i]->name || !(members[i]->ty == o.members[i]->ty))
      return false;
  return true;
}

inline Type Type::structTy(std::vector<Member> m) {
  Type t;
  t.k = TK::Struct;
  t.members.reserve(m.size());
  for (auto& mm : m)
    t.members.push_back(std::make_unique<Member>(std::move(mm)));
  return t;
}
