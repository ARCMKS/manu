## `docs/language/patterns.md`
# Idioms and Conventions

*Current as of commit `<hash>`, <date>.*

Patterns that appear in the prelude, the test suite, and any
program that uses the language's features idiomatically. This is
not a specification; it is a collection of conventions that make
code read uniformly.

---

## §1 — Method naming

Prism does not have traits. There is no compiler-enforced
relationship between methods on different types. The stdlib and
the prelude follow three naming patterns so a reader can guess
the shape of an operation from its name.

**`map`** — produces the same kind of wrapper with a new payload.

```
Option<i64>.map(f: fn(i64) : U) : Option<U>
Result<i64, E>.map(f: fn(i64) : U) : Result<U, E>
[i64].map(f: fn(i64) : U) : [U]
```

**`unwrap_or`** — unwraps the payload, using a fallback if the
wrapper is in its "empty" state.

```
Option<i64>.unwrap_or(fallback: i64) : i64
Result<i64, E>.unwrap_or(fallback: i64) : i64
```

**`is_X`** — a test that returns `bool` without unwrapping.

```
Option<i64>.is_some() : bool
Option<i64>.is_none() : bool
Result<i64, E>.is_ok() : bool
Result<i64, E>.is_err() : bool
```

These names are a convention. `Option.map` and `[T].map` are not
"the same method" to the compiler; they are two methods that
happen to share a name and a shape. User-defined wrapper types
should follow the same convention so that code reads uniformly.

The `?` operator is the only monadic operation the compiler knows
about; it is hard-coded for `Result` and `Option` and cannot be
extended to user types.

---

## §2 — Building wrapper types

A wrapper type should be an RC-managed enum:

```
type Wrapper<T> = enum { Value(T), Empty }
```

If the wrapper is declared with `is_builtin` — which only the
prelude can do — every use site sees it as `rc<Wrapper<T>>`. User
types do not get this treatment; a user wrapper is used as a
plain enum and the programmer chooses RC wrapping explicitly with
`rc e`.

The prelude's choice reflects the intended use case: `Result` and
`Option` are returned from functions and outlive the producing
scope, so they need the RC heap. A user's wrapper type may have
different lifetime requirements.

---

## §3 — Error propagation

The `?` operator makes chained error handling concise:

```
fn process(path: string): Result<i64, string> {
    let text = read_file(path)?;
    let parsed = parse(text)?;
    let value = validate(parsed)?;
    return Result.Ok(value);
}
```

Each `?` either unwraps a success payload or returns the `Err`
from the enclosing function.

When the error types do not match, convert explicitly:

```
fn process(path: string): Result<i64, string> {
    let text = read_file(path)
        .map_err(fmt_error)?;     // convert to string
    return Result.Ok(len(text) as i64);
}
```

A user-facing API often has a nested error type:

```
type AppError = enum {
    Io(string),
    Parse(string),
}
```

Converting from an inner `Result<T, string>` to a
`Result<T, AppError>` requires a helper method or an explicit
`match`; there is no automatic conversion.

---

## §4 — Arena idioms

Reach for `&e` when the value's lifetime is bounded by the
enclosing scope. This is the common case for locals, temporary
buffers, and short-lived wrappers.

```
fn main(): i64 {
    let buf = [0, 0, 0, 0];      // array lives in the arena
    let p = &buf;                // ref<[i64]>
    return p[0];
}
```

Reach for `rc e` when the value must outlive its scope, or when
it is returned from a function.

```
fn make_buffer(): rc<[i64]> {
    return rc [0, 0, 0, 0];
}
```

Because the escape rules forbid returning a `ref<T>`, the choice
is often forced: if the value escapes, it must be `rc`.

---

## §5 — FFI wrappers

A common pattern is to hide the C surface behind Prism types.
The wrapper converts at the boundary and exposes Prism types
inside.

```
extern lib "libc.so.6" as c
extern fn strlen(s: *u8): u64 from c

fn prisms_len(s: string): i64 {
    return strlen(ffi_cstring(s)) as i64;
}
```

Inside `prisms_len`, the C function is called with a C string
(via `ffi_cstring`). Callers use the Prism signature.

For a C resource like `FILE*`:

```
extern type FILE = opaque
extern fn fopen(path: *u8, mode: *u8): Option<*FILE> from c
extern fn fclose(f: *FILE): i32 from c

fn open_file(path: string): Option<*FILE> {
    return fopen(ffi_cstring(path), c"r");
}
```

The opaque type prevents `*FILE` from being confused with other
pointer types. The `Option<*FILE>` convention handles NULL.

Lifetime is the user's responsibility. The C API's documentation
says who frees what; Prism's RC system does not extend across
the boundary.

---

## §6 — Multi-file program layout

A typical program:

```
src/
  main.manu          — entry point, `fn main`
  lib/
    math.manu        — pure functions
    io.manu          — I/O wrappers
  types.manu         — shared type definitions
```

`main.manu` imports:

```
using lib.math
using lib.io
using types

fn main(): i64 {
    let n = lib.math.double(21);
    lib.io.print_number(n);
    return 0;
}
```

Use `pub` on declarations that are meant to be visible. Leave it
off for internal helpers.

The module loader resolves `lib.math` to `lib/math.manu` in an
include directory. The default include directory is the directory
of the root file, then the current directory, then any `-I`
paths.

---

## §7 — Iterator-style patterns

Prism has no iterator protocol. The idiomatic pattern is a
`for..in` loop over an array or a `while` loop with an index:

```
fn sum(xs: [i64]): i64 {
    let mut total = 0;
    for x in xs {
        total = total + x;
    }
    return total;
}
```

To process a range with an index:

```
fn sum_of_squares(n: i64): i64 {
    let mut total = 0;
    for i in 0..n {
        total = total + i * i;
    }
    return total;
}
```

For manual index access:

```
fn find(xs: [i64], target: i64): i64 {
    let mut i = 0;
    while (i < len(xs)) {
        if (xs[i] == target) { return i; }
        i = i + 1;
    }
    return -1;
}
```

---

## §8 — Testing

Prism has no in-language test framework. Tests are written in C
and compiled into the test binary via `tests/t_*.c`.

A test exercises a small program through the compiler and the VM:

```c
static void t_addition(void) {
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 { return 1 + 2; }"), 3);
}
```

`test_value` compiles the source, runs it, and asserts that the
result matches. It fails if compilation failed, so a test cannot
pass for the wrong reason.

For tests that exercise diagnostics:

```c
static void t_type_error(void) {
    assert_error(
        "fn main(): i64 { let x: i8 = 300; return 0; }",
        "does not fit");
}
```

`assert_error` asserts that a diagnostic contains the given
substring.

For tests that capture output:

```c
static void t_print(void) {
    char buf[64];
    test_output("fn main(): i64 { print(\"hi\"); return 0; }",
                buf, sizeof buf);
    ASSERT_EQ_STR(buf, "hi\n");
}
```

See `arch/files/24-test-framework.md` for the framework's full
surface.

---

## §9 — Anti-patterns

**Trying to return a reference.** Prism forbids `ref<T>` return
types. If you find yourself wanting one, the value should
probably be `rc<T>`.

```
// Rejected:
fn get_ref(): ref<i64> { let x = 5; return &x; }

// Correct:
fn get_rc(): rc<i64> { return rc 5; }
```

**Assuming `rc variable` works.** `rc` on a compound variable is
rejected; the operand must be a literal form.

```
// Rejected:
let a = [1, 2, 3];
let b = rc a;

// Correct:
let b = rc [1, 2, 3];
```

**Relying on boolean operators.** `&&` and `||` are lexed but
the parser does not produce AST nodes for them. Use nested `if`
or `match`:

```
// Rejected:
if (a > 0 && b > 0) { ... }

// Correct:
if (a > 0) {
    if (b > 0) { ... }
}
```

**Expecting implicit numeric coercion.** Prism requires matching
operand types in a binary expression. Use `as`:

```
// Rejected:
let x: i8 = 5;
let y = x + 1000;

// Correct:
let y = x as i64 + 1000;
```

**Treating `struct` or `enum` as first-class return values.**
Aggregates must be wrapped in `rc` if they escape:

```
// Rejected:
fn make(): Point { return Point { x: 1, y: 2 }; }

// Correct:
fn make(): rc<Point> { return rc Point { x: 1, y: 2 }; }
```

**Assuming constant folding.** A `const` initializer must be a
literal. Arithmetic is not folded at compile time:

```
// Rejected:
const SIZE = 4 * 1024;

// Correct:
const SIZE = 4096;
```

**Using `using Name = Type`.** The syntax parses but has no
effect. Use explicit type annotations or a real type alias via
a struct wrapper.

**Expecting `let` bindings to be mutable by default.** They are
not. Reassignment requires `let mut`.
