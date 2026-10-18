#!/usr/bin/env bash
#
# branch_boot_test.sh -- end-to-end branch compute boot through the prepared
# branch manifest/install flow, under full routing.
#
# This is the successor to the old single-compute timeline-switch demo.  That
# shortcut edited pagestore.timeline in the same PGDATA, a path that branch
# startup now fails closed on.  Here the
# branch is a real second compute booted the supported way:
#
#   - a parent runs with route_all=on over an imported store (timeline 0),
#     shipping WAL and SLRU snapshots;
#   - the test explicitly permits an unsafe expert cut; production uses the
#     serialized controller instead;
#   - pagestore_prepare_branch() seeds the branch SLRUs as of the fork LSN,
#     forks the store timeline, and publishes the durable manifest;
#   - the branch datadir is a copy of the cleanly-stopped parent, with the
#     prepared artifacts installed by pagestore_install_prepared_branch();
#   - the branch boots with pagestore.timeline=1, passes the fail-closed
#     manifest validation, sees exactly the fork-point data, and writes
#     forward on its own timeline isolated from the parent.
#
# Self-asserting: prints "ok"/"FAIL" lines and exits non-zero on any failure.
# Needs a full PostgreSQL build; pass the meson build directory as $1.
#
#   contrib/pagestore/branch_boot_test.sh /path/to/build
#   contrib/pagestore/branch_boot_test.sh /path/to/pg18-build --member-wrap=zero
#   contrib/pagestore/branch_boot_test.sh /path/to/pg18-build --member-wrap=cross
#
set -uo pipefail

BUILD=${1:?usage: branch_boot_test.sh <meson-build-dir> [--member-wrap=zero|cross]}
MEMBER_WRAP=${2:-}
case "$MEMBER_WRAP" in
	''|--member-wrap=zero|--member-wrap=cross) ;;
	*) echo "FAIL - unknown branch boot option: $MEMBER_WRAP"; exit 1 ;;
esac
PGCTL=$(find "$BUILD/tmp_install" -path '*/bin/pg_ctl' -type f 2>/dev/null | head -1)
if [ -z "$PGCTL" ]; then
	echo "FAIL - no tmp_install found under $BUILD/tmp_install (run: meson test -C $BUILD --suite setup)"
	exit 1
fi
BIN=$(dirname "$PGCTL")
ROOT=$(dirname "$BIN")
export LD_LIBRARY_PATH="$ROOT/lib:$ROOT/lib64"
if [ -n "$MEMBER_WRAP" ] && ! "$BIN/postgres" --version | grep -q 'PostgreSQL) 18\.'; then
	echo "SKIP - 32-bit multixact member wrap requires PostgreSQL 18"
	exit 0
fi
DAEMON="$BUILD/contrib/pagestore/pagestore_daemon"
IMPORT="$BUILD/contrib/pagestore/pagestore_import"

SOCKROOT=$(mktemp -d /tmp/psbboot-sock.XXXXXX)
new_sockdir() {
	mktemp -d "$SOCKROOT/$1.XXXXXX"
}

DATA=$(mktemp -d)/pgdata
STORE=$(mktemp -d)/store
SCRATCH=$(mktemp -d)/walredo
SOCKDIR=$(new_sockdir main)
BRANCHSOCK=$(new_sockdir branch)
SHM=/psbboot_$$

PORT=5432
P="$BIN/psql -h $SOCKDIR -p $PORT -U postgres -tA"
PB="$BIN/psql -h $BRANCHSOCK -p $PORT -U postgres -tA"
fail=0

assert() {  # $1=actual $2=expected $3=message
	if [ "$1" = "$2" ]; then
		echo "ok   - $3"
	else
		echo "FAIL - $3 (got '$1', want '$2')"
		fail=1
	fi
}

cleanup() {
	local result=$?
	if [ "$result" != 0 ] || [ "$fail" != 0 ]; then
		mkdir -p "$BUILD/pagestore-branch-boot-logs"
		for logfile in "$DATA/server.log" "$DATA/daemon.log" "${BRANCHDATA:-/nonexistent}/server.log"; do
			if [ -f "$logfile" ]; then
				echo "---- $logfile"
				tail -50 "$logfile"
				cp "$logfile" "$BUILD/pagestore-branch-boot-logs/$(basename "$(dirname "$logfile")")-$(basename "$logfile")"
			fi
		done
	fi
	"$BIN/pg_ctl" -D "$DATA" -m immediate -w stop >/dev/null 2>&1 || true
	[ -n "${BRANCHDATA:-}" ] && "$BIN/pg_ctl" -D "$BRANCHDATA" -m immediate -w stop >/dev/null 2>&1 || true
	if [ -n "${DPID:-}" ]; then
		kill "$DPID" 2>/dev/null || true
		wait "$DPID" 2>/dev/null || true
	fi
	rm -rf "$(dirname "$DATA")" "$(dirname "$STORE")" "$(dirname "$SCRATCH")" \
		"$BRANCHSOCK" "${BRANCHDATA:+$(dirname "$BRANCHDATA")}" "${PREP:+$(dirname "$PREP")}" \
		"${SCRATCH2:+$(dirname "$SCRATCH2")}" "$SOCKROOT"
	rm -f "/dev/shm$SHM"
}
trap cleanup EXIT

"$BIN/initdb" -D "$DATA" -U postgres -A trust >/dev/null 2>&1
if [ -n "$MEMBER_WRAP" ]; then
	# Only this disposable, never-started initdb cluster is reset. Prime one
	# two-member multixact, then let a second reach zero or span the wrap.
	wrap_start=4294967292
	[ "$MEMBER_WRAP" = --member-wrap=cross ] && wrap_start=4294967293
	"$BIN/pg_resetwal" -O "$wrap_start" "$DATA" >/dev/null || exit 1
	# TrimMultiXact reads the current members page on clean startup. The
	# reset changes counters only, so initialize its unused final segment.
	wrap_bs=$(LC_ALL=C "$BIN/pg_controldata" "$DATA" | awk '/Database block size:/ {print $NF}')
	python3 - "$DATA" "$wrap_start" "$wrap_bs" <<'PYMEMBERPAGE' || exit 1
import pathlib, sys
root, offset, block_size = pathlib.Path(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
per_page = (block_size // 20) * 4
page = offset // per_page
with (root / "pg_multixact/members" / f"{page // 32:04X}").open("wb") as stream:
    stream.truncate((page % 32 + 1) * block_size)
PYMEMBERPAGE
fi
"$BIN/initdb" -D "$SCRATCH" -U postgres -A trust >/dev/null 2>&1
"$DAEMON" --shm "$SHM" --store "$STORE" >"$DATA/daemon.log" 2>&1 &
DPID=$!
# Wait for the daemon's READY handshake instead of a fixed delay: an import
# that races a slow start fails, and the compute then boots over an empty
# store ("role "postgres" does not exist").
INSPECT="$BUILD/contrib/pagestore/pagestore_inspect"
daemon_ready=no
for _ in $(seq 1 600); do
	kill -0 "$DPID" 2>/dev/null || break
	if "$INSPECT" --shm "$SHM" health >/dev/null 2>&1; then
		daemon_ready=yes
		break
	fi
	sleep 0.05
done
[ "$daemon_ready" = yes ] || { echo "FAIL - pagestore daemon did not become ready"; exit 1; }

# The whole database -- catalogs included -- must live on the store for a
# branch compute to be viable, so import the initdb'd cluster first.
"$IMPORT" --shm "$SHM" --pgdata "$DATA" >/dev/null 2>&1 ||
	{ echo "FAIL - could not import the base cluster"; exit 1; }

cat >> "$DATA/postgresql.conf" <<EOF
shared_preload_libraries = 'pagestore'
pagestore.backend = 'localsvc'
pagestore.localsvc_shm = '$SHM'
pagestore.route_all = on
pagestore.walredo_datadir = '$SCRATCH'
pagestore.timeline = 0
io_method = sync
archive_mode = on
archive_library = 'pagestore'
listen_addresses = ''
unix_socket_directories = '$SOCKDIR'
port = $PORT
EOF
if [ -n "$MEMBER_WRAP" ]; then
	echo 'autovacuum = off' >> "$DATA/postgresql.conf"
fi
"$BIN/pg_ctl" -D "$DATA" -l "$DATA/server.log" -w start >/dev/null 2>&1 || exit 1

$P -v ON_ERROR_STOP=1 -c "CREATE EXTENSION pagestore;
       CREATE FUNCTION pagestore_ship_slru_snapshot(text, pg_lsn) RETURNS bigint
        AS 'pagestore','pagestore_ship_slru_snapshot' LANGUAGE C STRICT;
       CREATE FUNCTION pagestore_prepare_branch(text, int, int, pg_lsn, pg_lsn, xid, xid, xid, xid, xid, xid, bigint, bigint) RETURNS bigint
        AS 'pagestore','pagestore_prepare_branch' LANGUAGE C STRICT;
       CREATE FUNCTION pagestore_install_prepared_branch(text, text, int, int, pg_lsn) RETURNS void
        AS 'pagestore','pagestore_install_prepared_branch' LANGUAGE C STRICT;
       CREATE TABLE demo(id int primary key, note text);" >/dev/null || exit 1
$P -c "CHECKPOINT;" >/dev/null

# Two ordinary SQL lockers create actual WAL records and tuple multixact
# headers. Terminating the first session releases its lock without a fixed
# sleep race; member identities remain independently inspectable afterwards.
create_wrap_multixact() {
	local sock=$1 row=$2
	local app="psmember-$row-$$" pid='' mid
	local query="$BIN/psql -X -h $sock -p $PORT -U postgres -tA -v ON_ERROR_STOP=1"
	PGAPPNAME="$app" "$BIN/psql" -X -h "$sock" -p "$PORT" -U postgres -tA \
		-v ON_ERROR_STOP=1 -c "BEGIN; SELECT id FROM wrap_mx WHERE id=$row FOR SHARE; SELECT pg_sleep(120);" \
		>/dev/null 2>&1 &
	local locker=$!
	for ((attempt = 0; attempt < 200; attempt++)); do
		pid=$($query -c "SELECT pid FROM pg_stat_activity WHERE application_name='$app' AND wait_event='PgSleep';") || break
		[ -n "$pid" ] && break
		kill -0 "$locker" 2>/dev/null || break
		sleep 0.05
	done
	if [ -z "$pid" ]; then
		kill "$locker" 2>/dev/null || true
		wait "$locker" 2>/dev/null || true
		echo "FAIL - multixact locker did not acquire its row lock" >&2
		return 1
	fi
	$query -c "BEGIN; SELECT id FROM wrap_mx WHERE id=$row FOR SHARE; COMMIT;" >/dev/null || return 1
	mid=$($query -c "SELECT xmax FROM wrap_mx WHERE id=$row;") || return 1
	$query -c "SELECT pg_terminate_backend($pid);" >/dev/null || return 1
	wait "$locker" 2>/dev/null || true
	[ "$($query -c "SELECT count(*) FROM pg_get_multixact_members('$mid');")" = 2 ] || return 1
	echo "$mid"
}

oldest_multi=1 next_multi=1 oldest_member=0 next_member=0
if [ -n "$MEMBER_WRAP" ]; then
	$P -v ON_ERROR_STOP=1 -c "CREATE TABLE wrap_mx(id int PRIMARY KEY); INSERT INTO wrap_mx VALUES (1),(2),(3);" >/dev/null || exit 1
	oldest_multi=$(create_wrap_multixact "$SOCKDIR" 1) || exit 1
	oldest_member=$wrap_start
	$P -c "CHECKPOINT;" >/dev/null || exit 1
fi

# base cutoff C: ship the SLRU snapshots the prepare replay starts from
bc=$($P -c "SELECT pg_current_wal_lsn();")
$P -c "SELECT pagestore_ship_slru_snapshot('pg_xact', '$bc');" >/dev/null
$P -c "SELECT pagestore_ship_slru_snapshot('pg_multixact/offsets', '$bc');" >/dev/null
$P -c "SELECT pagestore_ship_slru_snapshot('pg_multixact/members', '$bc');" >/dev/null

if [ -n "$MEMBER_WRAP" ]; then
	wrapped_multi=$(create_wrap_multixact "$SOCKDIR" 2) || exit 1
	wrap_members=$($P -c "SELECT string_agg(xid::text || ':' || mode, ',' ORDER BY xid::text) FROM pg_get_multixact_members('$wrapped_multi');")
fi
$P -c "INSERT INTO demo VALUES (1, 'before_fork');" >/dev/null   # commits in (C, L]
$P -c "CHECKPOINT;" >/dev/null                                   # ship the heap page
if [ -n "$MEMBER_WRAP" ]; then
	# Also exercise checkpoint-derived horizons: this entrypoint requires a
	# real shutdown checkpoint, independently of our explicit test horizons.
	"$BIN/pg_ctl" -D "$DATA" -m fast -w stop >/dev/null 2>&1 || exit 1
	"$BIN/pg_ctl" -D "$DATA" -l "$DATA/server.log" -w start >/dev/null 2>&1 || exit 1
	wrap_redo=$($P -c "SELECT redo_lsn FROM pg_control_checkpoint();")
fi
bL=$($P -c "SELECT pg_current_wal_lsn();")                       # fork LSN L
nxid=$($P -c "SELECT pg_snapshot_xmax(pg_current_snapshot());")
if [ -n "$MEMBER_WRAP" ]; then
	next_multi=$($P -c "SELECT next_multixact_id FROM pg_control_checkpoint();")
	next_member=$($P -c "SELECT next_multi_offset FROM pg_control_checkpoint();")
	expected_next=0
	[ "$MEMBER_WRAP" = --member-wrap=cross ] && expected_next=1
	assert "$next_member" "$expected_next" "real SQL member allocation reached the expected 32-bit wrap boundary"
	[ "$fail" = 0 ] || exit 1
fi

# prepare needs the (C, L] WAL: it replays from the parent's local pg_wal
PREP=$(mktemp -d)/prep
mkdir -p "$PREP"
if [ -n "$MEMBER_WRAP" ]; then
	mkdir -p "$PREP.control"
	derived=$($P -q -v ON_ERROR_STOP=1 -c "SET pagestore.allow_unsafe_branch_cut = on;
		SELECT pagestore_prepare_branch_from_control('$PREP.control', 2, 0, '$bc', '$wrap_redo', '$bL');") || exit 1
	assert "$([ "${derived:-0}" -gt 0 ] && echo ok || echo no)" "ok" \
		"shutdown-checkpoint preparation accepts real wrapped member horizons"
	assert "$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["oldest_member"])' "$PREP.control/pagestore_branch.manifest")" "$oldest_member" \
		"checkpoint-derived preparation preserves the oldest member offset"
	assert "$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["next_member"])' "$PREP.control/pagestore_branch.manifest")" "$next_member" \
		"checkpoint-derived preparation preserves the wrapped next member offset"
fi
seeded=$($P -q -c "SET pagestore.allow_unsafe_branch_cut = on; SELECT pagestore_prepare_branch('$PREP', 1, 0, '$bc', '$bL',
	'3'::xid, '$nxid'::xid, '1'::xid, '1'::xid, '$oldest_multi'::xid, '$next_multi'::xid, $oldest_member, $next_member);")
assert "$([ "${seeded:-0}" -gt 0 ] && echo ok || echo no)" "ok" \
	"branch prepared: SLRUs seeded as-of L and store timeline forked ($seeded page(s))"

# The branch datadir is a copy of the cleanly-stopped parent taken at the fork:
# its pg_control/WAL are the parent's at L, and the SLRUs get replaced by the
# prepared artifacts below.  Copy before the parent moves past L so the branch's
# local state matches the fork point.
"$BIN/pg_ctl" -D "$DATA" -m fast -w stop >/dev/null 2>&1
BRANCHDATA=$(mktemp -d)/branch
cp -a "$DATA" "$BRANCHDATA"
"$BIN/pg_ctl" -D "$DATA" -l "$DATA/server.log" -w start >/dev/null 2>&1 || exit 1

# parent moves past the fork: the branch must never see this row
$P -c "INSERT INTO demo VALUES (2, 'after_fork'); CHECKPOINT;" >/dev/null

$P -c "SELECT pagestore_install_prepared_branch('$PREP', '$BRANCHDATA', 1, 0, '$bL');" >/dev/null
assert "$([ -f "$BRANCHDATA/pagestore_branch.manifest" ] && echo present)" "present" \
	"prepared branch artifacts installed (manifest published last)"

# the branch is a second live compute: it needs its own wal-redo scratch
# cluster (the helper is a standalone backend holding its datadir's lock
# file, so two computes sharing the parent's scratch would collide)
SCRATCH2=$(mktemp -d)/walredo2
"$BIN/initdb" -D "$SCRATCH2" -U postgres -A trust >/dev/null 2>&1
cat >> "$BRANCHDATA/postgresql.conf" <<EOF
pagestore.timeline = 1
listen_addresses = ''
unix_socket_directories = '$BRANCHSOCK'
port = $PORT
archive_mode = off
pagestore.walredo_datadir = '$SCRATCH2'
EOF
if "$BIN/pg_ctl" -D "$BRANCHDATA" -l "$BRANCHDATA/server.log" -w start >/dev/null 2>&1; then
	echo "ok   - branch compute booted through manifest validation (route_all + timeline 1)"
else
	echo "FAIL - branch compute did not boot; log tail:"
	tail -5 "$BRANCHDATA/server.log" | sed 's/^/       /'
	fail=1
fi

assert "$($PB -c "SELECT note FROM demo WHERE id=1;" 2>/dev/null)" "before_fork" \
	"branch sees the row committed before the fork LSN"
assert "$($PB -c "SELECT count(*) FROM demo WHERE id=2;" 2>/dev/null)" "0" \
	"branch does NOT see the row the parent committed after the fork"
assert "$($PB -c "SELECT pagestore_shipped_wal_lsn();" 2>/dev/null)" "$bL" \
	"empty branch WAL inherits a durable boundary at the fork LSN"

if [ -n "$MEMBER_WRAP" ]; then
	assert "$($PB -c "SELECT string_agg(xid::text || ':' || mode, ',' ORDER BY xid::text) FROM pg_get_multixact_members('$wrapped_multi');")" "$wrap_members" \
		"booted branch resolves the exact pre-fork wrapped multixact members"
	branch_multi=$(create_wrap_multixact "$BRANCHSOCK" 3) || exit 1
	assert "$($PB -c "SELECT count(*) FROM pg_get_multixact_members('$branch_multi');")" "2" \
		"branch allocates a fresh multixact after the wrap"
fi

# write forward on the branch timeline, force it to the store, evict buffers by
# restarting, and re-read: the write must have gone to timeline 1, not the parent
$PB -c "INSERT INTO demo VALUES (3, 'branch_local'); CHECKPOINT;" >/dev/null 2>&1
"$BIN/pg_ctl" -D "$BRANCHDATA" -w restart >/dev/null 2>&1
assert "$($PB -c "SELECT note FROM demo WHERE id=3;" 2>/dev/null)" "branch_local" \
	"branch write persisted on its own timeline (survives restart + buffer eviction)"
assert "$($PB -c "SELECT count(*) FROM demo WHERE id=2;" 2>/dev/null)" "0" \
	"post-fork parent row is physically absent on the branch timeline after re-read"
assert "$($P -c "SELECT count(*) FROM demo WHERE id=3;")" "0" \
	"parent is isolated from the branch's write"
assert "$($P -c "SELECT note FROM demo WHERE id=2;")" "after_fork" \
	"parent still sees its own post-fork row"

if [ -n "$MEMBER_WRAP" ]; then
	assert "$($PB -c "SELECT string_agg(xid::text || ':' || mode, ',' ORDER BY xid::text) FROM pg_get_multixact_members('$wrapped_multi');")" "$wrap_members" \
		"pre-fork wrapped multixact members survive branch writes and restart"
fi

"$BIN/pg_ctl" -D "$BRANCHDATA" -m immediate -w stop >/dev/null 2>&1

echo "----"
[ "$fail" = 0 ] && echo "branch boot test: PASS" || echo "branch boot test: FAIL"
exit $fail
