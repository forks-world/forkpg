# Pre-parse gate audit: fuzz_crc_fixup.c vs. every target's real loader

Mechanical audit requested during PR #303 review (round 6), after Codex kept
finding the same class of bug: a check the product's loader runs *before*
semantic/crc validation (magic/version, exact rec_len/struct-size, a fixed
total size, `count*size==length`, `reserved/pad==0`, a sibling/manifest
identity, or text canonicalization) that the "fixup" half of a persisted-
format fuzz target did not satisfy, so a mutation hitting that field alone
hard-failed the *whole* target's load (or the whole `ps_core_open()`) before
anything downstream was ever reached, wasting the point of running the
structure-aware fixup at all.

For every one of the 21 targets in `ps_fuzz_targets[]` (fuzz_common.c), this
lists every such gate its actual loader path enforces, the file:line for
both sides (product check, fixup handling), and how the fixup half
satisfies it -- or, where it deliberately does not, why. "Probe" is the
concrete verification: `contrib/pagestore/fuzz/build/pagestore_format_fuzz_replay`
plus a throwaway probe linking fuzz_common.c/fuzz_crc_fixup.c directly
against the real `ps_core_open()` (not part of the repo -- see PR #303's
review comments for the harness), mutating exactly the named field(s) and
comparing `ps_core_open()` under `PS_FUZZ_CRC_FIXUP=always` before/after.

Six gaps were found and fixed in the same round this file was written
(marked **FIXED (round 6)** below); everything else was already correct,
verified in place, or is `raw-only by design` for a stated reason.

## manifest (layers.manifest)

Loader: `manifest_record_valid_at()` / replay loop, pagestore_manifest.c:643.
Fixup: `fixup_manifest()`, fuzz_crc_fixup.c:288.

| Gate | Product file:line | Fixup handling | Probe |
|---|---|---|---|
| Per-record `len` must equal `manifest_type_payload_len(type)` exactly | pagestore_manifest.c:280,658 | Re-derived from the record's own `type` when recognized and it still fits the buffer (fuzz_crc_fixup.c:301-308) | Historical (round unknown, already in tree); re-confirmed clean in this round's full-suite replay |
| crc (FNV-1a over header+payload) | pagestore_manifest.c (`manifest_record_crc()`) | Recomputed every record (fuzz_crc_fixup.c:311-314) | same |
| magic/type (format identity) | manifest_record_valid_at | left fuzzer-controlled | raw-only by design: identity fields, not redundant-size fields |
| `PS_MANIFEST_ADD_LAYER` payload's per-location `size` vs. sibling image-layer file length | pagestore_layer.c (loc.size precondition) | `fixup_manifest_layer_size()` / `manifest_sync_add_layer_location_sizes()` called from `fixup_image_layer()` (fuzz_crc_fixup.c:1262,1360) | see image_layer row below (cross-file, not this target's own gate) |

## forkmeta (forkmeta)

Loader: `fork_meta_rec_wire_valid()`, pagestore_core.c:9635; replay loop.
Fixup: `fixup_forkmeta()` -> `fixup_forkmeta_records()`, fuzz_crc_fixup.c:391,442.

| Gate | Product file:line | Fixup handling | Probe |
|---|---|---|---|
| FKM3/FKM4 crc-24 (pad's low 3 bytes) | pagestore_core.c:9635 area (`fork_meta_rec_wire_valid`) | Recomputed (fuzz_crc_fixup.c:405-413) | historical, in tree |
| FKM2 `pad` must be exactly zero (no crc to make it self-consistent) | pagestore_core.c (`fork_meta_rec_wire_valid`, FKM2 branch) | Zeroed (fuzz_crc_fixup.c:402-403) | historical (round 4), in tree |
| magic (FKM2/FKM3/FKM4 selector) | same | left fuzzer-controlled | raw-only by design: format-identity |
| Record 0 must equal the selected snapshot's `FEV_SNAPSHOT_BASE` marker, or `fork_meta_source_conflicts_with_snapshot()`'s silent full-log rewrite kicks in | pagestore_core.c (`fork_meta_snapshot_reconcile_source()`) | **Deliberately not pinned** -- measured with before/after `-runs=0`: pinning record 0's identity made overall coverage *worse* (cov 1925/ft 2018 -> cov 1911/ft 1916), because it stopped the silent-rewrite fallback that currently lets `ps_core_open()` succeed and explore every later open step. See fuzz_crc_fixup.c:414-437's own comment. | raw-only by design, empirically justified in-file |

V4 classification remains fuzzer-controlled: unknown kinds, flags on markers,
and UNSTAMPED PAGE GROWs are semantic rejections. CRC repair also handles V4
records; the current fixture's source, checkpoint and tail are included in
the replay corpus alongside the legacy seeds.

## forkmeta_snapshot_manifest (forkmeta_snapshots/forkmeta_manifest_v1)

Loader: `read_record()`, pagestore_forkmeta_snapshot.c:955.
Fixup: `fixup_forkmeta_snapshot_manifest()`, fuzz_crc_fixup.c:1443.

| Gate | Product file:line | Fixup handling | Probe |
|---|---|---|---|
| `magic` | pagestore_forkmeta_snapshot.c:970 | left fuzzer-controlled | raw-only by design |
| `version` | pagestore_forkmeta_snapshot.c:971 | left fuzzer-controlled | raw-only by design |
| **`header_bytes`@8 must equal `FORKMETA_SNAPSHOT_HEADER_BYTES` (80)** | pagestore_forkmeta_snapshot.c:972 | **FIXED (round 6)**: pinned to `FORKMETA_SNAPSHOT_HEADER_BYTES_LOCAL` (fuzz_crc_fixup.c:1489). Previously grouped with magic/version and left unpinned -- wrong classification, since every record this function seals has the same fixed size by construction. | `scanat forkmeta_snapshot_manifest 8 1`: before rc=-1 ("open selected forkmeta snapshot failed"), after rc=0 |
| reserved@12 (4B), @68 (4B), @72 (8B) must be zero | pagestore_forkmeta_snapshot.c:973 | Zeroed (fuzz_crc_fixup.c:1490-1491) | historical (round 3), in tree |
| generation/cutoff_lsn/cutoff_admission_seq must equal the checkpoint/tail parts' own headers | pagestore_forkmeta_snapshot.c:975,986-989 | Read from `parts[0]`/checked against `parts[1]`, pinned (fuzz_crc_fixup.c:1496-1512) | historical, in tree |
| checkpoint/tail (`len`,`crc`) entries must match the live part files | pagestore_forkmeta_snapshot.c:990-994 (`published_part_valid`) | Derived from the pristine template parts and written into the buffer (fuzz_crc_fixup.c:1508-1513) | historical, in tree |
| crc (whole 80 bytes) | pagestore_forkmeta_snapshot.c:983-984 | Recomputed (fuzz_crc_fixup.c:1514) | historical, in tree |

## forkmeta_snapshot_checkpoint / forkmeta_snapshot_tail

Loader: `fork_meta_snapshot_load()`, pagestore_core.c:10294.
Fixup: `fixup_forkmeta_snapshot_part()`, fuzz_crc_fixup.c:1580.

| Gate | Product file:line | Fixup handling | Probe |
|---|---|---|---|
| `file length == header_bytes(80) + nrecords*sizeof(ForkMetaRecV2)` (this part's own record-count field) | pagestore_core.c:10294 area | Derived from the resized buffer (round 2, e402d1d9a) | historical, in tree |
| checkpoint/tail headers byte-identical from `generation` through `tail_bytes` (both parts carry all four count/byte fields) | pagestore_core.c (same function) | Cross-part sync via live sibling read/write (round 3, 10548b631) | historical, in tree |
| First 16 bytes (magic/version/header_bytes/part/record_bytes) and generation/cutoff_lsn/cutoff_admission_seq/freeze_admission_seq (next 32) are this part's own fixed identity | pagestore_core.c (same function) | Copied verbatim from this part's own template (round 3) | historical, in tree; this already covers the "header_bytes" field for *this* target (unlike the outer manifest above, which needed a round-6 fix) |
| Outer manifest's (len, crc) entry for this part | pagestore_forkmeta_snapshot.c (`open_validated_part`) | Patched into the live manifest (fuzz_crc_fixup.c ~1600-1650) | historical, in tree |
| Each FKM3 record's own crc-24 | pagestore_core.c (`fork_meta_snapshot_record_valid`) | `fixup_forkmeta_records()` reused (fuzz_crc_fixup.c:1580 body) | historical (round 4), in tree |
| Byte-0 divergence (fixup heals it) | -- | **Not a gap**: mutated byte lands inside a record body the crc recompute covers; expected, matches this file's documented contract | `scan forkmeta_snapshot_checkpoint 1 1` and `..._tail 1 1`: pristine==corrupt under fixup (both open), diverge under raw |

## image_layer (layer_0_...0a)

Loader: `ps_image_layer_read_index()` / `ps_image_layer_verify_data()`, pagestore_layer.c:597.
Fixup: `fixup_image_layer()`, fuzz_crc_fixup.c:1354.

| Gate | Product file:line | Fixup handling | Probe |
|---|---|---|---|
| Footer read at `loc->size - sizeof(footer)`, where `loc->size` is the *manifest's* recorded size, not this file's actual length | pagestore_layer.c:597 area | `fixup_manifest_layer_size()` patches the sibling layers.manifest record's `size` (+ crc) to this file's actual (possibly resized) length (fuzz_crc_fixup.c:1262,1360) | historical (round 5), in tree |
| `nrecs * entry_size == index span` (index_off..footer), entry_size picked by `version` | pagestore_layer.c (`ps_image_layer_read_index`) | Derived for an entry-aligned span (fuzz_crc_fixup.c:1400-1414) | historical (round 4), in tree |
| `version` (unrecognized -> reader's own first check fails regardless of nrecs) | same | left fuzzer-controlled | raw-only by design |
| `index_off` out of range | same | left as-is ("give up, still a valid fuzz input") | raw-only by design, documented fallback |
| data_crc / index_crc | pagestore_layer.c | Recomputed over data/index spans (fuzz_crc_fixup.c:1418-1421) | historical, in tree |
| Byte-0 divergence (fixup heals it) | -- | **Not a gap**: byte 0 is opaque page *data*, not a header field -- covered only by the data_crc this function recomputes over whatever bytes are present | `scan image_layer 1 1`: pristine==corrupt under fixup (both open), diverge under raw |

## retention.meta / retention.state

Loader: `ps_retention_open()`, pagestore_retention.c:1551 (outer header check ~1551+195; per-record `retention_record_valid()`, pagestore_retention.c:292).
Fixup: `fixup_retention_meta()`, fuzz_crc_fixup.c:523.

| Gate | Product file:line | Fixup handling | Probe |
|---|---|---|---|
| Per-record `magic`/`version` | pagestore_retention.c:295 | left fuzzer-controlled | raw-only by design |
| **Per-record `len` must equal `sizeof(PsRetentionRecord)` (64) exactly** | pagestore_retention.c:297 | **FIXED (round 6)**: pinned to `stride` before crc (fuzz_crc_fixup.c:547-548) | `scanat retention_meta 12 1`: before rc=-1 ("unrecognized version 2 record size 191"), after rc=0 |
| Record 0's `len` also drives the file-level "legacy vs. current" branch, hard-failing the *whole* open if it does not match either known size | pagestore_retention.c:1831,1834 (per-record hard fail; record 0 also selects the file's legacy-vs-current branch) | same fix as above (record 0 is covered by the same per-record loop) | same probe |
| `pad` must be exactly zero | pagestore_retention.c:297 | Zeroed before crc (fuzz_crc_fixup.c:549, pre-existing round 4) | historical, in tree |
| crc | pagestore_retention.c:298 | Recomputed (fuzz_crc_fixup.c:550-551) | historical, in tree |
| Sibling retention.state's (nrecords, log_hash) must equal this log's actual replay | pagestore_retention.c (`ps_retention_open`'s post-loop check) | `retention_meta_derive_state()` + live pwrite to retention.state (fuzz_crc_fixup.c:505,563-583) | historical (round 6 coordinator review), in tree |
| Semantic fields (owner_kind/resources/lsn/...) | pagestore_retention.c:298-312 | left fuzzer-controlled ("give up, still valid fuzz input") | raw-only by design, documented |

Loader: `retention_state_crc()` gate only (single fixed struct).
Fixup: `fixup_retention_state()`, fuzz_crc_fixup.c:598. No file-size/count gate beyond exact struct size, which the "give up if len is wrong" pattern already covers; magic/version left fuzzer-controlled. No gap.

## timelines

Loader: `load_timelines()` in `pagestore_core.c` reads an eight-byte framing
header before validating each known record shape. `fixup_timelines()` repairs
the 56-byte lifecycle V2 and 64-byte lifecycle V3 shapes, including mixed logs.

| Gate | Fixup handling |
|---|---|
| Magic | Left fuzzer-controlled; identity rejection is meaningful. |
| Record length | Pinned to the selected 56- or 64-byte stride. A damaged length in a homogeneous V3 seed is inferred from the buffer length. |
| Reserved field | Zeroed before checksum repair. |
| CRC | Recomputed over each complete record with its checksum field zeroed. |
| V3 finite `branch_seq` | Pinned to infinity in the repaired pass while activation is unsupported; the raw pass exercises finite-cap rejection. |
| Lifecycle kind, id, state, incarnation and ancestry | Left fuzzer-controlled; semantic rejection is meaningful. |
| 16-, 32- and 48-byte legacy shapes | Not repaired; covered by the raw pass and compatibility fixtures. |

The gate sweep retains its legacy V2 fixture and allowlist. The V3 fixture's
timeline log is also included in the replay corpus.

## page_frontier / walidx_frontier

Loader: `page_frontier_load()` pagestore_core.c:4777; `walidx_frontier_load()` pagestore_core.c:5074 (structurally identical).
Fixup: `fixup_page_frontier()`/`fixup_walidx_frontier()`, fuzz_crc_fixup.c:728,738.

| Gate | Product file:line | Fixup handling | Probe |
|---|---|---|---|
| `st.st_size == sizeof(state)` exactly (selects the current vs. legacy V2 branch) | pagestore_core.c:4799,4812 | Fixup only runs `if (len < sizeof(FuzzPageFrontierState)) return;` -- a length-changing mutation is the fuzz input's own length, which this target's raw and fixup halves both just write as-is; the "give up" fallback already covers a wrong length | raw-only by design (no separate on-disk-length field exists to desync from the buffer, unlike wal_segment/retention.meta) |
| `magic`/`version` | pagestore_core.c:4803-4804 | left fuzzer-controlled | raw-only by design |
| crc (over `offsetof(crc)` leading bytes) | pagestore_core.c:4805 | Recomputed (fuzz_crc_fixup.c:732-733,742-743) | historical, in tree; re-confirmed in this round's byte-0 sweep (diverges under both modes) |

## walidx_<tl>_<shard>[_e<epoch>] (walidx_log_epoch / walidx_log_legacy)

Loader: `walidx_recover_one()`, pagestore_core.c:17958.
Fixup: `fixup_walidx_log()`, fuzz_crc_fixup.c:775 (called for both targets, different `expected_timeline`); epoch watermark handled separately (below).

| Gate | Product file:line | Fixup handling | Probe |
|---|---|---|---|
| Shape selection: `magic==WIDX && rec_len∈{64,56}`, or `shard==0 && magic==WIPG && rec_len==1080` | pagestore_core.c:18011-18020 | WIDX branches already select `stride` *by* rec_len matching (fuzz_crc_fixup.c:786-791, no fix needed -- confirmed this round); **WIPG branch previously keyed off magic alone and never pinned rec_len -- FIXED last round (d18a889268)**: `put_le32(buf+off+4, WALIDX_PROGRESS_BYTES)` when magic matches (fuzz_crc_fixup.c:801-816) | `progress_rec_len_fixed`/`_buggy` probes (prior round): fixed opens (rc=0), buggy fails (rc=-1) |
| V1 `reserved`@12 must be zero | pagestore_core.c:18046 | Zeroed for the V1 shape only (fuzz_crc_fixup.c:817-826) | historical (round 4), in tree |
| `timeline`@16 must equal the directory's own timeline (0 for epoch, 1 for legacy) | pagestore_core.c:18033,18046,18084 | Pinned per-call via `expected_timeline` param (fuzz_crc_fixup.c:827) | historical, in tree; `widx_v1_pristine` probe confirms timeline=1 is the right pin for legacy |
| Per-record crc | pagestore_core.c:18037,18051,18086 | Recomputed for all three shapes (fuzz_crc_fixup.c:828-830) | historical, in tree |
| Progress-record semantic fields (start_lsn continuity, shard masks, coverage) | pagestore_core.c:18083-18093 | left fuzzer-controlled | raw-only by design |
| **Epoch log only: sibling `.size` watermark clamps every read to its recorded length, independent of this file's actual bytes** | storage_posix.c:1046 (`posix_walidx_epoch_reconcile_locked`), consumed at storage_posix.c:1184 | **FIXED last round (d18a889268)**: `fixup_walidx_log_epoch_watermark()` (fuzz_crc_fixup.c:927) now called *unconditionally* by `ps_fuzz_run_one()` (fuzz_common.c), in both raw and fixup iterations, not only when this target's own crc fixup ran | Synthetic timeline-0 record probe: raw-mode pristine opens (rc=0), raw-mode byte-0-flip rejected (rc=-1) |

## walidx_<tl>_<shard>_e<epoch>.size (walidx_watermark)

Loader: `posix_walidx_watermark_read()`, storage_posix.c:953.
Fixup: `fixup_walidx_watermark()`, fuzz_crc_fixup.c:847.

| Gate | Product file:line | Fixup handling | Probe |
|---|---|---|---|
| `length` must equal the sibling log file's actual on-disk size | storage_posix.c:953 area | Read via `fstat()` on the live sibling, pinned (fuzz_crc_fixup.c:857-864) | historical, in tree |
| `reserved` must be zero | storage_posix.c (`posix_walidx_watermark_read`) | Zeroed (fuzz_crc_fixup.c:873) | historical (round 3), in tree |
| `magic` | same | left fuzzer-controlled | raw-only by design |
| crc | same | Recomputed (fuzz_crc_fixup.c:874-875) | historical, in tree |

## wal_segments_0/wal_store_identity_v1 (wal_store_identity)

Loader: `decode_metadata()`, pagestore_wal_store.c:104.
Fixup: `fixup_wal_store_identity()`, fuzz_crc_fixup.c:975.

| Gate | Product file:line | Fixup handling | Probe |
|---|---|---|---|
| `magic`/`version` | pagestore_wal_store.c:109-110 | left fuzzer-controlled | raw-only by design |
| **header@8 must equal `PS_WAL_STORE_METADATA_BYTES` (64)** | pagestore_wal_store.c:111 | **FIXED (round 6)**: pinned to 64 (fuzz_crc_fixup.c:989, this function's own established "fixed 64-byte encoding" invariant) | `scanat wal_store_identity 8 1`: before rc=-1 ("refusing invalid immutable WAL segments"), after rc=0 |
| reserved@12 (4B), @52 (4B), @56 (8B) must be zero | pagestore_wal_store.c:112-114 | Zeroed (fuzz_crc_fixup.c:1000-1002, pre-existing round 3) | historical, in tree |
| crc | pagestore_wal_store.c:115 | Recomputed (fuzz_crc_fixup.c:1003-1004) | historical, in tree |
| Semantic fields (segment_size power-of-2/range, LSN alignment/ordering) | pagestore_wal_store.c:118-128 | left fuzzer-controlled | raw-only by design |

## wal_segments_0/walv1_1_... (wal_segment)

Loader: `ps_wal_segment_decode()` (pagestore_wal_segment.c:279, incl. `segment_identity_valid()` pagestore_wal_segment.c:186) *and* `load_segment()` (pagestore_wal_store.c:752).
Fixup: `fixup_wal_segment()`, fuzz_crc_fixup.c:1065; length resize in `ps_fuzz_run_one()`, fuzz_common.c.

| Gate | Product file:line | Fixup handling | Probe |
|---|---|---|---|
| `magic`/`version` (ps_wal_segment_decode) | pagestore_wal_segment.c:302-303 | left fuzzer-controlled | raw-only by design |
| `header_len == PS_WAL_SEGMENT_HEADER_BYTES` | pagestore_wal_segment.c:304 | Not independently mutated by this harness's scope (whole-file length is the resized quantity, see below); no dedicated gap found | verified via wal_segment_append probe: opens fully post-fix |
| `payload_len == payload's declared identity` / `segment_identity_valid()` (payload_len ≤ segment_size, power-of-2, LSN/segment_no alignment) | pagestore_wal_segment.c:186-195,307 | payload_len derived from actual length; xlp identity derived via `ps_wal_segment_payload_identity()` (fuzz_crc_fixup.c:1082-1091, historical c959796) | historical, in tree |
| header_crc | pagestore_wal_segment.c:309 (`header_crc`) | Recomputed (fuzz_crc_fixup.c:1099-1102) | historical, in tree |
| **`load_segment()`: `header.payload_len == store->segment_size` *exactly*, `header.segment_size == store->segment_size`, and `st.st_size == PS_WAL_SEGMENT_HEADER_BYTES + header.payload_len` -- `store->segment_size` is the fixed product constant `WAL_IMMUTABLE_SEGMENT_BYTES` == `PS_WAL_SEGMENT_MIN_BYTES`, not derived from this file** | pagestore_wal_store.c:777-779 | **FIXED (round 6)**: `ps_fuzz_run_one()` (fuzz_common.c) resizes the buffer to `ps_fuzz_wal_segment_fixed_len()` (= `PS_WAL_SEGMENT_HEADER_BYTES + PS_WAL_SEGMENT_MIN_BYTES`) *before* the fixup ever runs, but only for a fixed-up iteration; `fixup_wal_segment()` then also pins `timeline`/`segment_no`/`start_lsn`/`segment_size` from the one real captured segment's own identity (fuzz_crc_fixup.c:1075-1081) | `wal_segment_append` probe (+1 byte, `PS_FUZZ_CRC_FIXUP=always`): before rc=-1 ("refusing invalid immutable WAL segments"), after opens fully |
| `load_segment()`: `header.timeline`/`segment_no`/`start_lsn` must equal the store's current state | pagestore_wal_store.c:770-772 (timeline/segment_no/start_lsn) | Pinned from template (fuzz_crc_fixup.c:1075-1081), part of the same round-6 fix above | same probe |
| payload_crc (hash-per-chunk against `header.payload_crc`) | pagestore_wal_store.c (`load_segment`'s chunked hash loop) | Recomputed over the resized payload (fuzz_crc_fixup.c:1095-1096) | 60s run_fuzz.sh, 0 crashes |

## .pagestore-nshards (store_config)

Loader: `validate_store_shard_count()`, pagestore_core.c:2501 (`sscanf("PSS2 %u %x %c", ...)`).
Fixup: `fixup_store_config()`, fuzz_crc_fixup.c:1147.

| Gate | Product file:line | Fixup handling | Probe |
|---|---|---|---|
| Text must parse as `PSS2 <count> <hex>` with exactly 2 fields consumed | pagestore_core.c:2501 area | Only fixed up when the input already parses this way; otherwise left alone ("give up, still valid fuzz input") | historical, in tree |
| crc32c of the 4-byte count | pagestore_core.c (same fn) | Recomputed (fuzz_crc_fixup.c ~1180) | historical, in tree |
| `%u` canonicalization (e.g. "01"->"1") can shrink the formatted text, leaving stale trailing bytes that `%x`/`%c` greedily re-parse | pagestore_core.c:2501 (`sscanf` greediness) | Trailing region blanked with spaces instead of left as stale bytes (round 4) | historical, in tree; `store_config_trailing` probe confirms genuine trailing garbage still rejected (not papered over) |

## timelines / page_segment / wal_log: no crc, so "fixup" is a no-op by design

`page_segment` and `wal_log` have **no dedicated fixup function at all**
(`ps_fuzz_crc_fixup()`'s dispatch comment: "wal_log, page_segment, and any
other/unknown target: no checksum to fix up"). Checked as part of this
round's audit, since that is exactly the kind of target most likely to
have an un-analyzed gate:

- **page_segment**: `SegRecHdr` (pagestore_core.c:4113) carries no crc field
  at all. The only pre-parse gates are magic (must be one of the
  `SEG*_MAGIC`/`SEG_HOLE*_MAGIC` values, `recover()` pagestore_core.c:21385
  area, `goto fail` on an unrecognized *nonzero* magic) and `len == page_size`
  for a non-hole record (a *soft* stop -- `break`, treated as end-of-log, not
  a hard reject). Neither is the "whole-file hard fail on a redundant
  length/count field" class this round is about: an unrecognized magic is a
  meaningful, independently fuzzable identity rejection (raw-only by
  design, same as magic elsewhere), and a bad `len` just truncates how much
  of the file gets scanned rather than rejecting it outright. This round's
  actual page_segment fix (see PR #303 history) was the *target* naming a
  segment past the durable flush watermark, not a fixup gap -- fixed last
  round (05b3ae11b3).
- **wal_log**: `WalRecHdr` (pagestore_core.c:12454) likewise carries no crc.
  `wal_coverage_advance()` (pagestore_core.c:12912) and the various replay
  loops reject an unrecognized magic (a meaningful identity field, left
  alone) but otherwise only bound-check offsets/lengths for overflow, not a
  redundant self-describing size against a fixed constant. No gap found.

## walidx_snapshots_0/walidx_manifest_v1 (walidx_snapshot_manifest)

Loader: `ps_walidx_snapshot_open_internal()`, pagestore_walidx_snapshot.c:1449 (and `read_prepared()`, pagestore_walidx_snapshot.c:892, same shape).
Fixup: `fixup_walidx_snapshot_manifest()`, fuzz_crc_fixup.c:1739.

| Gate | Product file:line | Fixup handling | Probe |
|---|---|---|---|
| `magic`@0 | pagestore_walidx_snapshot.c:1477 | left fuzzer-controlled | raw-only by design |
| `version`@4 | pagestore_walidx_snapshot.c:1478 | left fuzzer-controlled | raw-only by design |
| **`header@8` must equal `WALIDX_SNAPSHOT_HEADER_BYTES` (64)** | pagestore_walidx_snapshot.c:1479 | **FIXED (round 6)**: pinned to `WALIDX_SNAPSHOT_MANIFEST_HEADER_BYTES_LOCAL` (fuzz_crc_fixup.c:1773) | `scanat walidx_snapshot_manifest 8 1`: before rc=-1, after rc=0 |
| **`header@12` must equal `WALIDX_SNAPSHOT_ENTRY_BYTES` (16)** | pagestore_walidx_snapshot.c:1480 | **FIXED (round 6)**: pinned to `WALIDX_SNAPSHOT_MANIFEST_ENTRY_BYTES_LOCAL` (fuzz_crc_fixup.c:1774) | `scanat walidx_snapshot_manifest 12 1`: before rc=-1, after rc=0 |
| Actual file length must equal `HEADER_BYTES + nshards*ENTRY_BYTES` | pagestore_walidx_snapshot.c:1478-1481 | nshards@20 re-derived for an entry-aligned length (round 2 class, fuzz_crc_fixup.c:1750-1762) | historical, in tree |
| `timeline`@16 must equal the caller's timeline (0) | pagestore_walidx_snapshot.c:1481 | Pinned (fuzz_crc_fixup.c:1776-1780, round 3) | historical, in tree |
| reserved@52 (4B), @56 (8B) must be zero | pagestore_walidx_snapshot.c:1481 | Zeroed (fuzz_crc_fixup.c:1781, round 3) | historical, in tree |
| generation/start_lsn/end_lsn must match the real shard file's header | pagestore_core.c (`walidx_snapshot_decode_header`) | Pinned from template (fuzz_crc_fixup.c:1800-1820) | historical, in tree |
| **Each shard entry's own index@0 must equal its array position `i`** | pagestore_walidx_snapshot.c:1512,1526 | **FIXED last round (05b3ae11b3)**: pinned unconditionally for every entry in range (fuzz_crc_fixup.c:1849-1863) | `manifest_index_fixed`/`_buggy` probes (prior round): fixed opens, buggy fails |
| Per-entry (len,crc) vs. the real shard file | pagestore_walidx_snapshot.c:1517-1518 (`published_shard_valid`) | Derived from the in-memory shard template (fuzz_crc_fixup.c:1826-1846) | historical, in tree |
| crc (whole file, FNV-1a) | pagestore_walidx_snapshot.c:1489-1491 | Recomputed (fuzz_crc_fixup.c:1897-1898) | historical, in tree |

## walidx_snapshots_0/walidxg1_..._000 (walidx_snapshot_shard)

Loader: `walidx_snapshot_decode_header()`, pagestore_core.c:16321; `validate_shard()`, pagestore_walidx_snapshot.c:1401.
Fixup: `fixup_walidx_snapshot_shard()`, fuzz_crc_fixup.c:1928.

| Gate | Product file:line | Fixup handling | Probe |
|---|---|---|---|
| `magic`@0 | pagestore_core.c:16336 | left fuzzer-controlled | raw-only by design |
| `version`@4 paired with `bytes`@8 (must be one of 3 known (version,bytes) pairs; also selects `crc_offset`) | pagestore_core.c:16338-16347 | `version`@4 left fuzzer-controlled (meaningful identity); **`bytes`@8 FIXED (round 6)**: pinned to 72, this function's one shape (fuzz_crc_fixup.c:1937-1955) -- previously left alongside version, but unlike version it is a redundant size value with only one legal value for the shape this fixup actually seals | `scanat walidx_snapshot_shard 8 1`: before rc=-1, after rc=0; `scanat walidx_snapshot_shard 4 1` (version): still correctly rc=-1 post-fix, confirming the identity/redundant-field split is right |
| `timeline`@12, `shard`@16, `generation`@24, `start_lsn`@32, `end_lsn`@40 | pagestore_core.c:16358-16362 (tl@12, shard@16, generation@24, start_lsn@32, end_lsn@40) | Pinned (fuzz_crc_fixup.c:1957-1966, historical round 1-3) | historical, in tree |
| `source_offset`@48 must be 0 | pagestore_core.c:16363-16364 (V1 branch; 0 unconditionally satisfies it) | Zeroed (fuzz_crc_fixup.c:1966) | historical, in tree |
| crc (span = `bytes`, offset = `crc_offset`) | pagestore_core.c:16355-16365 | Recomputed at offset 64 over 72 bytes -- this function's one shape (fuzz_crc_fixup.c:1971-1972) | historical, in tree |
| `nrecords` (low32@20, high32@68) vs. `expected_len = header_bytes + nrecords*record_bytes` against the manifest's recorded shard length | pagestore_core.c (decode caller) | Re-derived for a record-aligned length (fuzz_crc_fixup.c:1978-1990, round 2) | historical, in tree |
| Sibling manifest's shard-0 (len,crc) entry (`validate_shard()`) | pagestore_walidx_snapshot.c:1401 | Patched into the live manifest (fuzz_crc_fixup.c:2018-2060) | historical, in tree |
| Per-record `timeline`@16 and crc | pagestore_core.c (per-record loop) | Pinned + recomputed (fuzz_crc_fixup.c:1999-2007, round 4) | historical, in tree |

## Summary of this round's fixes (round 6)

1. `wal_segment`: `ps_fuzz_run_one()` (fuzz_common.c) now resizes the
   fixed-up buffer to the fixed `PS_WAL_SEGMENT_HEADER_BYTES +
   PS_WAL_SEGMENT_MIN_BYTES` length before `fixup_wal_segment()` runs;
   `fixup_wal_segment()` also pins timeline/segment_no/start_lsn/segment_size.
2. `timelines`: `rec_len` pinned to the current shape's own size.
3. `retention.meta`: per-record `len` pinned to `sizeof(PsRetentionRecord)`.
4. `walidx_snapshot_manifest`: `header@8`/`header@12` (header_bytes/entry_bytes)
   pinned to their fixed constants.
5. `walidx_snapshot_shard`: `bytes`@8 (header_bytes) pinned to 72.
6. `wal_store_identity`: `header@8` pinned to `PS_WAL_STORE_METADATA_BYTES` (64).
7. `forkmeta_snapshot_manifest`: `header_bytes`@8 pinned to
   `FORKMETA_SNAPSHOT_HEADER_BYTES_LOCAL` (80) -- previously misclassified
   as a format-identity field alongside magic/version.

Findings 1-2 were the round's two accepted Codex findings; 3-7 were found by
this table's own mechanical sweep (probe every candidate "redundant
size/count field" per target under `PS_FUZZ_CRC_FIXUP=always`) and fixed in
the same commit.

## Round 7: the manual method wasn't enough -- gate_sweep.c

Codex reviewed round 6's commit and found three more gates this table
missed by manual reading:

1. `wal_segment`: `ps_wal_segment_decode()` (pagestore_wal_segment.c) also
   requires `header_len`@8 to equal `PS_WAL_SEGMENT_HEADER_BYTES` (64) and
   `flags`@12 to be exactly zero. **FIXED**: both pinned in
   `fixup_wal_segment()` (fuzz_crc_fixup.c), the same way payload_len/
   segment_size already were.
2. `image_layer`: `ps_image_layer_verify_data()` (pagestore_layer.c)
   requires the footer's `page_size` to equal the store's own configured
   `page_size` (the `page_size` global in pagestore_core.c,
   `PS_DEFAULT_PAGE_SIZE`=8192 unless overridden). **FIXED**: pinned in
   `fixup_image_layer()`.
3. `forkmeta` / `forkmeta_snapshot_checkpoint` / `forkmeta_snapshot_tail`
   (all three share `fixup_forkmeta_records()`): `fork_meta_rec_wire_valid()`
   (pagestore_core.c) checks `rec->rec_len != sizeof(*rec)` *first*, before
   magic, pad, or crc. **FIXED**: `rec_len` pinned to
   `sizeof(FuzzForkMetaRecV2)` (64) for every record whose magic this
   function recognizes (FKM2 or FKM3).

Three rounds of Codex findings on top of three rounds of "we read the
loader and thought we got every gate" makes the point: manual reading does
not scale to a file that now fixes up ~20 formats' worth of pre-parse
gates. `contrib/pagestore/fuzz/tests/gate_sweep.c` replaces judgment with
brute force wherever brute force is cheap enough to run -- see its own
header comment for exactly what it does and why (byte-flip every offset in
each target's pristine file's first N bytes, fixup(always), open, and
treat the presence of `pagestore_core: open step ... failed` in that one
call's stderr as authoritative pass/fail, verified against
`open_step_failed()`'s own comment that *every* `ps_core_open()` failure
path is routed through it).

### Rerunning it

```
contrib/pagestore/fuzz/build.sh   # or: ninja -C build contrib/pagestore/pagestore_gate_sweep
# full audit (256 bytes/target, both flip masks) -- what round 7 used:
build/contrib/pagestore/pagestore_gate_sweep list contrib/pagestore
# regression check against the reviewed allowlist (what meson runs, with a
# smaller sweep width so it finishes in seconds -- see meson.build's own
# comment on why: neither this binary nor pagestore_format_fuzz_replay
# wraps fsync(), unlike the instrumented libFuzzer binary):
build/contrib/pagestore/pagestore_gate_sweep check \
  contrib/pagestore/fuzz/tests/gate_sweep_allowlist.txt contrib/pagestore
```

`check` exits nonzero and prints every offset not on the allowlist. For
each new one, decide (see gate_sweep.c's own header comment for the full
rubric): (a) a fixable gate -> pin the field in fuzz_crc_fixup.c and rerun;
(b) a genuine semantic rejection, or (c) a deliberately unpinned identity
field -> add a documented entry to
`fuzz/tests/gate_sweep_allowlist.txt` explaining which, with a file:line
citation for the check that rejects it.

### Round 7 sweep results (256 bytes/target, both masks, after all fixes)

| target | rejected offsets found | classification |
|---|---|---|
| manifest | 47 (0-11, 20-27, 100-103) | (c) magic/type identity; (b) layer_id/URI consistency + location_count bound |
| forkmeta | 186 | (c) per-record magic; (b) per-record semantic fields (rec_len fixed this round) |
| forkmeta_snapshot_manifest | 16 (0-7) | (c) magic/version (header_bytes fixed this round) |
| forkmeta_snapshot_checkpoint | 131 | same per-record pattern as forkmeta, offset by the 80-byte header |
| forkmeta_snapshot_tail | 122 | same |
| image_layer | 0 | none (page_size fixed this round; data bytes healed by crc recompute) |
| page_frontier | 16 (0-7) | (c) magic/version |
| page_segment | 7 (0-3) | (c) magic (this format has no crc at all) |
| retention_meta | 283 | (c) per-record magic/type; (b) per-record PsRetentionPin semantic fields (len fixed this round) |
| retention_state | 16 (0-7) | (c) magic/version |
| store_config | structured PSS2 seed | (c) malformed text; (b) zero shard count or trailing garbage; checksum digits are repaired |
| timelines | 333 | (c) per-record magic (rec_len fixed this round); (b) per-record semantic fields |
| wal_log | 32 (0-15) | (c) magic; (b) len/start_lsn (this target has no fixup at all -- see below) |
| wal_store_identity | 80 (0-7, 16-47) | (c) magic/version (header@8 fixed round 6); (b) decode_metadata()'s semantic LSN/segment_size checks |
| wal_segment | 16 (0-7) | (c) magic/version only (everything else fixed round 6/7) |
| walidx_frontier | 16 (0-7) | (c) magic/version |
| walidx_log_epoch | 40 | (c) magic/rec_len shape-selection; (b) flags/end_lsn semantic (timeline correctly pinned, absent) |
| walidx_log_legacy | 380 | (c) per-record magic/rec_len shape-selection; (b) WIPG progress-record semantic fields |
| walidx_watermark | 16 (0-7) | (c) magic |
| walidx_snapshot_manifest | 16 (0-7) | (c) magic/version (header@8/@12 fixed round 6) |
| walidx_snapshot_shard | 139 | (c) magic/version/per-record magic+rec_len; (b) per-record semantic fields (bytes@8/timeline both fixed) |
| **total** | **1895** | **all (b)/(c), `check` PASSes against gate_sweep_allowlist.txt** |

Full per-offset classification with file:line citations for every (b)/(c)
class lives in `fuzz/tests/gate_sweep_allowlist.txt`'s own header comment
and per-target blocks -- this table is the summary; that file is the
source of truth `gate_sweep check` actually enforces.

No new class-(a) gaps were found beyond the three Codex reported this
round: every one of the 1895 rejections this sweep found, across all 21
targets, was already explained by an existing fixed-up field (rec_len/
header_bytes fixes from this round and round 6), a deliberately-unpinned
identity field, or a genuine semantic/consistency check.

### Current-head review regressions

- `walidx_snapshot_manifest`: fixed-up inputs are resized to the fixture
  manifest before checksum repair. This preserves exactly the shard set whose
  payload files exist. Raw inputs keep their original lengths. The gate sweep
  requires a one-entry append and removal to open after repair.
- `store_config`: the sweep uses `corpus/store_config/posix-artifact-lifecycle`
  and requires its `PSS2` prefix, instead of using the fixture's legacy `1\n`.
  Missing or invalid seeds fail the sweep. The allowlist documents malformed
  text, zero shard counts, and trailing garbage; checksum repair stays checked.
