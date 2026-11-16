# Bounded control-history pruning investigation

## Reproducer

Run `pagestore_wal_reclaim_core_test --many-control-layers` from the Meson
build. It creates three WAL segments and 70 checkpoint triplets in 210 image
layers, reopens the store, sets a two-record test-only legacy gathering limit,
and gives maintenance at least two real retry windows. The oldest note's redo
is inside segment zero; later checkpoint redo positions advance with shipped
and indexed WAL. The test expects the old note to be retired and at least one
segment to be reclaimed.

The reproducer now writes all three checkpoint blocks. The bounded control
planner retires obsolete groups without exceeding the whole-timeline gather
budget: in the fixture, 70 notes become 3 and all three WAL segments are
reclaimed. The pinned variant retains its original note and WAL. Both are
registered as tests, including a reopen after maintenance.

An experimental read-only snapshot now projects the global note decisions by
LSN and admission sequence. Unknown identities are retained, and conflicting
decisions fail closed. This projection alone has no authority to prune. A 28,209-note
synthetic case compares sampled decisions against the original linear policy;
the global chain lookup uses its existing sort order to avoid a quadratic scan.

`ps_compaction_identity_covered` is a separate, read-only physical coverage
check: given a caller-supplied complete immutable layer snapshot, it reads all
indexes to completion and accepts one candidate only when at least one copy
is selected and no copy is in an excluded layer. Tests cover overlapping
copies, same-LSN distinct admission sequences, unknown identities, duplicate
descriptors, unsupported layers and corrupt excluded indexes. The batched
coverage scan sorts candidate identities and matches them during one traversal
of the supplied indexes; it exposes results only after every index succeeds.
It uses a staged cursor that reads only the footer when opened and at most one
64 KiB chunk per next call, checking the checksum and ordering before results
become available. Provider latency is not bounded. An experimental binding
copies the layer descriptors and, under the caller's locks, checks the complete
relevant layer map and retention, timeline, admission, incarnation and floor
fences before and after scanning. Every layer must stay pinned; unsupported or
legacy cross-shard layers fail closed. A nonzero reader count does not prove
the caller owns the pin. This is a pre-publication check, not publication or
retirement authority. Memory and pin count grow with the number of layers, so
production resource impact remains to be measured.

`pagestore_wal_reclaim_core_test --control-coverage` exercises an integrated
read-only preflight on 70 checkpoints and over 200 real layers. It captures the
global note decisions and fences while locked, pins and scans the complete
layer snapshot, then revalidates before reporting covered candidates. The test
checks full versus partial layer selection and installs a history pin while the
scan is unlocked to reject stale results. This path is diagnostic only: it
does not participate in maintenance and cannot retire notes or reclaim WAL.
The paired planner only considers an LSN when image, note and admission
fence all exist, every known version is durable and globally disposable, and
the selected layers cover every candidate identity's physical copies. Missing
or in-flight members retain the group. This conservative rule can retain more
than the existing full planner. Strict group scanning also rejects an unknown
physical image, note or admission-fence identity at a candidate LSN, in both
selected and excluded layers. Maintenance discovers the physical source layers
of the oldest eligible group, verifies that every retained identity keeps its
highest-layer winner, independently checks the staged outputs, revalidates
all captured epochs and the layer map, and retires the source set with one
checksummed manifest event. Source data checksums are reused across output
batches and verified again before publication.
The event is preceded by a durable guard that makes older readers reject the
manifest instead of truncating its tail. Crash tests cover the frontier,
publication and source-retirement boundaries, and branch tests cover retained
exact-redo twins. The production entry supports providers without an object
upload interface and the bundled provider when object tiering is disabled.
Providers with active or uncoordinated uploads fail closed until their upload
lifetimes participate in the same transaction.

Old reader-snapshot and SLRU versions can also hold the earliest artifact
fence and indirectly prevent control-history and WAL reclamation. Maintenance
plans bounded removal using the existing artifact lifecycle and branch-view
rules, and publishes it through the same coverage and atomic retirement path.
Reader-pin, branch, restart and three publication crash boundaries are tested.
The retained stress snapshot confirms old SLRU versions can be removed; the
time needed to clear the entire historical backlog remains under measurement.

The companion `--many-control-layers-pinned` scenario places a WAL and page
history pin at the original checkpoint. It must retain the old note and all
three segments; it currently passes. Both scenarios also evaluate the existing
global control-chain plan without publishing: it keeps 3 of 70 notes in the
unpinned case and all 70 when the old view is pinned. Any implementation must
make the unpinned scenario pass while retaining this protection.

The large retained stress snapshot similarly had 28,209 control-note versions,
24,964 image layers and 101,888 immutable WAL segments. A durable old note with
version and redo `0/2A00EFA8` pinned the root WAL floor. With the separate
fail-closed early-yield experiment, maintenance ran three bounded pruning
passes on other records while the old note and WAL floor stayed unchanged.

## Required proof for a bounded control-pruning implementation

The existing relation-only bounded planner must not simply classify control
records as relation pages. A safe implementation needs an explicit global
control-chain plan across selected and unselected live layers, including:

* the control image, redo note and admission-fence pairing, including exact-LSN
  retries and sequence-position closure;
* independent control blocks such as the materializer marker;
* the newest visible identity for every retained WAL/page fence, descendant
  branch cap and finite admission-sequence view, plus in-flight images/notes;
* a complete proof that any selected record to be dropped is superseded even
  when the superseding record lives in an unselected layer;
* the existing epoch, incarnation, admission, manifest and input/output fences,
  revalidated before publication and again before retiring inputs;
* atomic crash recovery across publication, index updates, input retirement and
  frontier durability. A missing/corrupt/changed proof must keep the record.

The bounded operation must retain all unrelated data and stay provider-neutral:
an optional provider-specific implementation can fail closed, but must not be
treated as a proof for another provider. A practical implementation can snapshot
the existing global `control_chain_plan` under its required locks, stage a
bounded set of selected layers outside the locks, and revalidate the complete
plan and retained fences before publishing. The existing disjoint-range proof
alone is insufficient for an overlapping control chain.

Tests need the opt-in reproducer above, different protected branch/pin horizons,
same-LSN checkpoint retries, in-flight notes/images, independent marker blocks,
corruption, concurrent pin/branch/writer changes and crash points at each
publication and retirement boundary. Verify the exact protected views and WAL
floor before and after restart, then demonstrate actual WAL prefix reclamation.
