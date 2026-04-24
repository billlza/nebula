# Language Core

Nebula language core is intentionally small and explicit:
- syntax and type surface
- region lifetime model
- Rep x Owner semantics
- unsafe boundary model

Borrow/exclusivity checks are **not** the language identity. They are a conservative safety assist layer documented under Static Analysis.

## 1. Syntax surface

Normative grammar: `spec/grammar.ebnf`.

Core item kinds:
- `fn`, `struct`, `enum`
- item-level generics on `fn`, `struct`, and `enum`

Core statement kinds:
- `let`, assignment, expression statement, `return`, `region`, `unsafe`, `if`, `for`, `while`, `break`, `continue`, statement-form and expression-form `match`

Expression highlights:
- arithmetic with unary sign (`+x`, `-x`) and binary operators including `%`
- boolean literals and boolean conditions (`true`, `false`)
- comparisons / logical operators (`==`, `!=`, `<`, `<=`, `>`, `>=`, `&&`, `||`, `!`)
- call forms: direct call and method sugar (`x.m(...)`)
- `Result<T, E>`-style error flow with postfix `?`
- async control-flow core: `async fn`, prefix `await expr`, and `spawn(fut)`
- ownership/representation directives: `shared`, `unique`, `heap`, `promote`

Pattern/control-flow highlights:
- statement-form and expression-form `match expr { ... }`
- loop forms: `for`, `while`, `break`, `continue`
- supported arm patterns in the current slice:
  - `_`
  - `true` / `false`
  - enum variant match with optional first-level payload binding, for example `Some(x)`, `Some(_)`, `None`
  - enum payload first-level struct destructuring, for example `Some({ x, y: y0, z: _ })`

Binding/postfix highlights:
- `let` struct destructuring with first-level field projection, for example `let { x, y: y0, z: _ } = pair`
- postfix on temporary bases is supported for reads and method sugar, for example `foo().x`, `foo().m()`, `foo().x.y`
- zero-payload enum constructors can be written bare in expected enum contexts, for example `return None`

Generic highlights:
- generic `struct`, `enum`, and `fn` declarations are supported
- generic types use explicit type arguments, for example `Pair<Int, String>`
- generic function instantiation is inferred from arguments and available expected-type context
- generic lowering remains monomorphized through the current C++ backend

Error-flow highlights:
- `?` requires the enclosing function to return `Result<T, E>`
- `?` preserves the operand error payload type and yields the `Ok` payload on success
- `Result<T, E>` constructors work with existing expected-type enum construction rules, for example
  `return Ok(x)` inside a `Result<Int, String>` function
- `?` is valid inside ordinary expressions, including `match` expression arms, as long as the
  enclosing function still returns a compatible `Result`

Project/module surface:
- optional `module foo.bar` declaration at file top
- zero or more `import foo.bar` declarations before items
- dependency-qualified imports use `import dep::foo.bar`
  the package-qualified form is stable across `path`, `git`, `installed`, and exact-version
  registry-backed dependencies; changing the package source does not introduce a new import syntax
- compiler-shipped bundled std imports use `import std::task`, `import std::time`, `import std::env`, `import std::result`, `import std::log`, `import std::bytes`, `import std::fs`, `import std::net`, `import std::http`, `import std::http_json`, and `import std::json`
- `extern fn` declarations for host-linked call surfaces
- `@export @abi_c fn ...` marks a Nebula-defined function for the narrow generated C ABI surface

Current intentional restrictions:
- async blocks / async move blocks are not in the shipped phase-1 executable slice yet
- rooted and temporary-base postfix chains are supported for reads / method sugar
- assignment targets still require a rooted base (`x.f = v`, not `foo().x = v`)
- patterns are intentionally first-level only; no guards, top-level struct arms, or nested recursive patterns
- function calls do not yet expose explicit type-argument syntax such as `foo<Int>(x)`
- generic `extern fn` is intentionally rejected until the host ABI contract is defined
- first-generation C ABI export only supports top-level non-generic functions over `Int/Float/Bool/Void`

## 2. Type and unsafe boundary

Unsafe boundary contract:
- `@unsafe fn` defines unsafe callable boundaries
- `unsafe { ... }` is explicit opt-in scope
- safe context calling unsafe callable: `NBL-U001`
- `@unsafe` on non-function items: `NBL-U002`

Function type surface:
- `Fn(...) -> T`
- `UnsafeFn(...) -> T`

## 3. Region model

- `region R { ... }` introduces a lexical allocation/lifetime domain
- region-origin values cannot outlive region semantics without promotion
- escape policy:
  - default: emit `NBL-R001` and auto-promote
  - `--strict-region`: treat escape as error

Region details are specified in `spec/region_semantics.md`.

## 4. Rep x Owner model

Representation axis:
- `StackValue(T)`
- `RegionPtr(R,T)`
- `HeapPtr(T)`

Owner axis (heap only):
- `Unique(T)`
- `Shared(T)`

Auto-promotion owner selection is implementation-defined but stable enough for tooling:
- default single-owner flow -> heap unique
- alias fanout / uncertain cross-function return path -> heap shared

Detailed inference contract: `spec/rep_owner_model.md`.

## 5. Language core identity

Nebula core positioning:
- explicit regions + Rep x Owner semantics are primary design center
- unsafe boundaries are explicit and opt-in
- borrow/exclusivity is supportive analysis, not the core narrative
