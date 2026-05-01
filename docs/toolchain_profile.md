# Toolchain Profile

This document maps Nebula's installable toolchain pieces to a Rustup-style mental model without
claiming that Nebula has a channel manager or target component downloader.

## Core Profile

The default binary release and installers provide the core profile:

- `bin/nebula`: compiler, CLI, package workflow, `check`, `build`, `run`, `test`, and `bench`
- `share/nebula/std`: bundled Nebula `std` source files used by installed builds
- `include/runtime`: C++ runtime headers required by generated hosted artifacts
- `share/nebula/registry`: hosted-registry helper scripts used by package fetch/update workflows
- `share/doc/nebula`: release documentation, support matrix, install lifecycle, and this profile

This is Nebula's equivalent of having the compiler, package workflow, standard library sources,
runtime support, and documentation present after install.

## External Prerequisites

The core profile intentionally does not vendor every host dependency:

- `clang++` remains the default hosted C++ compiler when `CXX` is unset.
- `git` is required for git-backed dependencies and source checkouts.
- Python 3.11+ is required for hosted-registry helper workflows.
- CMake is required for source builds and release packaging, not for every installed CLI use.

Installers print these prerequisites and fail fast on unsupported release targets or broken
release artifacts.

## Lint And Check Surface

Nebula does not ship a separate `clippy`-style binary. The current checked surface is:

- `nebula check` for parsing, type checking, package graph validation, and warning diagnostics
- contract tests under `tests/cases` for product and release behavior
- focused scripts such as `scripts/app_platform_bench.py verify` for benchmark matrix drift

Warnings in release/app validation gates are treated as failures by the repo's contract tests and
verification scripts.

## Optional Backend SDK Profile

The Linux x86_64 backend SDK is an opt-in profile, installed separately with
`scripts/install.sh --with-backend-sdk` or the equivalent PowerShell flow where supported by the
release channel.

It includes:

- `nebula-service` and `nebula-observe` as backend GA installed packages
- `nebula-auth`, `nebula-config`, `nebula-db-sqlite`, and `nebula-jobs` as installed-preview
  packages
- backend docs and an installed `hello_api` example

It does not promote repo-local preview packages such as Postgres, TLS, PQC, QKD, or UI into GA.

## Target And Preview Boundaries

Nebula does not currently download per-target standard libraries. The bundled `std` is source-level
and hosted builds rely on the host C++ toolchain for ABI and platform linkage.

Current release targets are documented in `docs/support_matrix.md`. Experimental system-profile
flags (`--target system|freestanding|<triple>`, `--profile system`, `--no-std`) are contract gates,
not a freestanding runtime or kernel support claim.

Repo-local preview packages remain source-checkout dependencies unless the support matrix says they
are part of the backend SDK installed-preview profile.

## Upgrade Shape

Nebula's update equivalent is installer-driven:

- install or upgrade a specific release version with `scripts/install.sh --version <version>` or
  `scripts/install.ps1 -Version <version>`
- optionally verify provenance/SBOM attestations before extraction
- run post-upgrade smoke: `nebula --version`, `nebula new /tmp/nebula-smoke`, and
  `nebula run /tmp/nebula-smoke --run-gate none`

There is no `nebula toolchain update stable` command in this wave.
