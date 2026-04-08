# Language Core (v1.0.0)

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

Core statement kinds:
- `let`, assignment, expression statement, `return`, `region`, `unsafe`, `if`, `for`

Expression highlights:
- arithmetic with unary sign (`+x`, `-x`) and binary operators including `%`
- boolean literals and boolean conditions (`true`, `false`)
- comparisons / logical operators (`==`, `!=`, `<`, `<=`, `>`, `>=`, `&&`, `||`, `!`)
- call forms: direct call and method sugar (`x.m(...)`)
- ownership/representation directives: `shared`, `unique`, `heap`, `promote`

Project/module surface:
- optional `module foo.bar` declaration at file top
- zero or more `import foo.bar` declarations before items
- dependency-qualified imports use `import dep::foo.bar`
- `extern fn` declarations for host-linked call surfaces

Current intentional restrictions:
- field/method base is identifier-only (`x.f`, `x.m(...)`)
- chained temporary bases are not yet part of the core (`a.b.c`, `foo().m()`)

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
