#include <stdlib.h>

#include "pagestore_compaction_prune.h"

typedef struct RelationPlanContext
{
	PsPruneFence floor;
	const PsViewFence *fences;
	uint32_t nfences;
	uint64_t inherited_below;
	int has_inherited_below;
	PsCompactionRelationVisitor visit;
	void *arg;
	PsPruneVersion *versions;
	unsigned char *keep;
	unsigned char *closure;
} RelationPlanContext;

static int
plan_relation_group(const PsImageGroupEntry *entries, uint32_t count, void *arg)
{
	RelationPlanContext *ctx = arg;

	if (entries[0].entry.key.klass != PS_KLASS_RELATION)
		return -1;
	for (uint32_t i = 0; i < count; i++)
	{
		ctx->versions[i].lsn = entries[i].entry.lsn;
		ctx->versions[i].admission_seq = entries[i].entry.admission_seq;
	}
	if (ps_page_prune_plan_capped(ctx->versions, count, ctx->floor,
			ctx->fences, ctx->nfences, ctx->inherited_below,
			ctx->has_inherited_below, ctx->keep, ctx->closure) < 0)
		return -1;
	return ctx->visit(entries, count, ctx->keep, ctx->closure, ctx->arg);
}

int
ps_compaction_plan_relation_groups(const PsLayerDesc *layers, uint32_t count,
	uint32_t max_group, PsPruneFence floor, const PsViewFence *fences,
	uint32_t nfences, uint64_t inherited_below, int has_inherited_below,
	PsCompactionRelationVisitor visit, void *arg)
{
	RelationPlanContext ctx = {0};
	int rc = -1;

	if (max_group == 0 || max_group > PS_IMAGE_GROUP_MAX_VERSIONS ||
		floor.lsn == 0 || floor.admission_seq == 0 || visit == NULL ||
		(nfences != 0 && fences == NULL))
		return -1;
	ctx.floor = floor;
	ctx.fences = fences;
	ctx.nfences = nfences;
	ctx.inherited_below = inherited_below;
	ctx.has_inherited_below = has_inherited_below;
	ctx.visit = visit;
	ctx.arg = arg;
	ctx.versions = malloc((size_t) max_group * sizeof(*ctx.versions));
	ctx.keep = malloc(max_group);
	ctx.closure = malloc(max_group);
	if (ctx.versions != NULL && ctx.keep != NULL && ctx.closure != NULL)
		rc = ps_image_index_visit_groups(layers, count, max_group,
									 plan_relation_group, &ctx);
	free(ctx.versions);
	free(ctx.keep);
	free(ctx.closure);
	return rc;
}
