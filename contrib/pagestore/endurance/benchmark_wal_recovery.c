/* Benchmark recovery of a disposable copy of an immutable WAL store. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../pagestore_wal_store.h"

int
main(int argc, char **argv)
{
	PsWalStore store;
	struct timespec start, end;
	unsigned long timeline, segment_size;
	int rc;

	if (argc != 4)
	{
		fprintf(stderr, "usage: %s DISPOSABLE_STORE TIMELINE SEGMENT_SIZE\n", argv[0]);
		return 2;
	}
	timeline = strtoul(argv[2], NULL, 10);
	segment_size = strtoul(argv[3], NULL, 10);
	if (timeline > UINT32_MAX || segment_size == 0 || segment_size > UINT32_MAX)
		return 2;
	clock_gettime(CLOCK_MONOTONIC, &start);
	rc = ps_wal_store_open_existing(&store, argv[1], (uint32_t) timeline,
								(uint32_t) segment_size);
	clock_gettime(CLOCK_MONOTONIC, &end);
	printf("rc=%d elapsed_seconds=%.3f segments=%u\n", rc,
		   (double) (end.tv_sec - start.tv_sec) +
		   (double) (end.tv_nsec - start.tv_nsec) / 1000000000.0,
		   rc == 0 ? store.nentries : 0);
	if (rc == 0)
		ps_wal_store_close(&store);
	return rc != 0;
}
