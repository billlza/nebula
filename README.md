# Nebula

Nebula is a minimal language + compiler pipeline focused on:
- **Region allocation** as an explicit allocation domain (`region R { ... }`)
- **Ownership inference** (bootstrap) with explicit user overrides (`shared`, `unique`, `heap`, `promote`)
- **Unsafe boundary contract** (`@unsafe fn` + `unsafe { ... }`)
- **Boolean/control-flow core**: `Bool`, comparisons/logical operators, `if` / `else`, `for` / `while`, `break` / `continue`, statement-form and expression-form `match`
- **Monomorphized generics**: generic `struct`, `enum`, and `fn`, with explicit type arguments on
  types and inference-driven generic function calls
- **Structured error flow**: generic `Result<T, E>` conventions plus postfix `?` for propagation
- **Async + service foundation**: `async fn`, `await`, `spawn`, async `main/@test/@bench`, single-thread cooperative runtime with reactor-backed TCP I/O, bundled `std::task/std::time/std::env/std::result/std::log/std::bytes/std::fs/std::process/std::net` (including literal IPv4 plus narrow hostname dialing via `resolve_ipv4(...)` / `connect_host(...)`), a synchronous string-path `std::fs` slice for file-oriented CLI/tools, a preview argv-based `std::process` slice for macOS/Linux internal tooling, a minimal HTTP/1.1 layer in `std::http` that now covers both server-side request/response handling and outbound client request/response I/O, `Get`/`Post`/`Head`, `:param` route matching, sequential long-lived `serve(...)`, narrow header lookup, `head_request(...)`, method-aware response reads for `HEAD`, status-aware no-body handling (`1xx`/`204`/`205`/`304`), and chunked-response decode, plus a service-oriented `std::json` bridge module `std::http_json` for helpers such as `parse_json_body` and `ok_json`
- **Multi-file project model**: `module`, `import`, and `nebula.toml` entry manifests
- **Package-native bridge**: package-scoped `[native]` sources with reproducible lock/cache identity for executable targets
- **Thin host support**: built-in `print`, `assert`, `argc`, `argv`, plus `extern fn` declarations
- **Narrow C ABI export**: `@export @abi_c` plus `nebula build --emit staticlib|sharedlib` for
  stable scalar mixed-language entrypoints
- **Struct ergonomics (minimal)**: field read/write (`x.f`, `x.f = v`), variable reassignment (`x = v`),
  rooted field chains (`x.f.g`, `x.f.g = v`), `let` struct destructuring (`let { x, y: y0, z: _ } = pair`),
  enum payload struct destructuring in `match` (`Some({ x, y: y0, z: _ })`), and postfix over temporary bases
  (`foo().x`, `foo().m()`, `foo().x.y`)
- **Conservative safety assist layer**: borrow/exclusivity diagnostics (`NBL-T09x`) and related
  checks are intentionally secondary to the language core narrative (see `spec/static_analysis.md`)
- **Epistemic linting** (performance/latency-oriented diagnostics)
- **Three-axis CLI model**:
  - command responsibility (`check` / `build` / `run`)
  - build mode (`--mode debug|release`)
  - analysis depth (`--profile auto|fast|deep`)
- **Experimental system-profile gate**: `--target system|freestanding|<triple>`, `--profile system`,
  `--no-std`, and `--panic abort|trap` make early universeOS-facing constraints explicit by
  forbidding bundled `std` imports and forcing strict region diagnostics before any no-std runtime
  claim is made.

This repo implements a practical compiler in **C++**:

Source → AST → Typed AST → NIR/CFG → EscapeAnalysis → Rep×Owner inference → C++23 → clang++

## Current Boundary

Nebula is ready for:

- small-to-medium multi-file CLI and system tools, especially file-oriented utilities
- Linux backend services through the opt-in backend SDK, behind a reverse proxy
- preview internal service-to-service TLS/mTLS transport through `official/nebula-tls`,
  `official/nebula-tls-server`, and the thin `service::tls` adapter, including narrow preview
  ALPN / HTTP/2 transport paths for internal hops
- backend-first internal control-plane products that combine a shared core, Linux backend service,
  operator CLI, and an explicit preview embedded-data package
- performance-sensitive cores where explainable promotion/ownership matters
- mixed-language components behind a thin host boundary
- preview Nebula UI semantic trees through `ui` / `view` syntax and `official/nebula-ui`
- executable-side packages that vendor native C/C++ sources through `[native]`

Nebula is not yet positioned as:

- a broad pure-Nebula backend platform with native edge TLS termination, middleware ecosystems, and wide host parity
- a pure-Nebula desktop/mobile/web app platform
- a mature SwiftUI/Qt/Flutter-class UI renderer, layout engine, or native adapter stack
- a direct native embedding surface for Swift/Rust/JavaScript without a C ABI or process boundary
- a complete PQC / TLS stack yet; the current official crypto slice is intentionally narrower
- a freestanding/no-std OS implementation language; the new system-profile CLI gate is a diagnostic
  and codegen-contract milestone, not a kernel/runtime/backend independence claim

## Backend Service Profile

Nebula 1.0 GA applies to the compiler, CLI, package workflow, bundled `std`, runtime headers, and
documented release assets. The Linux backend SDK is shipped as a separate opt-in asset; remaining
packages under `official/` continue as repo-local preview/pilot surfaces.

Nebula's backend service profile is intentionally narrow:

- production target: Linux x86_64
- development hosts: macOS + Linux
- inbound public TLS terminated by a reverse proxy or service mesh
- Nebula handles business HTTP logic, outbound TLS, preview internal TLS/mTLS service hops, and
  PQC application flows
- backend service helpers expose env-driven bind/limits/timeouts, inbound request-id policy, request context, JSON framework errors, and drain/shutdown-file based graceful quiesce through the installed Linux backend SDK

See:

- `docs/service_profile.md`
- `docs/backend_developer_guide.md`
- `docs/support_matrix.md`
- `docs/app_platform_convergence.md`
- `docs/universeos_convergence.md`
- `docs/stability_policy.md`
- `docs/official_package_tiering.md`
- `docs/reverse_proxy_deployment.md`
- `docs/install_lifecycle.md`
- `docs/release_verification.md`
- `docs/release_signoff.md`
- `docs/rc_checklist_v1.0.md`

## Build

From `nebula/`:

```bash
cmake -S . -B build
cmake --build build -j
```

The compiler binary is `build/nebula`.

Try the bundled TCP loopback example with:

```bash
build/nebula run examples/tcp_loopback_echo --run-gate none
```

Try the one-request HTTP example with:

```bash
build/nebula run examples/http_hello_once --run-gate none
```

Try the route-param HTTP example with:

```bash
build/nebula run examples/http_route_param_once --run-gate none
```

Try the one-request HTTP+JSON example with:

```bash
build/nebula run examples/http_json_echo_once --run-gate none
```

Try the file-copy CLI example with:

```bash
printf 'nebula\n' > /tmp/nebula.in
build/nebula run examples/project_cli --run-gate none -- /tmp/nebula.in /tmp/nebula.out
cat /tmp/nebula.out
```

Try the combined CLI + backend-service workspace skeleton with:

```bash
build/nebula fetch examples/cli_service_workspace
NEBULA_BIND_HOST=127.0.0.1 NEBULA_PORT=40480 \
  build/nebula run examples/cli_service_workspace/apps/service --run-gate none
build/nebula run examples/cli_service_workspace/apps/ctl --run-gate none -- status
```

That workspace keeps the shared business/JSON contract in `packages/core`, the long-lived service in
`apps/service`, and the operator/dev CLI in `apps/ctl`. More detail lives in
`examples/cli_service_workspace/README.md`.

Try the backend-first internal control-plane workspace slice with:

```bash
build/nebula fetch examples/release_control_plane_workspace
NEBULA_BIND_HOST=127.0.0.1 NEBULA_PORT=40480 \
  build/nebula run examples/release_control_plane_workspace/apps/service --run-gate none
build/nebula run examples/release_control_plane_workspace/apps/ctl --run-gate none -- status
```

That workspace keeps the same `packages/core` / `apps/service` / `apps/ctl` split, but now grows
from current release state into approval decisions, append-only audit events, and an explicit local
apply-runner handoff on top of the preview `official/nebula-db-sqlite` package. More detail lives in
`examples/release_control_plane_workspace/README.md`.

Try the long-lived hello API starter with:

```bash
build/nebula run examples/hello_api --run-gate none
```

That starter demonstrates the current repo-local preview service stack; it is not a claim that the
binary release archives install a GA backend platform. For the recommended installed Linux backend
GA starter, use `nebula new --template backend-service`.

Try the PQC-signed service starter with:

```bash
build/nebula run examples/pqc_signed_service --run-gate none
```

Try the thin-host app-core sample with:

```bash
build/nebula run examples/thin_host_app_core --run-gate none
```

Its bridge contract is the repo-local preview package `official/nebula-thin-host-bridge`; it owns
versioned command/event/snapshot envelopes for an external host shell, not rendering or native UI
platform behavior.

Thin-host app-shell guidance lives in:

- `docs/thin_host_app_shell.md`

## Install

Nebula release assets cover:

- `nebula-v<version>-darwin-x86_64.tar.gz`
- `nebula-v<version>-darwin-arm64.tar.gz`
- `nebula-v<version>-linux-x86_64.tar.gz`
- `nebula-v<version>-windows-x86_64.zip`
- `nebula-v<version>-<target>.spdx.json`
- `nebula-v<version>-<target>.provenance.intoto.jsonl`
- `nebula-v<version>-<target>.sbom.intoto.jsonl`
- `SHA256SUMS.txt.intoto.jsonl`

GitHub Release is the official distribution source. Homebrew and the install scripts resolve from
those same artifacts.

Binary release assets and the install scripts install the compiler/CLI, bundled `std`, runtime
headers, and release documentation by default. On Linux x86_64 they can also install the backend
SDK when you opt in. They do not provision a host C++ compiler, `git`, or the remaining repo-local
preview packages under `official/`.

Install with the shipped scripts:

```bash
bash scripts/install.sh --version <version>
pwsh -File scripts/install.ps1 -Version <version>
```

Install the Linux backend SDK alongside the core CLI/tooling surface with:

```bash
bash scripts/install.sh --with-backend-sdk --version <version>
```

Opt in to stronger attestation verification:

```bash
bash scripts/install.sh --verify-attestations --version <version>
pwsh -File scripts/install.ps1 -VerifyAttestations -Version <version>
```

Defaults:

- Unix install location: `$HOME/.local/bin/nebula`
- PowerShell install location: `$HOME\\.local\\bin\\nebula.exe`
- Default release source: `https://github.com/billlza/nebula/releases/download/v<version>`

Supported overrides:

- `--prefix` or `NEBULA_INSTALL_PREFIX`
- `--version` or `NEBULA_INSTALL_VERSION`
- `--repo` or `NEBULA_INSTALL_REPOSITORY`
- `--base-url` or `NEBULA_INSTALL_BASE_URL`
- `--verify-attestations` / `-VerifyAttestations` or `NEBULA_INSTALL_VERIFY_ATTESTATIONS=1`

For local smoke, release dry-runs, or custom mirrors, point the installers at a local artifact directory:

```bash
bash scripts/install.sh --version <version> --prefix "$PWD/dist/install-smoke" --base-url "$PWD/dist"
pwsh -File scripts/install.ps1 -Version <version> -InstallPrefix "$PWD/dist/install-smoke" -BaseUrl "$PWD/dist"
```

Default installer verification is checksum-only. If you opt into `--verify-attestations` or
`-VerifyAttestations`, the installers require `gh` and verify:

- `SHA256SUMS.txt` with `SHA256SUMS.txt.intoto.jsonl`
- the selected platform archive with its provenance bundle
- the selected platform archive with its SBOM predicate bundle

If you point `--base-url` at a mirror or fork, also set `--repo` /
`NEBULA_INSTALL_REPOSITORY` so attestation verification matches the repository that signed the
bundles.

For signed provenance and SBOM verification details, see:

- `docs/install_lifecycle.md`
- `docs/release_verification.md`

Homebrew formula output is published as `nebula.rb` on each release. The release workflow also
smoke-tests a local-file formula before publishing.

Homebrew toolchain policy:

- The official Homebrew formula depends on `llvm`.
- The Homebrew launcher defaults `CXX` to Homebrew's `llvm/bin/clang++` only when `CXX` is unset.
- An explicit `CXX` still wins, so advanced users can override the host compiler path deliberately.

Host compiler requirement:

- `nebula build`, `nebula run`, `nebula test`, and `nebula bench` lower Nebula code to C++ and
  require a working host C++23 compiler.
- Nebula uses `$CXX` when it is set; otherwise it defaults to `clang++`.
- Official release tooling supports `clang++` as the default host compiler contract; Nebula does
  not silently fall back to `g++`.
- If the compiler is missing, Nebula now reports a dedicated CLI diagnostic instead of only a raw
  process failure.

Git dependency requirement:

- `nebula add --git`, `nebula fetch`, and `nebula update` require `git` on `PATH`.
- Remote git dependencies also require the current environment to have whatever network/auth access
  the target remote needs.

## CLI

- `nebula check <path>`: static analysis and diagnostics for a file, project dir, or `nebula.toml`
- `nebula build <path>`: compile and link
  - `--emit executable|staticlib|sharedlib` controls the build artifact kind
- `nebula run <path> [-- <program-args...>]`: preflight, build, execute
- `nebula test`: run `@test` harness; `--dir` accepts a raw source dir or a project/workspace root
- `nebula bench`: run `@bench` harness with stable clock/platform/perf capability output; `--dir`
  accepts a raw source dir or a project/workspace root
- `nebula new <path> [--template basic|cli|http-service|backend-service|control-plane-workspace]`: scaffold a self-contained Nebula starter or repo-preview workspace
- `nebula add <project> ...`: add an exact-version, path, or git dependency to `nebula.toml`
- `nebula publish <project>`: publish a package into a deterministic local file-based registry
- `nebula fetch <project>`: resolve dependencies and write `nebula.lock`
- `nebula update <project>`: refresh dependency resolution and update `nebula.lock`
- `nebula fmt <file-or-dir>`: rewrite Nebula sources into canonical formatting while preserving comments
- `nebula explain <path>`: query explain data in text or JSON via `path+span` or `symbol+file`,
  including async allocation metadata plus service/TLS/HTTP2 transport debug contracts for
  `RequestContext.transport_debug()` helpers, including HTTP/2 phase-event
  `classification.reason` / `classification.detail` metadata
- `nebula lsp`: start the language-server entrypoint with diagnostics, hover, definition,
  completion, references, document symbols, workspace symbols, signature help, rename, code
  actions, semantic tokens, and incremental document sync

Run `./build/nebula` to see full CLI flags.

Current 1.0 package-manager GA scope includes exact-version local-registry, `path`, and `git`
dependencies plus a reproducible `nebula.lock`. Registry resolution defaults to
`<project-or-workspace>/.nebula/registry` and can be overridden with `NEBULA_REGISTRY_ROOT`.
`[workspace]` manifests now participate in shared root-lock resolution for member-package
`fetch/update/check/build/run/test/bench`. Workspace-only roots resolve the first member package in
sorted member-path order as the deterministic default target. `nebula publish` writes deterministic
local-registry artifacts, and installed/repo-local Nebula layouts now ship hosted registry helpers
for remote exact-version publish/fetch/update flows.

Hosted registry flows are available directly from installed binaries and repo checkouts with:

- `nebula publish <project> --registry-url <url> [--registry-token <token>]`
- `nebula fetch <project> --registry-url <url> [--registry-token <token>]`
- `nebula update <project> --registry-url <url> [--registry-token <token>]`

This hosted path still mirrors remote exact-version packages into a local registry root before the
core resolver runs; it widens the distribution/install contract, not the core resolver model.

Current 1.0 primary authoring story is small-to-medium CLI / system tools. `nebula run` supports
argument pass-through via `--`, and CLI-oriented programs may use `fn main() -> Int` to return a
process exit code.

Executable packages may also declare a `[native]` section. The stable 1.0 slice now supports both:

- flat compatibility fields:
  - `c_sources`
  - `cxx_sources`
  - `include_dirs`
  - `defines`
- structured descriptor fields:
  - `[[native.sources]]` with `path`, `language = "c" | "cxx" | "asm"`, optional
    `include_dirs`, `defines`, `arch`, and `cpu_features`
  - `[[native.generated_headers]]` with `out`, `template`, and `values`

Those inputs are folded into manifest fingerprints, source/integrity fingerprints, lock replay, and
build cache identity. Generated headers are materialized into Nebula's own project-local generated
include tree rather than the package source tree. Library builds (`--emit staticlib|sharedlib`)
reject reachable `[native]` inputs for now rather than pretending to export a stable ABI through
them.

Repo-local preview packages currently include:

- `official/nebula-db-sqlite`
  - preview embedded-data slice for backend-first internal apps
  - current shipped surface:
    - runtime-backed SQLite connection / transaction / result-set / row handles
    - migration runner
    - narrow execute/query helpers
    - row getters for `String / Int / Bool / Json / Bytes`
  - current support matrix: macOS + Linux only
  - current distribution posture:
    - repo-local preview package from a checkout on all supported hosts
    - additionally shipped as an installed preview package inside the opt-in Linux backend SDK asset
      on Linux x86_64 via `db_sqlite = { installed = "nebula-db-sqlite" }`
    - still not part of the Linux backend SDK GA contract
  - current explicit non-goals: ORM, query DSL, cross-database abstraction, connection pools
- `official/nebula-db-postgres`
  - preview network-database slice for backend-first internal apps
  - current shipped surface:
    - runtime-backed PostgreSQL connection / result-set / row handles
    - dynamic `libpq` runtime probe and explicit missing-client diagnostics
    - migration runner
    - narrow execute/query helpers
    - row getters for `String / Int / Bool / Json`
  - current support matrix: macOS + Linux hosts with PostgreSQL client libraries available at
    runtime
  - current distribution posture:
    - repo-local preview package from a checkout on supported hosts
    - not included in the opt-in Linux backend SDK installed-preview payload yet
  - current forcing-app posture:
    - `examples/release_control_plane_workspace` can opt into it with `APP_DATA_BACKEND=postgres`
      plus `APP_POSTGRES_PREVIEW=1`, `APP_AUTH_REQUIRED=1`, and `APP_POSTGRES_DSN`
    - SQLite remains the default data plane; there is no automatic migration, pooling, ORM, or
      dual-write behavior
  - current explicit non-goals: ORM, query DSL, connection pools, transparent SQLite/Postgres
    abstraction, database server orchestration
- `official/nebula-config`
  - preview app-level config and mounted-secret lifecycle helpers for backend-first internal apps
  - current shipped surface:
    - required/optional env accessors
    - typed `Int` / `Bool` env parsing
    - direct-env or mounted-secret file resolution with mutual-exclusion checks
    - redacted diagnostic JSON for startup preflight
  - current distribution posture:
    - repo-local preview package from a checkout
    - not included in the opt-in Linux backend SDK installed-preview payload yet
  - current explicit non-goals: cloud KMS, dynamic secret rotation, secret storage, policy DSL
- `official/nebula-crypto`
  - import path: `import crypto::rand`, `import crypto::hash`, `import crypto::pqc.kem`,
    `import crypto::pqc.sign`
  - current shipped surface:
    - `crypto::rand::random_bytes(len: Int) -> Result<Bytes, String>`
    - `crypto::kdf::blake3_derive_key(context, material, out_len)`
    - `crypto::hash::blake3(data: Bytes) -> Bytes`
    - `crypto::hash::sha3_256(data: Bytes) -> Bytes`
    - `crypto::hash::sha3_512(data: Bytes) -> Bytes`
    - `crypto::hash::hex(data: Bytes) -> String`
    - `crypto::aead` with `ChaCha20-Poly1305` key/`seal`/`open`
    - `crypto::pqc.kem` with ML-KEM-768 keypair / encapsulate / decapsulate
    - `crypto::pqc.sign` with ML-DSA-65 keypair / sign / verify
  - current support matrix: macOS + Linux only
  - current explicit non-goals: TLS/HTTPS, PKI/certificates, Windows support
- `official/nebula-service`
  - env-driven bind config
  - bounded request limits
  - request timeout wrapping
  - JSON health/readiness helpers
- `official/nebula-observe`
  - structured JSON logs
  - counter-shaped metric events
- `official/nebula-pqc-protocols`
  - application-layer signed payload helpers built on `nebula-crypto`
  - ciphertext-only ML-KEM envelope helpers for transport/persistence paths
  - authenticated `pqc::channel` session establishment + encrypted message envelopes
- `official/nebula-tls`
  - outbound client-side TLS/HTTPS helpers on macOS + Linux only
- `official/nebula-qcomm-sim`
  - simulation-only BB84 experiments and report helpers
  - explicit experimental preview, not hardware/QKD support
- `official/nebula-thin-host-bridge`
  - command/event/snapshot envelope helpers for thin-host app cores
  - `correlation_id` and `state_revision` fields for replay parity and telemetry correlation
  - current explicit non-goals: renderer, widget toolkit, layout/style/animation, accessibility
    stack, native UI platform, packaging/signing/update lifecycle
- `official/nebula-ui`
  - semantic UI tree helpers for the preview `ui` / `view` syntax
  - headless adapter for deterministic CI/debug rendering
  - guarded AppKit/GTK minimal-window smoke assets
  - current explicit non-goals: mature renderer, style engine, animation system, native adapter parity

Preview examples for these repo-local surfaces include:

- `examples/pqc_secure_service`
- `examples/qcomm_bb84_lab`
- `examples/local_ops_console_ui`

These preview packages remain outside the installed GA surface even when one of them is shipped for
convenience inside the opt-in backend SDK payload. Today `official/nebula-db-sqlite` is the only
installed preview package in that category; `official/nebula-db-postgres` and the rest are still
consumed from the repo checkout with `path` dependencies until they have their own stable release
channels.

Performance positioning stays narrow on purpose: the next hard-win program focuses on
`std::bytes/std::json/std::http`, `nebula-service`, `nebula-crypto`, `nebula-tls`, and
`nebula-pqc-protocols` rather than widening the surface with a broad new stdlib wave before the
hot paths are measured.

Broader APP-platform positioning also stays explicit on purpose:

- Nebula is moving first toward a backend-first internal app platform, not directly toward a pure
  Nebula GUI/platform claim
- `Nebula UI` is now a preview semantic-UI lane: source syntax plus stable JSON IR first,
  headless rendering plus guarded AppKit/GTK minimal-window assets now, mature native adapters later,
  and no mature renderer claim yet
- the lane-by-lane maturity/gap contract lives in `docs/app_platform_convergence.md`
- universeOS positioning lives in `docs/universeos_convergence.md` and stays staged behind
  language, compiler, platform, and freestanding-runtime gates
- `benchmarks/backend_crypto` remains the current narrow hard-win matrix
- `benchmarks/app_platform` now fixes the representative APP-platform comparison workloads and
  responsibility split for future broader parity work

Useful planning commands:

```bash
python3 scripts/competitive_bench.py plan --format json
python3 scripts/app_platform_bench.py verify
python3 scripts/app_platform_bench.py plan --format json
```

Useful internal-app standard smoke:

```bash
python3 scripts/verify_release_control_plane_standard.py --binary ./build/nebula
```

Official binary support is:

- macOS x86_64
- macOS arm64
- Linux x86_64
- Windows x86_64

See `spec/SPEC.md` for layered docs:
- Language Core: `spec/language_core.md`
- Generics policy: `spec/generics_policy.md`
- C ABI interop: `spec/interop_c_abi.md`
- Static Analysis: `spec/static_analysis.md`
- Tooling/CLI: `spec/tooling_cli.md`
- Experimental/Infra: `spec/experimental_infra.md`
- Service profile: `docs/service_profile.md`
- Backend developer guide: `docs/backend_developer_guide.md`
- App platform convergence: `docs/app_platform_convergence.md`
- UniverseOS convergence: `docs/universeos_convergence.md`
- System profile: `docs/system_profile.md`
- Thin-host app-shell guide: `docs/thin_host_app_shell.md`
- Official package tiering: `docs/official_package_tiering.md`
- Support matrix: `docs/support_matrix.md`
- Stability policy: `docs/stability_policy.md`
- Package tiering: `docs/official_package_tiering.md`
- Install lifecycle: `docs/install_lifecycle.md`
- Release verification: `docs/release_verification.md`
- Release sign-off: `docs/release_signoff.md`
- RC checklist: `docs/rc_checklist_v1.0.md`

## Release metadata

- Version source of truth: `VERSION`
- License: Apache-2.0 in `LICENSE`
- Changelog: `CHANGELOG.md`
- Current release notes: `RELEASE_NOTES_v<VERSION>.md`
- Release process: `RELEASE_PROCESS.md`
- Release verification: `docs/release_verification.md`
- RC checklist: `docs/rc_checklist_v1.0.md`

## Project quick start

Scaffold and run a starter project:

```bash
./build/nebula new /tmp/hello-nebula
./build/nebula new /tmp/cli-nebula --template cli
./build/nebula new /tmp/http-nebula --template http-service
./build/nebula new /tmp/backend-nebula --template backend-service
./build/nebula new /tmp/control-plane-nebula --template control-plane-workspace
./build/nebula run /tmp/hello-nebula --run-gate none
```

Starter templates:

- `basic`: single-file hello world
- `cli`: multi-file file-copy CLI starter without external package dependencies
- `http-service`: long-lived stdlib HTTP service starter with `PORT`, `/healthz`, and `/hello/:name`
- `backend-service`: installed backend-SDK starter with `service::*`, `/healthz`, `/readyz`, request-id policy, and drain/shutdown handling
- `control-plane-workspace`: repo-preview backend-first internal app workspace starter that layers
  the backend SDK with installed-preview `nebula-db-sqlite`, repo-preview `nebula-db-postgres` /
  `nebula-tls`, and a release-state control-plane slice

For Linux backend GA work, prefer `backend-service`. The repo-local `examples/hello_api` project
remains a preview/reference example from a source checkout rather than the recommended installed
starter. `control-plane-workspace` is also repo-local preview wiring today and is not installed by
release archives.

The bundled 1.0 `std` now includes `std::fs` for synchronous string-path file helpers:

- `exists`
- `is_file`
- `is_dir`
- `read_bytes`
- `read_string`
- `write_bytes`
- `write_string`
- `create_dir_all`

Example project layout:

```text
hello-nebula/
  nebula.toml
  src/
    main.nb
    util.nb
```

Package imports use dependency-qualified module paths:

```nebula
import dep::util
```

And dependencies are declared in `nebula.toml`:

```toml
schema_version = 1

[package]
name = "hello-nebula"
version = "0.1.0"
entry = "src/main.nb"
src_dir = "src"

[dependencies]
dep = { path = "../dep" }
```

After adding dependencies, resolve them before `run/build/check`:

```bash
./build/nebula fetch /tmp/hello-nebula
./build/nebula run /tmp/hello-nebula --run-gate none
```

Nebula already has a usable module and package-distribution surface:

- same-package modules use `module foo.bar` plus `import foo.bar`
- dependency-qualified imports use `import dep::foo.bar`
- bundled std uses `import std::...`
- manifest dependency sources already cover `path`, `git`, Linux backend-SDK `installed`,
  exact-version local registry, and helper-backed hosted registry flows through the installed
  helper

The next competitive gap is not another import syntax. It is higher-performance backend/crypto
libraries plus reproducible benchmark proof on a fixed workload matrix. See
`docs/import_library_performance.md` and `benchmarks/backend_crypto/README.md`.

To seed a shared local registry from a package:

```bash
./build/nebula publish /tmp/dep
./build/nebula publish /tmp/dep --force
```

Re-publishing identical contents is a no-op. Changing contents under the same version is rejected
unless `--force` is passed.

## C ABI Export

Nebula's first mixed-language contract is a narrow generated C ABI surface.

Export a function:

```nebula
@export
@abi_c
fn add(a: Int, b: Int) -> Int {
  return a + b
}
```

Build a library:

```bash
./build/nebula build examples/c_abi_export --emit staticlib
./build/nebula build examples/c_abi_export --emit sharedlib
```

The build writes a library artifact plus a matching header in `generated_cpp/`.
Only the current root package's `@export @abi_c` functions become part of that generated public
surface; dependency-package exports are not re-exported transitively.
If you override the artifact path with `-o` or `--out-dir`, the generated header follows the
library artifact and is written next to it.

Current ABI-safe exported types are intentionally narrow:

- `Int`
- `Float`
- `Bool`
- `Void`

Today the repo validates C++ consumption directly. Rust/Swift/JavaScript remain directional
integration paths through the generated C ABI or a process boundary rather than host-native ABI
promises.

`String`, `Result`, `struct`, `enum`, `ref` parameters, and generic exports are rejected from the
public C ABI in the current slice.

## Contract Tests

Nebula ships a CLI/diagnostics contract suite:

```bash
python3 tests/run.py --suite all --report text
```

You can run specific slices with `--suite` (`check|build|run|test|bench|safety`) and filter by case id with `--filter`.

## Cross-stage baseline

For a deterministic off-vs-safe comparison on run-stage cross-stage reuse:

```bash
./scripts/run_cross_stage_baseline.sh
```

This command does not build; it expects `build/nebula` to already exist and writes:
- `benchmark_results/cross_stage_reuse_baseline.csv`
- `benchmark_results/cross_stage_reuse_baseline.json`

Optional one-shot baseline + perf diff:

```bash
./scripts/run_cross_stage_baseline.sh \
  build/nebula tests/fixtures/smoke.nb benchmark_results \
  --perf-current /tmp/current_perf.json \
  --perf-baseline /tmp/baseline_perf.json \
  --perf-diff-json benchmark_results/perf_baseline_diff.json \
  --perf-diff-md benchmark_results/perf_baseline_diff.md
```

## Perf baseline diff

For current-vs-baseline performance judgement from `tests/run.py --perf-json-out`:

```bash
python3 scripts/perf_baseline_diff.py \
  --current /tmp/current_perf.json \
  --baseline /tmp/baseline_perf.json \
  --out-json /tmp/perf_diff.json \
  --out-md /tmp/perf_diff.md
```

Useful controls:
- `--fail-on-regression on|off` (default `off`)
- `--max-total-regression-pct` (default `15`)
- `--max-suite-regression-pct` (default `20`)
- `--max-case-regression-ms` (default `250`)
- `--min-case-duration-ms` (default `200`)
- `--max-cross-stage-reused-drop` (default `2`)
- `--max-cross-stage-saved-ms-drop` (default `200`)
- `--max-disk-hit-drop` (default `2`)
- `--max-disk-miss-increase` (default `10`)
- `--max-disk-eviction-increase` (default `5`)
- `--max-grouping-total-ms-increase` (default `400`)
- `--max-grouping-budget-fallback-increase` (default `0`)

## Artifact Policy

Build/test outputs are local artifacts and should not be committed:

- `build/`
- `generated_cpp/`
- `tests/artifacts/`
- `benchmark_results/`

See `ARTIFACT_POLICY.md` for cleanup and retention rules.
