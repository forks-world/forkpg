#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "pagestore_archive_progress.h"
#include "pagestore_archive_policy.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(void)
{
	PsArchiveProgress s = {0};
	CHECK(ps_archive_progress_begin(&s, 3, 1, 2, 100, 192, 64) != 0);
	CHECK(ps_archive_progress_begin(&s, 3, 1, 1, 100, 192, 64) == 0);
	CHECK(ps_archive_progress_safe(&s, 3, 1, 0, 100, 180) != 0);
	CHECK(ps_archive_progress_safe(&s, 3, 2, 1, 100, 180) != 0);
	CHECK(ps_archive_progress_safe(&s, 3, 1, 1, 99, 180) != 0);
	CHECK(ps_archive_progress_safe(&s, 3, 1, 1, 100, 180) == 0);
	CHECK(ps_archive_progress_prepare(&s, 3, 1, 1, 100, 1, 200, 256) != 0);
	CHECK(ps_archive_progress_freeze_budget(&s, 3, 1, 1, 100, 192, 64) == 0);
	CHECK(ps_archive_progress_freeze_budget(&s, 3, 1, 1, 100, 193, 64) != 0);
	CHECK(ps_archive_progress_prepare(&s, 3, 1, 1, 100, 1, 200, 256) == 0);
	CHECK(!s.grant_active && s.candidate_wal_tli == 1);
	CHECK(ps_archive_progress_activate(&s, 3, 1, 1, 100, 1, 200, 256) == 0);
	CHECK(s.grant_active);
	CHECK(ps_archive_progress_prepare(&s, 3, 2, 1, 100, 1, 200, 256) != 0);
	CHECK(ps_archive_progress_begin(&s, 3, 1, 1, 100, 192, 64) == 0);
	CHECK(s.safe_lsn == 180 && s.target == 200 && s.grant_end == 256);
	CHECK(ps_archive_progress_begin(&s, 3, 1, 1, 100, 193, 64) == 0);
	for (int i = 0; i < 100; i++)
		CHECK(ps_archive_progress_activate(&s, 3, 1, 1, 100, 1, 200, 256) == 0);
	CHECK(ps_archive_progress_activate(&s, 3, 1, 1, 100, 1, 201, 256) != 0);
	CHECK(ps_archive_progress_begin(&s, 3, 1, 2, 100, 192, 64) == 0);
	CHECK(s.target == 200 && s.grant_end == 256 && s.safe_lsn == 0 && s.grant_active);
	CHECK(s.budget_frozen && s.original_limit == 192 && s.max_extra == 64);
	CHECK(ps_archive_progress_safe(&s, 3, 1, 1, 100, 180) != 0);
	CHECK(ps_archive_progress_safe(&s, 3, 1, 2, 100, 180) == 0);
	CHECK(s.budget_frozen && s.original_limit == 192 && s.max_extra == 64);
	CHECK(ps_archive_progress_activate(&s, 3, 1, 2, 100, 1, 201, 256) != 0);
	CHECK(ps_archive_progress_begin(&s, 3, 1, 3, 110, 192, 64) == 0);
	CHECK(s.target == 0 && s.grant_end == 0);
	CHECK(ps_archive_progress_safe(&s, 3, 1, 1, 110, 180) != 0);
	CHECK(ps_archive_progress_safe(&s, 3, 1, 3, 110, 180) == 0);
	CHECK(!s.budget_frozen && s.original_limit == 0 && s.max_extra == 0);
	CHECK(ps_archive_progress_freeze_budget(&s, 3, 1, 3, 110, UINT64_MAX - 4, 8) != 0);
	CHECK(ps_archive_progress_freeze_budget(&s, 3, 1, 3, 110, 192, 64) == 0);
	CHECK(ps_archive_progress_freeze_budget(&s, 3, 1, 3, 110, 193, 64) != 0);
	CHECK(ps_archive_progress_prepare(&s, 3, 1, 3, 110, 1, 200, 257) != 0);
	CHECK(ps_archive_progress_prepare(&s, 3, 1, 3, 110, 1, 180, 256) != 0);

	/*
	 * A checkpoint beyond the frozen cap: clamp the grant to the cap and let
	 * the normal PREPARE/ACTIVATE path authorize it.  These are the values
	 * recovered from the 2026-11-26 round-428 deadlock.
	 */
	{
		PsArchiveProgress t = {0};
		const uint64_t marker = UINT64_C(0x62FCE7C0);
		const uint64_t safe = UINT64_C(0x68FFE5C8);
		const uint64_t original = UINT64_C(0x69000000);
		const uint64_t extra = UINT64_C(0x08000000);
		uint64_t target, target_end;

		CHECK(ps_archive_progress_begin(&t, 0, 1, 1, marker, original, extra) == 0);
		CHECK(ps_archive_progress_safe(&t, 0, 1, 1, marker, safe) == 0);
		CHECK(ps_archive_progress_freeze_budget(&t, 0, 1, 1, marker,
												original, extra) == 0);
		CHECK(ps_materializer_progress_clamp(original + extra, safe, marker,
											 &target, &target_end) == 0);
		CHECK(target == safe + 1 && target_end == original + extra);
		CHECK(ps_archive_progress_prepare(&t, 0, 1, 1, marker, 1,
										  target, target_end) == 0);
		CHECK(ps_archive_progress_activate(&t, 0, 1, 1, marker, 1,
										   target, target_end) == 0);
		CHECK(t.grant_active && t.grant_end == original + extra);
	}
	puts("archive progress transition tests passed");
	return 0;
}
