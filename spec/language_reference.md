# Nebula Language Reference

This reference is an implementation-anchored overview for the current Nebula compiler. It links to
focused specifications for details and uses these status labels:

- `implemented`: accepted by the current compiler and covered by contract tests or existing specs
- `preview`: usable but intentionally narrow or not part of a broad platform promise
- `experimental`: present as an early gate or policy check, not a release support promise
- `planned`: documented direction only; not implemented
- `not yet implemented`: explicitly outside the current language surface

Normative grammar lives in `spec/grammar.ebnf`. Diagnostics are listed in `spec/diagnostics.md`.

## Lexical Model

Implemented:

- whitespace separates tokens and is otherwise insignificant
- line comments start with `//`
- block comments use `/* ... */`
- identifiers match `[a-zA-Z_][a-zA-Z0-9_]*`
- integer literals support decimal plus `0b`/`0B`, `0o`/`0O`, and `0x`/`0X` prefixes
- numeric separators use `_` between digits only
- float literals support decimal fractions and `e`/`E` scientific notation
- string literals support common escapes: `\n`, `\r`, `\t`, `\"`, and `\\`
- boolean literals are `true` and `false`

Not yet implemented:

- a Unicode identifier policy
- raw string literals
- character literals
- user-defined literal suffixes

## Modules, Imports, And Packages

Implemented:

- an optional `module foo.bar` declaration may appear at file top
- `import foo.bar` imports modules in the current package
- `import dep::foo.bar` imports modules from a package dependency
- `nebula.toml` and `nebula.lock` drive package/workspace dependency resolution
- dependency sources include path, git, installed packages, and exact-version registry entries
- compiler-shipped bundled std modules are imported through `std::...`

Current bundled std imports include `std::task`, `std::time`, `std::env`, `std::result`,
`std::log`, `std::bytes`, `std::fs`, `std::net`, `std::http`, `std::http_json`, and `std::json`.
The support posture for those modules is documented in `spec/tooling_cli.md` and
`docs/support_matrix.md`.

## Primitive Types

Implemented:

- `Int`
- `Float`
- `Bool`
- `String`
- `Void`

Preview library/domain types:

- `Bytes` is provided by `std::bytes`
- `Future<T>` and `Task<T>` are provided by `std::task`
- `Result<T, E>` is provided by `std::result` and is also recognized by postfix `?`

Not yet implemented:

- user-defined numeric widths
- a stable language-level collection hierarchy
- built-in nullable types

## Functions And Methods

Implemented:

- functions use `fn name(params) -> ReturnType { ... }`
- omitted return type means `Void`
- parameters may use `ref` for exclusive mutable borrow checks
- `main` may return `Void` or `Int`; `Int` maps to the host process exit code
- method sugar `value.method(args)` resolves to mapped functions with `self` in slot 0
- function types are `Fn(...) -> T` and `UnsafeFn(...) -> T`

Current restrictions:

- closures are not yet implemented
- explicit call-site generic specialization, such as `foo<Int>(x)`, is not implemented
- method declarations are not a separate item kind; current method syntax is call sugar

## Structs And Enums

Implemented:

- `struct Name { field: Type }`
- `enum Name { Variant(Type) }`
- generic `struct` and `enum` items
- field read/write through rooted field chains such as `x.f` and `x.f.g`
- rooted assignment targets such as `x.f = value`
- first-level `let` struct destructuring
- first-level enum payload destructuring in `match`
- bare zero-payload enum constructors in expected enum contexts

Current restrictions:

- recursive/nested pattern matching is not implemented
- match guards are not implemented
- assignment to a temporary-base postfix chain, such as `foo().x = value`, is not implemented

## Generics

Implemented:

- generic parameters on `fn`, `struct`, and `enum`
- explicit type arguments on generic types, for example `Pair<Int, String>`
- inferred generic function instantiation from arguments and expected result context
- monomorphized lowering through the current C++23 backend

Not yet implemented:

- traits, protocols, or interface-constrained generics
- variance rules
- runtime generic dispatch
- generic `extern fn` declarations

See `spec/generics_policy.md` and `spec/type_system.md`.

## Regions

Implemented:

- `region R { ... }` introduces a lexical allocation/lifetime domain
- region-origin values may be represented as region pointers in generated C++
- region values must not outlive their region unless promoted
- default escape behavior emits `NBL-R001` and auto-promotes to heap-safe representation
- `--strict-region` treats region escape as an error

Experimental:

- system-profile modes force strict-region behavior without requiring an explicit
  `--strict-region` flag

See `spec/region_model.md`, `spec/region_semantics.md`, `spec/escape_analysis.md`, and
`docs/system_profile.md`.

## Ownership And Promotion

Implemented:

- representation and ownership are tracked as a product model: stack, region, or heap
- heap ownership may be unique or shared
- explicit directives include `heap`, `promote`, `unique`, and `shared`
- default region escape promotion chooses unique ownership for single-owner flows
- alias fanout and conservative cross-function return paths promote to shared ownership
- promotion diagnostics expose machine-readable metadata through `NBL-R001`

Preview safety assist:

- borrow/exclusivity diagnostics such as `NBL-T090` through `NBL-T095`
- safe-subset shared-cycle diagnostics such as `NBL-S101`
- epistemic follow-up diagnostics such as `NBL-P010` and `NBL-X003`

See `spec/ownership_model.md`, `spec/static_analysis.md`, and `spec/safety_contract.md`.

## Unsafe Boundary

Implemented:

- `@unsafe fn` marks unsafe callable boundaries
- `unsafe { ... }` creates a scoped opt-in context
- safe calls to unsafe callables are rejected with `NBL-U001`
- `@unsafe` on non-function items is rejected with `NBL-U002`
- invalid external escape/ownership annotations are rejected with `NBL-U003`

The unsafe boundary marks where safe-subset guarantees stop. It is not a hardware, driver, or
kernel API.

See `spec/safety_contract.md`.

## Extern And FFI

Implemented:

- `extern fn` imports host-linked functions
- `@returns_fresh`, `@returns_paramN`, `@paramN_noescape`, `@paramN_may_escape`, and
  `@paramN_escape_unknown` describe narrow escape/ownership contracts for extern boundaries
- `@export @abi_c fn` exports top-level Nebula functions through the generated C ABI surface
- `nebula build --emit staticlib|sharedlib` emits a library and matching C header

Current C ABI restrictions:

- only top-level non-generic functions are exportable
- current ABI-safe types are `Int`, `Float`, `Bool`, and `Void`
- `String`, `Result`, `struct`, `enum`, callable types, `ref` parameters, generic exports, and
  exported `extern fn` declarations are rejected

See `spec/interop_c_abi.md`.

## Result And Error Propagation

Implemented:

- `Result<T, E>` follows the `Ok(T)` / `Err(E)` convention from `std::result`
- postfix `?` requires the enclosing function to return `Result<T, E>`
- `?` unwraps `Ok` and propagates compatible `Err`
- mismatches are diagnosed with `NBL-T125`, `NBL-T126`, and `NBL-T127`

Not yet implemented:

- exceptions
- a generalized effect system

## Async And Task Model

Implemented:

- `async fn`
- prefix `await expr`
- `spawn(fut)`
- async `main`, `@test`, and `@bench` harness paths
- single-thread cooperative runtime backed by the bundled runtime and reactor I/O
- `await` diagnostics such as `NBL-T132`, `NBL-T133`, and `NBL-T134`

Current restrictions:

- async blocks and async move blocks are not implemented
- phase-1 async functions reject suspension with `ref` parameters
- the current runtime is hosted; it is not a freestanding scheduler or kernel facility

## Match And Control Flow

Implemented:

- `if` / `else`
- `for` integer ranges with `..`, `..<`, and `...`
- `while`
- `break` and `continue`
- statement-form `match`
- expression-form `match`
- Bool and enum exhaustiveness checks for supported patterns

Current restrictions:

- pattern guards are not implemented
- recursive patterns are not implemented
- top-level struct match arms are not implemented

## System Profile Relationship

Experimental:

- `--profile system`
- `--target system`
- `--target freestanding`
- target triples containing `-none`
- `--no-std`
- `--panic abort|trap`

The current system profile is a compiler/CLI contract gate. It rejects hosted bundled std imports,
forces strict-region diagnostics, rejects unwind policy, and records target/profile/panic markers in
generated C++ artifacts.

It does not implement a freestanding runtime, no-std standard library, kernel target, boot path,
driver API, interrupt model, MMU model, syscall ABI, scheduler, or backend-independent object-code
pipeline.

See `docs/system_profile.md`, `docs/universeos_convergence.md`, and
`docs/universeos/gate_registry.md`.

## Related Specifications

- `spec/type_system.md`
- `spec/ownership_model.md`
- `spec/region_model.md`
- `spec/static_analysis.md`
- `spec/safety_contract.md`
- `spec/tooling_cli.md`
- `docs/system_profile.md`
- `docs/universeos_convergence.md`
