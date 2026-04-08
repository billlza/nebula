#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

usage() {
  cat <<EOF
usage:
  $(basename "$0") [BIN] [SRC] [OUT_DIR] [JSON_PATH] [options]

positional defaults:
  BIN      = $ROOT/build/nebula
  SRC      = $ROOT/tests/fixtures/smoke.nb
  OUT_DIR  = $ROOT/benchmark_results
  JSON_PATH= <OUT_DIR>/cross_stage_reuse_baseline.json

optional perf diff options:
  --perf-current <path>
  --perf-baseline <path>
  --perf-diff-json <path>            (default: <OUT_DIR>/perf_baseline_diff.json)
  --perf-diff-md <path>              (default: <OUT_DIR>/perf_baseline_diff.md)
  --perf-fail-on-regression on|off   (default: off)
  --max-total-regression-pct <float> (default: 15)
  --max-suite-regression-pct <float> (default: 20)
  --max-case-regression-ms <int>     (default: 250)
  --min-case-duration-ms <int>       (default: 200)
  --max-cross-stage-reused-drop <int>      (default: 2)
  --max-cross-stage-saved-ms-drop <int>    (default: 200)
  --max-disk-hit-drop <int>                (default: 2)
  --max-disk-miss-increase <int>           (default: 10)
  --max-disk-eviction-increase <int>       (default: 5)
  --max-grouping-total-ms-increase <int>         (default: 400)
  --max-grouping-budget-fallback-increase <int>  (default: 0)

notes:
  - existing cross-stage baseline outputs are always generated.
  - perf diff runs only when both --perf-current and --perf-baseline are provided.
EOF
}

POSITIONAL=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --*)
      break
      ;;
    *)
      if [[ ${#POSITIONAL[@]} -ge 4 ]]; then
        echo "error: too many positional args: $1" >&2
        usage >&2
        exit 2
      fi
      POSITIONAL+=("$1")
      shift
      ;;
  esac
done

BIN="${POSITIONAL[0]-$ROOT/build/nebula}"
SRC="${POSITIONAL[1]-$ROOT/tests/fixtures/smoke.nb}"
OUT_DIR="${POSITIONAL[2]-$ROOT/benchmark_results}"
if [[ ${#POSITIONAL[@]} -ge 4 ]]; then
  JSON_PATH="${POSITIONAL[3]}"
else
  JSON_PATH="$OUT_DIR/cross_stage_reuse_baseline.json"
fi

PERF_CURRENT=""
PERF_BASELINE=""
PERF_DIFF_JSON="$OUT_DIR/perf_baseline_diff.json"
PERF_DIFF_MD="$OUT_DIR/perf_baseline_diff.md"
PERF_FAIL_ON_REGRESSION="off"
PERF_MAX_TOTAL_REGRESSION_PCT="15"
PERF_MAX_SUITE_REGRESSION_PCT="20"
PERF_MAX_CASE_REGRESSION_MS="250"
PERF_MIN_CASE_DURATION_MS="200"
PERF_MAX_CROSS_STAGE_REUSED_DROP="2"
PERF_MAX_CROSS_STAGE_SAVED_MS_DROP="200"
PERF_MAX_DISK_HIT_DROP="2"
PERF_MAX_DISK_MISS_INCREASE="10"
PERF_MAX_DISK_EVICTION_INCREASE="5"
PERF_MAX_GROUPING_TOTAL_MS_INCREASE="400"
PERF_MAX_GROUPING_BUDGET_FALLBACK_INCREASE="0"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --perf-current)
      [[ $# -ge 2 ]] || { echo "error: missing value for --perf-current" >&2; exit 2; }
      PERF_CURRENT="$2"
      shift 2
      ;;
    --perf-baseline)
      [[ $# -ge 2 ]] || { echo "error: missing value for --perf-baseline" >&2; exit 2; }
      PERF_BASELINE="$2"
      shift 2
      ;;
    --perf-diff-json)
      [[ $# -ge 2 ]] || { echo "error: missing value for --perf-diff-json" >&2; exit 2; }
      PERF_DIFF_JSON="$2"
      shift 2
      ;;
    --perf-diff-md)
      [[ $# -ge 2 ]] || { echo "error: missing value for --perf-diff-md" >&2; exit 2; }
      PERF_DIFF_MD="$2"
      shift 2
      ;;
    --perf-fail-on-regression)
      [[ $# -ge 2 ]] || { echo "error: missing value for --perf-fail-on-regression" >&2; exit 2; }
      PERF_FAIL_ON_REGRESSION="$2"
      shift 2
      ;;
    --max-total-regression-pct)
      [[ $# -ge 2 ]] || { echo "error: missing value for --max-total-regression-pct" >&2; exit 2; }
      PERF_MAX_TOTAL_REGRESSION_PCT="$2"
      shift 2
      ;;
    --max-suite-regression-pct)
      [[ $# -ge 2 ]] || { echo "error: missing value for --max-suite-regression-pct" >&2; exit 2; }
      PERF_MAX_SUITE_REGRESSION_PCT="$2"
      shift 2
      ;;
    --max-case-regression-ms)
      [[ $# -ge 2 ]] || { echo "error: missing value for --max-case-regression-ms" >&2; exit 2; }
      PERF_MAX_CASE_REGRESSION_MS="$2"
      shift 2
      ;;
    --min-case-duration-ms)
      [[ $# -ge 2 ]] || { echo "error: missing value for --min-case-duration-ms" >&2; exit 2; }
      PERF_MIN_CASE_DURATION_MS="$2"
      shift 2
      ;;
    --max-cross-stage-reused-drop)
      [[ $# -ge 2 ]] || { echo "error: missing value for --max-cross-stage-reused-drop" >&2; exit 2; }
      PERF_MAX_CROSS_STAGE_REUSED_DROP="$2"
      shift 2
      ;;
    --max-cross-stage-saved-ms-drop)
      [[ $# -ge 2 ]] || { echo "error: missing value for --max-cross-stage-saved-ms-drop" >&2; exit 2; }
      PERF_MAX_CROSS_STAGE_SAVED_MS_DROP="$2"
      shift 2
      ;;
    --max-disk-hit-drop)
      [[ $# -ge 2 ]] || { echo "error: missing value for --max-disk-hit-drop" >&2; exit 2; }
      PERF_MAX_DISK_HIT_DROP="$2"
      shift 2
      ;;
    --max-disk-miss-increase)
      [[ $# -ge 2 ]] || { echo "error: missing value for --max-disk-miss-increase" >&2; exit 2; }
      PERF_MAX_DISK_MISS_INCREASE="$2"
      shift 2
      ;;
    --max-disk-eviction-increase)
      [[ $# -ge 2 ]] || { echo "error: missing value for --max-disk-eviction-increase" >&2; exit 2; }
      PERF_MAX_DISK_EVICTION_INCREASE="$2"
      shift 2
      ;;
    --max-grouping-total-ms-increase)
      [[ $# -ge 2 ]] || { echo "error: missing value for --max-grouping-total-ms-increase" >&2; exit 2; }
      PERF_MAX_GROUPING_TOTAL_MS_INCREASE="$2"
      shift 2
      ;;
    --max-grouping-budget-fallback-increase)
      [[ $# -ge 2 ]] || { echo "error: missing value for --max-grouping-budget-fallback-increase" >&2; exit 2; }
      PERF_MAX_GROUPING_BUDGET_FALLBACK_INCREASE="$2"
      shift 2
      ;;
    *)
      echo "error: unknown option/arg: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ ! -x "$BIN" ]]; then
  echo "error: nebula binary not found or not executable: $BIN" >&2
  echo "hint: run cmake -S $ROOT -B $ROOT/build && cmake --build $ROOT/build -j" >&2
  exit 2
fi

if [[ ! -f "$SRC" ]]; then
  echo "error: source fixture not found: $SRC" >&2
  exit 2
fi

if [[ -n "$PERF_CURRENT" || -n "$PERF_BASELINE" ]]; then
  if [[ -z "$PERF_CURRENT" || -z "$PERF_BASELINE" ]]; then
    echo "error: --perf-current and --perf-baseline must be provided together" >&2
    exit 2
  fi
  if [[ ! -f "$PERF_CURRENT" ]]; then
    echo "error: perf current JSON not found: $PERF_CURRENT" >&2
    exit 2
  fi
  if [[ ! -f "$PERF_BASELINE" ]]; then
    echo "error: perf baseline JSON not found: $PERF_BASELINE" >&2
    exit 2
  fi
fi

mkdir -p "$OUT_DIR"

json_escape() {
  printf "%s" "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

extract_metric() {
  local key="$1"
  local line="$2"
  local value
  value="$(sed -nE "s/.*\\\"${key}\\\":([0-9]+).*/\\1/p" <<<"$line" | head -n1)"
  if [[ -z "$value" ]]; then
    echo "error: metric '$key' not found in cache report line" >&2
    echo "line: $line" >&2
    exit 1
  fi
  printf "%s" "$value"
}

run_mode() {
  local mode="$1"
  local log_path="$OUT_DIR/cross_stage_reuse_${mode}.log"
  local artifact_path="$OUT_DIR/cross_stage_reuse_${mode}.out"
  "$BIN" run "$SRC" \
    --run-gate none \
    --profile fast \
    --cross-stage-reuse "$mode" \
    --cache-report on \
    --cache-report-format json \
    -o "$artifact_path" >"$log_path" 2>&1

  local report_line
  report_line="$(grep -Eo '\{"cache_report":\{.*\}\}' "$log_path" | tail -n1 || true)"
  if [[ -z "$report_line" ]]; then
    echo "error: missing cache_report JSON in $log_path" >&2
    cat "$log_path" >&2
    exit 1
  fi

  local candidates reused saved_ms
  candidates="$(extract_metric "cross_stage_candidates" "$report_line")"
  reused="$(extract_metric "cross_stage_reused" "$report_line")"
  saved_ms="$(extract_metric "cross_stage_saved_ms_estimate" "$report_line")"

  if [[ "$mode" == "off" ]]; then
    OFF_CANDIDATES="$candidates"
    OFF_REUSED="$reused"
    OFF_SAVED_MS="$saved_ms"
  else
    SAFE_CANDIDATES="$candidates"
    SAFE_REUSED="$reused"
    SAFE_SAVED_MS="$saved_ms"
  fi
}

OFF_CANDIDATES=0
OFF_REUSED=0
OFF_SAVED_MS=0
SAFE_CANDIDATES=0
SAFE_REUSED=0
SAFE_SAVED_MS=0

run_mode off
run_mode safe

CSV_PATH="$OUT_DIR/cross_stage_reuse_baseline.csv"
{
  echo "mode,cross_stage_candidates,cross_stage_reused,cross_stage_saved_ms_estimate"
  echo "off,$OFF_CANDIDATES,$OFF_REUSED,$OFF_SAVED_MS"
  echo "safe,$SAFE_CANDIDATES,$SAFE_REUSED,$SAFE_SAVED_MS"
} >"$CSV_PATH"

BIN_ESCAPED="$(json_escape "$BIN")"
SRC_ESCAPED="$(json_escape "$SRC")"
OUT_DIR_ESCAPED="$(json_escape "$OUT_DIR")"
GENERATED_AT="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"

cat >"$JSON_PATH" <<EOF
{
  "generated_at_utc": "$GENERATED_AT",
  "binary": "$BIN_ESCAPED",
  "source": "$SRC_ESCAPED",
  "out_dir": "$OUT_DIR_ESCAPED",
  "results": [
    {
      "mode": "off",
      "cross_stage_candidates": $OFF_CANDIDATES,
      "cross_stage_reused": $OFF_REUSED,
      "cross_stage_saved_ms_estimate": $OFF_SAVED_MS
    },
    {
      "mode": "safe",
      "cross_stage_candidates": $SAFE_CANDIDATES,
      "cross_stage_reused": $SAFE_REUSED,
      "cross_stage_saved_ms_estimate": $SAFE_SAVED_MS
    }
  ],
  "delta": {
    "cross_stage_reused_gain": $((SAFE_REUSED - OFF_REUSED)),
    "cross_stage_saved_ms_gain": $((SAFE_SAVED_MS - OFF_SAVED_MS))
  }
}
EOF

echo "baseline-csv: $CSV_PATH"
cat "$CSV_PATH"
echo "baseline-json: $JSON_PATH"
cat "$JSON_PATH"

if [[ -n "$PERF_CURRENT" && -n "$PERF_BASELINE" ]]; then
  python3 "$ROOT/scripts/perf_baseline_diff.py" \
    --current "$PERF_CURRENT" \
    --baseline "$PERF_BASELINE" \
    --out-json "$PERF_DIFF_JSON" \
    --out-md "$PERF_DIFF_MD" \
    --fail-on-regression "$PERF_FAIL_ON_REGRESSION" \
    --max-total-regression-pct "$PERF_MAX_TOTAL_REGRESSION_PCT" \
    --max-suite-regression-pct "$PERF_MAX_SUITE_REGRESSION_PCT" \
    --max-case-regression-ms "$PERF_MAX_CASE_REGRESSION_MS" \
    --min-case-duration-ms "$PERF_MIN_CASE_DURATION_MS" \
    --max-cross-stage-reused-drop "$PERF_MAX_CROSS_STAGE_REUSED_DROP" \
    --max-cross-stage-saved-ms-drop "$PERF_MAX_CROSS_STAGE_SAVED_MS_DROP" \
    --max-disk-hit-drop "$PERF_MAX_DISK_HIT_DROP" \
    --max-disk-miss-increase "$PERF_MAX_DISK_MISS_INCREASE" \
    --max-disk-eviction-increase "$PERF_MAX_DISK_EVICTION_INCREASE" \
    --max-grouping-total-ms-increase "$PERF_MAX_GROUPING_TOTAL_MS_INCREASE" \
    --max-grouping-budget-fallback-increase "$PERF_MAX_GROUPING_BUDGET_FALLBACK_INCREASE"
  rc=$?
  echo "perf-diff-json: $PERF_DIFF_JSON"
  echo "perf-diff-md: $PERF_DIFF_MD"
  exit "$rc"
fi
