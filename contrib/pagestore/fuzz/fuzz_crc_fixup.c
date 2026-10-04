/*-------------------------------------------------------------------------
 *
 * fuzz_crc_fixup.c
 *	  Per-format checksum recomputation.  See fuzz_crc_fixup.h.
 *
 * Every record layout below is copied from the product source named in its
 * comment (never guessed), the same convention the existing
 * pagestore_*_test.c white-box tests already use for a format whose struct
 * is private to one .c file.  Two generic hash primitives cover every
 * format in this codebase:
 *   - fnv1a_step(): the project's standard FNV-1a (offset basis
 *     2166136261, prime 16777619), identical in pagestore_core.c's fnv(),
 *     pagestore_manifest.c's manifest_fnv1a(), pagestore_retention.c's
 *     retention_fnv1a(), pagestore_layer.c's img_crc(),
 *     pagestore_wal_segment.c's fnv1a(), pagestore_wal_store.c's
 *     metadata_hash(), storage_posix.c's posix_walidx_watermark_crc(), and
 *     pagestore_walidx_snapshot.c/pagestore_forkmeta_snapshot.c's fnv1a().
 *   - ps_crc32c_update()/PS_CRC32C_INIT/PS_CRC32C_FIN(): PostgreSQL's
 *     CRC-32C, from pagestore_artifact_format.h (the one public, shared
 *     implementation -- used directly here, not reimplemented).
 *
 *-------------------------------------------------------------------------
 */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pagestore_core.h"
#include "pagestore_prune.h"
#include "pagestore_wal_segment.h"
#include "fuzz_crc_fixup.h"
#include "fuzz_common.h"

/* ---- generic hash primitives ------------------------------------------ */

static uint32_t
fnv1a_step(uint32_t h, const void *p, size_t n)
{
	const unsigned char *b = p;

	for (size_t i = 0; i < n; i++)
	{
		h ^= b[i];
		h *= 16777619u;
	}
	return h;
}
#define FNV1A_INIT 2166136261u

static uint32_t
get_le32(const unsigned char *p)
{
	return (uint32_t) p[0] | (uint32_t) p[1] << 8 |
		(uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

static void
put_le32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char) v;
	p[1] = (unsigned char) (v >> 8);
	p[2] = (unsigned char) (v >> 16);
	p[3] = (unsigned char) (v >> 24);
}

static void
put_le16(unsigned char *p, uint16_t v)
{
	p[0] = (unsigned char) v;
	p[1] = (unsigned char) (v >> 8);
}

static uint64_t
get_le64(const unsigned char *p)
{
	return (uint64_t) get_le32(p) | (uint64_t) get_le32(p + 4) << 32;
}

static void
put_le64(unsigned char *p, uint64_t v)
{
	put_le32(p, (uint32_t) v);
	put_le32(p + 4, (uint32_t) (v >> 32));
}

/*
 * Mirrors of pagestore_manifest.c's private PsManifestRecord/
 * PsManifestKeyDisk/PsManifestLocationDisk/PsManifestLayerDisk -- not
 * exported, so reproduced by field-for-field struct layout (sizeof()/
 * offsetof() do the layout-sensitive work, same convention as every other
 * mirrored struct in this file) rather than hand-computed byte offsets.
 */
#define PS_MANIFEST_HEADER_BYTES_LOCAL 20u		/* magic,version,type,len,crc */
#define PS_MANIFEST_ADD_LAYER_LOCAL 1u
#define PS_MANIFEST_SET_REMOTE_DURABLE_LOCAL 2u
#define PS_MANIFEST_DROP_LOCAL_LOCAL 3u
#define PS_MANIFEST_MARK_DELETE_LOCAL 4u
#define PS_MANIFEST_REMOVE_LAYER_LOCAL 5u
#define PS_MANIFEST_SET_FLUSH_WATERMARK_LOCAL 6u
#define PS_MANIFEST_SET_REMOTE_LOCATION_LOCAL 7u
#define PS_MANIFEST_REBASE_FLUSH_WATERMARK_LOCAL 8u
#define PS_LAYER_URI_MAX_LOCAL 512u
#define PS_LAYER_MAX_LOCATIONS_LOCAL 3u
#define PS_LAYER_TIER_LOCAL_HOT_LOCAL 1u
#define PS_LAYER_TIER_LOCAL_COLD_LOCAL 2u
/* This harness's one local image-layer target file (fuzz_common.c's
 * ps_fuzz_targets table: {"image_layer", "layer_0_000000000000000a"}) --
 * matched by basename, the same way canonicalize_local_layer_uri()
 * (pagestore_layer_store.c) itself matches a recorded location's uri
 * against the store's own layer directory. */
#define PS_FUZZ_IMAGE_LAYER_BASENAME "layer_0_000000000000000a"
/* Bound on the sibling layers.manifest this harness ever writes -- well
 * above round-4's manifest target -max_len (49152; largest checked-in
 * seed 21852), read/patched/written whole like the other cross-file
 * fixups' sibling manifests. */
#define PS_FUZZ_MANIFEST_BUF_BYTES_LOCAL 65536u

typedef struct FuzzManifestKeyDisk
{
	uint32_t	spcOid;
	uint32_t	dbOid;
	uint32_t	relNumber;
	int32_t		forkNum;
	uint32_t	klass;
} FuzzManifestKeyDisk;

typedef struct FuzzManifestLocationDisk
{
	uint32_t	tier;
	char		uri[PS_LAYER_URI_MAX_LOCAL];
	uint64_t	size;
	uint32_t	generation;
	uint8_t		available;
	uint8_t		pad[3];
} FuzzManifestLocationDisk;

typedef struct FuzzManifestLayerDisk
{
	uint64_t	layer_id;
	uint32_t	kind;
	uint32_t	timeline;
	FuzzManifestKeyDisk start_key;
	FuzzManifestKeyDisk end_key;
	uint32_t	start_block;
	uint32_t	end_block;
	uint64_t	lsn_start;
	uint64_t	lsn_end;
	uint32_t	location_count;
	FuzzManifestLocationDisk locations[PS_LAYER_MAX_LOCATIONS_LOCAL];
	uint64_t	created_at_lsn;
	uint64_t	remote_uploaded_lsn;
	uint8_t		remote_durable;
	uint8_t		local_pinned;
	uint8_t		deleting;
	uint8_t		pad;
} FuzzManifestLayerDisk;

/* Mirrors pagestore_manifest.c's private PsManifestLayerIdEvent/
 * PsManifestRemoteLocationEvent and pagestore_manifest.h's exported
 * PsFlushWatermark -- the other three payload shapes
 * manifest_type_payload_len() maps a record's `type` to. */
typedef struct FuzzManifestLayerIdEvent
{
	uint64_t	layer_id;
	uint64_t	value;
} FuzzManifestLayerIdEvent;

typedef struct FuzzManifestRemoteLocationEvent
{
	uint64_t	layer_id;
	FuzzManifestLocationDisk location;
} FuzzManifestRemoteLocationEvent;

typedef struct FuzzFlushWatermark
{
	uint32_t	shard;
	uint32_t	seg_id;
	uint64_t	seg_off;
} FuzzFlushWatermark;

/*
 * Codex round-4-audit finding: manifest_record_valid_at()
 * (pagestore_manifest.c) requires a record's on-disk `len` field to
 * *exactly* equal manifest_type_payload_len(type) -- a fixed size that
 * depends only on `type`, never fuzzer-controlled -- checked before the
 * crc. A record whose type survived a mutation unchanged (very likely for
 * a byte flip elsewhere in the 20-byte header) but whose len field did not
 * was rejected outright, no matter how correct the fixed-up crc was.
 * Mirrors manifest_type_payload_len()'s switch exactly; an unrecognized
 * type returns -1, same as the product function, and is left to the raw
 * "explore malformed type" path untouched. */
static int
manifest_type_payload_len_local(uint32_t type)
{
	switch (type)
	{
		case PS_MANIFEST_ADD_LAYER_LOCAL:
			return (int) sizeof(FuzzManifestLayerDisk);
		case PS_MANIFEST_SET_REMOTE_DURABLE_LOCAL:
		case PS_MANIFEST_DROP_LOCAL_LOCAL:
		case PS_MANIFEST_MARK_DELETE_LOCAL:
		case PS_MANIFEST_REMOVE_LAYER_LOCAL:
			return (int) sizeof(FuzzManifestLayerIdEvent);
		case PS_MANIFEST_SET_REMOTE_LOCATION_LOCAL:
			return (int) sizeof(FuzzManifestRemoteLocationEvent);
		case PS_MANIFEST_SET_FLUSH_WATERMARK_LOCAL:
		case PS_MANIFEST_REBASE_FLUSH_WATERMARK_LOCAL:
			return (int) sizeof(FuzzFlushWatermark);
		default:
			return -1;
	}
}

/*
 * Round-6 coordinator review, full-audit finding: local_recover_local_
 * layers() (pagestore_layer_store.c) rejects the whole open if *any*
 * ADD_LAYER location's recorded size disagrees with its real on-disk
 * file's actual length -- the same gate fixup_manifest_layer_size() below
 * already satisfies from the image_layer target's side (patching the
 * manifest to match a mutated layer file), but nothing previously kept
 * the *manifest* target's own mutations of a location's `size` field in
 * sync with the real, unmutated layer file it names. Since layer_0_...0a/
 * _0b never change size for this target (only layers.manifest itself is
 * the fuzzed file), the real length is just a template lookup by
 * basename -- no live-directory I/O needed, unlike the cross-file fixups
 * that patch a *different* file. */
static void
manifest_sync_add_layer_location_sizes(uint8_t *buf, size_t record_off,
										uint32_t declared_len)
{
	FuzzManifestLayerDisk layer;
	uint32_t	j;

	if (declared_len != sizeof(FuzzManifestLayerDisk))
		return;
	memcpy(&layer, buf + record_off + PS_MANIFEST_HEADER_BYTES_LOCAL,
		   sizeof(layer));
	for (j = 0; j < layer.location_count && j < PS_LAYER_MAX_LOCATIONS_LOCAL;
		 j++)
	{
		const FuzzManifestLocationDisk *loc = &layer.locations[j];
		const uint8_t *tmpl;
		size_t		tmpl_len;
		char		basename[PS_LAYER_URI_MAX_LOCAL];
		size_t		urilen;
		const char *slash;
		size_t		loc_off;
		unsigned	i;

		if (loc->tier != PS_LAYER_TIER_LOCAL_HOT_LOCAL &&
			loc->tier != PS_LAYER_TIER_LOCAL_COLD_LOCAL)
			continue;
		urilen = strnlen(loc->uri, sizeof(loc->uri));
		if (urilen == 0 || urilen >= sizeof(basename))
			continue;
		memcpy(basename, loc->uri, urilen);
		basename[urilen] = '\0';
		slash = strrchr(basename, '/');
		tmpl = ps_fuzz_template_lookup(slash ? slash + 1 : basename, &tmpl_len);
		if (tmpl == NULL)
			continue;			/* not a real template file: leave as-is */
		loc_off = record_off + PS_MANIFEST_HEADER_BYTES_LOCAL +
			offsetof(FuzzManifestLayerDisk, locations) +
			(size_t) j * sizeof(FuzzManifestLocationDisk) +
			offsetof(FuzzManifestLocationDisk, size);
		for (i = 0; i < 8; i++)
			buf[loc_off + i] = (unsigned char) ((uint64_t) tmpl_len >> (i * 8));
	}
}

/* ---- manifest: layers.manifest (pagestore_manifest.c) -----------------
 * Sequence of [PsManifestRecord header (20 bytes: magic,version,type,len,
 * crc) | 'len' bytes of payload].  crc = FNV-1a over the header's first 16
 * bytes (offsetof(PsManifestRecord, crc)), then the payload
 * (manifest_record_crc()).  Self-sized: len picks where the next record
 * starts, so a fixup must recompute crc using the record's own (possibly
 * fuzzed) len, clamped to what is actually left in the buffer. */
static void
fixup_manifest(uint8_t *buf, size_t len)
{
	size_t		off = 0;

	while (off + 20 <= len)
	{
		uint32_t	type = get_le32(buf + off + 8);
		uint32_t	declared_len = get_le32(buf + off + 12);
		size_t		payload_len;
		int			expected_len = manifest_type_payload_len_local(type);
		uint32_t	crc;

		/* A record's len must exactly equal its type's fixed payload size
		 * (see manifest_type_payload_len_local()'s comment); correct it
		 * here when the type is recognized and its size still fits what is
		 * actually left in the buffer, so a len mutation alone no longer
		 * rejects an otherwise-intact record. An unrecognized type, or one
		 * whose size no longer fits, is left exactly as before. */
		if (expected_len >= 0 && (size_t) expected_len <= len - off - 20)
		{
			declared_len = (uint32_t) expected_len;
			put_le32(buf + off + 12, declared_len);
		}
		payload_len = declared_len;

		if (payload_len > len - off - 20)
			payload_len = len - off - 20;
		else if (type == PS_MANIFEST_ADD_LAYER_LOCAL)
			manifest_sync_add_layer_location_sizes(buf, off, declared_len);
		crc = fnv1a_step(FNV1A_INIT, buf + off, 16);
		if (payload_len > 0)
			crc = fnv1a_step(crc, buf + off + 20, payload_len);
		put_le32(buf + off + 16, crc);
		/* Advance by the record's own declared size (matching how the
		 * reader frames records); a declared len that ran past the buffer
		 * consumed the rest of it as payload above, so the loop ends. */
		if (declared_len > len - off - 20)
			break;
		off += 20 + declared_len;
	}
}

/* ---- forkmeta (pagestore_core.c ForkMetaRecV2) -------------------------
 * Fixed 64-byte records.  FORK_META_V3_MAGIC ("FKM3") records carry a
 * CRC-24 (OpenPGP polynomial) in the 3 low bytes of `pad`, computed over
 * every byte before `pad` (fork_meta_rec_crc24()/fork_meta_rec_seal()).
 * FORK_META_V2_MAGIC ("FKM2") records carry no checksum (left untouched).
 * Mirrors ForkMetaRecV2 exactly (same field order/types as
 * pagestore_core.c), using the shared PsKey type so the layout matches by
 * construction. */
typedef struct FuzzForkMetaRecV2
{
	uint32_t	magic;
	uint32_t	rec_len;
	uint32_t	timeline;
	PsKey		key;
	uint64_t	lsn;
	uint64_t	admission_seq;
	uint64_t	order_id;
	uint32_t	nblocks;
	uint8_t		kind;
	uint8_t		pad[3];
} FuzzForkMetaRecV2;

#define FORK_META_V2_MAGIC_LOCAL 0x324d4b46u /* "FKM2" */
#define FORK_META_V3_MAGIC_LOCAL 0x334d4b46u /* "FKM3" */

static uint32_t
crc24_openpgp(const unsigned char *bytes, size_t n)
{
	uint32_t	crc = 0xB704CEu;

	for (size_t i = 0; i < n; i++)
	{
		crc ^= (uint32_t) bytes[i] << 16;
		for (int bit = 0; bit < 8; bit++)
		{
			crc <<= 1;
			if (crc & 0x1000000u)
				crc ^= 0x1864CFBu;
		}
	}
	return crc & 0xFFFFFFu;
}

/* Recompute every FKM3 record's crc-24 in a buffer of fixed 64-byte
 * ForkMetaRecV2 records -- the shared loop fixup_forkmeta() (the "forkmeta"
 * target's own top-level fixup, below) and fixup_forkmeta_snapshot_part()
 * (the checkpoint/tail targets, whose payload is framed records with no
 * "record 0 is the snapshot-base marker" meaning) both need, but only the
 * former also owns record-0 identity-pinning: applying that to a
 * checkpoint/tail payload's first record would be wrong -- it is an
 * ordinary event record, not a marker, and pinning it to marker identity
 * would just make fork_meta_snapshot_record_valid()'s ordering/semantic
 * checks fail instead.
 *
 * Codex round-4-audit finding: fork_meta_rec_wire_valid() (pagestore_core.c)
 * requires an FKM2 (legacy, no-checksum) record's `pad` to be exactly zero
 * -- unlike FKM3, FKM2 has no crc-24 to make a mutated pad self-consistent
 * with, so a nonzero pad there was rejected outright regardless of every
 * other field. Zeroed here rather than left "no checksum, nothing to fix"
 * like the rest of an FKM2 record. */
static void
fixup_forkmeta_records(uint8_t *buf, size_t len)
{
	size_t		stride = sizeof(FuzzForkMetaRecV2);
	size_t		crc_off = offsetof(FuzzForkMetaRecV2, pad);

	for (size_t off = 0; off + stride <= len; off += stride)
	{
		uint32_t	magic = get_le32(buf + off);

		/*
		 * Codex finding on PR #303 (round 7, gate-sweep):
		 * fork_meta_rec_wire_valid() (pagestore_core.c) checks
		 * `rec->rec_len != sizeof(*rec)` *first*, before magic, pad, or
		 * crc -- an unrecognized rec_len rejects the record outright
		 * regardless of everything else this function already fixes up.
		 * This function commits to treating every stride-aligned chunk as
		 * one fixed-size ForkMetaRecV2 record (the same shape FKM2 and
		 * FKM3 both share), so rec_len is pinned to that one size for
		 * every record whose magic it recognizes -- the same reasoning
		 * already applied to rec_len/header_bytes-style fields elsewhere
		 * in this file. A record whose magic is neither FKM2 nor FKM3 is
		 * left alone: it already fails wire_valid() on magic regardless,
		 * so pinning its rec_len would not change reachability and would
		 * cost coverage of that independent identity gate.
		 */
		if (magic == FORK_META_V2_MAGIC_LOCAL)
		{
			put_le32(buf + off + offsetof(FuzzForkMetaRecV2, rec_len),
					 (uint32_t) stride);
			memset(buf + off + crc_off, 0, 3);
		}
		else if (magic == FORK_META_V3_MAGIC_LOCAL)
		{
			uint32_t	crc;

			put_le32(buf + off + offsetof(FuzzForkMetaRecV2, rec_len),
					 (uint32_t) stride);
			crc = crc24_openpgp(buf + off, crc_off);
			buf[off + crc_off + 0] = (uint8_t) (crc >> 16);
			buf[off + crc_off + 1] = (uint8_t) (crc >> 8);
			buf[off + crc_off + 2] = (uint8_t) crc;
		}
	}
}

/*
 * Round-6 coordinator review, full-audit finding, investigated and
 * deliberately *not* bypassed: whenever a forkmeta snapshot manifest
 * exists (always true in this harness's fixture),
 * fork_meta_snapshot_reconcile_source() (pagestore_core.c) requires this
 * log's very first record to be an exact-field match for the selected
 * snapshot's FEV_SNAPSHOT_BASE marker (fork_meta_snapshot_marker_matches():
 * timeline, key, lsn, admission_seq, order_id, nblocks, kind), or it falls
 * back to fork_meta_source_conflicts_with_snapshot() and, finding none (an
 * ordinary early event's lsn/admission_seq predates the freeze), silently
 * *rewrites the whole log to just the marker* -- ps_core_open() then
 * carries on through every later open step regardless.
 *
 * An earlier version of this fixup pinned record 0's identity fields to
 * the real marker whenever it declared FKM3, to make the "already
 * matches" fast path exercise fork_meta_selected_suffix_valid() on the
 * rest of the log instead of being discarded. Measured with before/after
 * -runs=0 on the real corpus, this made things *worse*: three of this
 * target's four seeds are real FKM3 FEV_GROW records (an ordinary event,
 * not a marker) with lsn/admission_seq values that predate any snapshot
 * cutoff, so once record 0 matched, fork_meta_selected_suffix_valid()
 * immediately rejected record 1 for not being "future" relative to the
 * cutoff -- failing the *entire* ps_core_open() right here and cutting off
 * every later open step this target used to reach via the silent-rewrite
 * path (cov 1925/ft 2018 -> cov 1911/ft 1916 on the unmodified corpus).
 * Left alone: the discard-and-rewrite fallback already lets the open
 * succeed and explore everything downstream; that is worth more than
 * exercising this one gate's suffix-validation branch. */
static void
fixup_forkmeta(uint8_t *buf, size_t len)
{
	fixup_forkmeta_records(buf, len);
}

/* ---- retention.meta (pagestore_retention.c PsRetentionRecord) ---------
 * Fixed-size records: magic,version,type,len (16 bytes) + PsRetentionPin
 * (40 bytes, shared type) + crc(4) + pad(4) = 64 bytes.  crc = FNV-1a over
 * offsetof(crc) = 56 leading bytes (retention_record_crc()).
 *
 * Round-6 coordinator review: ps_retention_open() (pagestore_retention.c)
 * replays every complete record here, folding an FNV-1a hash across them
 * (retention_fnv1a(), same constants as fnv1a_step()/FNV1A_INIT below) and
 * counting them, then requires that (count, hash) to equal the sibling
 * retention.state's committed (nrecords, log_hash) *exactly* -- any
 * mismatch is a hard "state nrecords != log" failure, independent of
 * whether every individual record's own crc (fixed above) and semantic
 * fields are otherwise fine. retention_meta_derive_state() computes what
 * the mutated log's (nrecords, log_hash) actually are and patches the
 * live retention.state to match, the same cross-file shape used
 * throughout this file. */
typedef struct FuzzRetentionRecord
{
	uint32_t	magic;
	uint32_t	version;
	uint32_t	type;
	uint32_t	len;
	PsRetentionPin pin;
	uint32_t	crc;
	uint32_t	pad;
} FuzzRetentionRecord;

/* ---- retention.state (pagestore_retention.c PsRetentionState) ---------
 * Single fixed struct: magic,version(8) + nrecords(8) + log_hash(4) +
 * crc(4).  crc = FNV-1a over offsetof(crc) leading bytes
 * (retention_state_crc()). */
typedef struct FuzzRetentionState
{
	uint32_t	magic;
	uint32_t	version;
	uint64_t	nrecords;
	uint32_t	log_hash;
	uint32_t	crc;
} FuzzRetentionState;

/*
 * Replay a retention.meta-shaped buffer exactly the way
 * ps_retention_open()'s read loop does, just to compute the (nrecords,
 * log_hash) its sibling retention.state needs to describe it: every
 * complete FuzzRetentionRecord-sized stride, hashed in as FNV-1a chained
 * from FNV1A_INIT over the *whole* record (crc field included, matching
 * retention_fnv1a(retention_log_hash, &rec, sizeof(rec)) in the product's
 * loop -- it hashes whatever crc ended up on disk, which fixup_retention_
 * meta() has already made self-consistent by the time this runs on that
 * side). A trailing partial record is simply not counted, same as the
 * product's own `off + sizeof(rec) <= st.st_size` loop bound. This does
 * not replicate retention_record_valid()'s semantic field checks (owner
 * kind/resources/lsn/...) -- a record that fails those still hard-fails
 * ps_retention_open() regardless of what (nrecords, log_hash) says, the
 * same "give up, still a valid fuzz input" case used everywhere else in
 * this file.
 */
static void
retention_meta_derive_state(const uint8_t *buf, size_t len,
							 uint64_t *nrecords_out, uint32_t *hash_out)
{
	size_t		stride = sizeof(FuzzRetentionRecord);
	uint64_t	nrecords = 0;
	uint32_t	hash = FNV1A_INIT;
	size_t		off;

	for (off = 0; off + stride <= len; off += stride)
	{
		hash = fnv1a_step(hash, buf + off, stride);
		nrecords++;
	}
	*nrecords_out = nrecords;
	*hash_out = hash;
}

static void
fixup_retention_meta(const char *work_dir, uint8_t *buf, size_t len)
{
	size_t		stride = sizeof(FuzzRetentionRecord);
	size_t		crc_off = offsetof(FuzzRetentionRecord, crc);
	uint64_t	nrecords;
	uint32_t	hash;
	char		path[4096];
	unsigned char state[sizeof(FuzzRetentionState)];
	int			fd;

	for (size_t off = 0; off + stride <= len; off += stride)
	{
		uint32_t	crc;

		/*
		 * Codex finding on PR #303 round 4: retention_record_valid()
		 * (pagestore_retention.c) also requires `pad` (the four bytes after
		 * `crc`, outside what the crc itself covers) to be exactly zero --
		 * a record with a mutated, nonzero pad still stopped at that gate
		 * regardless of a correct crc. Zeroed before both the crc and the
		 * sibling retention.state (nrecords, log_hash) derivation below,
		 * which hashes this whole record (pad included) the same way the
		 * product's own replay loop does.
		 *
		 * Codex finding on PR #303 (round 6, gate-table audit): the same
		 * function also requires `len` to equal exactly sizeof(*rec) (64)
		 * -- checked in the same `||` chain as pad, magic, and version,
		 * independent of crc -- and ps_retention_open()'s own outer header
		 * check (pagestore_retention.c) reads *record 0's* len first, to
		 * pick the legacy-vs-current record shape for the whole file, and
		 * hard-fails the entire open ("unrecognized version ... record
		 * size ...") if it does not match sizeof(PsRetentionRecord)
		 * either. A len mutation was previously left fuzzer-controlled
		 * like magic/version/type, but unlike those, len is not a
		 * meaningful identity value to fuzz here -- every valid record of
		 * this fixed-size format has the same len by construction, so
		 * pinning it costs no coverage, the same reasoning already applied
		 * to walidx/timelines/wal_segment rec_len-style fields elsewhere in
		 * this file. Pinned before the crc, for the same reason pad is.
		 */
		put_le32(buf + off + offsetof(FuzzRetentionRecord, len),
				 (uint32_t) stride);
		memset(buf + off + offsetof(FuzzRetentionRecord, pad), 0, 4);
		crc = fnv1a_step(FNV1A_INIT, buf + off, crc_off);
		put_le32(buf + off + crc_off, crc);
	}

	retention_meta_derive_state(buf, len, &nrecords, &hash);
	if (snprintf(path, sizeof(path), "%s/retention.state", work_dir) >=
		(int) sizeof(path))
		return;
	fd = open(path, O_RDWR);
	if (fd < 0)
		return;
	if (read(fd, state, sizeof(state)) != (ssize_t) sizeof(state))
	{
		close(fd);
		return;
	}
	for (unsigned i = 0; i < 8; i++)
		state[offsetof(FuzzRetentionState, nrecords) + i] =
			(unsigned char) (nrecords >> (i * 8));
	put_le32(state + offsetof(FuzzRetentionState, log_hash), hash);
	put_le32(state + offsetof(FuzzRetentionState, crc),
			 fnv1a_step(FNV1A_INIT, state, offsetof(FuzzRetentionState, crc)));
	if (pwrite(fd, state, sizeof(state), 0) != (ssize_t) sizeof(state))
	{
		/* best-effort: an iteration that fails this write just runs
		 * without the cross-file fixup, same as
		 * fixup_forkmeta_snapshot_part() */
	}
	close(fd);
}

static void
fixup_retention_state(uint8_t *buf, size_t len)
{
	size_t		crc_off = offsetof(FuzzRetentionState, crc);
	const uint8_t *meta_tmpl;
	size_t		meta_tmpl_len;

	if (len < sizeof(FuzzRetentionState))
		return;
	/* The sibling retention.meta is not the file this target mutates, so
	 * its pristine template bytes are exactly what a live open would
	 * replay -- derive the (nrecords, log_hash) it actually implies and
	 * pin them here, the same "pin identity fields to real captured
	 * values, leave the rest fuzzer-controlled" shape
	 * fixup_walidx_snapshot_shard() uses. */
	meta_tmpl = ps_fuzz_template_lookup("retention.meta", &meta_tmpl_len);
	if (meta_tmpl != NULL)
	{
		uint64_t	nrecords;
		uint32_t	hash;
		unsigned	i;

		retention_meta_derive_state(meta_tmpl, meta_tmpl_len, &nrecords, &hash);
		for (i = 0; i < 8; i++)
			buf[offsetof(FuzzRetentionState, nrecords) + i] =
				(unsigned char) (nrecords >> (i * 8));
		put_le32(buf + offsetof(FuzzRetentionState, log_hash), hash);
	}
	put_le32(buf + crc_off, fnv1a_step(FNV1A_INIT, buf, crc_off));
}

/* ---- timelines -------------------------------------------------------
 * Repair framing, reserved bytes and CRC for the legacy 56-byte and
 * current 64-byte lifecycle shapes, including mixed logs. The repaired
 * pass pins current caps to infinity while activation is unsupported;
 * the raw pass still exercises rejection of finite caps. Magic and the
 * lifecycle payload remain fuzzer-controlled.
 */
typedef struct FuzzTimelineRecEvent
{
	uint32_t	magic;
	uint32_t	rec_len;
	uint32_t	kind;
	uint32_t	id;
	int32_t		parent;
	uint32_t	state;
	uint64_t	branch_lsn;
	uint64_t	incarnation;
	uint64_t	parent_incarnation;
	uint32_t	crc;
	uint32_t	reserved;
} FuzzTimelineRecEvent;

static void
fixup_timelines(uint8_t *buf, size_t len)
{
	size_t off = 0;
	size_t previous_stride = sizeof(FuzzTimelineRecEvent);

	while (off + 8 <= len)
	{
		uint32_t framed_len = get_le32(buf + off + 4);
		size_t stride = framed_len;
		size_t crc_off;
		uint32_t crc;

		if (stride != 56 && stride != 64)
		{
			int next56 = off + 56 + 8 <= len &&
				get_le32(buf + off + 56) == get_le32(buf + off) &&
				(get_le32(buf + off + 60) == 56 || get_le32(buf + off + 60) == 64);
			int next64 = off + 64 + 8 <= len &&
				get_le32(buf + off + 64) == get_le32(buf + off) &&
				(get_le32(buf + off + 68) == 56 || get_le32(buf + off + 68) == 64);

			/* A length divisible by both shapes cannot identify framing.
			 * Prefer an intact following header, also in mixed logs. At
			 * EOF use the remaining record size; otherwise retain the
			 * previous shape when neither candidate has a clear header. */
			if (next56 != next64)
				stride = next64 ? 64 : 56;
			else if (len - off == 56 || len - off == 64)
				stride = len - off;
			else if ((len - off) % 64 == 0 && (len - off) % 56 != 0)
				stride = 64;
			else
				stride = previous_stride;
		}
		crc_off = stride - 8;

		if (off + stride > len)
			break;
		put_le32(buf + off + 4, (uint32_t) stride);
		if (stride == 64)
			memset(buf + off + 48, 0xff, 8); /* branch_seq */
		memset(buf + off + stride - 4, 0, 4);
		put_le32(buf + off + crc_off, 0);
		crc = fnv1a_step(FNV1A_INIT, buf + off, stride);
		put_le32(buf + off + crc_off, crc);
		previous_stride = stride;
		off += stride;
	}
}

/* ---- page-prune.frontiers / walidx-prune.frontiers (pagestore_core.c) -
 * One fixed struct per file: magic,version + a big fixed entries array +
 * crc.  crc = FNV-1a over offsetof(crc) leading bytes
 * (page_frontier_crc()/walidx_frontier_crc()).  Mirrors PsPageFrontierState
 * / PsWalIdxFrontierState using the shared PsPruneFence type so padding
 * matches by construction; sizeof()/offsetof() (not hand-computed byte
 * offsets) do the layout-sensitive work. */
#define PS_FUZZ_FRONTIER_SLOTS 2
typedef struct FuzzPageFrontierEntry
{
	uint64_t	incarnation;
	PsPruneFence fence;
} FuzzPageFrontierEntry;
typedef struct FuzzPageFrontierState
{
	uint32_t	magic;
	uint32_t	version;
	FuzzPageFrontierEntry entries[1024][PS_FUZZ_FRONTIER_SLOTS];
	uint32_t	crc;
} FuzzPageFrontierState;

typedef struct FuzzWalIdxFrontierEntry
{
	uint64_t	incarnation;
	uint64_t	frontier;
} FuzzWalIdxFrontierEntry;
typedef struct FuzzWalIdxFrontierState
{
	uint32_t	magic;
	uint32_t	version;
	FuzzWalIdxFrontierEntry entries[1024][PS_FUZZ_FRONTIER_SLOTS];
	uint32_t	crc;
} FuzzWalIdxFrontierState;

static void
fixup_page_frontier(uint8_t *buf, size_t len)
{
	size_t		crc_off = offsetof(FuzzPageFrontierState, crc);

	if (len < sizeof(FuzzPageFrontierState))
		return;
	put_le32(buf + crc_off, fnv1a_step(FNV1A_INIT, buf, crc_off));
}

static void
fixup_walidx_frontier(uint8_t *buf, size_t len)
{
	size_t		crc_off = offsetof(FuzzWalIdxFrontierState, crc);

	if (len < sizeof(FuzzWalIdxFrontierState))
		return;
	put_le32(buf + crc_off, fnv1a_step(FNV1A_INIT, buf, crc_off));
}

/* ---- walidx_<tl>_<shard>[_e<epoch>] (pagestore_core.c WalIdxRec family) -
 * A log of self-sized records that can be any of three shapes, told apart
 * by magic + declared rec_len exactly as the reader does
 * (WALIDX_MAGIC=="WIDX" with rec_len 64 -> WalIdxRec, rec_len 56 ->
 * legacy WalIdxRecV1; WALIDX_PROGRESS_MAGIC=="WIPG" -> WalIdxProgressRec,
 * fixed 1080 bytes).  Every shape's crc sits at byte offset 8 and is
 * FNV-1a over the whole record with crc zeroed
 * (walidx_rec_crc()/walidx_rec_v1_crc()/walidx_progress_crc()); an
 * unrecognized magic/rec_len combination stops the walk (leaves the rest
 * of the buffer as pure fuzzer output, same as the manifest walk). */
#define WALIDX_MAGIC_LOCAL 0x57494458u			/* "WIDX" */
#define WALIDX_PROGRESS_MAGIC_LOCAL 0x57495047u	/* "WIPG" */
#define WALIDX_REC_BYTES 64u
#define WALIDX_REC_V1_BYTES 56u
#define WALIDX_PROGRESS_BYTES 1080u /* 4*6 + 8+8+16 + PS_MAX_CHANNELS(128)*8 */

/*
 * expected_timeline: walidx_recover_one() (pagestore_core.c) requires
 * every shape's `timeline` field (offset 16 in all three -- WalIdxRec,
 * WalIdxRecV1, and WalIdxProgressRec all put it right after their common
 * magic/rec_len/crc/flags-or-reserved-or-pad prefix) to equal the
 * directory's own timeline -- a fixed, known harness constant (which
 * timeline differs by target: walidx_log_epoch's walidx_0_0_... is
 * timeline 0, walidx_log_legacy's walidx_1_0 is timeline 1 -- see
 * posix_walidx_name()'s "walidx_%u_%u" in storage_posix.c), not a sibling
 * file's content, so both callers pass their own target's real value.
 */
static void
fixup_walidx_log(uint8_t *buf, size_t len, uint32_t expected_timeline)
{
	size_t		off = 0;

	while (off + 8 <= len)
	{
		uint32_t	magic = get_le32(buf + off);
		uint32_t	rec_len = get_le32(buf + off + 4);
		size_t		stride;
		int			is_v1 = 0;

		if (magic == WALIDX_MAGIC_LOCAL && rec_len == WALIDX_REC_BYTES)
			stride = WALIDX_REC_BYTES;
		else if (magic == WALIDX_MAGIC_LOCAL && rec_len == WALIDX_REC_V1_BYTES)
		{
			stride = WALIDX_REC_V1_BYTES;
			is_v1 = 1;
		}
		else if (magic == WALIDX_PROGRESS_MAGIC_LOCAL)
			stride = WALIDX_PROGRESS_BYTES;
		else
			break;
		if (off + stride > len)
			break;
		/*
		 * Codex finding on PR #303: walidx_recover_one() (pagestore_core.c)
		 * only ever treats a WIPG record as the progress shape when its own
		 * rec_len field (offset 4, same layout slot the WIDX branches key
		 * off above) equals exactly sizeof(WalIdxProgressRec)
		 * (WALIDX_PROGRESS_BYTES) -- unlike the two WIDX branches above,
		 * this function's own shape selection for WIPG keys off the magic
		 * alone and never required rec_len to already hold that value, so a
		 * fuzzer-mutated rec_len sailed through crc recomputation and still
		 * hit the reader's `else return -1` (an unrecognized shape, not a
		 * bad-crc rejection of just this record) -- failing the whole
		 * recovery, not merely skipping the record. Pin it before the crc
		 * is (re)computed over the record, the same "pin identity, leave
		 * the rest fuzzer-controlled" shape used throughout this file.
		 */
		if (magic == WALIDX_PROGRESS_MAGIC_LOCAL)
			put_le32(buf + off + 4, WALIDX_PROGRESS_BYTES);
		/*
		 * Codex finding on PR #303 round 4: walidx_recover_one()
		 * (pagestore_core.c) rejects a V1 (56-byte) record outright when its
		 * `reserved` field (offset 12, WalIdxRecV1) is nonzero -- checked
		 * before the record is added to the index, independent of a
		 * correct crc. Only the V1 shape carries this field (WalIdxRec and
		 * WalIdxProgressRec have no equivalent), so it is zeroed only here.
		 */
		if (is_v1)
			memset(buf + off + 12, 0, 4);
		put_le32(buf + off + 16, expected_timeline);
		put_le32(buf + off + 8, 0);
		put_le32(buf + off + 8, fnv1a_step(FNV1A_INIT, buf + off, stride));
		off += stride;
	}
}

/* ---- walidx_<tl>_<shard>_e<epoch>.size (storage_posix.c
 * PosixWalIdxWatermark) -- fixed 24 bytes: magic(8),length(8),crc(4),
 * reserved(4).  crc = FNV-1a over the whole struct with crc zeroed
 * (posix_walidx_watermark_crc()). */
#define POSIX_WALIDX_WATERMARK_MAGIC_LOCAL UINT64_C(0x31524b4d58444957)
typedef struct FuzzWalIdxWatermark
{
	uint64_t	magic;
	uint64_t	length;
	uint32_t	crc;
	uint32_t	reserved;
} FuzzWalIdxWatermark;

static void
fixup_walidx_watermark(const char *work_dir, uint8_t *buf, size_t len)
{
	char		path[4096];
	struct stat st;
	int			fd;
	size_t		crc_off = offsetof(FuzzWalIdxWatermark, crc);

	if (len < sizeof(FuzzWalIdxWatermark))
		return;
	if (snprintf(path, sizeof(path), "%s/walidx_0_0_e00000000000000000001",
				 work_dir) >= (int) sizeof(path))
		return;
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0)
	{
		close(fd);
		return;
	}
	close(fd);
	for (unsigned i = 0; i < 8; i++)
		buf[offsetof(FuzzWalIdxWatermark, length) + i] =
			(unsigned char) ((uint64_t) st.st_size >> (i * 8));
	/*
	 * Proactive audit fix (PR #303 round 3): posix_walidx_watermark_read()
	 * (storage_posix.c) also requires `reserved` to be exactly zero,
	 * checked before the crc -- the same reserved-padding class as the
	 * forkmeta/walidx snapshot manifest headers, zeroed here for the same
	 * reason (magic is left fuzzer-controlled, matching this file's
	 * convention of not pinning format-identity constants).
	 */
	memset(buf + offsetof(FuzzWalIdxWatermark, reserved), 0, 4);
	put_le32(buf + crc_off, 0);
	put_le32(buf + crc_off,
			 fnv1a_step(FNV1A_INIT, buf, sizeof(FuzzWalIdxWatermark)));
}

/*
 * Codex finding on PR #303 (walidx_log_epoch, same class as the
 * page_segment finding this round): posix_walidx_epoch_reconcile_locked()
 * (storage_posix.c) is on every read path a store open takes to an epoch
 * (nonzero) log (posix_walidx_read(), used by walidx_recover_one() in
 * pagestore_core.c) -- it reads the sibling <logname>.size watermark and
 * clamps every read to that recorded length, regardless of the log file's
 * actual on-disk bytes. This harness's one epoch log fixture
 * (walidx_0_0_e00000000000000000001) starts empty, so its watermark
 * already records length 0: independent of what fixup_walidx_log() above
 * did to the record bytes, or whether it ran at all, every read of this
 * file was clamped straight back down to zero bytes before a single
 * record was parsed -- exactly the "mutated bytes never reach the parser"
 * shape as the watermark-segment page_segment finding, just via a sibling
 * length record instead of a segment scan cursor.
 *
 * Sets the sibling watermark's length (and crc) to new_length so the
 * reconcile is a no-op and the mutated bytes actually reach
 * walidx_recover_one()'s record loop. Exported (see fuzz_crc_fixup.h) and
 * called unconditionally from ps_fuzz_run_one() (fuzz_common.c) right
 * after the walidx_log_epoch target file itself is written -- in *both*
 * raw and CRC-fixup iterations, not only fixed-up ones: raw mode still
 * needs to see whatever bad crc/framing the mutation produced inside the
 * log, it just should not be denied that chance by an unrelated, always-
 * stale sibling file. new_length is always the log's own full length here
 * (not a value derived from a few input bytes to sometimes clamp shorter):
 * the fuzz input already supplies every interesting "torn" shape recover()
 * itself checks for (a short header, a body that does not fit before the
 * file's own end, ...), so a *shorter-than-physical* durable watermark
 * would only ever throw away already-written suffix bytes without
 * exercising any check this harness does not already reach some other
 * way, while making the primary "does the mutation get parsed at all"
 * property probabilistic instead of guaranteed. Kept simple, per the
 * coordinator's call, at the cost of not separately fuzzing that specific
 * watermark-below-physical-length relationship in this target.
 *
 * walidx_log_legacy (epoch 0) is not touched by this at all --
 * posix_walidx_read()/_append() special-case epoch 0 to skip reconcile
 * entirely (it is the pre-epoch lazily-created log), so that target has no
 * sibling watermark to keep in sync.
 */
void
fixup_walidx_log_epoch_watermark(const char *work_dir, uint64_t new_length)
{
	char		path[4096];
	unsigned char watermark[sizeof(FuzzWalIdxWatermark)];
	int			fd;

	if (snprintf(path, sizeof(path),
				 "%s/walidx_0_0_e00000000000000000001.size", work_dir) >=
		(int) sizeof(path))
		return;
	fd = open(path, O_RDWR);
	if (fd < 0)
		return;
	memset(watermark, 0, sizeof(watermark));
	for (unsigned i = 0; i < 8; i++)
		watermark[offsetof(FuzzWalIdxWatermark, magic) + i] =
			(unsigned char) (POSIX_WALIDX_WATERMARK_MAGIC_LOCAL >> (i * 8));
	for (unsigned i = 0; i < 8; i++)
		watermark[offsetof(FuzzWalIdxWatermark, length) + i] =
			(unsigned char) (new_length >> (i * 8));
	put_le32(watermark + offsetof(FuzzWalIdxWatermark, crc),
			 fnv1a_step(FNV1A_INIT, watermark, sizeof(watermark)));
	if (pwrite(fd, watermark, sizeof(watermark), 0) != (ssize_t) sizeof(watermark))
	{
		/* best-effort: an iteration that fails this write just runs
		 * without the cross-file fixup, same as
		 * fixup_forkmeta_snapshot_part() */
	}
	close(fd);
}

static void
fixup_walidx_log_epoch(const char *work_dir, uint8_t *buf, size_t len)
{
	(void) work_dir;
	fixup_walidx_log(buf, len, 0);		/* walidx_0_0_e...: timeline 0 */
	/* The sibling watermark is now kept in sync unconditionally by
	 * ps_fuzz_run_one() (fuzz_common.c), in every iteration -- not only a
	 * fixed-up one -- via fixup_walidx_log_epoch_watermark(); see that
	 * function's own comment. Calling it again here would just re-derive
	 * the same value from the same length a second time, not fix anything
	 * this call site's own crc/framing work still needs. */
}

/* ---- wal_segments_<tl>/wal_store_identity_v1 (pagestore_wal_store.c) --
 * Fixed 64-byte encoding: crc (metadata_crc()) at byte offset 48, FNV-1a
 * over the whole 64 bytes with bytes 48..51 zeroed. */
static void
fixup_wal_store_identity(uint8_t *buf, size_t len)
{
	unsigned char copy[64];

	if (len != 64)
		return;					/* only the exact identity encoding is fixed up */
	/*
	 * Proactive audit fix (PR #303 round 3): decode_metadata()
	 * (pagestore_wal_store.c) also requires the reserved fields at offset
	 * 12 (4 bytes), 52 (4 bytes), and 56 (8 bytes) to be exactly zero,
	 * checked before the crc -- the same reserved-padding class as the
	 * forkmeta/walidx snapshot manifest headers and the walidx watermark,
	 * zeroed here for the same reason.
	 *
	 * Codex finding on PR #303 (round 6, gate-table audit): the same
	 * function also requires header@8 to equal PS_WAL_STORE_METADATA_BYTES
	 * (64) -- a fixed product constant in that same pre-crc `||` chain,
	 * not touched before -- so a mutation to it alone still rejected the
	 * file outright regardless of a correct crc. magic@0 and version@4 are
	 * deliberately left alone, matching how every other target's format-
	 * identity fields are treated in this file; header@8 is not a
	 * meaningful identity value the same way, since this function only
	 * ever produces (and this format only ever has) one encoding length.
	 */
	put_le32(buf + 8, 64);
	memset(buf + 12, 0, 4);
	memset(buf + 52, 0, 4);
	memset(buf + 56, 0, 8);
	memcpy(copy, buf, 64);
	memset(copy + 48, 0, 4);
	put_le32(buf + 48, fnv1a_step(FNV1A_INIT, copy, 64));
}

/*
 * ---- wal_segments_<tl>/walv1_* (pagestore_wal_segment.c
 * PsWalSegmentHeader) -- 64-byte header (offsets from encode_fields():
 * payload_len@20, payload_crc@40, header_crc@44, xlp_magic@56, xlp_info@58,
 * xlp_seg_size@60) + payload.  payload_crc = FNV-1a over the payload
 * (payload_crc()); header_crc = FNV-1a over the 64-byte header with
 * header_crc zeroed (header_crc()).
 *
 * Codex finding on PR #303 (c959796): a length-changing mutation of the
 * payload leaves payload_len and the payload's own recorded PostgreSQL page-
 * header identity (xlp_magic/xlp_info/xlp_seg_size) stale.
 * ps_wal_segment_validate() checks header->payload_len == payload_len and
 * payload_identity_matches() (both in pagestore_wal_segment.c) before ever
 * looking at header_crc, so those two rejects ran ahead of the CRC gate and
 * a mutation that only broke them (not the checksums) never reached later
 * WAL parsing. This now derives both from the actual (possibly resized)
 * payload with ps_wal_segment_payload_identity() -- the same exported
 * helper ps_wal_segment_seal_with_crc() uses to seal a real segment -- so
 * the encoded envelope always matches its own payload, the same invariant
 * the product's own writer maintains. A version-1 (legacy) header always
 * records a zero identity regardless of payload (payload_identity_matches()'s
 * version_known() special case), which the zero-initialized locals already
 * give it.
 *
 * Codex finding on PR #303 (round 6): the checksum gates above are not the
 * ones this target's own reader actually gets stopped at first.
 * load_segment() (pagestore_wal_store.c) requires header.payload_len to
 * equal store->segment_size *exactly* -- WAL_IMMUTABLE_SEGMENT_BYTES ==
 * PS_WAL_SEGMENT_MIN_BYTES, a fixed product constant, not "whatever length
 * this file happens to be" -- header.segment_size to equal that same
 * constant, and the file's own on-disk size (fstat(), independent of this
 * buffer) to equal PS_WAL_SEGMENT_HEADER_BYTES + header.payload_len, all
 * three checked before a single payload byte is hashed. Deriving
 * payload_len from this buffer's own (possibly mutated) length, as above,
 * satisfies the third check (self-consistent) but essentially never the
 * first two: a length-changing mutation only rarely lands on exactly
 * PS_WAL_SEGMENT_MIN_BYTES + 64 bytes, so every such mutation failed here,
 * long before header_crc or payload_crc ever mattered.
 * ps_fuzz_run_one() (fuzz_common.c) now resizes the fuzzed buffer itself
 * (zero-padding a short input, truncating a long one) to exactly
 * ps_fuzz_wal_segment_fixed_len() *before* this function -- or the crc
 * gates above -- ever runs, but only for a fixed-up iteration (see that
 * call site's own comment for why the raw half is deliberately left
 * untouched: a length-changing mutation is exactly what this finding is
 * about proving reaches deeper parsing once fixed up, not something this
 * fixup should silently repair in raw mode). By the time this function
 * runs, len already equals that fixed length in fixup mode, so
 * payload_len = len - 64 now legitimately equals segment_size for real.
 * This also pins timeline/segment_no/start_lsn/segment_size to this
 * fixture's one real captured segment's own identity -- its only legal
 * values for a freshly opened store's first segment on timeline 0, the
 * same "pin identity, leave the rest fuzzer-controlled" shape used
 * throughout this file -- since load_segment() checks all four against the
 * store's own state before payload_crc is ever computed, independent of
 * the length fix above.
 */
static void
fixup_wal_segment(uint8_t *buf, size_t len)
{
	unsigned char header[64];
	uint32_t	payload_crc;
	uint32_t	header_crc;
	uint32_t	payload_len;
	uint16_t	xlp_magic = 0;
	uint16_t	xlp_info = 0;
	uint32_t	xlp_seg_size = 0;
	const uint8_t *tmpl;
	size_t		tmpl_len;

	if (len < 64)
		return;
	payload_len = (uint32_t) (len - 64);
	/*
	 * Codex finding on PR #303 (round 7, gate-sweep): ps_wal_segment_decode()
	 * (pagestore_wal_segment.c) also requires header_len@8 to equal
	 * PS_WAL_SEGMENT_HEADER_BYTES (64) and flags@12 to be exactly zero,
	 * both checked before payload_len, segment_identity_valid(), or
	 * header_crc ever matter. Neither varies for any record this function
	 * seals -- header_len is this format's one fixed header size and flags
	 * has no defined nonzero value yet -- so both are pinned the same way
	 * payload_len/segment_size are, not left fuzzer-controlled like
	 * magic/version.
	 */
	put_le32(buf + 8, 64);		/* header_len */
	put_le32(buf + 12, 0);		/* flags */
	put_le32(buf + 20, payload_len);
	tmpl = ps_fuzz_template_lookup(
		"wal_segments_0/walv1_1_00000000000000000000", &tmpl_len);
	if (tmpl != NULL && tmpl_len >= 56)
	{
		put_le32(buf + 16, get_le32(tmpl + 16));	/* timeline */
		put_le64(buf + 24, get_le64(tmpl + 24));	/* segment_no */
		put_le64(buf + 32, get_le64(tmpl + 32));	/* start_lsn */
		put_le64(buf + 48, get_le64(tmpl + 48));	/* segment_size */
	}
	if (get_le32(buf + 4) != PS_WAL_SEGMENT_LEGACY_VERSION)
		(void) ps_wal_segment_payload_identity(buf + 64, payload_len,
												&xlp_magic, &xlp_info,
												&xlp_seg_size);
	put_le16(buf + 56, xlp_magic);
	put_le16(buf + 58, xlp_info);
	put_le32(buf + 60, xlp_seg_size);
	payload_crc = fnv1a_step(FNV1A_INIT, buf + 64, len - 64);
	put_le32(buf + 40, payload_crc);
	memcpy(header, buf, 64);
	memset(header + 44, 0, 4);
	header_crc = fnv1a_step(FNV1A_INIT, header, 64);
	put_le32(buf + 44, header_crc);
}

/*
 * Exported for ps_fuzz_run_one() (fuzz_common.c): the wal_segment target's
 * one legal on-disk length in a fixed-up iteration --
 * PS_WAL_SEGMENT_HEADER_BYTES + PS_WAL_SEGMENT_MIN_BYTES, the same fixed
 * product constant load_segment() (pagestore_wal_store.c) calls
 * WAL_IMMUTABLE_SEGMENT_BYTES -- see fixup_wal_segment()'s own comment.
 * The caller resizes its buffer to this length before fixup_wal_segment()
 * (or any of this file's crc gates) ever runs.
 */
size_t
ps_fuzz_wal_segment_fixed_len(void)
{
	return (size_t) PS_WAL_SEGMENT_HEADER_BYTES + PS_WAL_SEGMENT_MIN_BYTES;
}

/* ---- .pagestore-nshards (pagestore_core.c, "PSS2 %u %08x\n") -----------
 * Text format: "PSS2 <count> <crc32c-of-count-hex>\n".  crc is PostgreSQL
 * CRC-32C of the 4-byte native-endian count
 * (publish_store_shard_count(): ~ps_crc32c_update(UINT32_MAX,&current,4),
 * i.e. PS_CRC32C_FIN() since FIN is XOR 0xffffffff == bitwise NOT here).
 * Only fixed up when the buffer already parses as "PSS2 <digits> <hex>";
 * a mutation that broke the textual shape explores that path instead, same
 * as every other format's "give up cleanly" fallback.
 *
 * Codex finding on PR #303 round 4: %u canonicalizes a spelling like "01"
 * to "1", so the freshly formatted text can be shorter than the mutated
 * input it replaces (`len` itself cannot shrink -- ps_fuzz_crc_fixup() has
 * no way to report a new length back to its caller, which always writes
 * exactly `len` bytes). The old fix only memcpy()'d the new (shorter) text
 * over the front of `buf`, leaving that suffix's stale bytes -- part of the
 * old, longer spelling and/or old checksum -- immediately after the new
 * checksum with no separating whitespace. validate_store_shard_count()'s
 * `sscanf(line, "PSS2 %u %x %c", ...)` reads %x greedily, so leftover hex-
 * looking digits right after the new checksum silently became part of the
 * parsed checksum value instead of stopping there, and non-hex leftovers
 * fed the trailing `%c` (making sscanf return 3, not the required 2, since
 * the literal space in the format string only skips whitespace, not
 * arbitrary text). Space-padding this tail instead is always safe: %x never
 * treats ' ' as a digit, and the format's implicit "skip whitespace" before
 * %c consumes any amount of it, so %c still fails to match at EOF exactly
 * as it does for the checksum's own real trailing newline. */
static void
fixup_store_config(uint8_t *buf, size_t len)
{
	char		text[64];
	unsigned int count;
	unsigned int old_crc;
	int			consumed = 0;
	uint32_t	crc;
	char		out[64];
	int			n;

	if (len == 0 || len >= sizeof(text))
		return;
	memcpy(text, buf, len);
	text[len] = '\0';
	if (sscanf(text, "PSS2 %u %x%n", &count, &old_crc, &consumed) != 2)
		return;
	/* Require the parse to have consumed a sane prefix so we do not rewrite
	 * trailing garbage into something that looks well-formed. */
	if (consumed <= 0 || (size_t) consumed > len)
		return;
	crc = PS_CRC32C_FIN(ps_crc32c_update(PS_CRC32C_INIT, &count, sizeof(count)));
	n = snprintf(out, sizeof(out), "PSS2 %u %08x", count, crc);
	if (n <= 0 || (size_t) n > len)
		return;
	memcpy(buf, out, (size_t) n);
	/* Blank only the bytes the canonical spelling no longer covers; the
	 * original tail (newline or trailing garbage) after `consumed` stays
	 * fuzzer-controlled. */
	if ((size_t) n < (size_t) consumed)
		memset(buf + n, ' ', (size_t) consumed - (size_t) n);
}

/* ---- layer_<shard>_<id> (pagestore_layer.c/.h PsImgFooter) -------------
 * [page data][index][PsImgFooter (32 bytes, trailing)].  data_crc/
 * index_crc = FNV-1a (img_crc()) over the data and index sections; index_
 * off (in the footer, left as whatever the fuzzer produced) is the
 * boundary between them.  Only fixed up when index_off is in range --
 * exactly the "give up, still a valid fuzz input" fallback used
 * everywhere else in this file.
 *
 * Round-5 coordinator review: ps_image_layer_read_index()/
 * ps_image_layer_verify_data() (pagestore_layer.c) never stat() this file
 * -- they read the trailing footer at a computed offset of
 * "loc->size - sizeof(footer)", where loc->size is the *manifest's*
 * recorded PsLayerLocation.size for this layer, not this file's actual
 * length.  libFuzzer inserting or removing bytes changes this file's real
 * length without touching that manifest record, so the footer read above
 * lands at the wrong offset (or the length precondition
 * "loc->size < sizeof(footer)" trips) before any of the fixed-up
 * data_crc/index_crc bytes are ever looked at.  fixup_manifest_layer_size()
 * below patches the sibling layers.manifest record to match, the same
 * cross-file shape fixup_forkmeta_snapshot_part()/
 * fixup_walidx_snapshot_shard() already use for their own manifests. */
typedef struct FuzzImgFooter
{
	uint32_t	magic;
	uint32_t	version;
	uint32_t	page_size;
	uint32_t	nrecs;
	uint64_t	index_off;
	uint32_t	data_crc;
	uint32_t	index_crc;
} FuzzImgFooter;

/*
 * Mirrors pagestore_layer.c's private PsImgIndexEntV2/PsImgIndexEntV3 (not
 * exported -- reproduced field-for-field, sizeof() doing the layout-
 * sensitive work, same convention as every other mirrored struct in this
 * file) and pagestore_layer.h's exported PsImgIndexEnt (the current,
 * version-4 shape) -- the three per-entry sizes ps_image_layer_read_index()
 * picks between by the footer's own `version` field. */
typedef struct FuzzImgIndexEntV2
{
	PsKey		key;
	uint32_t	block;
	uint64_t	lsn;
	uint64_t	data_off;
} FuzzImgIndexEntV2;
typedef struct FuzzImgIndexEntV3
{
	PsKey		key;
	uint32_t	block;
	uint64_t	lsn;
	uint64_t	data_off;
	uint64_t	growth_lsn;
	uint64_t	order_id;
	uint64_t	seg_off;
	uint32_t	seg_id;
	uint32_t	flags;
} FuzzImgIndexEntV3;
typedef struct FuzzImgIndexEntV4
{
	PsKey		key;
	uint32_t	block;
	uint64_t	lsn;
	uint64_t	admission_seq;
	uint64_t	data_off;
	uint64_t	growth_lsn;
	uint64_t	order_id;
	uint64_t	seg_off;
	uint32_t	seg_id;
	uint32_t	flags;
} FuzzImgIndexEntV4;

/*
 * Find the ADD_LAYER record in the (pristine, in-memory-cached) template
 * layers.manifest whose local location names PS_FUZZ_IMAGE_LAYER_BASENAME,
 * then patch that same record's `size` field -- and the record's own crc
 * -- in the *live* layers.manifest in work_dir to new_size.  A miss at any
 * step (no such record, a struct-size mismatch against a differently
 * shaped seed, a manifest larger than this harness ever produces) is the
 * same "give up, still a valid fuzz input" fallback used everywhere else
 * in this file.
 */
static void
fixup_manifest_layer_size(const char *work_dir, uint64_t new_size)
{
	const uint8_t *manifest_tmpl;
	size_t		manifest_tmpl_len;
	size_t		off;
	size_t		record_off = 0;
	size_t		loc_size_off = 0;
	int			found = 0;
	char		path[4096];
	unsigned char manifest[PS_FUZZ_MANIFEST_BUF_BYTES_LOCAL];
	int			fd;
	uint32_t	crc;

	manifest_tmpl = ps_fuzz_template_lookup("layers.manifest", &manifest_tmpl_len);
	if (manifest_tmpl == NULL || manifest_tmpl_len > sizeof(manifest))
		return;

	off = 0;
	while (!found && off + PS_MANIFEST_HEADER_BYTES_LOCAL <= manifest_tmpl_len)
	{
		uint32_t	type = get_le32(manifest_tmpl + off + 8);
		uint32_t	declared_len = get_le32(manifest_tmpl + off + 12);

		if (declared_len > manifest_tmpl_len - off - PS_MANIFEST_HEADER_BYTES_LOCAL)
			break;
		if (type == PS_MANIFEST_ADD_LAYER_LOCAL &&
			declared_len == sizeof(FuzzManifestLayerDisk))
		{
			FuzzManifestLayerDisk layer;
			uint32_t	j;

			memcpy(&layer, manifest_tmpl + off + PS_MANIFEST_HEADER_BYTES_LOCAL,
				   sizeof(layer));
			for (j = 0; j < layer.location_count &&
				 j < PS_LAYER_MAX_LOCATIONS_LOCAL; j++)
			{
				const FuzzManifestLocationDisk *loc = &layer.locations[j];
				size_t		urilen;
				size_t		baselen = strlen(PS_FUZZ_IMAGE_LAYER_BASENAME);

				if (loc->tier != PS_LAYER_TIER_LOCAL_HOT_LOCAL &&
					loc->tier != PS_LAYER_TIER_LOCAL_COLD_LOCAL)
					continue;
				urilen = strnlen(loc->uri, sizeof(loc->uri));
				if (urilen >= baselen &&
					strcmp(loc->uri + urilen - baselen,
						   PS_FUZZ_IMAGE_LAYER_BASENAME) == 0)
				{
					record_off = off;
					loc_size_off = PS_MANIFEST_HEADER_BYTES_LOCAL +
						offsetof(FuzzManifestLayerDisk, locations) +
						(size_t) j * sizeof(FuzzManifestLocationDisk) +
						offsetof(FuzzManifestLocationDisk, size);
					found = 1;
					break;
				}
			}
		}
		off += PS_MANIFEST_HEADER_BYTES_LOCAL + declared_len;
	}
	if (!found)
		return;

	if (snprintf(path, sizeof(path), "%s/layers.manifest", work_dir) >=
		(int) sizeof(path))
		return;
	fd = open(path, O_RDWR);
	if (fd < 0)
		return;
	if (read(fd, manifest, manifest_tmpl_len) != (ssize_t) manifest_tmpl_len)
	{
		close(fd);
		return;
	}

	for (unsigned i = 0; i < 8; i++)
		manifest[record_off + loc_size_off + i] =
			(unsigned char) (new_size >> (i * 8));
	crc = fnv1a_step(FNV1A_INIT, manifest + record_off, 16);
	crc = fnv1a_step(crc, manifest + record_off + PS_MANIFEST_HEADER_BYTES_LOCAL,
					  get_le32(manifest + record_off + 12));
	put_le32(manifest + record_off + 16, crc);

	if (pwrite(fd, manifest, manifest_tmpl_len, 0) != (ssize_t) manifest_tmpl_len)
	{
		/* best-effort: an iteration that fails this write just runs without
		 * the cross-file fixup, same as fixup_forkmeta_snapshot_part() */
	}
	close(fd);
}

static void
fixup_image_layer(const char *work_dir, uint8_t *buf, size_t len)
{
	size_t		footer_bytes = sizeof(FuzzImgFooter);
	uint8_t    *footer;
	uint64_t	index_off;
	uint32_t	data_crc;
	uint32_t	index_crc;
	uint32_t	version;
	size_t		ent_size;

	/* Sync the manifest's recorded size to this mutated file's actual
	 * length regardless of whether the footer below gets fixed up too --
	 * a length change libFuzzer made on its own (insert/delete bytes) is
	 * exactly what needs this, not just a change this function makes. */
	fixup_manifest_layer_size(work_dir, (uint64_t) len);

	if (len < footer_bytes)
		return;
	footer = buf + (len - footer_bytes);
	index_off = get_le64(footer + offsetof(FuzzImgFooter, index_off));
	if (index_off > len - footer_bytes)
		return;					/* out of range: leave fully fuzzer-controlled */

	/*
	 * Codex finding on PR #303 (round 7, gate-sweep):
	 * ps_image_layer_verify_data() (pagestore_layer.c) also requires the
	 * footer's `page_size` to equal the store's own configured page_size
	 * (pagestore_core.c's `page_size` global, PS_DEFAULT_PAGE_SIZE unless a
	 * test overrides it) -- checked before data_crc ever matters, right
	 * alongside magic/version. Unlike magic/version (left fuzzer-
	 * controlled: a real format-identity choice), page_size is this
	 * store's one fixed operating parameter, the same class as
	 * segment_size in wal_segment or header_bytes elsewhere in this file,
	 * so it is pinned.
	 */
	put_le32(footer + offsetof(FuzzImgFooter, page_size), page_size);

	/*
	 * Codex finding on PR #303 round 4: a whole-entry insertion/deletion in
	 * the index section changes its span (len - footer_bytes - index_off)
	 * without updating `nrecs`, and ps_image_layer_read_index()
	 * (pagestore_layer.c) requires nrecs*entry_size to equal that span
	 * exactly -- checked before index_crc even matters. Entry size depends
	 * on the footer's own (possibly fuzzer-mutated) version field, exactly
	 * as the reader itself picks it; an unrecognized version already fails
	 * the reader's very first check regardless of nrecs, so it is left
	 * alone. Derived only for an entry-aligned span.
	 */
	version = get_le32(footer + offsetof(FuzzImgFooter, version));
	ent_size = version == 2 ? sizeof(FuzzImgIndexEntV2) :
		version == 3 ? sizeof(FuzzImgIndexEntV3) :
		version == 4 ? sizeof(FuzzImgIndexEntV4) : 0;
	if (ent_size != 0)
	{
		uint64_t	index_span = (len - footer_bytes) - index_off;

		if (index_span % ent_size == 0)
		{
			uint64_t	derived_nrecs = index_span / ent_size;

			if (derived_nrecs > 0 && derived_nrecs <= UINT32_MAX)
				put_le32(footer + offsetof(FuzzImgFooter, nrecs),
						 (uint32_t) derived_nrecs);
		}
	}

	data_crc = fnv1a_step(FNV1A_INIT, buf, (size_t) index_off);
	index_crc = fnv1a_step(FNV1A_INIT, buf + index_off,
							(len - footer_bytes) - (size_t) index_off);
	put_le32(footer + offsetof(FuzzImgFooter, data_crc), data_crc);
	put_le32(footer + offsetof(FuzzImgFooter, index_crc), index_crc);
}

/* ---- forkmeta_snapshots/forkmeta_manifest_v1 (pagestore_forkmeta_
 * snapshot.c encode_record()) -- fixed 80-byte record.  Byte offsets are
 * the product's own explicit put_le32/put_le64 layout (not a struct), so
 * they are reproduced numerically rather than mirrored: magic@0,
 * version@4, header_bytes@8, generation@16(8), cutoff_lsn@24(8),
 * cutoff_admission_seq@32(8), checkpoint.len@40(8), checkpoint.crc@48(4),
 * tail.len@52(8), tail.crc@60(4), crc@64(4) = FNV-1a over the whole 80
 * bytes with crc zeroed. */
#define FORKMETA_SNAPSHOT_HEADER_BYTES_LOCAL 80u

/*
 * sizeof(ForkMetaSnapshotPayloadHeader) (pagestore_core.c): magic(4) +
 * version(2) + header_bytes(2) + part(4) + record_bytes(4) = 16, then eight
 * uint64_t fields (generation, cutoff_lsn, cutoff_admission_seq,
 * freeze_admission_seq, checkpoint_records, tail_records, checkpoint_bytes,
 * tail_bytes) = 64, total 80 -- coincidentally the same number as the
 * *outer* forkmeta_manifest_v1 record size above, but a different struct in
 * a different file; kept as its own constant so the two are never confused.
 */
#define FORKMETA_SNAPSHOT_PAYLOAD_HEADER_BYTES_LOCAL 80u

/* checkpoint/tail live-directory paths, relative to work_dir -- shared by
 * fixup_forkmeta_snapshot_manifest() (reads the pristine template copies)
 * and fixup_forkmeta_snapshot_part() (reads/patches the live sibling). */
static const char *const forkmeta_snapshot_part_paths[2] = {
	"forkmeta_snapshots/forkmeta_checkpoint_v1_00000000000000000001",
	"forkmeta_snapshots/forkmeta_tail_v1_00000000000000000001"
};

static void
fixup_forkmeta_snapshot_manifest(uint8_t *buf, size_t len)
{
	const char *const *part_paths = forkmeta_snapshot_part_paths;
	const uint8_t *parts[2];
	size_t		part_lens[2];
	uint64_t	generation;
	uint64_t	cutoff_lsn;
	uint64_t	cutoff_seq;

	if (len != FORKMETA_SNAPSHOT_HEADER_BYTES_LOCAL)
		return;
	for (unsigned i = 0; i < 2; i++)
	{
		parts[i] = ps_fuzz_template_lookup(part_paths[i], &part_lens[i]);
		if (parts[i] == NULL ||
			part_lens[i] < FORKMETA_SNAPSHOT_PAYLOAD_HEADER_BYTES_LOCAL)
			return;
	}

	/*
	 * Proactive audit fix (PR #303 round 3, requested alongside the two
	 * findings): read_record() (pagestore_forkmeta_snapshot.c) also requires
	 * the reserved field at offset 12 (4 bytes) and the two reserved fields
	 * at offsets 68 (4 bytes) and 72 (8 bytes) to be exactly zero, checked
	 * before the crc alongside magic/version/header_bytes. Unlike
	 * magic/version (left fuzzer-controlled in both halves throughout this
	 * file, matching the project's convention of not pinning format-
	 * identity constants), these are pure reserved padding with no
	 * fuzzer-interesting shape of their own, so the fixup half zeroes them
	 * the same way the sibling walidx_snapshot_manifest fixup does for its
	 * own reserved fields.
	 *
	 * Codex finding on PR #303 (round 6, gate-table audit): header_bytes@8
	 * itself was grouped with magic/version above and left unpinned, but on
	 * its own gate it does not belong there -- every record this function
	 * (or the product's own writer) ever produces has the same fixed
	 * header_bytes value by construction, unlike magic/version, which
	 * really do vary across a format's real revisions and are worth
	 * independently fuzzing rejection of. Left unpinned, a mutation to
	 * header_bytes alone (leaving magic/version untouched) still rejected
	 * the whole manifest outright before any of the cross-file binding
	 * below -- or a single checkpoint/tail record -- was ever reached, the
	 * same class this round's audit is about. Pinned to this file's own
	 * fixed size, the same reasoning already applied to the sibling
	 * walidx_snapshot_manifest/walidx_snapshot_shard/wal_store_identity
	 * targets' equivalent fields this same round.
	 */
	put_le32(buf + 8, FORKMETA_SNAPSHOT_HEADER_BYTES_LOCAL);
	memset(buf + 12, 0, 4);
	memset(buf + 68, 0, 12);

	/* Bind the mutated manifest back to the fixture payloads.  The raw half
	 * still exercises arbitrary identity/length/hash corruption; in the fixup
	 * half these cross-file fields must agree so loading reaches payload and
	 * record validation.  Both payload headers carry the same snapshot identity. */
	generation = get_le64(parts[0] + 16);
	cutoff_lsn = get_le64(parts[0] + 24);
	cutoff_seq = get_le64(parts[0] + 32);
	if (get_le64(parts[1] + 16) != generation ||
		get_le64(parts[1] + 24) != cutoff_lsn ||
		get_le64(parts[1] + 32) != cutoff_seq)
		return;
	for (unsigned i = 0; i < 8; i++)
	{
		buf[16 + i] = (uint8_t) (generation >> (i * 8));
		buf[24 + i] = (uint8_t) (cutoff_lsn >> (i * 8));
		buf[32 + i] = (uint8_t) (cutoff_seq >> (i * 8));
		buf[40 + i] = (uint8_t) ((uint64_t) part_lens[0] >> (i * 8));
		buf[52 + i] = (uint8_t) ((uint64_t) part_lens[1] >> (i * 8));
	}
	put_le32(buf + 48, fnv1a_step(FNV1A_INIT, parts[0], part_lens[0]));
	put_le32(buf + 60, fnv1a_step(FNV1A_INIT, parts[1], part_lens[1]));
	put_le32(buf + 64, 0);
	put_le32(buf + 64, fnv1a_step(FNV1A_INIT, buf, len));
}

/*
 * forkmeta_snapshots/forkmeta_checkpoint_v1_* and .../forkmeta_tail_v1_*:
 * each is an 80-byte ForkMetaSnapshotPayloadHeader (pagestore_core.c;
 * magic/version/header_bytes/part/record_bytes(16) +
 * generation/cutoff_lsn/cutoff_admission_seq/freeze_admission_seq/
 * checkpoint_records/tail_records/checkpoint_bytes/tail_bytes(8 each) = 80,
 * no padding) followed by a run of fixed 64-byte ForkMetaRecV2 records --
 * the exact same wire record fixup_forkmeta() already knows how to fix up.
 *
 * Two checksum layers gate this target, both bypassed by round-3's fixup:
 *   - open_validated_part() in pagestore_forkmeta_snapshot.c hashes the raw
 *     file bytes against the (len, crc) the sibling forkmeta_manifest_v1
 *     record keeps for that part, so a mutated payload must also patch that
 *     record to match -- same fixed 80-byte layout as
 *     fixup_forkmeta_snapshot_manifest() above, this time read from and
 *     written back to the live directory, since the manifest is a
 *     different file from the one this target mutates.
 *   - fork_meta_snapshot_record_valid() in pagestore_core.c then checks
 *     every FKM3 record's own CRC-24 (round-4 coordinator review: this was
 *     missing entirely, so a mutation inside a record body passed the
 *     outer manifest checksum only to fail immediately on its own stale
 *     inner checksum, never reaching the semantic/ordering checks the
 *     record parser is supposed to see).
 *
 * Codex finding on PR #303 round 2 (e402d1d9a): a third gate sits ahead of
 * both of the above. fork_meta_snapshot_load() requires *this* part's own
 * record-count field (checkpoint_records for the checkpoint part,
 * tail_records for the tail part) to satisfy
 * `file length == header_bytes(80) + nrecords * sizeof(ForkMetaRecV2)`, and
 * then requires the checkpoint and tail headers to be byte-identical from
 * generation through tail_bytes -- which, since both headers carry *all
 * four* of checkpoint_records/tail_records/checkpoint_bytes/tail_bytes
 * (not one field per part), means a record-aligned append/remove to just
 * this file also goes stale in the *sibling* part's on-disk header. This
 * derives this part's own pair from the actual resized buffer, reads the
 * sibling's own pair from its live on-disk header (unchanged), writes the
 * agreed four fields into both this buffer and the sibling file, and -- since
 * that patches the sibling file's bytes -- refreshes the sibling's own
 * whole-file hash in the manifest alongside this part's (len, crc) entry.
 *
 * Codex finding on PR #303 round 3 (10548b631): round 2 only synced the
 * last 32 bytes of the header (the two record-count/byte pairs). The
 * fields ahead of those -- magic, version, header_bytes, part, record_bytes
 * (16 bytes, offsets 0-15) and generation/cutoff_lsn/cutoff_admission_seq/
 * freeze_admission_seq (32 bytes, offsets 16-47) -- are *also* required by
 * fork_meta_snapshot_load(): the first five directly (a fixed, known shape
 * for this part), and generation/cutoff_lsn/cutoff_admission_seq both
 * directly (must equal the outer manifest's own fields) and via the same
 * checkpoint/tail byte-identical memcmp() the round-2 fix already satisfies
 * for the trailing 32 bytes, with freeze_admission_seq required nonzero and
 * matched the same way. A mutation to any of these first 48 bytes (e.g.
 * byte 16, inside generation) was rejected at `load selected forkmeta
 * snapshot` before a single record was parsed, no matter how correct the
 * trailing 32 bytes and the two checksum layers above were. Unlike
 * checkpoint_records/etc., none of these 48 bytes can legitimately differ
 * between a resized and a pristine file of the *same part* -- they are this
 * part's own fixed identity -- so they are simply copied from this part's
 * own template, the same "pin identity, leave the rest fuzzer-controlled"
 * shape used throughout this file.
 */
static void
fixup_forkmeta_snapshot_part(const char *work_dir, uint8_t *buf, size_t len,
							  int is_tail)
{
	char		path[4096];
	char		sibling_path[4096];
	unsigned char manifest[FORKMETA_SNAPSHOT_HEADER_BYTES_LOCAL];
	int			fd;
	uint32_t	part_crc;
	size_t		len_off = is_tail ? 52 : 40;
	size_t		crc_off = is_tail ? 60 : 48;
	size_t		sib_crc_off = is_tail ? 48 : 60;
	int			sib_crc_valid = 0;
	uint32_t	sib_crc = 0;
	const uint8_t *own_tmpl;
	size_t		own_tmpl_len;

	if (len > FORKMETA_SNAPSHOT_PAYLOAD_HEADER_BYTES_LOCAL)
		fixup_forkmeta_records(buf + FORKMETA_SNAPSHOT_PAYLOAD_HEADER_BYTES_LOCAL,
							   len - FORKMETA_SNAPSHOT_PAYLOAD_HEADER_BYTES_LOCAL);

	own_tmpl = ps_fuzz_template_lookup(forkmeta_snapshot_part_paths[is_tail ? 1 : 0],
										&own_tmpl_len);
	if (own_tmpl != NULL && own_tmpl_len >= 48 && len >= 48)
		memcpy(buf, own_tmpl, 48);

	if (len >= FORKMETA_SNAPSHOT_PAYLOAD_HEADER_BYTES_LOCAL &&
		(len - FORKMETA_SNAPSHOT_PAYLOAD_HEADER_BYTES_LOCAL) %
			sizeof(FuzzForkMetaRecV2) == 0 &&
		snprintf(sibling_path, sizeof(sibling_path), "%s/%s", work_dir,
				 forkmeta_snapshot_part_paths[is_tail ? 0 : 1]) <
			(int) sizeof(sibling_path))
	{
		uint64_t	nrecords_this = (uint64_t)
			(len - FORKMETA_SNAPSHOT_PAYLOAD_HEADER_BYTES_LOCAL) /
			sizeof(FuzzForkMetaRecV2);
		uint64_t	bytes_this = nrecords_this * sizeof(FuzzForkMetaRecV2);
		int			sib_fd = open(sibling_path, O_RDWR);
		struct stat sib_st;
		uint8_t    *sib_buf = NULL;

		if (sib_fd >= 0 && fstat(sib_fd, &sib_st) == 0 &&
			sib_st.st_size >=
				(off_t) FORKMETA_SNAPSHOT_PAYLOAD_HEADER_BYTES_LOCAL &&
			(sib_buf = malloc((size_t) sib_st.st_size)) != NULL &&
			read(sib_fd, sib_buf, (size_t) sib_st.st_size) ==
				(ssize_t) sib_st.st_size)
		{
			/* Offsets within the 80-byte ForkMetaSnapshotPayloadHeader:
			 * checkpoint_records@48, tail_records@56, checkpoint_bytes@64,
			 * tail_bytes@72 (8 bytes each, pagestore_core.c). */
			uint64_t	ckpt_records =
				is_tail ? get_le64(sib_buf + 48) : nrecords_this;
			uint64_t	tail_records =
				is_tail ? nrecords_this : get_le64(sib_buf + 56);
			uint64_t	ckpt_bytes =
				is_tail ? get_le64(sib_buf + 64) : bytes_this;
			uint64_t	tail_bytes =
				is_tail ? bytes_this : get_le64(sib_buf + 72);

			put_le64(buf + 48, ckpt_records);
			put_le64(buf + 56, tail_records);
			put_le64(buf + 64, ckpt_bytes);
			put_le64(buf + 72, tail_bytes);

			put_le64(sib_buf + 48, ckpt_records);
			put_le64(sib_buf + 56, tail_records);
			put_le64(sib_buf + 64, ckpt_bytes);
			put_le64(sib_buf + 72, tail_bytes);

			if (pwrite(sib_fd, sib_buf, (size_t) sib_st.st_size, 0) ==
				(ssize_t) sib_st.st_size)
			{
				sib_crc = fnv1a_step(FNV1A_INIT, sib_buf,
									  (size_t) sib_st.st_size);
				sib_crc_valid = 1;
			}
		}
		free(sib_buf);
		if (sib_fd >= 0)
			close(sib_fd);
	}

	if (snprintf(path, sizeof(path),
				 "%s/forkmeta_snapshots/forkmeta_manifest_v1", work_dir) >=
		(int) sizeof(path))
		return;
	fd = open(path, O_RDWR);
	if (fd < 0)
		return;
	if (read(fd, manifest, sizeof(manifest)) != (ssize_t) sizeof(manifest))
	{
		close(fd);
		return;
	}
	part_crc = fnv1a_step(FNV1A_INIT, buf, len);
	/* len is a 64-bit field on disk; this harness's inputs never approach
	 * that range, so the low 32 bits (put_le32) plus zeroed high half
	 * (already true after the read above only if the file already encoded
	 * a small length -- write the full 8 bytes explicitly instead). */
	for (unsigned i = 0; i < 8; i++)
		manifest[len_off + i] = (unsigned char) ((uint64_t) len >> (i * 8));
	put_le32(manifest + crc_off, part_crc);
	/* The sibling's manifest-recorded length is left untouched: patching
	 * its header in place above never changes its file length, only its
	 * content hash. */
	if (sib_crc_valid)
		put_le32(manifest + sib_crc_off, sib_crc);
	put_le32(manifest + 64, 0);
	put_le32(manifest + 64,
			 fnv1a_step(FNV1A_INIT, manifest, sizeof(manifest)));
	if (pwrite(fd, manifest, sizeof(manifest), 0) != (ssize_t) sizeof(manifest))
	{
		/* best-effort: an iteration that fails this write just runs
		 * without the cross-file fixup, same as any other skipped case */
	}
	close(fd);
}

/*
 * sizeof(WalIdxRec) (pagestore_core.c) and the byte offset of its own `crc`
 * field within that fixed-size record -- see walidx_rec_crc(): FNV-1a over
 * the whole 64-byte record with crc temporarily zeroed. */
#define WALIDX_REC_BYTES_LOCAL 64u
#define WALIDX_REC_CRC_OFF_LOCAL 8u

/* Mirrors pagestore_walidx_snapshot.c's private WALIDX_SNAPSHOT_HEADER_BYTES/
 * WALIDX_SNAPSHOT_ENTRY_BYTES/PS_WALIDX_SNAPSHOT_MAX_SHARDS (from
 * pagestore_walidx_snapshot.h) -- not exported, so reproduced numerically
 * like every other cross-file layout in this file. */
#define WALIDX_SNAPSHOT_MANIFEST_HEADER_BYTES_LOCAL 64u
#define WALIDX_SNAPSHOT_MANIFEST_ENTRY_BYTES_LOCAL 16u
#define WALIDX_SNAPSHOT_MANIFEST_MAX_SHARDS_LOCAL 128u

/* Fixed-up manifests can only name the fixture's existing shard files. */
size_t
ps_fuzz_walidx_manifest_fixed_len(void)
{
	size_t len = 0;

	(void) ps_fuzz_template_lookup("walidx_snapshots_0/walidx_manifest_v1",
								  &len);
	return len;
}

/* ---- walidx_snapshots_<tl>/walidx_manifest_v1 (pagestore_walidx_
 * snapshot.c encode_manifest()) -- header_bytes(64) + nshards*entry_bytes
 * (16) bytes, whatever the buffer's actual (possibly fuzzer-resized)
 * length is; crc@48 = FNV-1a over the whole buffer with crc zeroed
 * (read_prepared() checks stored_crc against fnv1a(...) over the buffer's
 * declared expected_len, but hashing "whatever is actually here" is the
 * only definition that make sense once nshards itself may have been
 * mutated -- see the file's own comment on this).
 *
 * Round-6 coordinator review, full-audit finding: validate_shard()
 * (pagestore_walidx_snapshot.c, via ps_walidx_snapshot_open()) rejects the
 * whole snapshot if *any* shard entry's recorded (len, crc) disagrees with
 * its real on-disk shard file -- the same gate fixup_walidx_snapshot_
 * shard() below already satisfies from the shard file's side. Nothing
 * previously kept *this* target's own mutations of an entry's len/crc in
 * sync with the real, unmutated shard file(s) it names. shard_name()
 * (product code) builds that filename from this manifest's own
 * `generation` field, so a fuzzed generation would send the real reader
 * looking for a shard file that does not exist -- this pins generation to
 * the template's real value first (same "pin identity, leave the rest
 * fuzzer-controlled" shape fixup_walidx_snapshot_shard() uses for its own
 * header), then syncs whichever shard entries name a real template file
 * (this harness has exactly one: shard 0), all without any live-directory
 * I/O -- the real shard's bytes are already in the in-memory template
 * cache under the same relpath fixup_walidx_snapshot_shard() reads. */
static void
fixup_walidx_snapshot_manifest(uint8_t *buf, size_t len)
{
	const uint8_t *manifest_tmpl;
	size_t		manifest_tmpl_len;

	if (len < 52)
		return;					/* need through the crc field itself */

	/*
	 * Audit fix alongside the Codex round-2 findings on PR #303 (e402d1d9a):
	 * the same "length/count field left stale after a resize" class those
	 * two findings named. ps_walidx_snapshot_open_internal()
	 * (pagestore_walidx_snapshot.c) requires this file's own actual length
	 * to equal HEADER_BYTES(64) + nshards*ENTRY_BYTES(16) *before* even
	 * reading the crc -- an entry-aligned length mutation left the
	 * fuzzer-mutated nshards@20 field unrelated to the buffer's actual
	 * (possibly resized) size, so it was rejected at that self-consistency
	 * gate before this function's own crc/identity fixups below ever
	 * mattered. Derived only for an entry-aligned length in range; anything
	 * else can never satisfy that equation regardless, so it is left as
	 * before.
	 */
	if (len >= WALIDX_SNAPSHOT_MANIFEST_HEADER_BYTES_LOCAL &&
		(len - WALIDX_SNAPSHOT_MANIFEST_HEADER_BYTES_LOCAL) %
			WALIDX_SNAPSHOT_MANIFEST_ENTRY_BYTES_LOCAL == 0)
	{
		uint64_t	derived_nshards = (uint64_t)
			(len - WALIDX_SNAPSHOT_MANIFEST_HEADER_BYTES_LOCAL) /
			WALIDX_SNAPSHOT_MANIFEST_ENTRY_BYTES_LOCAL;

		if (derived_nshards > 0 &&
			derived_nshards <= WALIDX_SNAPSHOT_MANIFEST_MAX_SHARDS_LOCAL)
			put_le32(buf + 20, (uint32_t) derived_nshards);
	}

	/*
	 * Proactive audit fix (PR #303 round 3, requested alongside the two
	 * findings): ps_walidx_snapshot_open_internal() also requires
	 * timeline@16 to equal the caller's timeline (always 0 -- this harness's
	 * one fixture only has a walidx_snapshots_0 directory, a fixed,
	 * known-correct value, not a sibling file's content) and the two
	 * reserved fields at 52-55 and 56-63 to be exactly zero, all checked
	 * before the crc, let alone generation/start_lsn/end_lsn or any shard
	 * entry. Neither was pinned, so a mutation to either -- fully
	 * independent of every gate already fixed up above -- still rejected
	 * the file outright. */
	/*
	 * Codex finding on PR #303 (round 6, gate-table audit): the same
	 * open_internal() check also requires header@8 to equal
	 * WALIDX_SNAPSHOT_HEADER_BYTES (64) and header@12 to equal
	 * WALIDX_SNAPSHOT_ENTRY_BYTES (16) -- two more fixed product
	 * constants in that same pre-crc `||` chain, exactly the same class
	 * as timeline@16 and the two reserved fields just pinned above, but
	 * neither was pinned: a mutation to either field alone still
	 * rejected the file outright regardless of everything else.
	 */
	if (len >= WALIDX_SNAPSHOT_MANIFEST_HEADER_BYTES_LOCAL)
	{
		put_le32(buf + 8, WALIDX_SNAPSHOT_MANIFEST_HEADER_BYTES_LOCAL);
		put_le32(buf + 12, WALIDX_SNAPSHOT_MANIFEST_ENTRY_BYTES_LOCAL);
		put_le32(buf + 16, 0);		/* timeline */
		memset(buf + 52, 0, 12);	/* reserved: must be zero */
	}

	/*
	 * Codex finding on PR #303: ps_walidx_snapshot_open_internal()
	 * (pagestore_walidx_snapshot.c) rejects the whole manifest unless every
	 * entry's own shard-index field (offset 0 in the entry, distinct from
	 * this entry's crc/len this function syncs below) equals that entry's
	 * position `i` in the array -- checked in the same per-entry loop as
	 * the crc/len gate, so a fuzzer-mutated index there rejected the file
	 * outright regardless of everything else already fixed up. This must
	 * run for *every* entry in range (not only the ones with a real
	 * template-backed shard file below, which `continue`s past entries it
	 * does not recognize): the reader's loop checks the index field for
	 * every entry from 0..nshards-1 unconditionally.
	 */
	{
		uint32_t	nshards_for_index = get_le32(buf + 20);
		uint32_t	idx;

		for (idx = 0; idx < nshards_for_index &&
			 idx < WALIDX_SNAPSHOT_MANIFEST_MAX_SHARDS_LOCAL; idx++)
		{
			size_t		entry_off = WALIDX_SNAPSHOT_MANIFEST_HEADER_BYTES_LOCAL +
				(size_t) idx * WALIDX_SNAPSHOT_MANIFEST_ENTRY_BYTES_LOCAL;

			if (entry_off + WALIDX_SNAPSHOT_MANIFEST_ENTRY_BYTES_LOCAL > len)
				break;
			put_le32(buf + entry_off, idx);
		}
	}

	manifest_tmpl = ps_fuzz_template_lookup(
		"walidx_snapshots_0/walidx_manifest_v1", &manifest_tmpl_len);
	if (manifest_tmpl != NULL && manifest_tmpl_len >= 48)
	{
		uint64_t	generation = get_le64(manifest_tmpl + 24);
		uint64_t	start_lsn = get_le64(manifest_tmpl + 32);
		uint64_t	end_lsn = get_le64(manifest_tmpl + 40);
		uint32_t	nshards = get_le32(buf + 20);
		uint32_t	i;

		/*
		 * Codex finding on PR #303 (c959796): walidx_snapshot_decode_header()
		 * (pagestore_core.c) checks the shard header's generation, start_lsn,
		 * *and* end_lsn against this manifest's own fields before a single
		 * record is read -- pinning only generation left start_lsn/end_lsn
		 * fuzzer-controlled, so a mutation of either still failed against
		 * the untouched (pristine, template-matching) shard file every
		 * time. All three cross-file identity fields are pinned to the
		 * template's real values now, the same "pin identity, leave the
		 * rest fuzzer-controlled" shape fixup_walidx_snapshot_shard() below
		 * already uses for the shard header's own identity fields.
		 */
		for (i = 0; i < 8; i++)
		{
			buf[24 + i] = (unsigned char) (generation >> (i * 8));
			buf[32 + i] = (unsigned char) (start_lsn >> (i * 8));
			buf[40 + i] = (unsigned char) (end_lsn >> (i * 8));
		}

		for (i = 0; i < nshards && i < WALIDX_SNAPSHOT_MANIFEST_MAX_SHARDS_LOCAL;
			 i++)
		{
			char		relpath[128];
			const uint8_t *shard_tmpl;
			size_t		shard_tmpl_len;
			size_t		entry_off;
			uint32_t	shard_crc;
			unsigned	b;

			if (snprintf(relpath, sizeof(relpath),
						 "walidx_snapshots_0/walidxg1_%020llu_%03u",
						 (unsigned long long) generation, i) >=
				(int) sizeof(relpath))
				continue;
			shard_tmpl = ps_fuzz_template_lookup(relpath, &shard_tmpl_len);
			if (shard_tmpl == NULL)
				continue;		/* not a real shard file: leave as-is */
			entry_off = WALIDX_SNAPSHOT_MANIFEST_HEADER_BYTES_LOCAL +
				(size_t) i * WALIDX_SNAPSHOT_MANIFEST_ENTRY_BYTES_LOCAL;
			if (entry_off + WALIDX_SNAPSHOT_MANIFEST_ENTRY_BYTES_LOCAL > len)
				break;
			shard_crc = fnv1a_step(FNV1A_INIT, shard_tmpl, shard_tmpl_len);
			put_le32(buf + entry_off + 4, shard_crc);
			for (b = 0; b < 8; b++)
				buf[entry_off + 8 + b] =
					(unsigned char) ((uint64_t) shard_tmpl_len >> (b * 8));
		}
	}

	put_le32(buf + 48, 0);
	put_le32(buf + 48, fnv1a_step(FNV1A_INIT, buf, len));
}

/*
 * walidx_snapshots_<tl>/walidxg1_<generation>_<shard> (pagestore_core.c
 * walidx_snapshot_encode_header(), WALIDX_SNAPSHOT_PAYLOAD_BYTES == 72):
 * fixed 72-byte header + a WalIdxRec array payload.  Three checksum layers
 * gate this target, all bypassed by round-3's fixup save for the first:
 *   - decode_header() (the reader) checks the header's crc *and* five
 *     identity fields (timeline, shard, generation, start_lsn, end_lsn)
 *     against values the caller already trusts from elsewhere (the sibling
 *     manifest's generation/start_lsn/end_lsn plus which shard/timeline is
 *     being recovered) -- getting only the crc right still means instant
 *     rejection on an identity mismatch, so this pins the header's identity
 *     fields to the *template* fixture's real values (read from this
 *     harness's in-memory template cache -- real captured values, not
 *     invented ones) for timeline 0/shard 0, which is what every corpus
 *     seed here already is, before recomputing the header crc.
 *   - round-4 coordinator review: ps_walidx_snapshot_open()'s
 *     validate_shard() (pagestore_walidx_snapshot.c) compares this file's
 *     whole-file length and FNV-1a checksum against the sibling
 *     walidx_manifest_v1's per-shard entry *before* decode_header() is even
 *     reached -- a payload mutation that only fixed the 72-byte header
 *     above still failed here every time, so this now also patches that
 *     entry (length + FNV-1a checksum) in the live manifest file, same
 *     read/modify/pwrite-back shape as fixup_forkmeta_snapshot_part() uses
 *     for its own sibling manifest.
 *   - round-4 coordinator review: once both of those pass, the reader's
 *     record-parsing loop (pagestore_core.c) still checks each WalIdxRec's
 *     own crc (walidx_rec_crc()) -- this now recomputes it for every fixed
 *     64-byte record after the 72-byte header, the same "zero the crc
 *     field, FNV-1a the record, write it back" shape fixup_forkmeta()/
 *     fixup_timelines() already use for their own framed records.
 * nrecords / the record payload otherwise stay entirely fuzzer-controlled.
 */
static void
fixup_walidx_snapshot_shard(const char *work_dir, uint8_t *buf, size_t len)
{
	const uint8_t *manifest_tmpl;
	size_t		manifest_tmpl_len;
	uint64_t	generation;
	uint64_t	start_lsn;
	uint64_t	end_lsn;
	size_t		off;
	char		path[4096];
	unsigned char manifest[WALIDX_SNAPSHOT_MANIFEST_HEADER_BYTES_LOCAL +
		WALIDX_SNAPSHOT_MANIFEST_MAX_SHARDS_LOCAL *
		WALIDX_SNAPSHOT_MANIFEST_ENTRY_BYTES_LOCAL];
	uint32_t	nshards;
	size_t		manifest_len;
	size_t		entry_off;
	int			entry_found;
	int			fd;
	uint32_t	shard_crc;

	if (len < 72)
		return;
	manifest_tmpl = ps_fuzz_template_lookup(
		"walidx_snapshots_0/walidx_manifest_v1", &manifest_tmpl_len);
	if (manifest_tmpl == NULL ||
		manifest_tmpl_len < WALIDX_SNAPSHOT_MANIFEST_HEADER_BYTES_LOCAL +
			WALIDX_SNAPSHOT_MANIFEST_ENTRY_BYTES_LOCAL)
		return;
	generation = get_le64(manifest_tmpl + 24);
	start_lsn = get_le64(manifest_tmpl + 32);
	end_lsn = get_le64(manifest_tmpl + 40);

	/*
	 * Codex finding on PR #303 (round 6, gate-table audit):
	 * walidx_snapshot_decode_header() (pagestore_core.c) only accepts this
	 * header's version@4 and "bytes" (header_bytes)@8 as one of three fixed
	 * pairs -- (V1, 64), (V2, 64), or (current, 72) -- and uses that same
	 * "bytes" value both as the span it hashes and to pick which of two
	 * crc offsets (56 or 64) to check against, all before this record's own
	 * per-field identity/crc checks below ever run. This function already
	 * commits to only ever fixing up the *current* (72-byte, crc@64) shape
	 * -- the crc it computes a few lines down is always `fnv1a_step(...,
	 * buf, 72)` -- so "bytes" is pinned to that shape's own fixed size for
	 * the same reason a rec_len/struct-size field is pinned elsewhere in
	 * this file: it is not a meaningful identity value to leave fuzzer-
	 * controlled on its own, since every record this function actually
	 * knows how to seal has the same value by construction. version@4 is
	 * deliberately left alone -- unlike "bytes", it *is* a meaningful,
	 * independently fuzzable identity field (which shape gets selected at
	 * all), matching how magic/version are treated everywhere else in this
	 * file.
	 */
	put_le32(buf + 8, 72);		/* header_bytes: this function's one shape */
	put_le32(buf + 12, 0);		/* timeline 0 */
	put_le32(buf + 16, 0);		/* shard 0 */
	for (unsigned i = 0; i < 8; i++)
	{
		buf[24 + i] = (unsigned char) (generation >> (i * 8));	/* generation */
		buf[32 + i] = (unsigned char) (start_lsn >> (i * 8));		/* start_lsn */
		buf[40 + i] = (unsigned char) (end_lsn >> (i * 8));		/* end_lsn */
		buf[56 + i] = (unsigned char) (generation >> (i * 8));	/* log_epoch */
	}
	memset(buf + 48, 0, 8);		/* source_offset must be 0 */

	/*
	 * Codex finding on PR #303 round 2 (e402d1d9a): a record-aligned length
	 * mutation (a complete 64-byte WalIdxRec appended or removed) left
	 * nrecords (the low 32 bits @20, the high 32 bits @68 -- see
	 * walidx_snapshot_encode_header()/decode_header() in pagestore_core.c)
	 * at whatever the fuzzer-mutated header carried. The reader computes
	 * `expected_len = header_bytes + nrecords * record_bytes` and rejects
	 * the shard if that disagrees with the manifest-recorded length (which
	 * the fixup below already pins to this buffer's real len) -- so nrecords
	 * must track the actual payload here too. Derived only for a
	 * record-aligned length; a non-aligned length can never satisfy
	 * expected_len regardless, so it is left alone like every other
	 * unrecoverable shape in this file.
	 */
	if (len >= 72 && (len - 72) % WALIDX_REC_BYTES_LOCAL == 0)
	{
		uint64_t	derived_nrecords = (uint64_t) (len - 72) /
			WALIDX_REC_BYTES_LOCAL;

		put_le32(buf + 20, (uint32_t) derived_nrecords);
		put_le32(buf + 68, (uint32_t) (derived_nrecords >> 32));
	}

	put_le32(buf + 64, 0);
	put_le32(buf + 64, fnv1a_step(FNV1A_INIT, buf, 72));

	/* Every framed WalIdxRec after the header, regardless of magic (this
	 * harness's corpus only ever contains the current fixed-size shape). */
	for (off = 72; off + WALIDX_REC_BYTES_LOCAL <= len;
		 off += WALIDX_REC_BYTES_LOCAL)
	{
		uint32_t	crc;

		/*
		 * Codex round-4-audit finding: the reader's per-record check in
		 * pagestore_core.c (the walidx_snapshot_shard recovery loop) also
		 * requires each WalIdxRec's own `timeline` (offset 16) to equal the
		 * timeline being recovered -- 0, this harness's only populated
		 * timeline, a fixed known constant like the header's own timeline
		 * field above, not a sibling file's content.
		 */
		put_le32(buf + off + 16, 0);
		put_le32(buf + off + WALIDX_REC_CRC_OFF_LOCAL, 0);
		crc = fnv1a_step(FNV1A_INIT, buf + off, WALIDX_REC_BYTES_LOCAL);
		put_le32(buf + off + WALIDX_REC_CRC_OFF_LOCAL, crc);
	}

	/* Patch the sibling manifest's shard-0 entry (length + FNV-1a checksum
	 * of this whole file) so validate_shard() lets the header above ever
	 * get read at all. */
	nshards = get_le32(manifest_tmpl + 20);
	if (nshards == 0 || nshards > WALIDX_SNAPSHOT_MANIFEST_MAX_SHARDS_LOCAL)
		return;
	manifest_len = WALIDX_SNAPSHOT_MANIFEST_HEADER_BYTES_LOCAL +
		(size_t) nshards * WALIDX_SNAPSHOT_MANIFEST_ENTRY_BYTES_LOCAL;
	if (manifest_len != manifest_tmpl_len || manifest_len > sizeof(manifest))
		return;

	if (snprintf(path, sizeof(path),
				 "%s/walidx_snapshots_0/walidx_manifest_v1", work_dir) >=
		(int) sizeof(path))
		return;
	fd = open(path, O_RDWR);
	if (fd < 0)
		return;
	if (read(fd, manifest, manifest_len) != (ssize_t) manifest_len)
	{
		close(fd);
		return;
	}

	entry_found = 0;
	entry_off = 0;
	for (uint32_t i = 0; i < nshards; i++)
	{
		size_t		e = WALIDX_SNAPSHOT_MANIFEST_HEADER_BYTES_LOCAL +
			(size_t) i * WALIDX_SNAPSHOT_MANIFEST_ENTRY_BYTES_LOCAL;

		if (get_le32(manifest + e) == 0)	/* shard 0's entry */
		{
			entry_off = e;
			entry_found = 1;
			break;
		}
	}
	if (!entry_found)
	{
		close(fd);
		return;
	}

	shard_crc = fnv1a_step(FNV1A_INIT, buf, len);
	put_le32(manifest + entry_off + 4, shard_crc);
	for (unsigned i = 0; i < 8; i++)
		manifest[entry_off + 8 + i] = (unsigned char) ((uint64_t) len >> (i * 8));
	put_le32(manifest + 48, 0);
	put_le32(manifest + 48, fnv1a_step(FNV1A_INIT, manifest, manifest_len));
	if (pwrite(fd, manifest, manifest_len, 0) != (ssize_t) manifest_len)
	{
		/* best-effort: an iteration that fails this write just runs
		 * without the cross-file fixup, same as
		 * fixup_forkmeta_snapshot_part() */
	}
	close(fd);
}

/* ---- dispatch ----------------------------------------------------------
 * wal_log (wal_<tl>, WalRecHdr) and page_segment (seg_*, SegRecHdr) carry
 * no checksum at all in this codebase -- their headers are magic/len/lsn
 * fields only, integrity coming solely from being append-only and
 * self-describing -- so there is nothing for a fixup to recompute; both
 * fall through untouched below. */
void
ps_fuzz_crc_fixup(const char *target_name, const char *work_dir,
				   uint8_t *buf, size_t len)
{
	if (target_name == NULL)
		return;
	if (strcmp(target_name, "manifest") == 0)
		fixup_manifest(buf, len);
	else if (strcmp(target_name, "forkmeta") == 0)
		fixup_forkmeta(buf, len);
	else if (strcmp(target_name, "retention_meta") == 0)
		fixup_retention_meta(work_dir, buf, len);
	else if (strcmp(target_name, "retention_state") == 0)
		fixup_retention_state(buf, len);
	else if (strcmp(target_name, "timelines") == 0)
		fixup_timelines(buf, len);
	else if (strcmp(target_name, "page_frontier") == 0)
		fixup_page_frontier(buf, len);
	else if (strcmp(target_name, "walidx_frontier") == 0)
		fixup_walidx_frontier(buf, len);
	else if (strcmp(target_name, "walidx_log_epoch") == 0)
		fixup_walidx_log_epoch(work_dir, buf, len);
	else if (strcmp(target_name, "walidx_log_legacy") == 0)
		fixup_walidx_log(buf, len, 1);	/* walidx_1_0: timeline 1 */
	else if (strcmp(target_name, "walidx_watermark") == 0)
		fixup_walidx_watermark(work_dir, buf, len);
	else if (strcmp(target_name, "wal_store_identity") == 0)
		fixup_wal_store_identity(buf, len);
	else if (strcmp(target_name, "wal_segment") == 0)
		fixup_wal_segment(buf, len);
	else if (strcmp(target_name, "store_config") == 0)
		fixup_store_config(buf, len);
	else if (strcmp(target_name, "image_layer") == 0)
		fixup_image_layer(work_dir, buf, len);
	else if (strcmp(target_name, "forkmeta_snapshot_manifest") == 0)
		fixup_forkmeta_snapshot_manifest(buf, len);
	else if (strcmp(target_name, "forkmeta_snapshot_checkpoint") == 0)
		fixup_forkmeta_snapshot_part(work_dir, buf, len, 0);
	else if (strcmp(target_name, "forkmeta_snapshot_tail") == 0)
		fixup_forkmeta_snapshot_part(work_dir, buf, len, 1);
	else if (strcmp(target_name, "walidx_snapshot_manifest") == 0)
		fixup_walidx_snapshot_manifest(buf, len);
	else if (strcmp(target_name, "walidx_snapshot_shard") == 0)
		fixup_walidx_snapshot_shard(work_dir, buf, len);
	/* wal_log, page_segment, and any other/unknown target: no checksum to
	 * fix up; left untouched. */
}
