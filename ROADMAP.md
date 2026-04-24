# Public Roadmap

Current release lane: `1.0.x`

Release-branch posture for `1.0.x`:

- `release/1.0`: blocker fixes, documentation corrections, release engineering hardening
- `main`: post-1.0 work only after GA cut
- `1.0.x` hardening already includes:
  - Linux backend SDK release/install gating
  - daily-use LSP baseline
  - helper-backed hosted registry installed-binary path

Wave A: Pilot Platform

- bounded HTTP service helpers
- structured JSON logs and counter events
- service profile and reverse-proxy deployment guidance
- reference API and PQC-signed service examples

Wave B: Usable Ecosystem

Wave B is post-1.0 / `1.1+` scope:

- docs portal and richer quickstarts
- faster workspace-scale editor indexing and richer code actions beyond the current daily-use LSP surface
- broader starter kits
- service-platform lift on top of preview packages:
  - route composition and richer request-context helpers
  - stronger graceful-drain / shutdown ergonomics beyond the current drain-file preview
  - richer config / secrets / deployment guidance
  - hosted registry hardening beyond the current helper-backed installed-binary path

Wave C: Differentiated Story

Wave C is also post-1.0 / `1.1+` scope:

- Backend + Crypto hard-win performance program:
  - optimize existing hot-path library surfaces before inventing a broad new stdlib wave
  - target `std::bytes`, `std::json`, `std::http`, `nebula-service`, `nebula-crypto`,
    `nebula-tls`, and `nebula-pqc-protocols`
  - hold public performance claims behind a fixed five-workload competitive matrix and explicit
    win/loss thresholds
- stronger PQC protocol stories
- deeper crypto lifecycle hardening
- storage/connectivity integrations such as Postgres
- stronger production proofs and interoperability evidence

Wave C.5: Backend-First Internal App Platform

Wave C.5 is post-1.0 / `1.1+` scope and must land before broad pure-Nebula app-platform claims:

- treat `release_control_plane_workspace` as the forcing-app lane for a publishable internal app
- stabilize the thin-host split as an app-core contract rather than a GUI-platform claim
- hold broader APP-platform comparisons behind the fixed `benchmarks/app_platform` matrix
- keep public positioning aligned with `docs/app_platform_convergence.md`

Wave D: Native UI Platform Exploration

Wave D is post-1.0 exploratory scope, not a current release contract:

- thin-host + Nebula app-core examples on top of the existing C ABI / native bridge
- API design work for `ui::app`, `ui::window`, `ui::event`, `ui::text`, `ui::layout`, and `ui::assets`
- packaging/signing/update lifecycle requirements for a future desktop release lane

Wave E: UniverseOS Convergence

Wave E is a staged product direction, not a current release contract:

- use Nebula first for universeOS tools, control plane, backend services, and thin-host app cores
- keep kernel, driver, and freestanding runtime claims blocked on a no-std/system profile
- close language gaps such as constrained generics, stricter region/ownership semantics,
  collections, closures, visibility, and deeper pattern matching before broad ecosystem claims
- evaluate LLVM/Cranelift/direct object backend options before claiming independence from the
  C++23 transpiler path
- keep public positioning aligned with `docs/universeos_convergence.md`
