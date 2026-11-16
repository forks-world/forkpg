/* Bounded, staged page-history planning for image compaction. */
#ifndef PAGESTORE_COMPACTION_PRUNE_H
#define PAGESTORE_COMPACTION_PRUNE_H

#include "pagestore_layer.h"
#include "pagestore_prune.h"

/* Batch equivalent of the physical coverage check below. Sources must remain
 * pinned until close. Open copies layer descriptors, candidates and selection
 * bits, rejects duplicate identities and validates layer IDs.
 * Each step consumes at most max_work index entries, end markers or layer
 * opens. Each cursor read requests at most 64 KiB, but provider latency is
 * not bounded. Return 1 while more work is
 * needed, 0 when complete, -1 on a terminal error. Results become available
 * only after all indexes have reached their verified end. A failed scan never
 * exposes partial results. None of these operations authorizes publication. */
typedef struct PsCompactionCoverage PsCompactionCoverage;
extern int ps_compaction_coverage_open(const PsLayerDesc *layers, uint32_t nlayers,
	const unsigned char *selected, const PsImgIndexEnt *identities, uint32_t nidentities,
	PsCompactionCoverage **out);
/* Before the first step, require every physical control image, redo note and
 * admission-fence identity at any candidate LSN to appear in the candidate
 * set. An unknown identity in any selected or excluded layer fails the
 * entire scan. All candidates must share a control key. This does not prove
 * completeness of non-candidate LSNs or of the caller's retention plan.
 * Layers whose durable key, block or LSN bounds exclude all candidates may
 * be skipped without reading their indexes. */
extern int ps_compaction_coverage_require_control_groups(PsCompactionCoverage *scan);
extern int ps_compaction_coverage_step(PsCompactionCoverage *scan, uint32_t max_work);
extern int ps_compaction_coverage_results(const PsCompactionCoverage *scan,
	unsigned char *covered, uint32_t count);
/* After a complete scan, verify each selected source was already the global
 * highest-layer copy of that identity before writing a newer output layer. */
extern int ps_compaction_coverage_selected_winners_valid(const PsCompactionCoverage *scan,
	const PsImageGroupEntry *selected, uint32_t count);
/* After a scan started with no selected layers, select every layer
 * containing a candidate plus extra_index (UINT32_MAX chooses the smallest
 * other local layer when one exists). Returns the source count, 0 when
 * incomplete or over limit, and -1 on invalid state; no output on failure. */
extern int ps_compaction_coverage_select_found_sources(PsCompactionCoverage *scan,
	uint32_t extra_index, uint32_t max_sources, unsigned char *selected, uint32_t count);
extern void ps_compaction_coverage_close(PsCompactionCoverage *scan);

/* Check physical coverage of one proposed dropped identity over a complete,
 * immutable, caller-pinned snapshot of the relevant timeline and shard.
 * selected[i] is 1 exactly for an input layer. Return 1 only if the identity
 * occurs in at least one selected layer and in no excluded layer, 0 if it is
 * absent from selected layers or occurs in an excluded layer, -1 on invalid
 * input or an index read/checksum failure. Every index is read to completion.
 * This does not establish snapshot completeness, retention decisions,
 * output correctness, or publication authority. The caller must prove and
 * revalidate those separately, including concurrent map and admission changes.
 * Runtime and I/O are proportional to the full supplied index set. */
extern int ps_compaction_identity_covered(const PsLayerDesc *layers,
	uint32_t nlayers, const unsigned char *selected, const PsImgIndexEnt *identity);

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
/* Only for all-version merge checks: pass through every non-relation version.
 * Pruning and verification plans retain the default reject behavior. */
extern void ps_compaction_relation_plan_retain_non_relation(PsCompactionRelationPlan *plan);
extern int ps_compaction_relation_plan_step(PsCompactionRelationPlan *plan,
	uint32_t max_groups, PsCompactionRelationVisitor visit, void *arg,
	uint32_t *visited);
extern void ps_compaction_relation_plan_close(PsCompactionRelationPlan *plan);

/* Construct a staged control/reader-snapshot/SLRU plan. Drop identities must already be
 * proven eligible by the caller. All other classes and identities survive.
 * This interface does not prove eligibility or authorize publication. */
extern int ps_compaction_control_plan_open(const PsLayerDesc *layers, uint32_t count,
	uint32_t max_group, const PsImgIndexEnt *drop, uint32_t ndrop,
	PsCompactionRelationPlan **out);
extern int ps_compaction_control_select_group(const PsImageGroupEntry *entries,
	uint32_t count, const unsigned char *keep,
	PsImageGroupEntry *selected, uint32_t selected_capacity, uint32_t *nselected,
	PsImageGroupEntry *dropped, uint32_t dropped_capacity, uint32_t *ndropped);

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
/* Recompute an explicit artifact/control plan and verify staged output records and data.
 * Every named drop must occur in the inputs. This checks output integrity only;
 * the caller must independently prove retention and physical coverage. */
extern int ps_compaction_verify_control_plan(const PsLayerDesc *inputs,
	uint32_t ninputs, const PsLayerDesc *outputs, uint32_t noutputs,
	uint32_t page_size, uint32_t max_group, const PsImgIndexEnt *drop, uint32_t ndrop,
	PsCompactionDroppedVisitor stage_drop, void *arg, uint64_t *kept_out, uint64_t *dropped_out);

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

/* Bind before the first scan step, and revalidate after completion under the
 * lifecycle, shard, prune and map locks used for the fence. Every layer in the
 * relevant timeline/shard must be present, unchanged and pinned; the caller
 * owns those pins and captures the fence under the same locks as the snapshot.
 * Legacy cross-shard or unsupported layers fail closed. These functions do not
 * verify retained views, staged outputs, pin ownership, or authorize publish.
 * Validate immediately before any use; subsequent mutations invalidate it. */
extern int ps_compaction_coverage_bind_snapshot(PsCompactionCoverage *scan,
	const PsLayerMap *map, const PsCompactionPruneFence *captured);
extern int ps_compaction_coverage_snapshot_valid(const PsCompactionCoverage *scan,
	const PsLayerMap *map, const PsCompactionPruneFence *current);
extern int ps_compaction_coverage_publication_valid(const PsCompactionCoverage *scan,
	const PsLayerMap *map, const PsCompactionPruneFence *current,
	const PsLayerDesc *outputs, uint32_t noutputs, uint32_t published);
extern int ps_compaction_coverage_inputs_match(const PsCompactionCoverage *scan,
	const PsLayerDesc *inputs, uint32_t ninputs);

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
/* As above, allowing only excluded layers disjoint from all sources and
 * outputs. Epochs, owned pins, frontier and index obligations are unchanged. */
extern int ps_compaction_prune_fence_valid_disjoint(const PsLayerMap *map,
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
