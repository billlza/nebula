#!/usr/bin/env bash
# Diagnostic for the contract-suite cases that fail only on the CI toolchain
# (ubuntu/GCC) yet pass locally (macOS/clang): the vendored official/* crypto,
# pqc, and qcomm smokes and the heavy release-control-plane workspace smokes.
#
# The contract harness only reports "step N: expected rc=0, got rc=1"; this
# script re-runs each failing case with --keep-temp, then dumps the sandbox and
# re-executes the underlying `nebula build`/`run` with FULL output so the real
# compiler/runtime error on the CI toolchain is visible in the job log.
#
# Usage: scripts/contract_failure_diagnostic.sh [path-to-nebula-binary] [filter...]
# Non-fatal: always exits 0 so it can run as a non-blocking diagnostic job.
set -u

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

BIN_ARG="${1:-build/nebula}"
case "$BIN_ARG" in
  /*) BIN="$BIN_ARG" ;;
  *)  BIN="$REPO_ROOT/$BIN_ARG" ;;
esac
shift || true

FILTERS=("$@")
if [ "${#FILTERS[@]}" -eq 0 ]; then
  FILTERS=(
    TST-111-official-crypto-invalid-bytes-rejected
    TST-131-official-pqc-protocols-package-smoke
    TST-155-official-qcomm-sim-bb84-smoke
    TST-292-official-pqc-channel-pinned-initiator-smoke
    TST-224-example-release-control-plane-workspace-approval-runner-smoke
    # Fail on CI but pass locally and are NOT timeouts: ps1 help surface (pwsh on
    # Linux) and a competitive crypto benchmark (CI-hardware perf). Capture their
    # real error here.
    TST-060-install-ps1-help-surface
    TST-201-competitive-benchmark-matrix-crypto-smoke
  )
fi

mkdir -p diag
echo "diagnostic: binary=$BIN"
"$BIN" --version 2>&1 | head -1 || true

for f in "${FILTERS[@]}"; do
  echo "::group::harness $f"
  python3 tests/run.py --suite all --filter "$f" --keep-temp --report text \
    --binary "$BIN" 2>&1 | tee "diag/$f.harness.log" || true
  echo "::endgroup::"

  # The harness prints "sandbox=<path>" for each case; re-run inside it.
  grep -oE 'sandbox=[^[:space:]]+' "diag/$f.harness.log" | sed 's/sandbox=//' | sort -u | while read -r sb; do
    [ -d "$sb" ] || continue
    echo "::group::diag $f :: $sb"
    {
      echo "=== sandbox tree (depth 4) ==="
      find "$sb" -maxdepth 4 \( -name '*.nb' -o -name 'nebula.toml' -o -name '*.cpp' -o -name '*.log' \) 2>/dev/null | head -80
    } | tee "diag/$f.tree.log"

    # Re-run every app (dir holding nebula.toml under work/) with full output so
    # the real build/run error surfaces.
    find "$sb" -path '*/work/*' -name nebula.toml 2>/dev/null | while read -r toml; do
      app="$(dirname "$toml")"
      # Emit the generated C++ so a CI-toolchain (gcc) compile error is visible.
      echo "----- nebula build --emit-cpp $app -----"
      ( cd "$sb" && "$BIN" build "$app" --emit-cpp --out-dir "$REPO_ROOT/diag/cpp/$f-$(basename "$app")" ) \
        2>&1 | tee -a "diag/$f.rerun.log" || true
      # Re-run the actual failing step with full output.
      echo "----- nebula run $app -----"
      ( cd "$sb" && "$BIN" run "$app" --run-gate none ) 2>&1 | tee -a "diag/$f.rerun.log" || true
    done
    echo "::endgroup::"
  done
done

echo "diagnostic complete; logs in diag/"
exit 0
