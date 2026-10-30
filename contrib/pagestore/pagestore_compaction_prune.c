#include <stdlib.h>
#include <string.h>

#include "pagestore_compaction_prune.h"

int
ps_compaction_prune_fence_valid(const PsLayerMap *map,
	const PsLayerDesc *inputs, uint32_t ninputs,
	const PsLayerDesc *outputs, uint32_t noutputs,
	const PsCompactionPruneFence *captured, const PsCompactionPruneFence *current,
	int timeline_live, int manifest_poisoned, uint32_t published_prefix,
	uint32_t retired_prefix, int frontier_durable, int page_index_updated)
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
		return ps_image_compaction_retirement_valid(map, inputs, ninputs,
			outputs, noutputs, retired_prefix);
	if (!ps_image_compaction_publication_valid(map, inputs, ninputs,
			outputs, noutputs, published_prefix))
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

static int
same_page(const PsImgIndexEnt *a, const PsImgIndexEnt *b)
{
	return a->block == b->block && a->key.spcOid == b->key.spcOid &&
		a->key.dbOid == b->key.dbOid && a->key.relNumber == b->key.relNumber &&
		a->key.forkNum == b->key.forkNum && a->key.klass == b->key.klass;
}

int
ps_compaction_relation_select_group(const PsImageGroupEntry *entries,
	uint32_t count, const unsigned char *keep,
	PsImageGroupEntry *selected, uint32_t selected_capacity, uint32_t *nselected,
	PsImageGroupEntry *dropped, uint32_t dropped_capacity, uint32_t *ndropped)
{
	uint32_t nsel = 0, ndrop = 0;

	if (nselected != NULL)
		*nselected = 0;
	if (ndropped != NULL)
		*ndropped = 0;
	if (entries == NULL || keep == NULL || nselected == NULL || ndropped == NULL ||
		nselected == ndropped ||
		count == 0 || count > PS_IMAGE_GROUP_MAX_VERSIONS ||
		entries[0].entry.key.klass != PS_KLASS_RELATION)
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

static int
emit_group(PsCompactionRelationPlan *plan, PsCompactionRelationVisitor visit,
		   void *arg)
{
	if (plan->group[0].entry.key.klass != PS_KLASS_RELATION)
		return -1;
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
			plan->failed = 1;
			*visited = emitted;
			return -2;
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
	uint64_t kept, dropped_count;
} RelationVerifyContext;

static int
verify_plan_group(const PsImageGroupEntry *entries, uint32_t count,
				  const unsigned char *keep, const unsigned char *closure, void *arg)
{
	RelationVerifyContext *ctx = arg;
	uint32_t nselected, ndropped;

	(void) closure;
	if (ps_compaction_relation_select_group(entries, count, keep,
			ctx->selected, PS_IMAGE_GROUP_MAX_VERSIONS, &nselected,
			ctx->dropped, PS_IMAGE_GROUP_MAX_VERSIONS, &ndropped) != 0)
		return -1;
	for (uint32_t i = 0; i < nselected; i++)
	{
		if (ctx->output_index >= ctx->noutputs)
			return -1;
		ctx->pending[ctx->pending_count++] = ctx->selected[i];
		if (ctx->pending_count == ctx->output_counts[ctx->output_index])
		{
			if (ps_image_compaction_verify_selected_batch(ctx->inputs, ctx->ninputs,
					ctx->pending, ctx->pending_count, &ctx->outputs[ctx->output_index],
					ctx->page_size) != 0)
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

int
ps_compaction_verify_relation_plan(const PsLayerDesc *inputs,
	uint32_t ninputs, const PsLayerDesc *outputs, uint32_t noutputs,
	uint32_t page_size, uint32_t max_group, PsPruneFence floor,
	const PsViewFence *fences, uint32_t nfences, uint64_t inherited_below,
	int has_inherited_below, PsCompactionDroppedVisitor stage_drop, void *arg,
	uint64_t *kept_out, uint64_t *dropped_out)
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
			ps_image_layer_verify_data(&copy, page_size) != 0)
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
		ps_compaction_relation_plan_open(inputs, ninputs, max_group, floor,
			fences, nfences, inherited_below, has_inherited_below, &plan) != 0)
		goto done;
	do {
		rc = ps_compaction_relation_plan_step(plan, 32, verify_plan_group, &ctx, &visited);
	} while (rc == 1);
	if (rc == 0 && (ctx.output_index != noutputs || ctx.pending_count != 0))
		rc = -1;
	if (rc == 0)
	{
		*kept_out = ctx.kept;
		*dropped_out = ctx.dropped_count;
	}
done:
	ps_compaction_relation_plan_close(plan);
	free(ctx.pending);
	free(ctx.selected);
	free(ctx.dropped);
	return rc;
}
