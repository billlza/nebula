# Tooling and CLI

This layer defines user-facing command behavior and stable CLI contracts.

## 1. Command model

Nebula CLI is defined by three orthogonal axes:
1. command responsibility: `check | build | run | test | bench | new | add | publish | fetch | update | fmt | explain | lsp`
2. build mode: `--mode debug|release`
3. analysis profile: `--profile auto|fast|deep`
4. runtime/target profile: hosted by default, or experimental system/no-std through
   `--profile system`, `--target system|freestanding|<triple>`, `--no-std`, and
   `--panic abort|trap`
5. project path model: commands accept a single-file source, project directory, or `nebula.toml`

## 2. Command responsibilities

- `nebula check <file.nb>`: static analysis + diagnostics only
- `nebula build <file.nb>`: compile/link artifact, never execute
  - default output kind: executable
  - `--emit staticlib|sharedlib` switches build output to a library artifact and emits a matching C header
  - `--emit freestanding-object` selects the isolated experimental primitive ELF object path and
    requires explicit exact `--target x86_64-unknown-none --panic trap` plus an absolute
    `--freestanding-toolchain-root` containing `bin/clang++`
  - library headers are emitted beside the selected library artifact, including raw single-file
    builds and custom `-o/--out-dir` output paths
- `nebula run <file.nb> [-- <program-args...>]`: preflight + build (if needed) + execute
- async `main` is supported; the generated harness drives it through the bundled cooperative runtime
- `run` forwards all tokens after `--` to the built artifact as child argv entries
- `nebula test`: run `@test` harness flow; `--dir` may name a raw source dir or a project/workspace target
- async `@test fn` is supported; the generated harness `block_on`s each async test case
- `nebula bench`: run `@bench`, emit latency/throughput summary plus stable
  `clock/platform/perf_capability/perf_reason` fields; `--dir` may name a raw source dir or a
  project/workspace target
- async `@bench fn` is supported; the generated harness `block_on`s each async benchmark
- `nebula new <path>`: scaffold a project root or repo-preview workspace starter depending on the
  selected template
- `nebula add <project>`: add an exact-version, path, or git dependency entry to `nebula.toml`
- `nebula publish <project>`: publish a package into a deterministic local file-based registry; with
  `--registry-url`, the bundled hosted-registry helper can also upload the staged package archive
- `nebula fetch <project>`: resolve dependencies and write reproducible `nebula.lock`; with
  `--registry-url`, the bundled hosted-registry helper first mirrors exact-version packages into a
  local registry root and then runs the normal local-registry resolver
- `nebula update <project>`: refresh dependency resolution and rewrite reproducible `nebula.lock`;
  with `--registry-url`, the bundled hosted-registry helper first mirrors exact-version packages
  into a local registry root and then runs the normal local-registry resolver
- `nebula fmt <file-or-dir>`: rewrite Nebula source formatting in place while preserving comments
- `nebula explain <path>`: emit stable explain JSON/text for `path+span` or `symbol+file` queries,
  including async explain data and transport-debug contract metadata for
  `RequestContext.transport_debug()` / `tls_peer_identity_debug()` / `http2_debug_state()`,
  including stable HTTP/2 phase-event `classification.reason` / `classification.detail`
  metadata
- `nebula lsp`: launch the language-server entrypoint with incremental document sync plus
  diagnostics publication, hover, definition, completion, references, document symbols, workspace
  symbols, signature help, rename, code actions, and semantic tokens

Current 1.0 GA scope:
- dependency resolution guaranteed for exact-version local-registry, `path`, `git`, and Linux
  backend-SDK `installed` packages
- executable targets may also consume dependency-scoped `[native]` inputs:
  `c_sources`, `cxx_sources`, `include_dirs`, and `defines`
- `[native]` additionally supports structured descriptors:
  - `[[native.sources]]` with `path`, `language`, optional `include_dirs`, `defines`, `arch`,
    and `cpu_features`
  - `[[native.generated_headers]]` with `out`, `template`, and `values`
- registry resolution defaults to `<project-or-workspace>/.nebula/registry` and may be overridden
  with `NEBULA_REGISTRY_ROOT`
- `publish` writes versioned package artifacts into that local registry; identical re-publish is
  allowed, but changed contents under the same version are rejected unless `--force` is used
- hosted registry integration is shipped through the bundled registry helper:
  - release archives carry it under `tooling/registry`
  - install prefixes place it under `share/nebula/registry`
  it still reuses the mirror-to-local-registry model rather than teaching the core resolver about
  HTTP sources directly
  - `NEBULA_REGISTRY_TOKEN` and `--registry-token` resolve to the same credential, with an explicit
    command-line value taking precedence
  - the resolved credential is delivered to the helper only through its child-local
    `NEBULA_REGISTRY_TOKEN` environment entry; helper argv and Nebula process diagnostics must not
    contain it, and the remaining parent environment is inherited without mutating global process
    state
- import syntax is uniform across these dependency sources:
  - same-package imports remain `import foo.bar`
  - package imports remain `import dep::foo.bar`
  - switching a dependency between `path`, `git`, `installed`, local registry, and hosted registry
    does not change source imports
- `[workspace]` manifests own a shared root `nebula.lock` and member packages resolve through that
  lock for `fetch/update/check/build/run/test/bench`
- workspace-only roots resolve the first member package in sorted member-path order when a
  deterministic default target is required
- official release assets are:
  `nebula-v<version>-darwin-x86_64.tar.gz`,
  `nebula-v<version>-darwin-arm64.tar.gz`,
  `nebula-v<version>-linux-x86_64.tar.gz`,
  `nebula-v<version>-windows-x86_64.zip`
- integrity sidecars are also part of the release contract:
  - `nebula-v<version>-<target>.spdx.json`
  - `nebula-v<version>-<target>.provenance.intoto.jsonl`
  - `nebula-v<version>-<target>.sbom.intoto.jsonl`
  - `SHA256SUMS.txt.intoto.jsonl`
- install scripts also support opt-in attestation verification:
  - `install.sh --verify-attestations`
  - `install.ps1 -VerifyAttestations`
  - `NEBULA_INSTALL_VERIFY_ATTESTATIONS=1`
- install surfaces are GitHub Release assets, `scripts/install.sh`, `scripts/install.ps1`,
  and the rendered `nebula.rb` formula
- installed binary surfaces cover compiler/tooling, bundled `std`, runtime headers, and release
  documentation only; repo-local preview packages under `official/*` are not installed by release
  assets
- `build`, `test`, and `bench`, plus `run` unless `--no-build` is selected, require a working host
  C++23 compiler; Nebula uses `CXX` when set and otherwise defaults to `clang++`. `run --no-build`
  resolves an existing artifact without resolving a compiler, but still acquires a private verified
  execution copy instead of executing the public pathname directly
- experimental hosted `build` and build-enabled `run` publish generated source, artifact, metadata,
  optional C ABI header, and optional import library through one process-level transaction. Public
  destinations are locked in canonical owner-controlled parents; staged files are private, exact
  protected-input digests are revalidated before publication, compiler-generated depfiles must
  match the pre-compilation native-header closure, and a failed process-level commit is rolled back
  without reporting success. On POSIX, the sealed output crosses one explicit signal checkpoint:
  an already-observed termination signal aborts before commit, while later delivery remains blocked
  until transaction cleanup completes. This is not a power-loss/SIGKILL atomic-set guarantee;
  linker-selected inputs, durable multi-file journaling, and startup recovery remain future work
- hosted `run --reuse` requires matching versioned build identity, metadata, artifact digest,
  toolchain identity, and protected inputs. Both fresh and reusable execution copy the exact assessed
  bytes through an open handle into an owner-private same-directory lease. The transaction locks are
  released before the user process starts. POSIX pathname launch and final unlink retain an explicit
  same-effective-UID trust boundary; the lease is not an immutable-file or hostile-same-UID sandbox
  and catchable termination during the user process can still end the parent before lease cleanup
- the experimental `build --emit freestanding-object` exception resolves exactly
  `<--freestanding-toolchain-root>/bin/clang++` without consulting `PATH`, freezes the compiler's
  canonical public path/content identity into provenance, and executes version, target, capability,
  and compilation requests through one matching owner-private verified lease. Logical `argv[0]`
  remains the canonical public compiler path, so later public replacement cannot select different
  bytes. The `x86_64-none-clang-cxx20-v4` command schema records the lease execution boundary,
  null-device stdin, minimal non-inherited environment, and separate 64 KiB stdout/stderr capture
  limits. Overflow is an explicit contained infrastructure failure and captured control bytes are
  never emitted raw. The private compiler lease must be identity-checked and removed before artifact
  publication; cleanup failure prevents publication. Compilation retains the 30-second
  process-group timeout, retains the POSIX leader as an unreaped identity anchor until the group is
  sealed (including ordinary compiler exit), verifies a bounded Darwin/Linux zombie-only group
  snapshot before claiming containment, and handles catchable CLI termination by cleaning the
  owned compiler group before restoring and re-delivering `SIGHUP`, `SIGINT`, `SIGQUIT`, or
  `SIGTERM`; a containment failure suppresses re-delivery and becomes an explicit infrastructure
  error with process exit status `125`. A confirmed 30-second compiler timeout exits `124`; a
  compiler that independently returns `124` remains an ordinary build failure and is not
  mislabeled as a timeout. One move-only toolchain session owns the signal scope continuously from
  before lease creation through resolver queries, analysis/emission, compilation, lease retirement,
  and final restoration; every owner early-return closes that session explicitly. The transaction
  freeze synchronously collects pending signals owned by the build and is
  the commit/cancellation handoff; later signals stay blocked until cleanup and then belong to the
  restored caller disposition. A signal already blocked by the caller is preserved instead of
  being consumed by the build. It ignores `CXX` and hosted standard/include/SDK
  overrides and never falls back to the
  hosted compiler path. `SIGKILL`, host failure, and parent-process crashes require an external
  supervisor for an orphan-free guarantee
- official release behavior treats `clang++` as the supported default host compiler contract; the
  CLI does not silently auto-fallback to `g++`
- the Homebrew formula depends on `llvm` and wraps `nebula` so `CXX` defaults to the brewed
  `clang++` only when the user has not already set `CXX`
- `add --git`, `fetch`, and `update` require `git` on `PATH`; remote git sources additionally
  require whatever network/auth access the remote demands
- hosted-registry `publish/fetch/update --registry-url ...` requires Python 3.11+ on `PATH`, or
  `PYTHON=/path/to/python3.11+`, because the installed helper client is Python-based
- `[native]` inputs are folded into manifest fingerprints, `nebula.lock` replay, and build cache
  identity; executable builds compile `.c` in C mode, `.cc/.cpp/.cxx` in C++ mode, and
  descriptor-marked `asm` sources in assembler-with-preprocessor mode
- descriptor-marked `arch` lists gate native source selection to the current host architecture
- descriptor-marked `cpu_features` map to a narrow compiler-supported feature policy rather than
  arbitrary raw flag strings
- generated native headers are rendered into Nebula's own generated include tree and may be
  consumed by reachable native sources without mutating the source package checkout
- library outputs (`build --emit staticlib|sharedlib`) reject reachable `[native]` inputs rather
  than exposing a fake stable ABI through package-native sources
- experimental system/no-std profile:
  - `--profile system` is accepted as a runtime-profile alias and is intentionally separate from
    analysis-depth `--profile fast|deep`
  - `--target system`, `--target freestanding`, and `*-none*` target strings select the system
    profile and imply `--no-std`
  - system/no-std rejects `import std::*` during project loading with `NBL-CLI-SYSTEM-STD`
  - system profile forces strict-region behavior even when `--strict-region` is not passed
  - system/no-std rejects reachable `host_cxx` and `[native]` package sources with
    `NBL-CLI-SYSTEM-NATIVE`
  - system/no-std rejects `--panic unwind`; abort/trap are the only accepted policies until a
    freestanding unwind/runtime ABI exists
  - generated C++ records `NEBULA_RUNTIME_PROFILE`, `NEBULA_TARGET`, and `NEBULA_PANIC_POLICY`
    so panic and target policy are visible in artifacts
  - `examples/system_no_std_smoke` is a check/hosted-C++ build fixture for this gate, not a
    freestanding execution proof
  - ordinary system/no-std builds are not freestanding objects; hosted C++23 codegen and runtime
    headers remain their implementation path
  - `build --emit freestanding-object --target x86_64-unknown-none --panic trap
    --freestanding-toolchain-root <absolute-clang-root>` is a separate exact
    experimental path for reachable `Int/Bool/Void`, direct resolved internal calls, and
    stack/non-owning storage only
  - the experimental publication implementation currently supports macOS/Linux hosts; Windows
    compiler/tooling builds remain supported but reject the request with
    `NBL-CLI-FS-HOST-UNSUPPORTED`
  - that path produces an audited ELF64 relocatable object with exactly one global
    `__nebula_uos_payload_entry_v1`, rejects payload ownership of the future image `_start`, has no
    undefined symbols, and binds metadata to object size, SHA-256, and the immutable toolchain
    snapshot; the build key also embeds the configured canonical protocol/ABI manifest size and
    SHA-256, so a contract-byte change invalidates provenance; unsupported semantics,
    toolchain output, or artifact state fail explicitly with no hosted fallback
  - the first publication refuses to replace existing source/metadata/object paths, serializes
    concurrent same-output builds, and commits the object last; callers must choose a clean output
    path for a rebuild
  - publication rollback is a process-failure guarantee for a caller-controlled output directory;
    it is not a hostile shared-directory sandbox or a power-loss-durable filesystem transaction
  - the object path remains clang-backed and does not link, execute, provide a runtime, or claim a
    direct object backend
- current complex-application positioning is intentionally narrow:
  - credible today for multi-file CLI/tools, especially file-oriented utilities, async service
    cores, backend-first internal app slices, and host-bridged modules
  - not yet a complete pure-Nebula HTTP service platform or GUI/web application platform
- bundled std surface is compiler-shipped and resolved from the detected std install root:
  - `import std::task`
  - `import std::time`
  - `import std::result`
  - `import std::log`
  - `import std::bytes`
  - `import std::fs`
    `std::fs` currently covers synchronous string-path predicates plus text/bytes read/write and
    recursive directory creation for file-oriented CLI/tools; it is intentionally blocking and does
    not model open file handles, directory walking, globbing, or process spawning
  - `import std::net`
    `std::net` currently covers literal IPv4 construction via `ipv4(...)`, first-IPv4 hostname
    resolution via `resolve_ipv4(host, port)`, and direct host dialing via
    `connect_host(host, port)` in addition to the existing `SocketAddr` / `TcpListener` /
    `TcpStream` TCP surface
  - `import std::http`
    `std::http` currently covers inbound request parsing, response writing, exact and `:param`
    path matching, sequential long-lived `serve(...)`, request-header lookup via
    `header_value(...)` / `content_type(...)`, outbound `ClientRequest` / `ClientResponse`,
    `Get` / `Post` / `Head`, `get_request(...)` / `post_request(...)` / `head_request(...)`,
    `request1/2/3` fixed-arity header helpers, and minimal outbound HTTP/1.1 client I/O via
    `TcpStream_write_request(...)`, `TcpStream_read_response(...)`, and
    `TcpStream_read_response_for(...)` with `Content-Length` request bodies plus
    `Content-Length` / `Transfer-Encoding: chunked` / EOF-delimited response framing, skipped
    interim `1xx`, and no-body handling for `HEAD` and `204`/`205`/`304`
  - `import std::http_json`
    `std::http_json` is the narrow bridge layer for JSON body parsing and JSON response helpers
    such as `parse_json_body` and `json_response`/`ok_json`
  - `import std::json`
    `std::json` stays narrow: validated JSON text values plus scalar/object helpers and top-level
    object field extraction for service code
- current mixed-language contract is C ABI first:
  - `extern fn` remains the host-import surface
  - `@export @abi_c` plus `build --emit staticlib|sharedlib` defines the export surface
  - library builds only export the current root package's explicit C ABI functions;
    dependency exports are not re-exported transitively
  - only top-level non-generic `Int/Float/Bool/Void` functions are ABI-safe in the current slice
- current repo-local preview crypto package slice is package-managed rather than bundled std:
  - repo-local source tree: `official/nebula-crypto`
  - intended dependency alias: `crypto`
  - currently shipped modules: `crypto::rand`, `crypto::hash`, `crypto::pqc.kem`,
    `crypto::pqc.sign`
  - currently shipped surface:
    `random_bytes(len: Int) -> Result<Bytes, String>`,
    `blake3(data: Bytes) -> Bytes`,
    `sha3_256(data: Bytes) -> Bytes`,
    `sha3_512(data: Bytes) -> Bytes`,
    `hex(data: Bytes) -> String`,
    ML-KEM-768 keypair / encapsulate / decapsulate,
    and ML-DSA-65 keypair / sign / verify
  - current host support: macOS + Linux only
  - current explicit non-goals: TLS/HTTPS, PKI/certificates, Windows host support
  - current release posture: preview/pilot only, outside the installed GA surface

## 3. Parsing and global flags

- unknown/invalid option or extra positional args -> parse error, exit code `2`
- `run` is the only subcommand that accepts `-- <program-args...>` after Nebula options
- `check|build|run` require exactly one source argument
- `--help|-h` returns `0`
- `--version` returns `0`

## 4. Run semantics

Two-stage run:
- Stage 1 preflight: `--preflight fast|off` (default `fast`)
- Stage 2 build/link: resolved mode/profile

Run gating (`--run-gate high|all|none`) applies to preflight diagnostics only.

Program argument pass-through:
- tokens after `--` are forwarded to the executed artifact without reinterpretation
- `argv(0)` remains the artifact path of the executed child process
- `argv(1..)` correspond to the forwarded program arguments

Artifact controls:
- `-o|--out <path>`
- `--reuse`
- `--no-build`

## 5. Diagnostics controls

Supported overlays:
- `--analysis-tier basic|smart|deep`
- `--smart on|off`
- `--diag-view raw|grouped`
- `--warn-policy strict|balanced|lenient`
- `--warn-class <class>=on|off`
- grouped/root-cause controls:
  `--diag-budget-ms`, `--diag-grouping-delay-ms`, `--max-root-causes`,
  `--root-cause-v2`, `--root-cause-top-k`, `--root-cause-min-covered`

## 6. Exit-code contract

- parse failures: `2`
- `check|build|test|bench`: non-zero when errors exist
- `run`: if execution starts, return child artifact exit code
- when the Nebula entry `main` returns `Int`, the generated executable maps that value to the host
  process exit code; `Void` `main` returns `0`

## 7. Layer boundaries

- This file is the normative Tooling/CLI layer.
- Diagnostic schema and code taxonomy: `spec/diagnostics.md`.
- Cache/reuse/grouping internals and perf baseline tooling: `spec/experimental_infra.md`.
