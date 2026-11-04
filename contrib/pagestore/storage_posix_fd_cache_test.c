#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include "pagestore_test_temp.h"
#include "pagestore_storage.h"

#define SEGMENTS 512
#define SHARDS 4
static int	checks,
			failures;
static void
check(int ok, const char *what)
{
	checks++;
	if (!ok)
	{
		failures++;
		fprintf(stderr, "FAIL: %s (errno=%d)\n", what, errno);
	}
}
static int
fd_count(void)
{
	struct rlimit limit;
	int			n = 0;

	if (getrlimit(RLIMIT_NOFILE, &limit) != 0)
		return -1;
	for (int fd = 0; fd < (int) limit.rlim_cur; fd++)
		if (fcntl(fd, F_GETFD) != -1)
			n++;
	return n;
}
static void
payload(uint32_t out[16], uint32_t shard, int seg)
{
	for (int i = 0; i < 16; i++)
		out[i] = shard * 1000000u + (uint32_t) seg * 16u + (uint32_t) i;
}
static void *
reader(void *arg)
{
	uint32_t	shard = (uint32_t) (uintptr_t) arg;

	for (int i = 0; i < 2048; i++)
	{
		uint32_t	want[16],
					got[16];
		int			seg = (i * 17) % (SEGMENTS / SHARDS);

		payload(want, shard, seg);
		if (PsStoragePosix.seg_read(shard, seg, 0, got, sizeof(got)) != 0 ||
			memcmp(want, got, sizeof(got)) != 0)
			return (void *) 1;
	}
	return NULL;
}
static pthread_mutex_t pause_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pause_changed = PTHREAD_COND_INITIALIZER;
static int	pause_read;
static int	read_entered;
static int pause_write;
static int write_entered;

void ps_test_posix_write_begun(uint32_t shard, int seg);
void
ps_test_posix_write_begun(uint32_t shard, int seg)
{
	if (shard != 0 || seg != 0)
		return;
	pthread_mutex_lock(&pause_lock);
	if (pause_write)
	{
		write_entered = 1;
		pthread_cond_broadcast(&pause_changed);
		while (pause_write)
			pthread_cond_wait(&pause_changed, &pause_lock);
	}
	pthread_mutex_unlock(&pause_lock);
}

static void *
paused_writer(void *unused)
{
	uint32_t data[16];

	(void) unused;
	payload(data, 0, 10000);
	return PsStoragePosix.seg_write(0, 0, 0, data, sizeof(data)) == 0 ? NULL : (void *) 1;
}

static void
active_write_token_test(void)
{
	pthread_t thread;
	struct timespec deadline;
	void *result = NULL;
	uint64_t token;
	int entered;

	pthread_mutex_lock(&pause_lock);
	pause_write = 1;
	write_entered = 0;
	pthread_mutex_unlock(&pause_lock);
	if (pthread_create(&thread, NULL, paused_writer, NULL) != 0)
	{
		pthread_mutex_lock(&pause_lock);
		pause_write = 0;
		pthread_mutex_unlock(&pause_lock);
		check(0, "start token test writer");
		return;
	}
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 5;
	pthread_mutex_lock(&pause_lock);
	while (!write_entered)
		if (pthread_cond_timedwait(&pause_changed, &pause_lock, &deadline) != 0)
			break;
	entered = write_entered;
	pthread_mutex_unlock(&pause_lock);
	check(entered && PsStoragePosix.seg_change_token(0, 0, &token) != 0,
		  "a token is unavailable during an in-flight segment write");
	pthread_mutex_lock(&pause_lock);
	pause_write = 0;
	pthread_cond_broadcast(&pause_changed);
	pthread_mutex_unlock(&pause_lock);
	check(pthread_join(thread, &result) == 0 && result == NULL &&
		  PsStoragePosix.seg_change_token(0, 0, &token) == 0,
		  "the token is available again after the writer finishes");
}

void		ps_test_posix_read_pinned(uint32_t shard, int seg);
void
ps_test_posix_read_pinned(uint32_t shard, int seg)
{
	if (shard != 0 || seg != 0)
		return;
	pthread_mutex_lock(&pause_lock);
	if (pause_read)
	{
		read_entered = 1;
		pthread_cond_broadcast(&pause_changed);
		while (pause_read)
			pthread_cond_wait(&pause_changed, &pause_lock);
	}
	pthread_mutex_unlock(&pause_lock);
}

static void *
pinned_reader(void *unused)
{
	uint32_t	got[16],
				want[16];

	(void) unused;
	payload(want, 0, 10000);
	return PsStoragePosix.seg_read(0, 0, 0, got, sizeof(got)) == 0 &&
		memcmp(got, want, sizeof(got)) == 0 ? NULL : (void *) 1;
}

static void
pinned_eviction_test(void)
{
	pthread_t	thread;
	struct timespec deadline;
	void	   *result = NULL;
	int			entered;
	uint32_t	data[16];

	pthread_mutex_lock(&pause_lock);
	pause_read = 1;
	read_entered = 0;
	pthread_mutex_unlock(&pause_lock);
	if (pthread_create(&thread, NULL, pinned_reader, NULL) != 0)
	{
		check(0, "start pinned reader");
		return;
	}
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 5;
	pthread_mutex_lock(&pause_lock);
	while (!read_entered)
		if (pthread_cond_timedwait(&pause_changed, &pause_lock, &deadline) != 0)
			break;
	entered = read_entered;
	pthread_mutex_unlock(&pause_lock);
	check(entered, "pause reader after pinning its descriptor");
	for (int i = 1; i < SEGMENTS; i++)
		if (PsStoragePosix.seg_read((uint32_t) (i % SHARDS), i / SHARDS,
									0, data, sizeof(data)) != 0)
		{
			check(0, "rotate cache while one descriptor is pinned");
			break;
		}
	pthread_mutex_lock(&pause_lock);
	pause_read = 0;
	pthread_cond_broadcast(&pause_changed);
	pthread_mutex_unlock(&pause_lock);
	check(pthread_join(thread, &result) == 0 && result == NULL,
		  "pinned reader survives a full cache rotation with correct data");
}


int
main(void)
{
	char		dir[PS_TEST_TEMP_PATH_MAX] = "psfdcacheXXXXXX";
	struct rlimit original,
				limited;
	uint32_t	data[16],
				got[16];
	int			filled = 1;

	check(getrlimit(RLIMIT_NOFILE, &original) == 0, "read descriptor limit");
	limited = original;
	limited.rlim_cur = 128;
	check(setrlimit(RLIMIT_NOFILE, &limited) == 0, "use an unchanged low descriptor ceiling");
	check(ps_test_mkdtemp(dir, sizeof(dir)) != NULL && PsStoragePosix.open(dir, 65536) == 0, "open independent store");
	{
		uint64_t initial = 99, changed = 0, other = 99, removed = 0;

		check(PsStoragePosix.seg_change_token(0, 0, &initial) == 0 && initial == 0 &&
			  PsStoragePosix.seg_change_token(1, 0, &other) == 0 && other == 0,
			  "untouched segments have independent initial change tokens");
		memset(data, 0x34, sizeof(data));
		check(PsStoragePosix.seg_write(0, 0, 0, data, sizeof(data)) == 0 &&
			  PsStoragePosix.seg_change_token(0, 0, &changed) == 0 && changed != initial &&
			  PsStoragePosix.seg_change_token(1, 0, &other) == 0 && other == 0,
			  "segment write changes only its own token");
		check(PsStoragePosix.seg_remove(0, 0) == 0 &&
			  PsStoragePosix.seg_change_token(0, 0, &removed) == 0 && removed != changed &&
			  PsStoragePosix.seg_write(0, 0, 0, data, sizeof(data)) == 0 &&
			  PsStoragePosix.seg_change_token(0, 0, &changed) == 0 && changed != removed,
			  "remove and recreation both invalidate the old token");
	}
	check(PsStoragePosix.wal_append(0, "healthy", 7, NULL, 0) == 0, "store independent healthy WAL");
	for (int i = 0; i < SEGMENTS; i++)
	{
		payload(data, (uint32_t) (i % SHARDS), i / SHARDS);
		if (PsStoragePosix.seg_write((uint32_t) (i % SHARDS), i / SHARDS, 0, data, sizeof(data)) != 0)
		{
			fprintf(stderr, "segment write failed at %d: errno=%d\n", i, errno);
			filled = 0;
			break;
		}
	}
	check(filled, "touch 512 retained segments below a 128-FD limit");
	{
		char		wal[7];

		check(PsStoragePosix.wal_read(0, 0, wal, sizeof(wal)) == 7 && !memcmp(wal, "healthy", 7),
			  "segment history leaves descriptors for WAL reads");
	}
	check(fd_count() > 0 && fd_count() < 100, "cached descriptors stay bounded with open headroom");
	if (filled)
	{
		pthread_t	threads[SHARDS];
		int			started[SHARDS] = {0};

		for (uint32_t i = 0; i < SHARDS; i++)
		{
			started[i] = pthread_create(&threads[i], NULL, reader, (void *) (uintptr_t) i) == 0;
			check(started[i], "start concurrent eviction reader");
		}
		for (uint32_t i = 0; i < SHARDS; i++)
			if (started[i])
			{
				void	   *result = NULL;

				check(pthread_join(threads[i], &result) == 0 && result == NULL, "eviction never closes another reader's descriptor");
			}
		check(PsStoragePosix.sync() == 0, "sync also covers writes whose cache entries were evicted");
		check(PsStoragePosix.seg_remove(0, 0) == 0, "remove a cached or evicted segment");
		payload(data, 0, 10000);
		check(PsStoragePosix.seg_write(0, 0, 0, data, sizeof(data)) == 0 &&
			  PsStoragePosix.seg_read(0, 0, 0, got, sizeof(got)) == 0 && !memcmp(data, got, sizeof(data)),
			  "recreated segment cannot reuse an unlinked cached file");
		active_write_token_test();
		pinned_eviction_test();
	}
	PsStoragePosix.close();
	{
		uint64_t before = 0, after = 0;

		check(setenv("PAGESTORE_TEST_FAIL_SEG_WRITES", "1", 1) == 0 &&
			  PsStoragePosix.open(dir, 65536) == 0 &&
			  PsStoragePosix.seg_change_token(0, 0, &before) == 0,
			  "arm a failing segment write with a fresh token");
		check(PsStoragePosix.seg_write(0, 0, 0, data, sizeof(data)) != 0 &&
			  PsStoragePosix.seg_change_token(0, 0, &after) == 0 && after != before,
			  "even a failed segment write invalidates its token");
		PsStoragePosix.close();
		unsetenv("PAGESTORE_TEST_FAIL_SEG_WRITES");
	}
	if (filled)
	{
		int			failed_at = -1,
					before = 0;

		check(setenv("PAGESTORE_TEST_FAIL_SEG_CACHE_SYNC", "1", 1) == 0 &&
			  PsStoragePosix.open(dir, 65536) == 0, "arm one eviction fsync failure");
		{
			uint64_t token = 99;

			check(PsStoragePosix.seg_change_token(0, 0, &token) == 0 && token == 0,
				  "reopen resets process-local segment change tokens");
		}
		for (int i = 0; i < SEGMENTS; i++)
		{
			payload(data, 0, i);
			before = fd_count();
			if (PsStoragePosix.seg_write(0, i, 0, data, sizeof(data)) != 0)
			{
				failed_at = i;
				break;
			}
		}
		check(failed_at >= 0 && errno == EIO, "dirty cache eviction propagates fsync failure");
		check(fd_count() == before, "failed fsync retains the dirty descriptor for retry");
		if (failed_at >= 0)
			check(PsStoragePosix.seg_write(0, failed_at, 0, data, sizeof(data)) == 0 &&
				  PsStoragePosix.sync() == 0, "dirty descriptor remains retryable after failed eviction");
		PsStoragePosix.close();
		unsetenv("PAGESTORE_TEST_FAIL_SEG_CACHE_SYNC");
	}
	check(setrlimit(RLIMIT_NOFILE, &original) == 0, "restore original descriptor limit");
	ps_test_remove_tree(dir);
	fprintf(stderr, "%d checks, %d failures\n", checks, failures);
	return failures != 0;
}
