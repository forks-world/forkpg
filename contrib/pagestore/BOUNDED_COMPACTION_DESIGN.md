# Bounded image compaction: publication contract

## Space-bounded key-range compaction (proposed)

Initial local integration selects a bounded overlap-closed input set when the
complete set does not fit the existing merge limit. It preserves every selected
version, verifies output coverage and revalidates isolation at publication and
retirement. With a nonzero history floor it first requires the existing plan
to prove every selected version is kept. Selection is work-limited and local
only; an ineligible group may still fall back to the legacy compactor.
The local pruning path also accepts an overlap-closed subset when all excluded
layers are provably disjoint. It retains the existing retention, admission,
frontier and source-pin fences at publication, page-index update and retirement.
It does not yet bound pruning when excluded layers overlap, convert oversized
sources, or enforce a store-wide disk limit.
If no isolated component is found, a local all-versions-preserving merge can
also select a bounded newest-ID suffix. Overlapping omitted layers must all
be older than every selected input; newly arriving overlapping layers must be
newer than every reserved output. Full coverage verification and publication
revalidation preserve the winner for equal identities.
Discovery examines at most 1024 candidate seeds and charges layer visits and
range comparisons against a 100000-step budget while holding the map lock.
If the all-versions search exhausts its budget, it defers without a legacy
rewrite and resumes from a process-local per-timeline cursor on the next
maintenance turn. The pruning search has an independent cursor; if it defers,
the all-versions path can still make progress in the same maintenance turn.
A seed that itself exceeds a whole turn's budget is skipped in that pass to
ensure progress; layer-map changes can also make a cursor miss a candidate.
Neither proves the skipped component is ineligible. Once a pass ends without a
candidate, or if the initial legacy-layer scan cannot complete, the caller can
still use the legacy path. A policy for avoiding unbounded fallback in those
cases remains separate work. Restart discards the cursor and starts discovery
again; it never publishes or retires layers as part of discovery.

Neon's pageserver partitions immutable layers by key range and compacts selected
ranges; see its [storage design](https://github.com/neondatabase/neon/blob/main/docs/pageserver-storage.md).
Apply that idea to local pagestore without assuming that object storage exists.
The existing 1024-record output batches alone do not bound peak disk use: all
input layers remain live until every required replacement is durable and covered.
In particular, a legacy single layer can already be many GiB, so writing many
small outputs before retiring it still duplicates that layer's live bytes.

The first incremental step is an all-versions-preserving compaction of complete
layers whose key/block intervals are isolated from every excluded live layer in
the same timeline and shard. Select inputs under both a byte budget and a fan-in
limit; an unknown, legacy, remote or overlapping descriptor is ineligible. The
range test must be conservative for descriptors whose boundaries do not prove
disjointness, including equal keys and block endpoints. Reserve output IDs,
pin the input descriptors, prepare and verify the winning records and bytes,
then revalidate the selected inputs, excluded-layer disjointness or suffix
precedence, timeline, and provider state at each publication and source
retirement. Newly admitted layers are safe only where their precedence over
the reserved outputs and their coverage are proven. Never infer safety merely
from the number of layers: replacement IDs sort after existing layers, so an
omitted overlapping layer between selected inputs and outputs can otherwise
lose a tied record to an older input's replacement.

This step can compact isolated ranges with bounded temporary output; it cannot
split or retire part of an existing oversized source. Handling such sources
requires a separately crash-safe partition conversion or a provider-specific
copy-on-write technique, with an explicit peak-space budget. Do not claim a
fixed store-wide bound: live data, retained branches and WAL, concurrent
admissions, failed cleanup, and source conversion require additional headroom.
Neon's local eviction relies on remote durability; local-only stores must not
discard their sole durable copy to satisfy a disk target.

For a distributed edition, remote-durable layers can additionally support a
bounded local cache with upload verification, safe eviction and read-through.
Those placement decisions must not change the range-compaction safety proof.
Coordination between multiple pagestore nodes, including remote publication
and GC authority, remains a separate design; the local lock is not such a
protocol, and this design does not require a particular consensus service.

Before integration, exercise overlapping and touching key/block boundaries,
tied identities, omitted older layers, newer arrivals, changed descriptors,
finite and ancestor views, reader pins, failed cleanup, and crashes after each
output publication and source retirement. Check reads before and after reopen,
measure peak allocated and filesystem-free space under a fixed workload, and
verify that rejected or oversized plans do not silently fall back to an
unbounded rewrite. Range partitioning is not a multi-node synchronization
protocol; the existing local GC serialization and publication fencing remain
required.

Status: design; the implementation remains in progress. This builds on the
staged local output work in PR #371. It does not describe behavior that is
already implemented.

The index visitor added with this design is a bounded-memory building block;
it is not yet used by compaction and does not implement the merge or publication
protocol. Its callbacks must not commit effects until the visitor returns success.
The pull cursor and index merge are also building blocks: the merge limits
fan-in to 64 and emits equal identities in ascending layer-ID order. The caller
must consume it to successful completion before publishing staged output. Stores
with more inputs require a separate bounded multi-pass plan.
Intermediate runs must retain each record's original source-layer precedence;
assigning temporary runs new layer IDs and then using those IDs as tie breakers
would change the winner for equal legacy identities. This provenance is merge
metadata, not permission to make an intermediate run visible in the manifest.
The sorted-batch writer bounds each output to 1024 records and seals it without
publishing it. It is a primitive, not a transaction: it does not bind multiple
outputs to their sources, persist merge progress, or make source deletion safe.
The preparation helper connects a single merge pass to up to 64 unpublished
outputs and reports cleanup ambiguity. It is not used by compaction, cannot
process an input requiring more than 64 output batches, and cannot publish
results safely across a crash.
Within that single pass it collapses exact duplicate identities to the final
source in read-precedence order before cutting an output batch. This ensures
that one identity cannot be split into an older output and a newer source;
it does not validate concurrent arrivals, make outputs atomic, or authorize
source deletion.
The coverage checker compares every winning source identity, recovery metadata
and page bytes against all prepared outputs with bounded memory. Its success is
only a precondition: caller-owned pins, state validation, durability, publication
and source retirement are still required. It currently verifies local sources
and outputs only and supports at most 64 of each.
Both preparation and coverage verification reject legacy shard-zero descriptors:
their keys may belong to multiple shards, and ordinary shard-local output
would not preserve the legacy routing contract.

## Boundaries

A batch must bound both the data it reads/writes and the metadata it holds in
memory. Limiting the number of input layers is insufficient: one image layer
can be many GiB. Likewise, producing several output files after loading and
sorting every input index is not a bounded compaction. The implementation must
use a bounded-memory merge over input indexes and cap output by bytes or
records. Tests must include an individual source larger than the output bound.
Older image indexes may be unsorted. The visitor preserves on-disk order; a
future merge must detect unsorted inputs and use a bounded external sort or fail
closed, rather than assume every source can be fed into a sorted merge.

## Preserve version precedence

For a key, block and exact (LSN, admission sequence), reads resolve a tie in
favor of the greatest layer ID. Legacy sequence-zero records with equal
identities can contain different page bytes. A new output has a later layer ID,
so copying an older source while a later source with the same identity remains
outside the batch can change the result. The planner must resolve the winning
copy across all relevant sources before publishing that identity; grouping all
copies of an identity at a batch boundary is one possible approach. Concurrent
new layers and legacy shard-zero inputs must also obey the existing ordering.
For example, publishing a first batch containing only an older copy of an
identity can make it outrank a newer copy still present in an input layer.
Therefore merely publishing output files in sorted order is not sufficient.

The first incremental merge must preserve every logical version. A page-history
frontier must not advance and page-index entries must not be removed merely
because that merge made a new physical copy. Pruning requires a separate proof
against the complete relevant history and current retention and branch fences.

## Publication and recovery

Pin a stable input set, capture its identity and durability requirements, then
write bounded output batches without holding the shard and map locks. Publish
only data that has been synced. All input layers remain readable until the
complete replacement covers the entire input set; a partial output cannot
justify marking any input deleting unless its coverage is independently proven.

Persist enough information to distinguish an incomplete replacement from a
committed one. Recovery must safely handle a crash before and after every output
file, publication record, commit record and input tombstone. Uncommitted files
must be recoverable or reclaimable without treating an incomplete manifest or an
ambiguous durability result as permission to delete data. A committed replacement
must remain valid if cleanup is interrupted. Reader pins protect both local and
remote copies until in-flight readers finish.

Compatibility constraint: the existing v3 manifest replay can classify an
unknown final record as a torn tail and truncate it. A new manifest event or
version alone therefore does not make old binaries fail closed. Do not retire
inputs using such a record until an independently enforced upgrade/downgrade
barrier exists. An alternative to investigate is existing ADD and MARK_DELETE
records after exact-identity winners are selected, keeping all sources until
every output ADD is durable. Its crash and concurrent-write properties still
require proof and tests. The single-batch ADD helper checks snapshot identity
and publication order, but does not itself prove coverage, acquire fences, or
retire inputs. Process-exit tests cover partial output and source retirement,
and use file-size limits to force torn ADD and MARK_DELETE writes. The
production orchestration still needs dedicated concurrency and recovery tests.

All output IDs must be reserved under the shard allocator before the snapshot
is released, so a later layer cannot reuse an output ID or receive an earlier
ID. A map snapshot check can reject newly added sources and changed descriptors,
but it cannot reserve IDs, pin files, or replace lifecycle and writer fencing.

If an input is remote-durable, the replacement must become durable in the same
tier before the old remote copy can be retired. Retry and cleanup of failed or
abandoned uploads need explicit durable ownership; otherwise repeated conflicts
can leak object storage.

## Progress and tests

Ordinary writes should not force a full rewrite of the same large input forever.
Use a finite input snapshot and admit new layers separately, then validate the
snapshot and the replacement at publication. A stale snapshot must fail closed,
with bounded retry and cleanup; fallback to a full locked merge is not a bound.
The scheduler must make progress across timelines, including one with a single
large layer or a stream of new writes. A successful data-preserving merge does
not by itself prove that historical data can be reclaimed.

Tests must exercise duplicate legacy identities across the batch boundary,
finite-sequence readers and branches, readers pinned during publication, writes
and retention changes throughout the merge, remote durability changes, and
crashes at every publication boundary. Measure maximum lock hold and temporary
space under sustained writes, not just eventual completion after writes stop.
