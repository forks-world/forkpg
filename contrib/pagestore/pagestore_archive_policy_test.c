#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "pagestore_archive_policy.h"

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition); \
		return EXIT_FAILURE; \
	} \
} while (0)

int
main(void)
{
	PsMaterializerRejectedFormat hint = {0};
	const uint64_t marker = 100, checkpoint = 200;
	const uint64_t segment = 16 * 1024 * 1024;
	const uint64_t original = 256 * 1024 * 1024;
	uint64_t bound;
	uint64_t target, target_end;

	CHECK(!ps_materializer_rejection_matches(&hint, 3, marker, checkpoint));
	hint.magic = PS_MATERIALIZER_REJECTED_MAGIC;
	hint.version = PS_MATERIALIZER_REJECTED_VERSION;
	hint.timeline = 3;
	hint.materialized_lsn = marker;
	hint.materialized_lsn_complement = ~marker;
	hint.checkpoint_lsn = checkpoint;
	hint.checkpoint_lsn_complement = ~checkpoint;
	CHECK(ps_materializer_rejection_matches(&hint, 3, marker, checkpoint));
	CHECK(!ps_materializer_rejection_matches(&hint, 4, marker, checkpoint));
	CHECK(!ps_materializer_rejection_matches(&hint, 3, marker + 1, checkpoint));
	CHECK(!ps_materializer_rejection_matches(&hint, 3, marker, checkpoint + 1));
	hint.checkpoint_lsn_complement ^= 1;
	CHECK(!ps_materializer_rejection_matches(&hint, 3, marker, checkpoint));
	hint.checkpoint_lsn_complement ^= 1;
	hint.materialized_lsn_complement ^= 1;
	CHECK(!ps_materializer_rejection_matches(&hint, 3, marker, checkpoint));
	bound = ps_materializer_rejection_limit(original, segment, 2);
	CHECK(bound == original + 2 * segment);
	for (int i = 0; i < 100; i++)
		CHECK(ps_materializer_rejection_limit(original, segment, 2) == bound);
	CHECK(ps_materializer_rejection_limit(original, segment, 0) == original);
	CHECK(ps_materializer_rejection_limit(original, segment, 4) == original + 4 * segment);
	CHECK(ps_materializer_rejection_limit(UINT64_MAX - 5, segment, 2) == UINT64_MAX);
	CHECK(ps_materializer_rejection_limit(1, UINT64_MAX, 2) == UINT64_MAX);

	/* Over-cap checkpoints consume the frozen cap instead of failing closed. */
	CHECK(ps_materializer_progress_clamp(256, 180, 100, &target, &target_end) == 0);
	CHECK(target == 181 && target_end == 256);
	CHECK(ps_materializer_progress_clamp(181, 180, 100, &target, &target_end) != 0);
	CHECK(ps_materializer_progress_clamp(182, 180, 100, &target, &target_end) == 0);
	CHECK(target == 181 && target_end == 182);
	CHECK(ps_materializer_progress_clamp(256, 100, 100, &target, &target_end) != 0);
	CHECK(ps_materializer_progress_clamp(256, 180, 180, &target, &target_end) != 0);
	CHECK(ps_materializer_progress_clamp(256, UINT64_MAX - 1, 100,
										 &target, &target_end) != 0);
	CHECK(ps_materializer_progress_clamp(UINT64_MAX, UINT64_MAX - 2, 100,
										 &target, &target_end) == 0);
	CHECK(target == UINT64_MAX - 1 && target_end == UINT64_MAX);
	CHECK(ps_materializer_progress_clamp(256, 180, 100, NULL, &target_end) != 0);
	CHECK(ps_materializer_progress_clamp(256, 180, 100, &target, NULL) != 0);

	/* Boundary selection: fit, clamp, blocked, impossible. */
	CHECK(ps_materializer_progress_boundary(256, 180, 100, 200, 256,
											&target, &target_end) == 1);
	CHECK(target == 200 && target_end == 256);
	CHECK(ps_materializer_progress_boundary(192, 180, 100, 200, 256,
											&target, &target_end) == 2);
	CHECK(target == 181 && target_end == 192);
	CHECK(ps_materializer_progress_boundary(181, 180, 100, 200, 256,
											&target, &target_end) == 0);
	/* A segment-aligned checkpoint end is valid, not an impossible input. */
	CHECK(ps_materializer_progress_boundary(256, 180, 100, 256, 256,
											&target, &target_end) == 1);
	CHECK(target == 255 && target_end == 256);
	CHECK(ps_materializer_progress_boundary(192, 180, 100, 192, 192,
											&target, &target_end) == 1);
	CHECK(target == 191 && target_end == 192);
	CHECK(ps_materializer_progress_boundary(191, 180, 100, 192, 192,
											&target, &target_end) == 2);
	CHECK(target == 181 && target_end == 191);
	CHECK(ps_materializer_progress_boundary(181, 180, 100, 192, 192,
											&target, &target_end) == 0);
	CHECK(ps_materializer_progress_boundary(256, 180, 100, 200, 199,
											&target, &target_end) == -1);
	CHECK(ps_materializer_progress_boundary(256, 180, 100, 200, 256,
											NULL, &target_end) == -1);
	CHECK(ps_materializer_progress_boundary(256, 180, 100, 200, 256,
											&target, NULL) == -1);
	puts("archive policy tests passed");
	return 0;
}
