/* Boundary tests and a literal circular-segment deletion oracle. */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include "pagestore_multixact_horizon.h"

int
main(void)
{
	int64_t lo[2], hi[2];
	const int64_t mpp = (8192 / 20) * 4;

	assert(ps_member_horizon_valid(UINT32_MAX - 1, 2, false));
	assert(!ps_member_horizon_valid(UINT32_MAX - 1, 2, true));
	assert(!ps_member_horizon_valid(0, (int64_t) UINT32_MAX + 1, false));
	assert(ps_member_horizon_valid(0, (int64_t) UINT32_MAX + 1, true));
	assert(!ps_member_horizon_valid(-1, 0, false));
	assert(!ps_member_horizon_valid(0, -1, true));
	assert(ps_member_seed_ranges(7, 7, mpp, lo, hi) == 0);
	assert(ps_member_seed_ranges(mpp - 1, mpp, mpp, lo, hi) == 1);
	assert(lo[0] == 0 && hi[0] == 1);
	assert(ps_member_seed_ranges(UINT32_MAX - 1, 0, mpp, lo, hi) == 2);
	assert(lo[0] == UINT32_MAX / mpp && hi[0] == lo[0]);
	assert(lo[1] == 0 && hi[1] == 0);
	assert(ps_member_seed_ranges(UINT32_MAX - 1, mpp, mpp, lo, hi) == 2);
	assert(lo[1] == 0 && hi[1] == 1);
	assert(ps_member_seed_ranges(INT64_MAX - 1, INT64_MAX, mpp, lo, hi) == 1);
	assert(lo[0] == (INT64_MAX - 1) / mpp && hi[0] == INT64_MAX / mpp);

	/* Independent oracle: walk exactly as PG18 PerformMembersTruncation. */
	for (int start = 0; start < 17; start++)
		for (int end = 0; end < 17; end++)
		{
			bool removed[17] = {false};
			for (int seg = start; seg != end; seg = (seg + 1) % 17)
				removed[seg] = true;
			for (int seg = 0; seg < 17; seg++)
				assert(ps_member_segment_in_range(seg, start, end) == removed[seg]);
		}
	puts("multixact member horizons: PASS");
	return 0;
}
