# Compiler Pipeline

This document names the current compiler pipeline boundary and the first backend interface seam. It
does not introduce a new backend or a backend selection surface.

Current supported production pipeline:

```text
Source -> AST -> Typed AST -> NIR/CFG -> analysis -> backend -> host toolchain
```

For the current release, `backend` means the C++23 backend and `host toolchain` means compiling the
generated C++ with the configured C++ compiler, currently `clang++` by release contract.

## Stage Contract

| Stage | Current responsibility | Current owner files |
| --- | --- | --- |
| Source | Nebula source files, package manifests, workspace inputs, profile flags, run arguments | `cli/`, `frontend/lexer.*`, `frontend/parser.*`, `cli/project.*` |
| AST | Parsed module/function/type/statement/expression surface before semantic resolution | `frontend/ast.hpp`, `frontend/parser.*` |
| Typed AST | Name resolution, type checking, imports, annotations, generic instantiation, diagnostics | `frontend/typecheck.*` |
| NIR/CFG | Lowered internal representation and control-flow graph used by compiler analyses | `nir/lower.*`, `nir/ir.hpp`, `nir/cfg.*`, `nir/cfg_ir.*` |
| Analysis | Escape analysis, borrow/exclusivity assists, Rep x Owner inference, async/call-target support | `passes/escape_analysis.*`, `passes/borrow_xstmt.*`, `passes/rep_owner_infer.*`, `passes/call_target_resolver.*` |
| Backend | Backend-specific emission from analyzed NIR plus codegen options | `codegen/backend.hpp`, `codegen/cpp_backend.*` |
| Host toolchain | Compile generated C++ and optional host/native sources into executable or library artifacts | `cli/build_run.*`, `cli/main.cpp` |

## Backend Boundary

`codegen/backend.hpp` is intentionally small. It owns:

- `EmitOptions`: main/test/bench mode, strict-region flag, runtime profile, panic policy, target
  marker, and C ABI wrapper options
- `CAbiFunction`: the narrow C ABI export description currently used by library builds
- `Backend`: a virtual interface for emitting a translation unit, collecting C ABI exports, and
  emitting a C header
- `cpp23_backend()` and `default_backend()`

`default_backend()` currently returns `cpp23_backend()`. C++23 remains the default and only
production backend. There is no LLVM backend, Cranelift backend, direct object backend, backend
registry, or CLI backend selector in this MVP.

The interface receives already-lowered and analyzed NIR plus `RepOwnerResult`. Parser, typechecker,
NIR, CFG, and analysis behavior are not backend-pluggable in this MVP.

## Required Changes In This MVP

Parser:

- no parser changes

Typechecker:

- no typechecker changes

NIR/CFG:

- no NIR or CFG semantic changes

Analysis:

- no analysis semantic changes
- backend emission continues to consume `RepOwnerResult`

Codegen:

- add `codegen/backend.hpp`
- move shared backend option/export structs to the backend boundary
- wrap the existing C++23 emit entrypoints behind a `Backend` implementation
- keep direct C++23 entrypoints available for current codegen-local tests and compatibility

CLI/build:

- use `default_backend()` at build, run cache-miss, test, and bench emission callsites
- keep artifact names, generated C++ markers, C ABI header behavior, and host compilation behavior
  unchanged

## Current Non-Claims

This boundary does not claim:

- LLVM or Cranelift support
- direct object-file generation
- backend-independent ABI stability
- cross compilation
- freestanding or no-std runtime support
- kernel, driver, interrupt, MMU, scheduler, or syscall ABI support
- any fallback backend behavior

Unsupported future backend requests should fail explicitly when such a selection surface exists. The
current CLI has no backend selector, so there is no hidden fallback path.

## Lowering Strategy

The current lowering strategy remains:

1. Parse source into AST.
2. Resolve names and type check into typed AST/module state.
3. Lower typed program state into NIR.
4. Build CFG and run analysis passes.
5. Pass analyzed NIR plus `RepOwnerResult` and `EmitOptions` to `default_backend()`.
6. The C++23 backend emits one translation unit and optional C ABI wrapper/header artifacts.
7. The CLI invokes the host C++ compiler for executable or library output.

Future backends may attach at step 5 only after their unsupported-target diagnostics, ABI contract,
runtime contract, object/linker strategy, and test gates are specified.

## Diagnostics Strategy

This MVP should not add backend-selection diagnostics because there is no backend-selection input.
Existing diagnostics remain the contract:

- parser/typechecker diagnostics stay owned by frontend stages
- system/no-std profile diagnostics stay owned by CLI/project loading
- C ABI export diagnostics stay owned by library build validation
- host compiler failures stay surfaced as host compilation failures

A future backend selector must introduce stable diagnostics for unsupported backend/profile/target
combinations before any non-C++23 backend is exposed.

## Test Plan

Current contract tests should prove behavior is unchanged:

- build a simple hosted program and inspect generated C++ profile markers
- run a simple hosted program through the default backend
- keep C ABI hosted layout/export tests green
- keep system/no-std profile marker tests green
- keep workspace `check/build/run/test/bench` smoke tests green
- keep full contract suite green

Future golden tests to add when expanding this boundary:

- `BLD-backend-default-cpp23-profile-markers`
- `RUN-backend-default-cpp23-main-exit`
- `TST-backend-interface-docs-contract`
- `ABI-backend-default-c-abi-header-stable`
- `ABI-backend-default-exported-scalar-stable`
- `TST-backend-selector-unsupported-llvm-rejected`
- `TST-backend-selector-unsupported-cranelift-rejected`
- `TST-backend-selector-unsupported-object-rejected`
- `TST-system-profile-backend-cpp23-non-freestanding-marker`
- `TST-freestanding-backend-requires-object-gate`
