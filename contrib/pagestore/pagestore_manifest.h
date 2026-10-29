/*-------------------------------------------------------------------------
 *
 * pagestore_manifest.h
 *	  Durable metadata log for immutable pagestore layers.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PAGESTORE_MANIFEST_H
#define PAGESTORE_MANIFEST_H

#include <stdint.h>

#include "pagestore_layer.h"

extern PsLayerMap ps_layer_map;

typedef struct PsFlushWatermark
{
	uint32_t	shard;
	uint32_t	seg_id;
	uint64_t	seg_off;
} PsFlushWatermark;

extern int	ps_manifest_open(const char *store_dir);
extern void ps_manifest_close(void);
extern int	ps_manifest_poisoned(void);
extern int	ps_manifest_replay_repaired(void);
extern int	ps_manifest_replay_had_manifest(void);
/* 1 means a durable replay-repair quarantine marker exists, 0 means absent,
 * and -1 means the marker itself is invalid. */
extern int	ps_manifest_orphan_sweep_inhibited(void);
extern int	ps_manifest_replay(PsLayerMap *map);
extern int	ps_manifest_add_layer(const PsLayerDesc *desc);
/* Add a freshly prepared layer only if its ID is not present. The caller
 * serializes map mutations, proves coverage and fences concurrent changes.
 * A failure after an append may have an ambiguous durable result; no output
 * or input may be deleted merely because this call failed. */
extern int ps_manifest_add_new_layer(const PsLayerDesc *desc);
/* Append one output in order only while the captured input set and already
 * published prefix still match. Caller must first verify complete coverage,
 * pin sources/outputs, reserve every output ID, and hold the map and relevant
 * writer/lifecycle fences. A failed append may be durably ambiguous; do not
 * delete inputs or outputs based on the return code. This does not retire
 * sources and does not make the operation crash-atomic. */
extern int ps_manifest_publish_image_batch(const PsLayerDesc *inputs,
	uint32_t ninputs, const PsLayerDesc *outputs, uint32_t noutputs,
	uint32_t batch_index);
/* Retire one source in order after all output ADDs are durable and coverage
 * is verified. Caller owns pins on all inputs, reserves output IDs, and holds
 * the map and writer/lifecycle fences while validating timeline state. A failed
 * append may be durable: stop and recover by replay, never delete on failure.
 * This writes one record, but its fsync has no time bound. */
extern int ps_manifest_retire_image_source(const PsLayerDesc *inputs,
	uint32_t ninputs, const PsLayerDesc *outputs, uint32_t noutputs,
	uint32_t source_index);
extern int	ps_manifest_set_remote_location(uint64_t layer_id,
											const PsLayerLocation *location);
extern int	ps_manifest_set_remote_durable(uint64_t layer_id,
										   uint64_t uploaded_lsn);
extern int	ps_manifest_drop_local(uint64_t layer_id);
extern int	ps_manifest_mark_delete(uint64_t layer_id);
extern int	ps_manifest_remove_layer(uint64_t layer_id);
extern int	ps_manifest_set_flush_watermark(uint32_t shard, uint32_t seg_id,
										 uint64_t seg_off);
/* Not called by the daemon (see the definition's header comment); kept for
 * pagestore_manifest_test.c's replay coverage of a pre-fix manifest. */
extern int	ps_manifest_rebase_flush_watermark(uint32_t shard, uint32_t seg_id,
										 uint64_t seg_off);
extern int	ps_manifest_get_flush_watermark(uint32_t shard,
										 PsFlushWatermark *out);

/* Bound replay time by rewriting the append-only log to one record per live layer. */
extern int	ps_manifest_should_compact(void);
extern int	ps_manifest_compact(void);

#endif							/* PAGESTORE_MANIFEST_H */
