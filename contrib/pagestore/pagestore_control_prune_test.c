/*
 * Focused core test for control-object retention during image compaction.
 *
 * pg_control images (block 0) and their same-version redo-floor notes (block
 * 1) are pruned by the operational page floor like relation pages, but they
 * are fenced by every retained WAL boundary: the newest image/note pair at or
 * below each owner pin and each live branch cap survives, and the WAL
 * retention floor derived from the surviving notes advances accordingly.  The
 * pair is planned over the complete version chain so a flush boundary between
 * the note and its image can never leave an unnoted image behind.
 *
 * Uses the shared core directly; no daemon, no PostgreSQL.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "pagestore_artifact_format.h"
#include "pagestore_core.h"
#include "pagestore_fault.h"
#include "pagestore_layer.h"
#include "pagestore_layer_store.h"
#include "pagestore_manifest.h"

static int checks;
static int failed;

static void
check(int ok, const char *name)
{
	checks++;
	if (!ok)
	{
		fprintf(stderr, "FAIL: %s\n", name);
		failed++;
	}
}

static void
remove_tree(const char *path)
{
	char command[512];

	if (snprintf(command, sizeof(command), "rm -rf -- '%s'", path) > 0 &&
		system(command) != 0)
		fprintf(stderr, "warning: could not remove %s\n", path);
}

static void
configure_core(int flush_every_page)
{
	page_size = 8192;
	segment_size = 65536;
	flush_pages = flush_every_page ? 1 : 64;
	compact_layers = 0;
	segment_gc_enabled = 1;
	cache_pages = 0;
	use_layers = 1;
	ps_nshards = 1;
	ps_storage = &PsStoragePosix;
}

static PsKey
control_key(void)
{
	PsKey key;

	memset(&key, 0, sizeof(key));
	key.klass = PS_KLASS_CONTROL;
	return key;
}

static int
write_control(uint32_t timeline, uint64_t version, uint64_t redo)
{
	PsKey key = control_key();
	unsigned char page[8192];

	ps_lock_shard_wr(ps_shard_of(&key));
	memset(page, 0, sizeof(page));
	memcpy(page, &redo, sizeof(redo));
	ps_artifact_trailer_set(page, PS_REDO_NOTE_MAGIC, PS_REDO_NOTE_VERSION);
	if (append_page(timeline, &key, PS_REDO_NOTE_BLOCK, page, version, NULL) != 0)
	{
		ps_unlock_shard(ps_shard_of(&key));
		return 0;
	}
	memset(page, 0xC3, sizeof(page));
	memcpy(page, &version, sizeof(version));
	if (append_page(timeline, &key, PS_CONTROL_IMAGE_BLOCK, page, version, NULL) != 0)
	{
		ps_unlock_shard(ps_shard_of(&key));
		return 0;
	}
	ps_unlock_shard(ps_shard_of(&key));
	return ps_storage->sync() == 0;
}

/* The redo-floor note alone: a mirror that timed out before its image.
 * Written without the identity trailer, as every note was before one
 * existed, so the tests keep covering the legacy note. */
static int
write_note(uint32_t timeline, uint64_t version, uint64_t redo)
{
	PsKey key = control_key();
	unsigned char page[8192];
	int rc;

	ps_lock_shard_wr(ps_shard_of(&key));
	memset(page, 0, sizeof(page));
	memcpy(page, &redo, sizeof(redo));
	rc = append_page(timeline, &key, PS_REDO_NOTE_BLOCK, page, version, NULL);
	ps_unlock_shard(ps_shard_of(&key));
	return rc == 0 && ps_storage->sync() == 0;
}

/* A note whose trailer names another format: the floor cannot be derived. */
static int
write_foreign_note(uint32_t timeline, uint64_t version, uint64_t redo)
{
	PsKey key = control_key();
	unsigned char page[8192];
	int rc;

	ps_lock_shard_wr(ps_shard_of(&key));
	memset(page, 0, sizeof(page));
	memcpy(page, &redo, sizeof(redo));
	ps_artifact_trailer_set(page, 0x41424344u, 7u);
	rc = append_page(timeline, &key, PS_REDO_NOTE_BLOCK, page, version, NULL);
	ps_unlock_shard(ps_shard_of(&key));
	return rc == 0 && ps_storage->sync() == 0;
}

/* One checkpoint-completing control publication as the compute mirrors it:
 * the exact-redo pair (note, image) at the checkpoint redo, the admission
 * fence at the redo, then the pair and the fence again at the update LSN. */
static int
write_checkpoint(uint32_t timeline, uint64_t redo, uint64_t update)
{
	PsKey key = control_key();
	unsigned char page[8192];
	PsAdmissionFence fence;
	int rc = 0;

	memset(&fence, 0, sizeof(fence));
	fence.magic = 0x50534146;
	fence.version = 1;
	fence.redo_lsn = redo;
	fence.admission_seq = 1;
	ps_lock_shard_wr(ps_shard_of(&key));
	memset(page, 0, sizeof(page));
	memcpy(page, &redo, sizeof(redo));
	rc |= append_page(timeline, &key, PS_REDO_NOTE_BLOCK, page, redo, NULL);
	memset(page, 0xC3, sizeof(page));
	memcpy(page, &redo, sizeof(redo));
	rc |= append_page(timeline, &key, PS_CONTROL_IMAGE_BLOCK, page, redo, NULL);
	memset(page, 0, sizeof(page));
	memcpy(page, &fence, sizeof(fence));
	rc |= append_page(timeline, &key, PS_ADMISSION_FENCE_BLOCK, page, redo, NULL);
	memset(page, 0, sizeof(page));
	memcpy(page, &redo, sizeof(redo));
	rc |= append_page(timeline, &key, PS_REDO_NOTE_BLOCK, page, update, NULL);
	memset(page, 0xC3, sizeof(page));
	memcpy(page, &update, sizeof(update));
	rc |= append_page(timeline, &key, PS_CONTROL_IMAGE_BLOCK, page, update, NULL);
	memset(page, 0, sizeof(page));
	memcpy(page, &fence, sizeof(fence));
	rc |= append_page(timeline, &key, PS_ADMISSION_FENCE_BLOCK, page, update, NULL);
	ps_unlock_shard(ps_shard_of(&key));
	return rc == 0 && ps_storage->sync() == 0;
}

/* The materializer's durable progress marker (control block 3). */
static int
write_marker(uint32_t timeline, uint64_t lsn)
{
	PsKey key = control_key();
	unsigned char page[8192];
	int rc;

	memset(page, 0x3d, sizeof(page));
	memcpy(page, &lsn, sizeof(lsn));
	ps_lock_shard_wr(ps_shard_of(&key));
	rc = append_page(timeline, &key, 3, page, lsn, NULL);
	ps_unlock_shard(ps_shard_of(&key));
	return rc == 0 && ps_storage->sync() == 0;
}

static int
write_relation(uint32_t timeline, uint32_t block, uint64_t lsn)
{
	PsKey key = {1, 1, 1, 0, PS_KLASS_RELATION};
	unsigned char page[8192];
	uint32_t hi = (uint32_t) (lsn >> 32);
	uint32_t lo = (uint32_t) lsn;
	int rc;

	memset(page, 0x5A, sizeof(page));
	memcpy(page, &hi, sizeof(hi));
	memcpy(page + sizeof(hi), &lo, sizeof(lo));
	ps_lock_shard_wr(ps_shard_of(&key));
	rc = append_page(timeline, &key, block, page, lsn, NULL);
	ps_unlock_shard(ps_shard_of(&key));
	return rc == 0;
}

static uint64_t
wal_floor(uint32_t timeline)
{
	PsChannel ch;

	memset(&ch, 0, sizeof(ch));
	ch.opcode = PS_OP_WAL_RETAIN_FLOOR;
	ch.timeline = timeline;
	ch.key = control_key();
	ch.status = PS_STATUS_OK;
	ps_lifecycle_read_lock();
	(void) ps_handle_meta(&ch);
	ps_lifecycle_read_unlock();
	return ch.status == PS_STATUS_OK ? ch.req_lsn : UINT64_MAX;
}

static int
reserve_pin(uint32_t timeline, uint32_t kind, uint64_t owner_id,
			uint32_t generation, uint32_t resources, uint64_t lsn)
{
	PsChannel ch;

	memset(&ch, 0, sizeof(ch));
	ch.opcode = PS_OP_RETENTION_PIN_RESERVE;
	ch.timeline = timeline;
	ch.blocknum = kind;
	ch.parent_timeline = resources;
	ch.old_nblocks = generation;
	ch.req_seq = owner_id;
	ch.req_lsn = lsn;
	ch.status = PS_STATUS_OK;
	ps_lifecycle_read_lock();
	(void) ps_handle_meta(&ch);
	ps_lifecycle_read_unlock();
	return ch.status == PS_STATUS_OK;
}

static int
drop_pin(uint32_t timeline, uint32_t kind, uint64_t owner_id, uint32_t generation)
{
	PsChannel ch;

	memset(&ch, 0, sizeof(ch));
	ch.opcode = PS_OP_RETENTION_PIN_DROP;
	ch.timeline = timeline;
	ch.blocknum = kind;
	ch.old_nblocks = generation;
	ch.req_seq = owner_id;
	ch.status = PS_STATUS_OK;
	ps_lifecycle_read_lock();
	(void) ps_handle_meta(&ch);
	ps_lifecycle_read_unlock();
	return ch.status == PS_STATUS_OK;
}

static int
create_branch(uint32_t timeline, uint32_t parent, uint64_t lsn)
{
	PsChannel ch;

	memset(&ch, 0, sizeof(ch));
	ch.opcode = PS_OP_CREATE_BRANCH;
	ch.timeline = timeline;
	ch.parent_timeline = parent;
	ch.req_lsn = lsn;
	ch.status = PS_STATUS_OK;
	ps_lifecycle_read_lock();
	ps_admission_read_lock();
	ps_lock_shard_wr(0);
	ps_lock_map_wr();
	(void) ps_handle_meta(&ch);
	ps_unlock_map();
	ps_unlock_shard(0);
	ps_admission_read_unlock();
	ps_lifecycle_read_unlock();
	return ch.status == PS_STATUS_OK;
}

static int
read_control_at(uint32_t timeline, uint32_t block, uint64_t lsn,
				uint64_t *version_out)
{
	PsKey key = control_key();
	unsigned char page[8192];
	int rc;

	ps_lifecycle_read_lock();
	ps_lock_shard_rd(ps_shard_of(&key));
	rc = read_resolve(timeline, &key, block, lsn, 0, page, NULL);
	ps_unlock_shard(ps_shard_of(&key));
	ps_lifecycle_read_unlock();
	if (rc != 1)
		return 0;
	memcpy(version_out, page, sizeof(*version_out));
	return 1;
}

static void
run_maintenance(int rounds)
{
	for (int i = 0; i < rounds; i++)
		(void) ps_core_maintenance();
}

static void
close_store(void)
{
	ps_core_close();
	if (ps_storage->close != NULL)
		ps_storage->close();
}

static int
read_relation_asof(uint32_t block, uint64_t lsn, uint64_t expected)
{
	PsKey key = {1, 1, 1, 0, PS_KLASS_RELATION};
	unsigned char page[8192];
	uint32_t hi, lo;
	int rc;

	ps_lifecycle_read_lock();
	ps_lock_shard_rd(ps_shard_of(&key));
	rc = read_resolve(0, &key, block, lsn, 0, page, NULL);
	ps_unlock_shard(ps_shard_of(&key));
	ps_lifecycle_read_unlock();
	if (rc != 1)
		return 0;
	memcpy(&hi, page, sizeof(hi));
	memcpy(&lo, page + sizeof(hi), sizeof(lo));
	return (((uint64_t) hi << 32) | lo) == expected;
}

static int
read_relation_at(uint32_t block, uint64_t lsn)
{
	return read_relation_asof(block, lsn, lsn);
}

static void
test_segment_gc_does_not_starve_compaction(void)
{
	char store[] = "/tmp/pagestore-gc-compaction-fairness-XXXXXX";
	uint64_t before;
	int wrote = 1;

	configure_core(1);
	compact_layers = 1;
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0,
		  "open segment GC and compaction fairness store");
	for (uint32_t block = 0; block < 24; block++)
		wrote &= write_relation(0, block, 1000 + block);
	check(wrote && ps_storage->seg_size(0, 0) > 0,
		  "create closed segments and excess image layers");
	before = ps_test_compaction_count();
	(void) ps_core_maintenance();
	check(ps_storage->seg_size(0, 0) < 0,
		  "the maintenance turn reclaims a closed segment");
	(void) ps_core_maintenance();
	check(ps_test_compaction_count() > before,
		  "a reclaimed segment does not prevent compaction on the next turn");
	run_maintenance(64);
	check(read_relation_at(0, 1000) && read_relation_at(23, 1023),
		  "fair segment reclamation and compaction preserve pages");
	close_store();
	remove_tree(store);
}

static void
test_nonzero_floor_bounded_plan(int prune_needed)
{
	char store[] = "/tmp/pagestore-bounded-floor-XXXXXX";
	uint64_t before, prune_before;

	configure_core(1);
	segment_gc_enabled = 0;
	compact_layers = 100;
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0,
		  "open nonzero-floor compaction store");
	check(reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
					  PS_RETENTION_RESOURCE_PAGE_HISTORY, 5000),
		  "set nonzero page-history floor");
	check(write_relation(0, 0, 1000) &&
		  write_relation(0, prune_needed ? 0 : 1, 2000) &&
		  write_relation(0, 2, 3000),
		  "write relation-only input layers");
	before = ps_test_bounded_nonzero_plan_count();
	prune_before = ps_test_bounded_prune_published_count();
	compact_layers = 1;
	/* Count is process-global, so compare with the value before this store. */
	for (int i = 0; i < 64 && !prune_needed &&
		 ps_test_bounded_nonzero_plan_count() == before; i++)
		(void) ps_core_maintenance();
	if (!prune_needed)
	{
		check(ps_test_bounded_nonzero_plan_count() > before &&
			  ps_test_page_prune_due(0, 0),
			  "nonzero bounded merge preserves a later pruning request");
		check(read_relation_at(0, 1000) && read_relation_at(1, 2000) &&
			  read_relation_at(2, 3000),
			  "bounded nonzero-floor merge preserves every page");
	}
	run_maintenance(64);
	if (prune_needed)
	{
		uint32_t old_versions = 0;

		ps_lock_map_rd();
		for (uint32_t i = 0; i < ps_layer_map.nlayers; i++)
		{
			PsImgIndexEnt *idx = NULL;
			uint32_t n = 0;

			if (ps_layer_map.layers[i].kind != PS_LAYER_IMAGE ||
				ps_layer_map.layers[i].deleting)
				continue;
			if (ps_image_layer_read_index(&ps_layer_map.layers[i], &idx, &n) == 0)
				for (uint32_t j = 0; j < n; j++)
					if (idx[j].key.klass == PS_KLASS_RELATION &&
						idx[j].block == 0 && idx[j].lsn == 1000)
						old_versions++;
			free(idx);
		}
		ps_unlock_map();
		check(ps_test_bounded_nonzero_plan_count() == before,
			  "prunable history falls back from the bounded merge");
		check(ps_test_bounded_prune_published_count() > prune_before,
			  "prunable history publishes through the bounded pruning path");
		check(old_versions == 0 && read_relation_asof(0, 5000, 2000),
			  "bounded pruning removes obsolete layer history and preserves its successor");
	}
	else
		check(!ps_test_page_prune_due(0, 0),
			  "deferred pruning eventually completes");
	close_store();
	check(ps_core_open(store) == 0 && read_relation_asof(0, 5000,
											prune_needed ? 2000 : 1000) &&
		  read_relation_asof(2, 5000, 3000),
		  "nonzero-floor result survives restart");
	close_store();
	remove_tree(store);
}

static void
test_bounded_compaction_maintenance(void)
{
	char store[] = "/tmp/pagestore-bounded-core-XXXXXX";
	uint64_t before, zero_before;
	uint32_t live = 0;
	int shared = 0;

	configure_core(1);
	compact_layers = 100;
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0,
		  "open bounded compaction store");
	check(write_relation(0, 0, 1000), "seed bounded compaction store");
	run_maintenance(32); /* consume startup pruning request */
	check(!ps_test_page_prune_due(0, 0), "startup pruning has converged");
	check(write_relation(0, 1, 2000) && write_relation(0, 1, 2000) &&
		  write_relation(0, 2, 3000),
		  "add bounded compaction inputs");
	before = ps_test_bounded_compaction_count();
	zero_before = ps_test_bounded_zero_floor_count();
	compact_layers = 1;
	run_maintenance(64);
	check(ps_test_bounded_compaction_count() > before,
		  "maintenance invokes bounded local compaction");
	check(ps_test_bounded_zero_floor_count() > zero_before,
		  "zero-floor maintenance bypasses the unbounded planner");
	ps_lock_map_rd();
	for (uint32_t i = 0; i < ps_layer_map.nlayers; i++)
		if (ps_layer_map.layers[i].kind == PS_LAYER_IMAGE &&
			!ps_layer_map.layers[i].deleting && ps_layer_map.layers[i].timeline == 0)
		{
			PsImgIndexEnt *index = NULL;
			uint32_t count = 0;

			live++;
			if (ps_image_layer_read_index(&ps_layer_map.layers[i], &index, &count) == 0)
				for (uint32_t j = 1; j < count; j++)
					if (index[j - 1].key.klass == PS_KLASS_RELATION &&
						index[j - 1].block == 1 && index[j].block == 1 &&
						index[j - 1].lsn == 2000 && index[j].lsn == 2000 &&
						index[j - 1].admission_seq != index[j].admission_seq &&
						index[j - 1].data_off == index[j].data_off)
						shared = 1;
			free(index);
		}
	ps_unlock_map();
	check(live == 1, "bounded compaction reduces live layers");
	check(shared, "real compaction shares identical pages with distinct sequences");
	check(read_relation_at(0, 1000) &&
		  read_relation_at(1, 2000) && read_relation_at(2, 3000),
		  "bounded compaction preserves source versions");
	close_store();
	check(ps_core_open(store) == 0 &&
		  read_relation_at(0, 1000) &&
		  read_relation_at(1, 2000) && read_relation_at(2, 3000),
		  "bounded compaction survives restart");
	close_store();
	remove_tree(store);
}

static int
read_fixture_page(const PsImgRec *rec, void *out, void *arg)
{
	(void) arg;
	memcpy(out, rec->page, 8192);
	return 0;
}

static void
test_bounded_shared_payload_capacity_fallback(void)
{
	char store[] = "/tmp/pagestore-bounded-capacity-XXXXXX";
	PsKey key = {1, 1, 1, 0, PS_KLASS_RELATION};
	PsImgRec *recs = calloc(1100, sizeof(*recs));
	PsLayerDesc first, second;
	unsigned char page[8192] = {0};
	uint64_t before;
	uint32_t live = 0, nentries = 0;
	int ok = recs != NULL;

	configure_core(1);
	compact_layers = 1;
	check(ok && mkdtemp(store) != NULL && ps_core_open(store) == 0,
		  "open shared-payload capacity store");
	if (!ok)
		return;
	for (uint32_t i = 0; i < 1100; i++)
	{
		recs[i].key = key;
		recs[i].lsn = 100;
		recs[i].admission_seq = i + 1;
		recs[i].page = page;
	}
	ok = ps_image_layer_write_compaction_stream(1, 0, recs, 1100, 8192,
			read_fixture_page, NULL, &first) == 0;
	recs[0].block = 1;
	recs[0].admission_seq = 1101;
	ok = ok && ps_image_layer_write_compaction_stream(2, 0, recs, 1, 8192,
			read_fixture_page, NULL, &second) == 0;
	ps_lock_map_wr();
	ok = ok && ps_manifest_add_layer(&first) == 0 && ps_manifest_add_layer(&second) == 0;
	ps_unlock_map();
	check(ok && first.locations[0].size < 2 * 1024 * 1024,
		  "seed more than one batch of logical versions in small shared-payload inputs");
	close_store();
	check(ok && ps_core_open(store) == 0, "recover shared-payload capacity store");
	before = ps_test_bounded_compaction_count();
	run_maintenance(64);
	ps_lock_map_rd();
	for (uint32_t i = 0; i < ps_layer_map.nlayers; i++)
		if (ps_layer_map.layers[i].kind == PS_LAYER_IMAGE && !ps_layer_map.layers[i].deleting)
		{
			PsImgIndexEnt *index = NULL;
			uint32_t n = 0;

			live++;
			if (ps_image_layer_read_index(&ps_layer_map.layers[i], &index, &n) == 0)
				nentries += n;
			free(index);
		}
	ps_unlock_map();
	check(ps_test_bounded_compaction_count() > before && live == 1 && nentries == 1101,
		  "capacity exhaustion falls back and preserves all logical versions");
	close_store();
	free(recs);
	remove_tree(store);
}

typedef struct MultiOutputConflict
{
	int mode;
	int called;
	int wrote;
} MultiOutputConflict;

static void
multi_output_conflict(void *arg)
{
	MultiOutputConflict *probe = arg;

	probe->called++;
	ps_test_set_prune_batch_hook(NULL, NULL);
	if (probe->mode == 2)
		_exit(86);
	probe->wrote = write_relation(0, 1090, 4000);
}

static void
test_bounded_prune_multiple_outputs(int mode)
{
	char store[] = "/tmp/pagestore-bounded-prune-multi-XXXXXX";
	uint64_t before;
	uint32_t live = 0, entries = 0;
	int ok = 1;
	MultiOutputConflict probe = {.mode = mode};

	configure_core(0);
	segment_gc_enabled = 0;
	compact_layers = 100;
	if (mkdtemp(store) == NULL || ps_core_open(store) != 0)
	{
		check(0, "open multi-output pruning fixture");
		return;
	}
	ok = write_relation(0, 0, 1000) && write_relation(0, 0, 2000);
	for (uint32_t i = 1; i <= 1087 && ok; i++)
		ok = write_relation(0, i, 3000);
	check(ok && reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
						 PS_RETENTION_RESOURCE_PAGE_HISTORY, 5000),
		  "write multi-output inputs and set retention floor");
	before = ps_test_bounded_prune_published_count();
	compact_layers = 1;
	if (mode == 2)
	{
		pid_t child;
		int status = 0;

		close_store();
		child = fork();
		if (child == 0)
		{
			if (ps_core_open(store) != 0)
				_exit(87);
			ps_test_set_prune_batch_hook(multi_output_conflict, &probe);
			run_maintenance(64);
			_exit(88);
		}
		check(child > 0 && waitpid(child, &status, 0) == child &&
			  WIFEXITED(status) && WEXITSTATUS(status) == 86,
			  "crash occurs after the first bounded output is published");
		check(ps_core_open(store) == 0 && read_relation_asof(0, 5000, 2000) &&
			  read_relation_asof(1086, 5000, 3000),
			  "partial output publication survives a crash");
	}
	else
	{
		if (mode == 1)
			ps_test_set_prune_batch_hook(multi_output_conflict, &probe);
		for (int i = 0; i < 64 && ps_test_bounded_prune_published_count() == before &&
			 !probe.called; i++)
			(void) ps_core_maintenance();
		ps_test_set_prune_batch_hook(NULL, NULL);
		if (mode == 1)
			check(probe.called && probe.wrote &&
				  ps_test_bounded_prune_published_count() == before &&
				  ps_test_page_prune_due(0, 0) && read_relation_asof(1090, 5000, 4000),
				  "concurrent writer stops partial publication and leaves pruning due");
	}
	if (mode != 0)
	{
		uint32_t before_retry = 0;

		ps_lock_map_rd();
		for (uint32_t i = 0; i < ps_layer_map.nlayers; i++)
			if (ps_layer_map.layers[i].kind == PS_LAYER_IMAGE &&
				!ps_layer_map.layers[i].deleting)
				before_retry++;
		ps_unlock_map();
		check(before_retry >= 18,
			  "partial publication leaves every source layer live before retry");
		run_maintenance(64);
	}
	compact_layers = 100;
	ps_lock_map_rd();
	for (uint32_t i = 0; i < ps_layer_map.nlayers; i++)
		if (ps_layer_map.layers[i].kind == PS_LAYER_IMAGE && !ps_layer_map.layers[i].deleting)
		{
			PsImgIndexEnt *index = NULL;
			uint32_t n = 0;

			live++;
			if (ps_image_layer_read_index(&ps_layer_map.layers[i], &index, &n) == 0)
				entries += n;
			free(index);
		}
	ps_unlock_map();
	if (mode == 0)
		check(ps_test_bounded_prune_published_count() > before && live == 2 && entries == 1087,
			  "bounded pruning publishes multiple outputs and drops the obsolete identity");
	check(read_relation_asof(0, 5000, 2000) && read_relation_asof(1086, 5000, 3000) &&
		  (mode != 1 || read_relation_asof(1090, 5000, 4000)),
		  "multi-output pruning preserves pages across batch boundary");
	close_store();
	check(ps_core_open(store) == 0 && read_relation_asof(0, 5000, 2000) &&
		  read_relation_asof(1086, 5000, 3000) &&
		  (mode != 1 || read_relation_asof(1090, 5000, 4000)),
		  "multi-output pruning survives restart");
	close_store();
	remove_tree(store);
}

typedef struct PruneIndexProbe
{
	int mode, called, wrote;
} PruneIndexProbe;

static void
prune_index_conflict(void *arg)
{
	PruneIndexProbe *probe = arg;

	probe->called++;
	ps_test_set_prune_index_hook(NULL, NULL);
	if (probe->mode == 2)
		_exit(86);
	if (probe->mode == 1)
		probe->wrote = write_relation(0, 1200, 4000);
}

static void
test_bounded_prune_index_batches(int mode)
{
	char store[] = "/tmp/pagestore-prune-index-batches-XXXXXX";
	PruneIndexProbe probe = {.mode = mode};
	uint64_t before;
	uint32_t live = 0;
	int ok = 1;

	configure_core(0);
	segment_gc_enabled = 0;
	compact_layers = 100;
	if (mkdtemp(store) == NULL || ps_core_open(store) != 0)
	{
		check(0, "open batched index pruning store");
		return;
	}
	for (uint32_t i = 0; i < 1152 && ok; i++)
		ok = write_relation(0, i, 1000) && write_relation(0, i, 2000);
	check(ok && reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
						 PS_RETENTION_RESOURCE_PAGE_HISTORY, 5000),
		  "prepare more than 1024 obsolete page identities");
	before = ps_test_bounded_prune_published_count();
	compact_layers = 1;
	if (mode == 2)
	{
		pid_t child;
		int status = 0;

		close_store();
		child = fork();
		if (child == 0)
		{
			if (ps_core_open(store) != 0)
				_exit(87);
			ps_test_set_prune_index_hook(prune_index_conflict, &probe);
			run_maintenance(64);
			_exit(88);
		}
		check(child > 0 && waitpid(child, &status, 0) == child &&
			  WIFEXITED(status) && WEXITSTATUS(status) == 86,
			  "crash after the first bounded page-index update");
		check(ps_core_open(store) == 0 && read_relation_asof(0, 5000, 2000) &&
			  read_relation_asof(1151, 5000, 2000),
			  "partial page-index update recovers after crash");
	}
	else
	{
		ps_test_set_prune_index_hook(prune_index_conflict, &probe);
		for (int i = 0; i < 64 && !probe.called; i++)
			(void) ps_core_maintenance();
		ps_test_set_prune_index_hook(NULL, NULL);
		check(probe.called && ps_test_bounded_prune_published_count() > before &&
			  (mode != 1 || (probe.wrote && ps_test_page_prune_due(0, 0))),
			  "bounded pruning splits index updates and fences concurrent writes");
	}
	if (mode != 0)
	{
		ps_lock_map_rd();
		for (uint32_t i = 0; i < ps_layer_map.nlayers; i++)
			if (ps_layer_map.layers[i].kind == PS_LAYER_IMAGE && !ps_layer_map.layers[i].deleting)
				live++;
		ps_unlock_map();
		check(live >= 38, "partial index update retains all source layers");
	}
	run_maintenance(64);
	compact_layers = 100;
	if (mode == 0)
	{
		uint32_t obsolete = 0;

		ps_lock_map_rd();
		for (uint32_t i = 0; i < ps_layer_map.nlayers; i++)
			if (ps_layer_map.layers[i].kind == PS_LAYER_IMAGE && !ps_layer_map.layers[i].deleting)
			{
				PsImgIndexEnt *index = NULL;
				uint32_t count = 0;

				if (ps_image_layer_read_index(&ps_layer_map.layers[i], &index, &count) == 0)
					for (uint32_t j = 0; j < count; j++)
						if (index[j].key.klass == PS_KLASS_RELATION && index[j].lsn == 1000)
							obsolete++;
				free(index);
			}
		ps_unlock_map();
		check(obsolete == 0, "all 1152 obsolete identities are absent from live layers");
	}
	check(read_relation_asof(0, 5000, 2000) && read_relation_asof(1151, 5000, 2000) &&
		  (mode != 1 || read_relation_asof(1200, 5000, 4000)),
		  "index pruning and retries preserve retained pages");
	close_store();
	check(ps_core_open(store) == 0 && read_relation_asof(0, 5000, 2000) &&
		  read_relation_asof(1151, 5000, 2000) &&
		  (mode != 1 || read_relation_asof(1200, 5000, 4000)),
		  "index pruning survives a subsequent restart");
	close_store();
	remove_tree(store);
}

typedef struct HotPagePruneProbe
{
	int mode, called, midpoint_ok, wrote;
} HotPagePruneProbe;

static void
hot_page_prune_hook(void *arg)
{
	HotPagePruneProbe *probe = arg;
	PsKey key = {1, 1, 1, 0, PS_KLASS_RELATION};

	probe->called++;
	probe->midpoint_ok = ps_test_page_version_count(0, &key, 0) == 128 &&
		read_relation_asof(0, 5000, 1255);
	ps_test_set_prune_index_hook(NULL, NULL);
	if (probe->mode == 2)
		_exit(probe->midpoint_ok ? 86 : 87);
	if (probe->mode == 1)
		probe->wrote = write_relation(0, 0, 2000);
}

static void
test_hot_page_prune_batches(int mode)
{
	char store[] = "/tmp/pagestore-prune-hot-page-XXXXXX";
	PsKey key = {1, 1, 1, 0, PS_KLASS_RELATION};
	HotPagePruneProbe probe = {.mode = mode};
	uint64_t before;
	int ok = 1;

	configure_core(0);
	segment_gc_enabled = 0;
	compact_layers = 100;
	if (mkdtemp(store) == NULL || ps_core_open(store) != 0)
	{
		check(0, "open hot-page pruning store");
		return;
	}
	for (uint32_t i = 0; i < 256 && ok; i++)
		ok = write_relation(0, 0, 1000 + i);
	check(ok && ps_test_page_version_count(0, &key, 0) == 256 &&
		  reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
					  PS_RETENTION_RESOURCE_PAGE_HISTORY, 5000),
		  "prepare one page with 256 durable versions");
	before = ps_test_bounded_prune_published_count();
	compact_layers = 1;
	if (mode == 2)
	{
		pid_t child;
		int status = 0;

		close_store();
		child = fork();
		if (child == 0)
		{
			if (ps_core_open(store) != 0)
				_exit(88);
			ps_test_set_prune_index_hook(hot_page_prune_hook, &probe);
			run_maintenance(64);
			_exit(89);
		}
		check(child > 0 && waitpid(child, &status, 0) == child &&
			  WIFEXITED(status) && WEXITSTATUS(status) == 86,
			  "crash after the first hot-page index batch remains readable");
		check(ps_core_open(store) == 0 && read_relation_asof(0, 5000, 1255),
			  "hot-page pruning recovers after the partial index update");
	}
	else
	{
		ps_test_set_prune_index_hook(hot_page_prune_hook, &probe);
		for (int i = 0; i < 64 && !probe.called; i++)
			(void) ps_core_maintenance();
		ps_test_set_prune_index_hook(NULL, NULL);
		check(probe.called && probe.midpoint_ok &&
			  ps_test_bounded_prune_published_count() > before &&
			  (mode != 1 || (probe.wrote && ps_test_page_prune_due(0, 0))),
			  "hot-page versions remain readable between index batches");
	}
	run_maintenance(64);
	compact_layers = 100;
	check(read_relation_asof(0, 5000, mode == 1 ? 2000 : 1255),
		  "hot-page pruning and retry preserve the newest version");
	if (mode == 0)
		check(ps_test_page_version_count(0, &key, 0) == 1,
			  "all obsolete hot-page versions are removed");
	close_store();
	check(ps_core_open(store) == 0 &&
		  read_relation_asof(0, 5000, mode == 1 ? 2000 : 1255),
		  "hot-page pruning preserves the newest version across restart");
	close_store();
	remove_tree(store);
}

typedef struct LongChainProbe
{
	int mode, called, midpoint_ok, wrote, reclaimed, worker_started;
	uint32_t block;
	pthread_t worker;
} LongChainProbe;

static void *
long_chain_gc_worker(void *arg)
{
	(void) arg;
	(void) ps_core_maintenance();
	return NULL;
}

static void
long_chain_snapshot_hook(void *arg)
{
	LongChainProbe *probe = arg;
	PsKey key = {1, 1, 1, 0, PS_KLASS_RELATION};

	probe->called++;
	probe->midpoint_ok = ps_test_page_version_count(0, &key, probe->block) ==
		(probe->mode == 5 ? 1537U : 2048U) &&
		read_relation_asof(probe->block, 5000, 1511) &&
		read_relation_asof(probe->block, 10000, 7535);
	ps_test_set_prune_snapshot_hook(NULL, NULL);
	ps_test_set_prune_snapshot_swap_hook(NULL, NULL);
	if (probe->mode == 2)
		_exit(probe->midpoint_ok ? 86 : 87);
	if (probe->mode == 1 || probe->mode == 5)
		probe->wrote = write_relation(0, probe->block, 9000);
	if (probe->mode == 3)
	{
		segment_gc_enabled = 1;
		probe->worker_started = pthread_create(&probe->worker, NULL, long_chain_gc_worker, NULL) == 0;
		for (int i = 0; probe->worker_started && i < 10000 &&
			 ps_storage->seg_size(0, 0) >= 0; i++)
			usleep(1000);
		probe->reclaimed = ps_storage->seg_size(0, 0) < 0;
	}
}

static int
long_chain_finish_setup(const char *store, uint32_t block)
{
	PsKey key = {1, 1, 1, 0, PS_KLASS_RELATION};
	int ok = ps_core_open(store) == 0;

	for (uint32_t i = 0; i < 1536 && ok; i++)
		ok = write_relation(0, block, 6000 + i);
	return ok && ps_test_page_version_count(0, &key, block) == 2048 &&
		reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
					PS_RETENTION_RESOURCE_PAGE_HISTORY, 5000);
}

static void
test_long_chain_prune_snapshot(int mode)
{
	char store[] = "/tmp/pagestore-prune-long-chain-XXXXXX";
	PsKey key = {1, 1, 1, 0, PS_KLASS_RELATION};
	LongChainProbe probe = {.mode = mode, .block = mode == 4 ? 1 : 0};
	uint32_t block = probe.block;
	uint64_t before;
	int ok = 1;

	configure_core(0);
	if (mode == 3)
		flush_pages = 512;
	segment_size = (mode == 3 ? 4 : 32) * 1024 * 1024;
	segment_gc_enabled = 0;
	compact_layers = 100;
	if (mkdtemp(store) == NULL || ps_core_open(store) != 0)
	{
		check(0, "open long-page-chain pruning store");
		return;
	}
	if (mode == 4)
		ok = write_relation(0, 0, 1000) && write_relation(0, 0, 2000);
	for (uint32_t i = 0; i < 512 && ok; i++)
		ok = write_relation(0, block, 1000 + i);
	if (mode == 3)
	{
		ok = ok && ps_test_resize_empty_memtable(0, 100000) == 0;
		for (uint32_t i = 0; i < 1536 && ok; i++)
			ok = write_relation(0, 0, 6000 + i);
		ok = ok && reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
							 PS_RETENTION_RESOURCE_PAGE_HISTORY, 5000);
	}
	else
		close_store();
	flush_pages = 100000;
	compact_layers = 1;
	if (mode == 2)
	{
		pid_t child = fork();
		int status = 0;

		if (child == 0)
		{
			if (!ok || !long_chain_finish_setup(store, block))
				_exit(88);
			ps_test_set_prune_snapshot_hook(long_chain_snapshot_hook, &probe);
			run_maintenance(64);
			_exit(89);
		}
		check(child > 0 && waitpid(child, &status, 0) == child &&
			  WIFEXITED(status) && WEXITSTATUS(status) == 86,
			  "crash while copying a long chain after publication");
		check(ps_core_open(store) == 0 && read_relation_asof(block, 5000, 1511) &&
			  read_relation_asof(block, 10000, 7535),
			  "long-chain snapshot crash recovers both retained views");
	}
	else
	{
		check(ok && (mode == 3 || long_chain_finish_setup(store, block)),
			  "prepare a long page chain with a prunable durable prefix");
		before = ps_test_bounded_prune_published_count();
		if (mode == 5)
			ps_test_set_prune_snapshot_swap_hook(long_chain_snapshot_hook, &probe);
		else
			ps_test_set_prune_snapshot_hook(long_chain_snapshot_hook, &probe);
		for (int i = 0; i < 64 && !probe.called; i++)
			(void) ps_core_maintenance();
		ps_test_set_prune_snapshot_hook(NULL, NULL);
		ps_test_set_prune_snapshot_swap_hook(NULL, NULL);
		if (probe.worker_started)
			(void) pthread_join(probe.worker, NULL);
		segment_gc_enabled = 0;
		check(probe.called && probe.midpoint_ok &&
			  ps_test_bounded_prune_published_count() > before &&
			  (mode != 1 || (probe.wrote && ps_test_page_prune_due(0, 0) &&
						 ps_test_page_version_count(0, &key, block) == 2049)) &&
			  (mode != 5 || (probe.wrote && ps_test_page_prune_due(0, 0) &&
						 ps_test_page_version_count(0, &key, block) == 1538)) &&
			  (mode != 3 || (probe.reclaimed && ps_test_page_prune_due(0, 0) &&
						 ps_test_page_version_count(0, &key, block) == 2048)),
			  "long chain is copied in chunks and rejects a concurrent mutation");
		if (mode == 0 || mode == 4)
			check(ps_test_page_version_count(0, &key, block) == 1537,
				  "snapshot replacement removes the complete obsolete prefix");
	}
	run_maintenance(64);
	compact_layers = 100;
	check(read_relation_asof(block, 5000, 1511) &&
		  read_relation_asof(block, 10000, (mode == 1 || mode == 5) ? 9000 : 7535) &&
		  (mode != 4 || read_relation_asof(0, 5000, 2000)),
		  "long-chain snapshot and retry preserve old and newest views");
	close_store();
	check(ps_core_open(store) == 0 && read_relation_asof(block, 5000, 1511) &&
		  read_relation_asof(block, 10000, (mode == 1 || mode == 5) ? 9000 : 7535),
		  "long-chain snapshot survives a later restart");
	close_store();
	remove_tree(store);
}

typedef struct BoundedConflictProbe
{
	int called;
	int wrote;
} BoundedConflictProbe;

static void *
bounded_conflict_writer(void *arg)
{
	BoundedConflictProbe *probe = arg;

	probe->wrote = write_relation(0, 3, 4000);
	return NULL;
}

static void
bounded_conflict_hook(void *arg)
{
	BoundedConflictProbe *probe = arg;
	pthread_t thread;

	probe->called++;
	ps_test_set_compaction_unlocked_hook(NULL, NULL);
	if (pthread_create(&thread, NULL, bounded_conflict_writer, probe) == 0)
		(void) pthread_join(thread, NULL);
}

static void
test_bounded_compaction_concurrent_write(void)
{
	char store[] = "/tmp/pagestore-bounded-conflict-XXXXXX";
	BoundedConflictProbe probe = {0};
	uint64_t before;
	uint32_t live = 0;

	configure_core(1);
	compact_layers = 100;
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0 &&
		  write_control(0, 1000, 800), "open bounded conflict store");
	run_maintenance(32);
	check(write_relation(0, 1, 2000) && write_relation(0, 2, 3000),
		  "prepare bounded conflict inputs");
	before = ps_test_bounded_compaction_count();
	ps_test_set_compaction_unlocked_hook(bounded_conflict_hook, &probe);
	compact_layers = 1;
	for (int i = 0; i < 64 && !probe.called; i++)
		(void) ps_core_maintenance();
	ps_test_set_compaction_unlocked_hook(NULL, NULL);
	ps_lock_map_rd();
	for (uint32_t i = 0; i < ps_layer_map.nlayers; i++)
		if (ps_layer_map.layers[i].kind == PS_LAYER_IMAGE &&
			!ps_layer_map.layers[i].deleting && ps_layer_map.layers[i].timeline == 0)
			live++;
	ps_unlock_map();
	check(probe.called && probe.wrote && ps_test_bounded_compaction_count() > before,
		  "writer runs while bounded compaction prepares");
	check(live >= 3 && read_relation_at(1, 2000) && read_relation_at(2, 3000) &&
		  read_relation_at(3, 4000),
		  "concurrent layer invalidates bounded publication and preserves data");
	run_maintenance(64);
	check(read_relation_at(1, 2000) && read_relation_at(2, 3000) &&
		  read_relation_at(3, 4000), "bounded retry preserves concurrent write");
	close_store();
	remove_tree(store);
}

typedef struct FloorPlanConflict
{
	int called;
	int changed;
	int pin_only;
	int remote_transition;
	uint64_t layer_id;
} FloorPlanConflict;

static void *
floor_plan_foreground(void *arg)
{
	FloorPlanConflict *probe = arg;

	if (probe->remote_transition)
	{
		ps_lock_map_wr();
		for (uint32_t i = 0; i < ps_layer_map.nlayers; i++)
			if (ps_layer_map.layers[i].kind == PS_LAYER_IMAGE &&
				!ps_layer_map.layers[i].deleting)
			{
				probe->layer_id = ps_layer_map.layers[i].layer_id;
				ps_layer_map.layers[i].remote_durable = true;
				probe->changed = 1;
				break;
			}
		ps_unlock_map();
		return NULL;
	}
	probe->changed = probe->pin_only ?
		reserve_pin(0, PS_RETENTION_OWNER_READER, 77, 1,
					PS_RETENTION_RESOURCE_PAGE_HISTORY, 4500) :
		write_relation(0, 3, 4000);
	return NULL;
}

static void
floor_plan_conflict_hook(void *arg)
{
	FloorPlanConflict *probe = arg;
	pthread_t thread;

	probe->called++;
	ps_test_set_compaction_unlocked_hook(NULL, NULL);
	if (pthread_create(&thread, NULL, floor_plan_foreground, arg) == 0)
		(void) pthread_join(thread, NULL);
}

static void
prune_retired_conflict_hook(void *arg)
{
	FloorPlanConflict *probe = arg;
	pthread_t thread;

	probe->called++;
	ps_test_set_prune_retired_hook(NULL, NULL);
	if (pthread_create(&thread, NULL, floor_plan_foreground, arg) == 0)
		(void) pthread_join(thread, NULL);
}

/* The hook runs only after the bounded plan and replacement are verified. */
static void
bounded_prune_crash_marker(void *arg)
{
	int fd = *(int *) arg;
	char marker = 'B';

	if (write(fd, &marker, 1) != 1)
		_exit(90);
	ps_test_set_compaction_unlocked_hook(NULL, NULL);
}

static void
test_bounded_prune_crash(const char *fault_name)
{
	char store[] = "/tmp/pagestore-bounded-prune-crash-XXXXXX";
	char control[] = "/tmp/pagestore-bounded-prune-fault-XXXXXX";
	char arm[512], marker = 0;
	FILE *arm_file;
	int pipefd[2], status = 0;
	pid_t child;
	ssize_t marker_bytes;

	configure_core(1);
	segment_gc_enabled = 0;
	compact_layers = 100;
	if (!mkdtemp(store) || !mkdtemp(control) ||
		snprintf(arm, sizeof(arm), "%s/arm", control) >= (int) sizeof(arm) ||
		(arm_file = fopen(arm, "w")) == NULL)
	{
		check(0, "prepare bounded-pruning crash fixture");
		return;
	}
	fclose(arm_file);
	if (ps_core_open(store) != 0)
	{
		check(0, "open bounded-pruning crash store");
		return;
	}
	check(reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
					  PS_RETENTION_RESOURCE_PAGE_HISTORY, 5000) &&
		  write_relation(0, 0, 1000) && write_relation(0, 0, 2000) &&
		  write_relation(0, 2, 3000), "prepare bounded-pruning crash inputs");
	close_store();
	if (pipe(pipefd) != 0)
	{
		check(0, "create bounded-pruning crash marker pipe");
		return;
	}
	child = fork();
	if (child == 0)
	{
		close(pipefd[0]);
		if (setenv("PAGESTORE_TEST_FAULT_NAME", fault_name, 1) != 0 ||
			setenv("PAGESTORE_TEST_FAULT_ACTION", "crash", 1) != 0 ||
			setenv("PAGESTORE_TEST_FAULT_HIT", "1", 1) != 0 ||
			setenv("PAGESTORE_TEST_FAULT_DIR", control, 1) != 0 ||
			ps_fault_init(store) != 0 || ps_core_open(store) != 0)
			_exit(91);
		ps_test_set_compaction_unlocked_hook(bounded_prune_crash_marker, &pipefd[1]);
		compact_layers = 1;
		run_maintenance(64);
		_exit(92);
	}
	close(pipefd[1]);
	if (child < 0 || waitpid(child, &status, 0) != child)
	{
		check(0, "wait for bounded-pruning crash");
		close(pipefd[0]);
		return;
	}
	marker_bytes = read(pipefd[0], &marker, 1);
	close(pipefd[0]);
	check(WIFEXITED(status) && WEXITSTATUS(status) == PS_FAULT_CRASH_EXIT &&
		  marker_bytes == 1 && marker == 'B',
		  "fault crashes after the bounded pruning plan was verified");
	check(ps_core_open(store) == 0 && read_relation_asof(0, 5000, 2000) &&
		  read_relation_asof(2, 5000, 3000),
		  "bounded pruning crash recovers all required pages");
	compact_layers = 1;
	run_maintenance(64);
	check(read_relation_asof(0, 5000, 2000) && read_relation_asof(2, 5000, 3000),
		  "bounded pruning crash remains readable after reclamation retries");
	close_store();
	check(ps_core_open(store) == 0 && read_relation_asof(0, 5000, 2000) &&
		  read_relation_asof(2, 5000, 3000),
		  "bounded pruning recovery and retries survive a second restart");
	close_store();
	remove_tree(store);
	remove_tree(control);
}

static void
test_bounded_prune_interrupted_retirement(void)
{
	char store[] = "/tmp/pagestore-bounded-prune-retirement-XXXXXX";
	FloorPlanConflict probe = {0};
	uint64_t before;
	uint32_t live = 0;

	configure_core(1);
	segment_gc_enabled = 0;
	compact_layers = 100;
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0 &&
		  reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
					  PS_RETENTION_RESOURCE_PAGE_HISTORY, 5000) &&
		  write_relation(0, 0, 1000) && write_relation(0, 0, 2000) &&
		  write_relation(0, 2, 3000), "prepare bounded-pruning retirement conflict");
	before = ps_test_bounded_prune_published_count();
	ps_test_set_prune_retired_hook(prune_retired_conflict_hook, &probe);
	compact_layers = 1;
	for (int i = 0; i < 64 && !probe.called; i++)
		(void) ps_core_maintenance();
	ps_test_set_prune_retired_hook(NULL, NULL);
	ps_lock_map_rd();
	for (uint32_t i = 0; i < ps_layer_map.nlayers; i++)
		if (ps_layer_map.layers[i].kind == PS_LAYER_IMAGE &&
			!ps_layer_map.layers[i].deleting && ps_layer_map.layers[i].timeline == 0)
			live++;
	ps_unlock_map();
	check(probe.called && probe.changed &&
		  ps_test_bounded_prune_published_count() > before && live >= 4 &&
		  ps_test_page_prune_due(0, 0),
		  "writer between source retirements stops the stale retirement prefix");
	run_maintenance(64);
	check(read_relation_asof(0, 5000, 2000) &&
		  read_relation_asof(2, 5000, 3000) && read_relation_asof(3, 5000, 4000),
		  "replanning after partial retirement preserves foreground data");
	close_store();
	check(ps_core_open(store) == 0 && read_relation_asof(0, 5000, 2000) &&
		  read_relation_asof(3, 5000, 4000),
		  "partial retirement recovery preserves pages after restart");
	close_store();
	remove_tree(store);
}

static void
test_bounded_prune_conflict(int mode)
{
	char store[] = "/tmp/pagestore-bounded-prune-conflict-XXXXXX";
	FloorPlanConflict probe = {.pin_only = mode == 1, .remote_transition = mode == 2};
	uint64_t before;
	uint32_t live = 0;

	configure_core(1);
	segment_gc_enabled = 0;
	compact_layers = 100;
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0 &&
		  reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
					  PS_RETENTION_RESOURCE_PAGE_HISTORY, 5000) &&
		  write_relation(0, 0, 1000) && write_relation(0, 0, 2000) &&
		  write_relation(0, 2, 3000), "prepare bounded-pruning conflict");
	before = ps_test_bounded_prune_published_count();
	ps_test_set_compaction_unlocked_hook(floor_plan_conflict_hook, &probe);
	compact_layers = 1;
	for (int i = 0; i < 64 && !probe.called; i++)
		(void) ps_core_maintenance();
	ps_test_set_compaction_unlocked_hook(NULL, NULL);
	ps_lock_map_rd();
	for (uint32_t i = 0; i < ps_layer_map.nlayers; i++)
		if (ps_layer_map.layers[i].kind == PS_LAYER_IMAGE &&
			!ps_layer_map.layers[i].deleting && ps_layer_map.layers[i].timeline == 0)
			live++;
	ps_unlock_map();
	check(probe.called && probe.changed &&
		  ps_test_bounded_prune_published_count() == before &&
		  live >= (uint32_t) (mode ? 3 : 4),
		  "concurrent mutation rejects a staged pruning replacement");
	if (probe.remote_transition)
	{
		ps_lock_map_wr();
		for (uint32_t i = 0; i < ps_layer_map.nlayers; i++)
			if (ps_layer_map.layers[i].layer_id == probe.layer_id)
				ps_layer_map.layers[i].remote_durable = false;
		ps_unlock_map();
	}
	run_maintenance(64);
	check(read_relation_asof(0, 5000, 2000) &&
		  read_relation_asof(2, 5000, 3000) &&
		  (mode != 0 || read_relation_asof(3, 5000, 4000)),
		  "bounded-pruning retry preserves required foreground data");
	close_store();
	remove_tree(store);
}

static void
test_nonzero_bounded_plan_conflict(int mode)
{
	char store[] = "/tmp/pagestore-bounded-floor-conflict-XXXXXX";
	FloorPlanConflict probe = {.pin_only = mode == 1, .remote_transition = mode == 2};
	uint32_t live = 0;

	configure_core(1);
	segment_gc_enabled = 0;
	compact_layers = 100;
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0 &&
		  reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
					  PS_RETENTION_RESOURCE_PAGE_HISTORY, 5000) &&
		  write_relation(0, 0, 1000) && write_relation(0, 1, 2000) &&
		  write_relation(0, 2, 3000),
		  "prepare nonzero-floor conflict sources");
	ps_test_set_compaction_unlocked_hook(floor_plan_conflict_hook, &probe);
	compact_layers = 1;
	for (int i = 0; i < 64 && !probe.called; i++)
		(void) ps_core_maintenance();
	ps_test_set_compaction_unlocked_hook(NULL, NULL);
	ps_lock_map_rd();
	for (uint32_t i = 0; i < ps_layer_map.nlayers; i++)
		if (ps_layer_map.layers[i].kind == PS_LAYER_IMAGE &&
			!ps_layer_map.layers[i].deleting && ps_layer_map.layers[i].timeline == 0)
			live++;
	ps_unlock_map();
	check(probe.called && probe.changed && live >= (uint32_t) (mode ? 3 : 4),
		  "concurrent change rejects the nonzero-floor staged plan");
	if (probe.remote_transition)
	{
		ps_lock_map_wr();
		for (uint32_t i = 0; i < ps_layer_map.nlayers; i++)
			if (ps_layer_map.layers[i].layer_id == probe.layer_id)
				ps_layer_map.layers[i].remote_durable = false;
		ps_unlock_map();
	}
	run_maintenance(64);
	check(read_relation_asof(0, 5000, 1000) &&
		  read_relation_asof(1, 5000, 2000) &&
		  read_relation_asof(2, 5000, 3000) &&
		  (mode != 0 || read_relation_asof(3, 5000, 4000)),
		  "nonzero-floor retry preserves foreground data");
	close_store();
	remove_tree(store);
}

typedef struct StagedCompactionProbe
{
	int called;
	int read_ok;
	int write_ok;
	int pin_only;
	int remote_transition;
	int hold_reader;
	int max_calls;
	uint64_t remote_layer_id;
	PsLayerDesc pinned_layer;
	uint32_t layers_after_write;
} StagedCompactionProbe;

static void staged_compaction_concurrent_write(void *arg);

static void *
staged_compaction_foreground(void *arg)
{
	StagedCompactionProbe *probe = arg;
	uint64_t version = 0;

	probe->read_ok = read_control_at(0, PS_CONTROL_IMAGE_BLOCK, 2000, &version) &&
		version == 2000;
	if (probe->hold_reader)
	{
		/* Keep a concurrent reader's layer pin until validation finishes. */
		ps_lock_map_rd();
		for (uint32_t i = 0; i < ps_layer_map.nlayers; i++)
			if (ps_layer_map.layers[i].kind == PS_LAYER_IMAGE &&
				!ps_layer_map.layers[i].deleting)
			{
				probe->remote_layer_id = ps_layer_map.layers[i].layer_id;
				probe->pinned_layer = ps_layer_map.layers[i];
				__atomic_add_fetch(&ps_layer_map.layers[i].cache_readers, 1,
								   __ATOMIC_ACQ_REL);
				probe->write_ok = 1;
				break;
			}
		ps_unlock_map();
	}
	else if (probe->remote_transition)
	{
		/* Model the in-memory publication performed by the upload worker.
		 * The test restores the flag before closing the fixture. */
		ps_lock_map_wr();
		for (uint32_t i = 0; i < ps_layer_map.nlayers; i++)
			if (ps_layer_map.layers[i].kind == PS_LAYER_IMAGE &&
				!ps_layer_map.layers[i].deleting)
			{
				probe->remote_layer_id = ps_layer_map.layers[i].layer_id;
				ps_layer_map.layers[i].remote_durable = true;
				probe->write_ok = 1;
				break;
			}
		ps_unlock_map();
	}
	else if (probe->pin_only)
		probe->write_ok = reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
									 PS_RETENTION_RESOURCE_ALL, 2000);
	else
		probe->write_ok = write_relation(0, 1, 3000);
	ps_lock_map_rd();
	probe->layers_after_write = ps_layer_map_count(&ps_layer_map);
	ps_unlock_map();
	return NULL;
}

static void
test_staged_compaction_rejects_remote_transition(void)
{
	char store[] = "/tmp/pagestore-staged-remote-XXXXXX";
	StagedCompactionProbe probe = {.remote_transition = 1};
	int restored = 0;

	configure_core(1);
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0,
		  "open staged remote transition store");
	check(write_control(0, 1000, 800) && write_control(0, 2000, 1800),
		  "prepare staged remote transition sources");
	ps_test_set_compaction_unlocked_hook(staged_compaction_concurrent_write, &probe);
	for (int i = 0; i < 64 && !probe.called; i++)
		(void) ps_core_maintenance();
	ps_test_set_compaction_unlocked_hook(NULL, NULL);
	ps_lock_map_wr();
	check(probe.called == 1 && probe.write_ok &&
		  ps_layer_map_count(&ps_layer_map) == probe.layers_after_write,
		  "remote durability transition invalidates staged replacement");
	for (uint32_t i = 0; i < ps_layer_map.nlayers; i++)
		if (ps_layer_map.layers[i].layer_id == probe.remote_layer_id)
		{
			ps_layer_map.layers[i].remote_durable = false;
			restored = 1;
			break;
		}
	ps_unlock_map();
	check(restored, "remote transition leaves the original layer live");
	run_maintenance(64);
	close_store();
	remove_tree(store);
}

static void
test_staged_compaction_preserves_pinned_reader(void)
{
	char store[] = "/tmp/pagestore-staged-reader-XXXXXX";
	StagedCompactionProbe probe = {.hold_reader = 1};
	int released = 0;
	int marked = 0;
	PsKey key = control_key();
	unsigned char page[8192];
	uint64_t found_lsn = 0, found_seq = 0;

	configure_core(1);
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0,
		  "open staged reader store");
	check(write_control(0, 1000, 800) && write_control(0, 2000, 1800),
		  "prepare staged reader sources");
	ps_test_set_compaction_unlocked_hook(staged_compaction_concurrent_write, &probe);
	for (int i = 0; i < 64 && !probe.called; i++)
		(void) ps_core_maintenance();
	ps_test_set_compaction_unlocked_hook(NULL, NULL);
	ps_lock_map_rd();
	for (uint32_t i = 0; i < ps_layer_map.nlayers; i++)
		if (ps_layer_map.layers[i].layer_id == probe.remote_layer_id)
			marked = ps_layer_map.layers[i].deleting;
	ps_unlock_map();
	check(probe.called == 1 && probe.write_ok && marked,
		  "compaction publishes while retaining a pinned source");
	run_maintenance(32);
	check(ps_layer_store->layer_exists_local(probe.remote_layer_id) == 1,
		  "background GC leaves the pinned source on disk");
	check(ps_image_layer_lookup(&probe.pinned_layer, &key,
			PS_REDO_NOTE_BLOCK, UINT64_MAX, 0, page, page_size,
			&found_lsn, &found_seq) == 1,
		  "pinned source remains readable during GC");
	ps_lock_map_rd();
	for (uint32_t i = 0; i < ps_layer_map.nlayers; i++)
		if (ps_layer_map.layers[i].layer_id == probe.remote_layer_id)
		{
			__atomic_sub_fetch(&ps_layer_map.layers[i].cache_readers, 1,
								   __ATOMIC_ACQ_REL);
			released = 1;
			break;
		}
	ps_unlock_map();
	check(released, "concurrent reader can release the original layer");
	run_maintenance(64);
	check(ps_layer_store->layer_exists_local(probe.remote_layer_id) == 0,
		  "background GC reclaims the source after the reader releases it");
	close_store();
	remove_tree(store);
}

static void
staged_compaction_concurrent_write(void *arg)
{
	StagedCompactionProbe *probe = arg;
	pthread_t thread;

	probe->called++;
	if (probe->called >= (probe->max_calls ? probe->max_calls : 1))
		ps_test_set_compaction_unlocked_hook(NULL, NULL);
	if (pthread_create(&thread, NULL, staged_compaction_foreground, arg) == 0)
		(void) pthread_join(thread, NULL);
}

static void
gather_compaction_concurrent_change(void *arg)
{
	StagedCompactionProbe *probe = arg;
	pthread_t thread;

	probe->called++;
	ps_test_set_compaction_gather_hook(NULL, NULL);
	if (pthread_create(&thread, NULL, staged_compaction_foreground, arg) == 0)
		(void) pthread_join(thread, NULL);
}

static void
test_compaction_gather_rejects_concurrent_change(int pin_only)
{
	char store[] = "/tmp/pagestore-gather-conflict-XXXXXX";
	StagedCompactionProbe probe = {.pin_only = pin_only};
	uint64_t version = 0;

	configure_core(1);
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0,
		  "open compaction gather conflict store");
	check(write_control(0, 1000, 800) && write_control(0, 2000, 1800),
		  "prepare compaction gather sources");
	ps_test_set_compaction_gather_hook(gather_compaction_concurrent_change, &probe);
	for (int i = 0; i < 64 && !probe.called; i++)
		(void) ps_core_maintenance();
	ps_test_set_compaction_gather_hook(NULL, NULL);
	check(probe.called == 1 && probe.read_ok && probe.write_ok,
		  "foreground operation proceeds after unlocked gathering and sorting");
	ps_lock_map_rd();
	check(ps_layer_map_count(&ps_layer_map) == probe.layers_after_write,
		  "changed snapshot rejects gathered compaction inputs");
	ps_unlock_map();
	run_maintenance(64);
	check(read_control_at(0, PS_CONTROL_IMAGE_BLOCK, 2000, &version) && version == 2000,
		  "gather retry preserves the control image");
	if (!pin_only)
		check(read_relation_at(1, 3000), "gather retry preserves the concurrent write");
	close_store();
	remove_tree(store);
}

static void
test_staged_compaction_makes_progress_after_conflicts(void)
{
	char store[] = "/tmp/pagestore-staged-retry-XXXXXX";
	StagedCompactionProbe probe = {.max_calls = 1};
	uint64_t version = 0;
	uint32_t layers;

	configure_core(1);
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0,
		  "open staged compaction retry store");
	check(write_control(0, 1000, 800) && write_control(0, 2000, 1800),
		  "prepare sources for staged retry");
	ps_test_set_compaction_unlocked_hook(staged_compaction_concurrent_write, &probe);
	run_maintenance(64);
	ps_test_set_compaction_unlocked_hook(NULL, NULL);
	ps_lock_map_rd();
	layers = ps_layer_map_count(&ps_layer_map);
	ps_unlock_map();
	check(probe.called == 1 && probe.write_ok && layers < probe.layers_after_write,
		  "compaction makes progress after repeated conflicts");
	check(read_control_at(0, PS_CONTROL_IMAGE_BLOCK, 2000, &version) && version == 2000,
		  "fallback preserves control image after conflicts");
	close_store();
	remove_tree(store);
}

static void
test_staged_compaction_rejects_concurrent_write(uint32_t nshards)
{
	char store[] = "/tmp/pagestore-staged-compaction-XXXXXX";
	StagedCompactionProbe probe = {0};
	uint64_t version = 0;

	configure_core(1);
	ps_nshards = nshards;
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0,
		  "open staged compaction store");
	check(write_control(0, 1000, 800) && write_control(0, 2000, 1800),
		  "prepare staged compaction sources");
	ps_test_set_compaction_unlocked_hook(staged_compaction_concurrent_write, &probe);
	for (int i = 0; i < 64 && !probe.called; i++)
		(void) ps_core_maintenance();
	ps_test_set_compaction_unlocked_hook(NULL, NULL);
	check(probe.called == 1 && probe.read_ok && probe.write_ok,
		  "foreground reads and writes proceed during staged output");
	ps_lock_map_rd();
	check(ps_layer_map_count(&ps_layer_map) == probe.layers_after_write,
		  "concurrent write invalidates staged replacement");
	ps_unlock_map();
	run_maintenance(64);
	check(read_control_at(0, PS_CONTROL_IMAGE_BLOCK, 2000, &version) && version == 2000,
		  "retry preserves control image after concurrent write");
	close_store();
	remove_tree(store);
}

static void
test_staged_compaction_rejects_new_retention_pin(void)
{
	char store[] = "/tmp/pagestore-staged-pin-XXXXXX";
	StagedCompactionProbe probe = {.pin_only = 1};
	uint64_t version = 0;

	configure_core(1);
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0,
		  "open staged retention store");
	check(write_control(0, 1000, 800) && write_control(0, 2000, 1800),
		  "prepare staged retention sources");
	ps_test_set_compaction_unlocked_hook(staged_compaction_concurrent_write, &probe);
	for (int i = 0; i < 64 && !probe.called; i++)
		(void) ps_core_maintenance();
	ps_test_set_compaction_unlocked_hook(NULL, NULL);
	check(probe.called == 1 && probe.read_ok && probe.write_ok,
		  "retention pin can be installed during staged output");
	ps_lock_map_rd();
	check(ps_layer_map_count(&ps_layer_map) == probe.layers_after_write,
		  "new retention pin invalidates staged replacement");
	ps_unlock_map();
	run_maintenance(64);
	check(read_control_at(0, PS_CONTROL_IMAGE_BLOCK, 2000, &version) && version == 2000,
		  "retry preserves the pinned control image");
	close_store();
	remove_tree(store);
}

/* Three checkpoints; the materializer floor above all of them keeps only the
 * newest pair, and the WAL floor follows its redo. */
static void
test_floor_retires_older_checkpoints(void)
{
	char store[] = "/tmp/pagestore-control-prune-floor-XXXXXX";

	configure_core(1);
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0,
		  "open store for floor test");
	check(write_relation(0, 0, 900) && write_control(0, 1000, 800) &&
		  write_relation(0, 0, 1900) && write_control(0, 2000, 1800) &&
		  write_relation(0, 0, 2900) && write_control(0, 3000, 2800),
		  "write three checkpoints with relation churn between them");
	check(wal_floor(0) == 800, "WAL floor is the oldest checkpoint redo before pruning");
	check(reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
					  PS_RETENTION_RESOURCE_ALL, 3500),
		  "materializer publishes a cutoff above every checkpoint");
	run_maintenance(64);
	check(wal_floor(0) == 2800,
		  "WAL floor advances to the newest checkpoint at or below the cutoff");
	{
		uint64_t version = 0;

		check(read_control_at(0, 0, 3500, &version) && version == 3000,
			  "newest control image still restores at the cutoff");
		check(read_control_at(0, 1, 3500, &version) && version == 2800,
			  "its same-version note still resolves");
	}
	close_store();

	/* The surviving pair must be exactly what recovery rebuilds. */
	configure_core(1);
	check(ps_core_open(store) == 0, "reopen store after pruning");
	check(wal_floor(0) == 2800, "WAL floor survives restart after pruning");
	close_store();
	remove_tree(store);
}

/*
 * The redo note carries the backend's identity trailer after its value; a
 * legacy note has none and still counts, a note naming another format or
 * version makes the floor unknown -- fail closed -- whatever its first eight
 * bytes say.
 */
static void
test_note_identity_trailer(void)
{
	char store[] = "/tmp/pagestore-control-prune-trailer-XXXXXX";

	configure_core(1);
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0,
		  "open store for the note trailer test");
	check(write_relation(0, 0, 900) && write_note(0, 1000, 800),
		  "a legacy note without a trailer");
	check(wal_floor(0) == 800, "a legacy note is a floor source");
	check(write_relation(0, 0, 1900) && write_control(0, 2000, 1800),
		  "a checkpoint pair with a stamped note");
	check(wal_floor(0) == 800, "the stamped note joins the legacy one; the floor is the oldest");
	close_store();

	configure_core(1);
	check(ps_core_open(store) == 0, "reopen store");
	check(wal_floor(0) == 800, "both notes survive a restart");
	check(write_relation(0, 0, 2900) && write_foreign_note(0, 3000, 100),
		  "a note whose trailer names another format, at a lower redo");
	check(wal_floor(0) == UINT64_MAX,
		  "a note naming another format makes the floor unknown, so nothing is reclaimed on its word");
	close_store();
	remove_tree(store);
}

/* A reader pinned between two checkpoints keeps the older pair it restores. */
static void
test_reader_pin_retains_its_checkpoint(void)
{
	char store[] = "/tmp/pagestore-control-prune-reader-XXXXXX";

	configure_core(1);
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0,
		  "open store for reader test");
	check(write_relation(0, 0, 900) && write_control(0, 1000, 800) &&
		  write_relation(0, 0, 1900) && write_control(0, 2000, 1800) &&
		  write_relation(0, 0, 2900) && write_control(0, 3000, 2800),
		  "write three checkpoints for the reader test");
	check(reserve_pin(0, PS_RETENTION_OWNER_READER, 7, 1,
					  PS_RETENTION_RESOURCE_ALL, 2500),
		  "reader pins between the second and third checkpoint");
	check(reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
					  PS_RETENTION_RESOURCE_ALL, 3500),
		  "materializer cutoff above every checkpoint");
	run_maintenance(64);
	check(wal_floor(0) == 1800,
		  "WAL floor stops at the checkpoint the pinned reader restores");
	{
		uint64_t version = 0;

		check(read_control_at(0, 0, 2500, &version) && version == 2000,
			  "reader still restores the second control image");
		check(read_control_at(0, 1, 2500, &version) && version == 1800,
			  "reader still resolves the second note");
	}
	check(drop_pin(0, PS_RETENTION_OWNER_READER, 7, 1), "reader drops its pin");
	run_maintenance(64);
	check(wal_floor(0) == 2800, "WAL floor advances once the reader leaves");
	close_store();
	remove_tree(store);
}

/* A live branch cap is a structural fence for the pair it forked from. */
static void
test_branch_cap_retains_its_checkpoint(void)
{
	char store[] = "/tmp/pagestore-control-prune-branch-XXXXXX";

	configure_core(1);
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0,
		  "open store for branch test");
	check(write_relation(0, 0, 900) && write_control(0, 1000, 800) &&
		  write_relation(0, 0, 1900) && write_control(0, 2000, 1800) &&
		  create_branch(1, 0, 2200) &&
		  write_relation(0, 0, 2900) && write_control(0, 3000, 2800),
		  "branch between the second and third checkpoint");
	check(reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
					  PS_RETENTION_RESOURCE_ALL, 3500),
		  "materializer cutoff above every checkpoint");
	run_maintenance(64);
	check(wal_floor(0) == 1800,
		  "WAL floor stops at the checkpoint the live branch restores");
	check(wal_floor(1) == 1800, "the branch itself reports the same floor");
	{
		uint64_t version = 0;

		check(read_control_at(1, 0, 2200, &version) && version == 2000,
			  "branch restores its fork-point control image through read-through");
	}
	close_store();
	remove_tree(store);
}

/* A flush boundary between the note and its image must not strand an
 * unnoted image: both blocks are planned over their complete chains. */
static void
test_split_pair_is_never_unnoted(void)
{
	char store[] = "/tmp/pagestore-control-prune-split-XXXXXX";
	PsKey key = control_key();
	unsigned char page[8192];

	/* flush_pages=2: the note of checkpoint 3 completes a flush window while
	 * its image stays in the page log. */
	configure_core(0);
	flush_pages = 2;
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0,
		  "open store for split-pair test");
	check(write_control(0, 1000, 800) && write_control(0, 2000, 1800),
		  "two complete checkpoints flushed into layers");
	ps_lock_shard_wr(ps_shard_of(&key));
	memset(page, 0, sizeof(page));
	{
		uint64_t redo = 2800;

		memcpy(page, &redo, sizeof(redo));
	}
	check(append_page(0, &key, 1, page, 3000, NULL) == 0,
		  "third note lands in the flushed window");
	ps_unlock_shard(ps_shard_of(&key));
	check(reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
					  PS_RETENTION_RESOURCE_ALL, 3500),
		  "materializer cutoff above the split checkpoint");
	run_maintenance(64);
	/* Note 3000 exists without image 3000 yet; the newest complete pair at or
	 * below the cutoff for the image block is 2000, so its note must survive
	 * and the floor must stay provable. */
	check(wal_floor(0) == 1800,
		  "floor stays provable while the third image is still pending");
	ps_lock_shard_wr(ps_shard_of(&key));
	memset(page, 0xC3, sizeof(page));
	check(append_page(0, &key, 0, page, 3000, NULL) == 0,
		  "third image completes the pair");
	ps_unlock_shard(ps_shard_of(&key));
	check(ps_storage->sync() == 0, "sync the completed pair");
	run_maintenance(64);
	check(wal_floor(0) == 2800, "floor advances once the pair is complete");
	close_store();
	remove_tree(store);
}

/* A mirror retry appends the same control bytes again under a new admission
 * sequence.  Only the newest copy of a retained version survives. */
static void
test_retry_copies_collapse(void)
{
	char store[] = "/tmp/pagestore-control-prune-retry-XXXXXX";
	PsKey key = control_key();

	configure_core(1);
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0,
		  "open store for retry test");
	check(write_relation(0, 0, 900) && write_control(0, 1000, 800) &&
		  write_control(0, 1000, 800) && write_control(0, 1000, 800) &&
		  write_relation(0, 0, 1900) && write_control(0, 2000, 1800) &&
		  write_control(0, 2000, 1800),
		  "write checkpoints with retried control appends");
	check(ps_test_page_version_count(0, &key, 0) == 5 &&
		  ps_test_page_version_count(0, &key, 1) == 5,
		  "retries leave one physical copy per append before compaction");
	check(reserve_pin(0, PS_RETENTION_OWNER_READER, 9, 1,
					  PS_RETENTION_RESOURCE_ALL, 1500) &&
		  reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
					  PS_RETENTION_RESOURCE_ALL, 2500),
		  "reader below and materializer above the second checkpoint");
	run_maintenance(64);
	check(ps_test_page_version_count(0, &key, 0) == 4 &&
		  ps_test_page_version_count(0, &key, 1) == 4,
		  "finite views keep the first and selected copy per retained position");
	check(wal_floor(0) == 800, "the reader's checkpoint keeps its floor");
	{
		uint64_t	version = 0;

		check(read_control_at(0, 0, 1500, &version) && version == 1000 &&
			  read_control_at(0, 1, 1500, &version) && version == 800,
			  "the reader still restores its pair after collapsing retries");
	}
	close_store();
	remove_tree(store);
}

/* A mirror that keeps timing out after its note but before its image
 * appends the same note again under a new sequence each time.  Compaction
 * keeps exactly one copy of that in-flight note while waiting for the image,
 * so repeated failures cannot grow the control layers without bound. */
static void
test_in_flight_note_retries_collapse(void)
{
	char store[] = "/tmp/pagestore-control-prune-inflight-XXXXXX";
	PsKey key = control_key();

	configure_core(1);
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0,
		  "open store for the in-flight note test");
	check(write_relation(0, 0, 900) && write_control(0, 1000, 800) &&
		  write_relation(0, 0, 1900) && write_control(0, 2000, 1800) &&
		  write_note(0, 3000, 2800) && write_note(0, 3000, 2800) &&
		  write_note(0, 3000, 2800),
		  "two checkpoints, then a note retried three times without its image");
	check(ps_test_page_version_count(0, &key, 0) == 2 &&
		  ps_test_page_version_count(0, &key, 1) == 5,
		  "every retried note copy exists before compaction");
	check(reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
					  PS_RETENTION_RESOURCE_ALL, 2500),
		  "materializer cutoff above the second checkpoint");
	run_maintenance(64);
	check(ps_test_page_version_count(0, &key, 1) == 2,
		  "one copy of the in-flight note survives with the retained pair's note");
	{
		uint64_t version = 0;

		check(read_control_at(0, 1, 3000, &version) && version == 2800,
			  "the surviving in-flight note is still readable");
	}
	check(write_control(0, 3000, 2800) &&
		  reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
					  PS_RETENTION_RESOURCE_ALL, 3500),
		  "the image finally arrives and the cutoff moves past it");
	run_maintenance(64);
	{
		uint64_t version = 0;

		check(read_control_at(0, 0, 3000, &version) && version == 3000 &&
			  read_control_at(0, 1, 3000, &version) && version == 2800,
			  "the completed pair restores at its checkpoint");
		check(wal_floor(0) == 2800, "the WAL floor follows the completed pair");
	}
	close_store();
	remove_tree(store);
}

/* A WAL-only pin fences control images.  Dropping it must schedule the
 * control layers for pruning even when no layer-count threshold is crossed. */
static void
test_wal_only_pin_release_reschedules(void)
{
	char store[] = "/tmp/pagestore-control-prune-walpin-XXXXXX";

	configure_core(1);
	compact_layers = 1;
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0,
		  "open store for WAL-only pin test");
	check(write_relation(0, 0, 900) && write_control(0, 1000, 800) &&
		  write_relation(0, 0, 1900) && write_control(0, 2000, 1800) &&
		  write_relation(0, 0, 2900) && write_control(0, 3000, 2800),
		  "write three checkpoints for the WAL-only pin test");
	check(reserve_pin(0, PS_RETENTION_OWNER_READER, 11, 1,
					  PS_RETENTION_RESOURCE_WAL, 2500) &&
		  reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
					  PS_RETENTION_RESOURCE_ALL, 3500),
		  "WAL-only reader below the third checkpoint");
	run_maintenance(64);
	check(wal_floor(0) == 1800,
		  "WAL-only pin keeps the checkpoint it restores");
	check(drop_pin(0, PS_RETENTION_OWNER_READER, 11, 1),
		  "WAL-only reader drops its pin");
	run_maintenance(64);
	check(wal_floor(0) == 2800,
		  "dropping the WAL-only pin reschedules control pruning");
	close_store();
	remove_tree(store);
}

/* An SLRU seed shipped at an older cutoff keeps the control image it
 * resolves its era from, across compaction and restart, while newer
 * unreferenced checkpoints are still retired. */
static void
test_slru_seed_keeps_its_control_image(void)
{
	char store[] = "/tmp/pagestore-control-prune-seed-XXXXXX";
	PsKey seed = {0, 0, 7, 0, PS_KLASS_SLRU};
	unsigned char page[8192];

	configure_core(1);
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0,
		  "open store for the SLRU seed test");
	check(write_relation(0, 0, 900) && write_control(0, 1000, 800) &&
		  write_relation(0, 0, 1900) && write_control(0, 2000, 1800) &&
		  write_relation(0, 0, 2900) && write_control(0, 3000, 2800),
		  "write three checkpoints before the seed");
	/* The consumer that captures a seed pins its cutoff first; the seed
	 * itself is retained by that pin and fences the control image. */
	check(reserve_pin(0, PS_RETENTION_OWNER_READER, 7, 1,
					  PS_RETENTION_RESOURCE_ALL, 2000),
		  "the seed's consumer pins the second checkpoint");
	memset(page, 0x77, sizeof(page));
	ps_lock_shard_wr(ps_shard_of(&seed));
	check(append_page(0, &seed, 0, page, 2000, NULL) == 0,
		  "ship an SLRU seed snapshot at the second checkpoint");
	ps_unlock_shard(ps_shard_of(&seed));
	check(ps_storage->sync() == 0, "sync the seed");
	check(reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
					  PS_RETENTION_RESOURCE_ALL, 3500),
		  "materializer cutoff above every checkpoint");
	run_maintenance(64);
	{
		uint64_t version = 0;

		check(read_control_at(0, 0, 2000, &version) && version == 2000,
			  "the seed's control image survives compaction");
		check(wal_floor(0) == 1800,
			  "the WAL floor is bounded by the seed's checkpoint, not older ones");
	}
	close_store();
	configure_core(1);
	check(ps_core_open(store) == 0, "reopen the seeded store");
	check(reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
					  PS_RETENTION_RESOURCE_ALL, 3600),
		  "advance the cutoff after restart");
	run_maintenance(64);
	{
		uint64_t version = 0;

		check(read_control_at(0, 0, 2000, &version) && version == 2000,
			  "recovery rebuilds the seed fence before pruning again");
	}
	close_store();
	remove_tree(store);
}

/* An artifact shipped at a cutoff that page compaction already passed, with
 * no fence at that cutoff, is refused: the control image it would resolve its
 * era from is gone, so registering it would only pin a dead era.  At or above
 * the frontier it is accepted as before. */
static void
test_late_artifact_below_frontier_is_refused(void)
{
	char store[] = "/tmp/pagestore-control-prune-late-XXXXXX";
	PsKey seed = {0, 0, 7, 0, PS_KLASS_SLRU};
	PsKey snapshot = {0, 0, 9, 0, PS_KLASS_READER_SNAPSHOT};
	unsigned char page[8192];
	int rc_below, rc_snapshot_below, rc_at;

	configure_core(1);
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0,
		  "open store for the late artifact test");
	check(write_relation(0, 0, 900) && write_control(0, 1000, 800) &&
		  write_relation(0, 0, 1900) && write_control(0, 2000, 1800) &&
		  write_relation(0, 0, 2900) && write_control(0, 3000, 2800),
		  "write three checkpoints");
	check(reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
					  PS_RETENTION_RESOURCE_ALL, 3500),
		  "materializer cutoff above every checkpoint");
	run_maintenance(64);
	{
		uint64_t version = 0;

		check(!read_control_at(0, 0, 2000, &version),
			  "the second checkpoint's control image was retired");
	}
	memset(page, 0x77, sizeof(page));
	ps_lock_shard_wr(ps_shard_of(&seed));
	rc_below = append_page(0, &seed, 0, page, 2000, NULL);
	ps_unlock_shard(ps_shard_of(&seed));
	ps_lock_shard_wr(ps_shard_of(&snapshot));
	rc_snapshot_below = append_page(0, &snapshot, 0, page, 2000, NULL);
	ps_unlock_shard(ps_shard_of(&snapshot));
	check(rc_below != 0 && rc_snapshot_below != 0,
		  "an SLRU seed or reader snapshot at the retired cutoff is refused");
	check(ps_test_artifact_fence_count(0) == 0,
		  "a refused artifact registers no fence");
	ps_lock_shard_wr(ps_shard_of(&seed));
	rc_at = append_page(0, &seed, 0, page, 3500, NULL);
	ps_unlock_shard(ps_shard_of(&seed));
	check(rc_at == 0 && ps_storage->sync() == 0,
		  "an SLRU seed at the cutoff itself is accepted");
	close_store();
	remove_tree(store);
}

/* An artifact admitted below the frontier reserves its fence before the
 * bytes are written.  If the append then fails, the reservation is released
 * and no fence remains, so an abandoned cutoff does not pin a control era
 * until restart; a later successful append at the same cutoff fences it. */
static void
test_failed_artifact_append_releases_its_fence(void)
{
	char store[] = "/tmp/pagestore-control-prune-release-XXXXXX";
	PsKey seed = {0, 0, 7, 0, PS_KLASS_SLRU};
	unsigned char page[8192];
	int rc;

	configure_core(1);
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0,
		  "open store for the released fence test");
	check(write_relation(0, 0, 900) && write_control(0, 1000, 800) &&
		  write_relation(0, 0, 1900) && write_control(0, 2000, 1800) &&
		  write_relation(0, 0, 2900) && write_control(0, 3000, 2800),
		  "write three checkpoints for the released fence test");
	check(reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
					  PS_RETENTION_RESOURCE_ALL, 3500),
		  "materializer cutoff above every checkpoint for the released fence test");
	run_maintenance(64);
	close_store();
	/* the next segment write fails: the seed is admitted but never lands */
	check(setenv("PAGESTORE_TEST_FAIL_SEG_WRITES", "1", 1) == 0,
		  "arm one failing segment write");
	configure_core(1);
	check(ps_core_open(store) == 0, "reopen with the failing write armed");
	memset(page, 0x77, sizeof(page));
	ps_lock_shard_wr(ps_shard_of(&seed));
	rc = append_page(0, &seed, 0, page, 3500, NULL);
	ps_unlock_shard(ps_shard_of(&seed));
	check(rc != 0, "the admitted seed append fails at the segment write");
	check(ps_test_artifact_fence_count(0) == 0,
		  "a failed artifact append leaves no fence behind");
	check(unsetenv("PAGESTORE_TEST_FAIL_SEG_WRITES") == 0,
		  "disarm the failing segment write");
	ps_lock_shard_wr(ps_shard_of(&seed));
	rc = append_page(0, &seed, 0, page, 3500, NULL);
	ps_unlock_shard(ps_shard_of(&seed));
	check(rc == 0 && ps_storage->sync() == 0 &&
		  ps_test_artifact_fence_count(0) == 1,
		  "the retried seed append lands and fences its cutoff");
	close_store();
	remove_tree(store);
}

/* A materializer pins WAL and the WAL index but no page history, at the redo
 * of its last durable restartpoint.  That pin is the operational page-history
 * cutoff: relation history and control checkpoints below it are retired with
 * no page-history owner at all, the newest checkpoint keeps its exact-redo
 * twin, later pins are refused below the frontier, and the cutoff survives a
 * restart. */
static void
test_materializer_pin_is_the_page_cutoff(void)
{
	char store[] = "/tmp/pagestore-control-prune-marker-XXXXXX";
	PsKey rel = {1, 1, 1, 0, PS_KLASS_RELATION};
	uint64_t version = 0;

	configure_core(1);
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0,
		  "open store for the materializer cutoff test");
	check(reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
					  PS_RETENTION_RESOURCE_WAL |
					  PS_RETENTION_RESOURCE_WAL_INDEX, 500),
		  "the materializer registers at its first restart redo");
	check(write_relation(0, 0, 900) && write_checkpoint(0, 800, 1000) &&
		  write_relation(0, 0, 1900) && write_checkpoint(0, 1800, 2000) &&
		  write_relation(0, 0, 2900) && write_checkpoint(0, 2800, 3000),
		  "three checkpoints with exact-redo twins and relation history");
	run_maintenance(16);
	check(ps_test_page_version_count(0, &rel, 0) == 3 &&
		  read_control_at(0, 0, 1000, &version) && version == 1000,
		  "nothing is pruned while the materializer's restart redo is below it");
	check(reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
					  PS_RETENTION_RESOURCE_WAL |
					  PS_RETENTION_RESOURCE_WAL_INDEX, 3400) &&
		  write_marker(0, 3450),
		  "the materializer advances its pin to a newer restart redo");
	run_maintenance(64);
	check(ps_test_page_version_count(0, &rel, 0) == 1,
		  "relation history below the pin is retired without a page owner");
	check(read_control_at(0, 0, 3000, &version) && version == 3000 &&
		  read_control_at(0, 0, 2800, &version) && version == 2800,
		  "the newest checkpoint keeps both its update image and its exact-redo twin");
	check(!read_control_at(0, 0, 2000, &version),
		  "older checkpoints are retired");
	check(wal_floor(0) == 2800, "the WAL floor follows the retained checkpoint");
	check(!reserve_pin(0, PS_RETENTION_OWNER_READER, 9, 1,
					   PS_RETENTION_RESOURCE_ALL, 1500),
		  "a page-history pin below the derived frontier is refused");
	check(reserve_pin(0, PS_RETENTION_OWNER_READER, 9, 2,
					  PS_RETENTION_RESOURCE_ALL, 3450),
		  "a page-history pin above the derived frontier is admitted");
	close_store();
	configure_core(1);
	check(ps_core_open(store) == 0, "reopen the store");
	run_maintenance(16);
	check(read_control_at(0, 0, 2800, &version) && version == 2800 &&
		  !read_control_at(0, 0, 2000, &version) && wal_floor(0) == 2800,
		  "the derived cutoff and the retained twin survive restart");
	close_store();
	remove_tree(store);
}

/* A direct-write compute pins nothing: the redo of its newest durable
 * checkpoint note is the cutoff, and a writer's notes are ignored while a
 * materializer owns the timeline. */
static void
test_checkpoint_note_is_the_page_cutoff(void)
{
	char store[] = "/tmp/pagestore-control-prune-note-XXXXXX";
	PsKey rel = {1, 1, 1, 0, PS_KLASS_RELATION};
	uint64_t version = 0;

	configure_core(1);
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0,
		  "open store for the note cutoff test");
	check(write_relation(0, 0, 900) && write_checkpoint(0, 800, 1000) &&
		  write_relation(0, 0, 1900) && write_checkpoint(0, 1800, 2000) &&
		  write_relation(0, 0, 2900) && write_checkpoint(0, 2800, 3000),
		  "three checkpoints from a direct-write compute");
	run_maintenance(64);
	check(ps_test_page_version_count(0, &rel, 0) == 2,
		  "history below the newest checkpoint redo is retired, the rest kept");
	check(read_control_at(0, 0, 2800, &version) && version == 2800 &&
		  read_control_at(0, 0, 3000, &version) && version == 3000 &&
		  !read_control_at(0, 0, 2000, &version),
		  "the newest checkpoint and its twin survive, older ones are retired");
	check(wal_floor(0) == 2800, "the WAL floor follows the newest checkpoint");
	close_store();
	remove_tree(store);

	/* With a materializer owner present, the writer's newer checkpoint notes
	 * do not move the cutoff ahead of materialization. */
	configure_core(1);
	strcpy(store, "/tmp/pagestore-control-prune-note-XXXXXX");
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0,
		  "open store for the materializer-owned note test");
	check(write_relation(0, 0, 900) && write_checkpoint(0, 800, 1000) &&
		  write_relation(0, 0, 1900) && write_checkpoint(0, 1800, 2000) &&
		  reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
					  PS_RETENTION_RESOURCE_WAL |
					  PS_RETENTION_RESOURCE_WAL_INDEX, 1500),
		  "a materializer owns the timeline while the writer checkpoints ahead");
	run_maintenance(64);
	check(ps_test_page_version_count(0, &rel, 0) == 2 &&
		  read_control_at(0, 0, 1000, &version) && version == 1000,
		  "writer checkpoint notes establish no cutoff on a materializer-fed timeline");
	close_store();
	remove_tree(store);
}

/* An object of any class, written as the backend's obj_write would. */
static int
write_object(uint32_t timeline, uint32_t klass, uint32_t object,
			 uint32_t block, uint64_t version, unsigned char tag)
{
	PsKey key = {0, 0, object, 0, klass};
	unsigned char page[8192];
	int rc;

	memset(page, tag, sizeof(page));
	memcpy(page, &version, sizeof(version));
	ps_lock_shard_wr(ps_shard_of(&key));
	rc = append_page(timeline, &key, block, page, version, NULL);
	ps_unlock_shard(ps_shard_of(&key));
	return rc == 0 && ps_storage->sync() == 0;
}

/* Resolved version of an object at or below lsn, or 0 when none is left. */
static uint64_t
object_version_at(uint32_t timeline, uint32_t klass, uint32_t object,
				  uint32_t block, uint64_t lsn)
{
	PsKey key = {0, 0, object, 0, klass};
	unsigned char page[8192];
	uint64_t version = 0;
	int rc;

	ps_lifecycle_read_lock();
	ps_lock_shard_rd(ps_shard_of(&key));
	rc = read_resolve(timeline, &key, block, lsn, 0, page, NULL);
	ps_unlock_shard(ps_shard_of(&key));
	ps_lifecycle_read_unlock();
	if (rc != 1)
		return 0;
	memcpy(&version, page, sizeof(version));
	return version;
}

/* SLRU seeds are replay bases and reader snapshots exact-horizon artifacts,
 * consumed as-of a horizon their consumer pinned or a branch forked at.  Below
 * the floor they survive as the newest version at or below such a fence; a
 * retired artifact also releases the control era it fenced, and a dropped pin
 * retires the artifacts only it protected. */
static void
test_stale_artifacts_are_retired(void)
{
	char store[] = "/tmp/pagestore-control-prune-artifact-XXXXXX";
	PsKey rel = {1, 1, 1, 0, PS_KLASS_RELATION};
	PsKey seed7 = {0, 0, 7, 0, PS_KLASS_SLRU};

	configure_core(1);
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0,
		  "open store for the artifact retention test");
	check(reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
					  PS_RETENTION_RESOURCE_WAL |
					  PS_RETENTION_RESOURCE_WAL_INDEX, 500),
		  "a materializer owns the timeline");
	check(write_relation(0, 0, 900) && write_checkpoint(0, 800, 1000) &&
		  write_relation(0, 0, 1900) && write_checkpoint(0, 1800, 2000) &&
		  write_relation(0, 0, 2900) && write_checkpoint(0, 2800, 3000),
		  "three checkpoints for the artifact retention test");
	check(reserve_pin(0, PS_RETENTION_OWNER_READER, 9, 1,
					  PS_RETENTION_RESOURCE_ALL, 1500),
		  "a reader pins the cutoff it captures its artifacts at");
	check(reserve_pin(0, PS_RETENTION_OWNER_READER, 10, 1,
					  PS_RETENTION_RESOURCE_ALL, 1500),
		  "a second reader pins the same cutoff and captures nothing later");
	check(write_object(0, PS_KLASS_SLRU, 7, 0, 1500, 0x71) &&
		  write_object(0, PS_KLASS_SLRU, 7, 1, 1500, 0x72) &&
		  write_object(0, PS_KLASS_READER_SNAPSHOT, 9, 0, 1500, 0x73) &&
		  write_object(0, PS_KLASS_SLRU, 7, 0, 2500, 0x74) &&
		  write_object(0, PS_KLASS_SLRU, 7, 0, 2500, 0x75) &&
		  write_object(0, PS_KLASS_READER_SNAPSHOT, 9, 0, 2500, 0x76) &&
		  write_object(0, PS_KLASS_READER_SNAPSHOT, 10, 0, 1500, 0x77),
		  "artifacts at the reader's cutoff and at a branch base (with a retry)");
	check(create_branch(1, 0, 2500), "a branch forks at the second cutoff");
	check(ps_test_artifact_fence_count(0) == 2,
		  "both artifact cutoffs fence control images before compaction");
	check(reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
					  PS_RETENTION_RESOURCE_WAL |
					  PS_RETENTION_RESOURCE_WAL_INDEX, 3400),
		  "the materializer moves its restart redo above every artifact");
	run_maintenance(64);
	/* The reader's pin is the floor, so everything at or above it stays,
	 * retried copies included. */
	check(object_version_at(0, PS_KLASS_SLRU, 7, 0, 1500) == 1500 &&
		  object_version_at(0, PS_KLASS_READER_SNAPSHOT, 9, 0, 1500) == 1500 &&
		  object_version_at(0, PS_KLASS_READER_SNAPSHOT, 10, 0, 1500) == 1500 &&
		  object_version_at(0, PS_KLASS_SLRU, 7, 0, 2500) == 2500 &&
		  ps_test_page_version_count(0, &seed7, 0) == 3 &&
		  ps_test_artifact_fence_count(0) == 2,
		  "artifacts at and above the reader's pin survive");
	check(drop_pin(0, PS_RETENTION_OWNER_READER, 9, 1) &&
		  drop_pin(0, PS_RETENTION_OWNER_READER, 10, 1),
		  "the readers release their pins");
	run_maintenance(64);
	/* Only the branch's fork point remains below the floor.  A consumer at
	 * that fork reads the seed at exactly the newest generation at or below
	 * it, 2500; the object's 1500 generation serves nobody any more, so both
	 * pages of it are retired, including the page that has no 2500 copy (it
	 * is absent at 2500, and its 1500 copy cannot stand in), and only the
	 * 2500 cutoff still fences control images. */
	check(object_version_at(0, PS_KLASS_SLRU, 7, 0, 1500) == 0 &&
		  object_version_at(0, PS_KLASS_READER_SNAPSHOT, 9, 0, 1500) == 0,
		  "a dropped pin retires the artifacts it protected below the floor");
	check(object_version_at(0, PS_KLASS_SLRU, 7, 1, 1500) == 0,
		  "a seed page missing from the newer generation at the fork is not kept");
	/* The second reader's snapshot has no later generation, so the branch's
	 * fork point is the newest horizon above it.  A snapshot is read at
	 * exactly the horizon it was captured for, and no fence names 1500 any
	 * more, so the fork above it must not keep it the way it keeps a seed. */
	check(object_version_at(0, PS_KLASS_READER_SNAPSHOT, 10, 0, 1500) == 0,
		  "a snapshot with no fence at its own horizon is retired, not held by a fence above it");
	check(object_version_at(0, PS_KLASS_SLRU, 7, 0, 2500) == 2500 &&
		  object_version_at(0, PS_KLASS_READER_SNAPSHOT, 9, 0, 2500) == 2500 &&
		  ps_test_page_version_count(0, &seed7, 0) == 1,
		  "artifacts at a live branch's fork point survive, one copy per cutoff");
	check(ps_test_artifact_fence_count(0) == 1,
		  "only the cutoff with a surviving generation still fences control images");
	check(ps_test_page_version_count(0, &rel, 0) == 2,
		  "relation history keeps the branch's base and the newest version");
	close_store();
	configure_core(1);
	check(ps_core_open(store) == 0, "reopen the store after artifact pruning");
	check(object_version_at(0, PS_KLASS_SLRU, 7, 0, 1500) == 0 &&
		  object_version_at(0, PS_KLASS_SLRU, 7, 0, 2500) == 2500 &&
		  object_version_at(0, PS_KLASS_SLRU, 7, 1, 1500) == 0 &&
		  ps_test_artifact_fence_count(0) == 1,
		  "retired artifacts stay retired and the surviving bases persist across restart");
	close_store();
	remove_tree(store);
}

/* The live SLRU mirror, its truncation tombstones, and the visibility
 * watermark are read as-of a horizon like relation pages, so they keep the
 * newest version at or below every fence and the floor, and everything above
 * the floor. */
static void
test_live_slru_mirror_follows_page_history(void)
{
	char store[] = "/tmp/pagestore-control-prune-slrulive-XXXXXX";
	PsKey live = {0, 0, 7, 0, PS_KLASS_SLRU_LIVE};
	PsKey tomb = {0, 0, 7, 0, PS_KLASS_SLRU_TOMB};
	PsKey wm = {0, 0, 0, 0, PS_KLASS_SLRU_WM};

	configure_core(1);
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0,
		  "open store for the live SLRU retention test");
	check(reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
					  PS_RETENTION_RESOURCE_WAL |
					  PS_RETENTION_RESOURCE_WAL_INDEX, 500),
		  "a materializer owns the live-mirror timeline");
	check(write_object(0, PS_KLASS_SLRU_LIVE, 7, 0, 1000, 0x11) &&
		  write_object(0, PS_KLASS_SLRU_WM, 0, 0, 1100, 0x12) &&
		  write_object(0, PS_KLASS_SLRU_TOMB, 7, 0, 1200, 0x13) &&
		  write_object(0, PS_KLASS_SLRU_LIVE, 7, 0, 2000, 0x14) &&
		  write_object(0, PS_KLASS_SLRU_WM, 0, 0, 2100, 0x15) &&
		  write_object(0, PS_KLASS_SLRU_TOMB, 7, 0, 2200, 0x16) &&
		  write_object(0, PS_KLASS_SLRU_LIVE, 7, 0, 3000, 0x17) &&
		  write_object(0, PS_KLASS_SLRU_WM, 0, 0, 3100, 0x18),
		  "three generations of live mirror, tombstone, and watermark");
	check(create_branch(1, 0, 1500), "a branch forks inside the history");
	check(reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
					  PS_RETENTION_RESOURCE_WAL |
					  PS_RETENTION_RESOURCE_WAL_INDEX, 3500),
		  "the materializer's restart redo passes the whole history");
	run_maintenance(64);
	check(ps_test_page_version_count(0, &live, 0) == 2 &&
		  object_version_at(0, PS_KLASS_SLRU_LIVE, 7, 0, 1500) == 1000 &&
		  object_version_at(0, PS_KLASS_SLRU_LIVE, 7, 0, 3500) == 3000,
		  "the live mirror keeps the branch's version and the newest one");
	check(ps_test_page_version_count(0, &tomb, 0) == 2 &&
		  object_version_at(0, PS_KLASS_SLRU_TOMB, 7, 0, 1500) == 1200 &&
		  object_version_at(0, PS_KLASS_SLRU_TOMB, 7, 0, 3500) == 2200,
		  "tombstones keep the branch's version and the newest one");
	check(ps_test_page_version_count(0, &wm, 0) == 2 &&
		  object_version_at(0, PS_KLASS_SLRU_WM, 0, 0, 1500) == 1100 &&
		  object_version_at(0, PS_KLASS_SLRU_WM, 0, 0, 3500) == 3100,
		  "the watermark keeps the branch's version and the newest one");
	close_store();
	remove_tree(store);
}

/*
 * A shutdown checkpoint writes a record-less DB_SHUTDOWNING state image at
 * the future checkpoint record's start LSN, then the completed checkpoint
 * publishes its exact-redo twin at that same LSN under a later admission
 * sequence.  Compaction must keep the twin: the two images share a version,
 * and only the exact (lsn, admission_seq) tuple is authoritative.  A stale
 * pre-checkpoint image surviving as the sole version makes
 * pagestore_branch_checkpoint() reject the writer's checkpoint.
 */
static void
test_same_version_twin_supersedes_pre_checkpoint_image(void)
{
	char store[] = "/tmp/pagestore-control-prune-twin-XXXXXX";
	PsKey key = control_key();
	unsigned char page[8192];
	uint64_t version = 0;
	const uint64_t redo = 3000;
	const uint64_t update = 3200;
	const uint64_t prev_redo = 2800;

	configure_core(1);
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0,
		  "open store for the exact-redo twin test");

	/* The pre-checkpoint state image: its note names the previous
	 * checkpoint's redo and its image carries that identity. */
	ps_lock_shard_wr(ps_shard_of(&key));
	memset(page, 0, sizeof(page));
	memcpy(page, &prev_redo, sizeof(prev_redo));
	check(append_page(0, &key, PS_REDO_NOTE_BLOCK, page, redo, NULL) == 0,
		  "pre-checkpoint note at the future redo");
	memset(page, 0, sizeof(page));
	memcpy(page, &prev_redo, sizeof(prev_redo));
	check(append_page(0, &key, PS_CONTROL_IMAGE_BLOCK, page, redo, NULL) == 0,
		  "pre-checkpoint image at the future redo");
	ps_unlock_shard(ps_shard_of(&key));
	check(ps_storage->sync() == 0, "sync the pre-checkpoint image");

	/* The completed checkpoint then publishes its exact-redo twin at the
	 * same version, under a later admission sequence. */
	check(write_checkpoint(0, redo, update),
		  "checkpoint with an exact-redo twin at the same redo");
	check(reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
					  PS_RETENTION_RESOURCE_ALL, redo - 1),
		  "materializer cutoff below the checkpoint");
	run_maintenance(64);
	check(read_control_at(0, PS_CONTROL_IMAGE_BLOCK, redo, &version) &&
		  version == redo,
		  "the exact-redo twin, not the pre-checkpoint image, is restorable");
	check(read_control_at(0, PS_REDO_NOTE_BLOCK, redo, &version) &&
		  version == redo,
		  "the twin's redo note is the one retained");
	close_store();
	remove_tree(store);
}

/* Independently versioned control blocks (materializer marker, checkpoints)
 * keep their own newest visible version, whether or not their LSNs coincide
 * with image versions the pair plan drops. */
static void
test_independent_blocks_keep_their_newest(void)
{
	char store[] = "/tmp/pagestore-control-prune-marker-XXXXXX";
	PsKey key = control_key();
	unsigned char page[8192];

	configure_core(1);
	check(mkdtemp(store) != NULL && ps_core_open(store) == 0,
		  "open store for the marker test");
	check(write_relation(0, 0, 900) && write_control(0, 1000, 800) &&
		  write_relation(0, 0, 1900) && write_control(0, 2000, 1800) &&
		  write_relation(0, 0, 2900) && write_control(0, 3000, 2800),
		  "write three checkpoints for the marker test");
	ps_lock_shard_wr(ps_shard_of(&key));
	for (uint64_t version = 1000; version <= 3000; version += 500)
	{
		memset(page, 0, sizeof(page));
		memcpy(page, &version, sizeof(version));
		check(append_page(0, &key, 3, page, version, NULL) == 0,
			  "write a materializer marker version");
	}
	ps_unlock_shard(ps_shard_of(&key));
	check(ps_storage->sync() == 0, "sync the markers");
	check(ps_test_page_version_count(0, &key, 3) == 5,
		  "five marker versions before compaction");
	check(reserve_pin(0, PS_RETENTION_OWNER_MATERIALIZER, 1, 1,
					  PS_RETENTION_RESOURCE_ALL, 3500),
		  "materializer cutoff above every marker");
	run_maintenance(64);
	check(ps_test_page_version_count(0, &key, 3) == 1,
		  "only the newest marker survives the cutoff");
	{
		uint64_t version = 0;

		check(read_control_at(0, 3, 3500, &version) && version == 3000,
			  "the newest marker is the one retained");
		check(read_control_at(0, 0, 3500, &version) && version == 3000,
			  "the image plan is unaffected by the marker block");
	}
	close_store();
	remove_tree(store);
}

static uint32_t compact_test_reads;
static bool compact_test_fail_bulk;

static int
compact_counted_read(const PsLayerDesc *layer, uint64_t off, void *buf, uint32_t len)
{
	compact_test_reads++;
	if (compact_test_fail_bulk && len > page_size)
		return -1;
	return PsLayerStoreLocal.read_layer_block(layer, off, buf, len);
}

static void
test_compaction_source_read_batch(void)
{
	char store[] = "/tmp/pagestore-compact-read-XXXXXX";
	PsLayerStore counted = PsLayerStoreLocal;
	PsImgRec recs[32] = {0};
	unsigned char pages[32][8192];
	unsigned char *out = malloc(sizeof(pages));
	PsLayerDesc layer;
	PsKey key = {1, 1, 31000, 0, PS_KLASS_RELATION};
	int rc;

	configure_core(0);
	if (out == NULL || mkdtemp(store) == NULL || ps_core_open(store) != 0)
	{
		check(0, "open compaction source-read fixture");
		free(out);
		return;
	}
	for (uint32_t i = 0; i < 32; i++)
	{
		memset(pages[i], (int) i, sizeof(pages[i]));
		recs[i] = (PsImgRec) {.key = key, .block = i, .lsn = 100, .page = pages[i]};
	}
	rc = ps_image_layer_write(31000, 0, recs, 32, page_size, &layer);
	check(rc == 0, "create contiguous compaction source layer");
	if (rc != 0)
	{
		close_store();
		remove_tree(store);
		free(out);
		return;
	}
	counted.read_layer_block = compact_counted_read;
	ps_layer_store = &counted;
	compact_test_reads = 0;
	compact_test_fail_bulk = false;
	rc = ps_test_compact_read_batch(&layer, 32, out);
	check(rc == 0 && memcmp(out, pages, sizeof(pages)) == 0 && compact_test_reads == 1,
		  "adjacent compaction pages share a verified source read");
	compact_test_reads = 0;
	compact_test_fail_bulk = true;
	rc = ps_test_compact_read_batch(&layer, 32, out);
	check(rc == 0 && memcmp(out, pages, sizeof(pages)) == 0 && compact_test_reads >= 32,
		  "failed bulk reads fall back to individual source reads");
	ps_layer_store = &PsLayerStoreLocal;
	close_store();
	remove_tree(store);
	free(out);
}

int
main(void)
{
	test_nonzero_floor_bounded_plan(0);
	test_nonzero_floor_bounded_plan(1);
	test_nonzero_bounded_plan_conflict(0);
	test_nonzero_bounded_plan_conflict(1);
	test_nonzero_bounded_plan_conflict(2);
	test_bounded_prune_conflict(0);
	test_bounded_prune_conflict(1);
	test_bounded_prune_conflict(2);
	test_bounded_prune_interrupted_retirement();
	test_bounded_prune_crash("page_prune.after_frontier");
	test_bounded_prune_crash("page_compaction.after_publish");
	test_bounded_prune_crash("page_gc.after_mark_delete");
	test_segment_gc_does_not_starve_compaction();
	test_bounded_compaction_maintenance();
	test_bounded_shared_payload_capacity_fallback();
	test_bounded_prune_multiple_outputs(0);
	test_bounded_prune_multiple_outputs(1);
	test_bounded_prune_multiple_outputs(2);
	test_bounded_prune_index_batches(0);
	test_bounded_prune_index_batches(1);
	test_bounded_prune_index_batches(2);
	test_hot_page_prune_batches(0);
	test_hot_page_prune_batches(1);
	test_hot_page_prune_batches(2);
	test_long_chain_prune_snapshot(0);
	test_long_chain_prune_snapshot(1);
	test_long_chain_prune_snapshot(2);
	test_long_chain_prune_snapshot(3);
	test_long_chain_prune_snapshot(4);
	test_long_chain_prune_snapshot(5);
	test_bounded_compaction_concurrent_write();
	test_compaction_gather_rejects_concurrent_change(0);
	test_compaction_gather_rejects_concurrent_change(1);
	test_staged_compaction_rejects_concurrent_write(1);
	test_staged_compaction_rejects_concurrent_write(2);
	test_staged_compaction_rejects_new_retention_pin();
	test_staged_compaction_rejects_remote_transition();
	test_staged_compaction_preserves_pinned_reader();
	test_staged_compaction_makes_progress_after_conflicts();
	test_compaction_source_read_batch();
	check(ps_test_control_fence_selections() == 0, "cached control fence selections match the original planner");
	test_floor_retires_older_checkpoints();
	test_note_identity_trailer();
	test_reader_pin_retains_its_checkpoint();
	test_branch_cap_retains_its_checkpoint();
	test_split_pair_is_never_unnoted();
	test_retry_copies_collapse();
	test_in_flight_note_retries_collapse();
	test_wal_only_pin_release_reschedules();
	test_slru_seed_keeps_its_control_image();
	test_late_artifact_below_frontier_is_refused();
	test_failed_artifact_append_releases_its_fence();
	test_materializer_pin_is_the_page_cutoff();
	test_checkpoint_note_is_the_page_cutoff();
	test_stale_artifacts_are_retired();
	test_live_slru_mirror_follows_page_history();
	test_same_version_twin_supersedes_pre_checkpoint_image();
	test_independent_blocks_keep_their_newest();
	fprintf(stderr, "%d checks, %d failures\n", checks, failed);
	return failed != 0;
}
