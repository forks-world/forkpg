# Replay stall investigation, 2026-11-17

The 72-hour endurance run stopped after 13 hours 50 minutes in round 165:
materializer startup exceeded the 300-second `pg_ctl` timeout before reaching
consistent recovery. The preserved run is
`/home/hurricane/ps72p-20261116/runs/run-1`; its `FAILURE.json`, logs,
`metrics.jsonl`, and `diagnostics/074100-*` contain the evidence. These paths
are local to the investigation machine. The separate copy-on-write reproduction
is under `/home/hurricane/ps72-replay-diag`.

## 1. Control-history planning blocks foreground requests

In the failed run, startup began redo at `A/A0B94400`, restored segment A1,
and did not reach consistent recovery before the timeout. On an isolated copy
of the retained store and materializer data, a freshly started daemon without
the original concurrent workload reached consistent recovery from the same
redo point in under one second. This shows the WAL itself does not invariably
take five minutes to replay; it does not by itself identify the original
run's exact lock holder.

During a subsequent isolated startup, two daemon stack samples showed the
foreground shard worker waiting for the shard write lock while maintenance held
the locks in `compact_control_pairs_bounded` and ran `control_chain_plan` ->
`control_note_redo` -> `layer_map_lookup_impl`. The store contained about
18,569 image layers and the plan had 1,903 history fences. The current bounded
maintenance entry holds all shard locks, the prune lock and map write lock
while computing this plan. Repeated layer scans under those locks can delay
foreground page operations and recovery even though later staging runs unlocked.
An interval index for note-bearing layers removes repeated full-map searches,
but a subsequent sample still found the foreground blocked while maintenance
read and checksummed a layer index under the shard lock. The bounded control
path now snapshots and pins note layers, reads note data without the shard and
map locks, and validates the page, timeline, admission, retention and layer
snapshot before planning. A concurrent segment read requires a provider opt-in
and a stable segment change token; unsupported reads defer rather than assume
a filesystem. Snapshot-race tests cover new pins and writes.

With the retained data on an isolated copy, a sample found maintenance reading
the layer index in the unlocked prefetch while the shard worker remained free;
three materializer immediate restarts completed in approximately 0.31 s each.
This is evidence for that reproduced stall, not proof that every other locked
history path or the 72-hour workload is free of long stalls. A separate
sample previously found another locked `layer_map_lookup_impl` call.

The endurance driver's `replay_s` is elapsed time waiting for replay to reach
an LSN, not CPU time spent applying WAL. It can include waiting on pagestore
requests and WAL availability. The run also reported archive waits over 900 s;
their complete causal relationship to this lock stall remains to be measured.

Follow-up: snapshot the needed history and layer identities while protected,
perform expensive planning outside the foreground locks, and revalidate all
retention, admission, timeline, incarnation, layer-map and page-index fences
before publication. Add a reproducible large-history test that measures
foreground request latency while pruning, alongside existing crash, pin,
branch and concurrency correctness tests. Compare replay and archive timing
on the retained workload before and after the change.

## 2. Immediate compute termination leaves claimed channels behind

After an immediate shutdown of the isolated materializer, no materializer
process remained, but two channels still had `claimed=1`, `state=DONE`, and
opcode `PS_OP_WRITEV`; another was marked abandoned. This is distinct from
the explicitly reclaimable abandoned state. During the original run the
aggregate inspector reported 125 of 128 channels claimed at failure, with only
about 27 processes in the sampled process trees. The aggregate metric does not
distinguish owned and abandoned channels, so it does not prove how many of the
125 were leaked or that channel exhaustion caused the startup timeout.

Normal `ls_detach` releases a channel on process exit, but abrupt process death
can bypass exit callbacks. A timeout or caught interruption marks a channel
abandoned, which can be reused only after its request has completed; an owned
channel left by a dead process has no equivalent safe reclamation in the
current protocol.

Follow-up: reproduce repeated immediate shutdowns and killed backends in a
small deterministic fixture, distinguishing live owners, dead owners and
abandoned in-flight requests. Design ownership and reclamation so that a dead
owner can be identified without PID-reuse races and a channel is never reused
while the daemon can still complete its prior request. Test owner death at
claim, request publication, daemon processing and completion boundaries, and
verify that a live or in-flight owner is never reclaimed.

### Ownership ABA hazard

A PID alone is not a safe channel ownership token, even when the claimant and
daemon share a verified PID namespace. Consider a reclaimer that observes PID
X absent and pauses before its CAS: another reclaimer can acquire and release
the slot, and the kernel can reuse X for a new live owner of that same slot.
The first CAS then sees the same PID and can steal the new owner's channel.
The prototype using only PID values was therefore discarded. The current
protocol uses an atomically compared 64-bit generation/PID token; the daemon
publishes its PID namespace identity, and the claimant verifies its own
namespace before attempting dead-owner recovery. Recovery also requires IDLE
or DONE, so an in-flight daemon request cannot be stolen. Generation exhaustion,
unavailable namespace information and non-lock-free 64-bit atomics fall back
to non-reclaimable legacy ownership. A child inherited from a fork detaches
without releasing its parent's channel. Unit and real-importer tests cover a
live owner, killed owner, in-flight request, PID-reuse interleaving, namespace
mismatch and generation exhaustion. Unknown/legacy claims remain conservative.
