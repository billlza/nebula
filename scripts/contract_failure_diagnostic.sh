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
  # The crypto/pqc/qcomm posix_memalign failures are fixed (gnu11); only the
  # shell-only stragglers that still fail on CI but pass locally remain. Keep the
  # filter to those so the diagnostic runs fast.
  FILTERS=(
    TST-060-install-ps1-help-surface
    TST-201-competitive-benchmark-matrix-crypto-smoke
    TST-176-reverse-proxy-backend-service-smoke
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

    # Dump step-output files (e.g. a shell step that redirects to work/*.out):
    # for shell-only cases (ps1 help, benchmarks, reverse-proxy) the real error
    # is captured there, not via an app re-run below.
    {
      echo "=== captured step-output files under work/ ==="
      find "$sb/work" -maxdepth 4 -type f \( -name '*.out' -o -name '*.txt' -o -name '*.log' -o -name '*.json' -o -name '*.err' \) 2>/dev/null | head -40 | while read -r of; do
        sz=$(wc -c < "$of" 2>/dev/null || echo 0)
        echo "----- $of (${sz} bytes) -----"
        head -c 4000 "$of" 2>/dev/null
        echo
      done
    } 2>&1 | tee "diag/$f.stepout.log"

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

    # Re-run the case's own shell steps with full output. The harness keeps only
    # each step's rc, so a stdout-only failure (e.g. the reverse-proxy probe) is
    # otherwise invisible. Re-run in the kept sandbox with the harness env.
    casedir="$(find tests/cases -type d -name "$f" 2>/dev/null | head -1)"
    if [ -n "$casedir" ] && [ -f "$casedir/case.toml" ]; then
      python3 - "$casedir/case.toml" "$sb" "$BIN" "$REPO_ROOT" <<'PY' 2>&1 | tee "diag/$f.stepreplay.log" || true
import os, subprocess, sys
try:
    import tomllib
except ModuleNotFoundError:
    sys.exit(0)
toml_path, sandbox, binary, repo_root = sys.argv[1:5]
case = tomllib.load(open(toml_path, "rb"))
env = {**os.environ, "NEBULA_BINARY": binary, "NEBULA_REPO_ROOT": repo_root}
for i, step in enumerate(case.get("steps", [])):
    if step.get("kind") != "shell":
        continue
    print(f"===== replay shell step {i+1} =====", flush=True)
    try:
        r = subprocess.run(step["run"], shell=True, env=env, cwd=sandbox,
                           capture_output=True, text=True, timeout=180)
        print("rc=", r.returncode)
        print("--- stdout (tail) ---\n" + (r.stdout or "")[-4000:])
        print("--- stderr (tail) ---\n" + (r.stderr or "")[-4000:])
    except Exception as exc:
        print("replay error:", exc)
PY
    fi
    echo "::endgroup::"
  done
done

echo "diagnostic complete; logs in diag/"
exit 0
