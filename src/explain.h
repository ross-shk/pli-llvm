// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM
// explain.h — `--explain <rule>`: print a TR 25.084 production.
//
// The production table (kRules) is generated from the spec by
// scripts/gen_rules.py so it cannot drift from TR25.084-concrete-syntax.md.
// See docs/ARCHITECTURE.md §6 for the diagnostics design.
#pragma once

struct RuleDef {
  int num;
  const char* text;
};

// Generated in src/rules.cpp (build output); sorted by rule number.
extern const RuleDef kRules[];
extern const int kRuleCount;

// Print rule `num`'s production to stdout. Returns true if found.
bool explainRule(int num);
