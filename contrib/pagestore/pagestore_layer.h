/*-------------------------------------------------------------------------
 *
 * pagestore_layer.h
 *	  LSM-like immutable layer metadata for the page-store daemon.
 *
 * This file defines logical layer identity separately from physical placement.
 * A layer map answers "which immutable layers cover this key/LSN range";
 * layer-store code answers "where do the bytes come from".
 *
 *-------------------------------------------------------------------------
 */
#ifndef PAGESTORE_LAYER_H
#define PAGESTORE_LAYER_H

#include <stdbool.h>
#include <stdint.h>

#include "pagestore_ipc.h"

#define PS_LAYER_MAX_LOCATIONS	3
#define PS_LAYER_URI_MAX	512

typedef enum PsLayerKind
{
	PS_LAYER_IMAGE = 1,
	PS_LAYER_DELTA = 2,
} PsLayerKind;

typedef enum PsLayerTier
{
	PS_LAYER_TIER_LOCAL_HOT = 1,
	PS_LAYER_TIER_LOCAL_COLD = 2,
	PS_LAYER_TIER_REMOTE_OBJECT = 3,
} PsLayerTier;

typedef struct PsLayerLocation
{
	PsLayerTier tier;
	char		uri[PS_LAYER_URI_MAX];
	uint64_t	size;
	uint32_t	generation;
	bool		available;
} PsLayerLocation;

typedef struct PsLayerDesc
{
	uint64_t	layer_id;
	PsLayerKind kind;

	uint32_t	timeline;
	PsKey		start_key;
	PsKey		end_key;
	uint32_t	start_block;
	uint32_t	end_block;

	uint64_t	lsn_start;
	uint64_t	lsn_end;

	uint32_t	location_count;
	PsLayerLocation locations[PS_LAYER_MAX_LOCATIONS];

	uint64_t	created_at_lsn;
	uint64_t	remote_uploaded_lsn;
	bool		remote_durable;
	bool		local_pinned;
	bool		deleting;
	/* In-memory only: canonical local cache was materialized from remote. */
	bool		cache_resident;
	/* In-memory only: shard-0 layer created before a 1-to-N reshard. */
	bool		legacy_shard_zero;
	/* Number of snapshot readers that may materialize/read the canonical cache. */
	uint32_t	cache_readers;
	/* DROP_LOCAL is durable but its physical unlink still needs retrying. */
	bool		local_cleanup_pending;
	/* In-memory only: remote GC succeeded; retry only local cleanup. */
	bool		remote_cleanup_done;

	/*
	 * In-memory only: whether this process has verified the layer's data
	 * section against foot.data_crc (done lazily before the first page is
	 * served, so a corrupt data section fails the layer instead of serving
	 * or compacting bad bytes).  Not persisted.
	 */
	bool		data_verified;
} PsLayerDesc;

typedef struct PsLayerMap
{
	PsLayerDesc *layers;
	uint32_t	nlayers;
	uint32_t	capacity;
} PsLayerMap;

extern void ps_layer_map_init(PsLayerMap *map);
extern void ps_layer_map_free(PsLayerMap *map);
extern int	ps_layer_map_reserve(PsLayerMap *map, uint32_t capacity);
extern int	ps_layer_map_add(PsLayerMap *map, const PsLayerDesc *desc);
extern uint32_t ps_layer_map_count(const PsLayerMap *map);

/* ---------------------------------------------------------------------------
 * Image layer file format (phase 2).
 *
 * An image layer is an immutable file holding full-page versions:
 *     [ page data : full pages, back to back; identical pages may share offsets ]
 *     [ index     : nrecs * PsImgIndexEnt, sorted by (key, block, lsn) ]
 *     [ footer    : PsImgFooter, fixed-size trailer at end of file ]
 * It stores *multiple versions* per (key, block) (keyed by page_lsn), so a read
 * picks the newest version <= read_lsn -- matching the current version-chain
 * (COW) semantics, just persisted immutably.  Data, index and footer carry
 * checksums.
 * ------------------------------------------------------------------------- */
#define PS_IMG_MAGIC	0x47494d50	/* "PIMG" */
#define PS_IMG_VERSION	4	/* 4: index entries carry global admission sequence */

#define PS_IMG_REC_ORDERED	0x01
#define PS_IMG_REC_WALLESS	0x02
#define PS_IMG_REC_SEG_VALID	0x04

typedef struct PsImgIndexEnt
{
	PsKey		key;
	uint32_t	block;
	uint64_t	lsn;			/* page_lsn of this version */
	uint64_t	admission_seq;	/* global append order; 0 = legacy */
	uint64_t	data_off;		/* byte offset of the page within the file */
	uint64_t	growth_lsn;		/* segment header's fork-growth/order LSN */
	uint64_t	order_id;		/* bound ordered-record identity, else zero */
	uint64_t	seg_off;		/* page byte offset in the source segment */
	uint32_t	seg_id;
	uint32_t	flags;			/* PS_IMG_REC_* */
} PsImgIndexEnt;

typedef struct PsImgFooter
{
	uint32_t	magic;
	uint32_t	version;
	uint32_t	page_size;
	uint32_t	nrecs;
	uint64_t	index_off;		/* == page-data section size */
	uint32_t	data_crc;		/* crc of the page-data section */
	uint32_t	index_crc;		/* crc of the index section */
} PsImgFooter;

/* One full-page version to write into an image layer. */
typedef struct PsImgRec
{
	PsKey		key;
	uint32_t	block;
	uint64_t	lsn;
	uint64_t	admission_seq;
	const void *page;
	uint64_t	growth_lsn;
	uint64_t	order_id;
	uint64_t	seg_off;
	uint32_t	seg_id;
	uint32_t	flags;
} PsImgRec;

/*
 * Write 'n' page versions into image layer 'layer_id' via the layer store, seal
 * it, and fill *out with the resulting descriptor (key/block/LSN range, local
 * location).  Returns 0 on success.
 */
extern int	ps_image_layer_write(uint64_t layer_id, uint32_t timeline,
								 const PsImgRec *recs, uint32_t n,
								 uint32_t page_size, PsLayerDesc *out);

/* The callback supplies one page on demand; rec->page may be an opaque
 * source reference understood only by that callback.  Metadata is sorted in
 * memory, but page payload buffers are independent of layer size. */
typedef int (*PsImagePageReader) (const PsImgRec *rec, void *page, void *arg);
extern int ps_image_layer_write_stream(uint64_t layer_id, uint32_t timeline,
 const PsImgRec *recs, uint32_t n, uint32_t page_size,
 PsImagePageReader read_page, void *arg, PsLayerDesc *out);

/* Compaction variant shares adjacent byte-identical payloads while retaining
 * each version's index identity. Callers provide the same lifetime and
 * publication guarantees as for the ordinary streaming writer. */
extern int ps_image_layer_write_compaction_stream(uint64_t layer_id, uint32_t timeline,
	const PsImgRec *recs, uint32_t n, uint32_t page_size,
	PsImagePageReader read_page, void *arg, PsLayerDesc *out);

/* One independently sealed, unpublished output batch. Metadata must already
 * be sorted by key, block, LSN and admission sequence.  The explicit record
 * limit bounds writer memory and output size to n * (page_size + index entry)
 * plus a footer. Adjacent identical page payloads share one data offset; their
 * distinct version identities and recovery metadata remain in the index.  Publishing and retiring sources remain the caller's job. */
#define PS_IMAGE_BATCH_MAX_RECORDS 1024
extern int ps_image_layer_write_sorted_batch(uint64_t layer_id, uint32_t timeline,
 const PsImgRec *recs, uint32_t n, uint32_t page_size,
 PsImagePageReader read_page, void *arg, PsLayerDesc *out);

/*
 * Look up the newest version of (key, block) with lsn <= read_lsn in image
 * layer 'layer'.  On a hit copies page_size bytes into out, stores that
 * version's lsn in *out_lsn (if non-NULL), and returns 1; 0 if the layer has no
 * such page; -1 on read/format error.
 */
extern int	ps_image_layer_lookup(const PsLayerDesc *layer, const PsKey *key,
								  uint32_t block, uint64_t read_lsn,
								  uint64_t read_seq,
								  void *out, uint32_t page_size,
								  uint64_t *out_lsn, uint64_t *out_seq);

/* Match both identity words exactly, including sequence zero. */
extern int ps_image_layer_lookup_exact(const PsLayerDesc *layer, const PsKey *key,
		uint32_t block, uint64_t lsn, uint64_t seq, void *out,
		uint32_t page_size, uint64_t *out_lsn, uint64_t *out_seq);

/*
 * Read an image layer's full index (every (key, block, lsn) entry), for
 * rebuilding the in-memory version index at startup without scanning page data.
 * On success *out is a malloc'd array of *n entries (caller frees) and the
 * return is 0; -1 on read/format/checksum error.
 */
extern int	ps_image_layer_read_index(const PsLayerDesc *layer,
									  PsImgIndexEnt **out, uint32_t *n);
/* Bounded index traversal.  The entry pointer is valid only during the call.
 * Callbacks must stage their effects until success: a later I/O or checksum
 * error invalidates every previously visited entry. */
typedef int (*PsImageIndexVisitor)(const PsImgIndexEnt *entry, void *arg);
extern int ps_image_layer_visit_index(const PsLayerDesc *layer,
									  PsImageIndexVisitor visit, void *arg);
/* Pull sorted entries from an immutable, caller-pinned layer. next returns 1
 * for an entry, 0 only after final checksum validation, and -1 on error.
 * Results must not be published before 0. Close releases cursor memory. */
typedef struct PsImageIndexCursor PsImageIndexCursor;
extern int ps_image_index_cursor_open(const PsLayerDesc *layer,
									   PsImageIndexCursor **out);
extern int ps_image_index_cursor_next(PsImageIndexCursor *cursor,
									   PsImgIndexEnt *entry);
extern void ps_image_index_cursor_close(PsImageIndexCursor *cursor);
/* Bounded fan-in for caller-pinned image layers on one timeline. Identical
 * versions are emitted in ascending source-layer order, so the last copy
 * matches the read path's layer-ID tie break.  As with a cursor, staged
 * results are valid only if next reaches 0; close alone does not verify them. */
#define PS_IMAGE_MERGE_MAX_INPUTS 64
typedef struct PsImageIndexMerge PsImageIndexMerge;
extern int ps_image_index_merge_open(const PsLayerDesc *layers, uint32_t count,
									  PsImageIndexMerge **out);
extern int ps_image_index_merge_next(PsImageIndexMerge *merge,
									  PsImgIndexEnt *entry, uint64_t *layer_id);
extern void ps_image_index_merge_close(PsImageIndexMerge *merge);

/* Visit complete (key, block) version groups in merge order, including
 * duplicate identities and source IDs. Groups are bounded by max_group
 * (1..PS_IMAGE_GROUP_MAX_VERSIONS). Return -2 if a group exceeds that limit,
 * -1 on I/O, ordering, allocation or callback failure, and 0 on success.
 * Callback data is transient. All effects must remain staged until the whole
 * traversal succeeds: even earlier groups are invalid on a later failure.
 * Inputs must remain immutable and pinned throughout the traversal. */
#define PS_IMAGE_GROUP_MAX_VERSIONS 1024
typedef struct PsImageGroupEntry
{
	PsImgIndexEnt entry;
	uint64_t layer_id;
} PsImageGroupEntry;
typedef int (*PsImageGroupVisitor)(const PsImageGroupEntry *entries,
	uint32_t count, void *arg);
extern int ps_image_index_visit_groups(const PsLayerDesc *layers, uint32_t count,
	uint32_t max_group, PsImageGroupVisitor visit, void *arg);

/* The high 16 layer-ID bits encode the shard; the lower 48 are local IDs. */
#define PS_IMAGE_LAYER_SHARD_SHIFT 48
/* Prepare at most 64 unpublished output layers from caller-pinned local
 * sources. Legacy shard-zero sources are unsupported and rejected. The input
 * and output arrays must not overlap.
 * Output IDs must be strictly increasing and greater than every source ID.
 * 0 means every output is sealed and *noutputs describes them;
 * -1 means failure with no known remaining outputs; -3 means the output
 * capacity was exhausted and all staged files were cleaned; -2 means cleanup
 * or a failed create/write needs reconciliation. On -2, outputs[0..*noutputs)
 * are sealed files whose cleanup failed; *uncertain_id is the attempted ID
 * whose create/write outcome could not be established (0 if none). Never
 * publish a partial result. Exact duplicate identities are collapsed to the
 * winning source; distinct logical versions are retained. This API does not
 * persist a transaction. */
#define PS_IMAGE_PREPARE_MAX_OUTPUTS 64
extern int ps_image_compaction_prepare(const PsLayerDesc *layers, uint32_t count,
									  const uint64_t *output_ids, uint32_t capacity,
									  uint32_t page_size, PsLayerDesc *outputs,
									  uint32_t *noutputs, uint64_t *uncertain_id);
/* As above, also report input entries and unique retained identities on
 * success. Counts are zero on failure and do not authorize publication. */
extern int ps_image_compaction_prepare_counted(const PsLayerDesc *layers,
	uint32_t count, const uint64_t *output_ids, uint32_t capacity,
	uint32_t page_size, PsLayerDesc *outputs, uint32_t *noutputs,
	uint64_t *uncertain_id, uint64_t *scanned_out, uint64_t *kept_out);
/* Compare the complete logical version set and page bytes against prepared
 * outputs. All inputs and outputs must remain immutable and pinned for the
 * whole call. Success alone is not authority to publish or retire a source:
 * the caller must separately fence concurrent state and ensure durability.
 * Legacy shard-zero inputs and outputs are unsupported and rejected. */
extern int ps_image_compaction_verify_coverage(const PsLayerDesc *inputs,
	uint32_t ninputs, const PsLayerDesc *outputs, uint32_t noutputs,
	uint32_t page_size);
/* Check that the current map still contains exactly the captured input set
 * plus any already-published outputs. Returns 1 when it matches, 0 otherwise.
 * Caller must hold the map lock and fence relevant writers and lifecycle
 * changes; output IDs must have been reserved so later layer allocations
 * cannot reuse them or sort before them. This check does not itself pin files,
 * reserve IDs, or validate timeline state. */
extern int ps_image_compaction_snapshot_valid(const PsLayerMap *map,
	const PsLayerDesc *inputs, uint32_t ninputs,
	const PsLayerDesc *outputs, uint32_t noutputs);
/* Validate a pinned, partially retired input set and all published outputs.
 * The caller must hold its own pin on every input throughout retirement; a
 * nonzero reader count alone does not establish ownership. It must also hold
 * the map lock and writer/lifecycle fences and independently ensure coverage,
 * output durability and timeline validity. Remote-durable inputs are rejected.
 * After process restart, replan instead of resuming this in-memory snapshot. */
extern int ps_image_compaction_retirement_valid(const PsLayerMap *map,
	const PsLayerDesc *inputs, uint32_t ninputs,
	const PsLayerDesc *outputs, uint32_t noutputs, uint32_t retired_prefix);
/* Force a fresh data-section checksum verification (ignores cached state). */
/* Empty the process-wide image index cache; the layer store calls it on close. */
extern void ps_image_layer_cache_reset(void);
extern int ps_image_layer_read_index_fd(const PsLayerDesc *layer, int fd,
									  PsImgIndexEnt **out, uint32_t *n);
extern int	ps_image_layer_verify_fd(int fd, uint64_t size, uint32_t page_size);
extern int	ps_image_layer_verify_data(const PsLayerDesc *layer,
								 uint32_t page_size);
extern int	ps_delta_layer_verify_data(const PsLayerDesc *layer);

/* ---------------------------------------------------------------------------
 * Delta layer file format (phase 7).
 *
 * A delta layer is an immutable file holding redo inputs (WAL records / page
 * deltas) keyed by (key, block, record_lsn), variable length:
 *     [ delta payloads, back to back ]
 *     [ index : nrecs * PsDeltaIndexEnt, sorted by (key, block, lsn) ]
 *     [ footer : PsDeltaFooter ]
 * A read applies the deltas of (key, block) in ascending LSN order on top of a
 * base image (an image layer) to reconstruct the page as of read_lsn.  The redo
 * itself (rm_redo on a held page) is the wal-redo helper -- this format just
 * stores and serves the ordered deltas.
 * ------------------------------------------------------------------------- */
#define PS_DELTA_MAGIC		0x544c4450	/* "PDLT" */
#define PS_DELTA_VERSION	2	/* 2: index entries embed PsKey with the klass field */

typedef struct PsDeltaIndexEnt
{
	PsKey		key;
	uint32_t	block;
	uint64_t	lsn;			/* record LSN of this delta */
	uint64_t	data_off;		/* byte offset of the payload in the file */
	uint32_t	data_len;		/* payload length */
} PsDeltaIndexEnt;

typedef struct PsDeltaFooter
{
	uint32_t	magic;
	uint32_t	version;
	uint32_t	nrecs;
	uint32_t	_pad;
	uint64_t	index_off;		/* == payload section size */
	uint32_t	data_crc;
	uint32_t	index_crc;
} PsDeltaFooter;

/* One delta (redo input) to write into a delta layer. */
typedef struct PsDeltaRec
{
	PsKey		key;
	uint32_t	block;
	uint64_t	lsn;
	const void *delta;
	uint32_t	delta_len;
} PsDeltaRec;

/* A delta read back from a layer (payload pointer is into a caller-freed buf). */
typedef struct PsDeltaOut
{
	uint64_t	lsn;
	uint32_t	data_off;
	uint32_t	data_len;
} PsDeltaOut;

/* Write 'n' deltas into delta layer 'layer_id', seal it, fill *out. */
extern int	ps_delta_layer_write(uint64_t layer_id, uint32_t timeline,
								 const PsDeltaRec *recs, uint32_t n,
								 PsLayerDesc *out);

/*
 * Collect the deltas of (key, block) with lo_lsn < lsn <= hi_lsn from delta
 * layer 'layer', in ascending LSN order.  Appends up to *cap entries to outs
 * (each describing offset+len of the payload within the layer) and updates *n;
 * call ps_layer_store read_layer_block at (data_off,data_len) to fetch a
 * payload.  Returns 0 on success, -1 on read/format error.
 */
extern int	ps_delta_layer_collect(const PsLayerDesc *layer, const PsKey *key,
								   uint32_t block, uint64_t lo_lsn,
								   uint64_t hi_lsn, PsDeltaOut *outs,
								   uint32_t cap, uint32_t *n);

/* ---------------------------------------------------------------------------
 * Read plan (phase 7b): how to reconstruct (key, block) as of read_lsn from the
 * layers -- the newest image-layer version <= read_lsn (the base) plus every
 * delta in (base_lsn, read_lsn], in ascending LSN order.  The redo helper (7c)
 * applies the deltas onto the base; if there are no deltas the base *is* the
 * page.  Payloads are materialized into the plan so the consumer needs no layer
 * access.  Single timeline for now (branch ancestry is a follow-up).
 * ------------------------------------------------------------------------- */
typedef struct PsPlanDelta
{
	uint64_t	lsn;
	uint32_t	len;
	unsigned char *bytes;		/* malloc'd payload */
} PsPlanDelta;

typedef struct PsReadPlan
{
	int			has_base;
	uint64_t	base_lsn;
	unsigned char *base;		/* malloc'd page_size, or NULL if no base */
	PsPlanDelta *deltas;		/* malloc'd, ascending lsn */
	uint32_t	ndelta;
} PsReadPlan;

/*
 * Build the read plan for (timeline, key, block) as of read_lsn from 'map'.
 * Returns 0 on success (plan filled; plan->has_base may be 0 if no image covers
 * it), -1 on a read/format error.  Free with ps_read_plan_free.
 */
extern int	ps_read_plan_build(const PsLayerMap *map, uint32_t timeline,
							   const PsKey *key, uint32_t block,
							   uint64_t read_lsn, uint32_t page_size,
							   PsReadPlan *plan);
extern void ps_read_plan_free(PsReadPlan *plan);

#endif							/* PAGESTORE_LAYER_H */
