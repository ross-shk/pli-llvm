# Built-in Functions — pli

Reference for every built-in function `pli` recognises. Built-in names are ordinary identifiers (not reserved words) — they are recognised by name after parsing, so `substr`, `SUBSTR`, or `Substr` are all the same call.

Built-in names are case-insensitive. Some built-ins require a constant argument so the compiler can size the result at compile time; passing a non-constant in that position is reported as an error.

---

## Quick reference

| Category | Built-in                                                                                                                                  | Signature                | Returns                           |
| -------- | ----------------------------------------------------------------------------------------------------------------------------------------- | ------------------------ | --------------------------------- |
| Pointer  | `NULL`                                                                                                                                    | `( )`                    | `POINTER`                         |
| Pointer  | `ADDR`                                                                                                                                    | `( var | var(i) | S.A )` | `POINTER`                         |
| Area     | `OFFSET`                                                                                                                                  | `( pointer, area )`      | `OFFSET`                          |
| Area     | `OFFSETADD`                                                                                                                               | `( offset, n )`          | `OFFSET`                          |
| Area     | `OFFSETSUBTRACT`                                                                                                                          | `( offset, n )`          | `OFFSET`                          |
| Area     | `OFFSETDIFF`                                                                                                                              | `( offset, offset )`     | `FIXED BIN(31,0)`                 |
| Storage  | `BYADDR`                                                                                                                                  | `( struct )`             | the struct (marker)               |
| String   | `SUBSTR`                                                                                                                                  | `( s, start, length )`   | `CHAR(length)`                    |
| String   | `INDEX`                                                                                                                                   | `( s, sub )`             | `FIXED BIN`                       |
| String   | `LENGTH`                                                                                                                                  | `( s )`                  | `FIXED BIN`                       |
| String   | `REVERSE`                                                                                                                                 | `( s )`                  | `CHAR(len(s))`                    |
| String   | `REPEAT`                                                                                                                                  | `( s, n )`               | `CHAR(len(s)*n)`                  |
| String   | `COPY`                                                                                                                                    | `( s, n )`               | `CHAR(len(s)*n)`                  |
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
| String   | `DATETIME`                                                                                                                                | `( )`                    | `CHAR(14)`                        |
| Type     | `CHAR`                                                                                                                                    | `( x )`                  | `CHAR(24)`                        |
| Type     | `FIXED`                                                                                                                                   | `( char | num )`         | `FIXED BIN(31)`                   |
| Type     | `FIXEDBIN`                                                                                                                                | `( x [,p[,q]] )`         | `FIXED BIN(p,q)`                  |
| Type     | `FIXEDDEC`                                                                                                                                | `( x [,p[,q]] )`         | `FIXED DEC(p,q)`                  |
| Numeric  | `ABS`                                                                                                                                     | `( x )`                  | same type as `x`                  |
| Numeric  | `SIGN`                                                                                                                                    | `( x )`                  | `FIXED BIN(31)`                  |
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
| Array    | `EVERY`, `SOME`                                                                                                                           | `( A )`                  | `BIT(1)`                          |
| Array    | `IDENTICAL`                                                                                                                               | `( a, b )`               | `BIT(1)`                          |
| Async    | `EVENT`                                                                                                                                   | `( ev )`                 | `BIT(1)`                          |
| Args     | `OMITTED`, `PRESENT`                                                                                                                      | `( p )`                  | `BIT(1)`                          |
| Misc     | `ONCODE`                                                                                                                                  | `( )`                    | `FIXED BIN`                       |
| Misc     | `ONKEY`                                                                                                                                    | `( )`                    | `BIT(1)`                          |
| Misc     | `ONSOURCE`                                                                                                                                  | `( )`                    | `FIXED BIN(15)`                   |
| Misc     | `ONFILE`                                                                                                                                    | `( )`                    | `CHAR(32)`                        |
| Misc     | `SYSPARM`                                                                                                                                 | `( )`                    | `CHAR(n)` (compile-time constant) |
| Misc     | `SYSTEM(x)`                                                                                                                               | `( x )`                  | `FIXED BIN(31)`                   |
| Misc     | `PRIORITY`                                                                                                                                | `( ... )`                | not implemented                   |
| Storage  | `EMPTY`                                                                                                                                    | `( )`                    | `AREA`                            |
| Storage  | `CURRENTSIZE`                                                                                                                                | `( x )`                  | `FIXED BIN(31)`                   |
| File     | `FILEOPEN`                                                                                                                                 | `( x )`                  | `BIT(1)`                          |
| File     | `LINENO`                                                                                                                                    | `( x )`                  | `FIXED BIN(31)`                   |
| File     | `PAGENO`                                                                                                                                    | `( x )`                  | `FIXED BIN(31)`                   |
| IBM      | `ISOCHAR`                                                                                                                                    | `( lo, hi )`             | `CHAR(n)`                         |

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

### `NULL(:ENTRY:)` / `NULL(:LABEL:)` *(Subset-G extension, ADR-206)*

Typed nulls for entry and label variables, which a plain `NULL()` (pointer type) cannot initialise:

```pli
dcl on_ping entry variable;
dcl lab label;

on_ping = null(:entry:);
lab = null(:label:);
```

`NULL(:FILE:)` is an error in this stage — files have no value type. Any other designator, or a `: ... :` outside an enquiry built-in, is an error.

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

### `COPY(s, n)` → CHARACTER(len(s) * n)

Identical to `REPEAT`: `n` must be a constant; the result's length is `len(s) * n`.

```pli
dcl s char(10);
s = copy('AB', 3);                   /* 'ABABAB' */
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

### `DATE()` → CHARACTER(8), `TIME()` → CHARACTER(6), `DATETIME()` → CHARACTER(14)

`DATE()` returns `YYYYMMDD`; `TIME()` returns `HHMMSS`; `DATETIME()` returns `YYYYMMDDHHMMSS` (i.e. `DATE() || TIME()`).

```pli
put skip list('Run started:', date(), time());  /* e.g. 20261002 094210 */
put skip list(datetime());                      /* e.g. 20261002094210 */
```

---

## Type-conversion built-ins

### `CHAR(x)` → CHARACTER(24) (29 for a complex value)

Renders a scalar (`FIXED`, `FLOAT`, `BIT`, `COMPLEX`) as text. A character
argument passes through (truncated/padded by assignment).

```pli
dcl s char(24);

s = char(42);                     /* '42' */
s = char(3.5);                    /* '3.5' */
s = char(complex(1.5, 2.5));      /* '1.5+2.5I' */
```

A `COMPLEX` argument returns `CHARACTER(29)` (the compiler's complex mode
is `FLOAT DECIMAL(6)`). The real part comes first, the imaginary part is
always signed, and an `I` follows; there are no blanks between the parts.
The parts use the same readable `%.6g` image as `PUT LIST`, so an exact IBM
`E`-format image remains in the decimal/precision tail.

**Limitations** — `CHAR` of a structure, array, or pointer is an error.

### `FIXED(x[,p[,q]])` → fixed point

Parses decimal text, or converts a numeric value to fixed point. The
one-argument form keeps the historical `FIXED BINARY(31,0)` result and
truncates toward zero; for a float, out-of-range values clamp and `NaN`
reads as 0.

```pli
dcl n fixed bin(31);
dcl b fixed bin(8,2);
dcl d fixed dec(9,2);

n = fixed('1234');                /* integer parse */
n = fixed(3.9);                   /* 3 */
b = fixed(3.75E0, 8, 2);          /* FIXED DEC(8,2), 3.75 */
d = fixed(fixeddec(3.75, 4, 2), 6); /* FIXED DEC(6,0), 4.00 */
```

`p` and `q` are integer constants; `q` defaults to zero and must be
nonnegative. The result keeps the base of a fixed source (`FIXED DEC` stays
decimal, `FIXED BIN` stays binary). A `FLOAT` source is read as `FLOAT
DECIMAL` because the compiler's type does not retain its base, so the result
is decimal; a character source produces a binary result. Limits are the same
as `FIXEDBIN`/`FIXEDDEC`: `p` up to 63 binary or 31 decimal, and `q` up to 62
binary or 18/38 decimal depending on the width.

A `COMPLEX` argument keeps the complex mode and truncates both parts toward
zero in the one-argument form; a complex value with `p`/`q` is rejected.
Complex values are carried as a `{double,double}` pair, so the fixed/float
distinction is not represented separately.

**Limitations** — `FIXED` of a `BIT` argument is an error. Character text
with `q > 0` is rejected until the fractional character scan lands; use a
numeric source for fractional values.

### `FIXEDBIN(x[,p[,q]])` / `FIXEDDEC(x[,p[,q]])`

Base-explicit fixed-point conversions. `FIXEDBIN` returns `FIXED BIN(p,q)`;
`FIXEDDEC` returns `FIXED DEC(p,q)`. `p` and `q` are integer constants, with
`p` up to 63 (binary) or 31 (decimal); `q` is nonnegative and must fit the
storage slot (at most 62 binary, 18 decimal, or 38 for a wide decimal). If
`p` is present and `q` is omitted, `q` is zero.

If both are omitted, the precision and scale come from the source type:
FIXED sources keep their base and scale (or convert through IBM's 3.32
factor), `BIT` sources take the maximum precision, and character sources
derive from their length. `FLOAT` sources are read as FLOAT DECIMAL because
the compiler's type does not keep the binary/decimal base.

```pli
dcl b fixed bin(31);
dcl d fixed dec(9,2);

b = fixedbin(3.75, 8, 2);   /* b is 3 */
d = fixeddec(3.5, 6, 1);    /* d is 3.50 */
b = fixedbin('123');        /* b is 123 */
```

Character input currently reuses `FIXED(char)`'s integer-only text scan, so
a decimal point in the text is not yet parsed; use a numeric source for
fractional values.

---

## Numeric built-ins

These work on numeric scalars (fixed or float, in their common arithmetic type unless noted). Numeric built-ins that take a fixed operand and produce a fixed result trap to `SIZE` on overflow when a handler exists, and abort otherwise.

### `ABS(x)` → same type

Magnitude; for `COMPLEX`, `|x + iy| = sqrt(x² + y²)` as a `FLOAT`.

```pli
put skip list(abs(-7));           /* 7 */
```

### `SIGN(x)` → FIXED BINARY(31)

Returns `-1` for a negative argument, `0` for zero, and `+1` for a positive one. The sign of a `FLOAT` argument is compared in floating point (a `NaN` argument yields `0`).

```pli
put skip list(sign(-7));          /* -1 */
put skip list(sign(0));           /* 0 */
put skip list(sign(3.5));         /* 1 */
put skip list(sign(-2.5));        /* -1 */
```

### `BOOL(b1, b2, b3)` → type of b2/b3

A conditional expression: returns `b2` when `b1` is true (`'1'b`), else `b3`. Arguments 2 and 3 must have the same type. The condition must be `BIT(1)`.

```pli
dcl x fixed bin(31);
x = bool(y > 0, 1, -1);           /* sign of y, without using SIGN(x) */
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

`CHAR` and `FIXED` of a complex value are served; `FIXED` keeps both
truncated parts, and assigning it to a real target takes the real part.

**Limitations** — ordered comparison (`<`, `>`, etc.) is an error; only
part-wise `=`/`^=` is supported.

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

### `EVERY(A)`, `SOME(A)` → BIT(1) *(Subset-G extension, ADR-195)*

`EVERY` is `ALL` and `SOME` is `ANY` under IBM names: `EVERY` is true if every element is set, `SOME` if any element is set. Same `BIT`-array restriction.

```pli
if every(b) then put skip list('all set');
if some(b) then put skip list('at least one set');
```

### `IDENTICAL(a, b)` → BIT(1) *(Subset-G extension, ADR-199)*

True (`'1'b`) when `a` and `b` have identical static types and equal values — compared element-wise for arrays and member-wise for structures (padding never participates) — and false (`'0'b`) otherwise. Differing static types (e.g. `FIXED` vs `FLOAT`, or `CHAR(3)` vs `CHAR(4)`) are never identical.

```pli
dcl v1(4) fixed bin(31), v2(4) fixed bin(31);

if identical(v1, v2) then put skip list('vectors match');
if identical(x, x) then put skip list('self-identical');
```

**Limitations**

- Arrays of structures/areas, unions anywhere in the operands, `VARYING` strings nested in compared aggregates, and dynamic arrays nested in compared structures are errors in this stage.

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

### `SYSTEM(x)` → FIXED BINARY(31)

Invokes the system command processor (`/bin/sh -c` on POSIX, `cmd.exe` on Windows) with the command string `x`. Returns the processor's exit code (0 = success, non-zero = failure). Character arguments are passed directly; numeric arguments are converted to character via `CHAR(x)`.

```pli
if system('test -f data.txt') = 0 then
    put skip list('file exists');
```

**Limitations** — the argument must be `CHARACTER` or a numeric type; a `POINTER` or other type is an error.

### `DATE()`, `TIME()`, `DATETIME()`

See [String built-ins](#string-built-ins).

### `ONCODE()` → FIXED BINARY

Returns `1` inside an `ERROR` unit raised by `SIGNAL`, `0` elsewhere.

### `ONKEY()` → BIT(1)

Returns `'1'b` when execution is inside an `ON` handler unit (i.e., a `SIGNAL` has been recognized and a handler is running), `'0'b` otherwise.

### `ONSOURCE()` → FIXED BINARY(15)

Returns the key identifying the source of the current `SIGNAL` (0 when no condition is active).

### `ONFILE()` → CHARACTER(32)

Returns the name of the file for which the current I/O condition (`ENDFILE`, `KEY`, `UNDEFINEDFILE`, …) was raised — blank when no file condition is active. `ON ENDFILE(f)` (ADR-285) and `ON UNDEFINEDFILE(f)` (ADR-309) now run, but the runtime does not track the file of the condition, so every reachable call still returns blank (IBM's out-of-context null); `ON KEY` stays diagnosed, so it never reaches a unit.

```pli
dcl f char(32);
f = onfile();
if f = '' then put skip list('no file condition active');
```

### `EMPTY()` → AREA

Returns a null AREA handle (the area equivalent of `NULL()` for pointers).

```pli
dcl area area;
allocate area in area;   /* AREA is implicitly initialised to EMPTY() */
```

### `OFFSET(p, area)` → OFFSET *(IBM built-in, ADR-319)*

Convert a `POINTER` that addresses a block in `area` into an `OFFSET`. The pointer must belong to that region — a foreign pointer raises the `AREA` condition — and a null pointer gives the null offset. It is the inverse direction of `ADDR`: `ADDR` gives an address, `OFFSET` binds that address to a region.

```pli
dcl region area(4096);
dcl p pointer;
dcl o offset(region);
dcl 1 rec based(p), 2 v fixed bin(31);

allocate rec in(region) set(p);
rec.v = 77;
o = offset(p, region);   /* the same block, now an offset */
```

`OFFSETVALUE(n)`, the inverse that turns an integer displacement into an offset, is not served: an offset here holds an absolute address, so there is no displacement to convert.

### `OFFSETADD(x, n)` / `OFFSETSUBTRACT(x, n)` → OFFSET *(IBM built-in, ADR-311)*

Move an offset by a byte count. The count converts to `FIXED BINARY(31,0)`, so a scaled decimal works and a negative count walks backwards; `OFFSETSUBTRACT(x, n)` is `OFFSETADD(x, -n)`. The result is still an offset into the same area, and there is no bounds check — IBM leaves that to the program.

### `OFFSETDIFF(x, y)` → FIXED BINARY(31,0) *(IBM built-in, ADR-311)*

The byte distance between two offsets. Use it to measure a block or check that a walk returned where it started.

```pli
dcl region area(4096);
dcl p offset(region);
dcl q offset(region);
dcl 1 rec based(p), 2 x fixed bin(31), 2 y fixed bin(31);
dcl 1 atq based(q), 2 y fixed bin(31);

allocate rec in(region) set(p);
rec.x = 11;
rec.y = 22;
q = offsetadd(p, 4);              /* four bytes on: rec.y */
if atq.y ^= 22 then put skip list('wrong field');
if offsetdiff(q, p) ^= 4 then put skip list('wrong distance');
```

### `CURRENTSIZE(x)` → FIXED BINARY(31)

Returns the storage size (in bytes) of the current value of variable `x`. For a `CHARACTER(n)` argument, returns `n`.

### `DECLARESIZE(:var:)` → FIXED BINARY(31) *(Subset-G extension, ADR-207)*

Returns the declared static storage size (in bytes) of a variable — the compile-time counterpart to `CURRENTSIZE`. `CHARACTER` gives the declared length, `BIT(n)` rounds up to whole bytes, structures sum their members, static arrays multiply out. The designator must name a declared variable; dynamically-sized variables are an error in this stage.

```pli
dcl name char(20);
dcl n fixed bin(31);

n = declaresize(:name:);   /* 20 */
```

### `EPSILON(:FLOAT...:)` / `MAXVAL(:FLOAT...:)` / `MINVAL(:FLOAT...:)` → FLOAT *(Subset-G extension, ADR-208)*

Properties of the FLOAT format: the double epsilon, the largest finite double, and the smallest positive normalized double. The designator must name a FLOAT type (precision details are accepted but do not change the value — every `FLOAT` lowers to a double).

```pli
dcl e float;

e = epsilon(:float binary(53):);
if e <= 0.0 then put skip list('no epsilon');
```

### `FILEOPEN(x)` → BIT(1)

Returns `'1'b` if the external file `x` is currently open, `'0'b` otherwise.

### `LINENO(x)` → FIXED BINARY(31)

Returns the current line number of file `x` (0 if closed or no line tracking is active).

### `PAGENO(x)` → FIXED BINARY(31)

Returns the current page number of file `x` (0 if closed or no page tracking is active).

### `ISOCHAR(lo, hi)` → CHARACTER(256) *(IBM extension)*

Returns the CHARACTER string comprising the characters whose ISO/ASCII code points range from `lo` to `hi` (inclusive).

```pli
put skip list(isochar(65, 67));   /* 'ABC' */
```
