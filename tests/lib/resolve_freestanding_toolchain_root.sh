#!/bin/sh
set -eu

probe_capabilities=false
case "$#:$*" in
  0:) ;;
  1:--probe-capabilities) probe_capabilities=true ;;
  *)
    echo "usage: resolve_freestanding_toolchain_root.sh [--probe-capabilities]" >&2
    exit 2
    ;;
esac

if [ -n "${NEBULA_FREESTANDING_TOOLCHAIN_ROOT:-}" ]; then
  requested_root=$NEBULA_FREESTANDING_TOOLCHAIN_ROOT
elif [ "$(uname -s)" = "Darwin" ]; then
  if ! command -v brew >/dev/null 2>&1 || ! brew --prefix llvm >/dev/null 2>&1; then
    echo "freestanding contract tests require Homebrew LLVM or NEBULA_FREESTANDING_TOOLCHAIN_ROOT on macOS" >&2
    exit 1
  fi
  requested_root=$(brew --prefix llvm)
else
  compiler=$(command -v clang++ || true)
  if [ -z "$compiler" ]; then
    echo "freestanding contract tests require clang++ or NEBULA_FREESTANDING_TOOLCHAIN_ROOT" >&2
    exit 1
  fi
  requested_root=$(CDPATH= cd -- "$(dirname -- "$compiler")/.." && pwd -P)
fi

case "$requested_root" in
  /*) ;;
  *)
    echo "freestanding toolchain root must be absolute: $requested_root" >&2
    exit 1
    ;;
esac

toolchain_root=$(CDPATH= cd -- "$requested_root" && pwd -P)
compiler="$toolchain_root/bin/clang++"
if [ ! -f "$compiler" ] || [ ! -x "$compiler" ]; then
  echo "freestanding toolchain root does not contain executable bin/clang++: $toolchain_root" >&2
  exit 1
fi

# VerifiedExecutableLease keeps snapshots beside the compiler so loader-relative
# dependencies and resource lookup remain unchanged. Probe that exact capability
# rather than treating a generic -w result as sufficient (notably under macOS SIP).
probe=$(mktemp "$toolchain_root/bin/.nebula-toolchain-probe.XXXXXX") || {
  echo "freestanding toolchain bin directory cannot host an owner-private execution lease: $toolchain_root/bin" >&2
  exit 1
}
trap 'rm -f -- "$probe"' EXIT HUP INT TERM
if ! cp "$compiler" "$probe" || ! chmod 700 "$probe"; then
  echo "freestanding toolchain compiler could not be copied into an owner-private execution lease: $compiler" >&2
  exit 1
fi

if [ "$probe_capabilities" = true ]; then
  if ! version_output=$(env -i LANG=C LC_ALL=C TZ=UTC "$probe" --version 2>&1) ||
    [ -z "$(printf '%s' "$version_output" | tr -d '[:space:]')" ]; then
    echo "freestanding compiler snapshot failed its bounded-version equivalent probe: $compiler" >&2
    exit 1
  fi

  if ! target_output=$(env -i LANG=C LC_ALL=C TZ=UTC "$probe" \
    --target=x86_64-unknown-none --no-default-config -dumpmachine 2>&1) ||
    [ "$target_output" != "x86_64-unknown-none" ]; then
    echo "freestanding compiler snapshot returned an unexpected target triple: $target_output" >&2
    exit 1
  fi

  if ! env -i LANG=C LC_ALL=C TZ=UTC "$probe" \
    --target=x86_64-unknown-none --no-default-config \
    -std=c++20 -x c++ -ffreestanding -nostdinc -nostdinc++ \
    -m64 -mabi=sysv -mno-red-zone -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
    -fsyntax-only /dev/null; then
    echo "freestanding compiler snapshot failed the required ABI capability probe: $compiler" >&2
    exit 1
  fi
fi

rm -f -- "$probe"
trap - EXIT HUP INT TERM

printf '%s\n' "$toolchain_root"
