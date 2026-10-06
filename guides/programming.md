# PL/I Programming Guide — plic

Welcome! This is a friendly tour of what `plic` can do today. You don't need to know 1960s mainframes — if you've written a little Python or Go, you already know most of the ideas. We'll point out the similarities as we go.

Each section has a short explanation plus a runnable snippet. Copy any snippet into a `.pli` file and run it with:

```bash
plic name.pli -o name && ./name
```

> Style note: `.pli` examples below use one leading space and modern lowercase. Text starts in column 2 and stops before column 72 (the old card margins). `plic` enforces this, so just copy the style you see here.

- [Quick reference](#quick-reference)
- [Getting started](#getting-started)
- [Data types](#data-types)
- [Declarations & structures](#declarations--structures)
- [Names for types & constants](#names-for-types--constants)
- [Arrays](#arrays)
- [Operators & expressions](#operators--expressions)
- [Control flow](#control-flow)
- [Procedures & functions](#procedures--functions)
- [Packages: grouping code](#packages-grouping-code)
- [First-class procedures](#first-class-procedures)
- [Storage: BASED, CONTROLLED, ALLOCATE/FREE](#storage-based-controlled-allocatefree)
- [AREA & OFFSET regions](#area--offset-regions)
- [DEFINED & iSUB overlays](#defined--isub-overlays)
- [Conditions: ON / SIGNAL / REVERT](#conditions-on--signal--revert)
- [Concurrency: TASK, EVENT, WAIT, DELAY](#concurrency-task-event-wait-delay)
- [Preprocessor](#preprocessor)
- [Input & output](#input--output)
- [A tour of built-ins](#a-tour-of-built-ins)
- [C interoperability](#c-interoperability)

---

## Quick reference

| Topic      | What works                                                       | What doesn't yet                 |
| ---------- | ---------------------------------------------------------------- | -------------------------------- |
| Program    | `procedure options(main)` entry point                            | —                                |
| Types      | FIXED BIN/DEC, FLOAT, COMPLEX, BIT, CHAR/VARYING, POINTER        | STERLING constants, PICTURE type |
| Arrays     | fixed & dynamic bounds, slices `a(i,*)`, whole-array math        | `BIT(n>1)` arrays in some spots  |
| Storage    | BASED, CONTROLLED, DEFINED with iSUB, AREA/OFFSET                | dynamic-extent BASED arrays      |
| Conditions | ON/SIGNAL/REVERT: ERROR, SIZE, SUBSCRIPTRANGE, ZERODIVIDE, named | OVERFLOW, CHECK                  |
| I/O        | PUT/GET LIST, EDIT, DATA, STRING, FILE, OPEN/CLOSE, RECORD       | KEYED/DIRECT record              |
| Built-ins  | strings, math, arrays, misc (see `builtins.md`)                  | —                                |
| Extensions | SELECT, LEAVE/ITERATE, DO UNTIL, TRIM, PACKAGE, OPTIONAL         | —                                |

If a feature is missing, `plic` tells you the rule number (like `(24)` or `(91)`) instead of silently accepting it. That's a feature: gaps are always diagnosed.

> **Run the full example:** [`examples/quick_reference.pli`](../examples/quick_reference.pli) — `plic quick_reference.pli -o quickref && ./quickref`.

---

## Getting started

A PL/I program is one or more procedures. The one marked `options(main)` is where execution starts — think `func main()` in Go or `if __name__ == "__main__"` in Python.

```pli
 hello: procedure options(main);
   put skip list('Hello, world!');
 end;
```

That's it. No imports, no headers. Save as `hello.pli`, then:

```bash
plic hello.pli -o hello
./hello
```

`put skip list(...)` is your `print()`. It prints each item and `skip` starts a new line first. We'll meet `get list(...)` soon — that's your `input()`.

> **Short-hand notation:** you can shorten `procedure` to `proc`, `initialize` to `init`, `varying` to `var`, `pointer` to `ptr` and so on for your convinience.

> **No reserved words:** `if`, `put`, `return` and friends are ordinary names; the parser figures out from position whether you mean the keyword or your own variable, so you don't have to worry about stumbling on reserved words when naming your functions and variables.

> **Python friends:** `put skip list('hi', x)` is `print('hi', x)`.  
> **Go friends:** `hello: procedure options(main); ... end hello;` is `func main() { ... }` with a name tag on both ends.

> **Run the full example:** [`examples/getting_started.pli`](../examples/getting_started.pli) — `plic getting_started.pli -o getting_started && ./getting_started`, make sure  
> to `cd` into `examples/ `first.

---

## Data types

If you've used `int`, `float`, `str`, `bool` — you're covered. PL/I just has more precise names.

```pli
 dcl count fixed bin(31);     /* an integer, like int */
 dcl price fixed dec(7,2);    /* money: 7 digits, 2 after the point */
 dcl temp float dec(6);       /* a float, like float */
 dcl name char(20) varying;   /* a string that knows its length, like str */
 dcl flag bit(1);             /* one bit: '1'b or '0'b, like True/False */
 dcl addr pointer;            /* an address, like a Go pointer */
```

A few beginner notes:

- `dcl` is short for `declare`. Use either.
- `/* ... */` is a multi-line comment. `// ...` to end-of-line also works.
- `fixed bin(31)` means "31-bit signed integer", actually maps to the regular 32-bit integer. That's the everyday integer — just use it.
- `char(10)` is a fixed 10-byte buffer, blank-padded. `char(10) varying` is the friendly one: it remembers how long your text actually is, while 10 sets the maximum number of character it can hold. **When in doubt, use **`varying`**.** It behaves like a Python string.
- `dcl z char(20) varyingz;` is a NUL-terminated string for talking to C (more in [C interoperability](#c-interoperability)).

If you skip the type, PL/I guesses: names starting with I–N become `fixed bin(31)`, everything else becomes `float dec(6)`. This is a 1960s habit. Explicit is better — always write your type.

### Money needs decimals, not floats

Floats can't represent 0.1 exactly (try `0.1 + 0.2` in Python!). For money or measurements, use `fixed dec(p,q)`: `p` digits total, `q` after the point. The value is stored as a scaled integer.

```pli
 dcl price fixed dec(5,2);
 price = 12.34;   /* stored as 1234 with scale 100 */
```

> **Python friends:** `fixed dec` is `decimal.Decimal` built into the language.  
> **Go friends:** think of it as an integer of cents with the compiler remembering where the point goes.

Overflow of `+ - *` on fixed types can raise the `size` condition (see [Conditions](#conditions-on--signal--revert)) when you have a handler.

**Not yet**

- `fixed bin` precision is 31 or 63 bits; mixed widths widen to the larger side.
- `/` and `**` use floating-point even for fixed operands.
- `picture` and `label` variables are not implemented.

> **Run the full example:** [`examples/data_types.pli`](../examples/data_types.pli) — `plic data_types.pli -o data_types && ./data_types`.

---

## Declarations & structures

`declare` introduces a name. Structures group related fields with level numbers: `1` is the top, `2` a member, `3` a sub-member. Think of a Python dataclass or a Go struct.

```pli
 dcl 1 employee,
       2 name   char(30) varying,
       2 id     fixed bin(31),
       2 salary fixed dec(7,2);

 employee.name = 'Alice';
 employee.salary = 75000.00;
```

Nesting goes as deep as you like:

```pli
 dcl 1 employee,
       2 name char(30) varying,
       2 address,
         3 street char(40) varying,
         3 city   char(25) varying;
 employee.address.city = 'Trenton';
```

### LIKE: copy a shape

`like` copies another structure's shape so you don't repeat yourself — like reusing a dataclass definition.

```pli
 dcl 1 template,
       2 id  fixed bin(31),
       2 tag char(8) varying;

 dcl 1 copy like template;
 copy.id = 7;
```

You can also narrow to a sub-structure with `like s.a.b` (a deep copy of that branch).

**Not yet**

- `like` of a non-structure, or `like` plus a dimension, is an error the compiler reports.

> **Run the full example:** [`examples/declarations_structures.pli`](../examples/declarations_structures.pli) — `plic declarations_structures.pli -o decls && ./decls`.

---

## Names for types & constants

Two small helpers keep code tidy.

`value` makes a named constant that can't be reassigned — like `const` in Go or an ALL_CAPS name in Python (but enforced!).

```pli
 dcl max fixed bin(31) value(100);
 dcl greeting char(20) varying value('hello');
 /* max = 5;  -- the compiler says no */
```

`define alias` names a reusable attribute set — like `type MyInt = int` in Go.

```pli
 define alias scores fixed bin(31);
 dcl midterm type scores;
 dcl final type scores;

 midterm = 88;
```

> **Beginner tip:** start with `value` for magic numbers (`max_retries`, `line_width`). Reach for `define alias` when three declarations share the same long attribute list.

> **Run the full example:** [`examples/names_types_constants.pli`](../examples/names_types_constants.pli) — `plic names_types_constants.pli -o names && ./names`.

---

## Arrays

Arrays look like most languages: declare a size, index from 1 by default (yes, 1 — you'll get used to it!).

### Fixed-size arrays

```pli
 dcl scores(5) fixed bin(31);
 dcl board(3,4) fixed bin(31);  /* 3 rows, 4 cols, row-major */

 scores(1) = 10;
 board(2,3) = 99;
```

Bounds can be explicit: `dcl temps(-10:40) fixed bin(31);` gives you index `-10` to `40`. Negative lower bounds work fine.

### Dynamic arrays: size decided at runtime

Here's the Python/Go-like part. A procedure can take its array size from a parameter — like a slice whose length you only know when the function is called.

```pli
 check: proc(n);
   dcl n fixed bin(31);
   dcl a(n) fixed bin(31);  /* sized when check is entered */

   dcl i fixed bin(31);
   do i = 1 to n;
     a(i) = i * 2;
   end;
 end check;  /* optionally, repeat the name of the procedure for readability */
```

The size is frozen when the procedure (or `begin` block) is entered. Ask about it with `lbound` / `hbound` / `dim` — that's PL/I's `len()`:

```pli
 put skip list(lbound(a));       /* first index, usually 1 */
 put skip list(hbound(a));       /* last index */
 put skip list(dim(a));          /* total element count */
```

> **Python friends:** `hbound(a)` is `len(a)` (minus the 0-vs-1 shift). `dim(a)` of a 2-D array is `rows * cols`.  
> **Go friends:** a dynamic array param is like receiving a slice with a known `len`.

Want a dynamic array in `main`? Read the size first, then enter a `begin` block — the block entry freezes the size:

```pli
 main: proc options(main);
   dcl n fixed bin(31);
   get list(n);

   begin;
     dcl values(n) fixed bin(31);
     /* ... use values here ... */
   end;
 end;
```

### Slices: cross-sections with `*`

An asterisk means "the whole axis" — like `:` in NumPy or Go slices.

```pli
 dcl a(3,5) fixed bin(31);
 a(2,*) = 0;        /* whole row 2, like a[1,:] = 0 in NumPy */

 dcl b(5) fixed bin(31);
 b = a(1,*);        /* copy row 1 into b */
```

A slice on the left writes element-by-element. A scalar on the right broadcasts:

```pli
 a(1,*) = 42;       /* fill row 1 with 42 */
```

### Whole-array math: leave the loop behind

This is the NumPy moment. `a + 1` means "every element plus 1":

```pli
 dcl a(100) fixed bin(31);

 dcl i fixed bin(31);
 do i = 1 to 100;
   a(i) = i;
 end;
 a = a * 2;         /* double every element, no loop */
```

Collapse an array with reductions — `sum` and `prod` are the builtin `sum()`:

```pli
 put skip list(sum(a));    /* 1+2+...+100 */
```

For `bit(1)` masks there are `any` and `all` (like Python's `any()` / `all()`).

**Not yet**

- Slices only on plain (non-based, non-defined) arrays for now.
- Both sides of an array assignment must have the same shape.
- Dynamic multi-axis arrays allow a dynamic *first* axis only, e.g. `a(n,4)`.

> **Run the full example:** [`examples/arrays.pli`](../examples/arrays.pli) — `plic arrays.pli -o arrays && ./arrays`.

---

## Operators & expressions

Precedence, lowest to highest: `||`, then `+ -`, then `* /`, then `**`, then prefix `+ - ^`. `^`, `~` and `¬` all mean "not". The words `and`/`or` spell `&`/`|` if you prefer reading them.

```pli
 dcl r fixed bin(31);
 r = -3 ** 2;      /* -(3**2) = -9, ** binds tightest */

 dcl s char(20) varying;
 s = 'a' || 'b' || 'c';   /* 'abc', || glues strings */

 if r < 0 then put skip list('negative');
```

### Gluing strings with `||`

`||` concatenates — that's `+` for strings in Python or Go. `!!` is an accepted alias; a lone `!` is an error.

```pli
 dcl tag char(20) varying;

 tag = 'user-' || '42';    /* 'user-42' */
 tag = 'user-' !! '42';    /* same thing */
```

Non-strings convert automatically (`char(42)` gives `'42'`), so `'n=' || 42` just works.

### Comparisons

The usual `> >= = < <=` plus `^=` / `¬=` for "not equal". Complex values only support `=` / `^=` part-wise — ordering complex numbers is an error, same as in Python.

**Not yet**

- `**` with complex exponents and a few edge cases are future work.

> **Run the full example:** [`examples/operators_expressions.pli`](../examples/operators_expressions.pli) — `plic operators_expressions.pli -o ops && ./ops`.

---

## Control flow

### IF / THEN / ELSE

Exactly what you expect:

```pli
 dcl score fixed bin(31);

 get list(score);

 if score >= 90 then 
   put skip list('A');
 else if score >= 80 then 
   put skip list('B');
 else if score >= 70 then 
   put skip list('C');
 else 
   put skip list('needs work');
```

### DO loops

```pli
 dcl total fixed bin(31);
 dcl i fixed bin(31);

 total = 0;
 do i = 1 to 10;
   total = total + i;
 end;

 do while (total < 1000);
   total = total * 2;
 end;
```

> Always declare your loop counter (`dcl i ...`). Undeclared names get an implicit type, which surprises beginners.

### BEGIN blocks are scopes

A `begin ... end` block is a real lexical scope — declarations inside don't leak out. Think `{ ... }` in Go.

```pli
 dcl total fixed bin(31);

 total = 0;
 begin;
   dcl subtotal fixed bin(31);

   subtotal = 10;
   total = total + subtotal;
 end;
 /* subtotal is gone here */
```

### SELECT: the friendly switch

`select` picks the first matching branch — like `match` in Python or `switch` in Go, but simpler (it lowers to an if-chain).

```pli
 dcl status char(10) varying value('pending');
 select (status);
   when ('pending') put skip list('waiting');
   when ('active')  put skip list('working');
   when ('done')    put skip list('finished');
   otherwise        put skip list('huh?');
 end;
```

Without an expression, each `when` is just a condition:

```pli
 select;
   when (score >= 90) put skip list('A');
   when (score >= 80) put skip list('B');
   otherwise put skip list('keep trying');
 end;
```

Rules: at least one `when`, `otherwise` (if present) goes last and only once.

### LEAVE / ITERATE and DO UNTIL

`leave` is `break`, `iterate` is `continue`. `do until` is a post-test loop — the body always runs once, like `do ... while` in C or a `while True: ... if cond: break` in Python.

```pli
 dcl i fixed bin(31);
 do i = 1 to 100;
   if i > 10 then leave;        /* break */
   if mod(i, 2) = 0 then iterate; /* continue: skip evens */

   put skip list(i);
 end;

 dcl step fixed bin(31) init(0);
 dcl done bit(1) init('0'b);

 do until (done);
   step = step + 1;
   if step >= 10 then done = '1'b;
 end;
```

### GO TO (use sparingly)

Local `go to` jumps to a label. You'll rarely need it now that `leave`/`iterate`/`select` exist, but it's there.

```pli
 dcl state fixed bin(31);
 state = 1;

 again: state = state + 1;
 if state < 10 then go to again;
```

Jumping *into* an inactive block is an error. Jumping to an enclosing procedure is future work.

> **Run the full example:** [`examples/control_flow.pli`](../examples/control_flow.pli) — `plic control_flow.pli -o flow && ./flow`.

---

## Procedures & functions

Top-level procedures link externally (upper-cased name). Nested procedures see the enclosing procedure's variables through a static link — like a closure that captures its surroundings.

```pli
 main: proc options(main);
   call helper(5);

   helper: procedure(n);
     dcl n fixed bin(31);
     put skip list('n =', n);
   end;
 end;
```

Nesting is for helpers that shouldn't pollute the global namespace.

### Functions with RETURNS

```pli
 add: procedure(a, b) returns(fixed bin(31));
   dcl (a, b) fixed bin(31);
   return(a + b);
 end;

 dcl r fixed bin(31);
 r = add(3, 4);     /* 7 */
```

A `return` with a value in a void procedure (or a bare `return` in a valued function) is an error — the compiler catches the mix-up.

### RECURSIVE: opt in

Every procedure in a call cycle must say `recursive`. Python and Go recurse by default; PL/I asks you to declare it so the compiler can set up fresh storage.

```pli
 fact: procedure(n) recursive returns(fixed bin(31));
   dcl n fixed bin(31);
   if n <= 1 then return(1);
   return(n * fact(n - 1));
 end fact;
```

Forget `recursive` and you'll get a clear diagnostic — add the word and move on.

### OPTIONAL parameters: like default args

Mark a trailing parameter `optional`. Callers can pass `*` or just leave it out, and the callee asks `omitted(p)` / `present(p)`. Think Python default arguments.

```pli
 greet: procedure(name, punct);
   dcl name char(20) varying;
   dcl punct char(5) varying optional;

   if omitted(punct) then 
     put skip list('hi ' || name);
   else 
     put skip list('hi ' || name || punct);
 end greet;

 call greet('sam', '!');
 call greet('sam', *);
 call greet('sam');       /* all fine */
```

### Multiple entry points

One body, several names, via `entry` — handy for related calculations that share state:

```pli
 area: procedure(d) returns(fixed bin(31));
   dcl d fixed bin(31);
   dcl r fixed bin(31);

   r = d * d;
   return(r);

   cube: entry(d) returns(fixed bin(31));
     r = d * d * d;
     return(r);
 end area;
```

`area(5)` is 25, `cube(5)` is 125.

> **Run the full example:** [`examples/procedures_functions.pli`](../examples/procedures_functions.pli) — `plic procedures_functions.pli -o procs && ./procs`.

---

## Packages: grouping code

A `package` groups procedures under one scope — like a Go package or a Python module. `exports(...)` picks what's visible outside; the rest stays private. Package-level data is shared by member procedures.

```pli
 mathlib: package exports(doubleit);
   doubleit: procedure(x) returns(fixed bin(31));
     dcl x fixed bin(31);
     return(helper(x) + x);
   end;

   helper: procedure(x) returns(fixed bin(31));
     dcl x fixed bin(31);
     return(x + 1);
   end;
 end;
```

> **Go friends:** `package ... exports(...)` is `package mathlib` plus capitalised (exported) names.  
> **Python friends:** it's a module where `exports` is your `__all__`.

> **Run the full example:** [`examples/packages.pli`](../examples/packages.pli) — `plic packages.pli -o pkgs && ./pkgs`.

---

## First-class procedures

An `entry ... variable` holds a procedure value — like a function variable in Go or a Python callable. Assign different procedures, pass them to dispatchers as callbacks.

```pli
 dcl on_get entry(fixed bin(31))
   returns(fixed bin(31)) variable;

 on_get = add1;             /* point at a function */
 put skip list(on_get(41)); /* call through it: 42 */

 on_get = dbl;              /* re-point, now 82 */
 put skip list(on_get(41));
```

Callbacks are just parameters with the same shape:

```pli
 dispatch: procedure(cb) returns(fixed bin(31));
   dcl cb entry(fixed bin(31))
     returns(fixed bin(31)) variable;

   return(cb(41));
 end;

 put skip list(dispatch(add1));  /* 42 */
```

> **Python friends:** `on_get = add1` is `on_get = add1` — then `on_get(41)` calls it. Same idea.  
> **Go friends:** think `var onGet func(int) int`.

> **Run the full example:** [`examples/first_class_procedures.pli`](../examples/first_class_procedures.pli) — `plic first_class_procedures.pli -o firstcls && ./firstcls`.

---

## Storage: BASED, CONTROLLED, ALLOCATE/FREE

> Beginner tip: you can skip this on first read. Automatic storage (plain `dcl`) covers most programs. Come back when you want linked lists, pools, or manual lifetimes.

### BASED: data behind a pointer

A `based(p)` structure has no storage of its own — it borrows it through pointer `p`. That's how you build lists, trees, graphs. `p -> rec.field` is the explicit "through this pointer" form.

```pli
 dcl 1 target,
       2 value fixed bin(31);

 dcl p pointer;
 dcl 1 view based(p),
       2 value fixed bin(31);

 p = addr(target);       /* p points at target */
 view.value = 42;        /* writes target.value */

 put skip list(target.value);    /* 42 */
 put skip list(p -> view.value); /* same, explicit form */
```

> **Go friends:** `based(p)` is like a struct accessed through a pointer, and `addr(x)` is `&x`.

### CONTROLLED: a stack of generations

`controlled` keeps a last-in-first-out stack of values. `allocate` pushes a fresh one, plain references see the newest, `free` pops. The declaration pushes a default generation on entry so first use just works.

```pli
 dcl c fixed bin(31) controlled;

 allocate c;    /* generation 1 */
 c = 11;

 allocate c;    /* generation 2 */
 c = 22;

 free c;        /* back to 11 */
```

### ALLOCATE / FREE on the heap

With `set`, `allocate` hands you a fresh heap block in a pointer:

```pli
 dcl p pointer;
 dcl 1 rec based(p),
       2 data fixed bin(31);

 allocate rec set(p);   /* fresh block, p points at it */
 p -> rec.data = 99;

 free rec;              /* release it */
```

**Not yet**

- `set` on a controlled allocate warns and is ignored (it's stack-managed, not heap).
- A few exotic combinations (dynamic-extent character members mid-structure, `bit(n>1)` controlled arrays in spots) are diagnosed.

> **Run the full example:** [`examples/storage_based_controlled.pli`](../examples/storage_based_controlled.pli) — `plic storage_based_controlled.pli -o storage && ./storage`.

---

## AREA & OFFSET regions

An `area` is a memory pool for `based` allocation; an `offset` is an opaque locator into one. Blocks come out of the pool and free in any order with reuse — like an arena allocator in Go or a quick scratch buffer you bulk-free.

```pli
 dcl region area(4096);
 dcl pa offset(region);
 dcl 1 slot based(pa),
       2 v fixed bin(31);

 allocate slot in(region) set(pa);
 slot.v = 11;

 free slot in(region);
 pa = null();
```

**Not yet**

- Static/external areas, `area(*)`, arrays of areas/offsets, and offset arithmetic are diagnosed.

> **Run the full example:** [`examples/area_offset.pli`](../examples/area_offset.pli) — `plic area_offset.pli -o arena && ./arena`.

---

## DEFINED & iSUB overlays

`defined` makes one variable share another's storage — a zero-cost view, no copy. Like a `union` in C or `memoryview` in Python.

```pli
 dcl x fixed bin(31);
 dcl y fixed bin(31) defined x;  /* same memory as x */

 x = 7;
 put skip list(y);               /* 7 */
```

Overlay with a different layout to reinterpret bytes — two halves over one word:

```pli
 dcl word fixed bin(31);
 dcl 1 halves defined word,
       2 hi fixed bin(31),
       2 lo fixed bin(31);

 word = 16#00420001#;
 put skip list(halves.hi, halves.lo);
```

iSUB builds a sliding view into one axis of an array without copying — like a slice that stays linked to the original:

```pli
 dcl x(10,10) fixed bin(31);
 dcl r(10) fixed bin(31) defined x(1, 1sub);

 x(1,1) = 5;
 put skip list(r(1));            /* 5, tracks x */
```

Affine forms like `x(m*1sub + c)` are served too.

**Not yet**

- `position` is an error; at most one iSUB per overlay; the base must be in the same or an enclosing scope.

> **Run the full example:** [`examples/defined_isub.pli`](../examples/defined_isub.pli) — `plic defined_isub.pli -o overlays && ./overlays`.

---

## Conditions: ON / SIGNAL / REVERT

PL/I has resumable conditions with dynamic scope — think `try/except` in Python, except the handler can fix things and *resume* right after the faulting statement.

```pli
 on error begin;
   put skip list('caught an error');
 end;

 signal error;          /* run the handler, then resume */

 put skip list('resumed');
 revert error;          /* pop the handler */
```

Supported: `error`, `size`, `subscriptrange`, `zerodivide`, `conversion`, plus your own named conditions.

Bounds-checking example — the handler runs, then execution resumes with the index clamped:

```pli
 dcl a(5) fixed bin(31);
 dcl i fixed bin(31);

 on subscriptrange put skip list('bad index');

 i = 0;
 put skip list(a(i));   /* traps, prints, resumes as a(1) */
```

`size` catches arithmetic overflow (binary, decimal wrap, float-to-fixed). Great for money code where silent wrap would be a bug:

```pli
 dcl total fixed dec(7,2);
 dcl bump fixed dec(7,2);

 bump = 99999.99;

 on size begin;
   put skip list('would overflow, capping');
   total = 99999.99;
 end;

 total = total + bump;
 revert size;
```

### One-statement opt-outs

Prefixes like `(nosize)` / `(nosubscriptrange)` / `(nozerodivide)` / `(noconversion)` skip the check for one statement when you've already validated it:

```pli
 (nosubscriptrange): a(i) = 0;  /* trust me, i is fine */
```

> **Python friends:** `on ... / signal ...` is `try: ... except: ...` that resumes instead of jumping to the except's end. `revert` is leaving the `with` block.

**Not yet**

- `check` conditions, `on return`, `go to` inside a unit, and `return(value)` inside a handler are diagnosed.

> **Run the full example:** [`examples/conditions.pli`](../examples/conditions.pli) — `plic conditions.pli -o conds && ./conds`.

---

## Concurrency: TASK, EVENT, WAIT, DELAY

`call ... event(e)` runs a procedure on a background thread. `wait(e)` blocks until it's done; `event(e)` polls it. Think goroutines + `WaitGroup`, or Python threads + `join()`.

```pli
 dcl done event;
 dcl counter fixed bin(31) init(0);

 call worker event(done);
 wait(done);                       /* join the worker */

 if counter = 1 then put skip list('PASS');

 worker: procedure;
   counter = counter + 1;
 end worker;
```

Overlap work while the background task runs, then join:

```pli
 dcl done event;
 dcl total fixed bin(31);

 call background_sum event(done);
 /* ... do other work here ... */
 wait(done);

 put skip list('sum =', total);

 background_sum: procedure;
   dcl i fixed bin(31);

   do i = 1 to 1000000;
     total = total + i;
   end;
 end background_sum;
```

Poll instead of blocking with the `event()` built-in (`'1'b` means done):

```pli
 do while (^event(done));
   put skip list('still working...');
   delay(50);       /* sleep 50 ms */
 end;
```

Also: `call ... task[(t)]` requests an async task (`priority(p)` is accepted but ignored for now); task/event arrays, elements `ev(i)`, and members `s.ev` work.

> **Go friends:** `call f event(e); ... wait(e);` is `go f(); ... wg.Wait()`. `event(e)` polling is a non-blocking channel check.

**Not yet**

- You must `wait` before the block exits, or the event's storage may vanish while the task still runs. External and function-async tasks are diagnosed.

> **Run the full example:** [`examples/concurrency.pli`](../examples/concurrency.pli) — `plic concurrency.pli -o conc && ./conc`.

---

## Preprocessor

`%include` pulls in another file; `%xinclude` is the include-once form for shared headers.

```pli
 %xinclude common, io;  /* includes files common.inc, io.inc */
```

Centralise widths and layouts in an include:

```pli
 /* config.inc */
 %declare max_records value(1000);
 %activate max_records;

 %include 'config.inc';

 dcl table(%max_records) char(80) varying;
```

`%if / %then / %else` with `%declare` / `%activate` selects code — like build tags or feature flags for debug vs. release:

```pli
 %declare debug value(1);
 %activate debug;
 %if debug = 1 %then put skip list('debug is on');
 %else put skip list('debug is off');
```

`%replace name by <tokens>;` substitutes an identifier with source text before parsing.

### Where includes are found

1. The including file's directory.
2. Repeatable `-I` flags (first match wins).
3. `PLIC_INCLUDE_PATH` (colon-separated, like `CPATH`).
4. `include/` and `inc/` found by walking up from the source and from the `plic` executable.
5. Static system paths (`/usr/include`, `/usr/local/include`) on Linux/macOS.
6. Executable-relative `share/plic/include`.

Use `-v` to list the configured paths.

**Not yet**

- `%do`, `%go to`, and preprocessor procedures parse but report "unsupported".

> **Run the full example:** [`examples/preprocessor.pli`](../examples/preprocessor.pli) (with `preprocessor_defs.inc` next to it) — `plic preprocessor.pli -o pp && ./pp`.

---

## Input & output

### List-directed: the easy path

`put list(...)` prints, `get list(...)` reads. `skip` starts a new line. This is `print` / `input().split()` — order matters, formatting is automatic.

```pli
 dcl name char(20) varying;
 dcl count fixed bin(31);

 put skip list('Enter name:');
 get list(name);

 put skip list('Hello,', name);
```

`get` reads stdin (`sysin`), `put` writes stdout (`sysout`). Redirect from a file and it just works. Whole arrays transmit element-by-element in row-major order.

`string(ref)` routes output into (or input from) a character variable instead of the terminal — like `io.StringIO` in Python:

```pli
 dcl buf char(100) varying;
 put string(buf) list('n=', 42);
```

### Edit-directed: precise columns

`put edit ... (...)` pairs each value with a format item — like `printf` / `format()`. Use it for reports and fixed-width files.

```pli
 dcl a fixed bin(31);
 dcl b char(4) varying;

 a = 123;

 put edit(a) (f(6));      /* right-justified, 6 wide */
 get edit(b) (a(4));      /* 4-char field */
```

Served items: `f(w,d)`, `e(w,d)`, `a(w)`, `x(w)`, `skip(n)`, `page`, `line(n)`, `b(n)`, `c(re,im)`, `col(n)`, and `(n)(...)` repetition groups. Labelled `format` statements plus remote `r(label)` splicing work too.

### DATA-directed: self-describing dumps

`put data(x, y)` prints `name=value` pairs; `get data` reads them in any order and skips unknown names. Great for configs and debugging — like `repr()` of your variables.

```pli
 dcl 1 config,
       2 host char(20) varying,
       2 port fixed bin(31);

 get data(config);   /* reads host='...', port=8080 */
 put data(config);   /* echoes it back */
```

### Files & records

```pli
 dcl f file;

 open file(f) title('data.txt') output;
 put file(f) list('hello');
 close file(f);

 open file(f) record sequential input
   title('rec.dat');

 read file(f) into(a);
 write file(f) from(a);
 close file(f);
```

`display(x)` prints one value plus a newline (handy for quick debugging).

**Not yet**

- `record`/`update`/`keyed` open options, `rewrite`/`delete`/`locate`/`unlock`, and `on endfile` are diagnosed. Record I/O is fixed-size binary only.

> **Run the full example:** [`examples/input_output.pli`](../examples/input_output.pli) — `plic input_output.pli -o io && ./io` (writes `io_demo.txt` in the current directory).

---

## A tour of built-ins

Built-ins are ordinary names (case-insensitive) recognised after parsing — see the [Built-ins guide](./guides/builtins.md) for the full reference. If you know Python's `str` methods and `math` module, you know most of these.

Strings (think `str` methods):

```pli
 dcl email char(40) varying;

 email = 'user@example.com';

 if index(email, '@') > 0 then  /* '@' in email */
   put skip list('valid');

 put skip list(length(email));     /* len(email) */
 put skip list(substr(email, 1, 4));  /* email[0:4] */
 put skip list(trim('  hi  '));    /* 'hi'.strip() */
 put skip list(uppercase(email));  /* .upper() */
```

Numbers and math (think `math` + `sum`):

```pli
 put skip list(abs(-7));        /* 7 */
 put skip list(mod(17, 5));     /* 2, like % */
 put skip list(min(3, 8, 1));   /* 1 */
 put skip list(sqrt(9.0));      /* 3.0 */
 put skip list(sum(a));         /* sum(a) */
```

Conversions (think `str()` / `int()`):

```pli
 dcl s char(24) varying;
 s = char(42);          /* '42' */

 dcl n fixed bin(31);
 n = fixed('1234');     /* 1234 */
 n = fixed(trim(input));/* parse after stripping blanks */
```

Dates, addresses, and friends: `date()` gives `YYYYMMDD`, `time()` gives `HHMMSS`; `addr(x)` is `&x` and `null()` is `None`/`nil` for pointers.

> **Run the full example:** [`examples/builtins_tour.pli`](../examples/builtins_tour.pli) — `plic builtins_tour.pli -o bitour && ./bitour`.

---

## C interoperability

Call C like `ctypes` (Python) or `cgo` (Go). Declare the C function as an `entry` with `options(linkage(system))` (or `byvalue`), then call it. PL/I passes scalars by reference by default; the linkage option marshals them as C values.

```pli
 dcl c_add entry(fixed bin(31), fixed bin(31))
   returns(fixed bin(31))
   options(linkage(system)) external('c_byval_add');

 dcl r fixed bin(31);
 r = c_add(3, 4);      /* calls the C function */
```

```c
 int c_byval_add(int a, int b) { return a + b; }
```

`byaddr(x)` passes one structure argument by address so the callee's writes are visible (otherwise structures copy by value):

```pli
 call proc(byaddr(mysize));
```

`char(n) varyingz` arrives at a by-value C entry as a bare NUL-terminated `char *` — no length juggling.

**Not yet**

- Only scalar and pointer C parameters by value for now; structure-valued C returns via the hidden-buffer convention are diagnosed.

> **Run the full example:** [`examples/c_interop.pli`](../examples/c_interop.pli) with [`examples/c_interop_helper.c`](../examples/c_interop_helper.c) — `clang -c c_interop_helper.c && plic c_interop.pli c_interop_helper.o -o cinterop && ./cinterop`.

---

*Next steps: [grammar coverage*](./docs/GRAMMAR-COVERAGE.md) f*or rule-by-rule status and *`tests/`* for runnable examples behind each feature.*
