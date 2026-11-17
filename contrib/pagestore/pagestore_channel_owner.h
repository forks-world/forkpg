/* Fail-closed ownership and dead-owner reclamation for shared channels. */
#ifndef PAGESTORE_CHANNEL_OWNER_H
#define PAGESTORE_CHANNEL_OWNER_H

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pagestore_ipc.h"

#ifdef PAGESTORE_CHANNEL_OWNER_TEST
extern void (*ps_test_channel_reclaim_hook)(PsChannel *);
#endif

/* This is optional: without a provably shared PID namespace we use legacy
 * ownership and do not attempt PID-based reclamation. */
static inline int
ps_channel_pid_namespace(uint64_t *dev, uint64_t *ino)
{
#ifdef __linux__
	struct stat st;

	if (stat("/proc/self/ns/pid", &st) == 0 && st.st_ino != 0)
	{
		*dev = (uint64_t) st.st_dev;
		*ino = (uint64_t) st.st_ino;
		return 1;
	}
#elif defined(__APPLE__)
	/* Darwin has a single system PID namespace. */
	*dev = UINT64_MAX;
	*ino = 1;
	return 1;
#endif
	*dev = 0;
	*ino = 0;
	return 0;
}

/* Each token is unique for this daemon incarnation. Exhaustion fails closed
 * instead of wrapping: a reused PID must never recreate an earlier token. */
static inline uint64_t
ps_channel_owner_claim(PsShmHeader *hdr)
{
	uint64_t dev, ino;
	pid_t pid = getpid();
	uint32_t generation;

	if (!__atomic_always_lock_free(sizeof(uint64_t), 0) ||
		pid <= 0 || (uint64_t) pid > INT_MAX ||
		!hdr->channel_pidns_ino || !ps_channel_pid_namespace(&dev, &ino) ||
		dev != hdr->channel_pidns_dev || ino != hdr->channel_pidns_ino)
		return 1;
	for (;;)
	{
		generation = ps_load_acquire(&hdr->channel_owner_generation);
		if (generation == UINT32_MAX)
			return 1;
		if (ps_cas(&hdr->channel_owner_generation, generation, generation + 1))
			return ((uint64_t) (generation + 1) << 32) | (uint32_t) pid;
	}
}

/* A reused PID only delays cleanup. The generation in the CAS prevents an
 * old death observation from stealing a newly claimed channel (ABA). */
static inline int
ps_channel_reclaim_dead(PsChannel *ch, uint64_t my_claim)
{
	uint64_t claim = ps_load_acquire_u64(&ch->claimed);
	uint32_t pid = (uint32_t) claim;
	uint32_t state;

	if ((my_claim >> 32) == 0 || (claim >> 32) == 0 ||
		pid == 0 || pid > INT_MAX || claim == my_claim)
		return 0;
	if (kill((pid_t) pid, 0) == 0 || errno != ESRCH)
		return 0;
	state = ps_load_acquire(&ch->state);
	if (state != PS_STATE_DONE && state != PS_STATE_IDLE)
		return 0;
#ifdef PAGESTORE_CHANNEL_OWNER_TEST
	if (ps_test_channel_reclaim_hook)
		ps_test_channel_reclaim_hook(ch);
#endif
	return ps_cas_u64(&ch->claimed, claim, my_claim);
}

#endif
