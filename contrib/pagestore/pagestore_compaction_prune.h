/* Bounded, staged page-history planning for image compaction. */
#ifndef PAGESTORE_COMPACTION_PRUNE_H
#define PAGESTORE_COMPACTION_PRUNE_H

#include "pagestore_layer.h"
#include "pagestore_prune.h"

/* Each callback receives one complete relation-page history in merge order,
 * together with the retention planner's decisions and closure protections.
 * Arrays are transient; all effects must be staged until the entire call
 * succeeds. Inputs and the supplied retention snapshot must stay immutable.
 * This is a conservative plan: it does not apply fork invalidation and does
 * not grant authority to publish, advance a frontier, or retire sources.
 * Non-relation groups fail closed. Return -2 for an oversized history, -1 for
 * invalid input, unsupported keys, I/O or callback failure, and 0 for success. */
typedef int (*PsCompactionRelationVisitor)(const PsImageGroupEntry *entries,
	uint32_t count, const unsigned char *keep,
	const unsigned char *closure_protect, void *arg);

/* Resumable variant. Open copies the retention fences but may scan whole input
 * indexes for validation; it is not a bounded-I/O operation. Sources must
 * remain immutable and pinned until close. Between steps the caller must keep
 * the retention/timeline/admission snapshot valid or discard all results.
 * Each step emits at most max_groups complete page groups, and may read one
 * entry of the next group. Return 1 for more work, 0 when all inputs have been
 * checked, -2 on oversized history, or -1 on any other failure. A failure is
 * terminal and invalidates every callback from this plan, including earlier
 * steps; no callback may publish, advance a frontier, or retire sources.
 * visited reports successful callbacks in this step, including on failure. */
typedef struct PsCompactionRelationPlan PsCompactionRelationPlan;
extern int ps_compaction_relation_plan_open(const PsLayerDesc *layers,
	uint32_t count, uint32_t max_group, PsPruneFence floor,
	const PsViewFence *fences, uint32_t nfences, uint64_t inherited_below,
	int has_inherited_below, PsCompactionRelationPlan **out);
extern int ps_compaction_relation_plan_step(PsCompactionRelationPlan *plan,
	uint32_t max_groups, PsCompactionRelationVisitor visit, void *arg,
	uint32_t *visited);
extern void ps_compaction_relation_plan_close(PsCompactionRelationPlan *plan);

extern int ps_compaction_plan_relation_groups(const PsLayerDesc *layers,
	uint32_t count, uint32_t max_group, PsPruneFence floor,
	const PsViewFence *fences, uint32_t nfences, uint64_t inherited_below,
	int has_inherited_below, PsCompactionRelationVisitor visit, void *arg);

#endif
