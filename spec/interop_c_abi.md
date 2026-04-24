# C ABI Interop

Nebula's first interop priority is a narrow, explicit C ABI surface for mixed-language systems.

## Current contract

- Importing host code continues to use `extern fn`.
- Exporting Nebula code uses function-level markers:
  - `@export`
  - `@abi_c`
- Library builds are driven through:
  - `nebula build <target> --emit staticlib`
  - `nebula build <target> --emit sharedlib`

## First ABI-safe subset

Only top-level non-generic functions are exportable in the current slice.

Allowed exported parameter/return types:
- `Int`
- `Float`
- `Bool`
- `Void`

Rejected from the public C ABI:
- `String`
- `struct`
- `enum`
- `Result`
- callable types
- `ref` parameters
- generic exported functions
- `extern fn` declarations marked as exports

## Generated surface

- The C ABI does not expose Nebula's internal C++ symbol names.
- Codegen emits stable `extern "C"` wrapper functions with readable qualified names:
  - `nebula_<package>_<module>_<name>`
  - when adjacent qualified segments normalize to the same text, duplicate segments are collapsed
- `build --emit staticlib|sharedlib` also emits a C header next to the library artifact.
- Raw single-file library builds are supported; in that case the generated header stem comes from
  the source file stem.
- When `-o/--out` or `--out-dir` is used, the generated header is emitted next to the selected
  library artifact.
- Library builds export only the current root package's explicitly annotated functions.
  Dependency-package `@export @abi_c` functions are not re-exported transitively.

## Mixed-language positioning

- **C++**: direct consumer of the generated header and library.
- **Rust**: directional path through C ABI bindings, not Rust ABI.
- **Swift**: directional path through the generated C header/module import path, not Swift ABI.
- **JavaScript**: directional path through Node-API/FFI wrappers or a process boundary.

Until richer ABI-safe types exist, complex objects should stay behind host-owned handles or process
boundaries rather than crossing the ABI directly.
