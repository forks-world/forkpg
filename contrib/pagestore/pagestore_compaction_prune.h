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

/* Collapse the physical copies of each planned logical identity. The selected
 * copy is the highest-layer kept copy; each fully dropped identity appears
 * exactly once in dropped. Inputs are one complete, merge-ordered relation
 * page group; arrays must not overlap. The function makes no publication or
 * reclamation decision. On failure counts are zero and output arrays are not
 * modified: -2 means insufficient output capacity, -1 malformed input. */
extern int ps_compaction_relation_select_group(const PsImageGroupEntry *entries,
	uint32_t count, const unsigned char *keep,
	PsImageGroupEntry *selected, uint32_t selected_capacity, uint32_t *nselected,
	PsImageGroupEntry *dropped, uint32_t dropped_capacity, uint32_t *ndropped);

/* Recompute the conservative relation plan and verify the entire ordered set
 * of staged outputs against it. At least one nonempty output is required;
 * each output is limited to one image batch.
 * Dropped identities are delivered once in plan order to stage_drop, which
 * must only stage effects. A later error invalidates all earlier callbacks.
 * On success, *kept_out and *dropped_out are logical identity counts; on
 * failure they are zero. Returns -2 for an oversized page history, -1 for
 * other failures. Sources, outputs and the retention snapshot must be pinned
 * and immutable throughout. The caller must separately revalidate concurrent
 * state and safely advance the frontier, update the page index and publish or
 * retire layers. Verification scans whole indexes and may rescan them for
 * each output, so it does not bound execution time or total I/O. */
typedef int (*PsCompactionDroppedVisitor)(const PsImageGroupEntry *dropped,
	uint32_t count, void *arg);
extern int ps_compaction_verify_relation_plan(const PsLayerDesc *inputs,
	uint32_t ninputs, const PsLayerDesc *outputs, uint32_t noutputs,
	uint32_t page_size, uint32_t max_group, PsPruneFence floor,
	const PsViewFence *fences, uint32_t nfences, uint64_t inherited_below,
	int has_inherited_below, PsCompactionDroppedVisitor stage_drop, void *arg,
	uint64_t *kept_out, uint64_t *dropped_out);

/* Values must be captured and sampled under the caller's lifecycle, all
 * relevant shard, page-prune and map locks. Equality is only a fence: it does
 * not prove output contents, pin ownership, or durable frontier publication. */
typedef struct PsCompactionPruneFence
{
	uint64_t retention_epoch;
	uint64_t timeline_epoch;
	uint64_t admission_epoch;
	uint64_t timeline_incarnation;
	uint64_t page_floor;
} PsCompactionPruneFence;

/* Check the next publication/retirement boundary without changing state.
 * published_prefix and retired_prefix are exact durable map prefixes. The
 * supplied frontier_durable and page_index_updated facts must be established
 * by the caller; frontier_durable means the durable frontier covers this
 * plan's cutoff and admission fence, not merely that some frontier exists.
 * A claim is not proof. Inputs must remain owned and pinned.
 * Call under the locks above before each action, and discard/recover the plan
 * on any mismatch or ambiguous manifest/frontier result. This function does
 * not authorize publishing: a complete verified plan and crash-safe ordering
 * are still required. Retired prefix is checked before retiring the next
 * source, so it must be smaller than ninputs. */
extern int ps_compaction_prune_fence_valid(const PsLayerMap *map,
	const PsLayerDesc *inputs, uint32_t ninputs,
	const PsLayerDesc *outputs, uint32_t noutputs,
	const PsCompactionPruneFence *captured, const PsCompactionPruneFence *current,
	int timeline_live, int manifest_poisoned, uint32_t published_prefix,
	uint32_t retired_prefix, int frontier_durable, int page_index_updated);

extern int ps_compaction_plan_relation_groups(const PsLayerDesc *layers,
	uint32_t count, uint32_t max_group, PsPruneFence floor,
	const PsViewFence *fences, uint32_t nfences, uint64_t inherited_below,
	int has_inherited_below, PsCompactionRelationVisitor visit, void *arg);

#endif
