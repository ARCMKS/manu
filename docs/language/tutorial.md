## `docs/language/tutorial.md`

```markdown
# Prism Tutorial

*Current as of commit `<hash>`, <date>.*

A guided introduction. Fifteen minutes from install to a running
program that uses the FFI. Every code block in this document is
executed by the test suite; if it does not compile and produce
the stated output, the test fails.

The tutorial is task-oriented. For exhaustive semantics, see
`reference.md`.

---

## §1 — Install and run

```sh
make              # builds manu.exe (the compiler driver)
make test         # runs the test suite
```

A Prism source file has the extension `.manu`. `manu run <file>`
compiles and runs it.

```sh
manu run hello.manu
```

A file may also be passed without the `run` keyword:

```sh
manu hello.manu     # equivalent to `manu run hello.manu`
```

Use `manu check <file>` to compile without running. Useful for
catching errors without executing side effects.

---

## §2 — Hello, world

```prism runnable
fn main(): i64 {
    print("hello, world");
    return 0;
}
```

Output:

```
hello, world
```

Every program has a `fn main`. The return type is `i64` (or `void`,
or inferred). `print` is a builtin that writes a line to stdout.

If the return type is omitted and the function returns a value,
the return type is inferred:

```prism runnable
fn main() {
    return 0;
}
```

---

## §3 — Values and types

The primitive types:

| Type | Notes |
|---|---|
| `i8` `i16` `i32` `i64` | Signed integers |
| `u8` `u16` `u32` `u64` | Unsigned integers |
| `f32` `f64` | IEEE floats |
| `bool` | `true` / `false` |
| `string` | RC-managed byte sequence |

```prism runnable
fn main(): i64 {
    let n = 42;              // i64
    let f = 3.14;            // f64
    let b = true;            // bool
    let s = "hello";         // string
    let t: i32 = 100;        // annotated
    return n;
}
```

Integer literals default to `i64`. An annotation narrows or
widens them if the value fits:

```prism runnable
fn main(): i64 {
    let x: i8 = 100;
    // let y: i8 = 300;   // error: does not fit
    return 0;
}
```

An `i8` and an `i32` cannot be added without an explicit cast:

```prism runnable
fn main(): i64 {
    let a: i8 = 10;
    let b: i8 = 20;
    let c = a + b;                 // i8
    let d = a as i32 + b as i32;   // i32
    return 0;
}
```

---

## §4 — Bindings and mutation

A binding is immutable by default. Reassignment requires
`let mut`:

```prism runnable
fn main(): i64 {
    let x = 5;
    // x = 10;         // error: cannot assign to immutable binding

    let mut y = 5;
    y = 10;
    return y;
}
```

Return value: `10`.

---

## §5 — Control flow

**`if` and `else`:**

```prism runnable
fn main(): i64 {
    let n = 7;
    if (n > 5) {
        return 100;
    } else {
        return 200;
    }
}
```

Return value: `100`.

**`while`:**

```prism runnable
fn main(): i64 {
    let mut i = 0;
    let mut sum = 0;
    while (i < 5) {
        sum = sum + i;
        i = i + 1;
    }
    return sum;
}
```

Return value: `10`.

**`for..range`:**

```prism runnable
fn main(): i64 {
    let mut sum = 0;
    for i in 0..5 {
        sum = sum + i;
    }
    return sum;
}
```

Return value: `10`.

**`for..in`:**

```prism runnable
fn main(): i64 {
    let mut sum = 0;
    for x in [10, 20, 30] {
        sum = sum + x;
    }
    return sum;
}
```

Return value: `60`.

---

## §6 — Functions

```prism runnable
fn add(a: i64, b: i64): i64 {
    return a + b;
}

fn main(): i64 {
    return add(3, 4);
}
```

Return value: `7`.

Function signatures can be forward-referenced: `main` may call a
function declared later in the file.

Recursion works without special syntax:

```prism runnable
fn fact(n: i64): i64 {
    if (n <= 1) { return 1; }
    return n * fact(n - 1);
}

fn main(): i64 {
    return fact(5);
}
```

Return value: `120`.

---

## §7 — Methods

A method is a function whose name is `Type.name`:

```prism runnable
type Point = struct { x: i64, y: i64 }

fn Point.sum(p: Point): i64 {
    return p.x + p.y;
}

fn main(): i64 {
    let p = Point { x: 3, y: 4 };
    return p.sum();
}
```

Return value: `7`.

The first parameter is the receiver. The call site uses method
syntax (`p.sum()`). There is no dynamic dispatch; methods are
resolved at compile time by the receiver's static type.

---

## §8 — Structs

```prism runnable
type Point = struct { x: i64, y: i64 }

fn main(): i64 {
    let mut p = Point { x: 1, y: 2 };
    p.x = 10;
    return p.x + p.y;
}
```

Return value: `12`.

Fields may be initialized in any order:

```prism runnable
type Point = struct { x: i64, y: i64 }

fn main(): i64 {
    let p = Point { y: 2, x: 1 };
    return p.x;
}
```

Return value: `1`.

---

## §9 — Enums and match

```prism runnable
type Color = enum { Red, Green, Blue }

fn main(): i64 {
    let c = Color.Green;
    match c {
        Color.Red   => { return 1; }
        Color.Green => { return 2; }
        Color.Blue  => { return 3; }
    }
    return 0;
}
```

Return value: `2`.

Variants can carry payloads:

```prism runnable
type Shape = enum {
    Circle(i64),
    Rect(i64, i64),
}

fn area(s: Shape): i64 {
    match s {
        Shape.Circle(r) => { return 3 * r * r; }
        Shape.Rect(w, h) => { return w * h; }
    }
    return 0;
}

fn main(): i64 {
    return area(Shape.Rect(3, 4));
}
```

Return value: `12`.

`match` must be exhaustive: every variant must be covered, or a
wildcard arm (`_`) must be present.

---

## §10 — Error handling

The prelude declares `Result<T, E>` and `Option<T>`:

```prism runnable
type Color = enum { Red, Green, Blue }   // user type, not from prelude

fn find(n: i64): Option<i64> {
    if (n > 0) { return Option.Some(n * 2); }
    return Option.None;
}

fn main(): i64 {
    match find(-1) {
        Option.Some(v) => { return v; }
        Option.None    => { return 0; }
    }
}
```

Return value: `0`.

The `?` operator propagates failures:

```prism runnable
fn half(n: i64): Result<i64, string> {
    if (n < 0) { return Result.Err("negative"); }
    return Result.Ok(n / 2);
}

fn quarter(n: i64): Result<i64, string> {
    let x = half(n)?;      // propagate Err
    let y = half(x)?;
    return Result.Ok(y);
}

fn main(): i64 {
    match quarter(20) {
        Result.Ok(v)   => { return v; }   // 5
        Result.Err(_)  => { return -1; }
    }
}
```

Return value: `5`.

`?` requires the enclosing function to have an annotated return
type that is `Result<_, _>` or `Option<_>`.

---

## §11 — Memory

Prism has two memory tiers: the **arena** (scope-bound, `&e`) and
the **RC heap** (shared, `rc e`).

`&e` produces a `ref<T>` that is valid until the enclosing scope
exits:

```prism runnable
fn main(): i64 {
    let x = 5;
    let p = &x;
    let y = *p;      // 5
    return y;
}
```

Return value: `5`.

`rc e` produces an `rc<T>` on the RC heap:

```prism runnable
fn make(): rc<i64> {
    return rc 42;
}

fn main(): i64 {
    let r = make();
    return *r;
}
```

Return value: `42`.

Three rules keep references from dangling:

1. A `ref<T>` cannot contain another reference.
2. A function cannot return a `ref<T>`.
3. A function cannot return a `*T`.

Aggregate types (`[T]`, `struct`, `enum`) cannot be returned
directly; use `rc<...>`.

```prism runnable
type Point = struct { x: i64, y: i64 }

fn make(): rc<Point> {
    return rc Point { x: 1, y: 2 };
}

fn main(): i64 {
    let p = make();
    return p.x + p.y;
}
```

Return value: `3`.

---

## §12 — Modules

Split a program across files:

`math.manu`:

```prism
pub fn double(x: i64): i64 {
    return x * 2;
}
```

`main.manu`:

```prism
using math

fn main(): i64 {
    return math.double(21);
}
```

Return value: `42`.

Three import forms:

- `using math` — whole-module import. Access as `math.double`.
- `using math as m` — with an alias.
- `using math.{ double }` — selective. Access as `double`
  directly.

Only `pub` declarations are visible from other files.

---

## §13 — FFI quickstart

Call a C function from `libc`:

```prism
extern lib "libc.so.6" as c   // Unix
// extern lib "ucrtbase.dll" as c   // Windows

extern fn strlen(s: *u8): u64 from c

fn main(): i64 {
    return strlen(c"hello") as i64;
}
```

Return value: `5`.

The `c"..."` syntax produces a C string (a NUL-terminated `*u8`
in the const arena). The FFI declaration names the library alias
after `from`.

Nullable C pointers use `Option<*T>`:

```prism
extern lib "libc.so.6" as c
extern fn getenv(name: *u8): Option<*u8> from c

fn main(): i64 {
    match getenv(c"PATH") {
        Option.Some(_) => { return 1; }
        Option.None    => { return 0; }
    }
}
```

Return value: `1` on any normal system.

A NULL return marshals to `Option.None`; a non-NULL return to
`Option.Some(ptr)`.

See `ffi.md` for the full FFI surface.

---

## §14 — Where to go next

- `reference.md` — the complete language. Every construct with
  its grammar, semantics, and edge cases.
- `patterns.md` — idioms and conventions for larger programs.
- `ffi.md` — the FFI, in depth.
- `errors.md` — every diagnostic the compiler can emit, with
  the trigger and the fix.