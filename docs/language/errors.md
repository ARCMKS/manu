## `docs/language/errors.md`

# Diagnostics Catalog

*Current as of commit `<hash>`, <date>.*

Every diagnostic the compiler can emit. Grouped by phase; within
each group, ordered roughly by frequency.

Each entry: the message, the trigger, and a fix hint. The message
text is a template; `%s` and `%d` placeholders are filled by the
compiler.

Many diagnostics are caught at multiple phases. The "Emitted by"
line names the file where the current implementation reports it;
a future reorganization might move a diagnostic without changing
its text.

For the escape-rule diagnostics and the memory-model rationale
behind them, see `reference.md` §2.3 and §10.

---

## §1 — Lexer and parser

### `unexpected character`

Emitted by: `parser.c`, in `advance`.

Trigger: the lexer produced a `TOK_ERROR` (an unterminated
string or an unrecognized byte).

Fix: check the source for a missing closing quote or a
non-ASCII byte. Strings cannot contain escaped quotes (`\"`); use
concatenation.

### `expected <what>, got <kind>`

Emitted by: `parser.c`, in `expect`.

Trigger: the parser expected a specific token and found a
different one.

Fix: the `<what>` names what was expected; check the syntax
around the given location.

### `expected expression, got <kind>`

Emitted by: `parser.c`, in `primary`.

Trigger: a token that cannot begin an expression appeared where
one was expected.

### `expected type`

Emitted by: `parser.c`, in `parse_type_expr`.

Trigger: a token that cannot begin a type appeared in a type
position.

### `expected 'struct' or 'enum' after '='`

Emitted by: `parser.c`, in `type_declaration`.

Trigger: `type X = ...` where `...` is not `struct` or `enum`.

### `expected 'fn', 'const', 'type', 'using', 'extern' or 'pub' at top level, got <kind>`

Emitted by: `parser.c`, in `parse_program`.

Trigger: a top-level token that is not a declaration keyword.

### `expected 'fn', 'const', or 'type' after 'pub'`

Emitted by: `parser.c`, in `pub_declaration`.

Trigger: `pub` followed by anything other than `fn`, `const`,
`type`, or `extern`.

### `expected 'lib' or 'fn' after 'extern'`

Emitted by: `parser.c`, in `extern_declaration`.

Trigger: `extern` followed by anything other than `lib`, `fn`,
or `type`.

### `too many parameters (max 8)`

Emitted by: `parser.c`.

Trigger: a function declaration with more than 8 parameters.
The extras are parsed and discarded; the first 8 are kept.

### `too many type parameters`

Emitted by: `parser.c`.

Trigger: a generic declaration with more than 8 type parameters.

### `type path too long` / `module path too long`

Emitted by: `parser.c`.

Trigger: a dotted path with more than 16 components.

### `integer literal too large`

Emitted by: `parser.c`, in `parse_int_value`.

Trigger: an integer literal that does not fit in `u64`.

### `extern fn requires a return type annotation`

Emitted by: `parser.c`.

Trigger: an `extern fn` without a `: T` clause.

### `extern parameters cannot be marked 'mut'`

Emitted by: `parser.c`.

Trigger: `mut` on an extern function parameter. Extern functions
have no body; mutability is meaningless.

### `match arm body cannot be empty`

Emitted by: `parser.c`.

### `expected '(', '.', or '{' after type arguments`

Emitted by: `parser.c`.

Trigger: after `<...>`, the parser expected `(`, `.`, or `{`.
The generic disambiguation accepted the `<` as a generic opener
but the following token is not one of the three valid
continuations.

---

## §2 — Types

### `unknown type '<name>'`

Emitted by: `types.c`, in `resolve_inner`.

Trigger: a type name that does not resolve.

Fix: declare the type, import it, or check spelling.

### `unknown generic type '<name>'`

Emitted by: `sema_mono.c`.

Trigger: a generic type name that is not registered as a template.

### `generic '<name>' expects N type argument(s), got M`

Emitted by: `types.c` and `sema_mono.c`.

Trigger: an instantiation with the wrong number of arguments.

### `generic type '<name>' requires type arguments`

Emitted by: `types.c`.

Trigger: a bare generic name in a type position.

### `too many type arguments (max 8)`

Emitted by: `sema_mono.c`.

### `type paths with more than two components are not yet supported`

Emitted by: `types.c`, in `resolve_inner`.

Trigger: `a.b.c.Type`.

Fix: use a selective import to shorten the path.

### `'<module>.<name>' is not a type`

Emitted by: `types.c`, in `TEX_QUALIFIED`.

Trigger: a qualified name whose second component is not a type.

### `type '<name>' is private; declared in <path> and not marked 'pub'`

Emitted by: `types.c`.

Trigger: cross-file reference to a non-`pub` type.

### `generic instantiation is not available in this context`

Emitted by: `types.c`.

Trigger: an internal error; `types.c` was called without a sema
context.

### `unknown type in variant pattern`

Emitted by: `sema.c`, in `analyze_pattern`.

Trigger: a variant pattern names a type that does not resolve.

### `variant pattern names type '<a>' but scrutinee is '<b>'`

Emitted by: `sema.c`, in `analyze_pattern`.

### `duplicate type '<name>'`

Emitted by: `sema_types.c`.

Trigger: two types with the same name. There is no per-file type
namespace; names must be globally unique.

### `extern types cannot have type parameters`

Emitted by: `sema_types.c`.

### `struct fields cannot be of type ref<T>; references are scope-bound and would dangle`

Emitted by: `sema_types.c`.

### `struct fields cannot hold extern struct values; use *T to store a pointer`

Emitted by: `sema_types.c`.

### `duplicate variant '<name>'`

Emitted by: `sema_types.c`.

### `enum '<name>' has more than N variants (max 128)`

Emitted by: `sema_types.c`.

### `variant '<name>' has N payload(s); max is 16`

Emitted by: `sema_types.c`.

### `enum payloads cannot be of type ref<T>`

Emitted by: `sema_types.c`.

### `enum payloads cannot hold extern struct values; use *T`

Emitted by: `sema_types.c`.

---

## §3 — Escape rules

### `a reference cannot contain another reference`

Emitted by: `types.c`, in `TEX_REF`.

Trigger: `ref<ref<T>>`, `ref<rc<T>>`, `ref<[T]>` where `T`
contains a `ref`, etc.

### `rc<T> cannot contain a reference`

Emitted by: `types.c`, in `TEX_RC`.

Trigger: `rc<ref<T>>`, `rc<rc<ref<T>>>`, etc.

### `a raw pointer cannot point to a reference type`

Emitted by: `types.c`, in `TEX_RAWPTR`.

Trigger: `*ref<T>`.

### `array element type cannot be a reference`

Emitted by: `types.c`, in `TEX_ARRAY`.

Trigger: `[ref<T>]`.

### `ref<T> cannot contain an extern struct`

Emitted by: `types.c`.

Trigger: `ref<ExternStruct>`.

### `rc<T> cannot contain an extern struct`

Emitted by: `types.c`.

Trigger: `rc<ExternStruct>`.

### `array element type cannot contain an extern struct`

Emitted by: `types.c`.

Trigger: `[ExternStruct]`.

### `cannot take a reference to a reference-typed value`

Emitted by: `sema.c`, in `AST_ALLOC_ARENA`.

Trigger: `&e` where `e`'s type contains a `ref`.

### `function cannot return a reference; use rc<T> for values that outlive this function`

Emitted by: `sema.c`, in `AST_RETURN`.

Trigger: a return statement with a `ref<T>` value.

### `raw pointers cannot be returned from functions in v1`

Emitted by: `sema.c`, in `AST_RETURN`.

Trigger: a return statement with a `*T` value.

### `function cannot return an array value; use rc<[...]> for arrays that outlive this function`

Emitted by: `sema.c`.

### `function cannot return a struct value; use rc<StructName> for structs that outlive this function`

Emitted by: `sema.c`.

### `function cannot return an enum value; use rc<EnumName> for enums that outlive this function`

Emitted by: `sema.c`.

### `cannot return a borrowed value; return a fresh expression (concatenate, allocate, or call a function that produces a new value)`

Emitted by: `sema.c`, in `AST_RETURN`.

Trigger: returning an RC value from a local, a field, an index,
or a deref.

### `cannot bind an extern struct value; extern structs are C memory with no Prism value representation`

Emitted by: `sema.c`, in `AST_LET`.

---

## §4 — Variables and assignments

### `undefined variable '<name>'`

Emitted by: `sema.c`, in `AST_VAR`.

### `cannot assign to immutable binding '<name>'; declare with 'let mut'`

Emitted by: `sema.c`, in `AST_ASSIGN` / `AST_ASSIGN_OP`.

### `cannot assign to constant '<name>'`

Emitted by: `sema.c`.

### `assignment to undefined variable '<name>'`

Emitted by: `sema.c`.

### `cannot assign through an immutable reference; use '&mut x' and 'ref<mut T>'`

Emitted by: `sema.c`.

### `cannot assign through a value of type <T>`

Emitted by: `sema.c`.

### `cannot assign to index of immutable binding '<name>'`

Emitted by: `sema.c`.

### `cannot assign to field of immutable binding '<name>'`

Emitted by: `sema.c`.

### `cannot assign to a nested extern struct field; assign to its individual fields`

Emitted by: `sema.c`.

### `invalid assignment target`

Emitted by: `sema.c`.

Trigger: `= <not-an-lvalue>`.

### `too many locals`

Emitted by: `sema.c`.

### `too many nested scopes (max 256)`

Emitted by: `sema.c`.

---

## §5 — Expressions

### `cannot implicitly convert <A> to <B>; use an explicit cast`

Emitted by: `sema.c`, in `coerce_to_target`.

Trigger: a value that does not fit the target type.

### `operands have different types: <A> and <B>`

Emitted by: `sema.c`, in `AST_BINARY`.

### `cannot combine a string with a non-string operand: <A> and <B>`

Emitted by: `sema.c`, in `AST_BINARY`.

### `operator '<op>' is not defined for <T>`

Emitted by: `sema.c`, in `AST_BINARY` / `AST_UNARY`.

### `cannot negate unsigned type <T>`

Emitted by: `sema.c`, in `AST_UNARY`.

### `expected bool, found <T>`

Emitted by: `sema.c`, in conditions.

### `literal <N> does not fit in <T>`

Emitted by: `sema.c`, in `AST_INT`.

### `cannot bind a void value to a variable`

Emitted by: `sema.c`, in `AST_LET`.

### `cannot index a value of type <T>`

Emitted by: `sema.c`, in `AST_INDEX`.

### `array index must be an integer, found <T>`

Emitted by: `sema.c`.

### `strings are immutable; cannot assign to s[i]`

Emitted by: `sema.c`.

### `empty array literals are not yet supported`

Emitted by: `sema.c`, in `AST_ARRAY_LIT`.

### `array element N has type <T>, expected <U>`

Emitted by: `sema.c`.

### `cannot access field on type <T>`

Emitted by: `sema.c`, in `AST_FIELD`.

### `struct '<S>' has no field '<name>'`

Emitted by: `sema.c`.

### `extern struct '<S>' has no field '<name>'`

Emitted by: `sema.c`.

### `cannot access field of opaque type '<T>'; its layout is not known to Prism, use the C API to manipulate its contents`

Emitted by: `sema.c`.

### `cannot dereference a value of type <T>`

Emitted by: `sema.c`, in `AST_DEREF`.

### `cannot dereference *<Opaque>; opaque types have no value representation`

Emitted by: `sema.c`.

### `cannot dereference *<ExternStruct>; extern structs have no value representation`

Emitted by: `sema.c`.

### `cannot cast from <A> to <B>`

Emitted by: `sema.c`, in `AST_CAST`.

### `sizeof(void) is not defined`

Emitted by: `sema.c`, in `AST_SIZEOF`.

### `sizeof of opaque type '<T>' is not defined; use sizeof(*<T>) for the pointer size`

Emitted by: `sema.c`.

### `value of type <T> is already RC-managed; 'rc' is not needed here`

Emitted by: `sema.c`, in `AST_ALLOC_RC`.

### `rc on a <T> variable is not yet supported; use 'rc <literal>' directly`

Emitted by: `sema.c`, in `AST_ALLOC_RC`.

---

## §6 — Function calls and methods

### `unknown function '<name>'`

Emitted by: `sema.c`, in `AST_CALL`.

### `function '<name>' expects N argument(s), got M`

Emitted by: `sema.c`.

### `function value expects N argument(s), got M`

Emitted by: `sema.c`.

### `cannot infer return type of '<name>' at this call site; add a return type annotation or move this call after '<name>''s definition`

Emitted by: `sema.c`.

Trigger: an unannotated function's return type is not yet
established when its call site is analyzed.

### `cannot take the value of function '<name>' before its return type is known`

Emitted by: `sema.c`.

### `extern function '<name>' cannot be used as a value; externs are not in the codegen function table`

Emitted by: `sema.c`.

### `generic function '<name>' must be instantiated with explicit type arguments before being used as a value`

Emitted by: `sema.c`.

### `cannot provide type arguments to a function value`

Emitted by: `sema.c`.

### `'<name>' is private; declared in <path> and not marked 'pub'`

Emitted by: `sema.c`, in `AST_CALL` and `validate_selective_imports`.

### `no method '<name>' on type <T>`

Emitted by: `sema.c`, in `AST_VARIANT_CTOR` (method path).

### `cannot call method on value of type <T>`

Emitted by: `sema.c`.

### `method expects N arg(s), got M`

Emitted by: `sema.c`.

### `unknown method owner type '<name>'`

Emitted by: `sema.c`, in Pass A.

### `method '<T>.<name>' is already defined`

Emitted by: `sema.c`.

### `function '<name>' is already defined in this file`

Emitted by: `sema.c`.

### `too many functions (max 64)`

Emitted by: `sema.c`.

---

## §7 — Enums, structs, variants

### `unknown struct '<name>'`

Emitted by: `sema.c`, in `AST_STRUCT_LIT`.

### `'<name>' is a struct; use '<name> { field: value, ... }'`

Emitted by: `sema.c`, in `AST_VARIANT_CTOR`.

### `'<name>' is an enum; use '<name>.Variant(...)'`

Emitted by: `sema.c`, in `AST_STRUCT_LIT`.

### `'<name>' is opaque and cannot be constructed`

Emitted by: `sema.c`.

### `unknown type or module '<name>'`

Emitted by: `sema.c`, in `AST_VARIANT_CTOR`.

### `cannot infer type arguments for '<name>' at this call site; annotate the binding, the enclosing function's return type, or use explicit '<...>'`

Emitted by: `sema.c`, in `AST_VARIANT_CTOR`.

### `enum '<E>' has no variant '<name>'`

Emitted by: `sema.c`.

### `variant '<E>.<v>' takes N payload(s), got M`

Emitted by: `sema.c`.

### `variant '<E>.<v>' takes N payload(s); use '<E>.<v>(...)'`

Emitted by: `sema.c`, in `AST_FIELD` (unit-variant path).

### `duplicate initializer for field`

Emitted by: `sema.c`, in `AST_STRUCT_LIT`.

### `missing initializer for field '<name>'`

Emitted by: `sema.c`.

### `field '<name>' expects <A>, got <B>`

Emitted by: `sema.c`.

---

## §8 — Patterns and matches

### `integer pattern doesn't match type <T>`

Emitted by: `sema.c`, in `analyze_pattern`.

### `pattern value out of range for <T>`

Emitted by: `sema.c`.

### `bool pattern doesn't match type <T>`

Emitted by: `sema.c`.

### `string pattern doesn't match type <T>`

Emitted by: `sema.c`.

### `variant pattern doesn't match type <T>`

Emitted by: `sema.c`.

### `variant pattern type does not match scrutinee`

Emitted by: `sema.c`.

### `variant '<E>' takes N payload(s), pattern has M`

Emitted by: `sema.c`.

### `cannot match on a void value`

Emitted by: `sema.c`, in `AST_MATCH`.

### `match is not exhaustive: variant '<v>' not covered`

Emitted by: `sema.c`, in `check_exhaustive`.

### `match on bool is not exhaustive: missing '<true/false>'`

Emitted by: `sema.c`.

### `match on <T> is not exhaustive; add a '_' arm`

Emitted by: `sema.c`.

### `'break' outside of a loop`

Emitted by: `sema.c`.

### `'continue' outside of a loop`

Emitted by: `sema.c`.

---

## §9 — Constants

### `constant initializer must be a literal (integer, float, or boolean)`

Emitted by: `sema.c`, in `const_from_literal`.

### `string constants are not yet supported`

Emitted by: `sema.c`.

### `duplicate constant '<name>'`

Emitted by: `sema.c`.

### `too many constants (max 256)`

Emitted by: `sema.c`.

### `constant is an integer but annotated as <T>`

Emitted by: `sema.c`.

### `constant value does not fit in <T>`

Emitted by: `sema.c`.

### `constant is bool but annotated as <T>`

Emitted by: `sema.c`.

### `constant is f64 but annotated as <T>`

Emitted by: `sema.c`.

---

## §10 — Modules and imports

### `cannot find module '<path>'`

Emitted by: `module.c`.

### `circular import involving '<path>'`

Emitted by: `module.c`.

### `cannot read '<path>'`

Emitted by: `module.c`.

### `cannot find '<path>'`

Emitted by: `module.c`, in `modload_load` for the root file.

### `nested symbol paths deeper than one level are not supported; use 'using a.b.c.{ Symbol }'`

Emitted by: `sema_modules.c`.

### `module '<m>' has no item '<name>'`

Emitted by: `sema_modules.c`, in `validate_selective_imports`.

### `module '<m>' has no public item '<name>'`

Emitted by: `sema_modules.c`.

### `'<m>.<name>' is private; declared in <path> and not marked 'pub'`

Emitted by: `sema_modules.c`.

---

## §11 — FFI

### `unknown library alias '<alias>'`

Emitted by: `sema.c`, in extern registration.

### `type <T> is not FFI-compatible`

Emitted by: `sema_ffi.c`, via `ffi_kind_from_type`.

### `extern functions cannot return an extern struct by value; use '*<T>'`

Emitted by: `sema.c`.

### `extern functions cannot take an extern struct by value; use '*<T>'`

Emitted by: `sema.c`.

### `parameter has type <T>, which is not FFI-compatible`

Emitted by: `sema.c`.

### `extern '<name>' expects N argument(s), got M`

Emitted by: `sema.c`.

### `extern '<name>' expects at least N argument(s), got M`

Emitted by: `sema.c` (variadic).

### `variadic argument N has type <T>; only integers and pointers are allowed in variadic position`

Emitted by: `sema.c`.

### `too many extern functions (max 128)`

Emitted by: `sema.c`.

### `too many extern lib declarations (max 16)`

Emitted by: `sema_ffi.c`.

---

## §12 — Codegen internal errors

These indicate a compiler bug; they should not fire on valid
input.

- `internal: codegen saw a NULL type` (warning)
- `internal: const id out of range`
- `internal: function value has no codegen index`
- `internal: unresolved identifier in codegen; this is a bug in sema, please report`
- `internal: unknown pattern kind`
- `codegen: invalid assignment target`
- `codegen: unexpected expression node`
- `codegen: unexpected statement node`
- `codegen: 'break' outside loop`
- `codegen: 'continue' outside loop`
- `too many nested loops (max 32)`
- `too many 'break's in one loop (max 64)`
- `too many 'continue's in one loop (max 64)`
- `no 'main' function declared`

---

## §13 — Runtime errors

These are reported by the VM at execution time. The message goes
to stderr; the exit code is 2.

### Arithmetic and value errors

- `division by zero`
- `cast of NaN to integer`
- `cast overflow`
- `cast of negative to unsigned`

### Stack and frame errors

- `pop on empty stack`
- `dup on empty stack`
- `dup2 on shallow stack`
- `stack underflow on call`
- `stack underflow on indirect call`
- `call stack overflow`
- `stack overflow extending locals`
- `bad local load slot N`
- `bad address-of-local slot N`
- `local store slot N out of range`

### Memory errors

- `null dereference`
- `null store through reference`
- `null address-of`
- `arena out of memory (request=N)`
- `rc heap out of memory`
- `arena out of memory for array (n=N)`
- `rc heap out of memory (array n=N)`
- `arena out of memory for enum`
- `rc heap out of memory for enum`
- `const arena overflow`

### Index and length errors

- `index through null`
- `index N out of bounds (length M)`
- `index load not supported for desc kind N`
- `index-store not supported for desc kind N`
- `len not supported for desc kind N`

### Scope errors

- `scope stack overflow`
- `scope stack underflow`
- `return outside function`
- `return with no value`

### Type dispatch errors

- `binary not supported for descriptor kind N`
- `unary not supported for descriptor kind N`
- `operator not supported for strings`
- `operator not supported for pointers`
- `bad binary sub-op N`
- `bad unary sub-op N on signed int`
- `bad unary sub-op N on f64`
- `bad unary sub-op N on bool`
- `bad mem_kind N`
- `conversion to descriptor kind N not supported`
- `cannot convert to signed integer`
- `cannot convert to unsigned integer`
- `cannot convert to f64`
- `cannot convert to f32`
- `cannot convert to bool`

### Reference counting errors

- `rc_inc on empty stack`
- `rc_dec on empty stack`
- `bad rc_dec desc N`

### FFI errors

- `bad ffi function index N`
- `ffi call with N args exceeds 16`
- `cannot load '<path>' (error N)` (Windows)
- `cannot load '<path>': <dlerror>` (Unix)
- `symbol '<name>' not found in '<path>'`
- `symbol '<name>' not found in '<path>': <dlerror>`
- `libffi cif prep failed for '<name>'`
- `libffi vararg cif prep failed for '<name>'`
- `FFI support not compiled in`

### Internal / defensive errors

- `unknown opcode N`
- `bad native index N`
- `bad constant index N`
- `unknown constant kind N`
- `bad len desc N`
- `bad binary type idx N`
- `bad unary type idx N`
- `bad call target N`
- `bad indirect call target N`
- `bad convert descriptors`
- `bad index load desc N`
- `bad index store desc N`
- `bad rc_dec desc N`
- `bad print desc N`
- `bad ffi function index N`

### Trap

- Any message emitted by `OP_TRAP`. Currently `match: no arm
  matched`, which is the defensive fall-through after a match's
  last arm.
