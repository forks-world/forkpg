# Bounded image compaction: publication contract

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
