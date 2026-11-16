#include <stdlib.h>
#include <string.h>

#include "pagestore_compaction_prune.h"
#include "pagestore_artifact_format.h"

typedef struct CoverageLayerRef
{
	uint64_t id;
	uint32_t index;
} CoverageLayerRef;

static int
coverage_layer_ref_cmp(const void *a, const void *b)
{
	uint64_t x = ((const CoverageLayerRef *) a)->id;
	uint64_t y = ((const CoverageLayerRef *) b)->id;

	return x < y ? -1 : x > y;
}

typedef struct CoverageIdentity
{
	PsImgIndexEnt identity;
	uint32_t original;
	unsigned char seen_selected, seen_excluded;
	uint64_t selected_max, excluded_max;
} CoverageIdentity;

struct PsCompactionCoverage
{
	PsLayerDesc *layers;
	CoverageLayerRef *layer_refs;
	uint32_t nlayers, current, nidentities;
	unsigned char *selected;
	unsigned char *source_seen;
	CoverageIdentity *identities;
	uint64_t *control_lsns;
	PsKey control_key;
	PsImageIndexCursor *cursor;
	PsCompactionPruneFence fence;
	int bound, started;
	int state; /* 0 scanning, 1 complete, -1 failed */
};

static int
coverage_identity_cmp(const void *va, const void *vb)
{
	const PsImgIndexEnt *a = &((const CoverageIdentity *) va)->identity;
	const PsImgIndexEnt *b = &((const CoverageIdentity *) vb)->identity;

#define CMP_FIELD(f) do { if (a->f != b->f) return a->f < b->f ? -1 : 1; } while (0)
	CMP_FIELD(key.spcOid);
	CMP_FIELD(key.dbOid);
	CMP_FIELD(key.relNumber);
	CMP_FIELD(key.forkNum);
	CMP_FIELD(key.klass);
	CMP_FIELD(block);
	CMP_FIELD(lsn);
	CMP_FIELD(admission_seq);
#undef CMP_FIELD
	return 0;
}

static int
coverage_lsn_cmp(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *) a, y = *(const uint64_t *) b;

	return x < y ? -1 : x > y;
}

static int
coverage_key_equal(const PsKey *a, const PsKey *b)
{
	return a->spcOid == b->spcOid && a->dbOid == b->dbOid &&
		a->relNumber == b->relNumber && a->forkNum == b->forkNum &&
		a->klass == b->klass;
}

void
ps_compaction_coverage_close(PsCompactionCoverage *scan)
{
	if (!scan)
		return;
	ps_image_index_cursor_close(scan->cursor);
	free(scan->layers);
	free(scan->layer_refs);
	free(scan->selected);
	free(scan->source_seen);
	free(scan->identities);
	free(scan->control_lsns);
	free(scan);
}

int
ps_compaction_coverage_require_control_groups(PsCompactionCoverage *scan)
{
	uint64_t *lsns;
	PsKey key;

	if (!scan || scan->started || scan->control_lsns || scan->state != 0 ||
		(scan->nidentities != 0 && sizeof(*lsns) > SIZE_MAX / (size_t) scan->nidentities))
		return -1;
	key = scan->identities[0].identity.key;
	if (key.klass != PS_KLASS_CONTROL)
		return -1;
	lsns = malloc((size_t) scan->nidentities * sizeof(*lsns));
	if (!lsns)
		return -1;
	for (uint32_t i = 0; i < scan->nidentities; i++)
	{
		const PsImgIndexEnt *e = &scan->identities[i].identity;

		if (!coverage_key_equal(&key, &e->key) || e->block >= PS_CONTROL_PAIRED_BLOCKS)
		{
			free(lsns);
			return -1;
		}
		lsns[i] = e->lsn;
	}
	qsort(lsns, scan->nidentities, sizeof(*lsns), coverage_lsn_cmp);
	scan->control_key = key;
	scan->control_lsns = lsns;
	return 0;
}

int
ps_compaction_coverage_open(const PsLayerDesc *layers, uint32_t nlayers,
	const unsigned char *selected, const PsImgIndexEnt *identities, uint32_t nidentities,
	PsCompactionCoverage **out)
{
	PsCompactionCoverage *scan = NULL;
	int rc = -1;

	if (!out)
		return -1;
	*out = NULL;
	if (!layers || !selected || !identities || nlayers == 0 || nidentities == 0 ||
		sizeof(*scan->layers) > SIZE_MAX / (size_t) nlayers ||
		sizeof(*scan->layer_refs) > SIZE_MAX / (size_t) nlayers ||
		sizeof(*scan->identities) > SIZE_MAX / (size_t) nidentities)
		return -1;
	scan = calloc(1, sizeof(*scan));
	if (!scan)
		goto done;
	scan->layers = malloc((size_t) nlayers * sizeof(*scan->layers));
	scan->layer_refs = malloc((size_t) nlayers * sizeof(*scan->layer_refs));
	scan->selected = malloc(nlayers);
	scan->source_seen = calloc(nlayers, 1);
	scan->identities = calloc(nidentities, sizeof(*scan->identities));
	if (!scan->layers || !scan->layer_refs || !scan->selected || !scan->source_seen || !scan->identities)
		goto done;
	for (uint32_t i = 0; i < nlayers; i++)
	{
		if (selected[i] > 1 || layers[i].kind != PS_LAYER_IMAGE ||
			layers[i].deleting || layers[i].timeline != layers[0].timeline ||
			(layers[i].layer_id >> PS_IMAGE_LAYER_SHARD_SHIFT) !=
			(layers[0].layer_id >> PS_IMAGE_LAYER_SHARD_SHIFT))
			goto done;
		scan->layer_refs[i].id = layers[i].layer_id;
		scan->layer_refs[i].index = i;
	}
	qsort(scan->layer_refs, nlayers, sizeof(*scan->layer_refs), coverage_layer_ref_cmp);
	for (uint32_t i = 1; i < nlayers; i++)
		if (scan->layer_refs[i].id == scan->layer_refs[i - 1].id)
			goto done;
	for (uint32_t i = 0; i < nidentities; i++)
	{
		scan->identities[i].identity = identities[i];
		scan->identities[i].original = i;
	}
	qsort(scan->identities, nidentities, sizeof(*scan->identities), coverage_identity_cmp);
	for (uint32_t i = 1; i < nidentities; i++)
		if (coverage_identity_cmp(&scan->identities[i - 1], &scan->identities[i]) == 0)
			goto done;
	memcpy(scan->selected, selected, nlayers);
	memcpy(scan->layers, layers, (size_t) nlayers * sizeof(*layers));
	scan->nlayers = nlayers;
	scan->nidentities = nidentities;
	*out = scan;
	scan = NULL;
	rc = 0;
done:
	ps_compaction_coverage_close(scan);
	return rc;
}

int
ps_compaction_coverage_step(PsCompactionCoverage *scan, uint32_t max_work)
{
	if (!scan || scan->state < 0)
		return -1;
	scan->started = 1;
	if (max_work == 0)
		goto fail;
	if (scan->state == 1)
		return 0;
	while (max_work-- != 0)
	{
		PsImgIndexEnt e;
		CoverageIdentity key = {0}, *match;
		int next;

		if (!scan->cursor)
		{
			if (scan->control_lsns)
			{
				PsLayerDesc range = {0};
				const PsLayerDesc *layer = &scan->layers[scan->current];

				range.start_key = range.end_key = scan->control_key;
				range.start_block = 0;
				range.end_block = PS_CONTROL_PAIRED_BLOCKS - 1;
				if (ps_image_layer_ranges_disjoint(layer, &range) ||
					(layer->lsn_start <= layer->lsn_end &&
					 (layer->lsn_end < scan->control_lsns[0] ||
					  layer->lsn_start > scan->control_lsns[scan->nidentities - 1])))
				{
					if (++scan->current == scan->nlayers)
					{
						scan->state = 1;
						return 0;
					}
					continue;
				}
			}
			if (ps_image_index_cursor_open_staged(&scan->layers[scan->current], &scan->cursor) != 0)
				goto fail;
			continue;
		}
		next = ps_image_index_cursor_next(scan->cursor, &e);
		if (next < 0)
			goto fail;
		if (next == 0)
		{
			ps_image_index_cursor_close(scan->cursor);
			scan->cursor = NULL;
			if (++scan->current == scan->nlayers)
			{
				scan->state = 1;
				return 0;
			}
			continue;
		}
		key.identity = e;
		match = bsearch(&key, scan->identities, scan->nidentities,
						sizeof(*scan->identities), coverage_identity_cmp);
		if (!match && scan->control_lsns && e.block < PS_CONTROL_PAIRED_BLOCKS &&
			coverage_key_equal(&e.key, &scan->control_key) &&
			bsearch(&e.lsn, scan->control_lsns, scan->nidentities,
					sizeof(*scan->control_lsns), coverage_lsn_cmp))
			goto fail;
		if (match)
		{
			scan->source_seen[scan->current] = 1;
			if (scan->selected[scan->current])
			{
				match->seen_selected = 1;
				if (scan->layers[scan->current].layer_id > match->selected_max)
					match->selected_max = scan->layers[scan->current].layer_id;
			}
			else
			{
				match->seen_excluded = 1;
				if (scan->layers[scan->current].layer_id > match->excluded_max)
					match->excluded_max = scan->layers[scan->current].layer_id;
			}
		}
	}
	return 1;
fail:
	ps_image_index_cursor_close(scan->cursor);
	scan->cursor = NULL;
	scan->state = -1;
	return -1;
}

int
ps_compaction_coverage_select_found_sources(PsCompactionCoverage *scan,
	uint32_t extra_index, uint32_t max_sources, unsigned char *selected, uint32_t count)
{
	uint32_t n = 0;

	if (!scan || scan->state != 1 || !selected ||
		count != scan->nlayers || (extra_index != UINT32_MAX && extra_index >= count) || max_sources == 0)
		return -1;
	if (extra_index == UINT32_MAX)
	{
		uint64_t smallest = UINT64_MAX;

		for (uint32_t i = 0; i < count; i++)
		{
			const PsLayerDesc *d = &scan->layers[i];

			if (scan->source_seen[i])
				continue;
			for (uint32_t j = 0; j < d->location_count && j < PS_LAYER_MAX_LOCATIONS; j++)
				if (d->locations[j].available &&
					(d->locations[j].tier == PS_LAYER_TIER_LOCAL_HOT ||
					 d->locations[j].tier == PS_LAYER_TIER_LOCAL_COLD) &&
					d->locations[j].size <= smallest)
				{
					smallest = d->locations[j].size;
					extra_index = i;
				}
		}
	}
	for (uint32_t i = 0; i < count; i++)
	{
		if (scan->selected[i])
			return -1;
		if (scan->source_seen[i] || i == extra_index)
			n++;
	}
	if (n > max_sources)
		return 0;
	for (uint32_t i = 0; i < scan->nidentities; i++)
		if (!scan->identities[i].seen_excluded || scan->identities[i].seen_selected)
			return 0;
	for (uint32_t i = 0; i < count; i++)
		selected[i] = scan->selected[i] = scan->source_seen[i] || i == extra_index;
	for (uint32_t i = 0; i < scan->nidentities; i++)
	{
		scan->identities[i].seen_selected = 1;
		scan->identities[i].seen_excluded = 0;
	}
	return (int) n;
}

int
ps_compaction_coverage_selected_winners_valid(const PsCompactionCoverage *scan,
	const PsImageGroupEntry *selected, uint32_t count)
{
	if (!scan || scan->state != 1 || !selected || count != scan->nidentities)
		return 0;
	for (uint32_t i = 0; i < count; i++)
	{
		const CoverageIdentity *identity = &scan->identities[i];
		const PsImageGroupEntry *entry = &selected[identity->original];
		CoverageIdentity key = {0};

		key.identity = entry->entry;
		if (coverage_identity_cmp(identity, &key) != 0 || !identity->seen_selected ||
			entry->layer_id != identity->selected_max ||
			(identity->seen_excluded && identity->excluded_max > identity->selected_max))
			return 0;
	}
	return 1;
}

int
ps_compaction_coverage_results(const PsCompactionCoverage *scan,
	unsigned char *covered, uint32_t count)
{
	if (!scan || scan->state != 1 || !covered || count != scan->nidentities)
		return -1;
	for (uint32_t i = 0; i < count; i++)
		covered[scan->identities[i].original] = scan->identities[i].seen_selected &&
			!scan->identities[i].seen_excluded;
	return 0;
}

/* Call while holding the map lock. All supplied descriptors must be pinned;
 * the nonzero reader count is necessary, but does not prove caller ownership. */
static int
coverage_map_matches(const PsCompactionCoverage *scan, const PsLayerMap *map,
	const PsLayerDesc *outputs, uint32_t noutputs, uint32_t published)
{
	unsigned char *seen;
	uint32_t count = 0;
	int ok = 0;
	uint32_t timeline = scan->layers[0].timeline;
	uint64_t shard = scan->layers[0].layer_id >> PS_IMAGE_LAYER_SHARD_SHIFT;
	uint64_t seen_outputs = 0;

	if (!map || (map->nlayers != 0 && !map->layers) ||
		noutputs > PS_IMAGE_PREPARE_MAX_OUTPUTS || published > noutputs ||
		(noutputs && !outputs))
		return 0;
	for (uint32_t i = 0; i < noutputs; i++)
	{
		CoverageLayerRef key = {0};

		key.id = outputs[i].layer_id;
		if (outputs[i].kind != PS_LAYER_IMAGE || outputs[i].deleting ||
			outputs[i].remote_durable || outputs[i].legacy_shard_zero ||
			outputs[i].timeline != timeline || (outputs[i].layer_id >> PS_IMAGE_LAYER_SHARD_SHIFT) != shard ||
			outputs[i].layer_id <= scan->layer_refs[scan->nlayers - 1].id ||
			(i && outputs[i].layer_id <= outputs[i - 1].layer_id) ||
			bsearch(&key, scan->layer_refs, scan->nlayers,
				sizeof(*scan->layer_refs), coverage_layer_ref_cmp))
			return 0;
	}
	seen = calloc(scan->nlayers, 1);
	if (!seen)
		return 0;
	for (uint32_t i = 0; i < map->nlayers; i++)
	{
		const PsLayerDesc *d = &map->layers[i];
		CoverageLayerRef key = {0}, *match;
		int output = -1;

		for (uint32_t j = 0; j < noutputs; j++)
			if (d->layer_id == outputs[j].layer_id)
			{
				output = (int) j;
				break;
			}
		if (output >= 0)
		{
			uint64_t bit = UINT64_C(1) << output;

			if ((uint32_t) output >= published || (seen_outputs & bit) != 0 ||
				!ps_image_snapshot_descriptor_equal(d, &outputs[output]))
				goto done;
			seen_outputs |= bit;
			continue;
		}

		if (d->timeline != timeline)
			continue;
		if ((d->layer_id >> PS_IMAGE_LAYER_SHARD_SHIFT) != shard)
		{
			if (!d->deleting && d->legacy_shard_zero)
				goto done;
			continue;
		}
		key.id = d->layer_id;
		match = bsearch(&key, scan->layer_refs, scan->nlayers,
						sizeof(*scan->layer_refs), coverage_layer_ref_cmp);
		if (d->deleting && !match)
			continue;
		if (d->legacy_shard_zero || d->kind != PS_LAYER_IMAGE || d->deleting)
			goto done;
		if (!match || seen[match->index] ||
			!ps_image_snapshot_descriptor_equal(d, &scan->layers[match->index]) ||
			__atomic_load_n(&d->cache_readers, __ATOMIC_ACQUIRE) == 0)
			goto done;
		seen[match->index] = 1;
		count++;
	}
	ok = count == scan->nlayers &&
		(published == 64 ? seen_outputs == UINT64_MAX :
		 seen_outputs == ((UINT64_C(1) << published) - 1));
done:
	free(seen);
	return ok;
}

int
ps_compaction_coverage_bind_snapshot(PsCompactionCoverage *scan,
	const PsLayerMap *map, const PsCompactionPruneFence *captured)
{
	if (!scan || scan->started || scan->bound || !captured ||
		captured->timeline_incarnation == 0 || captured->page_floor == 0 ||
		!coverage_map_matches(scan, map, NULL, 0, 0))
		return -1;
	scan->fence = *captured;
	scan->bound = 1;
	return 0;
}

int
ps_compaction_coverage_publication_valid(const PsCompactionCoverage *scan,
	const PsLayerMap *map, const PsCompactionPruneFence *current,
	const PsLayerDesc *outputs, uint32_t noutputs, uint32_t published)
{
	const PsCompactionPruneFence *captured;

	if (!scan || scan->state != 1 || !scan->bound || !current)
		return 0;
	captured = &scan->fence;
	if (captured->retention_epoch != current->retention_epoch ||
		captured->timeline_epoch != current->timeline_epoch ||
		captured->admission_epoch != current->admission_epoch ||
		captured->timeline_incarnation != current->timeline_incarnation ||
		captured->page_floor != current->page_floor)
		return 0;
	return coverage_map_matches(scan, map, outputs, noutputs, published);
}

int
ps_compaction_coverage_snapshot_valid(const PsCompactionCoverage *scan,
	const PsLayerMap *map, const PsCompactionPruneFence *current)
{
	return ps_compaction_coverage_publication_valid(scan, map, current, NULL, 0, 0);
}

int
ps_compaction_coverage_inputs_match(const PsCompactionCoverage *scan,
	const PsLayerDesc *inputs, uint32_t ninputs)
{
	uint32_t count = 0;

	if (!scan || !inputs || ninputs == 0 || ninputs > PS_IMAGE_MERGE_MAX_INPUTS)
		return 0;
	for (uint32_t i = 0; i < scan->nlayers; i++)
		count += scan->selected[i] != 0;
	if (count != ninputs)
		return 0;
	for (uint32_t i = 0; i < ninputs; i++)
	{
		CoverageLayerRef key = {0}, *match;

		key.id = inputs[i].layer_id;
		match = bsearch(&key, scan->layer_refs, scan->nlayers,
						sizeof(*scan->layer_refs), coverage_layer_ref_cmp);
		if (!match || !scan->selected[match->index] ||
			!ps_image_snapshot_descriptor_equal(&inputs[i], &scan->layers[match->index]))
			return 0;
		for (uint32_t j = 0; j < i; j++)
			if (inputs[j].layer_id == inputs[i].layer_id)
				return 0;
	}
	return 1;
}

int
ps_compaction_identity_covered(const PsLayerDesc *layers, uint32_t nlayers,
	const unsigned char *selected, const PsImgIndexEnt *identity)
{
	PsCompactionCoverage *scan = NULL;
	unsigned char covered;
	int rc;

	if (ps_compaction_coverage_open(layers, nlayers, selected, identity, 1, &scan) != 0)
		return -1;
	do {
		rc = ps_compaction_coverage_step(scan, 1024);
	} while (rc == 1);
	if (rc == 0 && ps_compaction_coverage_results(scan, &covered, 1) == 0)
		rc = covered;
	else
		rc = -1;
	ps_compaction_coverage_close(scan);
	return rc;
}

static int
compaction_prune_fence_valid(const PsLayerMap *map,
	const PsLayerDesc *inputs, uint32_t ninputs,
	const PsLayerDesc *outputs, uint32_t noutputs,
	const PsCompactionPruneFence *captured, const PsCompactionPruneFence *current,
	int timeline_live, int manifest_poisoned, uint32_t published_prefix,
	uint32_t retired_prefix, int frontier_durable, int page_index_updated,
	int disjoint)
{
	if (inputs == NULL || outputs == NULL || ninputs == 0 || noutputs == 0 ||
		ninputs > PS_IMAGE_MERGE_MAX_INPUTS || noutputs > PS_IMAGE_PREPARE_MAX_OUTPUTS ||
		captured == NULL || current == NULL || !timeline_live || manifest_poisoned ||
		captured->timeline_incarnation == 0 || captured->page_floor == 0 ||
		captured->retention_epoch != current->retention_epoch ||
		captured->timeline_epoch != current->timeline_epoch ||
		captured->admission_epoch != current->admission_epoch ||
		captured->timeline_incarnation != current->timeline_incarnation ||
		captured->page_floor != current->page_floor ||
		published_prefix > noutputs || retired_prefix >= ninputs ||
		(published_prefix != 0 && !frontier_durable) ||
		(page_index_updated && (!frontier_durable || published_prefix != noutputs)) ||
		(retired_prefix != 0 && !page_index_updated))
		return 0;
	for (uint32_t i = 0; i < ninputs; i++)
		if (inputs[i].remote_durable)
			return 0;
	if (page_index_updated || retired_prefix != 0)
		return disjoint ? ps_image_compaction_retirement_valid_disjoint(map, inputs, ninputs,
			outputs, noutputs, retired_prefix) : ps_image_compaction_retirement_valid(map,
			inputs, ninputs, outputs, noutputs, retired_prefix);
	if (!(disjoint ? ps_image_compaction_publication_valid_disjoint(map, inputs, ninputs,
			outputs, noutputs, published_prefix) : ps_image_compaction_publication_valid(map,
			inputs, ninputs, outputs, noutputs, published_prefix)))
		return 0;
	for (uint32_t k = 0; k < ninputs; k++)
	{
		int pinned = 0;

		for (uint32_t i = 0; i < map->nlayers; i++)
			if (map->layers[i].layer_id == inputs[k].layer_id)
			{
				pinned = __atomic_load_n(&map->layers[i].cache_readers,
										 __ATOMIC_ACQUIRE) != 0;
				break;
			}
		if (!pinned)
			return 0;
	}
	return 1;
}

int
ps_compaction_prune_fence_valid(const PsLayerMap *map,
	const PsLayerDesc *inputs, uint32_t ninputs,
	const PsLayerDesc *outputs, uint32_t noutputs,
	const PsCompactionPruneFence *captured, const PsCompactionPruneFence *current,
	int timeline_live, int manifest_poisoned, uint32_t published_prefix,
	uint32_t retired_prefix, int frontier_durable, int page_index_updated)
{
	return compaction_prune_fence_valid(map, inputs, ninputs, outputs, noutputs,
		captured, current, timeline_live, manifest_poisoned, published_prefix,
		retired_prefix, frontier_durable, page_index_updated, 0);
}

int
ps_compaction_prune_fence_valid_disjoint(const PsLayerMap *map,
	const PsLayerDesc *inputs, uint32_t ninputs,
	const PsLayerDesc *outputs, uint32_t noutputs,
	const PsCompactionPruneFence *captured, const PsCompactionPruneFence *current,
	int timeline_live, int manifest_poisoned, uint32_t published_prefix,
	uint32_t retired_prefix, int frontier_durable, int page_index_updated)
{
	return compaction_prune_fence_valid(map, inputs, ninputs, outputs, noutputs,
		captured, current, timeline_live, manifest_poisoned, published_prefix,
		retired_prefix, frontier_durable, page_index_updated, 1);
}

struct PsCompactionRelationPlan
{
	PsImageIndexMerge *merge;
	PsImageGroupEntry *group;
	PsPruneVersion *versions;
	unsigned char *keep;
	unsigned char *closure;
	PsViewFence *fences;
	uint32_t nfences, max_group, used;
	PsPruneFence floor;
	uint64_t inherited_below;
	int has_inherited_below;
	int retain_non_relation;
	int control_only;
	CoverageIdentity *control_drop;
	uint32_t ncontrol_drop;
	int finished, failed;
};

void
ps_compaction_relation_plan_close(PsCompactionRelationPlan *plan)
{
	if (plan == NULL)
		return;
	ps_image_index_merge_close(plan->merge);
	free(plan->group);
	free(plan->versions);
	free(plan->keep);
	free(plan->closure);
	free(plan->fences);
	free(plan->control_drop);
	free(plan);
}

int
ps_compaction_relation_plan_open(const PsLayerDesc *layers, uint32_t count,
	uint32_t max_group, PsPruneFence floor, const PsViewFence *fences,
	uint32_t nfences, uint64_t inherited_below, int has_inherited_below,
	PsCompactionRelationPlan **out)
{
	PsCompactionRelationPlan *plan;

	if (out == NULL)
		return -1;
	*out = NULL;
	if (max_group == 0 || max_group > PS_IMAGE_GROUP_MAX_VERSIONS ||
		floor.lsn == 0 || floor.admission_seq == 0 ||
		(nfences != 0 && (fences == NULL ||
		 SIZE_MAX / nfences < sizeof(*fences))))
		return -1;
	plan = calloc(1, sizeof(*plan));
	if (plan == NULL)
		return -1;
	plan->floor = floor;
	plan->nfences = nfences;
	plan->max_group = max_group;
	plan->inherited_below = inherited_below;
	plan->has_inherited_below = has_inherited_below;
	plan->group = malloc((size_t) max_group * sizeof(*plan->group));
	plan->versions = malloc((size_t) max_group * sizeof(*plan->versions));
	plan->keep = malloc(max_group);
	plan->closure = malloc(max_group);
	if (nfences != 0)
	{
		plan->fences = malloc((size_t) nfences * sizeof(*plan->fences));
		if (plan->fences != NULL)
			memcpy(plan->fences, fences, (size_t) nfences * sizeof(*plan->fences));
	}
	if (plan->group == NULL || plan->versions == NULL || plan->keep == NULL ||
		plan->closure == NULL || (nfences != 0 && plan->fences == NULL) ||
		ps_image_index_merge_open(layers, count, &plan->merge) != 0)
	{
		ps_compaction_relation_plan_close(plan);
		return -1;
	}
	*out = plan;
	return 0;
}

int
ps_compaction_control_plan_open(const PsLayerDesc *layers, uint32_t count,
	uint32_t max_group, const PsImgIndexEnt *drop, uint32_t ndrop,
	PsCompactionRelationPlan **out)
{
	PsCompactionRelationPlan *plan = NULL;

	if (!out)
		return -1;
	*out = NULL;
	if (!drop || ndrop == 0 || sizeof(*plan->control_drop) > SIZE_MAX / (size_t) ndrop ||
		ps_compaction_relation_plan_open(layers, count, max_group,
			(PsPruneFence) {1, 1}, NULL, 0, 0, 0, &plan) != 0)
		return -1;
	plan->control_drop = calloc(ndrop, sizeof(*plan->control_drop));
	if (!plan->control_drop)
		goto fail;
	for (uint32_t i = 0; i < ndrop; i++)
	{
		if ((drop[i].key.klass != PS_KLASS_CONTROL || drop[i].block >= PS_CONTROL_PAIRED_BLOCKS) &&
			drop[i].key.klass != PS_KLASS_READER_SNAPSHOT && drop[i].key.klass != PS_KLASS_SLRU)
			goto fail;
		plan->control_drop[i].identity = drop[i];
	}
	qsort(plan->control_drop, ndrop, sizeof(*plan->control_drop), coverage_identity_cmp);
	for (uint32_t i = 1; i < ndrop; i++)
		if (coverage_identity_cmp(&plan->control_drop[i - 1], &plan->control_drop[i]) == 0)
			goto fail;
	plan->ncontrol_drop = ndrop;
	plan->control_only = 1;
	*out = plan;
	return 0;
fail:
	ps_compaction_relation_plan_close(plan);
	return -1;
}

static int
same_page(const PsImgIndexEnt *a, const PsImgIndexEnt *b)
{
	return a->block == b->block && a->key.spcOid == b->key.spcOid &&
		a->key.dbOid == b->key.dbOid && a->key.relNumber == b->key.relNumber &&
		a->key.forkNum == b->key.forkNum && a->key.klass == b->key.klass;
}

static int
compaction_select_group(const PsImageGroupEntry *entries,
	uint32_t count, const unsigned char *keep,
	PsImageGroupEntry *selected, uint32_t selected_capacity, uint32_t *nselected,
	PsImageGroupEntry *dropped, uint32_t dropped_capacity, uint32_t *ndropped,
	int allow_non_relation)
{
	uint32_t nsel = 0, ndrop = 0;

	if (nselected != NULL)
		*nselected = 0;
	if (ndropped != NULL)
		*ndropped = 0;
	if (entries == NULL || keep == NULL || nselected == NULL || ndropped == NULL ||
		nselected == ndropped ||
		count == 0 || count > PS_IMAGE_GROUP_MAX_VERSIONS ||
		(!allow_non_relation && entries[0].entry.key.klass != PS_KLASS_RELATION))
		return -1;
	/* Validate the entire group and size both outputs before writing either. */
	for (uint32_t i = 0; i < count;)
	{
		uint32_t next = i + 1;
		int any_kept = keep[i] != 0;

		if (!same_page(&entries[0].entry, &entries[i].entry) ||
			(i != 0 && (entries[i - 1].entry.lsn > entries[i].entry.lsn ||
			 (entries[i - 1].entry.lsn == entries[i].entry.lsn &&
			  entries[i - 1].entry.admission_seq >= entries[i].entry.admission_seq))))
			return -1;
		while (next < count && entries[next].entry.lsn == entries[i].entry.lsn &&
			   entries[next].entry.admission_seq == entries[i].entry.admission_seq)
		{
			if (!same_page(&entries[0].entry, &entries[next].entry) ||
				entries[next - 1].layer_id > entries[next].layer_id)
				return -1;
			any_kept |= keep[next] != 0;
			next++;
		}
		if (any_kept)
			nsel++;
		else
			ndrop++;
		i = next;
	}
	if (nsel > selected_capacity || ndrop > dropped_capacity)
		return -2;
	if ((nsel != 0 && selected == NULL) || (ndrop != 0 && dropped == NULL))
		return -1;
	nsel = ndrop = 0;
	for (uint32_t i = 0; i < count;)
	{
		uint32_t next = i + 1, winner = UINT32_MAX;

		while (next < count && entries[next].entry.lsn == entries[i].entry.lsn &&
			   entries[next].entry.admission_seq == entries[i].entry.admission_seq)
			next++;
		for (uint32_t j = next; j > i; j--)
			if (keep[j - 1])
			{
				winner = j - 1;
				break;
			}
		if (winner != UINT32_MAX)
			selected[nsel++] = entries[winner];
		else
			dropped[ndrop++] = entries[i];
		i = next;
	}
	*nselected = nsel;
	*ndropped = ndrop;
	return 0;
}

int
ps_compaction_relation_select_group(const PsImageGroupEntry *entries,
	uint32_t count, const unsigned char *keep,
	PsImageGroupEntry *selected, uint32_t selected_capacity, uint32_t *nselected,
	PsImageGroupEntry *dropped, uint32_t dropped_capacity, uint32_t *ndropped)
{
	return compaction_select_group(entries, count, keep, selected, selected_capacity,
		nselected, dropped, dropped_capacity, ndropped, 0);
}

int
ps_compaction_control_select_group(const PsImageGroupEntry *entries,
	uint32_t count, const unsigned char *keep,
	PsImageGroupEntry *selected, uint32_t selected_capacity, uint32_t *nselected,
	PsImageGroupEntry *dropped, uint32_t dropped_capacity, uint32_t *ndropped)
{
	return compaction_select_group(entries, count, keep, selected, selected_capacity,
		nselected, dropped, dropped_capacity, ndropped, 1);
}

void
ps_compaction_relation_plan_retain_non_relation(PsCompactionRelationPlan *plan)
{
	if (plan != NULL)
		plan->retain_non_relation = 1;
}

static int
emit_group(PsCompactionRelationPlan *plan, PsCompactionRelationVisitor visit,
		   void *arg)
{
	if (plan->control_only)
	{
		for (uint32_t i = 0; i < plan->used; i++)
		{
			CoverageIdentity key = {0};

			key.identity = plan->group[i].entry;
			plan->keep[i] = bsearch(&key, plan->control_drop, plan->ncontrol_drop,
				sizeof(*plan->control_drop), coverage_identity_cmp) == NULL;
			plan->closure[i] = plan->keep[i];
		}
		return visit(plan->group, plan->used, plan->keep, plan->closure, arg);
	}
	if (plan->group[0].entry.key.klass != PS_KLASS_RELATION)
	{
		if (!plan->retain_non_relation)
			return -1;
		/* All-version merge only: never prune control or other key classes. */
		memset(plan->keep, 1, plan->used);
		memset(plan->closure, 1, plan->used);
		return visit(plan->group, plan->used, plan->keep, plan->closure, arg);
	}
	for (uint32_t i = 0; i < plan->used; i++)
	{
		plan->versions[i].lsn = plan->group[i].entry.lsn;
		plan->versions[i].admission_seq = plan->group[i].entry.admission_seq;
	}
	if (ps_page_prune_plan_capped(plan->versions, plan->used, plan->floor,
			plan->fences, plan->nfences, plan->inherited_below,
			plan->has_inherited_below, plan->keep, plan->closure) < 0)
		return -1;
	return visit(plan->group, plan->used, plan->keep, plan->closure, arg);
}

int
ps_compaction_relation_plan_step(PsCompactionRelationPlan *plan,
	uint32_t max_groups, PsCompactionRelationVisitor visit, void *arg,
	uint32_t *visited)
{
	uint32_t emitted = 0;

	if (visited != NULL)
		*visited = 0;
	if (plan == NULL || max_groups == 0 || visit == NULL || visited == NULL ||
		plan->failed)
		return -1;
	if (plan->finished)
		return 0;
	for (;;)
	{
		PsImgIndexEnt entry;
		uint64_t layer_id;
		int next = ps_image_index_merge_next(plan->merge, &entry, &layer_id);

		if (next < 0)
			goto fail;
		if (plan->used != 0 && (next == 0 || !same_page(&plan->group[0].entry, &entry)))
		{
			if (emit_group(plan, visit, arg) != 0)
				goto fail;
			emitted++;
			plan->used = 0;
		}
		if (next == 0)
		{
			plan->finished = 1;
			*visited = emitted;
			return 0;
		}
		if (plan->used == plan->max_group)
		{
			const PsImgIndexEnt *last = &plan->group[plan->used - 1].entry;

			/* Control-only decisions are per complete identity. A long page
			 * chain can be emitted in bounded chunks only between identities;
			 * duplicate physical copies must remain in the same chunk. */
			if (!plan->control_only || !same_page(last, &entry) ||
				(last->lsn == entry.lsn && last->admission_seq == entry.admission_seq))
			{
				plan->failed = 1;
				*visited = emitted;
				return -2;
			}
			if (emit_group(plan, visit, arg) != 0)
				goto fail;
			emitted++;
			plan->used = 0;
		}
		plan->group[plan->used].entry = entry;
		plan->group[plan->used++].layer_id = layer_id;
		if (emitted == max_groups)
		{
			*visited = emitted;
			return 1;
		}
	}
fail:
	plan->failed = 1;
	*visited = emitted;
	return -1;
}

int
ps_compaction_plan_relation_groups(const PsLayerDesc *layers, uint32_t count,
	uint32_t max_group, PsPruneFence floor, const PsViewFence *fences,
	uint32_t nfences, uint64_t inherited_below, int has_inherited_below,
	PsCompactionRelationVisitor visit, void *arg)
{
	PsCompactionRelationPlan *plan = NULL;
	uint32_t visited;
	int rc;

	if (visit == NULL || ps_compaction_relation_plan_open(layers, count, max_group,
			floor, fences, nfences, inherited_below, has_inherited_below, &plan) != 0)
		return -1;
	do {
		rc = ps_compaction_relation_plan_step(plan, 64, visit, arg, &visited);
	} while (rc == 1);
	ps_compaction_relation_plan_close(plan);
	return rc;
}

typedef struct RelationVerifyContext
{
	const PsLayerDesc *inputs, *outputs;
	uint32_t ninputs, noutputs, page_size, output_index, pending_count;
	uint32_t output_counts[PS_IMAGE_PREPARE_MAX_OUTPUTS];
	PsImageGroupEntry *pending, *selected, *dropped;
	PsCompactionDroppedVisitor stage_drop;
	void *arg;
	int control_only;
	PsImageVerifiedSources *sources;
	uint64_t kept, dropped_count;
} RelationVerifyContext;

static int
verify_plan_group(const PsImageGroupEntry *entries, uint32_t count,
				  const unsigned char *keep, const unsigned char *closure, void *arg)
{
	RelationVerifyContext *ctx = arg;
	uint32_t nselected, ndropped;

	(void) closure;
	if (compaction_select_group(entries, count, keep,
			ctx->selected, PS_IMAGE_GROUP_MAX_VERSIONS, &nselected,
			ctx->dropped, PS_IMAGE_GROUP_MAX_VERSIONS, &ndropped, ctx->control_only) != 0)
		return -1;
	for (uint32_t i = 0; i < nselected; i++)
	{
		if (ctx->output_index >= ctx->noutputs)
			return -1;
		ctx->pending[ctx->pending_count++] = ctx->selected[i];
		if (ctx->pending_count == ctx->output_counts[ctx->output_index])
		{
			if ((ctx->sources ? ps_image_compaction_verify_verified_batch(ctx->sources,
					ctx->pending, ctx->pending_count, &ctx->outputs[ctx->output_index]) :
				 ps_image_compaction_verify_selected_batch(ctx->inputs, ctx->ninputs,
					ctx->pending, ctx->pending_count, &ctx->outputs[ctx->output_index],
					ctx->page_size)) != 0)
				return -1;
			ctx->output_index++;
			ctx->pending_count = 0;
		}
	}
	if (ndropped != 0 && (ctx->stage_drop == NULL ||
		ctx->stage_drop(ctx->dropped, ndropped, ctx->arg) != 0))
		return -1;
	ctx->kept += nselected;
	ctx->dropped_count += ndropped;
	return 0;
}

static int
compaction_verify_plan(const PsLayerDesc *inputs,
	uint32_t ninputs, const PsLayerDesc *outputs, uint32_t noutputs,
	uint32_t page_size, uint32_t max_group, PsPruneFence floor,
	const PsViewFence *fences, uint32_t nfences, uint64_t inherited_below,
	int has_inherited_below, PsCompactionDroppedVisitor stage_drop, void *arg,
	uint64_t *kept_out, uint64_t *dropped_out,
	const PsImgIndexEnt *control_drop, uint32_t ncontrol_drop)
{
	RelationVerifyContext ctx = {0};
	PsCompactionRelationPlan *plan = NULL;
	uint64_t max_id = 0;
	uint32_t visited;
	int rc = -1;

	if (kept_out != NULL)
		*kept_out = 0;
	if (dropped_out != NULL)
		*dropped_out = 0;
	if (inputs == NULL || ninputs == 0 || ninputs > PS_IMAGE_MERGE_MAX_INPUTS ||
		noutputs == 0 || noutputs > PS_IMAGE_PREPARE_MAX_OUTPUTS || outputs == NULL ||
		page_size == 0 || kept_out == NULL || dropped_out == NULL || kept_out == dropped_out)
		return -1;
	for (uint32_t i = 0; i < ninputs; i++)
	{
		PsLayerDesc copy = inputs[i];

		if (copy.kind != PS_LAYER_IMAGE || copy.deleting || copy.remote_durable ||
			copy.legacy_shard_zero || copy.timeline != inputs[0].timeline ||
			(copy.layer_id >> PS_IMAGE_LAYER_SHARD_SHIFT) !=
			(inputs[0].layer_id >> PS_IMAGE_LAYER_SHARD_SHIFT) ||
			(control_drop == NULL && ps_image_layer_verify_data(&copy, page_size) != 0))
			return -1;
		if (copy.layer_id > max_id)
			max_id = copy.layer_id;
	}
	ctx.inputs = inputs;
	ctx.outputs = outputs;
	ctx.ninputs = ninputs;
	ctx.noutputs = noutputs;
	ctx.page_size = page_size;
	ctx.stage_drop = stage_drop;
	ctx.arg = arg;
	ctx.control_only = control_drop != NULL;
	if (ctx.control_only && ps_image_compaction_sources_open(inputs, ninputs,
			page_size, &ctx.sources) != 0)
		goto done;
	for (uint32_t i = 0; i < noutputs; i++)
	{
		PsImageIndexCursor *cursor = NULL;
		PsImgIndexEnt entry;
		int next;

		if (outputs[i].kind != PS_LAYER_IMAGE || outputs[i].deleting ||
			outputs[i].remote_durable || outputs[i].legacy_shard_zero ||
			outputs[i].timeline != inputs[0].timeline || outputs[i].layer_id <= max_id ||
			(outputs[i].layer_id >> PS_IMAGE_LAYER_SHARD_SHIFT) !=
			(inputs[0].layer_id >> PS_IMAGE_LAYER_SHARD_SHIFT) ||
			(i != 0 && outputs[i].layer_id <= outputs[i - 1].layer_id) ||
			ps_image_index_cursor_open(&outputs[i], &cursor) != 0)
			goto done;
		while ((next = ps_image_index_cursor_next(cursor, &entry)) == 1)
			if (++ctx.output_counts[i] > PS_IMAGE_BATCH_MAX_RECORDS)
				break;
		ps_image_index_cursor_close(cursor);
		if (next != 0 || ctx.output_counts[i] == 0 ||
			ctx.output_counts[i] > PS_IMAGE_BATCH_MAX_RECORDS)
			goto done;
	}
	ctx.pending = malloc((size_t) PS_IMAGE_BATCH_MAX_RECORDS * sizeof(*ctx.pending));
	ctx.selected = malloc((size_t) PS_IMAGE_GROUP_MAX_VERSIONS * sizeof(*ctx.selected));
	ctx.dropped = malloc((size_t) PS_IMAGE_GROUP_MAX_VERSIONS * sizeof(*ctx.dropped));
	if (ctx.pending == NULL || ctx.selected == NULL || ctx.dropped == NULL ||
		(ctx.control_only ? ps_compaction_control_plan_open(inputs, ninputs, max_group,
			control_drop, ncontrol_drop, &plan) :
		 ps_compaction_relation_plan_open(inputs, ninputs, max_group, floor,
			fences, nfences, inherited_below, has_inherited_below, &plan)) != 0)
		goto done;
	do {
		rc = ps_compaction_relation_plan_step(plan, 32, verify_plan_group, &ctx, &visited);
	} while (rc == 1);
	if (rc == 0 && (ctx.output_index != noutputs || ctx.pending_count != 0 ||
		(ctx.control_only && ctx.dropped_count != ncontrol_drop)))
		rc = -1;
	if (rc == 0 && ctx.sources && ps_image_compaction_sources_revalidate(ctx.sources) != 0)
		rc = -1;
	if (rc == 0)
	{
		*kept_out = ctx.kept;
		*dropped_out = ctx.dropped_count;
	}
done:
	ps_compaction_relation_plan_close(plan);
	ps_image_compaction_sources_close(ctx.sources);
	free(ctx.pending);
	free(ctx.selected);
	free(ctx.dropped);
	return rc;
}

int
ps_compaction_verify_relation_plan(const PsLayerDesc *inputs,
	uint32_t ninputs, const PsLayerDesc *outputs, uint32_t noutputs,
	uint32_t page_size, uint32_t max_group, PsPruneFence floor,
	const PsViewFence *fences, uint32_t nfences, uint64_t inherited_below,
	int has_inherited_below, PsCompactionDroppedVisitor stage_drop, void *arg,
	uint64_t *kept_out, uint64_t *dropped_out)
{
	return compaction_verify_plan(inputs, ninputs, outputs, noutputs, page_size,
		max_group, floor, fences, nfences, inherited_below, has_inherited_below,
		stage_drop, arg, kept_out, dropped_out, NULL, 0);
}

int
ps_compaction_verify_control_plan(const PsLayerDesc *inputs,
	uint32_t ninputs, const PsLayerDesc *outputs, uint32_t noutputs,
	uint32_t page_size, uint32_t max_group, const PsImgIndexEnt *drop, uint32_t ndrop,
	PsCompactionDroppedVisitor stage_drop, void *arg, uint64_t *kept_out, uint64_t *dropped_out)
{
	if (!drop || ndrop == 0)
	{
		if (kept_out)
			*kept_out = 0;
		if (dropped_out)
			*dropped_out = 0;
		return -1;
	}
	return compaction_verify_plan(inputs, ninputs, outputs, noutputs, page_size,
		max_group, (PsPruneFence) {1, 1}, NULL, 0, 0, 0, stage_drop, arg,
		kept_out, dropped_out, drop, ndrop);
}
