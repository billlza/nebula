# Backend + Crypto Competitive Matrix

This directory fixes Nebula's next performance story to a narrow backend/crypto workload set.

The goal is not a vague "faster than every systems language" claim. The goal is a reproducible
hard-win story on specific hot paths that matter to Nebula's current product lane.

## Fixed Workloads

- `http_route_roundtrip`
  - HTTP request parse + route + response writeback hot path
- `json_payload_roundtrip`
  - JSON parse/stringify service payload path
- `bytes_frame_hotpath`
  - `Bytes` framing/copy/encoding path
- `hash_throughput`
  - BLAKE3 / SHA3 throughput path
- `aead_pqc_core`
  - AEAD seal/open plus PQC encapsulate/decapsulate path

## Claim Gates

- Nebula must lead on at least `3/5` workloads versus the reference C++/Rust implementations.
- A lead means `>= 10%`.
- A non-win must not be worse than `>-5%`.
- Swift is only counted on fair macOS workloads rather than being forced into mismatched host or
  library environments.

## Current Repo State

- the workload manifest is fixed in `matrix.json`
- Nebula benchmark implementations are committed under `nebula/`
- external reference implementations are committed under `reference/cpp`, `reference/rust`, and
  `reference/swift`
- crypto-oriented external references reuse a shared C support layer under `ref_support/`
- the runner script is `scripts/competitive_bench.py`
- the runner emits a neutral machine-readable result schema:
  - `schema_version = 1`
  - `kind = backend_crypto_nebula_results | backend_crypto_reference_results | backend_crypto_competitive_results`
- C++/Rust/Swift reference slots are fixed by manifest and toolchain detection, but hard-win claims
  stay blocked until those reference implementations are filled in and measured

## Useful Commands

```bash
python3 scripts/competitive_bench.py verify
python3 scripts/competitive_bench.py plan --format json
python3 scripts/competitive_bench.py run-nebula --binary ./build/nebula --json-out /tmp/backend_crypto_results.json
python3 scripts/competitive_bench.py run-reference --language cpp --workload hash_throughput
python3 scripts/competitive_bench.py run-matrix --binary ./build/nebula --json-out /tmp/backend_crypto_matrix.json
```

`run-matrix` executes Nebula plus the available external reference implementations for the selected
workloads, then emits per-language metrics and Nebula-vs-reference diff summaries in one JSON
payload.
