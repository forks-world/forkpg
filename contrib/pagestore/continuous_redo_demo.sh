#!/usr/bin/env bash
#
# continuous_redo_demo.sh -- keep a WAL-only writer and redo materializer
# online together, and prove that the materializer follows newly archived WAL.
#
# The writer keeps relation pages local and ships only WAL.  A distinct
# PostgreSQL instance starts from a physical base backup, stays in archive
# recovery with route_all enabled, and materializes replayed relation pages into
# pagestore.  Two updates are archived after the materializer is already live;
# observing both on the recovery instance proves this is continuous following,
# not the one-shot writer-to-recovery handoff covered by wal_only_redo_demo.sh.
#
# Self-asserting; needs a full PostgreSQL build.  Pass the meson build dir as $1.
#
set -uo pipefail

BUILD=${1:?usage: continuous_redo_demo.sh <meson-build-dir>}
BUILD=$(CDPATH= cd -- "$BUILD" && pwd) || {
	echo "FAIL - cannot resolve build directory: $BUILD"
	exit 1
}
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
archive_defer_supported=0
if grep -q '^#define HAVE_ARCHIVE_MODULE_DEFER ' \
		"$SCRIPT_DIR/../../src/include/archive/archive_module.h"; then
	archive_defer_supported=1
fi
expedite_checkpoint_supported=0
if grep -q '^#define HAVE_EXPEDITE_RUNNING_CHECKPOINT ' \
		"$SCRIPT_DIR/../../src/include/postmaster/bgwriter.h"; then
	expedite_checkpoint_supported=1
fi
restartpoint_replay_supported=0
if grep -q '^#define HAVE_RECOVERY_RESTARTPOINT_REPLAYED_HOOK ' \
		"$SCRIPT_DIR/../../src/include/access/xlog.h"; then
	restartpoint_replay_supported=1
fi
PGCTL=$(find "$BUILD/tmp_install" -path '*/bin/pg_ctl' -type f 2>/dev/null | head -1)
[ -z "$PGCTL" ] && { echo "FAIL - no tmp_install"; exit 1; }
BIN=$(dirname "$PGCTL")
ROOT=$(dirname "$BIN")
export LD_LIBRARY_PATH="$ROOT/lib:$ROOT/lib64"
DAEMON="$BUILD/contrib/pagestore/pagestore_daemon"
IMPORT="$BUILD/contrib/pagestore/pagestore_import"
INSPECT="$BUILD/contrib/pagestore/pagestore_inspect"
WALRESTORE="$BUILD/contrib/pagestore/pagestore_walrestore"

TMPROOT=$(mktemp -d)
WRITER="$TMPROOT/writer"
REDO="$TMPROOT/redo"
STORE="$TMPROOT/store"
SHM=/pscontinuous_$$
WPORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')
RPORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')
WP="$BIN/psql -h 127.0.0.1 -p $WPORT -U postgres -tA"
RP="$BIN/psql -h 127.0.0.1 -p $RPORT -U postgres -tA"

cleanup()
{
	"$BIN/pg_ctl" -D "$REDO" -m immediate -w stop >/dev/null 2>&1 || true
	"$BIN/pg_ctl" -D "$WRITER" -m immediate -w stop >/dev/null 2>&1 || true
	if [ -n "${DPID:-}" ]; then
		kill "$DPID" 2>/dev/null || true
		wait "$DPID" 2>/dev/null || true
	fi
	rm -rf "$TMPROOT"
	rm -f "/dev/shm$SHM"
}
trap cleanup EXIT

fail()
{
	echo "FAIL - $1"
	echo "writer log:"
	tail -20 "$WRITER/writer.log" 2>/dev/null || true
	echo "redo log:"
	tail -40 "$REDO/redo.log" 2>/dev/null || true
	echo "daemon/import log:"
	tail -40 "$TMPROOT/daemon.log" "$TMPROOT/import.log" 2>/dev/null || true
	exit 1
}

# Finish the WAL segment containing the writer's current durable position and
# wait until archive_library has appended it to pagestore.
archive_current_wal()
{
	local switch_lsn target

	switch_lsn=$($WP -c "SELECT pg_switch_wal();") || return 1
	target=$($WP -c "SELECT pg_walfile_name('$switch_lsn'::pg_lsn - 1);") || return 1
	for _ in $(seq 1 300); do
		[ -f "$WRITER/pg_wal/archive_status/$target.done" ] && return 0
		sleep 0.1
	done
	echo "archive did not mark $target done" >&2
	$WP -c "SELECT archived_count, failed_count, last_archived_wal,
		last_failed_wal FROM pg_stat_archiver;" >&2 || true
	find "$WRITER/pg_wal/archive_status" -maxdepth 1 -type f -print >&2
	return 1
}

wait_redo_value()
{
	local expected=$1 value=

	for _ in $(seq 1 400); do
		value=$($RP -c "SELECT v FROM continuous_redo WHERE id=1;" 2>/dev/null || true)
		[ "$value" = "$expected" ] && return 0
		sleep 0.1
	done
	echo "redo returned '${value:-}', expected '$expected'" >&2
	return 1
}

wait_replay_lsn()
{
	local target=$1 reached=

	for _ in $(seq 1 400); do
		reached=$($RP -c "SELECT pg_last_wal_replay_lsn() >= '$target'::pg_lsn;" \
			2>/dev/null || true)
		[ "$reached" = "t" ] && return 0
		sleep 0.1
	done
	echo "redo did not replay checkpoint boundary $target" >&2
	return 1
}

wait_materializer_caught_up()
{
	local lag=

	for _ in $(seq 1 400); do
		lag=$($RP -c "SELECT pagestore_materializer_lag_bytes();" 2>/dev/null || true)
		[ "$lag" = "0" ] && return 0
		sleep 0.1
	done
	echo "materializer lag remained '${lag:-unknown}' bytes" >&2
	return 1
}

mkdir -p "$STORE"
"$BIN/initdb" -D "$WRITER" -U postgres -A trust >/dev/null 2>&1 ||
	fail "initdb failed"
"$DAEMON" --shm "$SHM" --store "$STORE" >"$TMPROOT/daemon.log" 2>&1 &
DPID=$!
daemon_ready=0
for _ in $(seq 1 200); do
	kill -0 "$DPID" 2>/dev/null || fail "pagestore daemon exited during startup"
	if "$INSPECT" --shm "$SHM" health >/dev/null 2>&1; then
		daemon_ready=1
		break
	fi
	sleep 0.05
done
[ "$daemon_ready" -eq 1 ] || fail "pagestore daemon did not become ready"
"$IMPORT" --shm "$SHM" --pgdata "$WRITER" >"$TMPROOT/import.log" 2>&1 ||
	fail "could not import the base cluster"

cat >> "$WRITER/postgresql.conf" <<EOF
shared_preload_libraries = 'pagestore'
pagestore.backend = 'localsvc'
pagestore.localsvc_shm = '$SHM'
pagestore.route_all = off
# Indexing can compact historical index views before recovery starts.  The
# materializer must still register its raw-WAL owner and advance restartpoints.
pagestore.auto_wal_index = on
io_method = sync
recovery_prefetch = try
archive_mode = on
archive_library = 'pagestore'
listen_addresses = '127.0.0.1'
port = $WPORT
EOF

"$BIN/pg_ctl" -D "$WRITER" -l "$WRITER/writer.log" -w start >/dev/null 2>&1 ||
	fail "WAL-only writer did not start"
$WP -c "SELECT 1;" >/dev/null 2>&1 || fail "WAL-only writer is not accepting connections"

# Take the redo worker's physical base while the writer remains online.  WAL is
# deliberately excluded: recovery must fetch it through pagestore_walrestore.
"$BIN/pg_basebackup" -h 127.0.0.1 -p "$WPORT" -U postgres -D "$REDO" \
	--wal-method=none --checkpoint=fast >/dev/null 2>&1 ||
	fail "could not create the redo worker base backup"
archive_current_wal || fail "base-backup WAL did not reach pagestore"

cat >> "$REDO/postgresql.conf" <<EOF
pagestore.route_all = on
pagestore.materializer = on
pagestore.auto_wal_index = off
pagestore.retention_owner_id = '1'
pagestore.retention_owner_generation = '1'
archive_mode = off
listen_addresses = '127.0.0.1'
port = $RPORT
hot_standby = on
restore_command = '$WALRESTORE --shm $SHM --timeline 0 --incarnation 1 --segsize 16777216 %f %p'
EOF
# Standby mode keeps retrying restore_command when it reaches the current end of
# the archive; recovery.signal alone would promote as soon as WAL is exhausted.
touch "$REDO/standby.signal"
find "$REDO/pg_wal" -maxdepth 1 -type f -name '0000000*' -delete

"$BIN/pg_ctl" -D "$REDO" -l "$REDO/redo.log" -w start >/dev/null 2>&1 ||
	fail "continuous redo worker did not start"
$RP -c "SELECT pg_is_in_recovery();" 2>/dev/null | grep -qx t ||
	fail "redo worker left archive recovery"

# Both changes happen after the redo worker is live.  Completing each segment
# lets archive recovery consume it without stopping either process.
$WP -c "CREATE EXTENSION pagestore;
	CREATE TABLE continuous_redo(id int primary key, v text);
	INSERT INTO continuous_redo VALUES (1, 'first');" >/dev/null ||
	fail "could not create the writer test relation"
# The recovery-start hook is before the first redo record and raises FATAL if
# this durable SET fails.  Reaching hot-standby service above therefore proves
# generation 1 was registered without relying on reserved generation zero.
echo "ok   - materializer retention owner is registered before serving redo"
if $WP -c "SELECT pagestore_materializer_lag_bytes();" \
		>"$TMPROOT/writer-lag.out" 2>"$TMPROOT/writer-lag.err"; then
	fail "materializer lag API accepted a non-recovery writer"
fi
grep -q "materializer monitoring requires recovery mode" "$TMPROOT/writer-lag.err" ||
	fail "writer lag rejection did not explain the recovery requirement"
echo "ok   - materializer lag API rejects a non-recovery writer"
relfile=$($WP -c "SELECT pg_relation_filepath('continuous_redo');")
[ -f "$WRITER/$relfile" ] || fail "writer did not keep the relation page local"
archive_current_wal || fail "first update WAL did not reach pagestore"
wait_redo_value first || fail "redo worker did not materialize the first value"
echo "ok   - live redo worker materialized the first archived change"

if grep -q '^#define HAVE_RECOVERY_ARCHIVE_READY_HOOK ' \
		"$SCRIPT_DIR/../../src/include/access/xlogrecovery.h"; then
	# A fresh, deliberately long wait leaves enough room to distinguish the
	# readiness probe from an ordinary retry, even on a loaded runner.
	echo "wal_retrieve_retry_interval = '10s'" >> "$REDO/postgresql.conf"
	"$BIN/pg_ctl" -D "$REDO" reload >/dev/null ||
		fail "could not set the materializer's WAL retry interval"
	for _ in $(seq 1 100); do
		[ "$($RP -c 'SHOW wal_retrieve_retry_interval;')" = "10s" ] && break
		sleep 0.1
	done
	[ "$($RP -c 'SHOW wal_retrieve_retry_interval;')" = "10s" ] ||
		fail "materializer did not reload the WAL retry interval"
	wait_count=$(grep -c "waiting for WAL to become available" "$REDO/redo.log")
	for _ in $(seq 1 150); do
		current_wait_count=$(grep -c "waiting for WAL to become available" "$REDO/redo.log")
		[ "$current_wait_count" -gt "$wait_count" ] && break
		sleep 0.1
	done
	[ "$current_wait_count" -gt "$wait_count" ] ||
		fail "materializer did not start a fresh WAL archive wait"
	sleep 2
	[ "$(grep -c "waiting for WAL to become available" "$REDO/redo.log")" -eq "$current_wait_count" ] ||
		fail "archive readiness probes retried restore without new WAL"
	retrieve_started=$SECONDS
	retrieve_lsn=$($WP -c "SELECT pg_create_restore_point('archive_ready_probe');") ||
		fail "could not create the archive readiness restore point"
	archive_current_wal || fail "readiness probe WAL was not archived"
	wait_replay_lsn "$retrieve_lsn" || fail "materializer did not replay the readiness probe WAL"
	retrieve_elapsed=$((SECONDS - retrieve_started))
	[ "$retrieve_elapsed" -lt 6 ] ||
		fail "materializer took ${retrieve_elapsed}s to replay WAL during an archive wait"
	echo "ok   - materializer replayed newly archived WAL in ${retrieve_elapsed}s during a 10s retry wait"
	echo "wal_retrieve_retry_interval = '5s'" >> "$REDO/postgresql.conf"
	"$BIN/pg_ctl" -D "$REDO" reload >/dev/null ||
		fail "could not restore the default WAL retry interval"
fi

checkpoint_lsn=$($WP -c "UPDATE continuous_redo SET v='second' WHERE id=1;
	CHECKPOINT;
	SELECT pg_current_wal_lsn();" | tail -1) ||
	fail "could not update the writer test relation"
archive_current_wal || fail "second update WAL did not reach pagestore"
wait_redo_value second || fail "redo worker did not follow the second value"
echo "ok   - redo worker continuously followed later archived WAL"
wait_replay_lsn "$checkpoint_lsn" ||
	fail "redo worker did not replay the newer checkpoint"

# A replay LSN alone is not a materialization boundary: the changed page may
# still live only in this standby's dirty buffer cache.  Shutdown recovery
# performs a restartpoint, whose post-flush hook publishes the durable store
# watermark.  Restarting also clears the cache, so the next read proves the
# page itself is store-visible.
"$BIN/pg_ctl" -D "$REDO" -m fast -w restart >/dev/null 2>&1 ||
	fail "redo worker restartpoint/restart failed"
$RP -c "SELECT pg_is_in_recovery();" 2>/dev/null | grep -qx t ||
	fail "redo worker left recovery after restartpoint"
wait_redo_value second || fail "second value was not store-visible after restart"
wait_materializer_caught_up || fail "materializer lag did not return to zero"
shipped_lsn=$($RP -c "SELECT pagestore_shipped_wal_lsn();")
materialized_lsn=$($RP -c "SELECT pagestore_materialized_wal_lsn();")
echo "ok   - materializer reports zero lag (shipped $shipped_lsn, materialized $materialized_lsn)"
writer_status=$($WP -F '|' -c "SELECT * FROM pagestore_materializer_status();") ||
	fail "writer could not inspect store-observed materializer status"
IFS='|' read -r status_shipped status_materialized status_lag status_release <<EOF
$writer_status
EOF
[ "$status_shipped" = "$shipped_lsn" ] &&
	[ "$status_materialized" = "$materialized_lsn" ] &&
	[ "$status_lag" = 0 ] && [ -z "$status_release" ] ||
	fail "writer materializer status did not report caught-up, unlatch state: $writer_status"
echo "ok   - writer observes caught-up materializer state without claiming the worker role"
$RP -v ON_ERROR_STOP=1 -c "CHECKPOINT;" >/dev/null ||
	fail "online restartpoint request failed on a caught-up materializer"
[ "$($RP -c "SELECT pagestore_materialized_wal_lsn();")" = "$materialized_lsn" ] ||
	fail "online restartpoint moved the watermark without a new replayed checkpoint"
echo "ok   - online restartpoint leaves the caught-up watermark in place"

# Stop recovery, establish a checkpoint, then advance several segments without
# another checkpoint.  The aligned limiter must finish usable input segments
# but eventually pause.  A later writer checkpoint moves the guaranteed-safe
# archive boundary far enough for recovery to reach a real restartpoint.
archive_failures=$($WP -c "SELECT failed_count FROM pg_stat_archiver;") ||
	fail "could not read the archiver's initial failure count"
$WP -c "ALTER SYSTEM SET pagestore.materializer_max_lag_mb = '1';" >/dev/null ||
	fail "could not configure materializer backpressure"
writer_started=$($WP -c "SELECT pg_postmaster_start_time();") || fail "writer start time unavailable"
$WP -c "SELECT pg_reload_conf();" >/dev/null || fail "writer backpressure reload failed"
for _ in $(seq 1 100); do
	[ "$( $WP -c "SHOW pagestore.materializer_max_lag_mb;" )" = "1" ] && break
	sleep 0.1
done
[ "$( $WP -c "SHOW pagestore.materializer_max_lag_mb;" )" = "1" ] ||
	fail "materializer backpressure did not reload"
"$BIN/pg_ctl" -D "$REDO" -m fast -w stop >/dev/null 2>&1 ||
	fail "could not stop the materializer for backpressure testing"
blocked_target=
$WP -c "CREATE TABLE materializer_backpressure_test(i int);" >/dev/null ||
	fail "could not create the materializer backpressure test relation"
$WP -c "INSERT INTO materializer_backpressure_test VALUES (1); CHECKPOINT;" \
	>/dev/null || fail "could not establish the backlog checkpoint"
$WP -c "SELECT pg_switch_wal();" >/dev/null ||
	fail "could not switch the checkpoint-bearing WAL segment"
for i in 2 3 4 5 6; do
	$WP -c "INSERT INTO materializer_backpressure_test VALUES ($i);" >/dev/null ||
		fail "could not generate materializer backlog segment $i"
	switch_lsn=$($WP -c "SELECT pg_switch_wal();") ||
		fail "could not switch materializer backlog segment $i"
	blocked_target=$($WP -c "SELECT pg_walfile_name('$switch_lsn'::pg_lsn - 1);") ||
		fail "could not identify backlog WAL segment $i"
done
backpressure_seen=0
for _ in $(seq 1 300); do
	if grep -q "archive paused: materializer" "$WRITER/writer.log"; then
		backpressure_seen=1
		break
	fi
	sleep 0.1
done
[ "$backpressure_seen" -eq 1 ] ||
	fail "archiver did not pause for a stalled materializer"
[ -f "$WRITER/pg_wal/archive_status/$blocked_target.ready" ] ||
	fail "materializer backpressure left no WAL segment pending"
writer_status=$($WP -F '|' -c "SELECT * FROM pagestore_materializer_status();") ||
	fail "writer could not inspect stalled materializer status"
IFS='|' read -r status_shipped status_materialized status_lag status_release <<EOF
$writer_status
EOF
[ -n "$status_materialized" ] && [ "${status_lag:-0}" -gt 0 ] &&
	[ -n "$status_release" ] ||
	fail "writer materializer status did not expose stalled release state: $writer_status"
echo "ok   - stalled materializer applies bounded WAL archive backpressure"

# Publish a newer completed checkpoint while archiving is paused.  The limiter
# keeps the earlier release checkpoint latched until the materializer advances,
# then admits complete segments through this next checkpoint.
if [ "$expedite_checkpoint_supported" -eq 1 ]; then
	$WP -v ON_ERROR_STOP=1 -c "ALTER SYSTEM SET checkpoint_timeout = '30s';" >/dev/null ||
		fail "could not set the writer checkpoint timeout"
	$WP -c "SELECT pg_reload_conf();" >/dev/null || fail "writer checkpoint reload failed"
	for _ in $(seq 1 100); do
		[ "$($WP -c 'SHOW checkpoint_timeout;')" = "30s" ] && break
		sleep 0.1
	done
	[ "$($WP -c 'SHOW checkpoint_timeout;')" = "30s" ] ||
		fail "writer did not reload the checkpoint timeout"
	$WP -v ON_ERROR_STOP=1 -c "CREATE TABLE paced_checkpoint_payload AS
		SELECT g, repeat(md5(g::text), 32) AS payload
		FROM generate_series(1, 12000) AS g;" >/dev/null ||
		fail "could not prepare dirty buffers for the paced checkpoint"
	paced_checkpoint_starts=$(grep -c "LOG:  checkpoint starting:" "$WRITER/writer.log")
	paced_started=$SECONDS
	$WP -v ON_ERROR_STOP=1 -c "CHECKPOINT (MODE SPREAD);" >/dev/null ||
		fail "could not run the paced writer checkpoint"
	paced_elapsed=$((SECONDS - paced_started))
	[ "$paced_elapsed" -lt 12 ] ||
		fail "paced writer checkpoint took ${paced_elapsed}s under archive backpressure"
	grep -q "pagestore expedited a writer checkpoint for WAL archiving" "$WRITER/writer.log" ||
		fail "archive backpressure did not expedite the paced writer checkpoint"
	$WP -v ON_ERROR_STOP=1 -c "ALTER SYSTEM RESET checkpoint_timeout;" >/dev/null ||
		fail "could not reset the writer checkpoint timeout"
	echo "ok   - archive backpressure expedited the paced writer checkpoint in ${paced_elapsed}s"
else
	$WP -c "CHECKPOINT;" >/dev/null ||
		fail "could not publish the backlog release checkpoint"
fi
release_lsn=$($WP -c "SELECT pg_switch_wal();") ||
	fail "could not switch the backlog release checkpoint"
release_target=$($WP -c "SELECT pg_walfile_name('$release_lsn'::pg_lsn - 1);") ||
	fail "could not identify the backlog release segment"
# Wake an archiver that may have entered its longer retry backoff after three
# failures; production would make the same retry without this test nudge.
$WP -c "SELECT pg_reload_conf();" >/dev/null ||
	fail "could not wake the WAL archiver after checkpoint progress"
sleep 2
if [ "$expedite_checkpoint_supported" -eq 1 ]; then
	[ "$(grep -c "LOG:  checkpoint starting:" "$WRITER/writer.log")" -eq "$((paced_checkpoint_starts + 1))" ] ||
		fail "archive backpressure queued an extra writer checkpoint"
fi
[ -f "$WRITER/pg_wal/archive_status/$release_target.ready" ] ||
	fail "a later writer checkpoint moved the latched bound before materialization advanced"
# An operator can temporarily expand the boundary when a replayed checkpoint
# is unusable, then restore it without restarting the writer or materializer.
$WP -c "ALTER SYSTEM SET pagestore.materializer_max_lag_mb = '256';" >/dev/null ||
	fail "could not expand archive backpressure"
$WP -c "SELECT pg_reload_conf();" >/dev/null || fail "could not reload expanded archive backpressure"
for _ in $(seq 1 600); do
	[ -f "$WRITER/pg_wal/archive_status/$release_target.done" ] && break
	sleep 0.1
done
[ -f "$WRITER/pg_wal/archive_status/$release_target.done" ] ||
	fail "archiver did not reload the expanded materializer limit"
[ "$( $WP -c "SELECT pg_postmaster_start_time();" )" = "$writer_started" ] ||
	fail "writer restarted while expanding archive backpressure"
$WP -c "ALTER SYSTEM SET pagestore.materializer_max_lag_mb = '1';" >/dev/null ||
	fail "could not restore archive backpressure"
$WP -c "SELECT pg_reload_conf();" >/dev/null || fail "could not reload original archive backpressure"
for _ in $(seq 1 100); do
	[ "$( $WP -c "SHOW pagestore.materializer_max_lag_mb;" )" = "1" ] && break
	sleep 0.1
done
[ "$( $WP -c "SHOW pagestore.materializer_max_lag_mb;" )" = "1" ] ||
	fail "materializer backpressure did not reload its original limit"
for i in 1 2; do
	$WP -c "SELECT pg_create_restore_point('lag_reload_$i');" >/dev/null || fail "could not generate WAL after restoring the limit"
	release_lsn=$($WP -c "SELECT pg_switch_wal();") || fail "could not switch WAL after restoring the limit"
	release_target=$($WP -c "SELECT pg_walfile_name('$release_lsn'::pg_lsn - 1);") || fail "could not identify WAL after restoring the limit"
done
$WP -c "CHECKPOINT;" >/dev/null || fail "could not checkpoint after restoring the limit"
final_checkpoint_lsn=$($WP -c "SELECT pg_current_wal_lsn();") || fail "could not locate the final checkpoint"
release_lsn=$($WP -c "SELECT pg_switch_wal();") || fail "could not switch the final checkpoint WAL"
release_target=$($WP -c "SELECT pg_walfile_name('$release_lsn'::pg_lsn - 1);") || fail "could not identify the final checkpoint WAL"
for _ in $(seq 1 300); do
	[ -f "$WRITER/pg_wal/archive_status/$release_target.ready" ] && break
	sleep 0.1
done
# Let the archiver finish any failure retries queued by the last WAL switch.
sleep 5
[ -f "$WRITER/pg_wal/archive_status/$release_target.ready" ] ||
	fail "restored materializer limit did not pause the later WAL segment"
echo "ok   - materializer archive limit expands and tightens without writer restart"
if [ "$archive_defer_supported" -eq 1 ]; then
	for _ in $(seq 1 50); do
		archive_wait_event=$($WP -c "SELECT wait_event FROM pg_stat_activity WHERE backend_type = 'archiver';")
		[ "$archive_wait_event" = "PageStoreArchiveMaterializer" ] && break
		sleep 0.1
	done
	[ "$archive_wait_event" = "PageStoreArchiveMaterializer" ] ||
		fail "writer did not expose the materializer archive wait"
fi
archive_wait_start=$SECONDS
"$BIN/pg_ctl" -D "$REDO" -l "$REDO/redo.log" -w start >/dev/null 2>&1 ||
	fail "materializer did not restart after backpressure"
redo_started=$($RP -c "SELECT pg_postmaster_start_time();") ||
	fail "materializer start time unavailable"
release_replayed=0
for _ in $(seq 1 400); do
	release_value=$($RP -c "SELECT max(i) FROM materializer_backpressure_test;" \
		2>/dev/null || true)
	if [ "${release_value:-0}" -ge 2 ] 2>/dev/null; then
		release_replayed=1
		break
	fi
	sleep 0.1
done
[ "$release_replayed" -eq 1 ] ||
	fail "materializer did not reach the latched release checkpoint"
if [ "$restartpoint_replay_supported" -eq 0 ]; then
	$RP -v ON_ERROR_STOP=1 -c "CHECKPOINT;" >/dev/null ||
		fail "materializer did not complete the online restartpoint request"
fi
# Keep the writer idle.  Publishing materializer progress must be sufficient
# for the archiver to retry within a small fraction of its normal 60s timeout.
archive_wait_attempts=100
if [ "$archive_defer_supported" -eq 0 ]; then
	# Release branches without the core deferral API still need a wakeup.
	$WP -c "SELECT pg_reload_conf();" >/dev/null ||
		fail "could not wake the WAL archiver after materializer progress"
	archive_wait_attempts=600
fi
archive_drained=0
for _ in $(seq 1 "$archive_wait_attempts"); do
	if [ -f "$WRITER/pg_wal/archive_status/$release_target.done" ]; then
		archive_drained=1
		break
	fi
	sleep 0.1
done
[ "$archive_drained" -eq 1 ] ||
	fail "archive backlog did not drain after online restartpoint"
[ "$restartpoint_replay_supported" -eq 0 ] ||
	grep -q "pagestore requested a fast restartpoint for WAL archiving" "$REDO/redo.log" ||
	fail "materializer did not request a restartpoint while replaying the release checkpoint"
[ "$($RP -c "SELECT pg_postmaster_start_time();")" = "$redo_started" ] ||
	fail "materializer restarted while releasing archive backpressure"
if [ "$archive_defer_supported" -eq 1 ]; then
	[ "$($WP -c "SELECT failed_count FROM pg_stat_archiver;")" = "$archive_failures" ] ||
		fail "archiver counted materializer backpressure as a failure"
	echo "ok   - idle writer archived pending WAL in $((SECONDS - archive_wait_start))s with an online restartpoint"
fi
wait_replay_lsn "$final_checkpoint_lsn" || fail "materializer did not replay the final checkpoint"
backlog_replayed=0
for _ in $(seq 1 400); do
	backlog_value=$($RP -c "SELECT max(i) FROM materializer_backpressure_test;" \
		2>/dev/null || true)
	if [ "$backlog_value" = "6" ]; then
		backlog_replayed=1
		break
	fi
	sleep 0.1
done
[ "$backlog_replayed" -eq 1 ] ||
	fail "materializer did not replay the released archive backlog"
"$BIN/pg_ctl" -D "$REDO" -m fast -w restart >/dev/null 2>&1 ||
	fail "materializer restartpoint after backlog drain failed"
wait_materializer_caught_up ||
	fail "materializer did not catch up after releasing archive backpressure"
echo "ok   - materializer progress releases WAL archive backpressure"

# Recovery alone is not a materializer identity.  Restart with the explicit
# role disabled and prove that a stale marker cannot make this worker pass the
# supervision contract.
"$BIN/pg_ctl" -D "$REDO" -m fast -w stop >/dev/null 2>&1 ||
	fail "could not stop materializer before stale-owner test"
# The backlog restartpoint advanced generation 1's pin and may have reclaimed
# the earlier materialized_lsn. Preserve its current restart-REDO boundary:
# the flushed replay watermark can be later than the WAL needed to restart.
takeover_lsn=$($WP -c "SELECT pagestore_retention_owner_lsn(0,2,1,1);") ||
	fail "could not inspect the stopped materializer's retention pin"
[ -n "$takeover_lsn" ] || fail "stopped materializer has no generation-1 pin"
takeover_status=$($WP -c "SELECT pagestore_retention_set(0,2,1,2,2,'$takeover_lsn');")
[ "$takeover_status" = "0" ] || fail "could not publish replacement owner generation"
# Hot-standby startup can briefly make pg_ctl report success before recovery
# reaches the pagestore owner check.  Observe the durable fencing result instead
# of treating that transient readiness as a successful stale takeover.
"$BIN/pg_ctl" -D "$REDO" -l "$REDO/redo.log" -w start >/dev/null 2>&1 || true
stale_fenced=0
for _ in $(seq 1 100); do
	if grep -q "retention generation is stale" "$REDO/redo.log"; then
		stale_fenced=1
		break
	fi
	sleep 0.1
done
[ "$stale_fenced" -eq 1 ] ||
	fail "stale materializer startup did not report owner fencing"
for _ in $(seq 1 100); do
	if ! "$BIN/pg_ctl" -D "$REDO" status >/dev/null 2>&1; then
		break
	fi
	sleep 0.1
done
if "$BIN/pg_ctl" -D "$REDO" status >/dev/null 2>&1; then
	fail "stale materializer generation remained running after controller takeover"
fi
echo "ok   - replacement owner generation fences stale materializer startup"

# Fencing the old worker is insufficient: its replacement must be able to
# register at pg_control's restart REDO and serve the recovered relation.
echo "pagestore.retention_owner_generation = '2'" >> "$REDO/postgresql.conf"
"$BIN/pg_ctl" -D "$REDO" -l "$REDO/redo.log" -w start >/dev/null 2>&1 ||
	fail "replacement materializer generation could not restart"
wait_redo_value second || fail "replacement generation could not serve recovered pages"
[ "$($RP -c "SELECT max(i) FROM materializer_backpressure_test;")" = "6" ] ||
	fail "replacement generation lost the recovered archive backlog"
echo "ok   - replacement owner generation restarts with its retained redo history"
"$BIN/pg_ctl" -D "$REDO" -m fast -w stop >/dev/null 2>&1 ||
	fail "could not stop replacement materializer before role-disabled restart"
echo "pagestore.materializer = off" >> "$REDO/postgresql.conf"
"$BIN/pg_ctl" -D "$REDO" -l "$REDO/redo.log" -w start >/dev/null 2>&1 ||
	fail "redo worker role-disabled restart failed"
if $RP -c "SELECT pagestore_materializer_lag_bytes();" \
		>"$TMPROOT/role-lag.out" 2>"$TMPROOT/role-lag.err"; then
	fail "materializer monitoring accepted an undeclared recovery worker"
fi
grep -q "requires pagestore.materializer = on" "$TMPROOT/role-lag.err" ||
	fail "undeclared worker rejection did not explain the materializer role"
echo "ok   - monitoring rejects a recovery worker without the materializer role"

[ ! -f "$REDO/$relfile" ] ||
	fail "redo worker created a local heap instead of materializing into pagestore"
echo "ok   - recovery relation has no local heap file (pagestore is authoritative)"
if grep -Eq 'materializer retention advance (was rejected|failed)' "$REDO/redo.log"; then
	fail "materializer could not advance raw-WAL retention while indexing was enabled"
fi
echo "ok   - indexed WAL does not reject materializer retention advancement"

# The materializer watermark is fixed with its role disabled. Hold WAL at
# that boundary until crash recovery completes a new writer checkpoint.
checkpoint_before_crash=$($WP -c "SELECT checkpoint_lsn FROM pg_control_checkpoint();") ||
	fail "could not read the writer checkpoint before the crash"
crash_boundary_ready=$($WP -c "SELECT coalesce(materialized_wal_lsn >=
	'$checkpoint_before_crash'::pg_lsn AND release_checkpoint_lsn IS NULL, false)
	FROM pagestore_materializer_status();")
[ "$crash_boundary_ready" = "t" ] ||
	fail "crash checkpoint test did not start beyond the last writer checkpoint"
for i in 1 2 3 4; do
	$WP -c "SELECT pg_create_restore_point('crash_checkpoint_$i');" >/dev/null ||
		fail "could not produce WAL for the crash checkpoint test"
	crash_switch=$($WP -c "SELECT pg_switch_wal();") || fail "could not seal crash test WAL"
	crash_target=$($WP -c "SELECT pg_walfile_name('$crash_switch'::pg_lsn - 1);") ||
		fail "could not identify crash test WAL"
done
sleep 2
[ -f "$WRITER/pg_wal/archive_status/$crash_target.ready" ] ||
	fail "crash test WAL was not held by the materializer bound"
$WP -v ON_ERROR_STOP=1 -c "INSERT INTO materializer_backpressure_test VALUES (7);" >/dev/null ||
	fail "could not leave committed WAL for crash recovery"
crash_started=$SECONDS
"$BIN/pg_ctl" -D "$WRITER" -m immediate -w stop >/dev/null 2>&1 ||
	fail "could not simulate a writer crash"
"$BIN/pg_ctl" -D "$WRITER" -l "$WRITER/writer.log" -w start >/dev/null 2>&1 ||
	fail "writer did not recover from the simulated crash"
crash_checkpoint=$($WP -c "SELECT checkpoint_lsn FROM pg_control_checkpoint();") ||
	fail "could not read the completed crash recovery checkpoint"
[ "$crash_checkpoint" != "$checkpoint_before_crash" ] ||
	fail "writer did not complete a new crash recovery checkpoint"
for _ in $(seq 1 100); do
	[ -f "$WRITER/pg_wal/archive_status/$crash_target.done" ] && break
	sleep 0.1
done
[ -f "$WRITER/pg_wal/archive_status/$crash_target.done" ] ||
	fail "crash recovery checkpoint did not release pending WAL within 10s"
release_checkpoint=$($WP -c "SELECT release_checkpoint_lsn FROM pagestore_materializer_status();")
[ "$release_checkpoint" = "$crash_checkpoint" ] ||
	fail "archive did not use the crash recovery checkpoint as its release checkpoint"
[ "$($WP -c "SELECT checkpoint_lsn FROM pg_control_checkpoint();")" = "$crash_checkpoint" ] ||
	fail "archive waited for a subsequent writer checkpoint"
echo "ok   - crash recovery checkpoint released pending WAL in $((SECONDS - crash_started))s"
echo "continuous redo demo: PASS"
