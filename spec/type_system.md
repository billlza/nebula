# Type System

This spec describes the current implementation behavior. It is not a design wish list.

## Status

Implemented:

- primitive types: `Int`, `Float`, `Bool`, `String`, `Void`
- user-defined `struct` and `enum` types
- item-level generics on `fn`, `struct`, and `enum`
- type arguments on generic type names
- inferred generic function instantiation
- callable types: `Fn(...) -> T` and `UnsafeFn(...) -> T`
- `ref` parameters as exclusive mutable borrow surfaces
- std-provided `Result<T, E>`, `Future<T>`, `Task<T>`, and `Bytes` where imported

Preview:

- bundled std domain types such as JSON, HTTP, net, fs, process, and task/runtime helpers
- UI semantic-tree types and adapters

Experimental:

- system-profile target policy; see `docs/system_profile.md`

Not yet implemented:

- traits or protocols
- interfaces or constrained generics
- explicit call-site generic specialization
- closures
- a language-level collection hierarchy
- lifetime parameters

## Type Names And Resolution

Type names resolve from:

- built-in primitive names
- same-module user items
- imported same-package modules
- dependency-qualified imports such as `dep::foo.bar`
- bundled std modules imported through `std::...`

Generic types require the correct number of explicit type arguments. Supplying type arguments to a
non-generic type is an error. Using a generic type without required type arguments is an error.

## Primitive And Literal Typing

- integer literals have type `Int`
- float literals have type `Float`
- boolean literals have type `Bool`
- string literals have type `String`
- `Void` is used for functions and externs that do not produce a value

Arithmetic is implemented for numeric operands. Modulo requires `Int` operands. Logical operators
require `Bool`. Comparisons over numeric operands produce `Bool`.

## Structs

Struct declarations introduce nominal types. The current surface supports:

- field declarations
- constructor-style calls when the callee resolves to a type
- field read and rooted field write
- first-level destructuring in `let`
- generic struct instantiation

No stable layout claim is made by this spec beyond the current generated-code behavior. ABI/layout
claims must go through `UOS-ABI-001` in `docs/universeos/gate_registry.md`.

## Enums

Enum declarations introduce nominal sum types. The current surface supports:

- payload variants
- zero-payload constructor use where the implementation supports expected-type context
- variant matching in statement-form and expression-form `match`
- first-level enum payload binding
- first-level struct destructuring inside enum payload patterns
- generic enum instantiation

`Result<T, E>` follows this enum convention through `std::result`.

## Functions, Methods, And Callables

Function signatures include parameter types, optional `ref`, unsafe metadata, and return type.
Omitted return type means `Void`.

Method syntax is call sugar. `obj.method(args)` resolves to a mapped function with `obj` in the
self position. There is no separate method item declaration syntax in the current language surface.
Representative callable, field, method, and assignment diagnostics include `NBL-T065`,
`NBL-T066`, `NBL-T067`, `NBL-T080`, `NBL-T081`, `NBL-T082`, `NBL-T083`, `NBL-T084`,
`NBL-T085`, and `NBL-T086`.

Callable values use:

- `Fn(...) -> T`
- `UnsafeFn(...) -> T`

Unsafe callable invocation requires an unsafe context as defined in `spec/safety_contract.md`.
Representative unsafe-boundary diagnostics include `NBL-U001`, `NBL-U002`, and `NBL-U003`.

## Generics

Current generics are monomorphized:

- type parameters belong to `fn`, `struct`, or `enum` items
- generic type use supplies explicit type arguments
- generic function calls infer type arguments from values and expected result context
- generic `extern fn` declarations are rejected

There is no variance model. There is no trait/protocol constraint language.
Representative generic diagnostics include `NBL-T122`, `NBL-T123`, and `NBL-T124`.

## Result And Async Types

`Result<T, E>` receives special treatment for postfix `?`:

- `NBL-T125`: enclosing function must return `Result<T, E>`
- `NBL-T126`: operand must be a `Result<T, E>`
- `NBL-T127`: propagated error type must match

`Future<T>` and `Task<T>` receive special treatment for `await`:

- `NBL-T132`: `await` outside async
- `NBL-T133`: awaited expression must be `Future<T>` or `Task<T>`
- `NBL-T134`: phase-1 async suspension rejects `ref` parameters

The current async runtime is hosted. It is not a freestanding scheduler or kernel facility.

## Open Design Questions

Traits/protocols:

- what constraint syntax should Nebula use?
- how should trait/protocol dictionaries or monomorphization interact with C ABI boundaries?
- how should trait/protocol conformance be documented for packages?

Lifetimes:

- whether explicit lifetime parameters are needed beyond lexical regions
- how lifetime names would interact with region names and strict-region diagnostics
- whether lifetime errors should remain separate from `NBL-R001` promotion diagnostics

Collections:

- which collection types belong in bundled std versus packages
- which collection operations are available in system/no-std profile
- how collection ownership and iteration interact with `ref` exclusivity

Closure capture:

- capture modes and ownership transfer rules
- whether async closures need distinct suspension restrictions
- how closure values interact with `Fn` and `UnsafeFn`
