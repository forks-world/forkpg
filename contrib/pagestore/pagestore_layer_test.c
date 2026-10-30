/*-------------------------------------------------------------------------
 *
 * pagestore_layer_test.c
 *	  Standalone unit test for the immutable image-layer file format
 *	  (pagestore_layer.c writer/reader over the local layer store).  Runs with
 *	  no daemon and no PostgreSQL.
 *
 * Usage: pagestore_layer_test
 * Exit status: 0 = all checks passed, 1 = one or more failed.
 *
 *-------------------------------------------------------------------------
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pagestore_layer.h"
#include "pagestore_compaction_prune.h"
#include "pagestore_layer_store.h"

static int	run = 0,
			failed = 0;

static void
check(int cond, const char *msg)
{
	run++;
	if (!cond)
	{
		failed++;
		fprintf(stderr, "  FAIL: %s\n", msg);
	}
}

#define PSZ 8192

typedef struct LegacyIndexV2
{
	PsKey key;
	uint32_t block;
	uint64_t lsn;
	uint64_t data_off;
} LegacyIndexV2;
typedef struct LegacyIndexV3
{
	PsKey key;
	uint32_t block;
	uint64_t lsn, data_off, growth_lsn, order_id, seg_off;
	uint32_t seg_id, flags;
} LegacyIndexV3;
static unsigned char legacy_file[1024];
static size_t legacy_file_size;

static int
legacy_memory_read(const PsLayerDesc *layer, uint64_t off, void *buf, uint32_t len)
{
	(void) layer;
	if (off > legacy_file_size || len > legacy_file_size - off)
		return -1;
	memcpy(buf, legacy_file + off, len);
	return 0;
}

static int
compare_legacy_index(const PsImgIndexEnt *entry, void *arg)
{
	PsImgIndexEnt **expected = arg;
	int different = memcmp(entry, *expected, sizeof(*entry)) != 0;
	(*expected)++;
	return different ? -1 : 0;
}

static void
legacy_index_visitor_test(void)
{
	PsLayerStore memory = PsLayerStoreLocal;
	const PsLayerStore *saved = ps_layer_store;
	PsLayerDesc layer;
	PsImgFooter foot;
	LegacyIndexV2 v2 = {.key = {1, 2, 3, 0, PS_KLASS_RELATION},
		.block = 9, .lsn = 17, .data_off = 4};
	LegacyIndexV3 v3 = {.key = {1, 2, 3, 0, PS_KLASS_RELATION},
		.block = 9, .lsn = 17, .data_off = 4, .growth_lsn = 13,
		.order_id = 23, .seg_off = 27, .seg_id = 2, .flags = 1};

	memory.read_layer_block = legacy_memory_read;
	ps_layer_store = &memory;
	for (int version = 2; version <= 3; version++)
	{
		const void *data = version == 2 ? (const void *) &v2 : (const void *) &v3;
		size_t len = version == 2 ? sizeof(v2) : sizeof(v3);
		uint32_t crc = 2166136261u;
		PsImgIndexEnt *expected = NULL, *cursor;
		PsImageIndexCursor *pull = NULL;
		PsImgIndexEnt actual;
		uint32_t count = 0;

		memcpy(legacy_file, data, len);
		for (size_t i = 0; i < len; i++)
			crc = (crc ^ legacy_file[i]) * 16777619u;
		memset(&foot, 0, sizeof(foot));
		foot.magic = PS_IMG_MAGIC; foot.version = version;
		foot.nrecs = 1; foot.index_crc = crc;
		memcpy(legacy_file + len, &foot, sizeof(foot));
		legacy_file_size = len + sizeof(foot);
		memset(&layer, 0, sizeof(layer));
		layer.location_count = 1;
		layer.locations[0].tier = PS_LAYER_TIER_LOCAL_HOT;
		layer.locations[0].size = legacy_file_size;
		layer.locations[0].available = true;
		check(ps_image_layer_read_index(&layer, &expected, &count) == 0 && count == 1,
			  "legacy fixture is accepted by the existing reader");
		cursor = expected;
		check(expected != NULL && ps_image_layer_visit_index(&layer,
							 compare_legacy_index, &cursor) == 0 && cursor == expected + 1,
			  "bounded visitor matches legacy index conversion");
		check(expected != NULL && ps_image_index_cursor_open(&layer, &pull) == 0 &&
			  ps_image_index_cursor_next(pull, &actual) == 1 &&
			  memcmp(&actual, expected, sizeof(actual)) == 0 &&
			  ps_image_index_cursor_next(pull, &actual) == 0,
			  "bounded cursor matches legacy index conversion");
		ps_image_index_cursor_close(pull);
		free(expected);
	}
	{
		LegacyIndexV2 unsorted[2] = {v2, v2};
		PsImageIndexCursor *pull = NULL;
		PsImageIndexMerge *merge = NULL;
		PsImgIndexEnt entry;
		uint64_t source = 0;
		uint32_t crc = 2166136261u;

		unsorted[1].block = unsorted[0].block - 1;
		memcpy(legacy_file, unsorted, sizeof(unsorted));
		for (size_t i = 0; i < sizeof(unsorted); i++)
			crc = (crc ^ legacy_file[i]) * 16777619u;
		memset(&foot, 0, sizeof(foot));
		foot.magic = PS_IMG_MAGIC; foot.version = 2;
		foot.nrecs = 2; foot.index_crc = crc;
		memcpy(legacy_file + sizeof(unsorted), &foot, sizeof(foot));
		legacy_file_size = sizeof(unsorted) + sizeof(foot);
		layer.locations[0].size = legacy_file_size;
		check(ps_image_index_cursor_open(&layer, &pull) == 0 &&
			  ps_image_index_cursor_next(pull, &entry) == 1 &&
			  ps_image_index_cursor_next(pull, &entry) == -1 &&
			  ps_image_index_cursor_next(pull, &entry) == -1,
			  "unsorted legacy input fails closed and stays failed");
		ps_image_index_cursor_close(pull);
		layer.kind = PS_LAYER_IMAGE;
		layer.layer_id = 1;
		check(ps_image_index_merge_open(&layer, 1, &merge) == 0 &&
			  ps_image_index_merge_next(merge, &entry, &source) == -1 &&
			  ps_image_index_merge_next(merge, &entry, &source) == -1,
			  "merge fails closed on an unsorted input");
		ps_image_index_merge_close(merge);
	}
	ps_layer_store = saved;
}

/* A generated 32-MiB layer must never require whole-payload I/O. */
static uint32_t stream_write_max;
static uint32_t stream_read_max;
static uint32_t stream_pages;
static int	stream_fail_after = -1;
static uint64_t prepare_fail_write_id;
static uint64_t prepare_fail_delete_id;
static int prepare_corrupt_source;
static uint32_t visitor_read_max;
static int visitor_corrupt;
static int visitor_first_chunk_reads;

static int
visitor_read(const PsLayerDesc *layer, uint64_t off, void *buf, uint32_t len)
{
	int rc = PsLayerStoreLocal.read_layer_block(layer, off, buf, len);
	if (len > visitor_read_max)
		visitor_read_max = len;
	if (off == (uint64_t) 4096 * PSZ)
	{
		visitor_first_chunk_reads++;
		if (rc == 0 && (visitor_corrupt == 1 ||
			(visitor_corrupt >= 2 && visitor_first_chunk_reads == visitor_corrupt)))
			((unsigned char *) buf)[0] ^= 1;
	}
	return rc;
}

static int
visit_stream_index(const PsImgIndexEnt *entry, void *arg)
{
	uint32_t *count = arg;
	if (entry->block != *count || entry->lsn != 100 ||
		entry->data_off != (uint64_t) *count * PSZ)
		return -1;
	(*count)++;
	return 0;
}

static int
reject_stream_index(const PsImgIndexEnt *entry, void *arg)
{
	uint32_t *count = arg;
	(void) entry;
	(*count)++;
	return -1;
}

static int
bounded_write(uint64_t id, uint64_t off, const void *buf, uint32_t len)
{
	if (id == prepare_fail_write_id)
		return -1;
	if (len > stream_write_max)
		stream_write_max = len;
	if (len > 256 * 1024)
		return -1;
	return PsLayerStoreLocal.write_local_layer_at(id, off, buf, len);
}

static int
bounded_delete(const PsLayerDesc *layer)
{
	if (layer->layer_id == prepare_fail_delete_id)
		return -1;
	return PsLayerStoreLocal.delete_local_layer(layer);
}

static int
bounded_read(const PsLayerDesc *layer, uint64_t off, void *buf, uint32_t len)
{
	int rc;
	/* Index metadata remains proportional to record count. */
	if (off < (uint64_t) 4096 * PSZ)
	{
		if (len > stream_read_max)
			stream_read_max = len;
		if (len > 256 * 1024)
			return -1;
	}
	rc = PsLayerStoreLocal.read_layer_block(layer, off, buf, len);
	if (rc == 0 && prepare_corrupt_source && layer->layer_id == 910 && off == 0 && len != 0)
		((unsigned char *) buf)[0] ^= 1;
	return rc;
}
static int
generated_page(const PsImgRec *rec, void *page, void *arg)
{
	(void) arg;
	if (stream_fail_after >= 0 && stream_pages >= (uint32_t) stream_fail_after)
		return -1;
	stream_pages++;
	memset(page, (unsigned char) rec->block, PSZ);
	return 0;
}

static void
streaming_layer_test(void)
{
	PsLayerStore bounded = PsLayerStoreLocal;
	const PsLayerStore *saved = ps_layer_store;
	PsImgRec   *recs = calloc(4096, sizeof(*recs));
	PsLayerDesc layer;
	unsigned char page[PSZ];
	PsKey		key = {1, 1, 905, 0, PS_KLASS_RELATION};

	check(recs != NULL, "streaming metadata allocation");
	if (!recs)
		return;
	bounded.write_local_layer_at = bounded_write;
	bounded.read_layer_block = bounded_read;
	bounded.delete_local_layer = bounded_delete;
	ps_layer_store = &bounded;
	for (uint32_t i = 0; i < 4096; i++)
	{
		recs[i].key = key;
		recs[i].block = 4095 - i;
		recs[i].lsn = 100;
	}
	check(ps_image_layer_write_stream(910, 0, recs, 4096, PSZ,
									  generated_page, NULL, &layer) == 0,
		  "stream generated 32-MiB image without resident input pages");
	check(stream_pages == 4096 && stream_write_max <= 256 * 1024,
		  "writer fetches one page per record with bounded writes");
	check(ps_image_layer_verify_data(&layer, PSZ) == 0 &&
		  stream_read_max <= 256 * 1024,
		  "large layer checksum uses bounded reads");
	{
		uint64_t ids[4] = {1010, 1011, 1012, 1013};
		uint64_t uncertain = 0;
		uint64_t wrong_shard_id = (UINT64_C(1) << PS_IMAGE_LAYER_SHARD_SHIFT) | ids[0];
		uint32_t noutputs = 0;
		PsLayerDesc outputs[4];
		PsLayerDesc mixed[2] = {layer, layer};
		PsLayerDesc legacy_layer = layer;
		int rc;

		legacy_layer.legacy_shard_zero = true;
		check(ps_image_compaction_prepare(&legacy_layer, 1, ids, 4, PSZ, outputs,
											  &noutputs, &uncertain) == -1 &&
			  noutputs == 0 && uncertain == 0,
			  "preparation rejects mixed-shard legacy sources");
		mixed[1].layer_id |= UINT64_C(1) << PS_IMAGE_LAYER_SHARD_SHIFT;
		check(ps_image_compaction_prepare(mixed, 2, ids, 4, PSZ, outputs,
											  &noutputs, &uncertain) == -1 &&
			  noutputs == 0 && uncertain == 0,
			  "preparation rejects sources from different shards");
		check(ps_image_compaction_prepare(&layer, 1, &wrong_shard_id, 1, PSZ, outputs,
											  &noutputs, &uncertain) == -1 &&
			  noutputs == 0 && uncertain == 0,
			  "preparation rejects an output ID on another shard");
		rc = ps_image_compaction_prepare(&layer, 1, ids, 4, PSZ, outputs,
												 &noutputs, &uncertain);

		check(rc == 0 && noutputs == 4 && uncertain == 0,
			  "large source prepares four bounded unpublished outputs");
		if (noutputs == 4)
		{
			PsLayerDesc present[3];
			PsLayerMap map = {.layers = present, .nlayers = 1, .capacity = 3};
			PsLayerDesc legacy_outputs[4];

			present[0] = layer;
			check(ps_image_compaction_snapshot_valid(&map, &layer, 1, outputs, 4) == 1,
				  "captured source set remains valid before publication");
			present[1] = outputs[0];
			present[1].cache_readers = 3;
			map.nlayers = 2;
			check(ps_image_compaction_snapshot_valid(&map, &layer, 1, outputs, 4) == 1,
				  "published prefix and reader pins preserve snapshot validity");
			present[2] = layer;
			present[2].layer_id = 1021;
			map.nlayers = 3;
			check(ps_image_compaction_snapshot_valid(&map, &layer, 1, outputs, 4) == 0,
				  "new source layer invalidates the preparation snapshot");
			present[2].deleting = true;
			check(ps_image_compaction_snapshot_valid(&map, &layer, 1, outputs, 4) == 1,
				  "unrelated already-deleting layer does not invalidate snapshot");
			present[2].deleting = false;
			present[2].layer_id |= UINT64_C(1) << PS_IMAGE_LAYER_SHARD_SHIFT;
			check(ps_image_compaction_snapshot_valid(&map, &layer, 1, outputs, 4) == 1,
				  "an unrelated shard does not invalidate the snapshot");
			present[2].legacy_shard_zero = true;
			check(ps_image_compaction_snapshot_valid(&map, &layer, 1, outputs, 4) == 0,
				  "a mixed-shard legacy layer invalidates the snapshot");
			present[2].deleting = true;
			present[0].remote_durable = true;
			check(ps_image_compaction_snapshot_valid(&map, &layer, 1, outputs, 4) == 0,
				  "source durability transition invalidates the snapshot");
			present[0] = layer;
			present[1].timeline++;
			check(ps_image_compaction_snapshot_valid(&map, &layer, 1, outputs, 4) == 0,
				  "output ID collision on another timeline is rejected");
			{
				PsLayerDesc sources[2] = {layer, layer}, entries[6];
				PsLayerMap guarded = {.layers = entries, .nlayers = 2, .capacity = 6};
				PsCompactionPruneFence captured = {11, 12, 13, 1, 5000};
				PsCompactionPruneFence current = captured;

				sources[1].layer_id = 911;
				entries[0] = sources[0];
				entries[1] = sources[1];
				entries[0].cache_readers = entries[1].cache_readers = 1;
				check(ps_compaction_prune_fence_valid(&guarded, sources, 2, outputs, 4,
						&captured, &current, 1, 0, 0, 0, 0, 0),
					  "pinned captured set passes before frontier publication");
				entries[0].cache_readers = 0;
				check(!ps_compaction_prune_fence_valid(&guarded, sources, 2, outputs, 4,
						&captured, &current, 1, 0, 0, 0, 0, 0),
					  "pruned publication fencing rejects an unpinned source");
				entries[0].cache_readers = 1;
				sources[0].remote_durable = true;
				entries[0].remote_durable = true;
				check(!ps_compaction_prune_fence_valid(&guarded, sources, 2, outputs, 4,
						&captured, &current, 1, 0, 0, 0, 0, 0),
					  "pruned publication fencing rejects remote-durable inputs");
				sources[0].remote_durable = false;
				entries[0].remote_durable = false;
				current.retention_epoch++;
				check(!ps_compaction_prune_fence_valid(&guarded, sources, 2, outputs, 4,
						&captured, &current, 1, 0, 0, 0, 0, 0),
					  "a changed retention epoch invalidates the pruning fence");
				current = captured;
				current.admission_epoch++;
				check(!ps_compaction_prune_fence_valid(&guarded, sources, 2, outputs, 4,
						&captured, &current, 1, 0, 0, 0, 0, 0),
					  "a concurrent write invalidates the pruning fence");
				current = captured;
				current.timeline_incarnation++;
				check(!ps_compaction_prune_fence_valid(&guarded, sources, 2, outputs, 4,
						&captured, &current, 1, 0, 0, 0, 0, 0),
					  "timeline reuse invalidates the pruning fence");
				current = captured;
				current.page_floor++;
				check(!ps_compaction_prune_fence_valid(&guarded, sources, 2, outputs, 4,
						&captured, &current, 1, 0, 0, 0, 0, 0) &&
					  !ps_compaction_prune_fence_valid(&guarded, sources, 2, outputs, 4,
						&captured, &captured, 0, 0, 0, 0, 0, 0),
					  "a changed floor or non-live timeline invalidates the pruning fence");
				current = captured;
				entries[2] = outputs[1];
				guarded.nlayers = 3;
				check(!ps_compaction_prune_fence_valid(&guarded, sources, 2, outputs, 4,
						&captured, &current, 1, 0, 1, 0, 1, 0),
					  "published outputs must form the exact prefix");
				entries[2] = outputs[0];
				check(!ps_compaction_prune_fence_valid(&guarded, sources, 2, outputs, 4,
						&captured, &current, 1, 0, 1, 0, 0, 0) &&
					  ps_compaction_prune_fence_valid(&guarded, sources, 2, outputs, 4,
						&captured, &current, 1, 0, 1, 0, 1, 0),
					  "a partial output prefix requires a durable frontier");
				memcpy(&entries[2], outputs, sizeof(legacy_outputs));
				guarded.nlayers = 6;
				check(!ps_compaction_prune_fence_valid(&guarded, sources, 2, outputs, 4,
						&captured, &current, 1, 0, 4, 1, 1, 0),
					  "retiring a source requires the page index update");
				entries[0].deleting = true;
				check(ps_compaction_prune_fence_valid(&guarded, sources, 2, outputs, 4,
						&captured, &current, 1, 0, 4, 1, 1, 1),
					  "completed outputs and index update allow the next source retirement check");
				current = captured;
				current.timeline_epoch++;
				check(!ps_compaction_prune_fence_valid(&guarded, sources, 2, outputs, 4,
						&captured, &current, 1, 0, 4, 1, 1, 1) &&
					  !ps_compaction_prune_fence_valid(&guarded, sources, 2, outputs, 4,
						&captured, &captured, 1, 1, 4, 1, 1, 1),
					  "retirement rejects changed timeline and poisoned manifest");
			}
			{
				PsLayerDesc entries[5], remote = layer;
				PsLayerMap retirement = {.layers = entries, .nlayers = 5, .capacity = 5};

				entries[0] = layer;
				memcpy(&entries[1], outputs, sizeof(legacy_outputs));
				entries[0].cache_readers = 0;
				check(!ps_image_compaction_retirement_valid(&retirement, &layer, 1, outputs, 4, 0),
					  "retirement rejects an unpinned source");
				entries[0].cache_readers = 1;
				check(ps_image_compaction_retirement_valid(&retirement, &layer, 1, outputs, 4, 0),
					  "retirement accepts complete outputs and pinned source");
				retirement.nlayers--;
				check(!ps_image_compaction_retirement_valid(&retirement, &layer, 1, outputs, 4, 0),
					  "retirement rejects missing output");
				retirement.nlayers++;
				remote.remote_durable = true;
				entries[0].remote_durable = true;
				check(!ps_image_compaction_retirement_valid(&retirement, &remote, 1, outputs, 4, 0),
					  "retirement rejects remote-durable inputs");
			}
			memcpy(legacy_outputs, outputs, sizeof(legacy_outputs));
			legacy_outputs[0].legacy_shard_zero = true;
			check(ps_image_compaction_verify_coverage(&layer, 1, legacy_outputs, 4,
												  PSZ) != 0 &&
				  ps_image_compaction_verify_coverage(&legacy_layer, 1, outputs, 4,
												  PSZ) != 0,
				  "coverage rejects mixed-shard legacy sources and outputs");
		}
		for (uint32_t i = 0; i < noutputs; i++)
		{
			uint32_t block = i * PS_IMAGE_BATCH_MAX_RECORDS;
			check(outputs[i].locations[0].size <=
				  (uint64_t) PS_IMAGE_BATCH_MAX_RECORDS *
					(PSZ + sizeof(PsImgIndexEnt)) + sizeof(PsImgFooter) &&
				  ps_image_layer_lookup(&outputs[i], &key, block, 100, UINT64_MAX,
										page, PSZ, NULL, NULL) == 1 &&
				  page[0] == (unsigned char) block,
				  "prepared output stays bounded and preserves source data");
			check(ps_layer_store->delete_local_layer(&outputs[i]) == 0,
				  "test removes sealed unpublished output");
		}
		uncertain = 0;
		noutputs = 0;
		prepare_corrupt_source = 1;
		check(ps_image_compaction_prepare(&layer, 1, ids, 4, PSZ, outputs,
											  &noutputs, &uncertain) == -1 &&
			  noutputs == 0 && uncertain == 0 &&
			  ps_layer_store->layer_exists_local(ids[0]) == 0,
			  "corrupt source data is rejected before creating output");
		prepare_corrupt_source = 0;
		uncertain = 0;
		noutputs = 0;
		check(ps_image_compaction_prepare(&layer, 1, ids, 1, PSZ, outputs,
											  &noutputs, &uncertain) == -3 &&
			  noutputs == 0 && uncertain == 0 &&
			  ps_layer_store->layer_exists_local(ids[0]) == 0,
			  "capacity exhaustion cleans every sealed output");
		prepare_fail_write_id = ids[1];
		prepare_fail_delete_id = ids[0];
		uncertain = 0;
		noutputs = 0;
		check(ps_image_compaction_prepare(&layer, 1, ids, 4, PSZ, outputs,
											  &noutputs, &uncertain) == -2 &&
			  noutputs == 1 && outputs[0].layer_id == ids[0] && uncertain == ids[1],
			  "write and unlink failures report all unresolved output identities");
		prepare_fail_write_id = 0;
		prepare_fail_delete_id = 0;
		if (noutputs == 1)
			check(ps_layer_store->delete_local_layer(&outputs[0]) == 0,
				  "test reconciles the reported failed unlink");
		{
			uint64_t occupied_id = 1020;
			PsLayerDesc occupied;
			PsImgRec existing = {.key = key, .block = 0, .lsn = 100, .page = page};

			memset(page, 0x4a, sizeof(page));
			check(ps_image_layer_write(occupied_id, 0, &existing, 1, PSZ, &occupied) == 0,
				  "create a pre-existing output ID");
			noutputs = 0;
			uncertain = 0;
			check(ps_image_compaction_prepare(&layer, 1, &occupied_id, 1, PSZ,
					outputs, &noutputs, &uncertain) == -2 && noutputs == 0 &&
				  uncertain == occupied_id &&
				  ps_image_layer_lookup(&occupied, &key, 0, 100, 0, page, PSZ,
											NULL, NULL) == 1 && page[0] == 0x4a,
				  "failed create reports ambiguity without deleting the existing layer");
			check(ps_layer_store->delete_local_layer(&occupied) == 0,
				  "test removes the pre-existing fixture layer");
		}
	}
	{
		uint32_t count = 0;
		PsImageIndexCursor *cursor = NULL;
		PsImgIndexEnt entry;
		int cursor_rc = -1;
		bounded.read_layer_block = visitor_read;
		visitor_read_max = 0;
		check(ps_image_layer_visit_index(&layer, visit_stream_index, &count) == 0 &&
			  count == 4096 && visitor_read_max <= 64 * 1024,
			  "index visitor returns sorted entries using bounded reads");
		count = 0;
		check(ps_image_index_cursor_open(&layer, &cursor) == 0,
			  "open bounded sorted index cursor");
		if (cursor != NULL)
			while ((cursor_rc = ps_image_index_cursor_next(cursor, &entry)) == 1)
				if (visit_stream_index(&entry, &count) != 0)
					break;
		check(cursor_rc == 0 && count == 4096 && visitor_read_max <= 64 * 1024,
			  "cursor yields ordered records with bounded reads and final validation");
		ps_image_index_cursor_close(cursor);
		visitor_corrupt = 1;
		visitor_first_chunk_reads = 0;
		count = 0;
		check(ps_image_layer_visit_index(&layer, visit_stream_index, &count) != 0 &&
			  count == 0, "corrupt index is rejected before visiting entries");
		visitor_corrupt = 2;
		visitor_first_chunk_reads = 0;
		count = 0;
		check(ps_image_layer_visit_index(&layer, visit_stream_index, &count) != 0,
			  "index changes during traversal are rejected");
		visitor_corrupt = 0;
		visitor_first_chunk_reads = 0;
		visitor_corrupt = 3;
		cursor = NULL;
		cursor_rc = -1;
		if (ps_image_index_cursor_open(&layer, &cursor) == 0)
			while ((cursor_rc = ps_image_index_cursor_next(cursor, &entry)) == 1)
				;
		check(cursor != NULL && cursor_rc == -1,
			  "cursor rejects changes after its precheck");
		ps_image_index_cursor_close(cursor);
		visitor_corrupt = 0;
		count = 0;
		check(ps_image_layer_visit_index(&layer, reject_stream_index, &count) != 0 &&
			  count == 1, "index visitor errors stop traversal");
		bounded.read_layer_block = bounded_read;
	}
	for (uint32_t block = 0; block < 4096; block += 4095)
	{
		check(ps_image_layer_lookup(&layer, &key, block, 100, UINT64_MAX,
									page, PSZ, NULL, NULL) == 1 &&
			  page[0] == (unsigned char) block && page[PSZ - 1] == (unsigned char) block,
			  "stream sorting preserves page bytes at both ends");
	}
	stream_pages = 0;
	stream_fail_after = 40;
	check(ps_image_layer_write_stream(911, 0, recs, 4096, PSZ,
									  generated_page, NULL, &layer) != 0,
		  "source read failure rejects partially written image");
	check(ps_layer_store->layer_exists_local(911) == 0,
		  "source failure removes unpublished output immediately");
	for (uint64_t id = 912; id < 916; id++)
	{
		stream_pages = 0;
		check(ps_image_layer_write_stream(id, 0, recs, 4096, PSZ,
										  generated_page, NULL, &layer) != 0 &&
			  ps_layer_store->layer_exists_local(id) == 0,
			  "repeated source failures do not accumulate partial layers");
	}
	stream_fail_after = -1;
	check(ps_image_layer_write_stream(910, 0, recs, 4096, PSZ,
									  generated_page, NULL, &layer) != 0 &&
		  ps_layer_store->layer_exists_local(910) == 1,
		  "failed create must preserve the existing layer");
	check(ps_image_layer_write_stream(911, 0, recs, 4096, PSZ,
									  generated_page, NULL, &layer) == 0 &&
		  ps_image_layer_verify_data(&layer, PSZ) == 0,
		  "retry can recreate and seal a cleaned-up output");
	ps_layer_store = saved;
	free(recs);
}

/* A sweep of tiny layers must not evict an expensive large index merely
 * because the fixed number of cache slots filled up. */
static unsigned large_index_reads;

static int
counted_index_read(const PsLayerDesc *layer, uint64_t off, void *buf, uint32_t len)
{
	if (layer->layer_id == 30000 && off == (uint64_t) 1024 * PSZ &&
		len == 1024 * sizeof(PsImgIndexEnt))
		large_index_reads++;
	return PsLayerStoreLocal.read_layer_block(layer, off, buf, len);
}

static void
index_cache_pressure_test(void)
{
	PsLayerStore counted = PsLayerStoreLocal;
	const PsLayerStore *saved = ps_layer_store;
	PsImgRec *recs = calloc(1024, sizeof(*recs));
	PsLayerDesc large, small;
	PsKey key = {1, 1, 30000, 0, PS_KLASS_RELATION};
	unsigned char page[PSZ] = {0};
	int ok = 1;

	check(recs != NULL, "allocate cache pressure fixture");
	if (recs == NULL)
		return;
	ps_image_layer_cache_reset();
	counted.read_layer_block = counted_index_read;
	ps_layer_store = &counted;
	for (uint32_t i = 0; i < 1024; i++)
	{
		recs[i].key = key;
		recs[i].block = i;
		recs[i].lsn = 100;
		recs[i].page = page;
	}
	if (ps_image_layer_write(30000, 0, recs, 1024, PSZ, &large) != 0 ||
		ps_image_layer_lookup(&large, &key, 0, 100, 0, page, PSZ, NULL, NULL) != 1)
		ok = 0;
	for (uint64_t id = 30001; id <= 30130 && ok; id++)
	{
		if (ps_image_layer_write(id, 0, recs, 1, PSZ, &small) != 0 ||
			ps_image_layer_lookup(&small, &key, 0, 100, 0, page, PSZ, NULL, NULL) != 1)
			ok = 0;
	}
	if (ok && ps_image_layer_lookup(&large, &key, 0, 100, 0, page, PSZ, NULL, NULL) != 1)
		ok = 0;
	check(ok, "large and small layers remain readable through cache pressure");
	check(large_index_reads == 1, "small-index churn retains the large verified index");
	ps_image_layer_cache_reset();
	ps_layer_store = saved;
	free(recs);
}

/* The current-format index need not allocate a second full-size copy. */
static void *index_read_destination;

static int
capture_index_read(const PsLayerDesc *layer, uint64_t off, void *buf, uint32_t len)
{
	if (layer->layer_id == 30131 && off == PSZ && len == sizeof(PsImgIndexEnt))
		index_read_destination = buf;
	return PsLayerStoreLocal.read_layer_block(layer, off, buf, len);
}

static void
index_direct_read_test(void)
{
	PsLayerStore counted = PsLayerStoreLocal;
	const PsLayerStore *saved = ps_layer_store;
	PsKey key = {1, 1, 30131, 0, PS_KLASS_RELATION};
	unsigned char page[PSZ] = {0};
	PsImgRec rec = {.key = key, .block = 0, .lsn = 100, .page = page};
	PsLayerDesc layer;
	PsImgIndexEnt *index = NULL;
	uint32_t n = 0;
	int rc;

	counted.read_layer_block = capture_index_read;
	ps_layer_store = &counted;
	index_read_destination = NULL;
	rc = ps_image_layer_write(30131, 0, &rec, 1, PSZ, &layer);
	if (rc == 0)
		rc = ps_image_layer_read_index(&layer, &index, &n);
	check(rc == 0 && n == 1 && index == index_read_destination &&
		  index->key.relNumber == key.relNumber,
		  "current index is read and verified directly into the result buffer");
	free(index);
	ps_layer_store = saved;
}

typedef struct BatchSource
{
	const PsLayerDesc *layer;
	uint64_t offset;
} BatchSource;

static int
read_batch_source(const PsImgRec *rec, void *page, void *arg)
{
	const BatchSource *source = rec->page;
	(void) arg;
	return ps_layer_store->read_layer_block(source->layer, source->offset, page, PSZ);
}

typedef struct GroupProbe
{
	uint32_t groups;
	uint32_t entries;
	uint64_t source_sum;
	int wrong_order;
	int reject;
} GroupProbe;

static int
visit_group(const PsImageGroupEntry *entries, uint32_t count, void *arg)
{
	GroupProbe *probe = arg;

	probe->groups++;
	probe->entries += count;
	for (uint32_t i = 0; i < count; i++)
		probe->source_sum += entries[i].layer_id;
	for (uint32_t i = 1; i < count; i++)
		if (entries[i - 1].entry.lsn == entries[i].entry.lsn &&
			entries[i - 1].entry.admission_seq == entries[i].entry.admission_seq &&
			entries[i - 1].layer_id > entries[i].layer_id)
			probe->wrong_order = 1;
	return probe->reject ? -1 : 0;
}

typedef struct RelationPlanProbe
{
	uint32_t calls;
	uint32_t count;
	uint64_t ids[4];
	unsigned char keep[4];
	unsigned char closure[4];
} RelationPlanProbe;

typedef struct SelectedPlanProbe
{
	PsImageGroupEntry selected[4], dropped[4];
	uint32_t nselected, ndropped, calls;
} SelectedPlanProbe;

static int
select_relation_plan(const PsImageGroupEntry *entries, uint32_t count,
					 const unsigned char *keep, const unsigned char *closure, void *arg)
{
	SelectedPlanProbe *probe = arg;

	(void) closure;
	probe->calls++;
	return ps_compaction_relation_select_group(entries, count, keep,
			probe->selected, 4, &probe->nselected,
			probe->dropped, 4, &probe->ndropped);
}

static int
visit_relation_plan(const PsImageGroupEntry *entries, uint32_t count,
					const unsigned char *keep, const unsigned char *closure, void *arg)
{
	RelationPlanProbe *probe = arg;

	if (count > 4 || probe->calls != 0)
		return -1;
	probe->calls++;
	probe->count = count;
	for (uint32_t i = 0; i < count; i++)
	{
		probe->ids[i] = entries[i].layer_id;
		probe->keep[i] = keep[i];
		probe->closure[i] = closure[i];
	}
	return 0;
}

static int
stage_pruned_bad_read(const PsLayerDesc *layer, uint64_t off, void *buf, uint32_t len)
{
	int rc = PsLayerStoreLocal.read_layer_block(layer, off, buf, len);

	if (rc == 0 && layer->layer_id == 31980 && off == 0 && len != 0)
		((unsigned char *) buf)[0] ^= 1;
	return rc;
}

static void
stage_pruned_batch_test(void)
{
	PsKey key = {1, 1, 31415, 0, PS_KLASS_RELATION};
	unsigned char pages[3][PSZ], out_page[PSZ];
	PsImgRec recs[2] = {0}, newer = {0};
	PsLayerDesc layers[2], output, ignored;
	PsImgIndexEnt *index0 = NULL, *index1 = NULL;
	PsImageGroupEntry selected[2], bad[2];
	uint32_t n0 = 0, n1 = 0;
	uint64_t uncertain = 0, found_lsn = 0, found_seq = 0;
	int ok;

	memset(pages[0], 0x11, PSZ);
	memset(pages[1], 0x22, PSZ);
	memset(pages[2], 0x33, PSZ);
	for (uint32_t i = 0; i < 2; i++)
	{
		recs[i].key = key;
		recs[i].block = i;
		recs[i].lsn = 100;
		recs[i].admission_seq = i + 1;
		recs[i].page = pages[i];
	}
	newer = recs[0];
	newer.lsn = 200;
	newer.admission_seq = 3;
	newer.page = pages[2];
	ok = ps_image_layer_write(31980, 0, recs, 2, PSZ, &layers[0]) == 0 &&
		ps_image_layer_write(31981, 0, &newer, 1, PSZ, &layers[1]) == 0 &&
		ps_image_layer_read_index(&layers[0], &index0, &n0) == 0 && n0 == 2 &&
		ps_image_layer_read_index(&layers[1], &index1, &n1) == 0 && n1 == 1;
	check(ok, "prepare selected staging source layers");
	if (!ok)
		goto done;
	selected[0].entry = index1[0];
	selected[0].layer_id = layers[1].layer_id;
	selected[1].entry = index0[1];
	selected[1].layer_id = layers[0].layer_id;
	check(ps_image_compaction_stage_selected_batch(layers, 2, selected,
			PS_IMAGE_BATCH_MAX_RECORDS + 1, 31983, PSZ, &ignored, &uncertain) == -1 &&
		  uncertain == 0 && ps_layer_store->layer_exists_local(31983) == 0,
		  "selected output batch refuses more than the record bound");
	check(ps_image_compaction_stage_selected_batch(layers, 2, selected, 2,
			31982, PSZ, &output, &uncertain) == 0 && uncertain == 0,
		  "seal one unpublished selected batch");
	check(ps_image_layer_lookup_exact(&output, &key, 0, 200, 3, out_page,
			PSZ, &found_lsn, &found_seq) == 1 &&
		  memcmp(out_page, pages[2], PSZ) == 0 &&
		  ps_image_layer_lookup_exact(&output, &key, 0, 100, 1, out_page,
			PSZ, NULL, NULL) == 0 &&
		  ps_image_layer_lookup_exact(&output, &key, 1, 100, 2, out_page,
			PSZ, NULL, NULL) == 1 && memcmp(out_page, pages[1], PSZ) == 0,
		  "staged output contains only the selected versions and bytes");
	check(ps_image_compaction_verify_selected_batch(layers, 2, selected, 2,
			&output, PSZ) == 0,
		  "independent verification matches selected sources and output bytes");
	bad[0] = selected[0];
	bad[1] = selected[1];
	bad[0].entry.growth_lsn++;
	check(ps_image_compaction_verify_selected_batch(layers, 2, bad, 2,
			&output, PSZ) == -1,
		  "verification rejects selected metadata absent from the source index");
	bad[0] = selected[0];
	bad[0].layer_id = layers[0].layer_id;
	check(ps_image_compaction_verify_selected_batch(layers, 2, bad, 2,
			&output, PSZ) == -1,
		  "verification rejects an identity attributed to the wrong source layer");
	bad[0] = selected[0];
	bad[1].entry.data_off = index0[0].data_off;
	check(ps_image_compaction_verify_selected_batch(layers, 2, bad, 2,
			&output, PSZ) == -1,
		  "verification rejects an in-range offset not belonging to the selected identity");
	check(ps_image_compaction_verify_selected_batch(layers, 2, selected, 1,
			&output, PSZ) == -1,
		  "verification rejects an extra output identity");
	{
		PsImgRec incorrect[2] = {newer, recs[1]};
		PsLayerDesc wrong, missing;

		incorrect[0].page = pages[0];
		check(ps_image_layer_write(31986, 0, incorrect, 2, PSZ, &wrong) == 0 &&
			  ps_image_compaction_verify_selected_batch(layers, 2, selected, 2,
				&wrong, PSZ) == -1,
			  "verification rejects validly checksummed output with wrong page bytes");
		check(ps_image_layer_write(31987, 0, &newer, 1, PSZ, &missing) == 0 &&
			  ps_image_compaction_verify_selected_batch(layers, 2, selected, 2,
				&missing, PSZ) == -1,
			  "verification rejects output missing a selected identity");
	}
	check(ps_image_compaction_stage_selected_batch(layers, 2, selected, 2,
			31982, PSZ, &ignored, &uncertain) == -2 && uncertain == 31982 &&
		  ps_image_layer_lookup_exact(&output, &key, 0, 200, 3, out_page,
			PSZ, NULL, NULL) == 1 && memcmp(out_page, pages[2], PSZ) == 0,
		  "failed create reports uncertainty without deleting an occupied output");
	bad[0] = selected[1];
	bad[1] = selected[0];
	check(ps_image_compaction_stage_selected_batch(layers, 2, bad, 2,
			31983, PSZ, &ignored, &uncertain) == -1 && uncertain == 0 &&
		  ps_layer_store->layer_exists_local(31983) == 0,
		  "unsorted selections fail before creating output");
	bad[0] = selected[0];
	bad[0].entry.data_off = layers[1].locations[0].size;
	check(ps_image_compaction_stage_selected_batch(layers, 2, bad, 1,
			31983, PSZ, &ignored, &uncertain) == -1 && uncertain == 0 &&
		  ps_layer_store->layer_exists_local(31983) == 0,
		  "out-of-range source offset fails before creating output");
	{
		PsLayerStore injected = PsLayerStoreLocal;
		const PsLayerStore *saved = ps_layer_store;

		injected.read_layer_block = stage_pruned_bad_read;
		ps_layer_store = &injected;
		check(ps_image_compaction_stage_selected_batch(layers, 2, selected, 2,
				31983, PSZ, &ignored, &uncertain) == -1 && uncertain == 0 &&
			  ps_layer_store->layer_exists_local(31983) == 0,
			  "corrupt source data is rejected before output creation");
		injected.read_layer_block = PsLayerStoreLocal.read_layer_block;
		injected.write_local_layer_at = bounded_write;
		prepare_fail_write_id = 31984;
		check(ps_image_compaction_stage_selected_batch(layers, 2, selected, 2,
				31984, PSZ, &ignored, &uncertain) == -2 && uncertain == 31984,
			  "failed output write reports its identity for reconciliation");
		prepare_fail_write_id = 0;
		injected.delete_local_layer = bounded_delete;
		prepare_fail_write_id = 31985;
		prepare_fail_delete_id = 31985;
		check(ps_image_compaction_stage_selected_batch(layers, 2, selected, 2,
				31985, PSZ, &ignored, &uncertain) == -2 && uncertain == 31985 &&
			  ps_layer_store->layer_exists_local(31985) == 1,
			  "failed cleanup leaves the uncertain partial file for reconciliation");
		prepare_fail_write_id = 0;
		prepare_fail_delete_id = 0;
		{
			PsLayerDesc partial = {.layer_id = 31985};

			check(PsLayerStoreLocal.delete_local_layer(&partial) == 0,
				  "test reconciles the reported partial file");
		}
		ps_layer_store = saved;
	}
done:
	free(index0);
	free(index1);
}

static void
verify_pruned_plan_duplicate_test(void)
{
	PsKey key = {1, 1, 31801, 0, PS_KLASS_RELATION};
	unsigned char pages[3][PSZ];
	PsImgRec recs[3] = {0}, selected[2], wrong[2];
	PsLayerDesc inputs[3], output, bad_output;
	PsViewFence fence = {200, 1, PS_PRUNE_SEQ_UNBOUNDED};
	uint64_t kept, dropped;
	int ok;

	for (uint32_t i = 0; i < 3; i++)
	{
		memset(pages[i], (int) (0x51 + i), PSZ);
		recs[i].key = key;
		recs[i].lsn = 100;
		recs[i].admission_seq = i == 0 ? 1 : 2;
		recs[i].page = pages[i];
	}
	selected[0] = recs[0];
	selected[1] = recs[2];
	wrong[0] = recs[0];
	wrong[1] = recs[1];
	ok = ps_image_layer_write(32010, 0, &recs[0], 1, PSZ, &inputs[0]) == 0 &&
		ps_image_layer_write(32011, 0, &recs[1], 1, PSZ, &inputs[1]) == 0 &&
		ps_image_layer_write(32012, 0, &recs[2], 1, PSZ, &inputs[2]) == 0 &&
		ps_image_layer_write(32013, 0, selected, 2, PSZ, &output) == 0 &&
		ps_image_layer_write(32014, 0, wrong, 2, PSZ, &bad_output) == 0;
	check(ok, "create duplicate-source full-plan fixture");
	if (!ok)
		return;
	check(ps_compaction_verify_relation_plan(inputs, 3, &output, 1, PSZ, 3,
			(PsPruneFence) {150, UINT64_MAX}, &fence, 1, 0, 0,
			NULL, NULL, &kept, &dropped) == 0 && kept == 2 && dropped == 0,
		  "full plan retains closure and selects the highest kept duplicate");
	check(ps_compaction_verify_relation_plan(inputs, 3, &bad_output, 1, PSZ, 3,
			(PsPruneFence) {150, UINT64_MAX}, &fence, 1, 0, 0,
			NULL, NULL, &kept, &dropped) == -1 && kept == 0 && dropped == 0,
		  "full plan rejects bytes from the lower-priority duplicate");
}

static void
verify_pruned_plan_test(void)
{
	PsKey key = {1, 1, 31800, 0, PS_KLASS_RELATION};
	unsigned char pages[4][PSZ];
	PsImgRec records[4] = {0}, first[2], second[1], extra[3], wrong[1];
	PsLayerDesc input, outputs[2], excessive[2], bad_page[2];
	uint64_t kept, dropped;
	GroupProbe drops = {0};
	PsViewFence historical = {100, 1, PS_PRUNE_SEQ_UNBOUNDED};
	int ok;

	for (uint32_t i = 0; i < 4; i++)
	{
		memset(pages[i], (int) (0x31 + i), PSZ);
		records[i].key = key;
		records[i].block = i < 2 ? 0 : i - 1;
		records[i].lsn = i == 1 ? 200 : 100;
		records[i].admission_seq = i + 1;
		records[i].page = pages[i];
	}
	first[0] = records[1];
	first[1] = records[2];
	second[0] = records[3];
	extra[0] = records[0];
	extra[1] = records[1];
	extra[2] = records[2];
	wrong[0] = records[3];
	wrong[0].page = pages[0];
	ok = ps_image_layer_write(32000, 0, records, 4, PSZ, &input) == 0 &&
		ps_image_layer_write(32001, 0, first, 2, PSZ, &outputs[0]) == 0 &&
		ps_image_layer_write(32002, 0, second, 1, PSZ, &outputs[1]) == 0 &&
		ps_image_layer_write(32003, 0, extra, 3, PSZ, &excessive[0]) == 0 &&
		ps_image_layer_write(32004, 0, second, 1, PSZ, &excessive[1]) == 0 &&
		ps_image_layer_write(32005, 0, first, 2, PSZ, &bad_page[0]) == 0 &&
		ps_image_layer_write(32006, 0, wrong, 1, PSZ, &bad_page[1]) == 0;
	check(ok, "create complete pruned-plan verification fixture");
	if (!ok)
		return;
	check(ps_compaction_verify_relation_plan(&input, 1, outputs, 2, PSZ, 4,
			(PsPruneFence) {250, UINT64_MAX}, NULL, 0, 0, 0,
			visit_group, &drops, &kept, &dropped) == 0 && kept == 3 && dropped == 1 &&
		  drops.groups == 1 && drops.entries == 1 && drops.source_sum == 32000,
		  "complete plan verifies multiple output batches and stages exact dropped identities");
	memset(&drops, 0, sizeof(drops));
	check(ps_compaction_verify_relation_plan(&input, 1, outputs, 1, PSZ, 4,
			(PsPruneFence) {250, UINT64_MAX}, NULL, 0, 0, 0,
			visit_group, &drops, &kept, &dropped) == -1 && kept == 0 && dropped == 0,
		  "missing output batch invalidates the whole staged plan");
	memset(&drops, 0, sizeof(drops));
	check(ps_compaction_verify_relation_plan(&input, 1, excessive, 2, PSZ, 4,
			(PsPruneFence) {250, UINT64_MAX}, NULL, 0, 0, 0,
			visit_group, &drops, &kept, &dropped) == -1 && kept == 0 && dropped == 0,
		  "an output containing a version the plan drops is rejected");
	memset(&drops, 0, sizeof(drops));
	check(ps_compaction_verify_relation_plan(&input, 1, bad_page, 2, PSZ, 4,
			(PsPruneFence) {250, UINT64_MAX}, NULL, 0, 0, 0,
			visit_group, &drops, &kept, &dropped) == -1 && kept == 0 && dropped == 0 &&
		  drops.entries == 1,
		  "later wrong bytes invalidate even previously staged drop callbacks");
	memset(&drops, 0, sizeof(drops));
	check(ps_compaction_verify_relation_plan(&input, 1, outputs, 2, PSZ, 4,
			(PsPruneFence) {250, UINT64_MAX}, &historical, 1, 0, 0,
			visit_group, &drops, &kept, &dropped) == -1 && kept == 0 && dropped == 0,
		  "a retained historical fence rejects output that omits its version");
	memset(&drops, 0, sizeof(drops));
	check(ps_compaction_verify_relation_plan(&input, 1, outputs, 2, PSZ, 1,
			(PsPruneFence) {250, UINT64_MAX}, NULL, 0, 0, 0,
			visit_group, &drops, &kept, &dropped) == -2 && kept == 0 && dropped == 0,
		  "oversized page history invalidates the full plan");
	drops.reject = 1;
	check(ps_compaction_verify_relation_plan(&input, 1, outputs, 2, PSZ, 4,
			(PsPruneFence) {250, UINT64_MAX}, NULL, 0, 0, 0,
			visit_group, &drops, &kept, &dropped) == -1 && kept == 0 && dropped == 0,
		  "drop staging failure invalidates the full plan");
}

static void
relation_group_selection_test(void)
{
	PsImageGroupEntry entries[6] = {0}, selected[2], dropped[1];
	unsigned char keep[6] = {1, 1, 0, 0, 0, 1};
	uint32_t nselected, ndropped;
	PsKey key = {1, 1, 12345, 0, PS_KLASS_RELATION};

	for (uint32_t i = 0; i < 6; i++)
	{
		entries[i].entry.key = key;
		entries[i].entry.block = 7;
		entries[i].entry.lsn = i < 3 ? 100 : i < 5 ? 200 : 300;
		entries[i].entry.admission_seq = i < 3 ? 1 : i < 5 ? 4 : 6;
		entries[i].entry.data_off = 10 + i;
		entries[i].layer_id = i < 2 ? 5 : i == 2 ? 9 : i == 3 ? 8 : 9;
	}
	check(ps_compaction_relation_select_group(entries, 6, keep,
			selected, 2, &nselected, dropped, 1, &ndropped) == 0 &&
		  nselected == 2 && ndropped == 1 && selected[0].layer_id == 5 &&
		  selected[0].entry.data_off == 11 && selected[1].entry.lsn == 300 &&
		  dropped[0].entry.lsn == 200 && dropped[0].layer_id == 8,
		  "group selection preserves last kept duplicate and records each dropped identity once");
	memset(selected, 0xA5, sizeof(selected));
	memset(dropped, 0xA5, sizeof(dropped));
	check(ps_compaction_relation_select_group(entries, 6, keep,
			selected, 1, &nselected, dropped, 1, &ndropped) == -2 &&
		  nselected == 0 && ndropped == 0 &&
		  ((unsigned char *) selected)[0] == 0xA5 &&
		  ((unsigned char *) dropped)[0] == 0xA5,
		  "insufficient capacity leaves both staged outputs untouched");
	entries[2].layer_id = 4;
	check(ps_compaction_relation_select_group(entries, 6, keep,
			selected, 2, &nselected, dropped, 1, &ndropped) == -1 &&
		  nselected == 0 && ndropped == 0 &&
		  ((unsigned char *) selected)[0] == 0xA5,
		  "unordered duplicate sources fail before output is modified");
	entries[2].layer_id = 9;
	entries[5].entry.block++;
	check(ps_compaction_relation_select_group(entries, 6, keep,
			selected, 2, &nselected, dropped, 1, &ndropped) == -1 &&
		  nselected == 0 && ndropped == 0,
		  "selection rejects an incomplete or mixed page group");
}

static void
resumable_relation_plan_test(void)
{
	PsKey key = {1, 1, 956, 0, PS_KLASS_RELATION};
	unsigned char page[PSZ] = {0};
	PsImgRec recs[3], big[5];
	PsLayerDesc layer, mixed, oversized;
	PsCompactionRelationPlan *plan = NULL;
	PsViewFence fence = {200, 1, PS_PRUNE_SEQ_UNBOUNDED};
	RelationPlanProbe probe = {0};
	uint32_t visited = 99;
	int ok;

	memset(recs, 0, sizeof(recs));
	for (uint32_t i = 0; i < 3; i++)
	{
		recs[i].key = key;
		recs[i].block = i == 2 ? 1 : 0;
		recs[i].lsn = 100;
		recs[i].admission_seq = i + 1;
		recs[i].page = page;
	}
	ok = ps_image_layer_write(30984, 0, recs, 3, PSZ, &layer) == 0;
	check(ok && ps_compaction_relation_plan_open(&layer, 1, 2,
			(PsPruneFence) {150, UINT64_MAX}, &fence, 1, 0, 0, &plan) == 0,
		  "open resumable relation plan");
	fence.seq = 2; /* The plan must use the copied fence, not this mutation. */
	check(plan && ps_compaction_relation_plan_step(plan, 1, visit_relation_plan,
			&probe, &visited) == 1 && visited == 1 && probe.count == 2 &&
		  probe.keep[0] && probe.keep[1] && probe.closure[0],
		  "one-step budget retains copied finite fence and leaves a next group");
	memset(&probe, 0, sizeof(probe));
	check(ps_compaction_relation_plan_step(plan, 1, visit_relation_plan,
			&probe, &visited) == 0 && visited == 1 && probe.count == 1 &&
		  probe.ids[0] == 30984,
		  "next step completes exactly the remaining group");
	check(ps_compaction_relation_plan_step(plan, 1, visit_relation_plan,
			&probe, &visited) == 0 && visited == 0,
		  "completed plan stays complete without another callback");
	ps_compaction_relation_plan_close(plan);
	plan = NULL;
	recs[2].key.klass = PS_KLASS_CONTROL;
	check(ps_image_layer_write(30985, 0, recs, 3, PSZ, &mixed) == 0 &&
		  ps_compaction_relation_plan_open(&mixed, 1, 2,
			(PsPruneFence) {150, UINT64_MAX}, NULL, 0, 0, 0, &plan) == 0,
		  "open plan with a later unsupported group");
	memset(&probe, 0, sizeof(probe));
	check(plan && ps_compaction_relation_plan_step(plan, 1, visit_relation_plan,
			&probe, &visited) == 1 && visited == 1,
		  "first group is staged before the later unsupported group");
	memset(&probe, 0, sizeof(probe));
	check(ps_compaction_relation_plan_step(plan, 1, visit_relation_plan,
			&probe, &visited) == -1 && visited == 0 && probe.calls == 0 &&
		  ps_compaction_relation_plan_step(plan, 1, visit_relation_plan,
			&probe, &visited) == -1,
		  "later unsupported group invalidates all staged work and fails permanently");
	ps_compaction_relation_plan_close(plan);
	plan = NULL;
	memset(big, 0, sizeof(big));
	for (uint32_t i = 0; i < 5; i++)
	{
		big[i].key = key;
		big[i].block = i < 2 ? 0 : 1;
		big[i].lsn = 100 + i;
		big[i].admission_seq = i + 1;
		big[i].page = page;
	}
	check(ps_image_layer_write(30986, 0, big, 5, PSZ, &oversized) == 0 &&
		  ps_compaction_relation_plan_open(&oversized, 1, 2,
			(PsPruneFence) {150, UINT64_MAX}, NULL, 0, 0, 0, &plan) == 0,
		  "open plan with later oversized history");
	memset(&probe, 0, sizeof(probe));
	check(plan && ps_compaction_relation_plan_step(plan, 1, visit_relation_plan,
			&probe, &visited) == 1 && visited == 1,
		  "stage complete group before oversized history");
	memset(&probe, 0, sizeof(probe));
	check(ps_compaction_relation_plan_step(plan, 1, visit_relation_plan,
			&probe, &visited) == -2 && visited == 0 && probe.calls == 0 &&
		  ps_compaction_relation_plan_step(plan, 1, visit_relation_plan,
			&probe, &visited) == -1,
		  "later oversized history fails closed and invalidates the plan");
	ps_compaction_relation_plan_close(plan);
}

static void
bounded_relation_plan_test(void)
{
	PsKey key = {1, 1, 954, 0, PS_KLASS_RELATION};
	unsigned char page[PSZ] = {0};
	PsLayerDesc inputs[3];
	PsImgRec rec = {.key = key, .block = 0, .lsn = 100, .page = page};
	PsViewFence fence = {200, 1, PS_PRUNE_SEQ_UNBOUNDED};
	RelationPlanProbe probe = {0};
	SelectedPlanProbe selection = {0};
	int ok;

	rec.admission_seq = 1;
	ok = ps_image_layer_write(980, 0, &rec, 1, PSZ, &inputs[0]) == 0;
	rec.admission_seq = 2;
	ok = ok && ps_image_layer_write(981, 0, &rec, 1, PSZ, &inputs[1]) == 0;
	ok = ok && ps_image_layer_write(982, 0, &rec, 1, PSZ, &inputs[2]) == 0;
	check(ok && ps_compaction_plan_relation_groups(inputs, 3, 3,
			(PsPruneFence) {150, UINT64_MAX}, &fence, 1, 0, 0,
			visit_relation_plan, &probe) == 0 && probe.calls == 1 &&
		  probe.count == 3 && probe.ids[0] == 980 && probe.ids[1] == 981 &&
		  probe.ids[2] == 982 && probe.keep[0] && !probe.keep[1] && probe.keep[2] &&
		  probe.closure[0],
		  "bounded relation plan preserves finite-sequence closure and latest duplicate");
	check(ps_compaction_plan_relation_groups(inputs, 3, 3,
			(PsPruneFence) {150, UINT64_MAX}, &fence, 1, 0, 0,
			select_relation_plan, &selection) == 0 && selection.calls == 1 &&
		  selection.nselected == 2 && selection.ndropped == 0 &&
		  selection.selected[0].layer_id == 980 &&
		  selection.selected[1].layer_id == 982,
		  "planned duplicate identities select the winning retained source");
	memset(&probe, 0, sizeof(probe));
	check(ok && ps_compaction_plan_relation_groups(inputs, 3, 2,
			(PsPruneFence) {150, UINT64_MAX}, &fence, 1, 0, 0,
			visit_relation_plan, &probe) == -2 && probe.calls == 0,
		  "relation plan refuses an incomplete oversized page history");
	check(ps_compaction_plan_relation_groups(inputs, 3, 3,
			(PsPruneFence) {0, UINT64_MAX}, &fence, 1, 0, 0,
			visit_relation_plan, &probe) == -1 && probe.calls == 0,
		  "relation plan refuses an invalid retention floor");
	rec.key.klass = PS_KLASS_CONTROL;
	check(ps_image_layer_write(983, 0, &rec, 1, PSZ, &inputs[0]) == 0 &&
		  ps_compaction_plan_relation_groups(inputs, 1, 3,
			(PsPruneFence) {150, UINT64_MAX}, &fence, 1, 0, 0,
			visit_relation_plan, &probe) == -1 && probe.calls == 0,
		  "relation plan rejects control-page groups");
	check(ps_compaction_plan_relation_groups(inputs, 2, 3,
			(PsPruneFence) {150, UINT64_MAX}, &fence, 1, 0, 0,
			visit_relation_plan, &probe) == -1 && probe.calls == 1 &&
		  probe.ids[0] == 981,
		  "later unsupported group invalidates an earlier staged relation plan");
}

static void
bounded_merge_test(void)
{
	PsKey key = {1, 1, 955, 0, PS_KLASS_RELATION};
	unsigned char pages[3][PSZ] = {{0}};
	PsLayerDesc layers[3];
	PsImgRec rec = {.key = key, .block = 0, .lsn = 100};
	PsImageIndexMerge *merge = NULL;
	PsImgIndexEnt entry;
	unsigned char out[PSZ];
	BatchSource sources[3];
	PsImgRec output[3];
	PsLayerDesc replacement;
	uint64_t id = 0;
	int ok = 1;

	for (int i = 0; i < 3; i++)
	{
		pages[i][0] = (unsigned char) i;
		rec.page = pages[i];
		if (ps_image_layer_write(995 + i, 0, &rec, 1, PSZ, &layers[2 - i]) != 0)
			ok = 0;
	}
	check(ok && ps_image_index_merge_open(layers, 3, &merge) == 0,
		  "open bounded merge with reverse-ordered layers");
	memset(output, 0, sizeof(output));
	memset(sources, 0, sizeof(sources));
	for (uint64_t expected = 995; expected < 998; expected++)
	{
		check(merge != NULL && ps_image_index_merge_next(merge, &entry, &id) == 1 &&
			  id == expected && entry.lsn == 100 && entry.admission_seq == 0 &&
			  ps_image_layer_lookup(&layers[997 - expected], &key, 0, 100, 0,
									out, PSZ, NULL, NULL) == 1 &&
			  out[0] == (unsigned char) (expected - 995),
			  "equal legacy identities follow layer-ID precedence with source bytes");
		sources[expected - 995].layer = &layers[997 - expected];
		sources[expected - 995].offset = entry.data_off;
		output[expected - 995].key = entry.key;
		output[expected - 995].block = entry.block;
		output[expected - 995].lsn = entry.lsn;
		output[expected - 995].admission_seq = entry.admission_seq;
		output[expected - 995].growth_lsn = entry.growth_lsn;
		output[expected - 995].order_id = entry.order_id;
		output[expected - 995].seg_off = entry.seg_off;
		output[expected - 995].seg_id = entry.seg_id;
		output[expected - 995].flags = entry.flags;
		output[expected - 995].page = &sources[expected - 995];
	}
	check(merge != NULL && ps_image_index_merge_next(merge, &entry, &id) == 0,
		  "bounded merge verifies and completes every input");
	ps_image_index_merge_close(merge);
	check(ps_image_layer_write_sorted_batch(1000, 0, output, 3, PSZ,
			read_batch_source, NULL, &replacement) == 0 &&
		  ps_image_layer_lookup_exact(&replacement, &key, 0, 100, 0,
								out, PSZ, NULL, NULL) == 1 && out[0] == 2,
		  "bounded output preserves the newest legacy source on an exact tie");
	{
		GroupProbe probe = {0};

		check(ps_image_index_visit_groups(layers, 3, 3, visit_group, &probe) == 0 &&
			  probe.groups == 1 && probe.entries == 3 && probe.source_sum == 995 + 996 + 997 &&
			  !probe.wrong_order,
			  "complete page group retains duplicate order and source IDs");
		memset(&probe, 0, sizeof(probe));
		check(ps_image_index_visit_groups(layers, 3, 2, visit_group, &probe) == -2 &&
			  probe.groups == 0,
			  "oversized page group fails without exposing a partial group");
		probe.reject = 1;
		check(ps_image_index_visit_groups(layers, 3, 3, visit_group, &probe) == -1,
			  "group traversal propagates callback failure");
		check(ps_image_index_visit_groups(layers, 3, 0, visit_group, &probe) == -1 &&
			  ps_image_index_visit_groups(layers, 3, PS_IMAGE_GROUP_MAX_VERSIONS + 1,
					visit_group, &probe) == -1,
			  "group traversal enforces memory limits");
	}
	merge = NULL;
	layers[1] = layers[0];
	check(ps_image_index_merge_open(layers, 3, &merge) != 0 && merge == NULL,
		  "bounded merge rejects duplicate source IDs");
	check(ps_image_index_merge_open(layers, PS_IMAGE_MERGE_MAX_INPUTS + 1,
									&merge) != 0 && merge == NULL,
		  "bounded merge enforces its fan-in limit");
}

static int
read_memory_batch(const PsImgRec *rec, void *page, void *arg)
{
	(void) arg;
	memcpy(page, rec->page, PSZ);
	return 0;
}

static void
bounded_shared_payload_test(void)
{
	PsKey key = {1, 1, 958, 0, PS_KLASS_RELATION};
	PsImgRec recs[35] = {0};
	unsigned char *pages = malloc((size_t) 33 * PSZ);
	unsigned char actual[PSZ];
	PsLayerDesc layer;
	PsImgIndexEnt *index = NULL;
	uint32_t nindex = 0;
	int ok = pages != NULL;

	check(ok, "allocate shared-payload fixture");
	if (!ok)
		return;
	for (uint32_t i = 0; i < 33; i++)
		memset(pages + (size_t) i * PSZ, (int) i, PSZ);
	for (uint32_t i = 0; i < 35; i++)
	{
		recs[i].key = key;
		recs[i].block = 0;
		recs[i].lsn = 100;
		recs[i].admission_seq = i + 1;
		recs[i].page = pages + (size_t) (i < 32 ? i : i < 34 ? 31 : 32) * PSZ;
	}
	ok = ps_image_layer_write_sorted_batch(984, 0, recs, 35, PSZ,
										  read_memory_batch, NULL, &layer) == 0;
	check(ok && layer.locations[0].size == (uint64_t) 33 * PSZ +
		  35 * sizeof(PsImgIndexEnt) + sizeof(PsImgFooter) &&
		  ps_image_layer_read_index(&layer, &index, &nindex) == 0 && nindex == 35 &&
		  index[31].data_off == index[32].data_off &&
		  index[32].data_off == index[33].data_off &&
		  index[34].data_off != index[33].data_off,
		  "identical adjacent payloads share one offset across a write-buffer boundary");
	check(ok && ps_image_layer_verify_data(&layer, PSZ) == 0 &&
		  ps_image_layer_lookup_exact(&layer, &key, 0, 100, 33, actual, PSZ, NULL, NULL) == 1 &&
		  memcmp(actual, pages + (size_t) 31 * PSZ, PSZ) == 0 &&
		  ps_image_layer_lookup_exact(&layer, &key, 0, 100, 35, actual, PSZ, NULL, NULL) == 1 &&
		  memcmp(actual, pages + (size_t) 32 * PSZ, PSZ) == 0,
		  "shared payload versions remain checksummed and individually readable");
	free(index);
	index = NULL;
	ok = ps_image_layer_write_compaction_stream(985, 0, recs, 35, PSZ,
											 read_memory_batch, NULL, &layer) == 0;
	check(ok && layer.locations[0].size == (uint64_t) 33 * PSZ +
		  35 * sizeof(PsImgIndexEnt) + sizeof(PsImgFooter) &&
		  ps_image_layer_read_index(&layer, &index, &nindex) == 0 && nindex == 35 &&
		  index[31].data_off == index[33].data_off &&
		  ps_image_layer_lookup_exact(&layer, &key, 0, 100, 34, actual, PSZ, NULL, NULL) == 1 &&
		  memcmp(actual, pages + (size_t) 31 * PSZ, PSZ) == 0,
		  "ordinary compaction also shares bytes while retaining version identities");
	free(index);
	free(pages);
}

static void
bounded_sorted_batch_test(void)
{
	PsImgRec *recs = calloc(PS_IMAGE_BATCH_MAX_RECORDS + 1, sizeof(*recs));
	PsLayerDesc layer;
	PsImgIndexEnt *index = NULL;
	PsKey key = {1, 1, 956, 0, PS_KLASS_RELATION};
	uint32_t nindex = 0;

	check(recs != NULL, "allocate bounded batch input");
	if (recs == NULL)
		return;
	for (uint32_t i = 0; i <= PS_IMAGE_BATCH_MAX_RECORDS; i++)
	{
		recs[i].key = key;
		recs[i].block = i;
		recs[i].lsn = 100;
	}
	stream_pages = 0;
	stream_fail_after = -1;
	check(ps_image_layer_write_sorted_batch(998, 0, recs,
			PS_IMAGE_BATCH_MAX_RECORDS, PSZ, generated_page, NULL, &layer) == 0 &&
		  stream_pages == PS_IMAGE_BATCH_MAX_RECORDS &&
		  layer.locations[0].size == (uint64_t) PS_IMAGE_BATCH_MAX_RECORDS *
			(PSZ + sizeof(PsImgIndexEnt)) + sizeof(PsImgFooter) &&
		  ps_image_layer_read_index(&layer, &index, &nindex) == 0 &&
		  nindex == PS_IMAGE_BATCH_MAX_RECORDS && index[nindex - 1].block == nindex - 1,
		  "sorted batch writes one bounded complete layer");
	free(index);
	{
		GroupProbe probe = {0};

		check(ps_image_index_visit_groups(&layer, 1, 1, visit_group, &probe) == 0 &&
			  probe.groups == PS_IMAGE_BATCH_MAX_RECORDS &&
			  probe.entries == PS_IMAGE_BATCH_MAX_RECORDS,
			  "group traversal separates distinct pages at the smallest bound");
	}
	stream_pages = 0;
	check(ps_image_layer_write_sorted_batch(999, 0, recs,
			PS_IMAGE_BATCH_MAX_RECORDS + 1, PSZ, generated_page, NULL, &layer) != 0 &&
		  stream_pages == 0 && ps_layer_store->layer_exists_local(999) == 0,
		  "oversized batch is rejected before output creation");
	recs[0].block = 2;
	check(ps_image_layer_write_sorted_batch(999, 0, recs, 2, PSZ,
			generated_page, NULL, &layer) != 0 &&
		  stream_pages == 0 && ps_layer_store->layer_exists_local(999) == 0,
		  "unsorted batch is rejected before output creation");
	recs[0].block = 0;
	stream_fail_after = 40;
	check(ps_image_layer_write_sorted_batch(999, 0, recs, 100, PSZ,
			generated_page, NULL, &layer) != 0 &&
		  ps_layer_store->layer_exists_local(999) == 0,
		  "failed batch removes unpublished output");
	stream_fail_after = -1;
	stream_pages = 0;
	free(recs);
}

static void
prepared_batch_boundary_test(void)
{
	PsKey key = {1, 1, 957, 0, PS_KLASS_RELATION};
	PsImgRec *records = calloc(PS_IMAGE_BATCH_MAX_RECORDS, sizeof(*records));
	unsigned char older[PSZ] = {1}, newer[PSZ] = {2}, result[PSZ];
	PsLayerDesc inputs[2], outputs[2];
	uint64_t ids[2] = {1300, 1301}, uncertain = 0, scanned = 0, kept = 0;
	uint32_t noutputs = 0;
	int ok = records != NULL;

	check(ok, "allocate duplicate-identity boundary fixture");
	if (!ok)
		return;
	for (uint32_t i = 0; i < PS_IMAGE_BATCH_MAX_RECORDS; i++)
	{
		records[i].key = key;
		records[i].lsn = 100;
		records[i].page = older;
	}
	ok = ps_image_layer_write(1200, 0, records, PS_IMAGE_BATCH_MAX_RECORDS,
								 PSZ, &inputs[0]) == 0;
	records[0].page = newer;
	ok = ok && ps_image_layer_write(1201, 0, records, 1, PSZ, &inputs[1]) == 0;
	{
		GroupProbe probe = {0};

		check(ok && ps_image_index_visit_groups(inputs, 2, PS_IMAGE_GROUP_MAX_VERSIONS,
				visit_group, &probe) == -2 && probe.groups == 0,
			  "a page history beyond the group bound is rejected intact");
	}
	{
		PsLayerDesc mixed[3];
		PsImgRec prefix = {.key = key, .block = 0, .lsn = 100, .page = older};
		GroupProbe probe = {0};

		prefix.key.relNumber--;
		mixed[0] = inputs[0];
		mixed[1] = inputs[1];
		check(ok && ps_image_layer_write(1199, 0, &prefix, 1, PSZ, &mixed[2]) == 0 &&
			  ps_image_index_visit_groups(mixed, 3, PS_IMAGE_GROUP_MAX_VERSIONS,
				visit_group, &probe) == -2 && probe.groups == 1 && probe.entries == 1,
			  "later oversized group invalidates earlier staged groups");
	}
	check(ok && ps_image_compaction_prepare_counted(inputs, 2, ids, 2, PSZ, outputs,
											&noutputs, &uncertain, &scanned, &kept) == 0 &&
		  noutputs == 1 && uncertain == 0 &&
		  scanned == PS_IMAGE_BATCH_MAX_RECORDS + 1 && kept == 1,
		  "collapse a duplicate identity crossing the batch limit");
	if (noutputs == 1)
	{
		PsImgIndexEnt *index = NULL;
		uint32_t nindex = 0;
		PsLayerDesc wrong;
		PsImgRec wrong_rec = {.key = key, .block = 0, .lsn = 100, .page = older};

		check(ps_image_compaction_verify_coverage(inputs, 2, outputs, 1, PSZ) == 0,
			  "coverage verifies the winning legacy duplicate");
		wrong = outputs[0];
		wrong.start_block++;
		check(ps_image_compaction_verify_coverage(inputs, 2, &wrong, 1, PSZ) != 0,
			  "coverage rejects a descriptor that hides an output page");
		{
			PsLayerDesc invalid_inputs[2] = {inputs[0], inputs[1]};
			PsLayerDesc unused;
			uint64_t new_id = 1306, unknown = 0, bad_scanned = 9, bad_kept = 9;
			uint32_t n = 0;

			invalid_inputs[0].start_block++;
			check(ps_image_compaction_verify_coverage(invalid_inputs, 2, outputs, 1, PSZ) != 0 &&
				  ps_image_compaction_prepare_counted(invalid_inputs, 2, &new_id, 1, PSZ,
										  &unused, &n, &unknown, &bad_scanned, &bad_kept) != 0 &&
				  n == 0 && bad_scanned == 0 && bad_kept == 0,
				  "hidden input pages cannot be promoted by preparation or verification");
		}
		check(ps_image_layer_write(1304, 0, &wrong_rec, 1, PSZ, &wrong) == 0 &&
			  ps_image_compaction_verify_coverage(inputs, 2, &wrong, 1, PSZ) != 0,
			  "coverage rejects correct identity with the wrong source bytes");
		check(ps_layer_store->delete_local_layer(&wrong) == 0,
			  "remove wrong-coverage fixture");
		wrong_rec.page = newer;
		wrong_rec.growth_lsn = 9;
		check(ps_image_layer_write(1305, 0, &wrong_rec, 1, PSZ, &wrong) == 0 &&
			  ps_image_compaction_verify_coverage(inputs, 2, &wrong, 1, PSZ) != 0,
			  "coverage rejects changed recovery metadata even with correct bytes");
		check(ps_layer_store->delete_local_layer(&wrong) == 0,
			  "remove wrong-metadata fixture");
		check(ps_image_layer_lookup_exact(&outputs[0], &key, 0, 100, 0,
										result, PSZ, NULL, NULL) == 1 && result[0] == 2 &&
			  ps_image_layer_read_index(&outputs[0], &index, &nindex) == 0 && nindex == 1,
			  "one prepared identity contains the highest-precedence source");
		free(index);
		for (uint32_t i = 0; i < noutputs; i++)
			check(ps_layer_store->delete_local_layer(&outputs[i]) == 0,
				  "remove boundary fixture output");
	}
	for (uint32_t i = 0; i < PS_IMAGE_BATCH_MAX_RECORDS; i++)
	{
		records[i].block = i;
		records[i].page = older;
	}
	ok = ps_image_layer_write(1202, 0, records, PS_IMAGE_BATCH_MAX_RECORDS,
								 PSZ, &inputs[0]) == 0;
	records[0].block = PS_IMAGE_BATCH_MAX_RECORDS - 1;
	records[1].block = PS_IMAGE_BATCH_MAX_RECORDS;
	records[0].page = newer;
	records[1].page = newer;
	ok = ok && ps_image_layer_write(1203, 0, records, 2, PSZ, &inputs[1]) == 0;
	ids[0] = 1302; ids[1] = 1303;
	noutputs = 0;
	uncertain = 0;
	check(ok && ps_image_compaction_prepare(inputs, 2, ids, 2, PSZ, outputs,
											&noutputs, &uncertain) == 0 &&
		  noutputs == 2 && uncertain == 0,
		  "prepare two batches with a duplicate at the first boundary");
	if (noutputs == 2)
	{
		check(ps_image_compaction_verify_coverage(inputs, 2, outputs, 2, PSZ) == 0 &&
			  ps_image_compaction_verify_coverage(inputs, 2, outputs, 1, PSZ) != 0,
			  "coverage requires every output batch");
		check(ps_image_layer_lookup_exact(&outputs[0], &key,
							 PS_IMAGE_BATCH_MAX_RECORDS - 1, 100, 0, result,
							 PSZ, NULL, NULL) == 1 && result[0] == 2 &&
			  ps_image_layer_lookup_exact(&outputs[1], &key,
							 PS_IMAGE_BATCH_MAX_RECORDS, 100, 0, result,
							 PSZ, NULL, NULL) == 1 && result[0] == 2,
			  "each prepared batch contains the winning copy for its identities");
		for (uint32_t i = 0; i < noutputs; i++)
			check(ps_layer_store->delete_local_layer(&outputs[i]) == 0,
				  "remove split-batch fixture output");
	}
	free(records);
}

static void
prepared_finite_sequence_test(void)
{
	PsKey key = {1, 1, 958, 0, PS_KLASS_RELATION};
	unsigned char legacy[PSZ] = {3}, newer[PSZ] = {4}, page[PSZ];
	PsImgRec rec = {.key = key, .block = 0, .lsn = 100};
	PsLayerDesc inputs[2], output;
	uint64_t id = 1500, uncertain = 0;
	uint32_t noutputs = 0;
	int ok;

	rec.page = legacy;
	ok = ps_image_layer_write(1400, 0, &rec, 1, PSZ, &inputs[0]) == 0;
	rec.page = newer;
	rec.admission_seq = 8;
	ok = ok && ps_image_layer_write(1401, 0, &rec, 1, PSZ, &inputs[1]) == 0;
	check(ok && ps_image_compaction_prepare(inputs, 2, &id, 1, PSZ, &output,
											&noutputs, &uncertain) == 0 && noutputs == 1,
		  "prepare equal-LSN distinct admission sequences");
	if (noutputs == 1)
	{
		check(ps_image_compaction_verify_coverage(inputs, 2, &output, 1, PSZ) == 0,
			  "coverage verifies all finite sequence versions");
		check(ps_image_layer_lookup_exact(&output, &key, 0, 100, 0, page, PSZ,
												NULL, NULL) == 1 && page[0] == 3 &&
			  ps_image_layer_lookup_exact(&output, &key, 0, 100, 8, page, PSZ,
												NULL, NULL) == 1 && page[0] == 4,
			  "exact sequence views retain both logical versions");
		check(ps_layer_store->delete_local_layer(&output) == 0,
			  "remove finite sequence fixture output");
	}
}

int
main(void)
{
	char		dir[] = "/tmp/pslayertestXXXXXX";
	uint32_t	psz = PSZ;
	static unsigned char pg[5][PSZ];
	PsKey		k5 = {1, 1, 5, 0, PS_KLASS_RELATION};	/* relation 5, block 0 */
	PsKey		k6 = {1, 1, 6, 0, PS_KLASS_RELATION};	/* relation 6, block 0 */
	PsKey		k9 = {1, 1, 9, 0, PS_KLASS_RELATION};	/* absent */
	PsLayerDesc d;
	unsigned char out[PSZ];
	int			r;

	if (!mkdtemp(dir) || ps_layer_store->open(dir) != 0)
	{
		fprintf(stderr, "setup failed\n");
		return 2;
	}
	legacy_index_visitor_test();
	bounded_relation_plan_test();
	resumable_relation_plan_test();
	relation_group_selection_test();
	stage_pruned_batch_test();
	verify_pruned_plan_test();
	verify_pruned_plan_duplicate_test();
	bounded_merge_test();
	bounded_shared_payload_test();
	bounded_sorted_batch_test();
	prepared_batch_boundary_test();
	prepared_finite_sequence_test();

	/* Upgraded stores retain sequence-zero tuples alongside same-LSN rewrites.
	 * Exact reads must find the old identity, including when another layer
	 * contains only the rewrite (which must be a miss rather than an error). */
	{
		PsLayerDesc legacy;
		PsImgRec recs[2] = {
			{.key = k5, .block = 0, .lsn = 100, .admission_seq = 0, .page = pg[0]},
			{.key = k5, .block = 0, .lsn = 100, .admission_seq = 9, .page = pg[1]},
		};
		uint64_t lsn, seq;

		memset(pg[0], 0x71, psz);
		memset(pg[1], 0x72, psz);
		check(ps_image_layer_write(700, 0, recs, 2, psz, &legacy) == 0,
			  "write legacy identity and same-LSN rewrite");
		check(ps_image_layer_lookup_exact(&legacy, &k5, 0, 100, 0, out,
			  psz, &lsn, &seq) == 1 && lsn == 100 && seq == 0 && out[128] == 0x71,
			  "exact lookup preserves sequence-zero bytes");
		check(ps_image_layer_lookup(&legacy, &k5, 0, 100, 0, out,
			  psz, &lsn, &seq) == 1 && seq == 9 && out[128] == 0x72,
			  "uncapped lookup still selects newest rewrite");
		check(ps_image_layer_lookup_exact(&legacy, &k5, 0, 100, 8, out,
			  psz, NULL, NULL) == 0, "missing exact identity does not substitute older tuple");
		check(ps_image_layer_write(701, 0, recs + 1, 1, psz, &legacy) == 0,
			  "write rewrite-only layer");
		check(ps_image_layer_lookup_exact(&legacy, &k5, 0, 100, 0, out,
			  psz, NULL, NULL) == 0, "rewrite-only layer misses sequence-zero identity");
	}

	/* versions (out of insertion order on purpose):
	 *   (5,0)@100=0xA1  (5,0)@200=0xA2  (5,0)@300=0xA3
	 *   (5,1)@150=0xB1  (6,0)@250=0xC1 */
	memset(pg[0], 0xA1, psz);
	memset(pg[1], 0xA2, psz);
	memset(pg[2], 0xA3, psz);
	memset(pg[3], 0xB1, psz);
	memset(pg[4], 0xC1, psz);
	{
		PsImgRec	recs[5] = {
			{.key = k5, .block = 0, .lsn = 100, .page = pg[0]},
			{.key = k5, .block = 0, .lsn = 300, .page = pg[2]},
			{.key = k5, .block = 0, .lsn = 200, .page = pg[1]},
			{.key = k5, .block = 1, .lsn = 150, .page = pg[3]},
			{.key = k6, .block = 0, .lsn = 250, .page = pg[4]},
		};
		PsImgIndexEnt *idx = NULL;
		uint32_t	nidx = 0;

		recs[1].growth_lsn = 275;
		recs[1].order_id = 44;
		recs[1].seg_id = 3;
		recs[1].seg_off = 1234;
		recs[1].flags = PS_IMG_REC_SEG_VALID | PS_IMG_REC_ORDERED;

		check(ps_image_layer_write(7, 0, recs, 5, psz, &d) == 0,
			  "write image layer");
		check(ps_image_layer_read_index(&d, &idx, &nidx) == 0 && nidx == 5,
			  "read image v3 index");
		if (idx && nidx == 5)
		{
			PsImgIndexEnt *e = NULL;

			for (uint32_t i = 0; i < nidx; i++)
				if (idx[i].lsn == 300)
					e = &idx[i];
			check(e && e->growth_lsn == 275 && e->order_id == 44 &&
				  e->seg_id == 3 && e->seg_off == 1234 &&
				  e->flags == (PS_IMG_REC_SEG_VALID | PS_IMG_REC_ORDERED),
				  "image v3 recovery metadata round-trips");
		}
		free(idx);
	}

	check(d.start_key.relNumber == 5 && d.end_key.relNumber == 6,
		  "layer key range");
	check(d.lsn_start == 100 && d.lsn_end == 300, "layer lsn range");

	/* newest version <= read_lsn */
	r = ps_image_layer_lookup(&d, &k5, 0, 250, 0, out, psz, NULL, NULL);
	check(r == 1 && out[0] == 0xA2, "(5,0)@<=250 -> version 200");
	r = ps_image_layer_lookup(&d, &k5, 0, 1000, 0, out, psz, NULL, NULL);
	check(r == 1 && out[0] == 0xA3, "(5,0)@<=1000 -> version 300");
	r = ps_image_layer_lookup(&d, &k5, 0, 100, 0, out, psz, NULL, NULL);
	check(r == 1 && out[0] == 0xA1, "(5,0)@<=100 -> version 100 (exact)");
	r = ps_image_layer_lookup(&d, &k5, 0, 50, 0, out, psz, NULL, NULL);
	check(r == 0, "(5,0)@<=50 -> no version (older than oldest)");
	r = ps_image_layer_lookup(&d, &k5, 1, 1000, 0, out, psz, NULL, NULL);
	check(r == 1 && out[0] == 0xB1, "(5,1) -> version 150");
	r = ps_image_layer_lookup(&d, &k6, 0, 1000, 0, out, psz, NULL, NULL);
	check(r == 1 && out[0] == 0xC1, "(6,0) -> version 250");
	r = ps_image_layer_lookup(&d, &k9, 0, 1000, 0, out, psz, NULL, NULL);
	check(r == 0, "absent key -> no version");

	/* The cache holds indexes only: an unverified descriptor copy still
	 * checksums the data section, and emptying the cache (the layer store's
	 * close) makes the next lookup read the index from the file again. */
	{
		PsLayerDesc copy = d;
		unsigned char bad = 0,
					  good = 0xA1;
		int			fd;

		copy.data_verified = false;
		fd = open(d.locations[0].uri, O_WRONLY);
		check(fd >= 0 && pwrite(fd, &bad, 1, 0) == 1 && close(fd) == 0,
			  "corrupt image bytes behind a cached index");
		r = ps_image_layer_lookup(&copy, &k6, 0, 1000, 0, out, psz, NULL, NULL);
		check(r < 0 && !copy.data_verified,
			  "a cached index does not vouch for the data section");
		fd = open(d.locations[0].uri, O_WRONLY);
		check(fd >= 0 && pwrite(fd, &good, 1, 0) == 1 && close(fd) == 0,
			  "restore image bytes");
		ps_image_layer_cache_reset();
		r = ps_image_layer_lookup(&copy, &k6, 0, 1000, 0, out, psz, NULL, NULL);
		check(r == 1 && copy.data_verified && out[0] == 0xC1,
			  "after a cache reset the layer is read, verified and served again");
	}

	/* GC must force a fresh checksum even if an earlier lookup cached success. */
	{
		unsigned char bad = 0,
					  good = 0xA1;
		int			fd = open(d.locations[0].uri, O_WRONLY);

		check(fd >= 0 && pwrite(fd, &bad, 1, 0) == 1 && close(fd) == 0,
			  "corrupt image bytes after cached verification");
		check(ps_image_layer_verify_data(&d, psz) != 0,
			  "forced data verification detects post-read corruption");
		fd = open(d.locations[0].uri, O_RDONLY);
		check(fd >= 0 && ps_image_layer_verify_fd(fd, d.locations[0].size, psz) != 0,
			  "single-file verification detects corrupted data");
		if (fd >= 0)
			close(fd);
		check(!d.data_verified, "failed forced verification clears cached success");
		fd = open(d.locations[0].uri, O_WRONLY);
		check(fd >= 0 && pwrite(fd, &good, 1, 0) == 1 && close(fd) == 0 &&
			  ps_image_layer_verify_data(&d, psz) == 0,
			  "restored image bytes pass forced verification");
		fd = open(d.locations[0].uri, O_RDONLY);
		check(fd >= 0 && ps_image_layer_verify_fd(fd, d.locations[0].size, psz) == 0,
			  "single-file verification accepts restored data");
		if (fd >= 0)
			close(fd);
	}

	/* Lookups go through a cached, binary-searched index: many keys written
	 * in unsorted order must resolve exactly as a full scan would. */
	{
		enum { NK = 300 };
		static unsigned char pages[NK][PSZ];
		static PsImgRec many[NK * 2];
		PsLayerDesc d2;
		int			ok = 1;

		for (uint32_t i = 0; i < NK; i++)
		{
			PsKey		k = k5;

			k.relNumber = 1000 + (i * 7919) % NK;	/* unsorted input */
			memset(pages[i], (int) (k.relNumber & 0xff), psz);
			many[2 * i] = (PsImgRec) {.key = k, .block = i % 3, .lsn = 500, .page = pages[i]};
			many[2 * i + 1] = (PsImgRec) {.key = k, .block = i % 3, .lsn = 400, .page = pg[0]};
		}
		check(ps_image_layer_write(77, 0, many, NK * 2, psz, &d2) == 0,
			  "write a many-key image layer");
		r = ps_image_layer_lookup(&d2, &k5, 0, 1000, 0, out, psz, NULL, NULL);
		check(r == 0, "a key of another cached layer is absent from this one");
		for (uint32_t i = 0; i < NK && ok; i++)
		{
			PsKey		k = k5;
			uint64_t	l = 0;

			k.relNumber = 1000 + i;
			for (uint32_t b = 0; b < 3; b++)
			{
				r = ps_image_layer_lookup(&d2, &k, b, 450, 0, out, psz, &l, NULL);
				if (r == 1 && (l != 400 || out[0] != 0xA1))
					ok = 0;
				r = ps_image_layer_lookup(&d2, &k, b, 1000, 0, out, psz, &l, NULL);
				if (r == 1 && (l != 500 || out[0] != (unsigned char) (k.relNumber & 0xff)))
					ok = 0;
				if (r < 0)
					ok = 0;
			}
		}
		check(ok, "every key resolves to its newest version through the cached index");
		{
			int			hits = 0;

			for (uint32_t i = 0; i < NK; i++)
			{
				PsKey		k = k5;

				k.relNumber = 1000 + i;
				for (uint32_t b = 0; b < 3; b++)
					hits += ps_image_layer_lookup(&d2, &k, b, 1000, 0, out, psz,
												  NULL, NULL) == 1;
			}
			check(hits == NK, "each written (key, block) is found exactly once");
		}
	}

	/* --- delta layer: ordered collect in an LSN range --- */
	{
		PsLayerDesc dd;
		PsDeltaRec	drecs[4] = {
			{k5, 0, 300, "D300", 4}, {k5, 0, 100, "D100", 4},
			{k5, 0, 200, "DD200", 5}, {k5, 1, 150, "E150", 4},
		};
		PsDeltaOut	outs[8];
		uint32_t	dn = 0;
		unsigned char dbuf[16];

		check(ps_delta_layer_write(8, 0, drecs, 4, &dd) == 0, "write delta layer");
		check(dd.kind == PS_LAYER_DELTA && dd.lsn_start == 100 &&
			  dd.lsn_end == 300, "delta layer kind + lsn range");

		/* (5,0) in (100, 300]: expect 200 then 300, ascending; 100 excluded */
		r = ps_delta_layer_collect(&dd, &k5, 0, 100, 300, outs, 8, &dn);
		check(r == 0 && dn == 2, "delta collect (100,300] -> 2 records");
		check(outs[0].lsn == 200 && outs[1].lsn == 300, "deltas in ascending LSN");
		check(ps_layer_store->read_layer_block(&dd, outs[0].data_off, dbuf,
											   outs[0].data_len) == 0 &&
			  outs[0].data_len == 5 && memcmp(dbuf, "DD200", 5) == 0,
			  "delta payload readable (200)");

		dn = 0;
		r = ps_delta_layer_collect(&dd, &k5, 0, 0, 1000, outs, 8, &dn);
		check(r == 0 && dn == 3, "delta collect (0,1000] -> all 3 of (5,0)");
		dn = 0;
		r = ps_delta_layer_collect(&dd, &k5, 1, 0, 1000, outs, 8, &dn);
		check(r == 0 && dn == 1 && outs[0].lsn == 150, "delta collect other block");
	}

	{
		PsLayerDesc huge;
		PsDeltaFooter foot;
		char		uri[PS_LAYER_URI_MAX];
		uint64_t	size = (uint64_t) UINT32_MAX + 1 + sizeof(foot);
		int			fd;

		memset(&huge, 0, sizeof(huge));
		memset(&foot, 0, sizeof(foot));
		foot.magic = PS_DELTA_MAGIC;
		foot.version = PS_DELTA_VERSION;
		foot.index_off = (uint64_t) UINT32_MAX + 1;
		check(ps_layer_store->create_local_layer(11, uri, sizeof(uri)) == 0,
			  "create oversized delta layer shell");
		fd = open(uri, O_WRONLY);
		check(fd >= 0 &&
			  pwrite(fd, &foot, sizeof(foot),
					 (off_t) (size - sizeof(foot))) == (ssize_t) sizeof(foot) &&
			  close(fd) == 0,
			  "write sparse oversized delta footer");
		huge.layer_id = 11;
		huge.kind = PS_LAYER_DELTA;
		huge.location_count = 1;
		huge.locations[0].tier = PS_LAYER_TIER_LOCAL_HOT;
		huge.locations[0].available = true;
		huge.locations[0].size = size;
		snprintf(huge.locations[0].uri, sizeof(huge.locations[0].uri), "%s", uri);
		check(ps_delta_layer_verify_data(&huge) != 0,
			  "delta verifier rejects sections above the 32-bit read limit");
		unlink(uri);
	}

	/* --- read plan: base image + ordered delta chain --- */
	{
		PsLayerDesc img,
					dl;
		PsLayerMap	map;
		PsReadPlan	plan;
		PsImgRec	irecs[2] = {
			{.key = k5, .block = 0, .lsn = 100, .page = pg[0]},
			{.key = k5, .block = 0, .lsn = 200, .page = pg[1]},
		};
		PsDeltaRec	drecs[3] = {
			{k5, 0, 150, "x150", 4}, {k5, 0, 250, "x250", 4}, {k5, 0, 300, "x300", 4},
		};

		check(ps_image_layer_write(9, 0, irecs, 2, psz, &img) == 0, "plan: image");
		check(ps_delta_layer_write(10, 0, drecs, 3, &dl) == 0, "plan: delta");
		ps_layer_map_init(&map);
		ps_layer_map_add(&map, &img);
		ps_layer_map_add(&map, &dl);

		/* @250: base=image@200 (0xA2); deltas in (200,250] = {250} */
		check(ps_read_plan_build(&map, 0, &k5, 0, 250, psz, &plan) == 0, "plan@250");
		check(plan.has_base && plan.base_lsn == 200 && plan.base[0] == 0xA2,
			  "plan@250 base = image@200");
		check(plan.ndelta == 1 && plan.deltas[0].lsn == 250 &&
			  memcmp(plan.deltas[0].bytes, "x250", 4) == 0, "plan@250 one delta");
		ps_read_plan_free(&plan);

		/* @300: base=image@200; deltas in (200,300] = {250,300} ascending */
		check(ps_read_plan_build(&map, 0, &k5, 0, 300, psz, &plan) == 0, "plan@300");
		check(plan.base_lsn == 200 && plan.ndelta == 2 &&
			  plan.deltas[0].lsn == 250 && plan.deltas[1].lsn == 300,
			  "plan@300 chain {250,300}");
		ps_read_plan_free(&plan);

		/* @180: base=image@100 (0xA1); deltas in (100,180] = {150} */
		check(ps_read_plan_build(&map, 0, &k5, 0, 180, psz, &plan) == 0, "plan@180");
		check(plan.base_lsn == 100 && plan.base[0] == 0xA1 && plan.ndelta == 1 &&
			  plan.deltas[0].lsn == 150, "plan@180 base@100 + delta150");
		ps_read_plan_free(&plan);

		ps_layer_map_free(&map);
	}

	streaming_layer_test();
	index_cache_pressure_test();
	index_direct_read_test();
	fprintf(stderr, "%d checks, %d failed\n", run, failed);
	return failed ? 1 : 0;
}
