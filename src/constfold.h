// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// constfold.h — compile-time evaluation of PL/I constant expressions (ADR-240).
//
// MULTICS folded exactly three operators — `+`, `-` and `*` — of constant
// operands (`bound_pl1_.2.s.archive/simplify_expression.pl1:49-52`), and IBM
// folds `A+B`/`A*B` of constant operands (mvs:24317-24319). The port keeps
// both restrictions: only those operators fold, and a division never folds,
// so the ZERODIVIDE condition a division may raise survives.
//
// The fold is *type-directed*: the value is computed at the scale of the
// expression's result type, so a folded constant is exactly the value the
// runtime path produces — the rescale rounds half away from zero, the rule
// the runtime rescale uses (Y33-6003).
#pragma once
#include "ast.h"
#include "types.h"

// A constant numeric value: `value` scaled by 10^`scale`.
struct ConstNum {
  PliI128 value = 0;
  int scale = 0; // fraction digits (the value is scaled by 10^scale)
  int prec = 0;  // significant digits of the value
};

// The number of decimal digits in |v| (zero has one digit).
int constDigits(PliI128 v);

// Evaluate a constant numeric expression exactly, at the scale its operands
// imply: the larger of the operand scales for `+` and `-`, the sum of them for
// `*` (ADR-006). Returns false when the expression is not foldable — a
// division, a non-constant or non-numeric operand, or a non-decimal leaf.
bool constNumOf(const Expr* e, ConstNum& out);

// Rewrite `e` in place as the DECIMAL literal its value denotes.
//
// `scale` is the scale of the folded value: the value is rescaled to it,
// rounding half away from zero. `ty` is the type the literal carries and
// must be a FIXED DECIMAL; a value that does not fit `ty` is not folded, so
// the fold can never drop a SIZE condition the runtime path would raise.
//
// Returns false when the expression is not foldable, leaving `e` untouched.
bool foldConstExpr(Expr* e, const Type& ty, int scale);
