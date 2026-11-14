#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "pagestore_archive_progress.h"
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
	CHECK(ps_archive_progress_pin(&s, 3, 1, 1, 100, 200, 256) == 0);
	CHECK(ps_archive_progress_pin(&s, 3, 2, 1, 100, 200, 256) != 0);
	CHECK(ps_archive_progress_begin(&s, 3, 1, 1, 100, 192, 64) == 0);
	CHECK(s.safe_lsn == 180 && s.target == 200 && s.grant_end == 256);
	CHECK(ps_archive_progress_begin(&s, 3, 1, 1, 100, 193, 64) != 0);
	for (int i = 0; i < 100; i++)
		CHECK(ps_archive_progress_pin(&s, 3, 1, 1, 100, 200, 256) == 0);
	CHECK(ps_archive_progress_pin(&s, 3, 1, 1, 100, 201, 256) != 0);
	CHECK(ps_archive_progress_begin(&s, 3, 1, 2, 100, 193, 64) != 0);
	CHECK(ps_archive_progress_begin(&s, 3, 1, 3, 100, 192, 64) != 0);
	CHECK(ps_archive_progress_begin(&s, 3, 1, 2, 100, 192, 64) == 0);
	CHECK(s.target == 200 && s.grant_end == 256 && s.safe_lsn == 0);
	CHECK(ps_archive_progress_safe(&s, 3, 1, 1, 100, 180) != 0);
	CHECK(ps_archive_progress_safe(&s, 3, 1, 2, 100, 180) == 0);
	CHECK(ps_archive_progress_pin(&s, 3, 1, 2, 100, 201, 256) != 0);
	CHECK(ps_archive_progress_begin(&s, 3, 1, 3, 110, 192, 64) == 0);
	CHECK(s.target == 0 && s.grant_end == 0);
	CHECK(ps_archive_progress_safe(&s, 3, 1, 1, 110, 180) != 0);
	CHECK(ps_archive_progress_safe(&s, 3, 1, 3, 110, 180) == 0);
	CHECK(ps_archive_progress_pin(&s, 3, 1, 3, 110, 200, 257) != 0);
	CHECK(ps_archive_progress_begin(&s, 3, 1, 4, 110, UINT64_MAX - 4, 8) != 0);
	CHECK(ps_archive_progress_pin(&s, 3, 1, 3, 110, 180, 256) != 0);
	puts("archive progress transition tests passed");
	return 0;
}
