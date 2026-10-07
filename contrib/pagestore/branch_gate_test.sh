#!/usr/bin/env bash
# Real PostgreSQL checks for G3 SQL entry gates and explicit test opt-in.
set -euo pipefail
BUILD=${1:?usage: branch_gate_test.sh <meson-build-dir>}
PGCTL=$(find "$BUILD/tmp_install" -path '*/bin/pg_ctl' -type f | head -1)
[ -n "$PGCTL" ] || { echo "FAIL - no tmp_install"; exit 1; }
BIN=$(dirname "$PGCTL")
ROOT=$(dirname "$BIN")
export LD_LIBRARY_PATH="$ROOT/lib:$ROOT/lib64"
WORK=$(mktemp -d /tmp/ps-g3-gate.XXXXXX)
DATA="$WORK/data"
SOCKET="$WORK/socket"
TARGET="$WORK/prepared"
mkdir -m 700 "$SOCKET" "$TARGET"
SHM="/psg3_${WORK##*/}"
DPID=""
cleanup() {
    "$BIN/pg_ctl" -D "$DATA" -m immediate -w stop >/dev/null 2>&1 || true
    if [ -n "$DPID" ]; then
        kill "$DPID" 2>/dev/null || true
        wait "$DPID" 2>/dev/null || true
    fi
    python3 - "$BUILD/../contrib/pagestore/harness" "$SHM" <<'PYCLEAN' || true
import sys
sys.path.insert(0, sys.argv[1])
from pagestore_harness import remove_shm
remove_shm(sys.argv[2])
PYCLEAN
    rm -rf "$WORK"
}
trap cleanup EXIT
fail() {
    echo "FAIL - $*"
    tail -30 "$DATA/server.log" "$WORK/daemon.log" 2>/dev/null || true
    exit 1
}
"$BUILD/contrib/pagestore/pagestore_daemon" --shm "$SHM" --store "$WORK/store" > "$WORK/daemon.log" 2>&1 &
DPID=$!
ready=false
for _ in $(seq 1 100); do
    if "$BUILD/contrib/pagestore/pagestore_inspect" --shm "$SHM" health >/dev/null 2>&1; then
        ready=true
        break
    fi
    kill -0 "$DPID" 2>/dev/null || fail "daemon exited"
    sleep 0.05
done
[ "$ready" = true ] || fail "daemon did not become ready"
"$BIN/initdb" -D "$DATA" -U postgres -A trust >/dev/null 2>&1
cat >> "$DATA/postgresql.conf" <<EOF
shared_preload_libraries = 'pagestore'
pagestore.backend = 'localsvc'
pagestore.localsvc_shm = '$SHM'
listen_addresses = ''
unix_socket_directories = '$SOCKET'
port = 5439
dynamic_shared_memory_type = mmap
EOF
"$BIN/pg_ctl" -D "$DATA" -l "$DATA/server.log" -w start >/dev/null 2>&1 || fail "could not start PostgreSQL"
P=("$BIN/psql" -X -h "$SOCKET" -p 5439 -U postgres -d postgres -qAt -v ON_ERROR_STOP=1 -v VERBOSITY=verbose)
"${P[@]}" -c "CREATE EXTENSION pagestore;
 CREATE FUNCTION pagestore_create_branch(int,int,pg_lsn) RETURNS void
 AS 'pagestore','pagestore_create_branch' LANGUAGE C STRICT;
 CREATE FUNCTION pagestore_prepare_branch(text,int,int,pg_lsn,pg_lsn,xid,xid,xid,xid,xid,xid,bigint,bigint) RETURNS bigint
 AS 'pagestore','pagestore_prepare_branch' LANGUAGE C STRICT;
 CREATE ROLE g3_unprivileged;" >/dev/null
printf '%s\n' 'unchanged-readiness' > "$TARGET/pagestore_branch.bootstrap"
reject() {
    local sql=$1 expected=$2 output
    if output=$("${P[@]}" -c "$sql" 2>&1); then
        fail "unexpected success: $sql"
    fi
    [[ "$output" == *"$expected"* ]] || fail "unexpected rejection: $output"
}
[ "$("${P[@]}" -c "SHOW pagestore.allow_unsafe_branch_cut;")" = off ] || fail "unsafe cut is not off by default"
# Valid create identities must fail before any IPC mutation. The invalid
# legacy prepare proves refusal precedes argument validation and file writes.
calls=(
 "SELECT pagestore_create_branch(85,0,pg_current_wal_lsn());"
 "SELECT pagestore_create_branch_with_incarnation(86,0,1,pg_current_wal_lsn());"
 "SELECT pagestore_prepare_branch('$TARGET',0,0,'0/0','0/0','3','3','3','3','3','3',0,0);"
)
for sql in "${calls[@]}"; do
    reject "$sql" "55000: pagestore branch creation outside the controller flow is disabled"
    reject "SET pagestore.allow_unsafe_branch_cut=on; RESET pagestore.allow_unsafe_branch_cut; $sql" \
        "55000: pagestore branch creation outside the controller flow is disabled"
    reject "SET pagestore.allow_unsafe_branch_cut=on; SET ROLE g3_unprivileged; $sql" \
        "42501: must be superuser to create an unsafe pagestore branch"
done
[ "$("${P[@]}" -c "SELECT count(*) FROM (VALUES (85),(86)) ids(id), LATERAL pagestore_timeline_state(id) s WHERE s.state IS NOT NULL;")" = 0 ] || fail "rejected calls created a timeline"
for sql in "${calls[@]}"; do
    output=$("${P[@]}" -c "SET pagestore.allow_unsafe_branch_cut=on; $sql" 2>&1) || true
    [[ "$output" == *"WARNING:"*"UNSAFE pagestore branch cut through"* ]] || fail "unsafe attempt did not warn: $output"
    [[ "$output" != *"outside the controller flow is disabled"* ]] || fail "explicit opt-in did not pass the gate"
done
[ "$("${P[@]}" -c "SELECT count(*) FROM (VALUES (85),(86)) ids(id), LATERAL pagestore_timeline_state(id) s WHERE s.state='live';")" = 2 ] || fail "explicit opt-in did not create test timelines"
reject "SET pagestore.allow_unsafe_branch_cut=on; SELECT * FROM pagestore_branch_window_open();" \
    "55000: restricted writer changed since the branch window was opened"
reject "SET ROLE g3_unprivileged; SET pagestore.allow_unsafe_branch_cut=on;" \
    "42501: permission denied to set parameter"
reject "ALTER SYSTEM SET pagestore.allow_unsafe_branch_cut=on;" "cannot be changed"
[ "$(cat "$TARGET/pagestore_branch.bootstrap")" = unchanged-readiness ] || fail "gate modified readiness"
[ "$(find "$TARGET" -type f | wc -l | tr -d ' ')" = 1 ] || fail "gate created artifacts"
echo "pagestore G3 branch entry gates: PASS"
