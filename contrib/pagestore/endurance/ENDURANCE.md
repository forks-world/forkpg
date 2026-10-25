# Pagestore endurance driver

`pagestore_endurance.py` runs the MVP topology under real PostgreSQL computes
and concurrent SQL for as long as it is left running, and stops with a
preserved run root at the first thing it cannot explain.  It is the first
executable slice of `RELEASE_VALIDATION.md`'s V1 (real PostgreSQL endurance),
V2 (independent SQL correctness) and V3 (faults in the composed workload).
It is a bug-finding tool, not yet release evidence: see "Not covered yet".

```sh
meson setup build -Dcassert=true -Dauto_features=disabled && ninja -C build
meson test -C build --suite setup
contrib/pagestore/endurance/pagestore_endurance.py --build build \
    --root /path/on/a/real/filesystem --forever --duration 3600
```

The driver requires Linux pidfd support and Python 3.9 or newer. Each started
postmaster is pinned by a kernel process handle after checking its executable
and data directory. If immediate `pg_ctl` shutdown fails or times out, cleanup
sends SIGKILL through that handle; PostgreSQL backends observe postmaster death.
An exited generation is never signalled through its old PID or process group.
The `mat_kill9` injection also pins descendants before killing its owned
postmaster and waits for those handles before restarting. PostgreSQL children
create separate sessions/process groups, so disappearance of the postmaster
process group does not prove its children have released shared memory.
A partially started compute is adopted only after the same ownership checks.
Cleanup waits for pinned descendants before inspecting or removing shared
resources. Failed compute cleanup is retried once; if it remains incomplete,
the failure is recorded and the daemon/shared memory are preserved.

At startup, recovery reads relation pages no earlier than the verified local
control file's checkpoint/minRecoveryPoint boundary. Before replay reaches
that boundary, selecting a historical image at the current record can reload
an image missing pre-checkpoint changes. After replay passes it, the normal
finite page horizon resumes. Relation metadata always keeps the current-record
cap, and pinned readers/hot-standby query caps are unchanged.

Put `--root` on a real filesystem.  On tmpfs every fsync is free and the run
says nothing about the store's durability ordering.

`.github/workflows/pagestore-endurance.yml` runs a bounded run on demand (any
ref, seed, duration and scale), a 15-minute one weekly, and a five-minute
one on a pull request that changes the driver, uploading the
history, the passed runs' events and metrics, and a failed run's logs,
diagnostics and controller journals as the `pagestore-endurance-<seed>`
artifact.  It keeps the driver exercised on the current tree; multi-hour
runs stay on a developer box, and neither is release evidence
(`RELEASE_VALIDATION.md`).

## Space budget

Set all three limits explicitly for a long run, for example:

```sh
python3 contrib/pagestore/endurance/pagestore_endurance.py --build build \
    --root /path/on/a/real/filesystem --duration 259200 \
    --max-run-gb 12 --max-store-gb 8 --min-free-gb 5 --sample-interval 5
```

`--max-run-gb` counts allocated space for the entire current run directory:
writer and materializer data/WAL, branch computes, prepared copies, redo scratch
space, logs and the page store. `--max-store-gb` retains the independent page
store limit. `--min-free-gb` applies before every run (including a single run)
and at every sample; it sees disk use by other runs and other applications.
Defaults are 100 GiB per run, 100 GiB per store and 50 GiB minimum free.
Choose these values for the machine and preserve headroom for cleanup/diagnostics.
A budget is a stopping threshold, not preallocated space or a filesystem quota.

Sampling starts before provisioning. Metrics include `space.run_kb`,
`space.store_total_kb` and `space.filesystem_free_bytes`, as well as the existing
`store_kb` categories. A breach records `run_size`, `store_size` or `disk_free`;
three consecutive failed measurements fail as `space_unmeasurable`. The first
failure is preserved. Normal failure cleanup keeps the evidence and stops the
owned computes. The main driver observes sampled failures at health checks and
between provisioning commands; an in-flight command can delay shutdown until
it returns or times out. Allocation can exceed the sampled threshold, so leave
ample headroom and use a filesystem quota if a strict physical bound is needed.
The scan counts files under the run root without following external symlinks;
externally located storage is not covered by the directory limit. Filesystem
free-space monitoring covers the filesystem containing the run root.

These checks prevent an unnoticed space-budget breach; they do not establish
bounded reclamation or qualify a 72-hour run. `--forever --duration 3600`
creates a fresh topology each hour; `--duration 259200` keeps one topology for
72 hours, plus provisioning and the final verification/cleanup.

## Archive lag limits

`--materializer-max-lag-mb N` and `--wal-index-max-lag-mb N` configure the
writer's existing PostgreSQL archive lag limiters. Both default to zero
(disabled). The driver first loads and verifies the initial data, replays a
writer checkpoint and publishes a durable materializer marker. It then restarts
the writer with the requested postmaster settings and verifies the effective
values before starting the timed workload. Missing markers fail the setup;
limits are never installed into bootstrap configurations before this gate.

These are archive limits, not limits on WAL generation. A paused archiver can
move backlog to writer-local `pg_wal`; `compute_wal_kb` samples each compute's
local WAL allocation separately. The whole-run space budget covers these files.
Keep total-run and filesystem headroom limits enabled. Materializer bounds allow
segment alignment and a latched checkpoint crossing; WAL-index limits reserve
maximum-record/padding headroom, so neither setting is an exact byte ceiling.
Warnings that archiving is paused are expected while the worker catches up;
failure to catch up within `--sync-timeout` remains a finding.

## Exit notifications and persistent status

Use `pagestore_endurance_watch.py` outside the driver to observe normal exits,
startup exceptions and a driver killed by a signal. The hook is explicitly
configured; the watcher has no default remote recipient. For example, a local
desktop notification (when a desktop notification service is available):

```sh
python3 contrib/pagestore/endurance/pagestore_endurance_watch.py \
    --status-file /path/to/endurance/watch.json --label '72h seed 1' \
    --notify-command "notify-send 'forkpg endurance stopped'" \
    -- python3 contrib/pagestore/endurance/pagestore_endurance.py \
    --build build --root /path/to/endurance/runs --seed 1 --duration 259200 \
    --max-run-gb 12 --max-store-gb 8 --min-free-gb 5
```

The command is parsed into words and executed without a shell. The absolute
status-file path is appended as one argument. A custom delivery script should
read that JSON and send it through the operator's existing notification system.
Do not put credentials in command arguments. Hook output is discarded to avoid
leaking secrets. Run a delivery probe with the same status path and hook plus
`--test-notification` (without a driver command) and confirm it actually reaches
the recipient before starting a long run.

The atomic, fsynced status file contains a unique run ID, label, watcher/driver
PIDs, phase, heartbeat, exit code, completion timestamp and hook result. One
watcher owns each status path via an advisory lock. Heartbeats default to five
seconds and continue while an interrupted driver cleans up. Failure evidence
is written before the hook runs; notifications are attempted once for a failed
or interrupted command, and optionally for success (`--notify-on-success`).
The ten-second default hook timeout is adjustable. A hook failure is recorded
separately, preserves the driver's nonzero exit status, and makes a successful
driver/watch combination exit nonzero. `command_succeeded` means only that the
hook exited zero: it does not certify human receipt or provider delivery.
There is no automatic delivery retry or cross-restart exactly-once guarantee.

The watcher must also be kept alive, for example by an existing service manager
or external monitor. Its heartbeat proves the watcher is running, not that SQL
is making progress; use driver timeouts and a service/CI runtime limit as well.
If the watcher or host is killed, crashes, loses power or cannot write its status,
it cannot guarantee a notification; an independent monitor must detect a stale
heartbeat. A killed driver may leave computes behind: the watcher does not guess
ownership or kill arbitrary descendants. During ordinary interruption it sends
one SIGINT to the driver, allowing the driver's pidfd cleanup to finish; it does
not enforce an extra cleanup deadline. Notification is attempted when the driver
exits, so lengthy diagnostics/cleanup can delay it. Keep the status file outside
per-seed run directories, since successful runs remove their bulky data.

## Memory and descriptor budgets

`--max-rss-mb 4096 --max-fds 4096` enables sampled stopping thresholds for
summed RSS and open descriptors across the daemon and tracked compute process
trees, including PostgreSQL backends and background workers. Both options
otherwise default to zero (disabled). The CI endurance lane sets them explicitly.
An over-budget sample fails as `resource_rss` or `resource_fds`. Three consecutive
incomplete live-root observations fail as `resource_unmeasurable` while either
limit is enabled. As with space budgets, a running command may delay shutdown.

Metrics retain the `procs` root-process readings and add `resource_groups` and
`resource_totals`, with process count, RSS KiB and descriptor count. Roots record
PID and Linux start-time ticks to distinguish restarted generations. Collection
uses in-memory parentage from procfs and checks identities before/after reads;
it discards a group if its owning pidfd/Popen generation exits while sampling.
Short-lived backends may disappear between observations. Overlapping roots are
counted only once in the total. `resource_errors` records incomplete observations;
summary `stats.resource_peaks` preserves sampled maxima after successful run data
is removed. No process is signalled by this read-only collector.

The RSS sum deliberately double-counts shared mappings across different
processes. It is a conservative budget rather than PSS or a physical-memory
measurement. Descriptor counts are per process, including inherited descriptors.
The readings are not an atomic process census and are not a kernel memory/FD
quota. Temporarily controller-owned/private computes and processes no longer in
a tracked root's ancestry can fall outside coverage; use a service/cgroup budget
for a hard whole-service bound. A threshold breach does not by itself prove a
leak. Cache warming, shared memory, branch creation and restart generations must
be separated before diagnosing a growth trend. Short budget probes do not
qualify 72-hour memory stability.

## Page-segment I/O error recovery

On Linux, `pagestore_storage_error_test.py` runs an isolated real SQL recovery
probe using the test-only `pagestore_storage_io_fault.so` built by Meson. It
sets LD_PRELOAD only for its daemon wrapper, matches that test store's `seg_*`
files by file descriptor path and arms after baseline data verification. It
never fills the host filesystem or injects errors into the writer's local WAL.

```sh
python3 contrib/pagestore/endurance/pagestore_storage_error_test.py \
    --library build/contrib/pagestore/pagestore_storage_io_fault.so \
    --operation pwrite -- --build build --root /path/to/enospc-probe \
    --seed 1 --scale 1 --clients 1 --max-branches 0 \
    --sample-interval 1 --sync-timeout 120 \
    --max-run-gb 3 --max-store-gb 2 --min-free-gb 5
```

Use `--operation fsync` in a separate fresh root for EIO instead of pwrite's
ENOSPC. The probe stops materialization, commits and archives new writer data,
then arms the segment fault and restarts materialization. It requires an actual
injected-syscall report from the daemon and an explicit compute-side daemon
error response. Computes stop immediately and the daemon is killed while the
fault is still armed. After disarming/reopening, normal checksums and amcheck
must pass and the writer must retain the expected committed contents. The
fault window permits the intended storage/RPC errors but still rejects traps,
segfaults and invalid-page corruption; normal strict log checks resume before
recovery. Passed fault reports, fault-window logs and scope/provenance remain
under `passed/<seed>/storage-error-<operation>/` alongside the usual metrics.
CI runs all seven operations and uploads these artifacts, including failed-run logs.

Use `--operation short-pwrite` in another fresh root to write and return only
half of each requested segment write of at least 512 bytes while armed.
Small record headers pass through so the SQL probe reaches page payloads. Unlike pre-syscall errors,
this mutates actual segment bytes. The report must prove a positive byte count
smaller than the request; the same compute-error, crash, checksum and mandatory
amcheck oracles apply. Scope tests also verify that the unwritten suffix keeps
its old bytes and that unrelated paths remain unaffected.

Use `--operation wal-fsync` in a fresh root to fail `fsync` on the
store's top-level legacy `wal_<timeline>` WAL log with EIO. This is the
WAL log appended by `posix_wal_append_locked`, not the writer's local PostgreSQL
WAL or the raw immutable WAL segments under `wal_segments_<timeline>`. New
WAL records can have reached the kernel before the sync fails. The same
armed crash and mandatory SQL/amcheck recovery oracle applies. The matcher
accepts only a numeric timeline suffix; scope tests exclude page segments,
immutable WAL segments, backup-like filenames and another store's WAL log.
This mode arms before new writer commits and WAL switching. It requires an
archiver-side daemon error, an explicit failure for the switched WAL segment,
no `.done` marker, and retention of the local retry source before crashing. Recovery must retransmit the local WAL and catch up the materializer.

Use `--operation forkmeta-fsync` in a fresh root to inject EIO at fsync of
the exact top-level `forkmeta` append log. The SQL probe creates a table on
the local-WAL writer, ships its WAL, and arms before materializer replay.
The failed metadata append must surface as an explicit daemon error to compute.
This mode also accepts the replay process promoting that error to FATAL and
exiting with status 1; other probes retain the ERROR-only response oracle.
Recovery must reconstruct
the new relation and its contents and pass mandatory amcheck on both computes.
This covers append-log file sync, including bytes written before the error;
it does not qualify metadata rewrite/rename, directory sync, timeline logs or
retention metadata. The fault remains armed until the daemon is killed.

`test_metadata_directory_sync.py` additionally runs the real POSIX backend in
an isolated subprocess. Its preload mode `metadata-dir-fsync` starts failing
only after the exact `forkmeta.rewrite.tmp` to `forkmeta` rename succeeds; it
then returns EIO for the containing store directory's fsync. Ordinary directory
syncs before publication and unrelated renames remain healthy. The backend
must report rewrite failure even though the complete replacement is visible.
The child exits immediately without backend close or a later successful sync.
A new process reopens the complete replacement, retries its publication, appends
new metadata and reopens again to check both acknowledged operations. CI runs
this probe and retains its syscall report in `metadata-directory-sync.log`.
This is storage-backend publication/retry coverage with opaque byte records,
not SQL-level snapshot-selector recovery or power-loss durability. It does not
model loss/reordering of kernel-cached directory entries after host failure.

The SQL runner also accepts `--operation metadata-dir-fsync`. It enables the
forkmeta reclamation controller at 64 KiB / 32 KiB and replays 128 create/drop
table pairs to force a snapshot cutover. The preload fails directory fsync
after the exact source-log replacement rename. Unlike append faults, cutover
failure poisons metadata and can stall replay without emitting a compute error.
This mode requires the explicit inspection state `forkmeta_poisoned=true` and
a published selector; a timeout or connection failure is insufficient. It
kills computes and daemon while still armed, retains selector/immutable parts
and source-log evidence, then reopens and requires poison to clear, materializer
catch-up, matching committed contents and mandatory amcheck on both computes.
This qualifies SQL recovery for this process-crash boundary, not selector loss
or directory-entry reordering under power failure.

Use `--operation selector-dir-fsync` for the earlier selector commit boundary.
It uses the same SQL churn, poison-state, armed daemon crash and recovery
oracles, but fails fsync of `forkmeta_snapshots` only after a successful
`renameat` of `forkmeta_manifest_v1.tmp.*` to `forkmeta_manifest_v1` in that
exact directory. This exercises manifest publication failure before the later
source-log rewrite; metadata-dir-fsync instead fails after source replacement.
`test_selector_directory_sync.py` verifies successful-rename gating, persistent
EIO, disarming and unaffected file/root/other-store fsyncs and unrelated renames.
CI retains the scope report as `selector-directory-sync.log` and the SQL runner
retains snapshot/source evidence. Process restart reconciles a manifest visible
in the kernel cache; power-loss disappearance or reordering is not simulated.

The models cover persistent pre-syscall errors and positive short writes on page
segments, legacy WAL-log fsync EIO and forkmeta append-log fsync EIO. They do not model torn writes after a successful full-length return,
actual full-filesystem behavior, immutable WAL-segment and other metadata I/O failures,
slow devices or power loss; process death leaves the kernel page cache intact.
It is a bounded recovery probe, not arbitrary fault injection into every random
endurance round or 72-hour storage-fault qualification. The preload library is
not installed as a runtime PostgreSQL module.

## Topology

The provisioning mirrors `mvp_golden_test.sh` step for step, so a failure here
that the golden scenario does not show is caused by load, duration or
interleaving, not by a different setup:

- a WAL-only **writer** (`route_all = off`, `archive_library = 'pagestore'`);
- a continuous **materializer** (hot standby, `route_all = on`, 32 MB of shared
  buffers so that most reads come from the store);
- up to `--max-branches` **branch computes** (0 disables branching; the
  oldest is retired before a new one is created), each created by the installed
  `pagestore_branch_prepare` controller with
  `--verify-seed-against-materializer`, booted the portable way
  (`pagestore_control_restore --archive-bootstrap`, then
  `pagestore_install_prepared_branch_bootstrap`);
- one `pagestore_daemon` on a POSIX store.

One run is one topology lifetime: provisioning, then rounds for
`--duration` seconds (at least one).  `--forever` runs seed,
seed+1, ... each from a fresh `initdb` and import, so the bootstrap paths are
exercised as often as the steady state.

## Repeated branch reclamation

`--branch-reclaim-cycles 3 --branch-reclaim-slack-mb 64` runs fixed-parent
create/write/restart/delete cycles before the random workload. At least two
cycles and `--max-branches >= 1` are required. The first deleted branch warms
up the parent checkpoint/materialization paths and establishes a physical store
allocation and retention-owner baseline. Subsequent cycles must return below
that store allocation plus the configured slack, and must not increase any
owner/resource count. Generation advances are expected and are not treated as
leaks. The existing `--sync-timeout` bounds each reclamation wait.

Each cycle requires the real timeline deletion API, verifies deletion reaches
`deleted`, advances parent checkpoints/materialization, checks parent contents
are unchanged, and rejects leftover branch PGDATA, redo scratch or prepared
copies. `branch_reclaim_complete` events record the warmup flag, timeline,
pre-delete store sample, retained allocation, baseline, slack and owner counts.
`store_kb()` fails on measurement errors instead of reporting false zero usage.
The global run/store budgets and free-space reserve apply throughout.

This is a finite residual-allocation regression check, not proof of a
long-term plateau: metadata may grow within the slack and the warmup itself
has no separate reclamation bound beyond the global space budgets. Increase
cycle count for longer qualification; the configured random-workload duration
starts after these cycles. Parent data is held constant during the preflight
so branch disposal is measured independently of random parent growth.

## Rounds

Each round is a seeded choice of:

1. **Burst.**  `pgbench` with eight custom scripts: balance transfers, FK
   inserts and range deletes on an indexed table, out-of-line TOAST rewrites
   up to 96 KB, aborts, subtransaction aborts, two-phase commits and
   overlapping `FOR KEY SHARE` lockers (which, with the FK, produce
   multixacts).  A second thread issues DDL and maintenance against the
   writer: create/drop/truncate, `VACUUM FULL`, `CLUSTER`, `REINDEX`,
   `ADD COLUMN`, bulk churn, `VACUUM (FREEZE)`, `CHECKPOINT`.  Autovacuum runs
   with a 5 s naptime and checkpoints every 30 s / 128 MB.
The DDL thread has the same wall-clock deadline as pgbench. A statement
already running may finish, but waiting for an injected restart or restartpoint
does not extend the DDL window and keep adding WAL during recovery catch-up.

2. **One injected event during the burst:** materializer fast restart,
   immediate stop, `SIGKILL` of its postmaster, a forced restartpoint (a
   writer `CHECKPOINT` is replayed first, and the materializer's `CHECKPOINT`
   must move `pg_control_checkpoint()` up to it; if the materializer's own
   timed restartpoint gets there first, the driver retries with a newer
   checkpoint); writer
   immediate stop, or `SIGKILL` of one writer backend (crash-restart cycle).
   Orphaned prepared transactions are rolled back afterwards.
3. **Verify** (below), sometimes after restarting the materializer so that its
   buffer cache is empty.
4. **Branch** (`--branch-probability`), or a **coordinated store restart**
   (every compute stopped first, which is the MVP contract; the daemon gets
   `SIGTERM` or `SIGKILL`), followed by another verify.
5. Live branches are written to again and restarted at random.

## Oracles

The design rule from `HARNESS_DESIGN.md` holds: no test-side model of
visibility or redo.  The independent reference is PostgreSQL itself on a
path that does not touch the store.

| Check | Reference | What a mismatch means |
|---|---|---|
| Per-table `count` and order-independent row hash, materializer vs writer, with the writer quiescent and replay past the switch LSN | The writer's relations live in its local `md` files; the materializer's come from the store | WAL shipping, redo into the store, page versioning, reclamation or restart recovery lost or invented a row |
| The same hash, new branch vs the writer's state in the quiescent branch window | Writer | Fork-point visibility, SLRU seeding, relation-map/bootstrap install |
| Writer hash unchanged after only the branch was written | Writer before | Ancestry cutoff leaked a branch write into the parent |
| Branch hash before vs after a fast or immediate restart following `CHECKPOINT` | The same compute with a warm cache | A branch write did not survive in the store |
| Every live branch's hash before vs after a coordinated store restart (clean or `SIGKILL`) | The same compute before the restart | Branch-only pages were lost or changed when the store reopened |
| Every `public` index definition and validity, alongside each row hash above | The same reference | Replay or bootstrap lost, invented or changed an index |
| `sum(bal)` equals its initial value on every compute | Workload invariant | Torn or duplicated transaction effects (covers 2PC and aborts) |
| Index scan vs sequential scan for one `ev.k` | Same compute | Index and heap disagree |
| `bt_index_check(idx, heapallindexed => true)` on every btree | amcheck | Structural index corruption |
| The controller's seeded SLRU pages vs the materializer's | PostgreSQL recovery | `clog`/`commit_ts`/`multixact` appliers diverge from redo |
| Log scan: `PANIC`, `TRAP`, invalid page, unreadable block, signals 6/7/9/11 (9 only for a backend the driver did not kill itself) | -- | Assertion or crash anywhere |
| Liveness: every wait has a deadline; daemon and postmasters must be alive when expected | -- | Hang or unexplained exit |
| During a writer fault, pgbench and DDL errors must all be connection loss or recovery-in-progress | -- | A read or write error hidden behind the injected crash |
| The run ends with a fast stop of every compute and a `SIGTERM` of the daemon, which must exit 0 | -- | The final flush or shutdown failed |

Sequences are not compared: a sequence page legitimately differs between a
primary's buffer and replayed WAL.

## Output

`<root>/run-<seed>/` holds the store, every data directory and log,
`events.jsonl` (every action with its seed-derived choice), `metrics.jsonl`
(every `--sample-interval` seconds: RSS and open descriptors of the daemon and
each postmaster, store size by top-level directory) and `diagnostics/`
(`pagestore_inspect` dumps and `pg_stat_activity` at a timeout or failure).

- Pass: `PASS.json`, `events.jsonl` and `metrics.jsonl` move to
  `<root>/passed/<seed>/`; the bulk is deleted.
- Failure: the root stays as it was -- computes stopped immediate, the daemon
  `SIGKILL`ed so the store is not flushed past the failure -- with
  `FAILURE.json` naming the kind, detail, seed, round and commit.
- `<root>/history.jsonl` has one line per run.  `--forever` stops after
  `--max-failures` preserved roots or below `--min-free-gb`.

A seed fixes every driver choice (burst lengths, events, DDL, pgbench seeds)
but not the interleaving of concurrent sessions, so a rerun repeats the
schedule, not necessarily the failure.  Rerunning a seed whose failure root
is still under `--root` renames that root to `run-<seed>.<mtime>` first.  Rule from `RELEASE_VALIDATION.md`: an
unexplained failure stays open even if the rerun passes.

## Findings so far

**E-1. Branch preparation failed whenever the newest replayed checkpoint
record already had its restartpoint** (fixed in the controller).
`pagestore_capture_slru_snapshot()` requests a restartpoint and then requires
the durable materializer watermark to equal the paused replay position.
`CreateRestartPoint()` is a no-op when no checkpoint record newer than the
last restartpoint has been replayed, so the capture raised "restartpoint did
not durably cover the paused WAL replay position".  The golden scenario never
saw it because the script issues `CHECKPOINT` just before the controller and
the materializer's 5-minute timer has not consumed it; on a system that
checkpoints normally the consumed state is the common one.  The controller
now publishes its own checkpoint before the base capture (`CHECKPOINT`, WAL
switch, wait for replay) and retries a bounded number of times when the
materializer's own restartpoint still wins the race.  The fork capture uses
the writer's shutdown checkpoint and can in principle lose the same race; the
driver retries the whole controller and logs `branch_prepare_retry`, so its
frequency is measured rather than assumed.

**E-2. Materializer replay is far slower than WAL production, and got slower
as the store grew** (seed 1000: 16 MB segments went from 5 s to over 2 min
each, and a 27 s burst was not replayed in 30 min).  Stack samples of the
daemon showed the request worker almost always waiting for a lock held by the
maintenance thread.  Three causes, in the order found:

1. *Fixed.*  `ps_image_layer_lookup()` read and checksummed the layer's whole
   index section on every page lookup, then scanned it linearly; one
   `wal_retain_floor_level()` pass does that for every candidate layer while
   holding the admission lock.  Image indexes are now cached per process,
   keyed by layer id plus the footer that certifies them, and binary-searched.
2. *Fixed.*  With the map lock already held, `layer_map_lookup_impl()` dropped
   the `data_verified` result, so every such lookup checksummed the layer's
   entire data section again.
3. *Open (design).*  `compact_timeline()` merges every image layer of the
   shard into one -- cost proportional to the store, once per eight flushes --
   and does all of it (read, checksum, sort, write, fsync) under the shard and
   map write locks that every page write needs.  The redundant second
   checksum of inputs under the lock is gone, but building the replacement
   outside the lock and tiered compaction (both already on the post-MVP list
   in `MVP_COMPLETION_PLAN.md`) are what would change the order of magnitude.
   Disabling segment GC (`--segment-gc 0`) made no measurable difference.

Same seed, scale 1, 4 clients for 20 s (~800 MB of WAL), cassert build: replay
288 s before, 238 s after (2.8 -> 3.5 MB/s).  The index cache matters more as
layers grow; the long runs are the measurement for that.  None of this has
been measured on a production build yet.

**E-3. Image layers over 2 GiB cannot be read** (seed 2100; fixed in #286).
`local_read_layer_block()` issued one `pread()` and required the full
length; Linux returns at most `0x7ffff000` bytes per read and data
verification reads a layer's whole data section at once, so a layer past
2 GiB failed every lookup and could never be compacted away.  The
materializer died with `daemon reported error for op 9` (READV).

**E-6. Writes of WAL-less pages are refused once the fork-metadata
compaction cutoff passes the fork's definition** (seeds 2101, 2103, 2104;
fixed by promoting ordered writes to the selected cutoff).  Symptom: a `route_all` compute logs
`daemon reported error for op 8` (WRITEV) for the same block again and again
-- `base/5/2840_vm` block 0 on a branch, `base/5/37249_fsm` block 2 on the
materializer -- until the checkpointer cannot write it and the compute dies
(FATAL on the materializer, aborted transactions on the branch).  The
daemon logs nothing.

Root cause, established on the preserved seed-2104 store:

- The relation's FSM fork was defined (`SET`, `GROW` to 3 blocks) at
  `0/FA14F078`; those events sit in the compacted fork-metadata checkpoint
  of generation 4, whose cutoff is `0/FFA58DB0` (seq 890109).
- FSM pages are never WAL-logged, so their `pd_lsn` is 0.  In
  `append_page_impl()` a zero-LSN relation page becomes a *WAL-less ordered
  record* whose header LSN is the fork's growth floor
  (`fe->last_def_lsn` = `0/FA14F078`), and every ordered record must satisfy
  `fork_meta_mutation_future(hdr_grow_lsn, admission_seq)`, i.e. be
  lexicographically above the cutoff -- which `0/FA14F078 < 0/FFA58DB0`
  never is, whatever the admission sequence.  The write is refused with
  `PS_APPEND_REFUSED_FORKMETA_CUTOFF` even though block 2 already exists
  (no growth) and the segment could hold it.
- On the branch the floor is `branch_lsn + 1` (a visibility-map page
  inherited from the parent keeps a pre-fork `pd_lsn`, so it is *clamped*
  to that floor); the branch is exempt from the cutoff only while it has no
  durable page frontier, so after enough branch writes the same refusal
  appears there.

So every relation's FSM (and any VM page that is dirtied without a WAL
record) stops being writable on a `route_all` compute as soon as one
fork-metadata compaction has run past the fork's definition, which on a busy
store is a matter of an hour.  The soak test never sees it because its
workload writes every page with a real LSN; the integration scripts are too
short for a compaction cutover.

The fix gives a newly admitted ordered record the operational position
`max(growth_floor, selected_cutoff_lsn)`. Its fresh admission sequence orders
it strictly after the selected cutoff. Both the segment header and the bound
marker carry that position; clamped copies use it as their version as well,
while WAL-less pages keep version zero and remain unavailable to capped reads.
Growth is recorded at the same position, so it cannot change an older retained
size. Real WAL-versioned writes and explicit historical metadata mutations
retain their rejection rules. No persisted format changes.

`pagestore_forkmeta_cutover_test` covers root and child timelines, WAL-less and
clamped writes, rewrites and growth after cutoff, same-LSN truncate/regrowth,
refusal of real historical WAL growth, source-tail recovery after an unclean
process exit, and two subsequent cutovers/reopens. The regression fails on the
old admission rule. Local cassert validation: 6,220 cutover checks, 77/77
pagestore Meson tests, and `integration_test.sh` including retained-store reopen.
These are focused regression results, not multi-day release qualification.

**E-7. Branch preparation intermittently finds no control image for the
shutdown checkpoint it forks from** (local seed 42, scale 1, `--max-branches
1`, third branch; open).  The controller stops the writer and forks from its
shutdown checkpoint (`lsn = redo = 0/40000028`, the first record of a fresh
segment), restarts it on a private socket, and
`pagestore_prepare_branch_from_control()` fails four seconds later -- well
inside its 10 s horizon timeout -- with `branch checkpoint redo is not
exactly mirrored`.  So the newest mirrored control image at or below
`0/40000028` still carried an older redo: the shutdown checkpoint's image,
written by the exiting checkpointer and shipped only by
`ps_control_exit_drain()` with bounded waits, did not reach the store (or not
at that LSN).  The two earlier branches of the same run took the identical
path (`0/14000028`, `0/2F000028`) and passed; the second, like the third,
directly followed the deletion of the retired branch.  The next step is to
count `ps_control_dropped` and the exit drain's outcome, and to read the
control object's versions on the preserved store.

**E-8. `pagestore_inspect health` reported a dead daemon's segment as
ready** (CI seed 1, round 5; fixed, twice).  A daemon killed with `SIGKILL`
leaves its shared-memory object with a valid header and `startup_state =
READY`.  The next daemon reuses the object (`O_CREAT` without unlinking)
and only invalidates the header after taking its locks, while `health`
mapped the object read-only and checked only the header.  A readiness probe
issued between the new daemon's launch and its initialization therefore
succeeded against the stale header; the writer then attached
mid-initialization and failed with `pagestore localsvc shared memory
incompatible ... magic=0x0`.

The first fix (round 1) had `health` and every client attach require two
independent `F_GETLK` probes -- one of the daemon lease (byte one), one of
the initialization byte (byte zero) -- plus pid comparisons to rule out a
daemon restart landing between them.  That scheme had its own gap (P1):
`F_GETLK` never blocks and never takes anything, so nothing kept the lease
probe's result from going stale before the init probe ran.  Concretely, the
lease probe could observe daemon A still alive, A could then exit, and the
init probe -- run strictly afterwards, so it necessarily found the
never-touched init byte free -- was on its own enough for the old decision
logic to report "ready", even though by then nobody held the lease at all.
No amount of extra pid bookkeeping closes a gap between two probes that are
simply not atomic with each other.

The real fix (round 2) stopped probing and instead serializes with the
daemon's own initialization protocol via a real, non-blocking lock:
`ps_shm_hold_init_shared()` takes a *shared* (`F_RDLCK`) lock on the init
byte.  The daemon holds the init byte *exclusively* from before it
invalidates the header until it publishes READY, and holds the lease byte
exclusively for its whole lifetime after that.  So a shared lock on the
init byte cannot be acquired while any daemon is mid-initialization, and
while our shared lock is held, no daemon can begin that window either.  For
as long as we hold it, therefore, no daemon is initializing and none can
start; if `ps_shm_lease_held()` then finds a lease holder, that holder must
already be past READY, and the header we validate while still holding the
lock is that live daemon's own stable, already-published header.  No pids
are compared anywhere, so pid namespaces are not a concern.  `health` and
every client attach (backend, `pagestore_import`, `pagestore_walrestore`,
`pagestore_control_restore`) now use this hold-lock / validate-header /
check-lease / release-lock sequence (see `pagestore_shm.h` for the full
argument); the relation-inspection path is unaffected, since it already
takes the init byte exclusively itself for its own mailbox-ownership
reasons.

One behavioural consequence: relation inspection's exclusive hold and
`health`'s new shared hold now legitimately contend, so `health` correctly
reports "not ready" for as long as a relation-inspection call is in
flight, clearing up as soon as it releases the byte.  On the daemon side,
clients briefly holding the init byte shared is now an expected, routine
occurrence (an orchestrator polling health across a restart), so the
daemon retries a busy init byte for up to ~10s instead of failing
immediately; it still fails immediately if the *lease* byte is held (a
live competing daemon, not a transient inspector).  Client attach
(`ls_attach`, `pagestore_import`, `pagestore_walrestore`,
`pagestore_control_restore`) likewise waits out a transient exclusive
holder of the init byte (bounded, ~10s) instead of raising a spurious "no
running, initialized daemon" error the moment it collides with a
relation-inspection call or a daemon that is (briefly) still initializing;
`health`/non-relation `pagestore_inspect` stays non-blocking, since its
callers already retry.

`pagestore_inspect_mailbox_test` covers a dead daemon's READY header, a
daemon still initializing, an initialized daemon, an inspector holding
byte zero, and -- the P1 regression -- a lease holder that has exited
between an earlier observation and the real check.  `pagestore_test`
additionally drives the real daemon binary against a pre-held shared init
lock and confirms it waits instead of exiting.  The driver still removes
the stale object before each daemon start; that is no longer required.

Also needed: the daemon should log one line for every refused or failed
READV/WRITEV/BEGIN_DELETE (opcode, timeline, key, block, reason), as it
already does for artifact refusals.  E-3 and E-6 each took a preserved store
and a debugger session to explain what one log line would have said.

## WAL indexing and space limits

The writer enables `pagestore.auto_wal_index`; the recovery materializer pins
only the raw WAL it consumes. Per-page index compaction must not prevent that
owner from advancing to a new restartpoint. Metrics include the daemon's
backpressure, GC, pruning and owner snapshots alongside RSS, descriptors and
per-file store sizes. An absent inspection sample can occur during a
coordinated daemon restart.

Rejected materializer retention advances are findings, rather than ignored
warnings. Failed pgbench runs retain their complete output, including the
initial error before the transaction statistics. Local-service failures also
report the relation/fork/block and request fence in their error detail.

Current writable-compute FSM reads negotiate the daemon's optional FSM-reset
operation. They rebuild inherited WAL-less free-space hints from zeros rather
than inheriting mutable parent hints. Ordinary and historical reads keep their
existing refusal behavior; data forks are not eligible for hint reset.

`--max-store-gb` is a stop threshold, not a reclamation controller. Controller
high-water/catch-up settings are supplied with repeated `--daemon-arg` options.
Indexing is necessary for WAL reclaim but does not guarantee bounded disk:
legacy unbounded WAL-index horizons and unversioned forks conservatively keep
FPI-led chains. Finite index views can instead retain durable bases, releasing
obsolete FPI dependencies after compaction. Do not treat a short run
with indexing enabled as proof that a 72-hour run fits its space budget.

## Not covered yet

- **Bounded space.**  Retired branches are deleted through
  `pagestore_delete_branch()` (extension 1.3) and the driver requires the
  store to reach `deleted`; the store size before and after is recorded in
  `branch_deleted`, but no bound is asserted yet.  On a build without the
  function the timeline stays in the store.
- **Pinned and advancing readers.**  Their provisioning (reader base,
  prepare/install, retention owner) is only scripted inside
  `integration_test.sh`.
- **The materializer supervisor.**  The driver restarts the materializer
  itself; `pagestore_materializer_supervisor` should own it.
- **Broad storage faults.** Isolated page-segment ENOSPC and fsync EIO recovery
  probes, positive short-write recovery, legacy WAL-log fsync EIO and forkmeta
  append-log fsync EIO recovery
  are covered. Torn writes after a
  successful full-length return, actual full-filesystem behavior, immutable WAL-segment and
  other metadata errors, slow devices and arbitrary named fault points under mixed load remain
  unqualified.
- **Power loss.**  Process kills leave the OS page cache intact.
- **Long-term leak verdicts.** Sampled RSS/descriptor budgets and resource peaks
  are enforced when configured, but steady-state leak trends across a long
  topology lifetime are not yet qualified.
- Branches of branches, user tablespaces, more than one database.
