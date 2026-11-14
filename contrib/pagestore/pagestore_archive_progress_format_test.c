#include <stdio.h>
#include <string.h>
#include "pagestore_archive_progress_format.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(void)
{
	PsArchiveProgress s = {0}, recovered = {0};
	unsigned char buf[PS_ARCHIVE_PROGRESS_FORMAT_BYTES];
	CHECK(ps_archive_progress_begin(&s, 3, 1, 1, 100, 192, 64) == 0);
	s.start_nonce_hi = 1;
	CHECK(ps_archive_progress_safe(&s, 3, 1, 1, 100, 180) == 0);
	CHECK(ps_archive_progress_freeze_budget(&s, 3, 1, 1, 100, 192, 64) == 0);
	CHECK(ps_archive_progress_freeze_budget(&s, 3, 1, 1, 100, 192, 64) == 0);
	CHECK(ps_archive_progress_prepare(&s, 3, 1, 1, 100, 1, 200, 256) == 0);
	CHECK(ps_archive_progress_activate(&s, 3, 1, 1, 100, 1, 200, 256) == 0);
	CHECK(ps_archive_progress_encode(&s, buf, sizeof(buf)) == 0);
	CHECK(ps_archive_progress_decode(&recovered, buf, sizeof(buf)) == 0);
	CHECK(recovered.timeline == 3 && recovered.incarnation == 1 &&
		  recovered.attempt == 1 && recovered.marker == 100 &&
		  recovered.safe_lsn == 180 && recovered.target == 200 && recovered.grant_end == 256 &&
		  recovered.original_limit == 192 && recovered.max_extra == 64 &&
		  recovered.budget_frozen == 1 && recovered.grant_active == 1 &&
		  recovered.candidate_wal_tli == 1);
	for (unsigned i = 0; i < sizeof(buf); i++) {
		buf[i] ^= 1;
		CHECK(ps_archive_progress_decode(&recovered, buf, sizeof(buf)) != 0);
		buf[i] ^= 1;
	}
	for (unsigned n = 0; n < sizeof(buf); n++)
		CHECK(ps_archive_progress_decode(&recovered, buf, n) != 0);
	ps_ap_put32(buf + 4, 1); /* old formats are never accepted as current */
	ps_ap_put32(buf + 100, ps_ap_checksum(buf, 100));
	CHECK(ps_archive_progress_decode(&recovered, buf, sizeof(buf)) != 0);
	ps_ap_put32(buf + 4, 2);
	ps_ap_put32(buf + 100, ps_ap_checksum(buf, 100));
	CHECK(ps_archive_progress_decode(&recovered, buf, sizeof(buf)) != 0);
	ps_ap_put32(buf + 4, PS_ARCHIVE_PROGRESS_FORMAT_VERSION);
	ps_ap_put64(buf + 56, 200); /* invalid bound with a correct checksum */
	ps_ap_put32(buf + 100, ps_ap_checksum(buf, 100));
	CHECK(ps_archive_progress_decode(&recovered, buf, sizeof(buf)) != 0);
	puts("archive progress format tests passed");
	return 0;
}
