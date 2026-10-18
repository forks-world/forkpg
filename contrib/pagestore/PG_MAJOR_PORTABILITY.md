# Porting `pagestore` across PostgreSQL majors

Two related but separate bodies of work, both driven by the V4 plan
(`branchdb_18` is the first release candidate; see `RELEASE_VALIDATION.md`
and `MVP_COMPLETION_PLAN.md`'s 2026-09-21 rows):

1. **The core patch series (C1-C7)** — a small, curated set of PostgreSQL
   core changes (outside `contrib/pagestore`) that `pagestore` carries on top
   of upstream. These are not in `contrib/pagestore` and are not guardable
   from within it; each `branchdb_N` gets its own port of the series, done
   once per major by cherry-picking and resolving conflicts against that
   major's headers (`scripts/branchdb-sync.sh forward`, once the series
   carries `Branchdb-Series:` trailers).
2. **`contrib/pagestore`'s own version-specific API surface** — places where
   `contrib/pagestore` calls a PostgreSQL core symbol, type or macro that
   does not exist, or has a different shape, before 19. Unlike the core
   series, `contrib/pagestore` stays **byte-identical** across `pagestore`
   and every `branchdb_N` (AGENTS.md), so these need `#if PG_VERSION_NUM`
   guards landed on `pagestore` itself *before* a release branch's
   `sync-contrib` copies the tree — a release branch cannot special-case its
   own copy without breaking that invariant.

## 1. The core patch series (C1-C7)

Source: the V4 plan (`git log --no-merges dc511678084..origin/pagestore --
src/ doc/ contrib/meson.build`, 51 commits, +2414/-211 across 58 files). Each
series becomes one squashed commit per release branch, carrying a
`Branchdb-Series: C<n>` trailer naming the series and listing its source
SHAs, cherry-picked in order by `scripts/branchdb-sync.sh forward`.

| # | Series | Source commits (oldest -> newest) | Files |
|---|---|---|---|
| C1 | smgr pluggable switch, md seams, recovery prefetch | `1ad5b2c7c17` (smgr part), `c0f4e0c320b`, `aa13cf51ce9` | `storage/smgr/smgr.c`, `smgr.h`, `md.c`, `md.h`, `xlogprefetcher.c`, `xlogutils.c` |
| C2 | walredo helper process + VM buffer router | `1ad5b2c7c17` (walredo part), `b0bb42d98e5`, `4f69e7a3b66`, `8e967c9dafe`, `0db86c4d4e6`, `6dd9d38205a` | `postmaster/walredo.c` (new), `walredo.h`, `main.c`, `postmaster/Makefile`, `postmaster/meson.build`, `postmaster.h`, `tcop/postgres.c`, `tcopprot.h`, `heap/visibilitymap.c`, `doc/src/sgml/config.sgml` |
| C3 | control-file write hook, LSN-versioned pg_control mirror, restartpoint ordering | `e2650f9cffb`, `3cdbed7c33e`, `0a23790e225`, `fd4c6f2ded2`, `1c65c1b425a`, `00ebe43db73`, `c24caa82cec`, `8d140241824`, `82ae54c6ae6`, `18b0d418ab8`, `95f7593d53c`, `58c8ff5c369`, `c8053577b8a`, `fc409dc117b`, `87b6fe0f104`, `ba9baf898fc` | `transam/xlog.c`, `xlog.h`, `xlogrecovery.c`, `xlogrecovery.h`, `catalog/pg_control.h` |
| C4 | SLRU read/write hooks and store-backed mirrors | `ec89df7fadf`, `317c69cdc54`, `d2c50638f95` | `transam/slru.c`, `slru.h`, `clog.c`, `commit_ts.c`, `multixact.c` |
| C5 | pinned/advancing reader: read horizon, snapshot pinning, read-only enforcement | `a710f29e37d`, `b8faccc7201`, `fe5f36f2a66`, `8521a7f7818`, `5c135ee76a6`, `f736ff6f92b`, `38f5834e09e`, `5def2a80515`, `22b1e577561`, `5f2cba89711`, `e677c12af20`, `f48c4e25887`, `39b998a8e5e`, `5ab5bf9e896`, `bc18352304b`, `841a79f0fe8`, `4196143380d`, `74dc472d58d`, `cca6d192978` | `procarray.c/h`, `snapmgr.c/h`, `bufmgr.c`, `buf_internals.h`, `bufmgr.h`, `xact.c/h`, `varsup.c`, `transam.h`, `commit_ts.c/h`, `postinit.c`, `miscadmin.h`, `variable.c`, `xid8funcs.c`, `lockfuncs.c`, `locktag.h`, `lmgr.c`, `relmapper.c/h`, `pruneheap.c`, `autovacuum.c`, `bgwriter.c`, `wait_event_names.txt`, `datachecksum_state.c`, `system-views.sgml` |
| C6 | fork-event stamping and ordering seams | `541b950dfbd`, `d055356e4ab`, `6ad088dcbb9`, `8559a3d5355` | `catalog/storage.c`, `catalog/index.c`, `heapam_handler.c`, `commands/sequence.c`, `commands/tablecmds.c`, `bufmgr.c`, `xact.c`, `xlog.c` |
| C7 | build wiring | `11e69d85266` | `contrib/meson.build`; `contrib/Makefile` (line 32, `pagestore` added to the `SUBDIRS` list -- also touched directly on `pagestore` itself, by `a3c234242f7`, not only by each `branchdb_N`) |

`datachecksum_state.c` (C5) is gone from `REL_19_STABLE` too (upstream
reverted online data checksums after the `pagestore` base) and from every
`branchdb_N` target -- the hunk touching it is dropped, not ported, on every
major, including the eventual `pagestore`-onto-`REL_19_STABLE` merge (P0).

## 2. `contrib/pagestore`'s version-specific API surface

`contrib/pagestore` carries one `#if PG_VERSION_NUM` guard from the P1 sync
pass (`pagestore_slru.c`'s `access/multixact_internal.h` vs
`access/multixact.h` include, which also re-implements
`MultiXactIdToOffsetPage()`/`MXOffsetToMemberPage()` under its `#else`
branch, see below) plus, as of the "PG 18 compatibility guards" PR (P2),
every remaining guard in the table below. Building the full local CI matrix
on `branchdb_18-rc` (`meson setup`/`ninja`, the standalone cc lane,
`meson test --suite pagestore`, `integration_test.sh`, the redo demos,
`mvp_golden_test.sh`, `branch_boot_test.sh`) surfaced these as the complete
set of sites where `contrib/pagestore` -- written once against pagestore's
tracked upstream (19+) -- calls an API or assumes an on-disk shape that
upstream introduced, renamed, or reshaped only after 18. Each was cross-
checked against the exact 18 shape (`git show upstream/REL_18_STABLE:<path>`,
not guessed from the compiler error) and guarded `#if PG_VERSION_NUM` (or,
for the two shell-script sites, an equivalent `server_version_num` check),
with no behavior change on 19+: every guard's `>= 190000` branch is
textually identical to the pre-guard expression, and building on 19 confirms
that branch is the one actually compiled. None of these are core-series
gaps (P2, section 1 above) — they are `contrib/pagestore` calling a core
symbol whose *name, arity, or field layout* differs between 18 and 19,
independent of whether the C1-C7 series is present.

| Symbol | 19 (pagestore's base) | 18 | Sites (file:line, this PR's HEAD) | Status |
|---|---|---|---|---|
| `MultiXactIdToOffsetPage()`, `MXOffsetToMemberPage()` | public `static inline` in `access/multixact_internal.h` | `static` in `access/transam/multixact.c` on stock 18, not exported | `pagestore_slru.c` (the include guard's `#else` branch) | Guarded by the P1 sync pass, not this PR: identical formula re-implemented under the `#else`, see the guard's own comment. Local helpers use `ps_multixact_offset_page()` / `ps_multixact_member_page()` to avoid conflicting with C4 core exports; 19 aliases these names to its public inline helpers |
| `ReplOriginId` | `access/xlogdefs.h` typedef, renamed from `RepOriginId` | `RepOriginId`, typedef in `access/xlogdefs.h:66` (not `replication/origin.h`, which only uses the type) | `pagestore.c`, `pagestore_slru.c` (both via `pagestore_backend.h`) | Guarded in this PR (`typedef RepOriginId ReplOriginId;` on 18) |
| SLRU control struct (`SlruCtl`/`SlruDesc`, `ctl->options.X`) | `SlruDesc`, fields on a nested `options` sub-struct | `SlruCtlData` (typedef'd `SlruCtl`), fields directly on the struct | ~30 sites in `pagestore_slru.c` | Guarded in this PR (`PS_SLRU_OPT(ctl)` macro + `SlruDesc` alias on 18) |
| `XLogFindNextRecord()` | 3 arguments (`char **errormsg` out-param) | 2 arguments | 10 sites in `pagestore.c` | Guarded in this PR (`PS_XLogFindNextRecord()` drops the extra argument on 18) |
| `xl_multixact_truncate` fields | `oldestMulti`, `oldestOffset` | `startTruncOff`/`endTruncOff`, `startTruncMemb`/`endTruncMemb` | `pagestore.c` (multixact apply/seed-reconstruct paths) | Guarded in this PR (`PS_XLREC_MXTRUNC_OLDEST_MULTI/OFFSET()`; 18's `endTrunc*` confirmed the same role by reading both versions' `multixact_redo()`) |
| `planner_hook_type` | trailing `ExplainState *es` | no `es` parameter | `pagestore_planner()`, `pagestore.c` | Guarded in this PR (`PS_PLANNER_CALL()`, `#undef`'d after use; `es` is only ever forwarded, never inspected) |
| `PageSetChecksum()` | `PageSetChecksum()` | `PageSetChecksumInplace()` | `pagestore.c` | Guarded in this PR (pure rename, `#define`'d on 18) |
| `CHECKPOINT_FAST` | `CHECKPOINT_FAST` | `CHECKPOINT_IMMEDIATE` (no `CHECKPOINT_FAST` flag) | `pagestore.c` | Guarded in this PR (pure rename, `#define`'d on 18) |
| `BGWORKER_INTERRUPTIBLE` | present (`postmaster/bgworker.h`) | **absent** -- no such flag value defined in 18's `bgworker.h` | `pagestore.c` (reader-artifact worker registration) | Guarded in this PR (`#define`'d to 0 on 18: every bgworker ran without this protection before it existed) |
| `CheckPoint.logicalDecodingEnabled` | present (`ControlFileData`/`CheckPoint`) | **absent** — not a field of 18's `CheckPoint` struct at all (`catalog/pg_control.h`); confirmed against both stock `REL_18_STABLE` and `branchdb_18-rc` | `ps_checkpoint_matches_control()`, `pagestore.c` | Guarded in this PR (`#if PG_VERSION_NUM >= 190000` around just this comparison) |
| `CheckPoint.dataChecksumState` / `ControlFileData.dataChecksumState` | present on `pagestore`'s current (pre-19.0-release) tracked-upstream base | **absent** — online data checksums never shipped in 18; the field exists only on the transient base `pagestore` currently compiles against, not before or after it. Confirmed absent from both stock `REL_18_STABLE` and `branchdb_18-rc`'s `pg_control.h` (C3's port drops the same online-data-checksums hunks `datachecksum_state.c` needed, see section 1's note) | `ps_checkpoint_matches_control()`/`pagestore_control_image_compatible()`, `pagestore.c` | Guarded in this PR (`#if PG_VERSION_NUM >= 190000`); see "Known limitations" below (limitation 1) |
| `ControlFileData.slru_pages_per_segment`, `SLRU_PAGES_PER_SEGMENT` | `ControlFileData.slru_pages_per_segment` is a real field (`catalog/pg_control.h:211`); `SLRU_PAGES_PER_SEGMENT` moved to `pg_config_manual.h` as a compile-time constant | **absent** -- `ControlFileData` has no `slru_pages_per_segment` field on 18 at all; only the `#define SLRU_PAGES_PER_SEGMENT 32` in `access/slru.h` exists | `pagestore.c` (`ps_checkpoint_matches_control()`/`pagestore_control_image_compatible()`), `pagestore_control_restore.c` (frontend; `access/slru.h` is not frontend-safe, so the constant 32 is aliased locally rather than included) | Guarded in this PR |
| `T_RepackStmt` / `T_ClusterStmt` (pinned-reader deny-list) | `T_RepackStmt` (`REPACK`, unifying `CLUSTER`/`VACUUM FULL` as `RepackCommand` `REPACK_COMMAND_CLUSTER`/`_VACUUMFULL`/`_REPACK`); `RepackStmt` present in `nodes/parsenodes.h` | **absent** -- `RepackStmt` does not exist in 18's `parsenodes.h` at all (`REPACK` is a 19+ feature); 18 instead has its own `T_ClusterStmt` for `CLUSTER`, and `VACUUM FULL` stays a plain `T_VacuumStmt` with `VACOPT_FULL` | `backend_localsvc.c`'s `ls_pinned_process_utility()` | Guarded in this PR: `#if PG_VERSION_NUM < 190000` adds a `T_ClusterStmt` case denying `"CLUSTER"` the same way `T_RepackStmt` denies `"REPACK"` on 19+ (confirmed against `upstream/REL_18_STABLE`'s `parsenodes.h`); `VACUUM FULL` needed no new case, since the existing `T_VacuumStmt` case already denies `VACUUM`/`ANALYZE` unconditionally, `FULL` or not. Before this fix, `CLUSTER` on an 18 pinned reader fell through this early, friendly ERROR to the deeper `wal_insert_restricted` backstop (xlog.c, C5) and PANICked instead; the read-only guarantee held either way (the backstop still refuses the write), but the failure mode was worse |
| `MULTIXACT_OFFSETS_PER_PAGE`, members segment name width | offsets = `BLCKSZ / 8` (`MultiXactOffset` 64-bit); members uses 15-hex "long" segment names | offsets = `BLCKSZ / 4` (`MultiXactOffset` 32-bit); members uses the short (4-hex) name every other SLRU uses | `pagestore.c`'s `PG_MULTIXACT_MEMBERS_LONG_NAMES`-gated call sites in `pagestore_seed_multixact()`; `integration_test.sh`'s `opp`/`mbSeg` derivation | Guarded in this PR. This was a real bug, not just a portability gap: the hardcoded `true` seeded a branch's `pg_multixact/members` at a 15-hex path an 18 cluster never reads, and the seeder's own reference-comparison path looked under the wrong name too — caught by `integration_test.sh`'s byte-for-byte members assertion (`could not open file "pg_multixact/members/000000000000000"`) |
| Members-offset wraparound seeding | 64-bit `MultiXactOffset` space, "never wraps" in practice — no wraparound-aware seeding needed | 32-bit `MultiXactOffset` space, wraps in the same way `MultiXactId` does | `pagestore_seed_multixact()`, `pagestore.c` | Not guarded — no fix landed in this PR; see "Known limitations" below (limitation 2) |

`GetMultiXactInfo()` (`pagestore_slru.c`'s `ps_slru_tomb_horizon_cutoff()`)
is not in this table: no public API exposes `oldestMulti`/`oldestOffset`
before 19, and that could not be fixed from within `contrib/pagestore`
itself — it needed a small export from core. The C1-C7 core series' C4 slice
(SLRU read/write hooks, section 1 above) added exactly that export to
`multixact.c`/`multixact.h` on `branchdb_18-rc`, so `contrib/pagestore`'s
unconditional call compiles unmodified on both majors; nothing to guard
here.

**C4 export compatibility.** The PG18 C4 slice exports
`MultiXactIdToOffsetPage()` and `MXOffsetToMemberPage()` from
`access/multixact.h`. The original contrib compatibility definitions used
those same names as `static inline`, which made the combined build fail
with "static declaration follows non-static declaration". Contrib now
uses private `ps_multixact_offset_page()` and `ps_multixact_member_page()`
names for the unchanged PG18 formulas. On PG19 those names alias the
upstream inline helpers. This works with both stock PG18 headers and the
C4 exports, without changing the core patch series or the byte-identical
contrib requirement. `GetMultiXactInfo()` still requires its C4 export.

**Clean-startup SLRU capture clock.** PG18 can flush the pages trimmed by
`TrimCLOG()` / `TrimMultiXact()` before shared replay and redo pointers are
available. Clean startup skips `PerformWalRecovery()`, so sampling those
pointers used the bootstrap sentinel LSN `1` even on an existing store.
After pruning, that write is correctly refused and the mirror retry can
abort startup. `ps_slru_now_lsn()` now falls back to the already-read
shutdown checkpoint's record end before using the bootstrap sentinel.
This preserves a real WAL bound without relabeling staged bytes at drain
time. The integration suite's clean restart after mirror-debt reset
reproduces the original refusal and exercises the corrected clock.

## Known limitations for 18 (not fixed by this PR)

One forward compatibility gap remains:

1. **`dataChecksumState` is a forward, not an 18, risk.** Guarded correctly
   for today's matrix (row above: absent from both stock 18 and
   `branchdb_18-rc`, so the comparison and printf field are `#if`'d out on
   18 and kept on `pagestore`'s current base), but the field is only present
   on `pagestore`'s base because it predates upstream's revert of online
   data checksums. Once P0 merges `REL_19_STABLE` into `pagestore`, the
   field disappears there too — the same revert `datachecksum_state.c` (C5,
   section 1) already tracks — and this guard's `#if PG_VERSION_NUM >=
   190000` will need to become unconditionally absent rather than
   version-gated. Not something to fix speculatively now; P0's own merge is
   where it gets resolved for real.
### PG18 members-offset wraparound

Branch seeding accepts member horizons in the PG18 32-bit offset space and
splits a wrapped horizon into the final pages and page zero onwards. An exact
wrap to next offset zero creates a zero bootstrap page, since the next
allocation has not initialized that page yet. All three validation paths
(direct seeding, composed SLRU seeding and checkpoint-derived horizons) share
the same rule and reject values outside the PG18 offset space. PG19 retains
its ordered 64-bit horizons.

Both single-page and batched members reconstruction now apply the PG18 WAL
record's circular segment deletion range, matching REL_18_6's
`PerformMembersTruncation()`. The previous monotonic-cutoff test could mark
unrelated low segments deleted and retain deleted high segments across wrap.
PG19 still uses its single monotonic cutoff.

The standalone `pagestore_multixact_horizon` Meson test covers wrap to zero,
page boundaries, the partial final page, SQL bigint bounds, and all start/end
pairs against a literal 17-segment circular deletion walk. The modified
extension compiles against both PG18 and PG19 generated headers; the boundary
test also passes with address and undefined-behavior sanitizers. A fresh PG18
extension, daemon and restore/import tools passed `integration_test.sh` against
an isolated PG18 installation, including ordinary multixact reconstruction,
SLRU seeding, branch preparation and restart. This evidence does not replace
a retained full branch-boot run on a cluster whose live multixact member
offsets cross the wrap point.

Process for adding a guard here: confirm the exact 18 shape against
`upstream/REL_18_STABLE` (`git show upstream/REL_18_STABLE:<path>`, the same
way the `multixact_internal.h` and `T_ClusterStmt` guards were verified —
do not guess), add the `#if PG_VERSION_NUM` guard on `pagestore`, update
this table, and re-run a real compile against `REL_18_6` (ideally with
`branchdb_18-rc`, so the compile reaches these sites instead of failing
earlier on a missing core hook).

### Promotion evidence for the core series

Before marking a major candidate/released, the authoritative pagestore manifest
must record `core_series`: keys C1-C7 each map to `{"commit": "<full approved port SHA>",
"sources": ["<full landed pagestore source SHA>", ...]}`. These pins identify
the reviewed per-major squash/conflict resolution; raw patch equality across
majors is not required. Verification requires actual PostgreSQL core/build
changes, requires source citations in the port/cherry-pick commit message,
checks source ancestry on pagestore, and compares each release series
commit to its approved pin. Update these pins with the base/source fields after
a deliberate minor rebase. Planned entries need no such release evidence.

The unfiltered release-provenance workflow validates the actual PR head with
complete history. A first planned sync is reviewable before promotion: its
upstream base must be a real release tag, its mechanical sync records the full
landed source SHA, and its core ports cite landed core sources. This staged
validation does not promote the manifest or qualify candidate acceptance. After
the PR lands, record its approved pins and promote metadata on pagestore.
Unsupported reference majors skip PR release evidence, never candidate acceptance.

PR review evidence also covers proposed minor/source updates to already-promoted
branches. It checks the proposed commit's real upstream tag, mechanical sync
and landed core sources without requiring post-landing metadata to exist first.
Frozen candidate acceptance and weekly release qualification still require the
approved authoritative base/source/core pins; metadata transitions never become
qualified merely because the unmerged PR is structurally reviewable.

The release workflow binds PR validation to the target branch's major, then
extracts validator/regression code from the freshly fetched authoritative
pagestore revision into a temporary directory. Candidate acceptance uses the
same trusted bootstrap. Proposed release code cannot replace its own checker.
Every commit changing contrib/pagestore must be a pure mechanical sync with a
full landed source SHA and an identical tree at that commit, not just at HEAD.
Legacy prototype history with direct contrib edits requires a clean release
cut before qualification; a later tree restore does not attest earlier edits.
