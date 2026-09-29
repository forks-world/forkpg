#!/usr/bin/env bash
# run_fuzz.sh -- run one or more persisted-format fuzz targets for a fixed
# wall-clock duration, in parallel (one process per target -- the targets
# do enough real filesystem/syscall work per iteration, see fuzz_common.c,
# that -fork=N workers on a single target does not help much; spreading
# across the 21 targets is where the 32 cores earn their keep).
#
# Usage:
#   run_fuzz.sh [-d seconds] [-o out-dir] [target ...]
# With no targets listed, runs every target in fuzz_common.c's table.
# Requires fuzz/build.sh to have been run first.
#
# Each target's working corpus starts as a copy of fuzz/corpus/<target>
# (never mutated in place) and any crash/leak/timeout artifact lands in
# <out-dir>/<target>/crashes/, named by libFuzzer's own content hash.
#
# Round 2 (coordinator review) changes from round 1:
#  - Leak detection is back ON (detect_leaks=1) with
#    lsan_suppressions.txt silencing only the one already-documented,
#    already-triaged leak (known-crashes/manifest/
#    leak_memtable_on_post_alloc_open_failure.txt) so a *new* leak still
#    stops the run and gets caught, instead of every run stopping on run #1.
#  - PS_FUZZ_CRC_FIXUP=1 by default (half of iterations get a structure-
#    aware checksum fixup after mutation -- see fuzz_crc_fixup.c -- the
#    other half stay pure mutation); pass -f 0 to disable for an apples-
#    to-apples round-1-style comparison run.
set -euo pipefail

FUZZ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$FUZZ_DIR/build/pagestore_format_fuzz"
DURATION=720
OUT_DIR="$FUZZ_DIR/run"
CRC_FIXUP="${PS_FUZZ_CRC_FIXUP:-1}"

while getopts "d:o:f:" opt; do
  case "$opt" in
    d) DURATION="$OPTARG" ;;
    o) OUT_DIR="$OPTARG" ;;
    f) CRC_FIXUP="$OPTARG" ;;
    *) echo "usage: $0 [-d seconds] [-o out-dir] [-f 0|1] [target ...]" >&2; exit 2 ;;
  esac
done
shift $((OPTIND - 1))

# Codex finding on PR #303 (c959796): DURATION is forwarded straight to
# -max_total_time, whose own -help=1 applies it "if positive" -- 0, a
# negative value, or anything non-numeric silently drops the campaign's
# total-time bound instead of failing, and the script then waits on
# workers that never stop on their own. Reject before launching any worker.
if ! [[ "$DURATION" =~ ^[0-9]+$ ]] || [[ "$DURATION" -eq 0 ]]; then
  echo "run_fuzz.sh: -d duration must be a positive integer (got '$DURATION')" >&2
  exit 2
fi

if [[ ! -x "$BIN" ]]; then
  echo "run_fuzz.sh: $BIN not found; run fuzz/build.sh first" >&2
  exit 1
fi

ALL_TARGETS=(
  manifest forkmeta forkmeta_snapshot_manifest forkmeta_snapshot_checkpoint
  forkmeta_snapshot_tail image_layer page_frontier page_segment
  retention_meta retention_state store_config timelines wal_log
  wal_store_identity wal_segment walidx_frontier walidx_log_epoch
  walidx_log_legacy walidx_watermark walidx_snapshot_manifest
  walidx_snapshot_shard
)
TARGETS=("$@")
if [[ ${#TARGETS[@]} -eq 0 ]]; then
  TARGETS=("${ALL_TARGETS[@]}")
fi

# Reject anything not in ALL_TARGETS *before* the target loop below ever
# builds a path from it: "$OUT_DIR/$tgt" is `rm -rf`'d to start each
# target's working corpus fresh, and an unvalidated tgt containing "../" (or
# an absolute path) lets that `rm -rf` walk outside $OUT_DIR -- e.g. a
# tgt of "../corpus" with the default $OUT_DIR resolves to fuzz/corpus
# itself, deleting the checked-in corpus before the following `cp -r` from
# that same (now-gone) directory fails.
for tgt in "${TARGETS[@]}"; do
  valid=0
  for allowed in "${ALL_TARGETS[@]}"; do
    if [[ "$tgt" == "$allowed" ]]; then
      valid=1
      break
    fi
  done
  if [[ $valid -eq 0 ]]; then
    echo "run_fuzz.sh: unknown target '$tgt' (not in ALL_TARGETS)" >&2
    exit 2
  fi
done
for ((i = 0; i < ${#TARGETS[@]}; i++)); do
  for ((j = i + 1; j < ${#TARGETS[@]}; j++)); do
    if [[ "${TARGETS[i]}" == "${TARGETS[j]}" ]]; then
      echo "run_fuzz.sh: duplicate target '${TARGETS[i]}'" >&2
      exit 2
    fi
  done
done

# Per-target -max_len: roughly 2x the largest checked-in seed, since a
# mutated file should be allowed to grow past what any fixture happened to
# record. Every case below was checked against `du -sb` on fuzz/corpus/<target>
# (round-4 coordinator review: manifest, wal_segment, wal_log, and
# walidx_frontier were below their own 2x-largest-seed line and are fixed
# here; everything else already had headroom, including the ones that fall
# through to the 16384 default).
max_len_for() {
  case "$1" in
    image_layer) echo 786432 ;;      # largest seed 388816
    wal_segment) echo 2359296 ;;     # largest seed 1048640 (2x = 2097280)
    page_segment) echo 131072 ;;     # largest seed 57744
    page_frontier) echo 131072 ;;    # largest seed 49168
    wal_log) echo 262144 ;;          # largest seed 65552 (2x = 131104)
    walidx_frontier) echo 131072 ;;  # largest seed 32784 (2x = 65568)
    manifest) echo 49152 ;;          # largest seed 21852 (2x = 43704)
    *) echo 16384 ;;
  esac
}

# Per-input -timeout: libFuzzer's own -max_total_time is only checked
# between executions, so a single hung input (stuck in open/maintenance/
# close) can run past it -- its documented default (-timeout, 1200s) is far
# longer than any campaign duration this script runs with. Cap each input
# well under the campaign's own duration so a hang still produces a timeout
# artifact and moves on instead of silently eating the whole run.
PER_INPUT_TIMEOUT=$((DURATION / 4))
if [[ $PER_INPUT_TIMEOUT -gt 60 ]]; then
  PER_INPUT_TIMEOUT=60
elif [[ $PER_INPUT_TIMEOUT -lt 1 ]]; then
  PER_INPUT_TIMEOUT=1
fi

mkdir -p "$OUT_DIR"
pids=()
fail=0

cleanup_workers() {
  local i pid attempt found
  local -a running=()

  # jobs -pr excludes workers that already exited, so a completed PID can
  # never be signalled again after the OS has had a chance to reuse it.
  mapfile -t running < <(jobs -pr; jobs -ps)
  for pid in "${running[@]}"; do
    for i in "${!pids[@]}"; do
      if [[ "$pid" == "${pids[i]}" ]]; then
        kill -TERM "$pid" 2>/dev/null || true
        break
      fi
    done
  done

  # A worker can be stuck in one input or ignore TERM. Give it a short grace
  # period, then force-stop only the still-running PIDs owned by this script.
  for ((attempt = 0; attempt < 10; attempt++)); do
    mapfile -t running < <(jobs -pr; jobs -ps)
    found=0
    for pid in "${running[@]}"; do
      for i in "${!pids[@]}"; do
        if [[ "$pid" == "${pids[i]}" ]]; then
          found=1
          break
        fi
      done
    done
    [[ $found -eq 1 ]] || break
    sleep 0.1
  done
  mapfile -t running < <(jobs -pr; jobs -ps)
  for pid in "${running[@]}"; do
    for i in "${!pids[@]}"; do
      if [[ "$pid" == "${pids[i]}" ]]; then
        kill -KILL "$pid" 2>/dev/null || true
        break
      fi
    done
  done
  for pid in "${pids[@]}"; do
    wait "$pid" 2>/dev/null || true
  done
}

on_exit() {
  local status=$?

  trap - EXIT INT TERM
  cleanup_workers
  exit "$status"
}

trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

for tgt in "${TARGETS[@]}"; do
  work="$OUT_DIR/$tgt"
  rm -rf "$work"
  mkdir -p "$work/crashes"
  cp -r "$FUZZ_DIR/corpus/$tgt" "$work/corpus"
  (
    cd "$work"
    exec env \
      PS_FUZZ_TARGET="$tgt" \
      PS_FUZZ_CRC_FIXUP="$CRC_FIXUP" \
      TMPDIR="${TMPDIR:-/tmp}" \
      ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:halt_on_error=1:allocator_may_return_null=1" \
      UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
      LSAN_OPTIONS="suppressions=$FUZZ_DIR/lsan_suppressions.txt" \
      "$BIN" -max_total_time="$DURATION" -max_len="$(max_len_for "$tgt")" \
      -timeout="$PER_INPUT_TIMEOUT" \
      -rss_limit_mb=4096 -artifact_prefix=crashes/ \
      corpus/ > run.log 2>&1
  ) &
  pids+=($!)
  echo "started $tgt (pid $!) -> $work"
done

echo "waiting for ${#pids[@]} target(s), duration ${DURATION}s each..."
for i in "${!pids[@]}"; do
  pid="${pids[i]}"
  if wait "$pid"; then
    status=0
  else
    status=$?
    fail=1
  fi
  echo "exit_code=$status" >> "$OUT_DIR/${TARGETS[i]}/run.log"
done
echo "all targets finished (some background jobs may report nonzero exit on found crashes; see run.log per target)"
# Propagate a worker's nonzero exit (ASan/UBSan/libFuzzer abort on a crash
# or timeout) as this script's own exit status -- an automated campaign
# must be able to tell a clean run from one that found something.
exit "$fail"
