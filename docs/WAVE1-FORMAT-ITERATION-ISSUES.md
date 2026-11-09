# Wave 1 Format Iteration Issues for A2

**Date**: 2026-11-09  
**From**: A3 (Verification)  
**To**: A2 (Codegen & Runtime)  
**Reference**: `.kilo/plans/1793869341000-tr25-three-agent-schedule.md` Wave 1 Slice 3

---

## Issue 1: Format Repetition Count Evaluated Multiple Times

**Test**: `tests/core/format_repeat_live.pli` — "count evaluated once" (lines 57-70)

**Expected per Y33-6003-0 §4.7 (lines 15675-15690)**: The repetition count expression is evaluated **once** at the specified iteration point, then the format group is repeated that many times.

**Actual**: The count expression `call_counter()` is evaluated 4 times (once per repetition) instead of once.

**Test Code**:
```pli
call reset_counter();           /* counter = 0 */
a = 6;
put string(buf) edit(a) ((call_counter())(f(4)));
                                /* Expected: call_counter() returns 2 ONCE, counter becomes 1 */
                                /* Actual:   call_counter() called 4 times, counter becomes 4 */
get string(buf) edit(c) (f(4));
if c ^= 6 then stop;            /* PASS: c = 6 */
call get_counter(d);            /* d = counter value */
if d ^= 1 then stop;            /* FAIL: d = 4, expected 1 */
```

**Location to Fix**: 
- `src/irgen.cpp` — format iteration codegen (search for `emitRepeatLoop` or `Repeat` node handling)
- `runtime/rt_edit.c` — if iteration logic is in runtime

**Expected Fix**: Evaluate the count expression once before the loop, store in a temporary, then loop using that value.

---

## Issue 2: GET EDIT Format Iteration Consumes Wrong Data Items

**Test**: `tests/core/format_repeat_live_get.pli` — "get_repeat" (lines 8-14)

**Expected**: With input `'  10  20'` and format `((n)(f(4)))` where `n=2`, the two data items `a` and `b` should receive `10` and `20` respectively.

**Actual**: `a = 20, b = 0` — the iteration appears to consume the second field for the first data item, then run out of data for the second.

**Test Code**:
```pli
n = 2;
buf = '  10  20';
get string(buf) edit(a, b) ((n)(f(4)));
if a ^= 10 | b ^= 20 then do;
   put skip list('FAIL get_repeat', a, b);   /* Prints: 20 0 */
   stop;
end;
```

**Location to Fix**:
- `src/irgen.cpp` — GET EDIT format iteration handling
- `runtime/rt_edit.c` — `pli_get_edit` iteration logic
- `runtime/rt_get.c` — if data item consumption is there

**Expected Fix**: The format iterator must properly pair each data item with the repeated format group in sequence. For `((2)(F(4)))` with data items `a, b`, it should:
1. Iteration 1: Apply `F(4)` to data item `a` → reads `'  10'`
2. Iteration 2: Apply `F(4)` to data item `b` → reads `'  20'`

---

## Issue 3: PUT EDIT Format Iteration (verify after fixes)

**Test**: `tests/core/format_repeat_live.pli` — "Variable count" (lines 15-28)

**Status**: Currently PASS — but verify after Issue 1/2 fixes.

```pli
n = 2;
a = 7; b = 9;
put string(buf) edit(a, b) ((n)(f(4)));
get string(buf) edit(c, d) (f(4), f(4));
/* c=7, d=9 expected */
```

---

## Y33-6003-0 Reference

- **Format iteration (rule 47)**: Lines 15675-15690 — "a repetition factor... the expression is evaluated once... if the value is zero or negative, the format item is skipped"
- **F scale factor (rule 50)**: Lines 15748-15754 — "the value is multiplied by 10^s for output, divided by 10^s for input"

---

## Acceptance Criteria

After A2 fixes:
```bash
python tests/run_tests.py core/format_repeat_live.pli
# PASS format_repeat_live

python tests/run_tests.py core/format_repeat_live_get.pli  
# PASS format_repeat_live_get
```

Both tests must pass with no modifications to the test files.

---

## Coordination

- A3 will re-run tests after A2 signals fix is ready
- No test file changes needed — tests are correct per Y33
- If A2 needs clarification on expected behavior, refer to Y33 lines cited above

*Per CONTRIBUTING.md: A3 writes failing tests first; slice owner (A2) implements fix.*