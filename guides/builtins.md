# Built-in Functions — plic

Reference for every built-in function `plic` recognises. Built-in names are ordinary identifiers (not reserved words) — they are recognised by name after parsing, so `substr`, `SUBSTR`, or `Substr` are all the same call.

Built-in names are case-insensitive. Some built-ins require a constant argument so the compiler can size the result at compile time; passing a non-constant in that position is reported as an error.

---

## Quick reference

| Category | Built-in                                                                                                                                  | Signature                | Returns                           |
| -------- | ----------------------------------------------------------------------------------------------------------------------------------------- | ------------------------ | --------------------------------- |
| Pointer  | `NULL`                                                                                                                                    | `( )`                    | `POINTER`                         |
| Pointer  | `ADDR`                                                                                                                                    | `( var | var(i) | S.A )` | `POINTER`                         |
| Storage  | `BYADDR`                                                                                                                                  | `( struct )`             | the struct (marker)               |
| String   | `SUBSTR`                                                                                                                                  | `( s, start, length )`   | `CHAR(length)`                    |
| String   | `INDEX`                                                                                                                                   | `( s, sub )`             | `FIXED BIN`                       |
| String   | `LENGTH`                                                                                                                                  | `( s )`                  | `FIXED BIN`                       |
| String   | `REVERSE`                                                                                                                                 | `( s )`                  | `CHAR(len(s))`                    |
| String   | `REPEAT`                                                                                                                                  | `( s, n )`               | `CHAR(len(s)*n)`                  |
| String   | `TRANSLATE`                                                                                                                               | `( s, out, in )`         | `CHAR(len(s))`                    |
| String   | `VERIFY`                                                                                                                                  | `( s, set [, start] )`   | `FIXED BIN`                       |
| String   | `TRIM`                                                                                                                                    | `( s [, pad] )`          | `CHAR(len(s))`                    |
| String   | `TALLY`                                                                                                                                   | `( x, y )`               | `FIXED BIN`                       |
| String   | `UPPERCASE`                                                                                                                               | `( s )`                  | `CHAR(len(s))`                    |
| String   | `LOWERCASE`                                                                                                                               | `( s )`                  | `CHAR(len(s))`                    |
| String   | `CENTER`                                                                                                                                  | `( s, w )`               | `CHAR(w)`                         |
| String   | `SEARCH`                                                                                                                                  | `( s, set, start )`      | `FIXED BIN`                       |
| String   | `RANK`                                                                                                                                    | `( c )`                  | `FIXED BIN`                       |
| String   | `COLLATE`                                                                                                                                 | `( n )`                  | `CHAR(1)`                         |
| String   | `HIGH`, `LOW`                                                                                                                             | `( n )`                  | `CHAR(n)`                         |
| String   | `DATE`                                                                                                                                    | `( )`                    | `CHAR(8)`                         |
| String   | `TIME`                                                                                                                                    | `( )`                    | `CHAR(6)`                         |
| Type     | `CHAR`                                                                                                                                    | `( x )`                  | `CHAR(24)`                        |
| Type     | `FIXED`                                                                                                                                   | `( char | num )`         | `FIXED BIN(31)`                   |
| Numeric  | `ABS`                                                                                                                                     | `( x )`                  | same type as `x`                  |
| Numeric  | `TRUNC`                                                                                                                                   | `( x )`                  | same type as `x`                  |
| Numeric  | `PRECISION`                                                                                                                               | `( x, p )`               | `x` at precision `p`              |
| Numeric  | `MIN`, `MAX`                                                                                                                              | `( a, b, ... )`          | common type                       |
| Numeric  | `MOD`                                                                                                                                     | `( a, b )`               | common type                       |
| Numeric  | `MULTIPLY`                                                                                                                                | `( a, b )`               | product type                      |
| Numeric  | `DIVIDE`                                                                                                                                  | `( a, b )`               | `FLOAT`                           |
| Numeric  | `ROUND`                                                                                                                                   | `( x, n )`               | `FLOAT`                           |
| Math     | `FLOOR`,`CEIL`,`SQRT`,`EXP`,`LOG`,`SIN`,`COS`,`TAN`,`LOG2`,`LOG10`,`ATAN`,`SINH`,`COSH`,`TANH`,`ATANH`,`ERF`,`ERFC`,`ASIN`,`ACOS`,`CBRT` | `( x )`                  | `FLOAT`                           |
| Math     | `SIND`,`COSD`,`TAND`,`ATAND`                                                                                                              | `( x )`                  | `FLOAT` (degrees)                 |
| Math     | `ATAN2`                                                                                                                                   | `( y, x )`               | `FLOAT`                           |
| Complex  | `COMPLEX`                                                                                                                                 | `( re, im )`             | `COMPLEX`                         |
| Complex  | `REAL`, `IMAG`                                                                                                                            | `( z )`                  | `FLOAT`                           |
| Complex  | `CONJG`                                                                                                                                   | `( z )`                  | `COMPLEX`                         |
| Array    | `LBOUND`, `HBOUND`, `DIM`, `DIMENSION`                                                                                                    | `( A [, axis] )`         | `FIXED BIN`                       |
| Array    | `SUM`, `PROD`                                                                                                                             | `( A )`                  | element type                      |
| Array    | `ANY`, `ALL`                                                                                                                              | `( A )`                  | `BIT(1)`                          |
| Async    | `EVENT`                                                                                                                                   | `( ev )`                 | `BIT(1)`                          |
| Args     | `OMITTED`, `PRESENT`                                                                                                                      | `( p )`                  | `BIT(1)`                          |
| Misc     | `ONCODE`                                                                                                                                  | `( )`                    | `FIXED BIN`                       |
| Misc     | `SYSPARM`                                                                                                                                 | `( )`                    | `CHAR(n)` (compile-time constant) |
| Misc     | `PRIORITY`                                                                                                                                | `( ... )`                | not implemented                   |

`*` = must be a constant in the current stage.

---

## Pointer / storage

### `NULL()` → POINTER

The null pointer value. Use `NULL()` to initialise a pointer or to test whether one has been set:

```pli
dcl file_path pointer;

file_path = null();

if file_path = null() then put skip list('no file selected');
```

### `ADDR(x)` → POINTER

The address of a variable, array element, or structure member. Taking an address is how you build pointer-linked data structures without losing safety checks — subscript range checking still applies:

```pli
dcl node fixed bin(31);
dcl node_ptr pointer;

node_ptr = addr(node);

dcl table(100) fixed bin(31);
dcl elem_ptr pointer;

elem_ptr = addr(table(7));   /* address of element 7 */

dcl 1 record,
      2 id    fixed bin(31),
      2 name  char(20);
dcl rec_ptr pointer;

rec_ptr = addr(record.name);  /* address of a structure member */
```

**Limitations** — `ADDR` of a cross-section `A(i, *)` is not implemented; a dynamic cross-section address is not supported.

### `BYADDR(s)` → struct (marker)

`BYADDR` is not a function — it is a marker used only as a direct call argument. It passes a structure by its own address so the callee's writes are visible to the caller. Without `BYADDR`, a structure argument is copied by value (the callee cannot modify the caller's data).

```pli
dcl 1 stats,
      2 count fixed bin(31),
      2 total fixed bin(31);

call accumulate(byaddr(stats));  /* stats is updated in place */
```

**Limitations** — using `BYADDR` anywhere other than a direct call argument is an error.

---

## String built-ins

### `SUBSTR(s, start, length)` → CHARACTER(length)

A character substring of `s`. The length must be a constant so the result type is sized; a runtime length yields a `VARYING` result over the source maximum.

```pli
dcl msg char(40) varying;

msg = 'Error: missing file';
put skip list(substr(msg, 3, 7));     /* 'rror: ' — wait, that's 7 chars from pos 3 */
```

A realistic use — extract a file extension:

```pli
dcl filename char(30) varying;

filename = 'report.pdf';

dcl ext char(5);
ext = substr(filename, length(filename) - 3, 4);  /* 'pdf' */
```

### `INDEX(s, sub)` → FIXED BINARY

1-based position of `sub` within `s`, or 0 if `s` does not contain `sub`. This is how you test for the presence of a substring:

```pli
dcl email char(40) varying;

email = 'user@example.com';
if index(email, '@') > 0 then put skip list('valid address');
```

### `LENGTH(s)` → FIXED BINARY

The current length of a character value (live length for `VARYING`, declared length otherwise).

### `REVERSE(s)` → CHARACTER(len(s))

Mirrors the string (blank-pads to the result length after reversing the source bytes).

```pli
dcl word char(10); word = 'abcdefg';
put skip list(reverse(word));         /* 'gfedcba' */
```

### `REPEAT(s, n)` → CHARACTER(len(s) * n)

`n` must be a constant; the result's length is `len(s) * n`.

```pli
dcl s char(10) varying;
s = repeat('ab', 3);                  /* 'ababab' */
```

A realistic use — pad a line to a fixed width:

```pli
dcl separator char(40);
separator = repeat('-', 40);
put skip list(separator);
```

### `TRANSLATE(s, out, in)` → CHARACTER(len(s))

Each character of `s` that appears in `in` is replaced by the corresponding character of `out`; characters not in `in` are left unchanged.

```pli
dcl encoded char(10);
encoded = translate('HELLO', '012345', 'ABCDE');  /* '01234' → 'HELLO' → '01234' */
```

### `VERIFY(s, set [, start])` → FIXED BINARY

Position of the first character in `s` (starting at `start`, if given) that is *not* in `set`, or 0 if every character belongs to the set. Useful for validating that a string contains only expected characters.

```pli
dcl digit char(10) value('0123456789');
dcl s char(20) varying;

s = '12345';
if verify(s, digit) = 0 then put skip list('all digits');
```

### `TRIM(s [, pad])` → CHARACTER(len(s)) *(extension)*

Strips leading/trailing blanks (or pad-set characters). The result keeps the input length, left-justified and blank-padded.

```pli
dcl name char(20); name = '  Alice  ';

put skip list('[' || trim(name) || ']');   /* '[Alice       ]' */
put skip list(trim(name, 'e'));            /* strip 'e' too */
```

A realistic use — clean up user input before comparison:

```pli
dcl input char(30) varying;

input = '  42  ';

dcl n fixed bin(31);
n = fixed(trim(input));   /* parse after stripping blanks */
```

**Limitations** — `TRIM` of a `BIT` argument is an error.

### `TALLY(x, y)` → FIXED BINARY *(extension)*

Counts non-overlapping, case-sensitive occurrences of `y` in `x`; 0 when absent or null.

```pli
put skip list(tally('a.b.c.b', 'b'));   /* 2 */
```

A realistic use — count delimiters in a record:

```pli
dcl record char(80) varying;

record = 'field1|field2|field3|';
put skip list(tally(record, '|'));      /* 3 fields (trailing delimiter) */
```

### `UPPERCASE(s)` → CHARACTER(len(s)) *(IBM extension)*

Folds `a–z` to `A–Z`; other characters unchanged. Handy for case-insensitive comparison.

```pli
dcl s char(10); s = 'mixedCase';
put skip list(uppercase(s));      /* 'MIXEDCASE' */
```

### `LOWERCASE(s)` → CHARACTER(len(s))

Fold `A–Z` to `a–z`. Same signature shape as `UPPERCASE`.

### `CENTER(s, w)` → CHARACTER(w)

Centers `s` in a field of width `w` (constant); pads with blanks.

```pli
dcl line char(20);

line = center('Report', 20);              /* '       Report        ' */
put skip list(line);
```

**Limitations** — `w` must be a constant; a runtime width is an error.

### `SEARCH(s, set, start)` → FIXED BINARY

1-based position in `s` of the first character (searching from `start`) that also appears in `set`, or 0 if none matches. This is how you find the next character from a delimiter set.

```pli
dcl s char(30) value('  skip leading blanks');
dcl blanks char(10) value(' ');
dcl pos fixed bin(31);

pos = search(s, blanks, 1);   /* position of first non-blank */
```

### `RANK(c)` → FIXED BINARY

The numeric collating (code point) value of the first character.

```pli
put skip list(rank('A'));       /* 65 */
```

### `COLLATE(n)` → CHARACTER(1)

The character for numeric code point `n` (constant `n`).

```pli
put skip list(collate(65));     /* 'A' */
```

### `HIGH(n)`, `LOW(n)` → CHARACTER(n)

`n` copies of the highest (`HIGH`) or lowest (`LOW`) collating character; `n` must be a constant to size the result. In the current runtime `HIGH `yields byte `0xFF` and `LOW` yields byte `0x00` (NUL).

```pli
dcl h char(3), l char(3);

h = high(3);                    /* three 0xFF bytes */
l = low(3);                     /* three NUL bytes */

put skip list(length(h));       /* 3 */
```

### `DATE()` → CHARACTER(8), `TIME()` → CHARACTER(6)

`DATE()` returns `YYYYMMDD`; `TIME()` returns `HHMMSS`.

```pli
put skip list('Run started:', date(), time());  /* e.g. 20261002 094210 */
```

---

## Type-conversion built-ins

### `CHAR(x)` → CHARACTER(24)

Renders a scalar (`FIXED`, `FLOAT`, `BIT`) as text. A character argument passes through (truncated/padded by assignment).

```pli
dcl s char(24);

s = char(42);                     /* '42' */
s = char(3.5);                    /* '3.5' */
```

**Limitations** — `CHAR` of a structure, array, pointer, or complex is an error.

### `FIXED(x)` → FIXED BINARY(31)

Parses decimal text, or truncates a float toward zero. For a float, out-of-range values clamp; `NaN` reads as 0.

```pli
dcl n fixed bin(31);

n = fixed('1234');                /* integer parse */
n = fixed(3.9);                   /* 3 */
```

**Limitations** — `FIXED` of a `BIT` or `COMPLEX` argument is an error.

---

## Numeric built-ins

These work on numeric scalars (fixed or float, in their common arithmetic type unless noted). Numeric built-ins that take a fixed operand and produce a fixed result trap to `SIZE` on overflow when a handler exists, and abort otherwise.

### `ABS(x)` → same type

Magnitude; for `COMPLEX`, `|x + iy| = sqrt(x² + y²)` as a `FLOAT`.

```pli
put skip list(abs(-7));           /* 7 */
```

### `TRUNC(x)` → same type

Drops fractional digits toward zero.

```pli
dcl d fixed dec(9,2);
d = trunc(12.349);                /* 12 */
```

### `PRECISION(x, p)` → `x` at precision `p`

`p` must be a positive constant. The value is unchanged; only the declared precision changes (storage width may grow).

### `MIN(a, b, ...)`, `MAX(a, b, ...)` → common type

Fold left-to-right; at least two arguments.

```pli
dcl lo fixed bin(31);
lo = min(3, 8, 1, 9);             /* 1 */
```

### `MOD(a, b)` → common type

Remainder with the divisor's sign. A zero divisor raises the `ZERODIVIDE `condition, resuming with 0 when a handler is present.

```pli
put skip list(mod(17, 5));        /* 2 */
```

### `MULTIPLY(a, b)` → product type

For `FIXED` operands the result scale is the sum of the operand scales.

```pli
dcl p fixed dec(9,2);
p = multiply(1.5, 2.0);           /* 3.00 */
```

### `DIVIDE(a, b)` → FLOAT

Floating-point quotient. A zero divisor raises `ZERODIVIDE`, resuming with 0.

### `ROUND(x, n)` → FLOAT

Rounds `x` to `n` decimal places.

```pli
put skip list(round(3.14159, 2)); /* 3.15 */
```

---

## Math built-ins

One numeric argument, converted to `FLOAT`, returns `FLOAT`. The trig `SIND`/`COSD`/`TAND`/`ATAND` take and return degrees; the rest are radians or natural where C's `<math.h>` is.

```pli
dcl y float dec(6);

y = sqrt(9.0);        /* 3.0 */
y = log(exp(1.0));    /* 1.0 */
y = sin(0.0);         /* 0.0 */
y = sind(90.0);       /* 1.0 (degrees) */
y = atan2(1.0, 1.0);  /* 0.7854... = pi/4 */
```

Available functions:

```
FLOOR CEIL SQRT EXP LOG            (also LOG2 LOG10)
SIN COS TAN                        (also SINH COSH TANH ATANH)
ASIN ACOS ATAN ATAN2 CBRT
ERF ERFC
SIND COSD TAND ATAND               (degree-trig built-ins)
```

**Limitations** — arguments are converted to float before the call, so fixed-point precision beyond the float mantissa may not survive a math built-in call.

---

## Complex built-ins

`COMPLEX` is stored as a pair of floats `{re, im}`. Complex arithmetic is useful in signal processing, Fourier transforms, and electrical calculations.

### `COMPLEX(re, im)` → COMPLEX

Build a complex value.

```pli
dcl z complex;
z = complex(1.5, 2.5);
```

### `REAL(z)`, `IMAG(z)` → FLOAT

Extract the real or imaginary part.

```pli
put skip list(real(z));     /* 1.5 */
put skip list(imag(z));     /* 2.5 */
```

### `CONJG(z)` → COMPLEX

Negate the imaginary part (the complex conjugate).

```pli
put skip list(conjg(z));    /* (1.5, -2.5) */
```

**Limitations** — ordered comparison (`<`, `>`, etc.) and `CHAR`/`FIXED` of a complex value are errors; only part-wise `=`/`^=` is supported.

---

## Array inquiry built-ins

### `LBOUND(A [, axis])`, `HBOUND(A [, axis])`, `DIM(A [, axis])`, `DIMENSION(A [, axis])`

→ `FIXED BINARY`. `axis` (1-based) must be a constant.

- Without an axis: `LBOUND`/`HBOUND` report the first axis; `DIM`/`DIMENSION `report the total element count.
- With an axis: that axis's lower/upper bound (for `LBOUND`/`HBOUND`) or extent (for `DIM`/`DIMENSION`).

```pli
dcl a(3:5, 1:4) fixed bin(31);

put skip list(lbound(a));        /* 3 */
put skip list(hbound(a, 2));     /* 4 */
put skip list(dim(a));           /* 12 = 3*4 elements */
put skip list(dimension(a, 1));  /* 3 = extent of axis 1 */
```

For dynamic arrays the live bounds are reported; for `CONTROLLED` arrays the live generation extent is used.

**Limitations** — the argument must be an unsubscripted array variable or a qualified member array; a subscripted `A(i)` or element expression is an error.

---

## Array reduction built-ins

### `SUM(A)`, `PROD(A)` → element type

Sum and product of all elements. These let you collapse an array to a single value in one expression.

### `ANY(A)`, `ALL(A)` → BIT(1)

`ANY` is true if any element is set; `ALL` is true if all are set. The array must be `BIT` (`BIT(1)` elements).

```pli
dcl a(5) fixed bin(31);
dcl b(4) bit(1);

do i = 1 to 5;  a(i) = i;  end;

put skip list(sum(a));          /* 15 */
put skip list(prod(a));         /* 120 */

b(1) = '1'b;
put skip list(any(b));          /* '1'B */
```

**Limitations**

- Reductions take one argument (the array) in the current stage; element-wise expression forms expand to a temp only in direct assignment.
- `BIT(n>1)` array reductions are errors.

---

## Concurrency built-ins

### `EVENT(ev)` → BIT(1)

Polls an `EVENT` variable, element `ev(i)`, or member `S.EV` for completion (`'1'B` complete, `'0'B` incomplete).

```pli
dcl done event;
call worker event(done);
do while (^event(done));
    put skip list('still working...');
end;
```

### `PRIORITY(...)`

Task priorities are accepted as a `CALL` option but the `PRIORITY` *built-in *form is not implemented.

---

## Argument-introspection built-ins *(OPTIONAL)*

### `OMITTED(p)`, `PRESENT(p)` → BIT(1)

Test whether an `OPTIONAL` parameter `p` was omitted at the call site. Both compare the parameter's by-reference slot against null.

```pli
dcl opt fixed bin(31) optional;

if omitted(opt) then put skip list('using default');
if present(opt) then put skip list('got:', opt);
```

**Limitations** — the argument must be an `OPTIONAL` parameter; anything else is an error.

---

## Misc built-ins

### `ONCODE()` → FIXED BINARY

Returns `1` inside an `ERROR` unit raised by `SIGNAL`, `0` elsewhere.

### `SYSPARM()` → CHARACTER(n)

The value of the `--sysparm <s>` driver option, baked in at compile time.  
It is a constant string whose length is fixed at compile time.

### `DATE()`, `TIME()`

See [String built-ins](#string-built-ins).
