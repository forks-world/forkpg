#include <string.h>

#include "pagestore_archive_progress_authority.h"
#include "pagestore_artifact_format.h"
#include "pagestore_core.h"

int
ps_archive_progress_marker_acquire(void *arg, uint32_t timeline,
								   uint64_t *marker_lsn, uint64_t *incarnation)
{
	PsTimelineState state;
	uint64_t found_incarnation = 0;
	PsKey key = {0};
	PsMaterializerMarkerFormat marker;
	unsigned char page[PS_IO_UNIT];
	int rc = -1;

	(void) arg;
	if (!marker_lsn || !incarnation || ps_nshards == 0)
		return -1;
	for (uint32_t shard = 0; shard < ps_nshards; shard++)
		ps_lock_shard_wr(shard);

	/* POSIX sync fsyncs the fd cache; SPDK sync flushes shard buffers. */
	if (ps_storage == NULL || ps_storage->sync == NULL || ps_storage->sync() != 0)
		goto out;

	ps_lock_map_rd();
	if (!ps_timeline_defined(timeline) ||
		!ps_timeline_state(timeline, &state, &found_incarnation) ||
		state != PS_TIMELINE_LIVE || found_incarnation == 0)
	{
		ps_unlock_map();
		goto out;
	}
	ps_unlock_map();

	key.klass = PS_KLASS_CONTROL;
	/* read_resolve takes map-rd internally, so do not retain map-rd here. */
	if (read_resolve(timeline, &key, PS_MATERIALIZER_MARKER_BLOCK,
					 UINT64_MAX, 0, page, NULL) != 1)
		goto out;
	memcpy(&marker, page, sizeof(marker));
	if (marker.magic != PS_MATERIALIZER_MARKER_MAGIC ||
		marker.version != PS_MATERIALIZER_MARKER_VERSION ||
		marker.timeline != timeline || marker.materialized_lsn == 0 ||
		marker.materialized_lsn_complement != ~marker.materialized_lsn)
		goto out;

	*marker_lsn = marker.materialized_lsn;
	*incarnation = found_incarnation;
	rc = 0;
out:
	if (rc != 0)
		for (uint32_t shard = ps_nshards; shard-- > 0;)
			ps_unlock_shard(shard);
	return rc;
}

void
ps_archive_progress_marker_release(void *arg, uint32_t timeline)
{
	(void) arg;
	(void) timeline;
	for (uint32_t shard = ps_nshards; shard-- > 0;)
		ps_unlock_shard(shard);
}
