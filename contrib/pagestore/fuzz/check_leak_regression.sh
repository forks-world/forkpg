#!/usr/bin/env bash
# check_leak_regression.sh -- regression check for "pagestore: free the
# memtable when ps_core_open() fails after allocating it": replay the
# checked-in reproducer (known-crashes/manifest/
# leak_memtable_on_post_alloc_open_failure.txt) through the instrumented
# libFuzzer binary with LeakSanitizer on and *no* suppression, and require a
# clean (leak-free) exit.
#
# Before that fix, this same command reliably reported the documented
# ps_memtable_create/ps_pgcache_init leak (see the .txt file for the full
# writeup and confirmed stack) -- this script is what turns "we believe it's
# fixed" into something CI/a human can just run.  Requires fuzz/build.sh to
# have been run first (same prerequisite as run_fuzz.sh).
#
# Usage: check_leak_regression.sh [-b build-dir]
set -euo pipefail

FUZZ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$FUZZ_DIR/build/pagestore_format_fuzz"
REPRO="$FUZZ_DIR/known-crashes/manifest/leak_memtable_on_post_alloc_open_failure.txt.bin"

while getopts "b:" opt; do
  case "$opt" in
    b) BIN="$OPTARG/pagestore_format_fuzz" ;;
    *) echo "usage: $0 [-b build-dir]" >&2; exit 2 ;;
  esac
done

if [[ ! -x "$BIN" ]]; then
  echo "check_leak_regression.sh: $BIN not found; run fuzz/build.sh first" >&2
  exit 1
fi
if [[ ! -f "$REPRO" ]]; then
  echo "check_leak_regression.sh: reproducer missing: $REPRO" >&2
  exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# No lsan_suppressions.txt here on purpose: the whole point is to prove the
# allocation is freed, not to hide it again. -runs=5000 (not 1) matches the
# original finding's "leaks accumulate across many retried opens in one
# long-lived process" shape -- a fix that only frees on the *first* failed
# open but not a later one would still pass at -runs=1.
if PS_FUZZ_TARGET=manifest PS_FUZZ_CRC_FIXUP=0 \
    ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:halt_on_error=1:allocator_may_return_null=1" \
    UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
    TMPDIR="$work" \
    "$BIN" -runs=5000 "$REPRO" > "$work/run.log" 2>&1; then
  echo "check_leak_regression.sh: OK -- no leak over 5000 replays of $REPRO"
  exit 0
else
  echo "check_leak_regression.sh: FAILED -- leak (or other abort) reproduced:" >&2
  cat "$work/run.log" >&2
  exit 1
fi
