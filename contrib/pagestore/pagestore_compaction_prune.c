#include <stdlib.h>
#include <string.h>

#include "pagestore_compaction_prune.h"

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
