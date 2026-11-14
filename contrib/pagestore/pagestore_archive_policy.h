#ifndef PAGESTORE_ARCHIVE_POLICY_H
#define PAGESTORE_ARCHIVE_POLICY_H
#include <stdint.h>
#include "pagestore_artifact_format.h"

static inline int
ps_materializer_rejection_matches(const PsMaterializerRejectedFormat *hint,
								  uint32_t timeline, uint64_t marker,
								  uint64_t checkpoint)
{
	return hint != 0 && hint->magic == PS_MATERIALIZER_REJECTED_MAGIC &&
		hint->version == PS_MATERIALIZER_REJECTED_VERSION &&
		hint->timeline == timeline && hint->materialized_lsn == marker &&
		hint->materialized_lsn_complement == ~marker &&
		hint->checkpoint_lsn == checkpoint &&
		hint->checkpoint_lsn_complement == ~checkpoint;
}

static inline uint64_t
ps_materializer_rejection_limit(uint64_t original_limit, uint64_t segment_size,
								uint64_t extra_segments)
{
	uint64_t allowance;

	if (segment_size != 0 && extra_segments > UINT64_MAX / segment_size)
		return UINT64_MAX;
	allowance = segment_size * extra_segments;
	return original_limit > UINT64_MAX - allowance ? UINT64_MAX :
		original_limit + allowance;
}
#endif
