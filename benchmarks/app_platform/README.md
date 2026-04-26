# App Platform Convergence Matrix

This directory fixes Nebula's broader APP-platform comparison story to one narrow, honest lane:

- backend-first internal applications first
- thin-host UI contracts second
- broader pure-Nebula app-platform claims only after those two lanes are real

Unlike `benchmarks/backend_crypto`, this matrix does not claim a hard-win story today. It defines
the fixed workloads and reference stacks that future APP-platform maturity claims must answer.

## Fixed Workloads

- `cli_cold_start`
  - operator/dev CLI startup on a representative real workspace CLI
- `service_json_db_crud`
  - long-lived service plus JSON plus embedded-data CRUD path
- `thin_host_bridge_roundtrip`
  - host<->core command/query/event bridge round-trip path
- `state_sync_latency`
  - state transition plus snapshot/update propagation path
- `resident_memory`
  - steady-state memory footprint for a representative long-lived internal app service
- `nebula_ui_startup`
  - Nebula UI semantic tree construction startup path
- `ui_action_roundtrip`
  - action identifier extraction and validation from UI JSON IR
- `ui_snapshot_render`
  - UI JSON IR stringify/render handoff path

## Reference Stacks

- `C++`
  - `clang/cmake + asio/beast + sqlite + Qt`
- `Rust`
  - `cargo + tokio/axum + serde + rusqlite/sqlx + tauri/egui`
- `Swift`
  - `SwiftPM + SwiftNIO/Vapor + Foundation + SQLite wrapper + SwiftUI/AppKit/UIKit`

## Responsibility Split

Measurements stay partitioned into:

- `Nebula-owned`
- `Host-owned`
- `Ops-owned`

This prevents host-renderer costs or packaging-system costs from being misreported as if they were
pure Nebula runtime behavior.

## Current Repo State

- the workload manifest is fixed in `matrix.json`
- the current planning/validation helper is `scripts/app_platform_bench.py`
- C++ reference workloads now exist for the first Nebula-owned backend and thin-host hot paths:
  - `service_json_db_crud`
  - `thin_host_bridge_roundtrip`
  - `state_sync_latency`
- Nebula-backed runnable workloads now exist for:
  - `cli_cold_start`
  - `service_json_db_crud`
  - `thin_host_bridge_roundtrip`
  - `state_sync_latency`
  - `resident_memory`
  - `nebula_ui_startup`
  - `ui_action_roundtrip`
  - `ui_snapshot_render`
- the extracted thin-host bridge is now consumed by:
  - `official/nebula-thin-host-bridge`
  - `examples/thin_host_app_core`
  - `examples/thin_host_bridge_replay`
  - `examples/thin_host_bridge_contract`
- the representative Nebula paths are fixed to the existing repo samples
- this wave now includes real Nebula workload execution for the current CLI, service, thin-host,
  and ops-memory lanes, while the broader cross-language matrix still remains a staged plan rather
  than a filled diff table
- the C++ reference lane is measurement-only and currently covers a SQLite-backed backend CRUD
  baseline plus single-file STL app-core baselines, not Qt/asio/beast/renderer parity
- thin-host timed workloads validate canonical event/snapshot wire text in the timed loop, matching
  the C++ reference lane; broader malformed/schema-mismatch parsing remains covered by bridge
  contract tests rather than repeated inside the microbenchmark

## Useful Commands

```bash
python3 scripts/app_platform_bench.py verify
python3 scripts/app_platform_bench.py plan --format json
python3 scripts/app_platform_bench.py plan
python3 scripts/app_platform_bench.py run-nebula --binary ./build/nebula --workload thin_host_bridge_roundtrip --workload state_sync_latency --json-out artifacts/app-platform-nebula.json
python3 scripts/app_platform_bench.py run-reference --stack cpp --workload service_json_db_crud --workload thin_host_bridge_roundtrip --workload state_sync_latency --json-out artifacts/app-platform-cpp.json
python3 scripts/app_platform_bench.py compare --stack cpp --nebula-json artifacts/app-platform-nebula.json --reference-json artifacts/app-platform-cpp.json
```
