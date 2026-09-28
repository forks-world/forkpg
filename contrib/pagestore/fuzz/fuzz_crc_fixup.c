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
#include <string.h>
#include <unistd.h>

#include "pagestore_core.h"
#include "pagestore_prune.h"
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

static uint64_t
get_le64(const unsigned char *p)
{
	return (uint64_t) get_le32(p) | (uint64_t) get_le32(p + 4) << 32;
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
		size_t		payload_len = declared_len;
		uint32_t	crc;

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
 * checks fail instead. */
static void
fixup_forkmeta_records(uint8_t *buf, size_t len)
{
	size_t		stride = sizeof(FuzzForkMetaRecV2);
	size_t		crc_off = offsetof(FuzzForkMetaRecV2, pad);

	for (size_t off = 0; off + stride <= len; off += stride)
	{
		if (get_le32(buf + off) == FORK_META_V3_MAGIC_LOCAL)
		{
			uint32_t	crc = crc24_openpgp(buf + off, crc_off);

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
		uint32_t	crc = fnv1a_step(FNV1A_INIT, buf + off, crc_off);

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

/* ---- timelines (pagestore_core.c TimelineRecEvent) ---------------------
 * Fixed 60-byte self-sized records (the V1/legacy shorter shape is not
 * fixed up here -- both stay readable, this just targets the current write
 * shape, still by far the common case in a mutated corpus of V2 seeds).
 * crc = FNV-1a over the whole record with crc temporarily zeroed
 * (timeline_event_crc()), so `reserved` (after crc) is part of the hash
 * input exactly as-is. */
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
	size_t		stride = sizeof(FuzzTimelineRecEvent);
	size_t		crc_off = offsetof(FuzzTimelineRecEvent, crc);

	for (size_t off = 0; off + stride <= len; off += stride)
	{
		uint32_t	saved = get_le32(buf + off + crc_off);
		uint32_t	crc;

		put_le32(buf + off + crc_off, 0);
		crc = fnv1a_step(FNV1A_INIT, buf + off, stride);
		(void) saved;
		put_le32(buf + off + crc_off, crc);
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

static void
fixup_walidx_log(uint8_t *buf, size_t len)
{
	size_t		off = 0;

	while (off + 8 <= len)
	{
		uint32_t	magic = get_le32(buf + off);
		uint32_t	rec_len = get_le32(buf + off + 4);
		size_t		stride;

		if (magic == WALIDX_MAGIC_LOCAL && rec_len == WALIDX_REC_BYTES)
			stride = WALIDX_REC_BYTES;
		else if (magic == WALIDX_MAGIC_LOCAL && rec_len == WALIDX_REC_V1_BYTES)
			stride = WALIDX_REC_V1_BYTES;
		else if (magic == WALIDX_PROGRESS_MAGIC_LOCAL)
			stride = WALIDX_PROGRESS_BYTES;
		else
			break;
		if (off + stride > len)
			break;
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
fixup_walidx_watermark(uint8_t *buf, size_t len)
{
	size_t		crc_off = offsetof(FuzzWalIdxWatermark, crc);

	if (len < sizeof(FuzzWalIdxWatermark))
		return;
	put_le32(buf + crc_off, 0);
	put_le32(buf + crc_off,
			 fnv1a_step(FNV1A_INIT, buf, sizeof(FuzzWalIdxWatermark)));
}

/*
 * Round-6 coordinator review: for an epoch (nonzero) log,
 * posix_walidx_epoch_reconcile_locked() (storage_posix.c) is on every read
 * path a store open takes to this file (posix_walidx_read(), used by
 * walidx_recover_one() in pagestore_core.c) -- it reads the sibling
 * <logname>.size watermark and, if the log's actual on-disk length exceeds
 * the watermark's recorded length, silently ftruncate()s the log down to
 * that length *before* a single byte is parsed. This harness's one epoch
 * log fixture (walidx_0_0_e00000000000000000001) starts empty, so its
 * watermark already records length 0: every mutation that grows the log
 * (which is most of them, since the seed itself is empty) was reconciled
 * straight back down to zero bytes, and fixup_walidx_log() above never got
 * a single record in front of the reader. This patches the sibling
 * watermark's length (and crc) to the mutated log's actual length so the
 * reconcile is a no-op and the mutated bytes actually reach
 * walidx_recover_one()'s record loop. walidx_log_legacy (epoch 0) is not
 * touched by this at all -- posix_walidx_read()/_append() special-case
 * epoch 0 to skip reconcile entirely (it is the pre-epoch lazily-created
 * log), so that target has no sibling watermark to keep in sync.
 */
static void
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
	fixup_walidx_log(buf, len);
	fixup_walidx_log_epoch_watermark(work_dir, (uint64_t) len);
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
	memcpy(copy, buf, 64);
	memset(copy + 48, 0, 4);
	put_le32(buf + 48, fnv1a_step(FNV1A_INIT, copy, 64));
}

/* ---- wal_segments_<tl>/walv1_* (pagestore_wal_segment.c
 * PsWalSegmentHeader) -- 64-byte header (offsets from encode_fields():
 * payload_crc@40, header_crc@44) + payload.  payload_crc = FNV-1a over the
 * payload (payload_crc()); header_crc = FNV-1a over the 64-byte header
 * with header_crc zeroed (header_crc()). */
static void
fixup_wal_segment(uint8_t *buf, size_t len)
{
	unsigned char header[64];
	uint32_t	payload_crc;
	uint32_t	header_crc;

	if (len < 64)
		return;
	payload_crc = fnv1a_step(FNV1A_INIT, buf + 64, len - 64);
	put_le32(buf + 40, payload_crc);
	memcpy(header, buf, 64);
	memset(header + 44, 0, 4);
	header_crc = fnv1a_step(FNV1A_INIT, header, 64);
	put_le32(buf + 44, header_crc);
}

/* ---- .pagestore-nshards (pagestore_core.c, "PSS2 %u %08x\n") -----------
 * Text format: "PSS2 <count> <crc32c-of-count-hex>\n".  crc is PostgreSQL
 * CRC-32C of the 4-byte native-endian count
 * (publish_store_shard_count(): ~ps_crc32c_update(UINT32_MAX,&current,4),
 * i.e. PS_CRC32C_FIN() since FIN is XOR 0xffffffff == bitwise NOT here).
 * Only fixed up when the buffer already parses as "PSS2 <digits> <hex>";
 * a mutation that broke the textual shape explores that path instead, same
 * as every other format's "give up cleanly" fallback. */
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

static void
fixup_forkmeta_snapshot_manifest(uint8_t *buf, size_t len)
{
	if (len != FORKMETA_SNAPSHOT_HEADER_BYTES_LOCAL)
		return;
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
 */
static void
fixup_forkmeta_snapshot_part(const char *work_dir, uint8_t *buf, size_t len,
							  int is_tail)
{
	char		path[4096];
	unsigned char manifest[FORKMETA_SNAPSHOT_HEADER_BYTES_LOCAL];
	int			fd;
	uint32_t	part_crc;
	size_t		len_off = is_tail ? 52 : 40;
	size_t		crc_off = is_tail ? 60 : 48;

	if (len > FORKMETA_SNAPSHOT_PAYLOAD_HEADER_BYTES_LOCAL)
		fixup_forkmeta_records(buf + FORKMETA_SNAPSHOT_PAYLOAD_HEADER_BYTES_LOCAL,
							   len - FORKMETA_SNAPSHOT_PAYLOAD_HEADER_BYTES_LOCAL);

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

	manifest_tmpl = ps_fuzz_template_lookup(
		"walidx_snapshots_0/walidx_manifest_v1", &manifest_tmpl_len);
	if (manifest_tmpl != NULL && manifest_tmpl_len >= 32)
	{
		uint64_t	generation = get_le64(manifest_tmpl + 24);
		uint32_t	nshards = get_le32(buf + 20);
		uint32_t	i;

		for (i = 0; i < 8; i++)
			buf[24 + i] = (unsigned char) (generation >> (i * 8));

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
	put_le32(buf + 64, 0);
	put_le32(buf + 64, fnv1a_step(FNV1A_INIT, buf, 72));

	/* Every framed WalIdxRec after the header, regardless of magic (this
	 * harness's corpus only ever contains the current fixed-size shape). */
	for (off = 72; off + WALIDX_REC_BYTES_LOCAL <= len;
		 off += WALIDX_REC_BYTES_LOCAL)
	{
		uint32_t	crc;

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
		fixup_walidx_log(buf, len);
	else if (strcmp(target_name, "walidx_watermark") == 0)
		fixup_walidx_watermark(buf, len);
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
