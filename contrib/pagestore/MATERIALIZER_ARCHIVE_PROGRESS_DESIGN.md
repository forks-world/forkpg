# Materializer archive progress: implemented scope and limits

Status: **experimental partial implementation; not a general deadlock fix**.
The current worktree implements the v4 durable progress record and localsvc
START, SAFE, FREEZE, PREPARE, ACTIVATE, and authorized-SNAPSHOT operations.
It can extend the writer's archive boundary only for a fixed, CRC-verified
completed checkpoint whose containing segment fits the per-timeline cap. A
standalone writer background worker can seal that already prepared candidate
without blocking in the archive callback. It does not create a checkpoint or
advance an arbitrary WAL gap.

## Failure addressed

In the 2026-11-13 endurance run, the materializer rejected a checkpoint while
recovery still had unresolved invalid-page references. Its existing fixed
archive allowance was exhausted before a later completed writer checkpoint
could be replayed. Recovery then waited for WAL that the writer would not
archive. This progress path is intended to cover the bounded case where
recovery has since passed PostgreSQL's consistency and invalid-page checks and
the writer has a suitable completed checkpoint within the frozen extension
cap.

## Current protocol

The daemon stores one v4 progress record per timeline. START records a
monotonic startup attempt and fresh nonce, but carries no writer archive
budget. Exact nonce retries are idempotent. A different nonce that has
advanced the attempt fences the old startup, which must be restarted by its
supervisor. The nonce is an idempotency token, not caller authentication.

SAFE is sent only after PostgreSQL reaches recovery consistency and
`XLogCheckInvalidPages()` succeeds with invalid-page bypass disabled. Its LSN
is fixed for that startup attempt and retried with bounded RPC timeouts while
WAL is unavailable. The recovery wait loop periodically retries the same
feedback, so a transient daemon outage at EOF does not suppress all later
attempts. Failure to publish progress does not prevent PostgreSQL from
announcing consistency or serving read-only traffic.

At the writer's existing archive boundary, the writer freezes its own policy
once for the marker. `original_limit` is the old computed archive boundary,
including any existing rejected-checkpoint allowance exactly once;
`max_extra` is the separate `pagestore.materializer_progress_extra_segments`
allowance (default eight WAL segments). Retries and configuration reloads
reuse the durable values. A marker advance clears the old frozen budget and
target; a same-marker attempt restart keeps an activated grant but clears SAFE
feedback and any inactive candidate. Setting the GUC to zero disables both
new progress work and consumption of an existing grant; re-enabling it reuses
the persisted per-marker values.

The writer checks its CRC-valid `pg_control` checkpoint against the local WAL
record, obtains the full record end, computes the exclusive end of the segment
containing that record, checks the timeline, and PREPAREs the fixed candidate
under the daemon marker-authority lease. PREPARE persists a candidate but does
not authorize archive. A standalone writer background worker observes pending
candidates, rechecks the candidate tuple under authority, and checks the local
flush and exact WAL insertion pointers. If the target segment is not yet
sealed, it requests a conditional switch that rechecks the expected segment
while holding the WAL insertion locks. Once the matching timeline's durable
flush pointer reaches the segment end, the worker ACTIVATEs the candidate
under the marker-authority lease. Archive authorization only raises the old
limit to the persisted `grant_end` for that same marker, incarnation, attempt,
SAFE, frozen budget, WAL timeline, and active grant. Repeated polls cannot
ratchet the target or budget. The archive callback itself does not wait for a
checkpoint or switch.

Progress records use format v4. v1/v2/v3 and unknown/corrupt records are
rejected fail-closed; there is no automatic migration. The daemon emits a
rate-limited diagnostic for rejected operations and unsupported stored
formats.

## Trust and lock assumptions

The current localsvc shared-memory endpoint is protected by the existing
same-OS-UID trust boundary. Any process with that UID can issue the progress
operations and can modify the local store; the nonce and role fields do not
authenticate a PostgreSQL writer or materializer. This implementation does
not provide isolation between clients sharing that UID. Marker receipts are
obtained through the daemon's serialized marker-authority path and durable
store sync; absent authority, stale incarnation, or marker mismatch fails
closed. Only the POSIX provider has been exercised for this progress state.

## Explicit limits and unresolved requirements

* The extension cap is per timeline. There is **no global cumulative
  outstanding-grant budget across timelines** in this implementation. A
  deployment with multiple timelines can therefore reserve/ship the sum of
their individual caps. Global admission/backpressure is required before
claiming the design's cumulative-space bound.
* PREPARE only selects the current completed checkpoint. If no suitable
  checkpoint exists, the candidate is beyond the frozen cap, the candidate
  belongs to another WAL timeline, or the worker cannot prove and seal its
  segment, the writer remains fail-closed with a blocked/deferred diagnostic.
  It does not request a checkpoint, and it does not keep issuing switches from
  an archive retry loop.
* The segment check and conditional switch prevent the observed stale-pointer
  race from sealing a later segment. A finite cap still cannot guarantee
  progress through an arbitrarily large WAL gap. There is no upstream write
  backpressure policy here; over-cap candidates remain blocked and require
  operator/supervisor action or a separately designed controller.
* A progress grant is not a restartpoint, retention receipt, marker update, or
  permission to reclaim WAL. Those remain governed by PostgreSQL's completed
  and synchronously durable restartpoint path.
* A strict reproduction against a fresh run-1 clone passed in a private mount
  namespace. Before startup it asserted all six `35.ready` through `3A.ready`
  files, shipped WAL end `3/35000000`, materializer marker `3/2CFDF818`, and
  release checkpoint `3/31000060`. With progress GUC zero, archive file 35
  remained blocked. After recovery published SAFE at `3/34FFF960`, reloading
  the private writer with an eight-segment cap caused durable FREEZE and
  PREPARE for checkpoint record `3/3B1D9618`, then the independent worker
  activated through `3/3C000000` without a manual switch. Archive files 35
  through 3B completed and the materializer replayed past the former wait
  point. A writer/materializer restart preserved the same budget and grant,
  advanced the attempt, republished SAFE, replayed through `3/3C000000`, and
  advanced the durable materializer marker to `3/35FDDC48` after a completed
  restartpoint. The original run-1 store was not modified, and the host mount
  view was unchanged after each private namespace exited.
* Unit, storage, daemon IPC/restart, and full build checks pass. The
  `test_custom_rmgrs` TAP test captures segment N, switches and writes in
  N+1, then calls the conditional API with stale N. It verifies the API
  returns an invalid LSN and does not advance the current segment. A
  simultaneous competing-backend switch race test remains
  outstanding.

Accordingly, this implementation must not be described as satisfying the
design's global cumulative bound or as generally resolving the observed
deadlock. Any release/PR review should require explicit acceptance of these
limits or further implementation and end-to-end evidence.
