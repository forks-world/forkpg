/*-------------------------------------------------------------------------
 *
 * pagestore_layer.c
 *	  LSM-like immutable layer metadata helpers.
 *
 *-------------------------------------------------------------------------
 */
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pagestore_layer.h"
#include "pagestore_layer_store.h"

void
ps_layer_map_init(PsLayerMap *map)
{
	memset(map, 0, sizeof(*map));
}

void
ps_layer_map_free(PsLayerMap *map)
{
	free(map->layers);
	memset(map, 0, sizeof(*map));
}

int
ps_layer_map_reserve(PsLayerMap *map, uint32_t capacity)
{
	if (capacity > map->capacity)
	{
		uint32_t	newcap = map->capacity ? map->capacity * 2 : 64;
		PsLayerDesc *newlayers;

		while (newcap < capacity)
			newcap *= 2;
		newlayers = realloc(map->layers, (size_t) newcap * sizeof(PsLayerDesc));
		if (newlayers == NULL)
			return -1;
		map->layers = newlayers;
		map->capacity = newcap;
	}
	return 0;
}

int
ps_layer_map_add(PsLayerMap *map, const PsLayerDesc *desc)
{
	if (ps_layer_map_reserve(map, map->nlayers + 1) != 0)
		return -1;
	map->layers[map->nlayers++] = *desc;
	return 0;
}

uint32_t
ps_layer_map_count(const PsLayerMap *map)
{
	return map->nlayers;
}

/* ===================== image layer file format ========================= */


/* FNV-1a checksum for layer integrity (not cryptographic). */
static uint32_t
img_crc_extend(uint32_t h, const void *p, size_t n)
{
	const unsigned char *b = p;

	for (size_t i = 0; i < n; i++)
	{
		h ^= b[i];
		h *= 16777619u;
	}
	return h;
}

static uint32_t
img_crc(const void *p, size_t n)
{
	return img_crc_extend(2166136261u, p, n);
}

#define IMAGE_STREAM_BYTES (256 * 1024)
static int
key_cmp(const PsKey *a, const PsKey *b)
{
	if (a->spcOid != b->spcOid)
		return a->spcOid < b->spcOid ? -1 : 1;
	if (a->dbOid != b->dbOid)
		return a->dbOid < b->dbOid ? -1 : 1;
	if (a->relNumber != b->relNumber)
		return a->relNumber < b->relNumber ? -1 : 1;
	if (a->forkNum != b->forkNum)
		return a->forkNum < b->forkNum ? -1 : 1;
	if (a->klass != b->klass)
		return a->klass < b->klass ? -1 : 1;
	return 0;
}

/* internal sort record: on-disk index entry plus its source page index */
typedef struct ImgSort
{
	PsImgIndexEnt ent;
	uint32_t	src;
} ImgSort;

typedef struct PsImgIndexEntV2
{
	PsKey		key;
	uint32_t	block;
	uint64_t	lsn;
	uint64_t	data_off;
} PsImgIndexEntV2;

typedef struct PsImgIndexEntV3
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
} PsImgIndexEntV3;

static int
img_sort_cmp(const void *pa, const void *pb)
{
	const ImgSort *a = pa;
	const ImgSort *b = pb;
	int			c = key_cmp(&a->ent.key, &b->ent.key);

	if (c)
		return c;
	if (a->ent.block != b->ent.block)
		return a->ent.block < b->ent.block ? -1 : 1;
	if (a->ent.lsn != b->ent.lsn)
		return a->ent.lsn < b->ent.lsn ? -1 : 1;
	if (a->ent.admission_seq != b->ent.admission_seq)
		return a->ent.admission_seq < b->ent.admission_seq ? -1 : 1;
	/* qsort is not stable.  Preserve append order for same-pd_lsn rewrites so
	 * the last index entry remains the latest bytes after segment reclamation. */
	return a->src < b->src ? -1 : (a->src > b->src ? 1 : 0);
}

/*
 * Necessary-condition prune: can layer 'd' possibly hold (key, block)?  Uses the
 * layer's key range and global block range (min/max across all its entries).
 * A 0 means definitely-absent, so the caller can skip the layer without reading
 * its footer/index/data.  (block range is coarse -- it is the global min/max,
 * not per-key -- but a block outside it is still definitely absent.)
 */
static int
layer_covers(const PsLayerDesc *d, const PsKey *key, uint32_t block)
{
	if (key_cmp(key, &d->start_key) < 0 || key_cmp(key, &d->end_key) > 0)
		return 0;
	if (block < d->start_block || block > d->end_block)
		return 0;
	return 1;
}

static const PsLayerLocation *
img_local_loc(const PsLayerDesc *layer)
{
	uint32_t	nlocs = layer->location_count;
	const PsLayerLocation *local = NULL;
	const PsLayerLocation *remote = NULL;

	if (nlocs > PS_LAYER_MAX_LOCATIONS)
		nlocs = PS_LAYER_MAX_LOCATIONS;
	for (uint32_t i = 0; i < nlocs; i++)
	{
		if (layer->locations[i].tier == PS_LAYER_TIER_REMOTE_OBJECT &&
			layer->locations[i].available)
			remote = &layer->locations[i];
		if ((layer->locations[i].tier == PS_LAYER_TIER_LOCAL_HOT ||
			 layer->locations[i].tier == PS_LAYER_TIER_LOCAL_COLD))
		{
			if (layer->locations[i].available)
				return &layer->locations[i];
			local = &layer->locations[i];
		}
	}
	return local ? local : remote;
}

static int
image_memory_page(const PsImgRec *rec, void *page, void *arg)
{
	memcpy(page, rec->page, *(uint32_t *) arg);
	return 0;
}

int
ps_image_layer_write(uint64_t layer_id, uint32_t timeline,
					 const PsImgRec *recs, uint32_t n, uint32_t page_size, PsLayerDesc *out)
{
	return ps_image_layer_write_stream(layer_id, timeline, recs, n, page_size,
									   image_memory_page, &page_size, out);
}

int
ps_image_layer_write_sorted_batch(uint64_t layer_id, uint32_t timeline,
								  const PsImgRec *recs, uint32_t n, uint32_t page_size,
								  PsImagePageReader read_page, void *arg, PsLayerDesc *out)
{
	if (recs == NULL || out == NULL || n == 0 || n > PS_IMAGE_BATCH_MAX_RECORDS)
		return -1;
	for (uint32_t i = 1; i < n; i++)
	{
		int c = key_cmp(&recs[i - 1].key, &recs[i].key);

		if (c > 0 || (c == 0 &&
			(recs[i - 1].block > recs[i].block ||
			 (recs[i - 1].block == recs[i].block &&
			  (recs[i - 1].lsn > recs[i].lsn ||
			   (recs[i - 1].lsn == recs[i].lsn &&
				recs[i - 1].admission_seq > recs[i].admission_seq))))))
			return -1;
	}
	return ps_image_layer_write_stream(layer_id, timeline, recs, n, page_size,
									   read_page, arg, out);
}

int
ps_image_layer_write_stream(uint64_t layer_id, uint32_t timeline,
							const PsImgRec *recs, uint32_t n, uint32_t page_size,
							PsImagePageReader read_page, void *arg, PsLayerDesc *out)
{
	ImgSort    *sorted = NULL;
	unsigned char *buffer = NULL;
	PsImgFooter foot;
	uint64_t	data_bytes = (uint64_t) n * page_size;
	uint64_t	idx_bytes = (uint64_t) n * sizeof(PsImgIndexEnt);
	uint64_t	total = data_bytes + idx_bytes + sizeof(PsImgFooter);
	uint64_t	offset = 0;
	uint32_t	used = 0;
	uint32_t	data_crc = 2166136261u;
	uint32_t	index_crc = 2166136261u;
	char		uri[PS_LAYER_URI_MAX];
	int			rc = -1;
	bool		created = false;

	if (n == 0 || page_size == 0 || page_size > IMAGE_STREAM_BYTES ||
		read_page == NULL || ps_layer_store->write_local_layer_at == NULL)
		return -1;
	sorted = malloc((size_t) n * sizeof(*sorted));
	buffer = malloc(IMAGE_STREAM_BYTES);
	if (!sorted || !buffer)
		goto out;
	for (uint32_t i = 0; i < n; i++)
	{
		memset(&sorted[i], 0, sizeof(sorted[i]));
		sorted[i].ent.key = recs[i].key;
		sorted[i].ent.block = recs[i].block;
		sorted[i].ent.lsn = recs[i].lsn;
		sorted[i].ent.admission_seq = recs[i].admission_seq;
		sorted[i].ent.growth_lsn = recs[i].growth_lsn;
		sorted[i].ent.order_id = recs[i].order_id;
		sorted[i].ent.seg_off = recs[i].seg_off;
		sorted[i].ent.seg_id = recs[i].seg_id;
		sorted[i].ent.flags = recs[i].flags;
		sorted[i].src = i;
	}
	qsort(sorted, n, sizeof(*sorted), img_sort_cmp);
	if (ps_layer_store->create_local_layer(layer_id, uri, sizeof(uri)) != 0)
		goto out;
	created = true;
	for (uint32_t i = 0; i < n; i++)
	{
		if (used + page_size > IMAGE_STREAM_BYTES)
		{
			if (ps_layer_store->write_local_layer_at(layer_id, offset, buffer, used) != 0)
				goto out;
			offset += used;
			used = 0;
		}
		sorted[i].ent.data_off = (uint64_t) i * page_size;
		if (read_page(&recs[sorted[i].src], buffer + used, arg) != 0)
			goto out;
		data_crc = img_crc_extend(data_crc, buffer + used, page_size);
		used += page_size;
	}
	if (used != 0 &&
		ps_layer_store->write_local_layer_at(layer_id, offset, buffer, used) != 0)
		goto out;
	offset += used;
	used = 0;
	for (uint32_t i = 0; i < n; i++)
	{
		if (used + sizeof(PsImgIndexEnt) > IMAGE_STREAM_BYTES)
		{
			if (ps_layer_store->write_local_layer_at(layer_id, offset, buffer, used) != 0)
				goto out;
			offset += used;
			used = 0;
		}
		memcpy(buffer + used, &sorted[i].ent, sizeof(PsImgIndexEnt));
		index_crc = img_crc_extend(index_crc, buffer + used, sizeof(PsImgIndexEnt));
		used += sizeof(PsImgIndexEnt);
	}
	if (used != 0 &&
		ps_layer_store->write_local_layer_at(layer_id, offset, buffer, used) != 0)
		goto out;
	offset += used;
	memset(&foot, 0, sizeof(foot));
	foot.magic = PS_IMG_MAGIC;
	foot.version = PS_IMG_VERSION;
	foot.page_size = page_size;
	foot.nrecs = n;
	foot.index_off = data_bytes;
	foot.data_crc = data_crc;
	foot.index_crc = index_crc;
	if (ps_layer_store->write_local_layer_at(layer_id, offset, &foot, sizeof(foot)) != 0 ||
		ps_layer_store->seal_local_layer(layer_id) != 0)
		goto out;

	memset(out, 0, sizeof(*out));
	out->layer_id = layer_id;
	out->kind = PS_LAYER_IMAGE;
	out->timeline = timeline;
	out->start_key = sorted[0].ent.key;
	out->end_key = sorted[n - 1].ent.key;
	out->start_block = sorted[0].ent.block;
	out->end_block = sorted[n - 1].ent.block;
	out->lsn_start = sorted[0].ent.lsn;
	out->lsn_end = sorted[0].ent.lsn;
	for (uint32_t i = 0; i < n; i++)
	{
		if (sorted[i].ent.lsn < out->lsn_start)
			out->lsn_start = sorted[i].ent.lsn;
		if (sorted[i].ent.lsn > out->lsn_end)
			out->lsn_end = sorted[i].ent.lsn;
		if (sorted[i].ent.block < out->start_block)
			out->start_block = sorted[i].ent.block;
		if (sorted[i].ent.block > out->end_block)
			out->end_block = sorted[i].ent.block;
	}
	out->created_at_lsn = out->lsn_end;
	out->location_count = 1;
	out->locations[0].tier = PS_LAYER_TIER_LOCAL_HOT;
	out->locations[0].size = total;
	out->locations[0].available = true;
	snprintf(out->locations[0].uri, sizeof(out->locations[0].uri), "%s", uri);
	rc = 0;

out:
	/* Only a successful create transfers ownership of this unpublished file.
	 * In particular, a failed O_EXCL create must not delete an existing layer.
	 * Source reads can fail after earlier chunks reached disk; clean them now
	 * so repeated maintenance retries do not accumulate partial replacements. */
	if (rc != 0 && created)
	{
		PsLayerDesc partial = {0};

		partial.layer_id = layer_id;
		partial.kind = PS_LAYER_IMAGE;
		partial.timeline = timeline;
		partial.location_count = 1;
		partial.locations[0].tier = PS_LAYER_TIER_LOCAL_HOT;
		partial.locations[0].size = total;
		partial.locations[0].available = true;
		snprintf(partial.locations[0].uri, sizeof(partial.locations[0].uri),
				 "%s", uri);
		(void) ps_layer_store->delete_local_layer(&partial);
	}
	free(sorted);
	free(buffer);
	return rc;
}
/* ===================== delta layer file format ========================= */

typedef struct DeltaSort
{
	PsDeltaIndexEnt ent;
	uint32_t	src;
} DeltaSort;

static int
delta_sort_cmp(const void *pa, const void *pb)
{
	const DeltaSort *a = pa;
	const DeltaSort *b = pb;
	int			c = key_cmp(&a->ent.key, &b->ent.key);

	if (c)
		return c;
	if (a->ent.block != b->ent.block)
		return a->ent.block < b->ent.block ? -1 : 1;
	if (a->ent.lsn != b->ent.lsn)
		return a->ent.lsn < b->ent.lsn ? -1 : 1;
	return 0;
}

int
ps_delta_layer_write(uint64_t layer_id, uint32_t timeline,
					 const PsDeltaRec *recs, uint32_t n, PsLayerDesc *out)
{
	DeltaSort  *sorted;
	char	   *filebuf;
	char	   *idxsec;
	PsDeltaFooter foot;
	uint64_t	data_bytes = 0;
	uint64_t	idx_bytes = (uint64_t) n * sizeof(PsDeltaIndexEnt);
	uint64_t	total;
	uint64_t	off;
	char		uri[PS_LAYER_URI_MAX];
	int			rc = -1;

	if (n == 0)
		return -1;
	for (uint32_t i = 0; i < n; i++)
		data_bytes += recs[i].delta_len;
	total = data_bytes + idx_bytes + sizeof(PsDeltaFooter);

	sorted = malloc((size_t) n * sizeof(DeltaSort));
	filebuf = malloc((size_t) total);
	if (!sorted || !filebuf)
		goto out;

	for (uint32_t i = 0; i < n; i++)
	{
		sorted[i].ent.key = recs[i].key;
		sorted[i].ent.block = recs[i].block;
		sorted[i].ent.lsn = recs[i].lsn;
		sorted[i].ent.data_len = recs[i].delta_len;
		sorted[i].src = i;
	}
	qsort(sorted, n, sizeof(DeltaSort), delta_sort_cmp);

	off = 0;
	for (uint32_t i = 0; i < n; i++)
	{
		sorted[i].ent.data_off = off;
		memcpy(filebuf + off, recs[sorted[i].src].delta, sorted[i].ent.data_len);
		off += sorted[i].ent.data_len;
	}
	idxsec = filebuf + data_bytes;
	for (uint32_t i = 0; i < n; i++)
		memcpy(idxsec + (size_t) i * sizeof(PsDeltaIndexEnt), &sorted[i].ent,
			   sizeof(PsDeltaIndexEnt));
	memset(&foot, 0, sizeof(foot));
	foot.magic = PS_DELTA_MAGIC;
	foot.version = PS_DELTA_VERSION;
	foot.nrecs = n;
	foot.index_off = data_bytes;
	foot.data_crc = img_crc(filebuf, (size_t) data_bytes);
	foot.index_crc = img_crc(idxsec, (size_t) idx_bytes);
	memcpy(filebuf + data_bytes + idx_bytes, &foot, sizeof(foot));

	if (ps_layer_store->create_local_layer(layer_id, uri, sizeof(uri)) != 0)
		goto out;
	if (ps_layer_store->write_local_layer(layer_id, filebuf, total) != 0 ||
		ps_layer_store->seal_local_layer(layer_id) != 0)
		goto out;

	memset(out, 0, sizeof(*out));
	out->layer_id = layer_id;
	out->kind = PS_LAYER_DELTA;
	out->timeline = timeline;
	out->start_key = sorted[0].ent.key;
	out->end_key = sorted[n - 1].ent.key;
	out->start_block = sorted[0].ent.block;
	out->end_block = sorted[n - 1].ent.block;
	out->lsn_start = sorted[0].ent.lsn;
	out->lsn_end = sorted[0].ent.lsn;
	for (uint32_t i = 0; i < n; i++)
	{
		if (sorted[i].ent.lsn < out->lsn_start)
			out->lsn_start = sorted[i].ent.lsn;
		if (sorted[i].ent.lsn > out->lsn_end)
			out->lsn_end = sorted[i].ent.lsn;
		if (sorted[i].ent.block < out->start_block)
			out->start_block = sorted[i].ent.block;
		if (sorted[i].ent.block > out->end_block)
			out->end_block = sorted[i].ent.block;
	}
	out->created_at_lsn = out->lsn_end;
	out->location_count = 1;
	out->locations[0].tier = PS_LAYER_TIER_LOCAL_HOT;
	out->locations[0].size = total;
	out->locations[0].available = true;
	snprintf(out->locations[0].uri, sizeof(out->locations[0].uri), "%s", uri);
	rc = 0;

out:
	free(sorted);
	free(filebuf);
	return rc;
}

int
ps_delta_layer_collect(const PsLayerDesc *layer, const PsKey *key,
					   uint32_t block, uint64_t lo_lsn, uint64_t hi_lsn,
					   PsDeltaOut *outs, uint32_t cap, uint32_t *n)
{
	const PsLayerLocation *loc = img_local_loc(layer);
	PsDeltaFooter foot;
	PsDeltaIndexEnt *idx = NULL;
	uint64_t	idx_bytes;
	int			rc = -1;
	int			retried = 0;

	/* prune: skip without any IO if this layer cannot hold (key,block) or its
	 * LSN range does not overlap the requested (lo_lsn, hi_lsn] */
	if (!layer_covers(layer, key, block) ||
		layer->lsn_end <= lo_lsn || layer->lsn_start > hi_lsn)
		return 0;

retry:
	if (!loc || loc->size < sizeof(PsDeltaFooter))
		return -1;
	if (ps_layer_store->read_layer_block(layer, loc->size - sizeof(foot),
									 &foot, sizeof(foot)) != 0)
		goto refresh;
	if (foot.magic != PS_DELTA_MAGIC || foot.version != PS_DELTA_VERSION ||
		foot.nrecs == 0)
		goto refresh;
	idx_bytes = (uint64_t) foot.nrecs * sizeof(PsDeltaIndexEnt);
	idx = malloc((size_t) idx_bytes);
	if (!idx)
		return -1;
	if (ps_layer_store->read_layer_block(layer, foot.index_off, idx,
									 (uint32_t) idx_bytes) != 0 ||
		img_crc(idx, (size_t) idx_bytes) != foot.index_crc)
		goto refresh;

	/* index is sorted by (key, block, lsn): matches are contiguous + ascending */
	for (uint32_t i = 0; i < foot.nrecs && *n < cap; i++)
	{
		if (idx[i].block != block || key_cmp(&idx[i].key, key) != 0)
			continue;
		if (idx[i].lsn <= lo_lsn || idx[i].lsn > hi_lsn)
			continue;
		outs[*n].lsn = idx[i].lsn;
		outs[*n].data_off = (uint32_t) idx[i].data_off;
		outs[*n].data_len = idx[i].data_len;
		(*n)++;
	}
	rc = 0;
	goto out;

refresh:
	free(idx);
	idx = NULL;
	if (!retried && ps_layer_store->refresh_layer_cache != NULL &&
		ps_layer_store->refresh_layer_cache(layer) == 0)
	{
		retried = 1;
		goto retry;
	}

out:
	free(idx);
	return rc;
}

/* ===================== read plan (phase 7b) ============================ */

#define PS_PLAN_MAX_CHAIN	1024	/* per-layer delta cap (chains are bounded
									 * by compaction policy) */

static int
plan_delta_cmp(const void *pa, const void *pb)
{
	uint64_t	a = ((const PsPlanDelta *) pa)->lsn;
	uint64_t	b = ((const PsPlanDelta *) pb)->lsn;

	return a < b ? -1 : (a > b ? 1 : 0);
}

void
ps_read_plan_free(PsReadPlan *plan)
{
	if (!plan)
		return;
	free(plan->base);
	for (uint32_t i = 0; i < plan->ndelta; i++)
		free(plan->deltas[i].bytes);
	free(plan->deltas);
	plan->base = NULL;
	plan->deltas = NULL;
	plan->ndelta = 0;
	plan->has_base = 0;
}

int
ps_read_plan_build(const PsLayerMap *map, uint32_t timeline, const PsKey *key,
				   uint32_t block, uint64_t read_lsn, uint32_t page_size,
				   PsReadPlan *plan)
{
	unsigned char *tmp = malloc(page_size);
	uint64_t	lo;
	uint32_t	cap = 0;
	int			rc = -1;

	memset(plan, 0, sizeof(*plan));
	if (!tmp)
		return -1;

	/* 1. base = newest image-layer version <= read_lsn on this timeline */
	for (uint32_t i = 0; i < map->nlayers; i++)
	{
		const PsLayerDesc *d = &map->layers[i];
		uint64_t	l;

		if (d->kind != PS_LAYER_IMAGE || d->deleting || d->timeline != timeline)
			continue;
		if (ps_image_layer_lookup(d, key, block, read_lsn, 0, tmp, page_size,
								  &l, NULL) == 1 &&
			(!plan->has_base || l > plan->base_lsn))
		{
			if (!plan->base)
				plan->base = malloc(page_size);
			if (!plan->base)
				goto out;
			memcpy(plan->base, tmp, page_size);
			plan->base_lsn = l;
			plan->has_base = 1;
		}
	}
	lo = plan->has_base ? plan->base_lsn : 0;

	/* 2. deltas in (lo, read_lsn] across this timeline's delta layers */
	for (uint32_t i = 0; i < map->nlayers; i++)
	{
		const PsLayerDesc *d = &map->layers[i];
		PsDeltaOut	outs[PS_PLAN_MAX_CHAIN];
		uint32_t	tn = 0;

		if (d->kind != PS_LAYER_DELTA || d->deleting || d->timeline != timeline)
			continue;
		if (ps_delta_layer_collect(d, key, block, lo, read_lsn, outs,
								   PS_PLAN_MAX_CHAIN, &tn) != 0)
			goto out;
		for (uint32_t j = 0; j < tn; j++)
		{
			unsigned char *bytes;

			if (plan->ndelta == cap)
			{
				uint32_t	nc = cap ? cap * 2 : 16;
				PsPlanDelta *nd = realloc(plan->deltas,
										  (size_t) nc * sizeof(PsPlanDelta));

				if (!nd)
					goto out;
				plan->deltas = nd;
				cap = nc;
			}
			bytes = malloc(outs[j].data_len);
			if (!bytes ||
				ps_layer_store->read_layer_block(d, outs[j].data_off, bytes,
												 outs[j].data_len) != 0)
			{
				free(bytes);
				goto out;
			}
			plan->deltas[plan->ndelta].lsn = outs[j].lsn;
			plan->deltas[plan->ndelta].len = outs[j].data_len;
			plan->deltas[plan->ndelta].bytes = bytes;
			plan->ndelta++;
		}
	}

	/* 3. order the chain by ascending LSN (it spans multiple layers) */
	if (plan->ndelta > 1)
		qsort(plan->deltas, plan->ndelta, sizeof(PsPlanDelta), plan_delta_cmp);
	rc = 0;

out:
	free(tmp);
	if (rc != 0)
		ps_read_plan_free(plan);
	return rc;
}

static int
image_fd_read(int fd, uint64_t off, void *buf, uint32_t len)
{
	uint32_t done = 0;
	if (off > INT64_MAX || len > INT64_MAX - off)
		return -1;
	while (done < len)
	{
		ssize_t n = pread(fd, (char *) buf + done, len - done, (off_t) (off + done));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		done += (uint32_t) n;
	}
	return 0;
}

static int
image_index_read(const PsLayerDesc *layer, int fd, uint64_t off, void *buf, uint32_t len)
{
	return fd >= 0 ? image_fd_read(fd, off, buf, len) :
		ps_layer_store->read_layer_block(layer, off, buf, len);
}

static void
image_index_decode(uint32_t version, const unsigned char *src, PsImgIndexEnt *entry)
{
	memset(entry, 0, sizeof(*entry));
	if (version == 2)
	{
		PsImgIndexEntV2 old;
		memcpy(&old, src, sizeof(old));
		entry->key = old.key; entry->block = old.block;
		entry->lsn = old.lsn; entry->data_off = old.data_off;
	}
	else if (version == 3)
	{
		PsImgIndexEntV3 old;
		memcpy(&old, src, sizeof(old));
		entry->key = old.key; entry->block = old.block;
		entry->lsn = old.lsn; entry->data_off = old.data_off;
		entry->growth_lsn = old.growth_lsn; entry->order_id = old.order_id;
		entry->seg_off = old.seg_off; entry->seg_id = old.seg_id; entry->flags = old.flags;
	}
	else
		memcpy(entry, src, sizeof(*entry));
}

int
ps_image_layer_visit_index(const PsLayerDesc *layer, PsImageIndexVisitor visit,
						   void *arg)
{
	const PsLayerLocation *loc = layer != NULL ? img_local_loc(layer) : NULL;
	PsImgFooter foot;
	unsigned char *buf;
	size_t ent_size;
	uint32_t per_chunk;
	uint64_t bytes;
	int rc = -1;

	if (visit == NULL || loc == NULL || loc->size < sizeof(foot) ||
		image_index_read(layer, -1, loc->size - sizeof(foot), &foot, sizeof(foot)) != 0 ||
		foot.magic != PS_IMG_MAGIC || foot.nrecs == 0 ||
		(foot.version != 2 && foot.version != 3 && foot.version != PS_IMG_VERSION))
		return -1;
	ent_size = foot.version == 2 ? sizeof(PsImgIndexEntV2) :
		foot.version == 3 ? sizeof(PsImgIndexEntV3) : sizeof(PsImgIndexEnt);
	bytes = (uint64_t) foot.nrecs * ent_size;
	if (foot.index_off > loc->size - sizeof(foot) ||
		bytes != loc->size - sizeof(foot) - foot.index_off)
		return -1;
	per_chunk = (64 * 1024) / ent_size;
	buf = malloc((size_t) per_chunk * ent_size);
	if (buf == NULL)
		return -1;
	/* Validate before yielding entries; validate again to detect changes or
	 * corruption during traversal.  The caller commits only after success. */
	for (int pass = 0; pass < 2; pass++)
	{
		uint32_t crc = 2166136261u;
		for (uint32_t first = 0; first < foot.nrecs;)
		{
			uint32_t count = foot.nrecs - first;
			uint32_t len;

			if (count > per_chunk)
				count = per_chunk;
			len = count * ent_size;
			if (image_index_read(layer, -1, foot.index_off + (uint64_t) first * ent_size,
								 buf, len) != 0)
				goto out;
			crc = img_crc_extend(crc, buf, len);
			if (pass == 1)
				for (uint32_t i = 0; i < count; i++)
				{
					PsImgIndexEnt entry;
					image_index_decode(foot.version, buf + i * ent_size, &entry);
					if (visit(&entry, arg) != 0)
						goto out;
				}
			first += count;
		}
		if (crc != foot.index_crc)
			goto out;
	}
	rc = 0;
out:
	free(buf);
	return rc;
}

struct PsImageIndexCursor
{
	PsLayerDesc layer;
	PsImgFooter foot;
	PsImgIndexEnt previous;
	unsigned char *buffer;
	size_t ent_size;
	uint32_t per_chunk, next, loaded, chunk_first, crc;
	int have_previous, failed;
};

static int
image_cursor_precheck(const PsImgIndexEnt *entry, void *arg)
{
	(void) entry;
	(void) arg;
	return 0;
}

int
ps_image_index_cursor_open(const PsLayerDesc *layer, PsImageIndexCursor **out)
{
	const PsLayerLocation *loc;
	PsImageIndexCursor *cursor;
	uint64_t bytes;

	if (out == NULL || layer == NULL)
		return -1;
	*out = NULL;
	if (ps_image_layer_visit_index(layer, image_cursor_precheck, NULL) != 0)
		return -1;
	loc = img_local_loc(layer);
	if (loc == NULL || loc->size < sizeof(PsImgFooter))
		return -1;
	cursor = calloc(1, sizeof(*cursor));
	if (cursor == NULL)
		return -1;
	cursor->layer = *layer;
	if (image_index_read(layer, -1, loc->size - sizeof(cursor->foot),
						 &cursor->foot, sizeof(cursor->foot)) != 0 ||
		cursor->foot.magic != PS_IMG_MAGIC || cursor->foot.nrecs == 0 ||
		(cursor->foot.version != 2 && cursor->foot.version != 3 &&
		 cursor->foot.version != PS_IMG_VERSION))
		goto fail;
	cursor->ent_size = cursor->foot.version == 2 ? sizeof(PsImgIndexEntV2) :
		cursor->foot.version == 3 ? sizeof(PsImgIndexEntV3) : sizeof(PsImgIndexEnt);
	bytes = (uint64_t) cursor->foot.nrecs * cursor->ent_size;
	if (cursor->foot.index_off > loc->size - sizeof(cursor->foot) ||
		bytes != loc->size - sizeof(cursor->foot) - cursor->foot.index_off)
		goto fail;
	cursor->per_chunk = (64 * 1024) / cursor->ent_size;
	cursor->buffer = malloc(cursor->per_chunk * cursor->ent_size);
	if (cursor->buffer == NULL)
		goto fail;
	cursor->crc = 2166136261u;
	*out = cursor;
	return 0;
fail:
	free(cursor);
	return -1;
}

int
ps_image_index_cursor_next(PsImageIndexCursor *cursor, PsImgIndexEnt *entry)
{
	int c;

	if (cursor == NULL || entry == NULL || cursor->failed)
		return -1;
	if (cursor->next == cursor->foot.nrecs)
	{
		if (cursor->crc != cursor->foot.index_crc)
		{
			cursor->failed = 1;
			return -1;
		}
		return 0;
	}
	if (cursor->next == cursor->loaded)
	{
		uint32_t count = cursor->foot.nrecs - cursor->next;
		uint32_t len;
		if (count > cursor->per_chunk)
			count = cursor->per_chunk;
		len = count * cursor->ent_size;
		if (image_index_read(&cursor->layer, -1,
				cursor->foot.index_off + (uint64_t) cursor->next * cursor->ent_size,
				cursor->buffer, len) != 0)
		{
			cursor->failed = 1;
			return -1;
		}
		cursor->crc = img_crc_extend(cursor->crc, cursor->buffer, len);
		cursor->chunk_first = cursor->next;
		cursor->loaded = cursor->next + count;
	}
	image_index_decode(cursor->foot.version,
		cursor->buffer + (cursor->next - cursor->chunk_first) * cursor->ent_size, entry);
	c = cursor->have_previous ? key_cmp(&cursor->previous.key, &entry->key) : 0;
	if (cursor->have_previous && (c > 0 || (c == 0 &&
		(cursor->previous.block > entry->block ||
		 (cursor->previous.block == entry->block &&
		  (cursor->previous.lsn > entry->lsn ||
		   (cursor->previous.lsn == entry->lsn &&
			cursor->previous.admission_seq > entry->admission_seq)))))))
	{
		cursor->failed = 1;
		return -1;
	}
	cursor->previous = *entry;
	cursor->have_previous = 1;
	cursor->next++;
	return 1;
}

void
ps_image_index_cursor_close(PsImageIndexCursor *cursor)
{
	if (cursor != NULL)
	{
		free(cursor->buffer);
		free(cursor);
	}
}

struct PsImageIndexMerge
{
	PsImageIndexCursor *cursors[PS_IMAGE_MERGE_MAX_INPUTS];
	PsImgIndexEnt heads[PS_IMAGE_MERGE_MAX_INPUTS];
	uint64_t layer_ids[PS_IMAGE_MERGE_MAX_INPUTS];
	unsigned char active[PS_IMAGE_MERGE_MAX_INPUTS];
	uint32_t count;
	int failed;
};

void
ps_image_index_merge_close(PsImageIndexMerge *merge)
{
	if (merge != NULL)
	{
		for (uint32_t i = 0; i < merge->count; i++)
			ps_image_index_cursor_close(merge->cursors[i]);
		free(merge);
	}
}

int
ps_image_index_merge_open(const PsLayerDesc *layers, uint32_t count,
						  PsImageIndexMerge **out)
{
	PsImageIndexMerge *merge;

	if (out == NULL)
		return -1;
	*out = NULL;
	if (layers == NULL || count == 0 || count > PS_IMAGE_MERGE_MAX_INPUTS)
		return -1;
	merge = calloc(1, sizeof(*merge));
	if (merge == NULL)
		return -1;
	merge->count = count;
	for (uint32_t i = 0; i < count; i++)
	{
		int rc;
		if (layers[i].kind != PS_LAYER_IMAGE || layers[i].deleting ||
			layers[i].timeline != layers[0].timeline)
			goto fail;
		for (uint32_t j = 0; j < i; j++)
			if (layers[j].layer_id == layers[i].layer_id)
				goto fail;
		merge->layer_ids[i] = layers[i].layer_id;
		if (ps_image_index_cursor_open(&layers[i], &merge->cursors[i]) != 0)
			goto fail;
		rc = ps_image_index_cursor_next(merge->cursors[i], &merge->heads[i]);
		if (rc < 0)
			goto fail;
		merge->active[i] = rc == 1;
	}
	*out = merge;
	return 0;
fail:
	ps_image_index_merge_close(merge);
	return -1;
}

int
ps_image_index_merge_next(PsImageIndexMerge *merge, PsImgIndexEnt *entry,
						  uint64_t *layer_id)
{
	uint32_t best = UINT32_MAX;

	if (merge == NULL || entry == NULL || layer_id == NULL || merge->failed)
		return -1;
	for (uint32_t i = 0; i < merge->count; i++)
	{
		ImgSort a, b;
		int cmp;
		if (!merge->active[i])
			continue;
		if (best == UINT32_MAX)
		{
			best = i;
			continue;
		}
		memset(&a, 0, sizeof(a));
		memset(&b, 0, sizeof(b));
		a.ent = merge->heads[i]; b.ent = merge->heads[best];
		cmp = img_sort_cmp(&a, &b);
		if (cmp < 0 || (cmp == 0 && merge->layer_ids[i] < merge->layer_ids[best]))
			best = i;
	}
	if (best == UINT32_MAX)
		return 0;
	*entry = merge->heads[best];
	*layer_id = merge->layer_ids[best];
	{
		int rc = ps_image_index_cursor_next(merge->cursors[best], &merge->heads[best]);
		if (rc < 0)
		{
			merge->failed = 1;
			return -1;
		}
		merge->active[best] = rc == 1;
	}
	return 1;
}

typedef struct ImagePrepareSource
{
	const PsLayerDesc *layer;
	uint64_t offset;
} ImagePrepareSource;

static int
image_prepare_read_page(const PsImgRec *rec, void *page, void *arg)
{
	const ImagePrepareSource *source = rec->page;
	uint32_t page_size = *(uint32_t *) arg;

	return ps_layer_store->read_layer_block(source->layer, source->offset,
											page, page_size);
}

/* Verify the data checksum and return the end of its page-data region. */
static int
image_compaction_data_end(const PsLayerDesc *layer, uint32_t page_size,
						  uint64_t *data_end)
{
	PsLayerDesc copy = *layer;
	const PsLayerLocation *loc = img_local_loc(&copy);
	PsImgFooter foot;

	if (loc == NULL || !loc->available || loc->tier == PS_LAYER_TIER_REMOTE_OBJECT ||
		loc->size < sizeof(foot) ||
		image_index_read(&copy, -1, loc->size - sizeof(foot), &foot, sizeof(foot)) != 0 ||
		foot.magic != PS_IMG_MAGIC || foot.page_size != page_size ||
		foot.index_off > loc->size - sizeof(foot) ||
		foot.index_off % page_size != 0 ||
		ps_image_layer_verify_data(&copy, page_size) != 0)
		return -1;
	*data_end = foot.index_off;
	return 0;
}

int
ps_image_compaction_prepare(const PsLayerDesc *layers, uint32_t count,
							const uint64_t *output_ids, uint32_t capacity,
							uint32_t page_size, PsLayerDesc *outputs,
							uint32_t *noutputs, uint64_t *uncertain_id)
{
	PsImageIndexMerge *merge = NULL;
	PsImgRec *batch = NULL;
	ImagePrepareSource *sources = NULL;
	uint64_t data_ends[PS_IMAGE_MERGE_MAX_INPUTS];
	uint64_t max_id = 0;
	uint32_t written = 0, used = 0;
	PsImgIndexEnt pending;
	uint32_t pending_source = 0;
	int have_pending = 0;
	int rc = -1;

	if (noutputs == NULL || uncertain_id == NULL)
		return -1;
	*noutputs = 0;
	*uncertain_id = 0;
	if (layers == NULL || output_ids == NULL || outputs == NULL || count == 0 ||
		count > PS_IMAGE_MERGE_MAX_INPUTS || capacity == 0 ||
		capacity > PS_IMAGE_PREPARE_MAX_OUTPUTS || page_size == 0 ||
		page_size > IMAGE_STREAM_BYTES)
		return -1;
	for (uint32_t i = 0; i < count; i++)
	{
		/* Legacy shard-zero layers may hold keys for every shard.  A normal
		 * shard-local output cannot represent their routing provenance. */
		if (layers[i].legacy_shard_zero ||
			(layers[i].layer_id >> PS_IMAGE_LAYER_SHARD_SHIFT) !=
			(layers[0].layer_id >> PS_IMAGE_LAYER_SHARD_SHIFT))
			return -1;
		if (layers[i].layer_id > max_id)
			max_id = layers[i].layer_id;
	}
	for (uint32_t i = 0; i < capacity; i++)
		if (output_ids[i] <= max_id ||
			(output_ids[i] >> PS_IMAGE_LAYER_SHARD_SHIFT) !=
			(layers[0].layer_id >> PS_IMAGE_LAYER_SHARD_SHIFT) ||
			(i != 0 && output_ids[i] <= output_ids[i - 1]))
			return -1;
	/* Verify payloads as well as indexes before copying bytes. A descriptor
	 * copy avoids publishing advisory cache state from this helper. */
	for (uint32_t i = 0; i < count; i++)
		if (image_compaction_data_end(&layers[i], page_size, &data_ends[i]) != 0)
			return -1;
	if (ps_image_index_merge_open(layers, count, &merge) != 0)
		return -1;
	batch = calloc(PS_IMAGE_BATCH_MAX_RECORDS, sizeof(*batch));
	sources = calloc(PS_IMAGE_BATCH_MAX_RECORDS, sizeof(*sources));
	if (batch == NULL || sources == NULL)
		goto out;
	for (;;)
	{
		PsImgIndexEnt ent;
		uint64_t layer_id;
		uint32_t source = 0;
		int next = ps_image_index_merge_next(merge, &ent, &layer_id);

		if (next < 0)
			goto out;
		if (next == 1)
		{
			for (source = 0; source < count && layers[source].layer_id != layer_id; source++)
				;
			if (source == count || data_ends[source] < page_size ||
				ent.data_off > data_ends[source] - page_size || ent.data_off % page_size != 0 ||
				!layer_covers(&layers[source], &ent.key, ent.block) ||
				ent.lsn < layers[source].lsn_start || ent.lsn > layers[source].lsn_end)
				goto out;
		}
		if (next == 1 && (!have_pending ||
			(key_cmp(&pending.key, &ent.key) == 0 && pending.block == ent.block &&
			 pending.lsn == ent.lsn && pending.admission_seq == ent.admission_seq)))
		{
			/* The merge orders equal identities by source layer ID, and within
			 * one source by index order. The final copy is the one a read sees. */
			pending = ent;
			pending_source = source;
			have_pending = 1;
			continue;
		}
		if (have_pending)
		{
			memset(&batch[used], 0, sizeof(batch[used]));
			batch[used].key = pending.key;
			batch[used].block = pending.block;
			batch[used].lsn = pending.lsn;
			batch[used].admission_seq = pending.admission_seq;
			batch[used].growth_lsn = pending.growth_lsn;
			batch[used].order_id = pending.order_id;
			batch[used].seg_off = pending.seg_off;
			batch[used].seg_id = pending.seg_id;
			batch[used].flags = pending.flags;
			sources[used].layer = &layers[pending_source];
			sources[used].offset = pending.data_off;
			batch[used].page = &sources[used];
			used++;
		}
		have_pending = next == 1;
		if (have_pending)
		{
			pending = ent;
			pending_source = source;
		}
		if (used == PS_IMAGE_BATCH_MAX_RECORDS || (next == 0 && used != 0))
		{
			if (written == capacity)
				goto out;
			if (ps_image_layer_write_sorted_batch(output_ids[written], layers[0].timeline,
					batch, used, page_size, image_prepare_read_page, &page_size,
					&outputs[written]) != 0)
			{
				/* The writer attempts cleanup, but its API does not establish
				 * ownership after a failed create or report failed unlink. */
				*uncertain_id = output_ids[written];
				goto out;
			}
			written++;
			used = 0;
		}
		if (next == 0)
		{
			*noutputs = written;
			rc = 0;
			goto done;
		}
	}
out:
	/* Preserve descriptors for failed unlinks so the owner can reconcile
	 * them; never silently claim that ambiguous cleanup succeeded. */
	{
		uint32_t remaining = 0;
		for (uint32_t i = 0; i < written; i++)
			if (ps_layer_store->delete_local_layer(&outputs[i]) != 0)
				outputs[remaining++] = outputs[i];
		*noutputs = remaining;
		if (remaining != 0 || *uncertain_id != 0)
			rc = -2;
	}
done:
	ps_image_index_merge_close(merge);
	free(batch);
	free(sources);
	return rc;
}

static int
image_identity_equal(const PsImgIndexEnt *a, const PsImgIndexEnt *b)
{
	return key_cmp(&a->key, &b->key) == 0 && a->block == b->block &&
		a->lsn == b->lsn && a->admission_seq == b->admission_seq;
}

/* Compare durable/routing properties, not transient reader counts or cache
 * verification hints. A tier transition invalidates the captured snapshot. */
static int
image_snapshot_descriptor_equal(const PsLayerDesc *a, const PsLayerDesc *b)
{
	if (a->layer_id != b->layer_id || a->kind != b->kind ||
		a->timeline != b->timeline || key_cmp(&a->start_key, &b->start_key) != 0 ||
		key_cmp(&a->end_key, &b->end_key) != 0 || a->start_block != b->start_block ||
		a->end_block != b->end_block || a->lsn_start != b->lsn_start ||
		a->lsn_end != b->lsn_end || a->created_at_lsn != b->created_at_lsn ||
		a->remote_uploaded_lsn != b->remote_uploaded_lsn ||
		a->remote_durable != b->remote_durable || a->local_pinned != b->local_pinned ||
		a->deleting != b->deleting || a->legacy_shard_zero != b->legacy_shard_zero ||
		a->location_count != b->location_count || a->location_count > PS_LAYER_MAX_LOCATIONS)
		return 0;
	for (uint32_t i = 0; i < a->location_count; i++)
		if (a->locations[i].tier != b->locations[i].tier ||
			a->locations[i].size != b->locations[i].size ||
			a->locations[i].generation != b->locations[i].generation ||
			a->locations[i].available != b->locations[i].available ||
			strncmp(a->locations[i].uri, b->locations[i].uri, PS_LAYER_URI_MAX) != 0)
			return 0;
	return 1;
}

static int
image_compaction_snapshot_valid(const PsLayerMap *map,
								   const PsLayerDesc *inputs, uint32_t ninputs,
								   const PsLayerDesc *outputs, uint32_t noutputs,
	uint32_t retired_prefix, int retiring)
{
	uint64_t max_id = 0;
	uint32_t seen_inputs = 0, seen_outputs = 0;
	uint64_t seen_input_bits = 0, seen_output_bits = 0;

	if (map == NULL || (map->nlayers != 0 && map->layers == NULL) ||
		inputs == NULL || outputs == NULL || ninputs == 0 ||
		noutputs == 0 || ninputs > PS_IMAGE_MERGE_MAX_INPUTS ||
		noutputs > PS_IMAGE_PREPARE_MAX_OUTPUTS ||
		(retiring && retired_prefix >= ninputs))
		return 0;
	for (uint32_t i = 0; i < ninputs; i++)
	{
		if (inputs[i].kind != PS_LAYER_IMAGE || inputs[i].deleting ||
			(retiring && inputs[i].remote_durable) ||
			inputs[i].location_count == 0 || inputs[i].location_count > PS_LAYER_MAX_LOCATIONS ||
			inputs[i].legacy_shard_zero || inputs[i].timeline != inputs[0].timeline ||
			(inputs[i].layer_id >> PS_IMAGE_LAYER_SHARD_SHIFT) !=
			(inputs[0].layer_id >> PS_IMAGE_LAYER_SHARD_SHIFT))
			return 0;
		for (uint32_t j = 0; j < i; j++)
			if (inputs[i].layer_id == inputs[j].layer_id)
				return 0;
		if (inputs[i].layer_id > max_id)
			max_id = inputs[i].layer_id;
	}
	for (uint32_t i = 0; i < noutputs; i++)
		if (outputs[i].kind != PS_LAYER_IMAGE || outputs[i].deleting ||
			outputs[i].location_count == 0 || outputs[i].location_count > PS_LAYER_MAX_LOCATIONS ||
			outputs[i].legacy_shard_zero || outputs[i].timeline != inputs[0].timeline ||
			outputs[i].layer_id <= max_id ||
			(outputs[i].layer_id >> PS_IMAGE_LAYER_SHARD_SHIFT) !=
			(inputs[0].layer_id >> PS_IMAGE_LAYER_SHARD_SHIFT) ||
			(i != 0 && outputs[i].layer_id <= outputs[i - 1].layer_id))
			return 0;
	for (uint32_t i = 0; i < map->nlayers; i++)
	{
		const PsLayerDesc *current = &map->layers[i];
		int matched = 0;
		if (current->kind != PS_LAYER_IMAGE || current->timeline != inputs[0].timeline ||
			(current->layer_id >> PS_IMAGE_LAYER_SHARD_SHIFT) !=
			(inputs[0].layer_id >> PS_IMAGE_LAYER_SHARD_SHIFT))
		{
			for (uint32_t j = 0; j < noutputs; j++)
				if (current->layer_id == outputs[j].layer_id)
					return 0;
			/* Legacy shard-zero layers can cover every shard. */
			if (!current->deleting && current->kind == PS_LAYER_IMAGE &&
				current->timeline == inputs[0].timeline && current->legacy_shard_zero)
				return 0;
			continue;
		}
		for (uint32_t j = 0; j < ninputs; j++)
			if (current->layer_id == inputs[j].layer_id)
			{
				PsLayerDesc expected = inputs[j];

				expected.deleting = retiring && j < retired_prefix;
				if ((seen_input_bits & (UINT64_C(1) << j)) != 0 ||
					(retiring && __atomic_load_n(&current->cache_readers, __ATOMIC_ACQUIRE) == 0) ||
					!image_snapshot_descriptor_equal(current, &expected))
					return 0;
				seen_input_bits |= UINT64_C(1) << j;
				seen_inputs++;
				matched = 1;
				break;
			}
		if (matched)
			continue;
		for (uint32_t j = 0; j < noutputs; j++)
			if (current->layer_id == outputs[j].layer_id)
			{
				if ((seen_output_bits & (UINT64_C(1) << j)) != 0 ||
					!image_snapshot_descriptor_equal(current, &outputs[j]))
					return 0;
				seen_output_bits |= UINT64_C(1) << j;
				seen_outputs++;
				matched = 1;
				break;
			}
		if (!matched && !current->deleting)
			return 0;
	}
	return seen_inputs == ninputs && (!retiring || seen_outputs == noutputs);
}

int
ps_image_compaction_snapshot_valid(const PsLayerMap *map,
	const PsLayerDesc *inputs, uint32_t ninputs,
	const PsLayerDesc *outputs, uint32_t noutputs)
{
	return image_compaction_snapshot_valid(map, inputs, ninputs, outputs, noutputs, 0, 0);
}

int
ps_image_compaction_retirement_valid(const PsLayerMap *map,
	const PsLayerDesc *inputs, uint32_t ninputs,
	const PsLayerDesc *outputs, uint32_t noutputs,
	uint32_t retired_prefix)
{
	return image_compaction_snapshot_valid(map, inputs, ninputs, outputs, noutputs,
	retired_prefix, 1);
}

int
ps_image_compaction_verify_coverage(const PsLayerDesc *inputs, uint32_t ninputs,
										const PsLayerDesc *outputs, uint32_t noutputs,
										uint32_t page_size)
{
	PsImageIndexMerge *src = NULL, *dst = NULL;
	uint64_t src_ends[PS_IMAGE_MERGE_MAX_INPUTS];
	uint64_t dst_ends[PS_IMAGE_MERGE_MAX_INPUTS];
	unsigned char *src_page = NULL, *dst_page = NULL;
	PsImgIndexEnt pending;
	uint64_t pending_id = 0, max_id = 0;
	int have_pending = 0, rc = -1;

	if (inputs == NULL || outputs == NULL || ninputs == 0 || noutputs == 0 ||
		ninputs > PS_IMAGE_MERGE_MAX_INPUTS || noutputs > PS_IMAGE_MERGE_MAX_INPUTS ||
		page_size == 0 || page_size > IMAGE_STREAM_BYTES)
		return -1;
	for (uint32_t i = 0; i < ninputs; i++)
	{
		if (inputs[i].legacy_shard_zero ||
			(inputs[i].layer_id >> PS_IMAGE_LAYER_SHARD_SHIFT) !=
			(inputs[0].layer_id >> PS_IMAGE_LAYER_SHARD_SHIFT) ||
			image_compaction_data_end(&inputs[i], page_size, &src_ends[i]) != 0)
			return -1;
		if (inputs[i].layer_id > max_id)
			max_id = inputs[i].layer_id;
	}
	for (uint32_t i = 0; i < noutputs; i++)
		if (outputs[i].legacy_shard_zero || outputs[i].timeline != inputs[0].timeline ||
			outputs[i].layer_id <= max_id ||
			(outputs[i].layer_id >> PS_IMAGE_LAYER_SHARD_SHIFT) !=
			(inputs[0].layer_id >> PS_IMAGE_LAYER_SHARD_SHIFT) ||
			(i != 0 && outputs[i].layer_id <= outputs[i - 1].layer_id) ||
			image_compaction_data_end(&outputs[i], page_size, &dst_ends[i]) != 0)
			return -1;
	if (ps_image_index_merge_open(inputs, ninputs, &src) != 0 ||
		ps_image_index_merge_open(outputs, noutputs, &dst) != 0)
		goto out;
	src_page = malloc(page_size);
	dst_page = malloc(page_size);
	if (src_page == NULL || dst_page == NULL)
		goto out;
	for (;;)
	{
		PsImgIndexEnt ent;
		uint64_t id;
		int next = ps_image_index_merge_next(src, &ent, &id);
		uint32_t source = 0;

		if (next < 0)
			goto out;
		if (next == 1)
		{
			for (source = 0; source < ninputs && inputs[source].layer_id != id; source++)
				;
			if (source == ninputs || src_ends[source] < page_size ||
				ent.data_off > src_ends[source] - page_size || ent.data_off % page_size != 0 ||
				!layer_covers(&inputs[source], &ent.key, ent.block) ||
				ent.lsn < inputs[source].lsn_start || ent.lsn > inputs[source].lsn_end)
				goto out;
		}
		if (next == 1 && (!have_pending || image_identity_equal(&pending, &ent)))
		{
			pending = ent;
			pending_id = id;
			have_pending = 1;
			continue;
		}
		if (have_pending)
		{
			PsImgIndexEnt actual;
			uint64_t actual_id;
			uint32_t si = 0, di = 0;

			if (ps_image_index_merge_next(dst, &actual, &actual_id) != 1 ||
				!image_identity_equal(&pending, &actual) ||
				pending.growth_lsn != actual.growth_lsn || pending.order_id != actual.order_id ||
				pending.seg_off != actual.seg_off || pending.seg_id != actual.seg_id ||
				pending.flags != actual.flags)
				goto out;
			for (si = 0; si < ninputs && inputs[si].layer_id != pending_id; si++)
				;
			for (di = 0; di < noutputs && outputs[di].layer_id != actual_id; di++)
				;
			if (si == ninputs || di == noutputs || dst_ends[di] < page_size ||
				actual.data_off > dst_ends[di] - page_size || actual.data_off % page_size != 0 ||
				!layer_covers(&outputs[di], &actual.key, actual.block) ||
				actual.lsn < outputs[di].lsn_start || actual.lsn > outputs[di].lsn_end ||
				ps_layer_store->read_layer_block(&inputs[si], pending.data_off, src_page, page_size) != 0 ||
				ps_layer_store->read_layer_block(&outputs[di], actual.data_off, dst_page, page_size) != 0 ||
				memcmp(src_page, dst_page, page_size) != 0)
				goto out;
		}
		have_pending = next == 1;
		if (have_pending)
		{
			pending = ent;
			pending_id = id;
		}
		else
		{
			PsImgIndexEnt extra;
			uint64_t id_out;
			if (ps_image_index_merge_next(dst, &extra, &id_out) != 0)
				goto out;
			rc = 0;
			break;
		}
	}
out:
	ps_image_index_merge_close(src);
	ps_image_index_merge_close(dst);
	free(src_page);
	free(dst_page);
	return rc;
}

static int
image_read_index_impl(const PsLayerDesc *layer, int fd, PsImgIndexEnt **out,
						  uint32_t *n)
{
	const PsLayerLocation *loc = img_local_loc(layer);
	PsImgFooter foot;
	PsImgIndexEnt *idx = NULL;
	void	   *disk = NULL;
	size_t		ent_size;
	uint64_t	idx_bytes;

	*out = NULL;
	*n = 0;
	if (!loc || loc->size < sizeof(PsImgFooter))
		return -1;
	if (image_index_read(layer, fd, loc->size - sizeof(foot),
									 &foot, sizeof(foot)) != 0)
		return -1;
	if (foot.magic != PS_IMG_MAGIC ||
		(foot.version != 2 && foot.version != 3 &&
		 foot.version != PS_IMG_VERSION) || foot.nrecs == 0)
		return -1;
	ent_size = foot.version == 2 ? sizeof(PsImgIndexEntV2) :
		foot.version == 3 ? sizeof(PsImgIndexEntV3) : sizeof(PsImgIndexEnt);
	idx_bytes = (uint64_t) foot.nrecs * ent_size;
	if (loc->size < sizeof(foot) || foot.index_off > loc->size - sizeof(foot) ||
		idx_bytes != loc->size - sizeof(foot) - foot.index_off ||
		idx_bytes > UINT32_MAX)
		return -1;
	idx = malloc((size_t) foot.nrecs * sizeof(PsImgIndexEnt));
	/* The current on-disk entries already have the in-memory layout.  Read
	 * them directly into the result; older formats still need conversion. */
	disk = foot.version == PS_IMG_VERSION ? idx : malloc((size_t) idx_bytes);
	if (!disk || !idx)
	{
		if (disk != idx)
			free(disk);
		free(idx);
		return -1;
	}
	if (image_index_read(layer, fd, foot.index_off, disk,
										 (uint32_t) idx_bytes) != 0 ||
		img_crc(disk, (size_t) idx_bytes) != foot.index_crc)
	{
		if (disk != idx)
			free(disk);
		free(idx);
		return -1;
	}
	if (foot.version == 2)
	{
		PsImgIndexEntV2 *old = disk;

		for (uint32_t i = 0; i < foot.nrecs; i++)
		{
			memset(&idx[i], 0, sizeof(idx[i]));
			idx[i].key = old[i].key;
			idx[i].block = old[i].block;
			idx[i].lsn = old[i].lsn;
			idx[i].data_off = old[i].data_off;
		}
	}
	else if (foot.version == 3)
	{
		PsImgIndexEntV3 *old = disk;

		for (uint32_t i = 0; i < foot.nrecs; i++)
		{
			memset(&idx[i], 0, sizeof(idx[i]));
			idx[i].key = old[i].key;
			idx[i].block = old[i].block;
			idx[i].lsn = old[i].lsn;
			idx[i].data_off = old[i].data_off;
			idx[i].growth_lsn = old[i].growth_lsn;
			idx[i].order_id = old[i].order_id;
			idx[i].seg_off = old[i].seg_off;
			idx[i].seg_id = old[i].seg_id;
			idx[i].flags = old[i].flags;
		}
	}
	if (disk != idx)
		free(disk);
	*out = idx;
	*n = foot.nrecs;
	return 0;
}

int
ps_image_layer_read_index(const PsLayerDesc *layer, PsImgIndexEnt **out, uint32_t *n)
{
	return image_read_index_impl(layer, -1, out, n);
}

int
ps_image_layer_read_index_fd(const PsLayerDesc *layer, int fd, PsImgIndexEnt **out, uint32_t *n)
{
	return image_read_index_impl(layer, fd, out, n);
}

static int
verify_layer_data(const PsLayerDesc *layer, uint64_t bytes, uint32_t expected)
{
	unsigned char *buffer = malloc(IMAGE_STREAM_BYTES);
	uint32_t	crc = 2166136261u;
	int			rc = -1;

	if (buffer == NULL)
		return -1;
	for (uint64_t offset = 0; offset < bytes;)
	{
		uint32_t	len = bytes - offset > IMAGE_STREAM_BYTES ?
			IMAGE_STREAM_BYTES : (uint32_t) (bytes - offset);

		if (ps_layer_store->read_layer_block(layer, offset, buffer, len) != 0)
			goto out;
		crc = img_crc_extend(crc, buffer, len);
		offset += len;
	}
	rc = crc == expected ? 0 : -1;
out:
	free(buffer);
	return rc;
}

/* Verify data through one already-open immutable file, without reopening its
 * pathname between chunks.  Its caller must check that the file did not change. */
int
ps_image_layer_verify_fd(int fd, uint64_t size, uint32_t page_size)
{
	PsImgFooter foot;
	unsigned char *buffer;
	uint32_t crc = 2166136261u;
	uint64_t offset = 0;

	if (size < sizeof(foot) || size > INT64_MAX ||
		image_fd_read(fd, size - sizeof(foot), &foot, sizeof(foot)) != 0 ||
		foot.magic != PS_IMG_MAGIC ||
		(foot.version != 2 && foot.version != 3 && foot.version != PS_IMG_VERSION) ||
		foot.page_size != page_size || foot.index_off > size - sizeof(foot))
		return -1;
	buffer = malloc(IMAGE_STREAM_BYTES);
	if (!buffer)
		return -1;
	while (offset < foot.index_off)
	{
		uint32_t len = foot.index_off - offset > IMAGE_STREAM_BYTES ?
			IMAGE_STREAM_BYTES : (uint32_t) (foot.index_off - offset);
		if (image_fd_read(fd, offset, buffer, len) != 0)
		{
			free(buffer);
			return -1;
		}
		crc = img_crc_extend(crc, buffer, len);
		offset += len;
	}
	free(buffer);
	return crc == foot.data_crc ? 0 : -1;
}

int
ps_image_layer_verify_data(const PsLayerDesc *layer, uint32_t page_size)
{
	const PsLayerLocation *loc = img_local_loc(layer);
	PsImgFooter foot;

	((PsLayerDesc *) layer)->data_verified = false;
	if (!loc || loc->size < sizeof(foot) ||
		ps_layer_store->read_layer_block(layer, loc->size - sizeof(foot),
										 &foot, sizeof(foot)) != 0 ||
		foot.magic != PS_IMG_MAGIC ||
		(foot.version != 2 && foot.version != 3 && foot.version != PS_IMG_VERSION) ||
		foot.page_size != page_size || foot.index_off > loc->size - sizeof(foot))
		return -1;
	if (verify_layer_data(layer, foot.index_off, foot.data_crc) != 0)
		return -1;
	((PsLayerDesc *) layer)->data_verified = true;
	return 0;
}

int
ps_delta_layer_verify_data(const PsLayerDesc *layer)
{
	const PsLayerLocation *loc = img_local_loc(layer);
	PsDeltaFooter foot;
	void *idx;
	uint64_t idx_bytes;
	int rc = -1;

	if (!loc || loc->size < sizeof(foot) ||
		ps_layer_store->read_layer_block(layer, loc->size - sizeof(foot),
								 &foot, sizeof(foot)) != 0 ||
		foot.magic != PS_DELTA_MAGIC || foot.version != PS_DELTA_VERSION ||
		foot.index_off > loc->size - sizeof(foot))
		return -1;
	idx_bytes = (uint64_t) foot.nrecs * sizeof(PsDeltaIndexEnt);
	if (idx_bytes > UINT32_MAX ||
		idx_bytes > loc->size - sizeof(foot) - foot.index_off)
		return -1;
	idx = malloc((size_t) idx_bytes);
	if (!idx || ps_layer_store->read_layer_block(layer, foot.index_off, idx,
								(uint32_t) idx_bytes) != 0 ||
		img_crc(idx, (size_t) idx_bytes) != foot.index_crc)
	{
		free(idx);
		return -1;
	}
	free(idx);
	rc = verify_layer_data(layer, foot.index_off, foot.data_crc);
	return rc;
}

/* ===================== image index cache =============================== */

/*
 * An image layer is immutable, so its index section needs to be read and
 * checksummed once, not on every page lookup: the index of a layer holding a
 * few hundred MB is several MB, and a single floor computation or read plan
 * looks a page up in every candidate layer.  Entries are keyed by the layer id
 * and the footer fields that certify the index bytes (a lookup still reads the
 * footer), so a layer file replaced by different content misses.  Only the
 * index is cached: whether the data section of a local copy was verified is
 * a fact about physical bytes that only the layer's owner can track.  The
 * layer store empties the cache when it closes, so a reopened store verifies
 * everything it reads again.
 */
#define PS_IMG_IDX_CACHE_SLOTS	128
#define PS_IMG_IDX_CACHE_BYTES	((size_t) 256 << 20)
#define PS_IMG_IDX_LARGE_BYTES	((size_t) 64 << 10)

typedef struct ImgIdxCacheEnt
{
	uint64_t	layer_id;
	uint64_t	file_size;
	PsImgFooter foot;
	PsImgIndexEnt *idx;
	size_t		bytes;
	uint64_t	last_use;
	uint32_t	refs;
	int			sorted;			/* (key, block) ascending: binary search is valid */
	int			cached;			/* in the table; else freed by the last release */
} ImgIdxCacheEnt;

static pthread_mutex_t img_idx_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static ImgIdxCacheEnt *img_idx_cache[PS_IMG_IDX_CACHE_SLOTS];
static size_t img_idx_cache_bytes;
static uint64_t img_idx_cache_clock;

static int
img_key_block_cmp(const PsKey *ka, uint32_t ba, const PsKey *kb, uint32_t bb)
{
	int			c = key_cmp(ka, kb);

	if (c)
		return c;
	return ba < bb ? -1 : (ba > bb ? 1 : 0);
}

static int
img_idx_cache_matches(const ImgIdxCacheEnt *e, const PsLayerDesc *layer,
					  uint64_t file_size, const PsImgFooter *foot)
{
	return e->layer_id == layer->layer_id && e->file_size == file_size &&
		memcmp(&e->foot, foot, sizeof(*foot)) == 0;
}

static void
img_idx_cache_release(ImgIdxCacheEnt *e)
{
	int			drop;

	pthread_mutex_lock(&img_idx_cache_lock);
	drop = --e->refs == 0 && !e->cached;
	pthread_mutex_unlock(&img_idx_cache_lock);
	if (drop)
	{
		free(e->idx);
		free(e);
	}
}

/* Drop every entry; one still referenced is freed by its last release. */
void
ps_image_layer_cache_reset(void)
{
	pthread_mutex_lock(&img_idx_cache_lock);
	for (int i = 0; i < PS_IMG_IDX_CACHE_SLOTS; i++)
	{
		ImgIdxCacheEnt *e = img_idx_cache[i];

		if (e == NULL)
			continue;
		img_idx_cache[i] = NULL;
		e->cached = 0;
		if (e->refs == 0)
		{
			free(e->idx);
			free(e);
		}
	}
	img_idx_cache_bytes = 0;
	pthread_mutex_unlock(&img_idx_cache_lock);
}

/* Make room for 'bytes' more; entries in use are never evicted. */
static int
img_idx_cache_reserve_locked(size_t bytes)
{
	for (;;)
	{
		int			free_slot = -1;
		int			victim = -1;

		for (int i = 0; i < PS_IMG_IDX_CACHE_SLOTS; i++)
		{
			ImgIdxCacheEnt *entry = img_idx_cache[i];
			bool prefer_small;

			if (entry == NULL)
			{
				free_slot = i;
				continue;
			}
			if (entry->refs != 0)
				continue;
			/* Under slot pressure, retain expensive indexes while cheap
			 * small indexes cycle.  Under byte pressure use ordinary LRU:
			 * keeping a big entry could otherwise evict every small one. */
			prefer_small = img_idx_cache_bytes + bytes <= PS_IMG_IDX_CACHE_BYTES;
			if (victim < 0 ||
				(prefer_small && entry->bytes < PS_IMG_IDX_LARGE_BYTES &&
				 img_idx_cache[victim]->bytes >= PS_IMG_IDX_LARGE_BYTES) ||
				((!prefer_small ||
				  (entry->bytes < PS_IMG_IDX_LARGE_BYTES) ==
				  (img_idx_cache[victim]->bytes < PS_IMG_IDX_LARGE_BYTES)) &&
				 entry->last_use < img_idx_cache[victim]->last_use))
				victim = i;
		}
		if (free_slot >= 0 &&
			img_idx_cache_bytes + bytes <= PS_IMG_IDX_CACHE_BYTES)
			return free_slot;
		if (victim < 0)
			return -1;
		img_idx_cache_bytes -= img_idx_cache[victim]->bytes;
		free(img_idx_cache[victim]->idx);
		free(img_idx_cache[victim]);
		img_idx_cache[victim] = NULL;
	}
}

/* Return the verified index of 'layer', whose footer the caller just read. */
static ImgIdxCacheEnt *
img_idx_cache_get(const PsLayerDesc *layer, uint64_t file_size,
				  const PsImgFooter *foot)
{
	ImgIdxCacheEnt *e;
	PsImgIndexEnt *idx;
	uint32_t	nidx;
	int			slot;

	pthread_mutex_lock(&img_idx_cache_lock);
	for (int i = 0; i < PS_IMG_IDX_CACHE_SLOTS; i++)
		if (img_idx_cache[i] != NULL &&
			img_idx_cache_matches(img_idx_cache[i], layer, file_size, foot))
		{
			e = img_idx_cache[i];
			e->refs++;
			e->last_use = ++img_idx_cache_clock;
			pthread_mutex_unlock(&img_idx_cache_lock);
			return e;
		}
	pthread_mutex_unlock(&img_idx_cache_lock);

	/* read and checksum outside the lock; a racing loader only costs a copy */
	if (ps_image_layer_read_index(layer, &idx, &nidx) != 0)
		return NULL;
	if (nidx != foot->nrecs || (e = calloc(1, sizeof(*e))) == NULL)
	{
		free(idx);
		return NULL;
	}
	e->layer_id = layer->layer_id;
	e->file_size = file_size;
	e->foot = *foot;
	e->idx = idx;
	e->bytes = (size_t) nidx * sizeof(PsImgIndexEnt);
	e->refs = 1;
	e->sorted = 1;
	for (uint32_t i = 1; i < nidx; i++)
		if (img_key_block_cmp(&idx[i - 1].key, idx[i - 1].block,
							  &idx[i].key, idx[i].block) > 0)
		{
			e->sorted = 0;
			break;
		}

	pthread_mutex_lock(&img_idx_cache_lock);
	for (int i = 0; i < PS_IMG_IDX_CACHE_SLOTS; i++)
		if (img_idx_cache[i] != NULL &&
			img_idx_cache_matches(img_idx_cache[i], layer, file_size, foot))
		{
			pthread_mutex_unlock(&img_idx_cache_lock);
			return e;			/* lost the race: use ours uncached */
		}
	if (e->bytes <= PS_IMG_IDX_CACHE_BYTES &&
		(slot = img_idx_cache_reserve_locked(e->bytes)) >= 0)
	{
		e->cached = 1;
		e->last_use = ++img_idx_cache_clock;
		img_idx_cache[slot] = e;
		img_idx_cache_bytes += e->bytes;
	}
	pthread_mutex_unlock(&img_idx_cache_lock);
	return e;
}

static int
image_layer_lookup_impl(const PsLayerDesc *layer, const PsKey *key,
					  uint32_t block, uint64_t read_lsn, uint64_t read_seq,
					  void *out, uint32_t page_size, uint64_t *out_lsn,
					  uint64_t *out_seq, int exact)
{
	const PsLayerLocation *loc = img_local_loc(layer);
	PsImgFooter foot;
	ImgIdxCacheEnt *cached = NULL;
	const PsImgIndexEnt *idx;
	uint32_t	first = 0;
	uint64_t	best_off = 0;
	uint64_t	best_lsn = 0;
	uint64_t	best_seq = 0;
	int			found = 0;
	int			rc = -1;
	int			retried = 0;

	/* prune: skip without any IO if this layer cannot hold (key,block) or has no
	 * version at/below read_lsn */
	if (!layer_covers(layer, key, block) || read_lsn < layer->lsn_start)
		return 0;

retry:
	first = 0;					/* computed from the entry read below */
	if (!loc || loc->size < sizeof(PsImgFooter))
		return -1;
	if (ps_layer_store->read_layer_block(layer, loc->size - sizeof(foot),
									 &foot, sizeof(foot)) != 0)
		goto refresh;
	if (foot.magic != PS_IMG_MAGIC ||
		(foot.version != 2 && foot.version != 3 &&
		 foot.version != PS_IMG_VERSION) ||
		foot.page_size != page_size || foot.nrecs == 0)
		goto refresh;
	cached = img_idx_cache_get(layer, loc->size, &foot);
	if (cached == NULL)
		goto refresh;
	idx = cached->idx;

	/*
	 * Verify the data section once (per process) before the first page is
	 * served from this layer: the writer stored foot.data_crc, and a layer
	 * whose data bytes rotted while footer+index stayed readable must fail
	 * here rather than serve -- or let compaction propagate -- bad pages.
	 */
	if (!((PsLayerDesc *) layer)->data_verified)
	{
		if (ps_image_layer_verify_data(layer, page_size) != 0)
		{
			goto refresh;
		}
	}

	/* the writer sorts by (key, block, ...): start at the first candidate */
	if (cached->sorted)
	{
		uint32_t	hi = foot.nrecs;

		while (first < hi)
		{
			uint32_t	mid = first + (hi - first) / 2;

			if (img_key_block_cmp(&idx[mid].key, idx[mid].block, key, block) < 0)
				first = mid + 1;
			else
				hi = mid;
		}
	}

	/* newest version of (key, block) with lsn <= read_lsn */
	for (uint32_t i = first; i < foot.nrecs; i++)
	{
		if (idx[i].block != block || key_cmp(&idx[i].key, key) != 0)
		{
			if (cached->sorted)
				break;
			continue;
		}
		if ((exact ? (idx[i].lsn == read_lsn &&
				  idx[i].admission_seq == read_seq) :
			 (idx[i].lsn <= read_lsn &&
			  (idx[i].lsn < read_lsn || read_seq == 0 ||
			   idx[i].admission_seq == 0 ||
			   idx[i].admission_seq <= read_seq))) &&
			(!found || idx[i].lsn > best_lsn ||
			 (idx[i].lsn == best_lsn && idx[i].admission_seq >= best_seq)))
		{
			best_lsn = idx[i].lsn;
			best_seq = idx[i].admission_seq;
			best_off = idx[i].data_off;
			found = 1;
		}
	}
	if (!found)
	{
		rc = 0;
		goto out;
	}
	if (ps_layer_store->read_layer_block(layer, best_off, out, page_size) != 0)
		goto refresh;
	if (out_lsn)
		*out_lsn = best_lsn;
	if (out_seq)
		*out_seq = best_seq;
	rc = 1;
	goto out;

refresh:
	if (cached != NULL)
		img_idx_cache_release(cached);
	cached = NULL;
	if (!retried && ps_layer_store->refresh_layer_cache != NULL &&
		ps_layer_store->refresh_layer_cache(layer) == 0)
	{
		retried = 1;
		goto retry;
	}

out:
	if (cached != NULL)
		img_idx_cache_release(cached);
	return rc;
}

/* Exact identity lookup includes legacy sequence zero; it is never a wildcard. */
int
ps_image_layer_lookup_exact(const PsLayerDesc *layer, const PsKey *key,
		uint32_t block, uint64_t lsn, uint64_t seq, void *out,
		uint32_t page_size, uint64_t *out_lsn, uint64_t *out_seq)
{
	return image_layer_lookup_impl(layer, key, block, lsn, seq, out,
			page_size, out_lsn, out_seq, 1);
}

int
ps_image_layer_lookup(const PsLayerDesc *layer, const PsKey *key,
		uint32_t block, uint64_t lsn, uint64_t seq, void *out,
		uint32_t page_size, uint64_t *out_lsn, uint64_t *out_seq)
{
	return image_layer_lookup_impl(layer, key, block, lsn, seq, out,
			page_size, out_lsn, out_seq, 0);
}
