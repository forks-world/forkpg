# Materializer / WAL archive progress protocol — proposed design

Status: **design for review; not implemented**. This document describes a
candidate protocol, its safety boundaries, and decisions that must be settled
before implementation. It does not change the current archive policy.

## Failure observed

In the 2026-11-13 endurance run, the materializer rejected checkpoint
`3/31000060` because recovery had unresolved invalid-page references. The
persisted materializer marker was `3/2CFDF818`. Recovery subsequently reached
consistency and replayed to `3/34FFF960`, but the archiver had exhausted its
fixed two-segment recovery allowance at `3/35000000`. Each side then waited for
the other: the materializer needed a later checkpoint to make a restartpoint,
while the archiver needed the marker to advance. The next complete writer
checkpoint record in the retained WAL was `3/35A06D10`. It is a candidate,
not proof of a safe restartpoint.

The existing rejected-checkpoint hint and fixed allowance deliberately do not
ratchet on archive retries. Retaining that property matters: blindly raising
or repeatedly extending a limit can consume unbounded storage when a
materializer is unavailable. A larger fixed allowance also cannot guarantee that
a checkpoint or the WAL resolving invalid-page references lies within it.

## Goals and non-goals

* Break the circular wait when the materializer has recovered enough to seek a
  later safe restartpoint, without relying on a particular filesystem or
  direct access to the writer's `pg_wal`.
* Keep the durable materialization marker and all retention/reclamation fences
  tied to an actual completed restartpoint, never to a progress hint.
* Do not turn repeated polls, archiver restarts, stale hints or writer
  checkpoints into automatically increasing archive grants.
* Bound both each grant and the outstanding extra WAL. If the bound is
  exhausted or progress cannot be demonstrated, surface a distinct blocked
  condition instead of claiming the protocol guarantees unconditional progress.
* This design does not establish an absolute bound on disk usage independent of
  writer-local WAL, concurrent writes and retention owners. Those require
  explicit admission/backpressure policies and operational disk monitoring.

## Participants and transport

The writer archiver and materializer startup process are separate processes,
usually in separate PostgreSQL instances. Both use the existing localsvc
protocol to communicate with the pagestore daemon. Control records in the
store, with validated version, timeline and identity fields, are one possible
durable transport; no shared process memory, POSIX locking, path, or filesystem
semantics are assumed. A message is advisory and is never a retention receipt.

A generation must identify **this actual startup-process attempt**, not just
the retention-owner generation: inspect and define how a unique attempt is
allocated, authenticated/fenced, invalidated on every recovery restart, and
observed by the archiver. Existing retention generation may survive a worker
restart and therefore must not be assumed to suffice. If no authoritative
attempt identity can be obtained and checked atomically, this protocol must
fail closed; an unchecked PID or timestamp is not a substitute.

## Candidate state machine

For a timeline, durable marker `M` and recovery attempt `G`:

1. **Normal.** Existing limits and the pinned release checkpoint remain in
   force. A rejected restartpoint records its existing advisory hint.
2. **Recovery progress.** Once PostgreSQL has actually reached recovery
   consistency with no unresolved invalid-page references, recovery may
   publish `(timeline, M, G, safe_replay_lsn)` to the daemon. A successful
   consistency check is evidence for this recovery attempt only, not a
   checkpoint, a restartpoint, or evidence that pages are durable. If recovery
   can become unsafe again, invalidate the hint before it can authorize more
   WAL. A hint must not be published when invalid-page checks were bypassed.
3. **Fixed grant.** After validating the hint and current identity, the
   archiver selects a completed writer checkpoint strictly after the reported
   safe replay point and pins `(timeline, M, G, safe_replay_lsn, target)` in
   durable control state before shipping beyond the existing boundary. Round
   to cover the complete target record/segment. The target remains fixed on
   repeated requests, process restarts and newer writer checkpoints. A target
   is merely a candidate: only PostgreSQL recovery determines if it is safe.
   If no suitable checkpoint exists, request one through a defined writer-side
   mechanism; do not assume a checkpoint will appear while the writer is idle.
4. **Consume.** The archiver ships through the fixed target, subject to a
   separately specified maximum outstanding grant and existing WAL-index and
   resource constraints. It does not advance `M`. The materializer replays
   the WAL and PostgreSQL decides whether a restartpoint can be made.
5. **Complete or blocked.** A completed restartpoint advances the durable
   marker using the existing durability ordering and invalidates the grant.
   If the target is rejected, no target exists within the configured bound,
   the materializer fails, or any identity/record cannot be validated, stop
   at the bound and expose the reason. A further grant, if supported, must be
   tied to newly verified recovery progress and a separately bounded budget;
   repeated rejection must not silently turn this into unlimited growth.

The writer checkpoint selected must be a **completed checkpoint record**, not
just its redo start. It may be in an incomplete WAL segment: specify how the
writer produces/switches the segment without inducing uncontrolled checkpoint
or WAL generation. Segment math must handle overflow and an already partially
shipped segment. Existing durable prefix and WAL-index invariants still apply.

## Concurrency, crash and downgrade requirements

* The authority must serialize attempt changes and fixed-target installation,
  or offer an atomic compare-and-set/fencing operation. Before granting, check
  the identity and marker against that same authority; separate unchecked
  reads admit a restart-between-check-and-use race.
* A process crash after target publication but before shipping must reuse the
  exact target. A crash during publication must yield either a validated whole
  record or no grant; torn, missing, stale or unknown-version records fail
  closed. The implementation must define the publication and sync ordering.
* If an attempt dies after a grant was committed, whether that fixed grant may
  finish must be explicit: permitting it can ship at most the already durable
  fixed bound, but cannot authorize another grant. It is not evidence that the
  replacement attempt is safe. Replacing it requires fenced new feedback.
* New state must not overwrite unrelated control blocks, change versioned
  layouts implicitly, or be read as valid by older binaries. Define on-disk
  artifact versions, mixed-version behavior, upgrade/downgrade and existing
  fixture identities before writing code. Unknown feedback must not extend a
  limit. Preserve existing provider and retention semantics.
* Polling and notification are optimizations; correctness must hold under
  dropped, duplicated, reordered or delayed responses and independent process
  restarts. An unavailable daemon must not cause marker advancement.

## Required evidence before implementation approval

The design review must resolve: (1) authoritative attempt identity and atomic
fencing; (2) safe-consistency hook and invalid-page bypass handling; (3)
checkpoint creation and complete-segment publication; (4) hard and cumulative
space limits, including what an operator sees when they are reached; and
(5) durable control format and mixed-version behavior. In particular, the
proposed feedback mechanism cannot guarantee recovery through an arbitrarily
large gap under a finite space bound. Any claim of unconditional liveness must
specify how upstream writes are backpressured and where retained WAL lives.

## Validation plan

1. Extend the existing real PostgreSQL rejected-checkpoint scenario in
   `continuous_redo_demo.sh`: resolve the invalid page before consistency,
   place the next completed checkpoint beyond the old two-segment allowance,
   demonstrate the old policy stalls, then demonstrate the protocol reaches
   that checkpoint, completes a restartpoint and unblocks archive. Check data,
   marker ordering, branch reads and WAL retention throughout.
2. Exercise a recovery crash before/after consistency feedback, during target
   publication, after target publication, mid-segment and before marker sync;
   restart writer, daemon and materializer independently and together. Verify
   no stale feedback or duplicate request extends the grant.
3. Cover missing/invalid/old-generation hints, rejected target, no later
   checkpoint, disabled invalid-page checking, no progress, exhausted budget,
   delayed/reordered responses, concurrent controllers, integer boundaries,
   and unknown/downgraded formats. Assert bounded archive and visible blocked
   status, not merely eventual timeout.
4. After deterministic tests pass, run a 72-hour workload with mixed writes,
   vacuum, process faults and branch churn. Record archive wait, replay and
   marker lag, grant usage, retention, storage growth and failure diagnostics.
