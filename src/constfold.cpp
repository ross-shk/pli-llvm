// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// constfold.cpp — compile-time evaluation of PL/I constant expressions (ADR-240).
#include "constfold.h"

#include <algorithm>

namespace {

// 10^k as a 128-bit integer; k <= 38 fits.
PliI128 pow10(int k) {
  PliI128 r = 1;
  for (int i = 0; i < k; ++i)
    r = r * 10;
  return r;
}

// A constant leaf's scale: an integer literal has scale 0 (rule 135), a
// decimal literal the number of fraction digits it was written with.
int leafScale(const Expr* e) { return e->kind == Expr::DecLit ? e->decScale : 0; }

// Scale a raw value up from `from` to `to` (to >= from). The value is exact:
// a scale-up multiplies by a power of ten.
PliI128 scaleUp(PliI128 v, int from, int to) {
  for (int i = from; i < to; ++i)
    v = v * 10;
  return v;
}

// The magnitude of the largest constant leaf of `e`, expressed at `scale`.
// False when a leaf is not a constant. The runtime path converts every
// operand to the common type before the operation and traps (SIZE) when an
// operand does not fit it, so the fold must see the same magnitudes.
bool maxLeafMag(const Expr* e, int scale, PliI128& out) {
  if (!e)
    return false;
  switch (e->kind) {
  case Expr::IntLit:
  case Expr::DecLit: {
    int ls = leafScale(e);
    PliI128 v = e->wideIval;
    if (ls < scale)
      v = scaleUp(v, ls, scale);
    v = v < 0 ? -v : v;
    if (out < v)
      out = v;
    return true;
  }
  case Expr::Unary:
    return e->op == Tok::Minus || e->op == Tok::Plus ? maxLeafMag(e->a.get(), scale, out) : false;
  case Expr::Binary: {
    if (e->op != Tok::Plus && e->op != Tok::Minus && e->op != Tok::Star)
      return false;
    return maxLeafMag(e->a.get(), scale, out) && maxLeafMag(e->b.get(), scale, out);
  }
  default:
    return false;
  }
}

// The number of decimal digits an i64/i128 storage of `ty` can hold without
// losing the value: 9 for an i32, 18 for an i64, 38 for an i128.
int storageDigits(const Type& ty) {
  int bits = ty.intBits();
  return bits == 128 ? 38 : (bits == 64 ? 18 : 9);
}

} // namespace

int constDigits(PliI128 v) {
  PliU128 u = pli_mag_s(v);
  int digits = 0;
  do {
    pli_divmod10(&u);
    ++digits;
  } while (!pli_iszero_u(u));
  return digits;
}

bool constNumOf(const Expr* e, ConstNum& out) {
  if (!e)
    return false;
  switch (e->kind) {
  case Expr::IntLit:
  case Expr::DecLit:
    out.value = e->wideIval;
    out.scale = leafScale(e);
    out.prec = constDigits(out.value);
    return true;
  case Expr::Unary: {
    ConstNum in;
    if (!constNumOf(e->a.get(), in))
      return false;
    if (e->op == Tok::Minus)
      in.value = -in.value;
    else if (e->op != Tok::Plus)
      return false;
    out = in;
    return true;
  }
  case Expr::Binary: {
    ConstNum a, b;
    if (!constNumOf(e->a.get(), a) || !constNumOf(e->b.get(), b))
      return false;
    if (e->op == Tok::Plus || e->op == Tok::Minus) {
      // A sum or a difference takes the larger of the operand scales (ADR-006);
      // the smaller operand is rescaled up to it, exactly as the runtime path
      // converts it to the common type.
      int s = std::max(a.scale, b.scale);
      PliI128 av = scaleUp(a.value, a.scale, s);
      PliI128 bv = scaleUp(b.value, b.scale, s);
      out.value = e->op == Tok::Plus ? av + bv : av - bv;
      out.scale = s;
      out.prec = constDigits(out.value);
      return true;
    }
    if (e->op == Tok::Star) {
      // A product's scale is the sum of the operand scales (ADR-006): the raw
      // scaled integers multiply directly, with no rescale of either operand.
      out.value = a.value * b.value;
      out.scale = a.scale + b.scale;
      out.prec = constDigits(out.value);
      return true;
    }
    // A division is never folded: PL/I raises ZERODIVIDE when the divisor is
    // zero, and folding would remove that condition (MULTICS folded only
    // `+`, `-` and `*` for exactly this reason).
    return false;
  }
  default:
    return false;
  }
}

bool foldConstExpr(Expr* e, const Type& ty, int scale) {
  if (!e || ty.k != TK::FixedDec)
    return false;
  ConstNum n;
  if (!constNumOf(e, n))
    return false;
  // Rescale to the literal's scale, rounding half away from zero (Y33-6003).
  PliI128 v = n.value;
  if (scale > n.scale) {
    v = scaleUp(v, n.scale, scale);
  } else if (scale < n.scale) {
    int k = n.scale - scale;
    PliI128 d = pow10(k);
    PliI128 half = pow10(k - 1) * 5;
    PliI128 sign = v < 0 ? -1 : 1;
    v = (v + half * sign) / d;
  }
  // The value must fit the literal's type: a fold that overflows would drop
  // the SIZE condition the runtime path raises on the assignment.
  int digits = constDigits(v);
  if (digits > ty.prec || digits > storageDigits(ty))
    return false;
  // The operands must fit too: the runtime path converts each operand to the
  // common type and traps (SIZE) when one does not fit.
  PliI128 leaf = 0;
  if (!maxLeafMag(e, scale, leaf) || constDigits(leaf) > ty.prec)
    return false;
  // Rewrite the node in place as the DECIMAL literal. The node keeps its
  // identity, so the AST owner still holds the folded constant.
  e->kind = Expr::DecLit;
  e->wideIval = v;
  e->ival = pli_to_i64(v);
  e->decScale = scale;
  e->decPrec = digits;
  e->ty = ty;
  e->folded = true; // the type is the result type, not the digits' own
  e->name.clear();
  e->sym = nullptr;
  e->a.reset();
  e->b.reset();
  return true;
}
