// types.h — PL/I data attributes reduced to a compiler type.
//
// The full attribute lattice of TR 25.084 rules (14)-(43) is much larger than
// this; M0 models the scalar computational types only. Aggregates, PICTURE,
// AREA/OFFSET, ENTRY/FILE/LABEL variables are M2-M4 (see IMPLEMENTATION-PLAN).
#pragma once
#include <string>

enum class TK {
  FixedBin,  // FIXED BINARY(p,q)
  FixedDec,  // FIXED DECIMAL(p,q)
  Float,     // FLOAT DECIMAL(p) / FLOAT BINARY(p)
  Char,      // CHARACTER(n) [VARYING]
  Bit,       // BIT(n)
  Void,
};

struct Type {
  TK k = TK::FixedBin;
  int prec = 15;         // precision: digits (DECIMAL) or bits (BINARY)
  int scale = 0;         // FIXED scale factor q
  int len = 1;           // CHARACTER/BIT length
  bool varying = false;  // VARYING (rule 15)

  static Type fixedBin(int p = 15, int q = 0) { Type t; t.k = TK::FixedBin; t.prec = p; t.scale = q; return t; }
  static Type fixedDec(int p = 5, int q = 0) { Type t; t.k = TK::FixedDec; t.prec = p; t.scale = q; return t; }
  static Type flt(int p = 6) { Type t; t.k = TK::Float; t.prec = p; return t; }
  static Type chr(int n, bool vary = false) { Type t; t.k = TK::Char; t.len = n; t.varying = vary; return t; }
  static Type bit(int n = 1) { Type t; t.k = TK::Bit; t.len = n; return t; }
  static Type voidTy() { Type t; t.k = TK::Void; return t; }

  bool isFixed() const { return k == TK::FixedBin || k == TK::FixedDec; }
  bool isNumeric() const { return isFixed() || k == TK::Float; }
  bool isChar() const { return k == TK::Char; }
  bool isBit() const { return k == TK::Bit; }
  bool isVoid() const { return k == TK::Void; }

  // Integer width chosen for FIXED values (M0 keeps FIXED scale 0 only).
  int intBits() const {
    if (k == TK::FixedBin) return prec <= 31 ? 32 : 64;
    if (k == TK::FixedDec) return prec <= 9 ? 32 : 64;
    return 32;
  }

  // LLVM first-class type used to hold a value of this PL/I type in memory.
  std::string llvmTy() const {
    switch (k) {
      case TK::FixedBin:
      case TK::FixedDec: return intBits() == 32 ? "i32" : "i64";
      case TK::Float: return "double";
      case TK::Bit: return "i8";
      case TK::Char:
        if (varying) return "{ i32, [" + std::to_string(len) + " x i8] }";
        return "[" + std::to_string(len) + " x i8]";
      case TK::Void: return "void";
    }
    return "i32";
  }

  // Human readable attribute list, used in diagnostics.
  std::string desc() const {
    switch (k) {
      case TK::FixedBin: return "FIXED BINARY(" + std::to_string(prec) + "," + std::to_string(scale) + ")";
      case TK::FixedDec: return "FIXED DECIMAL(" + std::to_string(prec) + "," + std::to_string(scale) + ")";
      case TK::Float: return "FLOAT DECIMAL(" + std::to_string(prec) + ")";
      case TK::Char: return "CHARACTER(" + std::to_string(len) + ")" + (varying ? " VARYING" : "");
      case TK::Bit: return "BIT(" + std::to_string(len) + ")";
      case TK::Void: return "VOID";
    }
    return "?";
  }
};
