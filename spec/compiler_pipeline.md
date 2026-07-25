# Compiler Pipeline

This document names the current compiler pipeline boundary, the first backend interface seam, and
the isolated experimental freestanding object bootstrap. It does not introduce a backend selection
surface or promote a second production backend.

Current supported production pipeline:

```text
Source -> AST -> Typed AST -> NIR/CFG -> analysis -> backend -> host toolchain
```

For the current release, `backend` means the C++23 backend and `host toolchain` means compiling the
generated C++ with the configured C++ compiler, currently `clang++` by release contract.

The explicit experimental object request has a separate pipeline:

```text
Source -> AST -> Typed AST -> NIR/CFG -> analysis
       -> primitive freestanding C++ emitter
       -> fixed clang++ x86_64-none compile
       -> bounded ELF audit -> transactional .o publication
```

This path is a C++ bootstrap for one exact target and a deliberately small reachable subset. It is
not `default_backend()`, does not create a CLI backend selector, and does not provide direct machine
code generation, linking, a runtime, or boot execution.

## Stage Contract

| Stage | Current responsibility | Current owner files |
| --- | --- | --- |
| Source | Nebula source files, package manifests, workspace inputs, profile flags, run arguments | `cli/`, `frontend/lexer.*`, `frontend/parser.*`, `cli/project.*` |
| AST | Parsed module/function/type/statement/expression surface before semantic resolution | `frontend/ast.hpp`, `frontend/parser.*` |
| Typed AST | Name resolution, type checking, imports, annotations, generic instantiation, diagnostics | `frontend/typecheck.*` |
| NIR/CFG | Lowered internal representation and control-flow graph used by compiler analyses | `nir/lower.*`, `nir/ir.hpp`, `nir/cfg.*`, `nir/cfg_ir.*` |
| Analysis | Escape analysis, borrow/exclusivity assists, Rep x Owner inference, async/call-target support | `passes/escape_analysis.*`, `passes/borrow_xstmt.*`, `passes/rep_owner_infer.*`, `passes/call_target_resolver.*` |
| Backend/codegen | Hosted backend emission plus the isolated primitive freestanding emitter from analyzed NIR | `codegen/backend.hpp`, `codegen/cpp_backend.*`, `codegen/freestanding_cpp_emitter.*` |
| Toolchain/artifact | Compile hosted outputs or invoke the fixed experimental target compiler, then validate and publish artifacts | `cli/build_run.*`, `cli/main.cpp`, `cli/freestanding_object.*`, `cli/elf_object.*` |

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

## Experimental Freestanding Object Slice

`codegen/freestanding_cpp_emitter.*` is intentionally adjacent to, but not registered as, the
hosted `Backend` implementation. `cli/build_run.cpp` enters it only for the exact artifact request
`freestanding-object`; all ordinary build/run/test/bench emission continues through
`default_backend()`.

The emitter consumes the existing analyzed NIR and `RepOwnerResult`; it does not fork the frontend,
resolver, typechecker, NIR, or ownership analyses. It validates a single reachability graph before
emitting only `Int`, `Bool`, `Void`, direct resolved internal calls, and stack/non-owning storage.
Unsupported reachable nodes produce `NBL-BE-FS-*` diagnostics with no hosted fallback.

The CLI artifact boundary then:

- resolves exactly `<explicit-root>/bin/clang++` once, hashes its canonical public identity, and
  copies those expected bytes into one owner-private verified executable lease; version, target,
  ABI-capability, and formal compilation requests all execute that same lease while retaining the
  canonical public path only as logical `argv[0]` and provenance. Public-path replacement therefore
  cannot redirect the resolved build. Command schema `x86_64-none-clang-cxx20-v4` binds this lease,
  null-stdin, and bounded-stream behavior into the build key. Formal compilation inherits no
  environment, receives only `LC_ALL`, `LANG`, `TZ`, and private `TMPDIR`, reads stdin from the null
  device, and captures stdout/stderr under separate 64 KiB limits; either overflow terminates and
  audits the contained process group and fails as infrastructure status `125` without rendering raw
  bytes. The lease is revalidated around compilation and explicitly removed before any ELF bytes or
  metadata can be published; cleanup failure is also infrastructure status `125`. The resolver does
  not consult `PATH`, `CXX`, or hosted standard/include/SDK override variables; it enforces a
  30-second process-group
  timeout; POSIX launch requires retained/default `SIGCHLD` ownership, observes the group leader
  without reaping it, sends final group termination on every exit path, performs a bounded Darwin
  `sysctl` or Linux `/proc` audit proving that only zombie/dead members remain, and only then reaps
  the identity anchor;
  while the CLI remains able to run cleanup, `SIGHUP`, `SIGINT`, `SIGQUIT`, and `SIGTERM` are
  recorded by an async-signal-safe scoped handler, followed by group cleanup, restoration of the
  caller disposition/mask, and re-delivery of the original signal; if group containment cannot be
  confirmed, re-delivery is explicitly suppressed and reported as an infrastructure failure with
  exit status `125`. The resolver transfers that armed scope with the move-only toolchain session,
  so it remains active continuously from before private-lease creation through build-key
  derivation, analysis, emission, compilation, pre-publication lease retirement, and final
  caller-state restoration; resolver and pre-compilation failures close the same session
  explicitly. A confirmed compiler timeout is a distinct `124` outcome.
  The freeze boundary atomically blocks termination delivery and consumes pending signals owned by
  the transaction (signals the caller had not blocked or ignored); that snapshot is the single
  commit/cancellation decision point. Later signals remain blocked until cleanup finishes and then
  belong to the restored caller disposition. Caller-blocked pending signals remain caller-owned
- supplies fixed freestanding/no-include/no-exception/no-RTTI/unwind-disabled flags plus the
  restricted x86-64 ABI flags (`-m64 -mabi=sysv -mno-red-zone -mno-80387 -mno-mmx -mno-sse
  -mno-sse2`) for `x86_64-unknown-none`
- validates the output with the repo-local bounded ELF parser, including finite file/string-scan/
  allocated-section budgets, W^X rejection, relocation width/target/symbol checks, and an exact
  `__nebula_uos_payload_entry_v1` section contract; payload `_start` ownership is rejected
- binds metadata to object size and SHA-256 content
- serializes same-output builds and publishes source, metadata, and object with object-last commit;
  existing outputs are never silently replaced

The transaction guarantee covers normal process-level failures and identity-checked rollback. It is
not a power-loss/hostile-shared-directory guarantee: the toolchain must be trusted, the output
directory must be controlled by the caller, and parent-directory durability is not yet fsynced.
Uncatchable termination such as `SIGKILL`, host failure, or a compiler-process crash in the parent
CLI remains outside the portable in-process cleanup guarantee and requires a future platform
supervisor for a stronger orphan-free claim.

## Hosted Artifact Boundary

Ordinary hosted `build` and build-enabled `run` keep C++23 as their backend, but they do not treat
the compiler invocation or public output path as an implicit trust boundary. Their current
experimental lifecycle is:

1. resolve canonical compiler/child-tool identities and a bounded compilation environment;
2. snapshot declared Nebula/package/runtime inputs and compiler-discovered native dependencies;
3. bind those paths, sizes, SHA-256 digests, directory membership, tool identities, and environment
   identity into the versioned build key;
4. compile only into an owner-private same-filesystem object/staging workspace, generating real
   depfiles and rejecting any difference from the pre-compilation dependency snapshot;
5. revalidate protected inputs and tool identities, then seal the exact staged output set;
6. on POSIX, freeze the owned termination signals after sealing; a signal observed before that
   snapshot aborts publication, while a signal after it remains blocked until commit/cleanup has
   completed and then belongs to the restored caller disposition;
7. publish under canonical output locks and release transaction state before reporting success;
8. for `run`, copy the exact sealed or reuse-assessed executable bytes into an owner-private
   same-directory execution lease rather than launching the public pathname.

On Darwin, the transaction treats `com.apple.provenance` as opaque operating-system metadata, not
as authentication evidence. The transaction-owned metadata sidecar is sealed as an owner-owned
`0600`, single-link regular file without an extended ACL or unrelated extended attributes. Before
publication, after link/unlink publication, and again after parent-directory durability, every
output is rehashed and its object identity, mode, owner, group, flags, ACL, and complete bounded
extended-attribute snapshot are revalidated. The metadata sidecar alone may consume one bounded
provenance stabilization: either an absent-to-present addition or a same-value rewrite that changes
only `ctime`. Every other field and byte must remain identical, and the refreshed snapshot closes
the exception for the remainder of the transaction. Other files, other attributes, repeated
changes, unsupported metadata inspection, and ambiguous observations fail closed.

This boundary is shared by hosted `build` and build-enabled `run` only. `test` and `bench` still use
their earlier public-output lifecycle, and `run --no-build` does not establish the compiler
termination boundary. The dependency set covers compiler-generated translation-unit includes but
does not yet bind every linker-selected object, archive, SDK stub, or linker script. Publication is
rollback-capable for ordinary process failures, not a journaled atomic multi-file filesystem
transaction: `SIGKILL`, power loss, and host crash can leave mixed public versions or private
recovery state. Execution-time termination can still interrupt the parent after its signal boundary
is restored and before lease cleanup. These are explicit blockers to a stable hosted artifact
contract, not cases where the CLI may silently fall back to an unverified path.

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
- a direct machine-code/object backend independent of generated C++ and clang
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

For the experimental freestanding object request, steps 5 through 7 are replaced by the isolated
primitive emitter, fixed clang invocation, ELF audit, and artifact transaction described above.
That exception does not change the hosted production lowering strategy.

## Diagnostics Strategy

This MVP should not add backend-selection diagnostics because there is no backend-selection input.
Existing diagnostics remain the contract:

- parser/typechecker diagnostics stay owned by frontend stages
- system/no-std profile diagnostics stay owned by CLI/project loading
- C ABI export diagnostics stay owned by library build validation
- host compiler failures stay surfaced as host compilation failures

A future backend selector must introduce stable diagnostics for unsupported backend/profile/target
combinations before any non-C++23 backend is exposed. The existing object slice instead uses stable
`NBL-BE-FS-*` semantic diagnostics and `NBL-CLI-FS-*` toolchain/artifact diagnostics because it is
an explicit artifact kind, not a backend selector.

## Test Plan

Current contract tests should prove behavior is unchanged:

- build a simple hosted program and inspect generated C++ profile markers
- run a simple hosted program through the default backend
- keep C ABI hosted layout/export tests green
- keep system/no-std profile marker tests green
- keep workspace `check/build/run/test/bench` smoke tests green
- keep full contract suite green
- keep `BLD-017` through `BLD-020` green for the experimental object, exact request matrix,
  allowlist, deterministic/no-replace transaction, and toolchain/ELF failure paths
- keep `BLD-021` through `BLD-025` green for hosted conflict preservation, alias rejection,
  concurrent publication, compiler interruption cleanup, and pre-commit signal linearization
- keep `RUN-088` through `RUN-090` green for content-bound reuse, transitive native-header identity,
  and logical `argv[0]` through a private execution lease

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
