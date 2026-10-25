/* Test-only subprocess driver for post-rename directory fsync failure. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pagestore_storage.h"

static void
require(int ok, const char *message)
{
	if (!ok)
	{
		fprintf(stderr, "FAIL: %s\n", message);
		exit(1);
	}
}

int
main(int argc, char **argv)
{
	char bytes[32] = {0};
	int fd;

	require(argc == 4, "usage: probe stage store arm");
	require(PsStoragePosix.open(argv[2], 0) == 0, "open owned store");
	if (strcmp(argv[1], "publish") == 0)
	{
		require(PsStoragePosix.fork_meta_append("old-record", 10) == 0,
				"acknowledge original metadata");
		fd = open(argv[3], O_CREAT | O_WRONLY, 0600);
		require(fd >= 0 && close(fd) == 0, "arm directory fault");
		fd = open(argv[2], O_RDONLY | O_DIRECTORY);
		require(fd >= 0 && fsync(fd) == 0 && close(fd) == 0,
				"directory sync before publication remains healthy");
		require(PsStoragePosix.fork_meta_rewrite("replacement-record", 18) != 0,
				"post-rename directory sync failure is not acknowledged");
		require(PsStoragePosix.fork_meta_read(0, bytes, sizeof(bytes)) == 18 &&
				memcmp(bytes, "replacement-record", 18) == 0,
				"failed publication may expose the complete replacement");
		/* Deliberately skip backend close: no graceful cleanup or later sync. */
		_exit(86);
	}
	require(strcmp(argv[1], "recover") == 0, "known probe stage");
	require(PsStoragePosix.fork_meta_read(0, bytes, sizeof(bytes)) == 18 &&
			memcmp(bytes, "replacement-record", 18) == 0,
			"reopen sees a complete replacement, not a mixed or partial log");
	require(PsStoragePosix.fork_meta_rewrite("replacement-record", 18) == 0,
			"retry completes publication durability");
	require(PsStoragePosix.fork_meta_append("+next", 5) == 0,
			"subsequent metadata append succeeds");
	PsStoragePosix.close();
	require(PsStoragePosix.open(argv[2], 0) == 0,
			"reopen acknowledged retry and append");
	require(PsStoragePosix.fork_meta_read(0, bytes, sizeof(bytes)) == 23 &&
			memcmp(bytes, "replacement-record+next", 23) == 0,
			"acknowledged replacement and append survive reopen");
	PsStoragePosix.close();
	return 0;
}
