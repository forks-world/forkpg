/* Validation of advisory materializer feedback.  A valid hint can increase
 * shipping only; it is never evidence that replayed data is durable. */
#ifndef PAGESTORE_ARCHIVE_POLICY_H
#define PAGESTORE_ARCHIVE_POLICY_H

#include <stdbool.h>
#include <stdint.h>
#include "pagestore_artifact_format.h"

static inline bool
ps_materializer_rejection_matches(const PsMaterializerRejectedFormat *hint,
								  uint32_t timeline, uint64_t marker,
								  uint64_t checkpoint)
{
	return marker != 0 && checkpoint != 0 &&
		hint->magic == PS_MATERIALIZER_REJECTED_MAGIC &&
		hint->version == PS_MATERIALIZER_REJECTED_VERSION &&
		hint->timeline == timeline &&
		hint->materialized_lsn_complement == ~hint->materialized_lsn &&
		hint->checkpoint_lsn_complement == ~hint->checkpoint_lsn &&
		hint->materialized_lsn == marker && hint->checkpoint_lsn >= checkpoint;
}

static inline uint64_t
ps_materializer_rejection_limit(uint64_t original_limit, uint64_t segment_size,
								uint64_t segments)
{
	uint64_t allowance = segments != 0 && segment_size > UINT64_MAX / segments ?
		UINT64_MAX : segment_size * segments;

	return original_limit > UINT64_MAX - allowance ?
		UINT64_MAX : original_limit + allowance;
}
#endif
