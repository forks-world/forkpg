#!/usr/bin/env bash
set -euo pipefail

FUZZ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_DIR="$(mktemp -d)"
test_pids=()

owned_jobs() {
  local pid owned
  local -a jobs_now=()

  mapfile -t jobs_now < <(jobs -pr; jobs -ps)
  for pid in "${jobs_now[@]}"; do
    for owned in "${test_pids[@]}"; do
      if [[ "$pid" == "$owned" ]]; then
        printf '%s\n' "$pid"
        break
      fi
    done
  done
}

cleanup() {
  local status=$?
  local pid attempt
  local -a active=()

  trap - EXIT INT TERM
  mapfile -t active < <(owned_jobs)
  for pid in "${active[@]}"; do
    kill -TERM "$pid" 2>/dev/null || true
  done
  for ((attempt = 0; attempt < 10; attempt++)); do
    mapfile -t active < <(owned_jobs)
    [[ ${#active[@]} -gt 0 ]] || break
    sleep 0.1
  done
  mapfile -t active < <(owned_jobs)
  for pid in "${active[@]}"; do
    kill -KILL "$pid" 2>/dev/null || true
  done
  for pid in "${test_pids[@]}"; do
    wait "$pid" 2>/dev/null || true
  done
  rm -rf "$TMP_DIR"
  exit "$status"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

cat > "$TMP_DIR/fuzz_stub.c" <<'EOF'
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void
ps_fuzz_global_init(void)
{
}

void
ps_fuzz_run_one(const char *target, const uint8_t *data, size_t size)
{
	(void) target;
	(void) data;
	(void) size;
}

/* Stands in for fuzz_common.c's real ps_fuzz_targets table with a single
 * entry ("manifest"), just enough for the driver-level "unknown target is
 * rejected before any corpus is touched" regression below. */
int
ps_fuzz_target_is_valid(const char *target)
{
	if (target == NULL || target[0] == '\0' || strcmp(target, "all") == 0 ||
		strcmp(target, "manifest") == 0)
		return 1;
	fprintf(stderr, "ps_fuzz: unknown target \"%s\"; valid targets are"
			" \"all\", \"manifest\"\n", target);
	return 0;
}
EOF
cc -Wall -Wextra -Werror -I"$FUZZ_DIR" \
  "$FUZZ_DIR/fuzz_standalone_driver.c" "$TMP_DIR/fuzz_stub.c" \
  -o "$TMP_DIR/replay_driver"

mkdir "$TMP_DIR/empty"
if "$TMP_DIR/replay_driver" manifest "$TMP_DIR/missing" \
  >"$TMP_DIR/missing.log" 2>&1; then
  echo "replay driver accepted a missing corpus directory" >&2
  exit 1
fi
grep -q 'opendir:' "$TMP_DIR/missing.log"

if [[ "$(id -u)" -ne 0 ]]; then
  mkdir "$TMP_DIR/unreadable"
  chmod 000 "$TMP_DIR/unreadable"
  if "$TMP_DIR/replay_driver" manifest "$TMP_DIR/unreadable" \
    >"$TMP_DIR/unreadable.log" 2>&1; then
    chmod 700 "$TMP_DIR/unreadable"
    echo "replay driver accepted an unreadable corpus directory" >&2
    exit 1
  fi
  chmod 700 "$TMP_DIR/unreadable"
  grep -q 'opendir:' "$TMP_DIR/unreadable.log"

  mkdir "$TMP_DIR/unreadable_file"
  printf x > "$TMP_DIR/unreadable_file/seed"
  chmod 000 "$TMP_DIR/unreadable_file/seed"
  if "$TMP_DIR/replay_driver" manifest "$TMP_DIR/unreadable_file" \
    >"$TMP_DIR/unreadable_file.log" 2>&1; then
    chmod 600 "$TMP_DIR/unreadable_file/seed"
    echo "replay driver accepted an unreadable corpus file" >&2
    exit 1
  fi
  chmod 600 "$TMP_DIR/unreadable_file/seed"
  grep -q 'fopen:' "$TMP_DIR/unreadable_file.log"
fi

if "$TMP_DIR/replay_driver" manifest "$TMP_DIR/empty" \
  >"$TMP_DIR/empty.log" 2>&1; then
  echo "replay driver accepted an empty corpus directory" >&2
  exit 1
fi
grep -q 'no corpus files:' "$TMP_DIR/empty.log"

mkdir "$TMP_DIR/stat_error"
ln -s "$TMP_DIR/no-such-file" "$TMP_DIR/stat_error/broken-link"
if "$TMP_DIR/replay_driver" manifest "$TMP_DIR/stat_error" \
  >"$TMP_DIR/stat.log" 2>&1; then
  echo "replay driver ignored a corpus entry whose stat failed" >&2
  exit 1
fi
grep -q 'stat:' "$TMP_DIR/stat.log"

mkdir "$TMP_DIR/valid"
printf x > "$TMP_DIR/valid/seed"
touch "$TMP_DIR/valid/empty-seed"
"$TMP_DIR/replay_driver" manifest "$TMP_DIR/valid" >"$TMP_DIR/valid.log" 2>&1
grep -q "replayed 2 corpus file" "$TMP_DIR/valid.log"

# Codex finding on PR #303: an unknown target must be rejected up front,
# with a nonzero exit and no corpus files "replayed", instead of silently
# reporting success without ever calling ps_fuzz_run_one().
if "$TMP_DIR/replay_driver" typo "$TMP_DIR/valid" \
  >"$TMP_DIR/bad-target.log" 2>&1; then
  echo "replay driver accepted an unknown target" >&2
  exit 1
fi
grep -q 'unknown target "typo"' "$TMP_DIR/bad-target.log"
if grep -q 'replayed' "$TMP_DIR/bad-target.log"; then
  echo "replay driver ran corpus files for an unknown target" >&2
  exit 1
fi

RUNNER_DIR="$TMP_DIR/fuzz"
mkdir -p "$RUNNER_DIR/build" "$RUNNER_DIR/corpus/manifest"
cp "$FUZZ_DIR/run_fuzz.sh" "$RUNNER_DIR/run_fuzz.sh"
cp "$FUZZ_DIR/lsan_suppressions.txt" "$RUNNER_DIR/lsan_suppressions.txt"
printf x > "$RUNNER_DIR/corpus/manifest/seed"
cat > "$RUNNER_DIR/build/pagestore_format_fuzz" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$$" > "$STUB_PID_FILE"
case "${STUB_MODE:-success}" in
  ignore-term)
    trap '' TERM
    exec sleep 60
    ;;
  fail)
    exit "${STUB_EXIT:-7}"
    ;;
esac
exit 0
EOF
chmod +x "$RUNNER_DIR/run_fuzz.sh" "$RUNNER_DIR/build/pagestore_format_fuzz"

mkdir -p "$TMP_DIR/out/manifest"
printf keep > "$TMP_DIR/out/manifest/sentinel"
if "$RUNNER_DIR/run_fuzz.sh" -o "$TMP_DIR/out" manifest manifest \
  >"$TMP_DIR/duplicate.log" 2>&1; then
  echo "runner accepted duplicate targets" >&2
  exit 1
fi
grep -q 'duplicate target' "$TMP_DIR/duplicate.log"
[[ "$(cat "$TMP_DIR/out/manifest/sentinel")" == keep ]]

# Codex finding on PR #303: -d 0, a negative value, or a non-integer must be
# rejected before any worker is launched, instead of being forwarded
# straight to libFuzzer's -max_total_time (which only bounds a campaign
# "if positive" per its own -help=1) and then running unbounded.
for bad_duration in 0 -5 abc 3.5; do
  if "$RUNNER_DIR/run_fuzz.sh" -d "$bad_duration" -o "$TMP_DIR/bad-duration-out" \
    manifest >"$TMP_DIR/bad-duration.log" 2>&1; then
    echo "runner accepted -d $bad_duration" >&2
    exit 1
  fi
  grep -q 'positive integer' "$TMP_DIR/bad-duration.log"
  if [[ -e "$TMP_DIR/bad-duration-out" ]]; then
    echo "runner touched the out-dir before validating -d $bad_duration" >&2
    exit 1
  fi
done

# Codex finding on PR #303 round 3: an out-dir that resolves to (or
# contains, or sits inside) the checked-in corpus/known-crashes trees must
# be rejected before anything is removed, instead of `rm -rf "$work"`
# deleting the checked-in corpus itself.
mkdir -p "$RUNNER_DIR/known-crashes/manifest"
printf keep > "$RUNNER_DIR/known-crashes/manifest/finding.txt"
for bad_out in \
  "$RUNNER_DIR/corpus" \
  "$RUNNER_DIR/corpus/manifest" \
  "$RUNNER_DIR" \
  "$RUNNER_DIR/known-crashes"
do
  if "$RUNNER_DIR/run_fuzz.sh" -o "$bad_out" manifest \
    >"$TMP_DIR/overlap.log" 2>&1; then
    echo "runner accepted an out-dir overlapping a checked-in tree: $bad_out" >&2
    exit 1
  fi
  grep -q 'overlaps the checked-in' "$TMP_DIR/overlap.log"
done
[[ "$(cat "$RUNNER_DIR/corpus/manifest/seed")" == x ]]
[[ "$(cat "$RUNNER_DIR/known-crashes/manifest/finding.txt")" == keep ]]

export STUB_PID_FILE="$TMP_DIR/worker.pid"
export STUB_MODE=fail
if "$RUNNER_DIR/run_fuzz.sh" -d 5 -o "$TMP_DIR/failure-out" manifest \
  >"$TMP_DIR/failure.log" 2>&1; then
  echo "runner hid a failing fuzz worker" >&2
  exit 1
fi
grep -q 'exit_code=7' "$TMP_DIR/failure-out/manifest/run.log"

export STUB_MODE=ignore-term
export STUB_PID_FILE="$TMP_DIR/term-worker.pid"
sleep 60 &
unrelated_pid=$!
test_pids+=("$unrelated_pid")
"$RUNNER_DIR/run_fuzz.sh" -d 60 -o "$TMP_DIR/term-out" manifest \
  >"$TMP_DIR/term.log" 2>&1 &
runner_pid=$!
test_pids+=("$runner_pid")
for ((attempt = 0; attempt < 100; attempt++)); do
  [[ -s "$STUB_PID_FILE" ]] && break
  sleep 0.05
done
if [[ ! -s "$STUB_PID_FILE" ]]; then
  kill -TERM "$runner_pid" 2>/dev/null || true
  wait "$runner_pid" 2>/dev/null || true
  kill "$unrelated_pid" 2>/dev/null || true
  wait "$unrelated_pid" 2>/dev/null || true
  echo "runner did not start the fuzz worker" >&2
  exit 1
fi
worker_pid="$(cat "$STUB_PID_FILE")"
kill -TERM "$runner_pid"
set +e
wait "$runner_pid"
term_status=$?
set -e
if [[ $term_status -ne 143 ]]; then
  echo "runner returned $term_status after SIGTERM; expected 143" >&2
  kill "$unrelated_pid" 2>/dev/null || true
  wait "$unrelated_pid" 2>/dev/null || true
  exit 1
fi
if kill -0 "$worker_pid" 2>/dev/null; then
  echo "runner left its fuzz worker alive after SIGTERM" >&2
  kill -KILL "$worker_pid" 2>/dev/null || true
  kill "$unrelated_pid" 2>/dev/null || true
  wait "$unrelated_pid" 2>/dev/null || true
  exit 1
fi
if ! kill -0 "$unrelated_pid" 2>/dev/null; then
  echo "runner signalled an unrelated process" >&2
  wait "$unrelated_pid" 2>/dev/null || true
  exit 1
fi
kill "$unrelated_pid"
wait "$unrelated_pid" 2>/dev/null || true

echo "runner regression checks passed"
