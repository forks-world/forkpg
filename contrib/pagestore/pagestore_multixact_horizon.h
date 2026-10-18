/* Member horizons shared by branch validation and SLRU seeding. */
#ifndef PAGESTORE_MULTIXACT_HORIZON_H
#define PAGESTORE_MULTIXACT_HORIZON_H

#include <stdbool.h>
#include <stdint.h>

static inline bool
ps_member_horizon_valid(int64_t oldest, int64_t next, bool offsets64)
{
	if (oldest < 0 || next < 0)
		return false;
	if (offsets64)
		return oldest <= next;
	return (uint64_t) oldest <= UINT32_MAX && (uint64_t) next <= UINT32_MAX;
}

/* Inclusive page ranges, including the allocation boundary at next. */
static inline int
ps_member_seed_ranges(int64_t oldest, int64_t next, int64_t per_page,
					 int64_t lo[2], int64_t hi[2])
{
	if (oldest == next)
		return 0;
	lo[0] = oldest / per_page;
	if (oldest < next)
	{
		hi[0] = next / per_page;
		return 1;
	}
	/* Only a validated 32-bit horizon can wrap. */
	hi[0] = UINT32_MAX / per_page;
	lo[1] = 0;
	hi[1] = next / per_page;
	return 2;
}

/* PG18 PerformMembersTruncation deletes [start, end) in segment order. */
static inline bool
ps_member_segment_in_range(int64_t segment, int64_t start, int64_t end)
{
	if (start <= end)
		return segment >= start && segment < end;
	return segment >= start || segment < end;
}

#endif
