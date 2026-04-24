# Import, Library Distribution, and Performance Positioning

This document keeps three related facts aligned:

- Nebula already has a usable module/import model.
- Nebula already has multiple package-distribution channels.
- Nebula's next competitive gap is performance proof on high-value backend/crypto workloads, not a
  new import syntax.

## Import Surface

Nebula's source-level import contract is already established:

- same-package modules:
  - `module foo.bar`
  - `import foo.bar`
- dependency-qualified package imports:
  - `import dep::foo.bar`
- compiler-shipped bundled std imports:
  - `import std::task`
  - `import std::time`
  - `import std::env`
  - `import std::result`
  - `import std::log`
  - `import std::bytes`
  - `import std::fs`
  - `import std::net`
  - `import std::http`
  - `import std::http_json`
  - `import std::json`

The important stability point is that package source changes do not change the import form. A
dependency still looks like `import dep::foo.bar` whether it comes from `path`, `git`, `installed`,
or an exact-version registry source.

## Package Distribution Surface

Nebula already supports practical library distribution through these manifest sources:

- repo-local package paths:
  - `dep = { path = "../dep" }`
- git sources:
  - `dep = { git = "https://...", rev = "..." }`
- installed backend SDK packages:
  - `service = { installed = "nebula-service" }`
  - `observe = { installed = "nebula-observe" }`
  - `db_sqlite = { installed = "nebula-db-sqlite" }` on Linux x86_64 when the backend SDK asset is
    installed, while still remaining preview
- exact-version local registry packages:
  - `dep = "1.2.3"`
- hosted registry flows through the installed helper:
  - `nebula publish --registry-url ...`
  - `nebula fetch --registry-url ...`
  - `nebula update --registry-url ...`

Current positioning:

- bundled `std::*` is compiler-shipped
- Linux backend SDK packages are installed on explicit opt-in
- preview packages under `official/*` remain non-GA even when one of them is shipped as an
  installed preview payload inside the opt-in backend SDK asset

## What Is Not Missing

Nebula does not currently need:

- a second dependency-qualified import syntax
- a new broad import grammar for hosted/installed packages
- a new dependency model before library quality improves

The next meaningful gap is library competitiveness and proof, not import plumbing.

## Performance Lane: Backend + Crypto

The current performance lane is intentionally narrow:

- optimize existing hot-path surfaces first:
  - `std::bytes`
  - `std::json`
  - `std::http`
  - `official/nebula-service`
  - `official/nebula-crypto`
  - `official/nebula-tls`
  - `official/nebula-pqc-protocols`
- do not start with a broad new stdlib wave
- only split out a new high-performance library when repeated benchmark evidence shows a stable hot
  path worth isolating

This is a "narrow hard-win" program, not a blanket claim that Nebula already beats C++/Rust/Swift
everywhere.

## Competitive Benchmark Matrix

The committed benchmark matrix lives under `benchmarks/backend_crypto/` and fixes five target
workloads:

- `http_route_roundtrip`: HTTP request parse + route + response writeback hot path
- `json_payload_roundtrip`: JSON parse/stringify service payload path
- `bytes_frame_hotpath`: `Bytes` framing/copy/encoding hot path
- `hash_throughput`: BLAKE3 / SHA3 throughput path
- `aead_pqc_core`: AEAD seal/open plus PQC encapsulate/decapsulate core path

Claim gates stay explicit:

- at least `3/5` selected workloads must show reproducible Nebula leadership versus the reference
  C++/Rust implementation set
- a win means `>= 10%` faster on the fixed workload
- non-winning workloads must not fall behind by more than `>-5%`
- Swift comparisons only count on fair macOS workloads instead of being forced into mismatched host
  or library environments

The current repo wave commits the matrix contract, the Nebula benchmark workloads, and the runner
that emits stable machine/toolchain metadata. Hard-win public claims remain gated on filling in and
running the C++/Rust/Swift reference implementations against this fixed matrix.
