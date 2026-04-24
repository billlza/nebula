# Stability Policy

Nebula uses a split stability policy between the compiler/tooling core, the Linux backend SDK GA
surface, and remaining preview packages.

Policy:

- `1.0.x` is the compatibility branch for the compiler, CLI, package workflow, bundled `std`,
  runtime headers, and documented release assets.
- `main` continues forward-looking work such as broader backend/platform capabilities.
- `nebula-service` and `nebula-observe` are part of the Linux backend SDK GA surface when installed
  through the opt-in backend SDK asset.
- `nebula-db-sqlite` may also be shipped inside the opt-in backend SDK asset as an installed
  preview package; that distribution path does not promote it to backend SDK GA.
- Remaining repo-local packages under `official/` are documented preview surfaces, not installed GA
  contract surfaces.
- Service-platform claims stay constrained to the documented service profile and support matrix.
- Experimental preview packages such as `official/nebula-qcomm-sim` sit even outside the normal
  pilot service profile: they are for protocol experimentation and differentiation work, not for
  security guarantees or hardware-support claims.

Compatibility expectations:

- documented CLI flags and package-manager semantics should remain stable through `1.0.x`
- preview package APIs may grow or tighten within their documented pilot boundaries
- experimental preview packages may grow, tighten, or be reshaped more aggressively than normal
  pilot packages as long as their experimental status remains explicit in docs/release notes
- widening production claims requires docs and release-note updates, not just code landing
