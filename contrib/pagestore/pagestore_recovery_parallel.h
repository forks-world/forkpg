#ifndef PAGESTORE_RECOVERY_PARALLEL_H
#define PAGESTORE_RECOVERY_PARALLEL_H

#include <stdint.h>
#include <unistd.h>

/* Includes the calling recovery thread; never create more threads than jobs. */
#define PS_RECOVERY_MAX_WORKERS 8

static inline uint32_t
ps_recovery_worker_count(uint64_t jobs)
{
	long cpus = sysconf(_SC_NPROCESSORS_ONLN);
	uint32_t count = cpus > 0 && cpus < PS_RECOVERY_MAX_WORKERS ?
		(uint32_t) cpus : (cpus > 0 ? PS_RECOVERY_MAX_WORKERS : 4);

	return jobs < count ? (uint32_t) jobs : count;
}

#endif
