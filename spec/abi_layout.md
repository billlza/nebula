# ABI And Layout

Status: specification of current hosted C++23 behavior plus future requirements. This document does
not change backend behavior.

Nebula's current supported pipeline is source to C++23 to a host C++ compiler. ABI and layout claims
must therefore distinguish current hosted C++ representation from future freestanding or
backend-independent object representation.

## Current Hosted C++23 Representation

| Nebula type | Current emitted C++ representation | C ABI export status |
| --- | --- | --- |
| `Int` | `std::int64_t` | exported as `int64_t` |
| `Float` | `double` | exported as `double` |
| `Bool` | `bool` | exported as `bool` |
| `Void` | `void` | exported as `void` |
| `String` | `std::string` internally | not ABI-safe for public C exports |
| `struct` | hashed nominal C++ `struct __nebula_ty_<hash>_<Name>` | not ABI-safe for public C exports |
| `enum` | hashed nominal C++ struct with nested variant structs and `std::variant` storage | not ABI-safe for public C exports |
| callable types | C++ callable/runtime representation | not ABI-safe for public C exports |

`String`, `struct`, `enum`, `Result`, callable types, `ref` parameters, generic functions, and
exported `extern fn` declarations are rejected from the public C ABI by the current typechecker and
library build path.

## Struct Layout

Current hosted behavior:

- generated C++ struct fields appear in Nebula source order
- constructor parameters and initializer list follow the same order
- type names are stable only through the compiler's generated C++ identity scheme, not through a
  public ABI name
- padding, alignment, standard-layout status, and cross-compiler binary compatibility are not
  specified as Nebula language guarantees

Future ABI work must decide:

- alignment rules for every primitive and aggregate type
- padding insertion and whether padding bytes are observable
- whether layout is target-specific or target-independent
- how `repr(C)`-like layout, if added, interacts with `@export @abi_c`

## Enum Layout

Current hosted behavior:

- generated C++ enums are represented as a wrapper struct
- each variant becomes a nested struct
- payload variants store their payload in fields inside the nested variant struct
- the active payload is stored in `std::variant`
- zero-payload variants are represented as empty nested structs

This is a hosted C++ representation, not a public ABI. Future freestanding work must define tags,
payload placement, niches, padding, and invalid states before enums can cross a stable ABI.

## C ABI Export Rules

Current C ABI exports are opt-in:

```nebula
@export
@abi_c
fn add(a: Int, b: Int) -> Int {
  return a + b
}
```

Current rules:

- both `@export` and `@abi_c` are required
- annotations are valid only on Nebula-defined functions
- exported functions cannot be generic
- exported functions cannot use `ref` parameters
- only `Int`, `Float`, `Bool`, and `Void` are ABI-safe today
- library builds export only root-package annotated functions
- `host_cxx` and `[native]` sources are rejected for current public library ABI builds
- duplicate sanitized exported symbol names are rejected

## Symbol Naming

Current generated C ABI wrapper names use readable sanitized names:

```text
nebula_<package>_<module>_<function>
```

Adjacent qualified segments that normalize to the same text may be collapsed by the current codegen
scheme. Internal C++ implementation symbols are hashed and are not public ABI names.

## Current Codegen Audit

The current compiler enforces and emits C ABI exports through:

- `frontend/typecheck.cpp`: validates `@export @abi_c`, rejects missing pairs, non-functions,
  extern exports, generics, `ref` parameters, and non-scalar ABI types.
- `codegen/cpp_backend.cpp`: collects exported functions, emits `extern "C"` wrappers, and emits
  the generated C header.
- `cli/build_run.cpp`: in `build --emit staticlib|sharedlib`, rejects library builds with no
  root-package exports, rejects duplicate public symbols, rejects `host_cxx`, rejects `[native]`,
  emits the header next to the library artifact, and writes artifact metadata.

## No-Std And Freestanding Requirements

Before ABI/layout can support a real system profile, Nebula needs:

- target-specific primitive width and alignment tables
- a no-std-safe representation for strings and bytes or an explicit rejection policy
- stable aggregate layout rules
- enum tag and payload rules
- panic ABI rules for `abort` and `trap`
- startup entry and symbol naming rules
- linker and object-file expectations
- C ABI export behavior that does not depend on hosted C++ runtime facilities

## Golden Tests To Implement Next

Current hosted C++23 golden tests:

- `ABI-001-exported-scalar-function`
- `ABI-002-struct-field-order-codegen`
- `ABI-003-enum-payload-lowering-codegen`
- `ABI-004-duplicate-exported-symbol-diagnostic`
- `ABI-005-library-without-export-diagnostic`
- C ABI unsafe type rejections: `CHK-194`, `CHK-222`, `CHK-223`, and `CHK-224`
- C ABI non-shape rule rejections: `CHK-193` for `ref` parameters and `CHK-195` for generic
  exports

Future freestanding ABI tests:

- `ABI-101-system-scalar-width-table`
- `ABI-102-system-struct-layout-padding-golden`
- `ABI-103-system-enum-tag-payload-golden`
- `ABI-104-system-exported-entry-symbol-golden`
- `ABI-105-system-reject-hosted-string-export`
- `ABI-106-system-panic-trap-no-unwind-symbols`
- `ABI-107-system-object-backend-symbol-map`
- `ABI-108-system-c-header-no-hosted-runtime-include`
