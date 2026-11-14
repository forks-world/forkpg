#ifndef PAGESTORE_ARCHIVE_PROGRESS_STORE_H
#define PAGESTORE_ARCHIVE_PROGRESS_STORE_H
#include "pagestore_archive_progress_txn.h"
#include "pagestore_storage.h"

/* One instance per timeline, owned and serialized by the daemon. Opening a
 * second instance on the same timeline would bypass the process-local lock. */
typedef struct PsArchiveProgressStore
{
	PsArchiveProgressTxn transaction;
	const PsStorage *storage;
	uint32_t timeline;
} PsArchiveProgressStore;

/* Acquire holds the control-marker serialization lock until release. If it
 * fails after taking any lock, it must unwind those locks itself. It must
 * return the completed durable marker and current timeline incarnation, and
 * is called with registry->lock held. It must not call back into this
 * registry. This lease makes marker comparison + START persistence atomic
 * with respect to marker publication. */
typedef int (*PsArchiveProgressMarkerAcquire)(void *arg, uint32_t timeline,
											  uint64_t *marker,
											  uint64_t *incarnation);
typedef void (*PsArchiveProgressMarkerRelease)(void *arg, uint32_t timeline);
int ps_archive_progress_store_open(PsArchiveProgressStore *store,
								   const PsStorage *storage, uint32_t timeline);
void ps_archive_progress_store_close(PsArchiveProgressStore *store);

/* Registry lifetime must be externally fenced against close. The registry
 * serializes loading and all transitions, so one timeline has one authority. */
typedef struct PsArchiveProgressRegistry
{
	pthread_mutex_t lock;
	const PsStorage *storage;
	PsArchiveProgressStore **stores;
	uint32_t capacity;
	PsArchiveProgressMarkerAcquire marker_acquire;
	PsArchiveProgressMarkerRelease marker_release;
	void *marker_authority_arg;
} PsArchiveProgressRegistry;
int ps_archive_progress_registry_open(PsArchiveProgressRegistry *registry,
									   const PsStorage *storage, uint32_t capacity);
void ps_archive_progress_registry_close(PsArchiveProgressRegistry *registry);
/* Install once, before the first registry operation. No authority means
 * START is fail-closed. */
int ps_archive_progress_registry_set_marker_authority(
	PsArchiveProgressRegistry *registry,
	PsArchiveProgressMarkerAcquire acquire,
	PsArchiveProgressMarkerRelease release, void *arg);
int ps_archive_progress_registry_apply(PsArchiveProgressRegistry *registry,
										uint32_t timeline,
										PsArchiveProgressTransition transition,
										void *arg, PsArchiveProgress *result);
/* Atomically allocate expected_attempt + 1. A 128-bit nonce uniquely names
 * a startup request; the same request is idempotent after a lost reply. The
 * caller must authenticate the startup and validate the durable marker. */
int ps_archive_progress_registry_start(PsArchiveProgressRegistry *registry,
										uint32_t timeline, uint64_t expected_attempt,
										uint64_t nonce_hi, uint64_t nonce_lo,
										uint64_t marker, uint64_t incarnation,
										uint64_t original_limit,
										uint64_t max_extra, PsArchiveProgress *result);
/* `replayed` reports an idempotent response from a previous durable START;
 * callers must not treat it as current-attempt authorization. */
int ps_archive_progress_registry_start_ex(PsArchiveProgressRegistry *registry,
										   uint32_t timeline,
										   uint64_t expected_attempt,
										   uint64_t nonce_hi, uint64_t nonce_lo,
										   uint64_t marker, uint64_t incarnation,
										   uint64_t original_limit,
										   uint64_t max_extra,
										   PsArchiveProgress *result,
										   int *replayed);
int ps_archive_progress_registry_safe(PsArchiveProgressRegistry *registry,
									   uint32_t timeline, uint64_t incarnation,
									   uint64_t attempt, uint64_t marker,
									   uint64_t safe_lsn,
									   PsArchiveProgress *result);
int ps_archive_progress_registry_freeze_budget(PsArchiveProgressRegistry *registry,
									   uint32_t timeline, uint64_t incarnation,
									   uint64_t attempt, uint64_t marker,
									   uint64_t original_limit, uint64_t max_extra,
									   PsArchiveProgress *result);
int ps_archive_progress_registry_prepare(PsArchiveProgressRegistry *registry,
									  uint32_t timeline, uint64_t incarnation,
									  uint64_t attempt, uint64_t marker,
									  uint32_t candidate_wal_tli,
									  uint64_t target, uint64_t target_end,
									  PsArchiveProgress *result);
int ps_archive_progress_registry_activate(PsArchiveProgressRegistry *registry,
									  uint32_t timeline, uint64_t incarnation,
									  uint64_t attempt, uint64_t marker,
									  uint32_t candidate_wal_tli,
									  uint64_t target, uint64_t target_end,
									  PsArchiveProgress *result);
int ps_archive_progress_registry_snapshot(PsArchiveProgressRegistry *registry,
										   uint32_t timeline, PsArchiveProgress *result);
/* Only returns an active state whose marker and incarnation still match the
 * marker authority under its lease. Empty state remains readable. */
int ps_archive_progress_registry_authorized_snapshot(
	PsArchiveProgressRegistry *registry, uint32_t timeline,
	uint64_t incarnation, PsArchiveProgress *result);
#endif
