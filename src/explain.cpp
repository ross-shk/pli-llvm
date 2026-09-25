// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM
// explain.cpp — lookup and print of TR 25.084 productions for `--explain`.
#include "explain.h"
#include <cstdio>

bool explainRule(int num) {
  for (int i = 0; i < kRuleCount; ++i) {
    if (kRules[i].num == num) {
      std::printf("(%-3d) %s\n", kRules[i].num, kRules[i].text);
      return true;
    }
  }
  return false;
}
