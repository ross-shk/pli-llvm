# PL/I Programming Guide — plic

A tour of the PL/I features `plic` supports today. Each section gives a short description, a runnable code snippet, and a "Limitations" note for what is *not* implemented.

> The `.pli` examples below follow one leading space, margins 2–72, modern lowercase style. Run any of them with `plic name.pli -o name && ./name`.

- [Quick reference](#quick-reference)
- [Getting started](#getting-started)
- [Data types](#data-types)
- [Declarations & structures](#declarations--structures)
- [Arrays](#arrays)
- [Operators & expressions](#operators--expressions)
- [Control flow](#control-flow)
- [Procedures & functions](#procedures--functions)
- [Storage: BASED, CONTROLLED, ALLOCATE/FREE](#storage-based-controlled-allocatefree)
- [AREA & OFFSET regions](#area--offset-regions)
- [DEFINED & iSUB overlays](#defined--isub-overlays)
- [Conditions: ON / SIGNAL / REVERT](#conditions-on--signal--revert)
- [Concurrency: TASK, EVENT, WAIT, DELAY](#concurrency-task-event-wait-delay)
- [Preprocessor](#preprocessor)
- [Input & output](#input--output)
- [C interoperability](#c-interoperability)

---

## Quick reference

| Topic      | What works                                                       | What doesn't                       |
| ---------- | ---------------------------------------------------------------- | ---------------------------------- |
| Program    | `PROCEDURE OPTIONS(MAIN)` entry point                            | —                                  |
| Types      | FIXED BIN/DEC, FLOAT, COMPLEX, BIT, CHAR/VARYING, POINTER        | STERLING constants, PICTURE type   |
| Arrays     | fixed & dynamic bounds, cross-sections `A(i,*)`, reductions      | `BIT(n>1)` arrays in some contexts |
| Storage    | BASED, CONTROLLED, DEFINED with iSUB, AREA/OFFSET                | dynamic-extent BASED arrays        |
| Conditions | ON/SIGNAL/REVERT: ERROR, SIZE, SUBSCRIPTRANGE, ZERODIVIDE, named | CONVERSION, OVERFLOW, CHECK        |
| I/O        | PUT/GET LIST, EDIT, DATA, STRING, FILE, OPEN/CLOSE, RECORD       | KEYED/DIRECT record                |
| Built-ins  | string, math, array/pointer, misc (see `builtins.md`)            | —                                  |
| Extensions | SELECT, LEAVE/ITERATE, DO UNTIL, TRIM, PACKAGE, OPTIONAL         | —                                  |

---

## Getting started

A PL/I program is one or more procedures. The one marked `OPTIONS(MAIN) `is where execution starts.

```pli
hello: procedure options(main);
  put skip list('Hello, world!');
end hello;
```

There is no header file or import needed. Save as `hello.pli`, then:

```bash
plic hello.pli -o hello
./hello
```

> **No reserved words.** `IF`, `THEN`, `PUT`, `RETURN`, etc. are ordinary identifiers by default; the parser recognises them as keywords only by position. This means you can name a variable `PUT` in data position, though it is usually a bad idea.

PL/I is designed for systems and business programming. Its type inference, array operations, and condition handling let you write concise yet safe code for numeric processing, data parsing, and systems-level work.

---

## Data types

PL/I infers a type when none is given: `FIXED BINARY` for names whose initial letter is I–N, otherwise `FLOAT DECIMAL`. You can always declare explicitly.

```pli
dcl x fixed bin(31);       /* 31-bit signed integer */
dcl d fixed dec(9,2);      /* 9 digits, 2 after the decimal point */
dcl f float dec(6);        /* double, 6 significant digits in DISPLAY */
dcl c char(10);            /* 10-byte character buffer, blank padded */
dcl v char(10) varying;    /* length prefix + up to 10 bytes */
dcl b bit(1);              /* single bit, '1'b / '0'b */
dcl z char(20) varyingz;   /* NUL-terminated (IBM extension) */
dcl p pointer;             /* an address */
```

**Decimal detail.** `FIXED DECIMAL(p,q)` is a scaled integer: the value is stored as if multiplied by 10^q. Arithmetic (`+ - *`) honors the scale with round-half-away; overflow traps to `SIZE` when a handler is present.

```pli
dcl price fixed dec(5,2);
price = 12.34;            /* stored as integer 1234, scale 10^2 */
```

Use fixed-decimal for money or measurements where exact decimal arithmetic matters — floats would introduce rounding errors.

**Limitations**

- `FIXED BINARY(p)` precision is 31 or 63 bits; mixed widths widen to the  
larger operand.
- `/` and `**` use floating-point even for fixed operands; exact fixed  
division is a future milestone.
- `BIT(n)` arrays with `n > 1` in expressions are errors in this stage.
- `PICTURE` and `LABEL` are not implemented.

---

## Declarations & Structures

`DECLARE` (`DCL` for short) introduces a name and its attributes. Structures use level numbers: `1` is the top, `2` a member, and so on. Structures let you group related fields — think of a record or a C `struct`.

```pli
dcl 1 employee,
      2 name      char(30),
      2 id        fixed bin(31),
      2 salary    fixed dec(7,2),
      2 address,
        3 street  char(40),
        3 city    char(25),
        3 zip     char(10);

employee.name = 'Alice';
employee.salary = 75000.00;
employee.address.city = 'Trenton';
```

In a business context, you might model a customer, an invoice line item, or a configuration record this way.

### LIKE

`LIKE` copies a structure's shape onto a new declaration, so you don't have to repeat the field layout — handy when you need multiple variables with the same record type.

```pli
dcl 1 template,
      2 id   fixed bin(31),
      2 tag  char(8);

dcl 1 copy like template;      /* same shape as template */
copy.id = 7;
```

Use this when you have a "master" structure definition and want typed local copies or parameters that match it exactly.

**Limitations**

- `LIKE` of a non-structure or an undeclared template is an error.
- Combining `LIKE` with a dimension is an error.

---

## Arrays

### Fixed-size arrays

```pli
dcl a(5) fixed bin(31);
dcl m(3,4) fixed bin(31);   /* 3×4, row-major */

a(1) = 10;
m(2,3) = 99;
```

### Dynamic arrays

An `AUTOMATIC` array whose bound is a runtime expression is sized when the procedure is entered. Use `LBOUND`/`HBOUND`/`DIM` to query the *live *bounds at runtime.

```pli
dcl n fixed bin(31);

n = 8;
dcl dyn(n) fixed bin(31);       /* size fixed at entry */

do i = 1 to n;
  dyn(i) = i * 2;
end;
```

A realistic use — read a count, then allocate and fill a list:

```pli
dcl count fixed bin(31);
get list(count);

dcl values(count) fixed bin(31);

do i = 1 to count;
  get list(values(i));
end;
```

### Cross-sections

An asterisk in a subscript stands for the whole axis of that dimension:

```pli
dcl a(3,5) fixed bin(31);
a(2,*) = 0;          /* the whole 2nd row */

dcl b(5) fixed bin(31);
b = a(1,*);          /* copy row 1 into b */
```

A cross-section on the left side of an assignment writes element-by-element into the slice — useful for bulk initialisation:

```pli
a(1,*) = 42;          /* fill entire row 1 with 42 */
```

**Limitations**

- Cross-sections only on plain (non-BASED, non-DEFINED) arrays in this stage.
- `CHARACTER(n)` element arrays have size restrictions.
- Dynamic multi-axis arrays where a non-first axis is dynamic are errors.

---

## Operators & expressions

Precedence, from lowest to highest: `||`, `+ -`, `* /`, `** `(exponentiation), then prefix `+ - ¬`. `^` and `~` are the not-sign besides `¬`. `and`/`or` spell `&`/`|` when the option words are used.

```pli
dcl r fixed bin(31);
r = -3 ** 2;        /* -(3**2) = -9 : ** binds tighter than unary minus */

dcl s char(10);
s = 'a' || 'b' || 'c';    /* concatenation */

if r < 0 then put skip list('negative');
```

### Concatenation (`||`) and the `!!` alias

`||` is the standard concatenation operator. `!!` is an accepted alias; a lone `!` is an error:

```pli
dcl tag char(20) varying;

tag = 'user-' !! '42';     /* same as 'user-' || '42' */
put skip list(tag);
```

### Comparison

Standard relations (`> >= = < <= ¬= ¬> ¬<`), plus part-wise `=`/`^=` for `COMPLEX`. Ordered comparison of complex values is an error.

### Whole-array expressions

An array expression like `a + 1` is expanded element-wise over the array's shape, so you can transform every element in one statement:

```pli
dcl a(100) fixed bin(31);

do i = 1 to 100; a(i) = i; end;

a = a * 2;          /* double every element */
```

**Limitations**

- Target and value must be the same static shape on assignment.
- `**` for complex exponents and a few edge cases are future milestones.

---

## Control flow

### IF / THEN / ELSE

```pli
dcl score fixed bin(31);

get list(score);

if      score >= 90 then put skip list('A');
else if score >= 80 then put skip list('B');
else if score >= 70 then put skip list('C');
else put skip list('needs work');
```

### DO groups

```pli
dcl sum fixed bin(31);

sum = 0;
do i = 1 to 10;
  sum = sum + i;
end;

do while (input ^ = '');   /* read until blank */
  process(input);
end;
```

### BEGIN blocks

A `BEGIN ... END` block is a real lexical scope: declarations inside it do not leak to the enclosing procedure. This is handy for temporary variables that are only relevant inside a small region.

```pli
dcl total fixed bin(31);

total = 0;
begin;
  dcl subtotal fixed bin(31);

  subtotal = items(1) + items(2);
  total = total + subtotal;
end;
```

### GO TO

Local `GO TO` jumps to a label in the same or an enclosing procedure. Jumping out of a `BEGIN` block reverts that block's `ON`-units.

```pli
dcl state fixed bin(31);

state = 1;

again: procedure;
  state = state + 1;
  if state < 10 then go to again;
end again;
```

**Limitations** — `GO TO` *into* an inactive block is an error; non-local `GO TO` to an enclosing procedure is a deferred milestone.

### SELECT

```pli
dcl status char(10) value('pending');

select(status);
  when('pending')  put skip list('waiting for data');
  when('active')   put skip list('processing');
  when('done')     put skip list('complete');
  otherwise        put skip list('unknown state');
end;
```

### LEAVE / ITERATE and DO UNTIL

`LEAVE` exits a loop early; `ITERATE` skips to the next iteration. `DO UNTIL `is a post-test loop (the body runs, then the condition is checked).

```pli
dcl i fixed bin(31);

do i = 1 to 100;
  if i > 10 then leave;       /* exit the loop */
  if i mod 2 = 0 then iterate; /* skip even numbers */

  put skip list(i);
end;

do until (done);
  step = step + 1;
  done = (step >= 10);
end;
```

---

## Procedures & functions

### External and internal procedures

A top-level procedure is externally linked under its upper-cased name. Internal procedures are nested and reach enclosing automatic storage via a static link. Nesting lets you factor helper logic without polluting the global namespace.

```pli
main: procedure options(main);
  call helper(5);

  helper: procedure(n);
    declare n fixed bin(31);
    put skip list('n =', n);
  end helper;
end main;
```

A real-world pattern — process a list with a nested helper that carries captured state:

```pli
process_file: procedure options(main);
  dcl line char(80) varying;
  dcl total fixed bin(31) init(0);

  read_loop: procedure;
    do while (get_line(line) > 0);
      total = total + length(line);
      end;
  end;

  call read_loop;
  put skip list('total chars:', total);

  get_line: procedure(result) returns(fixed bin(31));
   dcl result char(*) varying;
    /* read one line from stdin */
  end;
end;
```

### Function procedures with RETURNS

```pli
add: procedure(a, b) returns(fixed bin(31));
  declare a fixed bin(31);
  declare b fixed bin(31);

  return(a + b);
end;

dcl r fixed bin(31);

r = add(3, 4);     /* r = 7 */
```

### RECURSIVE

Every procedure in a static call cycle must declare `RECURSIVE`.

```pli
fact: procedure(n) recursive returns(fixed bin(31));
  declare n fixed bin(31);

  if n <= 1 then return(1);
  return(n * fact(n-1));
end;
```

**Limitations**

- A `RETURN` with a value in a void procedure, or a plain `RETURN` in a valued function, is an error.
- Truly mixed return types within an alternate-entry body are errors.

### Multiple entry points

One body, several entry names, or alternate entries with their own parameters via the `ENTRY` statement:

```pli
area: procedure(d) returns(fixed bin(31));
  declare d fixed bin(31);
  declare r fixed bin(31);

  r = d * d;
  return(r);

  cube: entry(d) returns(fixed bin(31));
    r = d * d * d;
    return(r);
end;
```

Here `area(5)` returns 25, while `cube(5)` (an alternate entry of the same procedure) returns 125 — useful for related calculations that share state.

---

## Storage: BASED, CONTROLLED, ALLOCATE/FREE

### BASED

A `BASED(P)` structure has no storage of its own; its members are addressed through the `POINTER P`. The locator-qualified form `P -> REC.A` does the same as `REC.A`. BASED is how you build pointer-linked data structures — lists, trees, graphs — where storage is allocated dynamically.

```pli
dcl 1 target,
      2 value fixed bin(31);
dcl p pointer;
dcl 1 rec based(p),
      2 value fixed bin(31);

p = addr(target);         /* point p at target's storage */
rec.value = 42;           /* writes target.value */

put skip list(target.value);   /* prints 42 */
put skip list(p -> rec.value); /* the locator form also reads 42 */
```

### CONTROLLED

`CONTROLLED` uses a generation stack (LIFO). `ALLOCATE` pushes a fresh generation, references address the latest, and `FREE` pops it. A declaration pushes a default generation on procedure entry so a first-use reference needs no explicit `ALLOCATE`.

```pli
dcl c fixed bin(31) controlled;

allocate c;              /* push generation 1 */
c = 11;

allocate c;              /* push generation 2 */
c = 22;

free c;                  /* pop generation 2; c is back to 11 */
```

A recursive procedure uses `CONTROLLED` naturally — each call pushes a new generation, and `FREE` on return cleans up:

```pli
walk: procedure recursive;
  dcl node fixed bin(31) controlled;

  allocate node;

  node = node_value;
  if has_children then call walk;

  free node;
end;
```

### ALLOCATE / FREE

```pli
dcl 1 rec based(p),
      2 data fixed bin(31);

allocate p;                       /* p points at a fresh heap block */
p -> rec.data = 99;

free p;                           /* release the block */
```

With `SET`, `ALLOCATE` stores the address in a pointer or offset: `allocate c set(p)`.

**Limitations**

- A `SET` option on a `CONTROLLED` allocate warns and is ignored (stack-managed).
- Dynamic-extent `CHARACTER` structure members that are not the trailing member are errors.
- `BIT(n>1)` CONTROLLED arrays in some contexts are errors.

---

## AREA & OFFSET regions

`AREA` is a region for `BASED` allocation. `OFFSET` is an opaque locator into an area. Blocks are taken from the region and freed out of order, with bytes reused. This is useful for custom memory pools — for example, a scratch area for temporary buffers that you want to bulk-free.

```pli
dcl region area(4096);
dcl 1 a based(pa),
      2 v fixed bin(31);
dcl pa offset(region);

allocate a in(region) set(pa);   /* take from the region */
a.v = 11;

free a in(region);               /* return out of order; reusable */
pa = null();                     /* offsets are pointer-like */
```

**Limitations**

- Static/external areas, `AREA(*)`, arrays of AREA/OFFSET, and offset arithmetic are not implemented.
- An `OFFSET` of a non-AREA variable is an error.

---

## DEFINED & iSUB overlays

`DEFINED` makes one variable share another's storage — a memory view without its own storage. This is like a `union` in C or a `reinterpret_cast`: you can view the same bytes at different types or with different structure layouts.

```pli
dcl x fixed bin(31);
dcl y fixed bin(31) defined x;   /* y and x are the same memory */

x = 7;
put skip list(y);                 /* 7 */
```

A real-world use — overlay a 32-bit integer with two 16-bit halves:

```pli
dcl word fixed bin(31);
dcl 1 halves defined word,
      2 hi fixed bin(31),
      2 lo fixed bin(31);

word = 16#00420001#;

put skip list(halves.hi, heads.lo);   /* 66, 1 */
```

iSUB (`integer SUB`) names an axis of a subscripted base. The notation `1SUB`, `2SUB`, etc. overlays one axis of `X`. This is how you build sliding-window views into a larger array without copying:

```pli
dcl x(10,10) fixed bin(31);
dcl r(10) fixed bin(31) defined x(1, 1sub);  /* row 1 of x */

x(1,1) = 5;

put skip list(r(1));                          /* 5 */
```

Affine iSUB index arithmetic like `X(m*1SUB + c)` is served too.

**Limitations**

- `POSITION` is an error.
- More than one iSUB on the same overlay is an error.
- The base must be declared in the same or an enclosing scope.

---

## Conditions: ON / SIGNAL / REVERT

PL/I conditions have dynamic scope and can *resume* after a handler runs. A handler is established with `ON condition unit`, raised with `SIGNAL`, and popped with `REVERT`. Conditions let you write defensive code that recovers gracefully from errors — for example, handling an arithmetic overflow instead of crashing.

```pli
on error begin;
  put skip list('caught an error');
end;

signal error;              /* runs the handler, then resumes here */

put skip list('resumed');
revert error;
```

Supported conditions: `ERROR`, `SIZE`, `SUBSCRIPTRANGE`, `ZERODIVIDE`, and also programmer-named conditions.

```pli
dcl a(5) fixed bin(31);

on subscriptrange put skip list('bad index');

dcl i fixed bin(31) init(0);

put skip list(a(i));         /* traps, handler runs, resumes clamped to a(1) */
```

`SIZE` is the condition for arithmetic overflow — it catches `FIXED BINARY `overflow, `FIXED DECIMAL` wrap, and out-of-range float-to-fixed conversions. This is essential in financial code where an overflow could mean lost money:

```pli
dcl total fixed dec(7,2);
dcl increment fixed dec(7,2);

increment = 99999.99;
on size begin;
  put skip list('total would overflow; capping');
  total = 99999.99;
end;

total = total + increment;
revert size;
```

### Condition prefixes

Prefixes like `(NOSIZE)`, `(NOSUBSCRIPTRANGE)`, `(NOZERODIVIDE)` disable the matching check for a single statement:

```pli
(nosubscriptrange) a(i) = 0;   /* no bounds check this statement */
```

Use these when you have already validated the index and want to skip the runtime check for performance.

**Limitations**

- `CONVERSION`, `OVERFLOW`, `STRINGRANGE`, and `UNDERFLOW` conditions are accepted as prefixes but only produce an unenforced warning — no runtime trap is raised.
- `CHECK` conditions are rejected with a diagnostic.
- `ON RETURN`, `GO TO` inside a unit, and `RETURN(value)` inside a handler are errors.

---

## Concurrency: TASK, EVENT, WAIT, DELAY

`CALL ... EVENT(e)` runs a procedure asynchronously on a detached thread. `WAIT(ev)` suspends until the event completes; `EVENT(ev)` polls it as a bit. This lets you overlap computation or I/O — for example, reading from two sensors at once.

```pli
dcl done event;
dcl counter fixed bin(31);

counter = 0;

call worker event(done);
wait(done);                     /* wait for the task to finish */

if counter = 1 then put skip list('PASS');

worker: procedure;
  counter = counter + 1;
end worker;
```

A real-world example — compute a running sum in the background while the  
main task does other work:

```pli
dcl done event;
dcl result fixed bin(31);
call background_sum event(done);
/* main task: read user input, update display, etc. */
wait(done);
put skip list('sum =', result);

background_sum: procedure;
    dcl i fixed bin(31);
    do i = 1 to 1000000;
        result = result + i;
    end;
end background_sum;
```

- `CALL ... TASK[(t)]` requests an async task; `PRIORITY(p)` is accepted  
but currently ignored.
- `TASK`/`EVENT` arrays and members are supported; external and function-async  
tasks are errors in this stage.
- `DELAY(n)` sleeps `n` milliseconds.

**Limitations**

- You must `WAIT` before a block exits, or the event's storage goes out of  
scope while the task may still run.
- `PRIORITY` as a built-in (not a statement option) is an error.

---

## Preprocessor

`%INCLUDE` pulls in another file. `%XINCLUDE` is the include-once list form  
 — handy for shared headers that should not be double-defined.

```pli
%xinclude 'common.inc', 'io.inc';   /* each file expands once */
```

A realistic use — centralise column widths and record layouts:

```pli
/* config.inc */
%dcl max_records fixed bin(31) value(1000);
%dcl rec_len fixed bin(31) value(80);

%include 'config.inc';
dcl table(max_records) char(rec_len);
```

`%IF / %THEN / %ELSE` and `%DECLARE`/`%ACTIVATE` parameterise includes  
 — useful for conditional builds (debug vs. release):

```pli
%declare debug;
%if debug %then
%activate debug;
```

`%REPLACE name BY <tokens>;` substitutes an identifier with source text  
before parsing.

**Limitations**

- `%DO`, `%GO TO`, and procedures are stubs: accepted syntactically but  
diagnosed as unsupported.

---

## Input & output

### List-directed I/O

`PUT` and `GET` transmit whole data lists. `SKIP` emits a blank line;  
`STRING(ref)` routes output into a character variable.

```pli
dcl name char(20) varying;
dcl count fixed bin(31);
put skip list('Enter name:');
get list(name);
put skip list('Hello,' name);
```

`GET` reads from `SYSIN` (stdin); `PUT` writes to `SYSOUT` (stdout). Both  
are portable — they work the same whether the input is a terminal or  
redirected from a file.

### Edit-directed I/O

`PUT EDIT` / `GET EDIT` pairs data items with format items. Use these when  
you need precise column layout — reports, data exports, fixed-width files.

```pli
dcl a fixed bin(31), b char(4);
a = 123;
put edit(a) (f(6));             /* right-justified 6-wide integer */
get edit(b) (a(4));             /* 4-char field */
```

Served format items: `F(w,d)`, `E(w,d)`, `A(w)`, `X(w)`, `SKIP(n)`,  
`PAGE`, `LINE(n)`, `B(n)` (bit), `C(real,imag)` (complex), `COL(n)`, and  
`(n)(...)` repetition groups.

### DATA-directed I/O

`PUT DATA(x, y)` prints `NAME=value` pairs; `GET DATA` reads them in any  
order, skipping unknown names. This is convenient for configuration files  
where field order may vary:

```pli
dcl 1 config,
    2 host char(20),
    2 port fixed bin(31),
    2 retries fixed bin(31);
get data(config);     /* reads host='...', port=8080, retries=3 */
```

### Files & records

```pli
dcl f file;
open file(f) title('data.txt') output;   /* stream file */
put file(f) list(a, b);
close file(f);

open file(f) record sequential input title('rec.dat');  /* binary records */
read file(f) into(a);
write file(f) from(a);
```

**Limitations**

- `RECORD`/`UPDATE`/`KEYED`/`IGNORE` options on `OPEN` are errors.
- Record I/O is fixed-size binary only: `REWRITE`, `DELETE`, `LOCATE`,  
`UNLOCK`, and `ON ENDFILE` are not implemented.
- `DISPLAY REPLY` form is an error.

---

## C interoperability

Declare an external C function with `ENTRY ... RETURNS ... EXTERNAL('name')`, binding it to a C symbol. PL/I passes scalar arguments  
by reference (address) by default; `OPTIONS(LINKAGE(SYSTEM))` or  
`OPTIONS(BYVALUE)` marshals them as C values instead.

```pli
dcl c_add entry(fixed bin(31), fixed bin(31))
     returns(fixed bin(31))
     options(linkage(system)) external('c_byval_add');
dcl r fixed bin(31);
r = c_add(3, 4);                 /* calls the C function by value */
```

The matching C side:

```c
int c_byval_add(int a, int b) { return a + b; }
```

`BYADDR(s)` opts a single `CALL` argument out of the structure copy so  
callee writes are visible:

```pli
call proc(byaddr(mysize));          /* pass structure by address */
```

`CHAR(n) VARYINGZ` arrives at a by-value C entry as a bare NUL-terminated  
`char *`.

**Limitations**

- Only scalar and pointer C parameters are supported by value; structure-valued C  
returns through the PL/I hidden-buffer convention are errors in this stage.
- Default by-reference calls pass addresses; a structure argument is copied  
by value unless `BYADDR` opts it out.
