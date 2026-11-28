#ifndef PAGESTORE_ARCHIVE_PROGRESS_AUTHORITY_H
#define PAGESTORE_ARCHIVE_PROGRESS_AUTHORITY_H

#include "pagestore_archive_progress_store.h"

/* Caller holds lifecycle/admission read locks. Acquire retains all shard
 * write locks until release, and returns only a synced valid marker. */
int ps_archive_progress_marker_acquire(void *arg, uint32_t timeline,
									   uint64_t *marker_lsn,
									   uint64_t *incarnation);
void ps_archive_progress_marker_release(void *arg, uint32_t timeline);

#endif
