#include <string.h>
#include <stdlib.h>
#include "pagestore_archive_progress_format.h"
#include "pagestore_archive_progress_store.h"

static int
persist_progress(void *arg, const PsArchiveProgress *state)
{
	PsArchiveProgressStore *store = arg;
	unsigned char encoded[PS_ARCHIVE_PROGRESS_FORMAT_BYTES];
	if (state->attempt && state->timeline != store->timeline)
		return -1;
	if (ps_archive_progress_encode(state, encoded, sizeof(encoded)) != 0)
		return -1;
	return store->storage->archive_progress_replace(store->timeline, encoded, sizeof(encoded));
}

int
ps_archive_progress_store_open(PsArchiveProgressStore *store,
							   const PsStorage *storage, uint32_t timeline)
{
	unsigned char encoded[PS_ARCHIVE_PROGRESS_FORMAT_BYTES];
	PsArchiveProgress recovered = {0};
	int status;
	if (!store || !storage || !storage->archive_progress_read ||
		!storage->archive_progress_replace)
		return -1;
	status = storage->archive_progress_read(timeline, encoded, sizeof(encoded));
	if (status < 0 || status > 1 ||
		(status == 0 && (ps_archive_progress_decode(&recovered, encoded, sizeof(encoded)) != 0 ||
						 (recovered.attempt && recovered.timeline != timeline))))
		return -1;
	memset(store, 0, sizeof(*store));
	store->storage = storage;
	store->timeline = timeline;
	return ps_archive_progress_txn_init(&store->transaction, &recovered, persist_progress, store);
}

void
ps_archive_progress_store_close(PsArchiveProgressStore *store)
{
	ps_archive_progress_txn_destroy(&store->transaction);
}

int
ps_archive_progress_registry_open(PsArchiveProgressRegistry *registry,
								  const PsStorage *storage, uint32_t capacity)
{
	if (!registry || !storage || !capacity ||
		!storage->archive_progress_read || !storage->archive_progress_replace)
		return -1;
	memset(registry, 0, sizeof(*registry));
	registry->stores = calloc(capacity, sizeof(*registry->stores));
	if (!registry->stores)
		return -1;
	if (pthread_mutex_init(&registry->lock, NULL) != 0)
	{
		free(registry->stores);
		registry->stores = NULL;
		return -1;
	}
	registry->storage = storage;
	registry->capacity = capacity;
	return 0;
}

int
ps_archive_progress_registry_set_marker_authority(
	PsArchiveProgressRegistry *registry,
	PsArchiveProgressMarkerAcquire acquire,
	PsArchiveProgressMarkerRelease release, void *arg)
{
	int empty = 1;

	if (!registry || !acquire || !release || pthread_mutex_lock(&registry->lock) != 0)
		return -1;
	for (uint32_t i = 0; i < registry->capacity; i++)
		if (registry->stores[i])
		{
			empty = 0;
			break;
		}
	if (empty && registry->marker_acquire == NULL)
	{
		registry->marker_acquire = acquire;
		registry->marker_release = release;
		registry->marker_authority_arg = arg;
	}
	else
		empty = 0;
	pthread_mutex_unlock(&registry->lock);
	return empty ? 0 : -1;
}

void
ps_archive_progress_registry_close(PsArchiveProgressRegistry *registry)
{
	for (uint32_t i = 0; i < registry->capacity; i++)
		if (registry->stores[i])
		{
			ps_archive_progress_store_close(registry->stores[i]);
			free(registry->stores[i]);
		}
	free(registry->stores);
	pthread_mutex_destroy(&registry->lock);
	registry->stores = NULL;
	registry->capacity = 0;
}

/* Caller holds registry->lock. */
static PsArchiveProgressStore *
registry_get_locked(PsArchiveProgressRegistry *registry, uint32_t timeline)
{
	PsArchiveProgressStore *store = registry->stores[timeline];
	if (!store)
	{
		store = malloc(sizeof(*store));
		if (!store)
			return NULL;
		if (ps_archive_progress_store_open(store, registry->storage, timeline) != 0)
		{
			free(store);
			return NULL;
		}
		registry->stores[timeline] = store;
	}
	return store;
}

int
ps_archive_progress_registry_apply(PsArchiveProgressRegistry *registry,
								   uint32_t timeline,
								   PsArchiveProgressTransition transition,
								   void *arg, PsArchiveProgress *result)
{
	PsArchiveProgressStore *store;
	int rc = -1;
	if (!registry || !transition || timeline >= registry->capacity ||
		pthread_mutex_lock(&registry->lock) != 0)
		return -1;
	store = registry_get_locked(registry, timeline);
	if (store)
		rc = ps_archive_progress_txn_apply(&store->transaction, transition, arg, result);
	pthread_mutex_unlock(&registry->lock);
	return rc;
}

int
ps_archive_progress_registry_snapshot(PsArchiveProgressRegistry *registry,
									  uint32_t timeline, PsArchiveProgress *result)
{
	PsArchiveProgressStore *store;
	int rc = -1;
	if (!registry || !result || timeline >= registry->capacity ||
		pthread_mutex_lock(&registry->lock) != 0)
		return -1;
	store = registry_get_locked(registry, timeline);
	if (store)
		rc = ps_archive_progress_txn_snapshot(&store->transaction, result);
	pthread_mutex_unlock(&registry->lock);
	return rc;
}

typedef struct PsArchiveProgressStart
{
	uint32_t timeline;
	uint64_t expected_attempt;
	uint64_t nonce_hi, nonce_lo;
	uint64_t marker, original_limit, max_extra;
	uint64_t incarnation;
} PsArchiveProgressStart;

static int
start_transition(PsArchiveProgress *state, void *arg)
{
	PsArchiveProgressStart *request = arg;

	if ((!request->nonce_hi && !request->nonce_lo) ||
		request->expected_attempt == UINT64_MAX)
		return -1;
	if (state->attempt == request->expected_attempt + 1 &&
		state->start_nonce_hi == request->nonce_hi &&
		state->start_nonce_lo == request->nonce_lo)
		return state->timeline == request->timeline &&
			state->incarnation == request->incarnation && state->marker == request->marker &&
			state->original_limit == request->original_limit && state->max_extra == request->max_extra ? 0 : -1;
	if (state->attempt != request->expected_attempt ||
		(state->attempt && state->start_nonce_hi == request->nonce_hi &&
		 state->start_nonce_lo == request->nonce_lo))
		return -1;
	if (ps_archive_progress_begin(state, request->timeline, request->incarnation,
								  request->expected_attempt + 1,
								  request->marker, request->original_limit,
								  request->max_extra) != 0)
		return -1;
	state->start_nonce_hi = request->nonce_hi;
	state->start_nonce_lo = request->nonce_lo;
	return 0;
}

int
ps_archive_progress_registry_start(PsArchiveProgressRegistry *registry,
								   uint32_t timeline, uint64_t expected_attempt,
								   uint64_t nonce_hi, uint64_t nonce_lo,
								   uint64_t marker, uint64_t incarnation,
								   uint64_t original_limit,
								   uint64_t max_extra, PsArchiveProgress *result)
{
	PsArchiveProgressStore *store;
	PsArchiveProgress committed;
	PsArchiveProgressStart request = {timeline, expected_attempt, nonce_hi, nonce_lo,
									  marker, original_limit, max_extra, incarnation};
	uint64_t current_marker = 0, current_incarnation = 0;
	int rc = -1;

	if (!registry || timeline >= registry->capacity ||
		pthread_mutex_lock(&registry->lock) != 0)
		return -1;
	/* A lost successful reply is idempotent even if the marker later moved.
	 * This returns the already committed generation only; it cannot grant a
	 * new one or alter its fixed target. */
	store = registry->stores[timeline];
	if (store && expected_attempt != UINT64_MAX &&
		ps_archive_progress_txn_snapshot(&store->transaction, &committed) == 0 &&
		committed.attempt == expected_attempt + 1 &&
		committed.start_nonce_hi == nonce_hi &&
		committed.start_nonce_lo == nonce_lo)
	{
		rc = committed.timeline == timeline &&
			committed.incarnation == incarnation && committed.marker == marker &&
			committed.original_limit == original_limit &&
			committed.max_extra == max_extra ? 0 : -1;
		if (rc == 0 && result)
			*result = committed;
		pthread_mutex_unlock(&registry->lock);
		return rc;
	}
	/* The caller's marker and timeline identity are not authority. Without a
	 * configured control-store reader, reject rather than persist a guess. */
	if (registry->marker_acquire == NULL || registry->marker_release == NULL ||
		registry->marker_acquire(registry->marker_authority_arg, timeline,
								 &current_marker, &current_incarnation) != 0)
		goto done;
	if (current_marker != marker || current_incarnation != incarnation)
	{
		registry->marker_release(registry->marker_authority_arg, timeline);
		goto done;
	}
	store = registry_get_locked(registry, timeline);
	if (store)
		rc = ps_archive_progress_txn_apply(&store->transaction, start_transition,
										   &request, result);
	registry->marker_release(registry->marker_authority_arg, timeline);
done:
	pthread_mutex_unlock(&registry->lock);
	return rc;
}
