#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "pagestore_archive_progress_format.h"
#include "pagestore_archive_progress_store.h"
#include "pagestore_storage.h"
#include "pagestore_test_temp.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static int begin_second(PsArchiveProgress *s, void *arg)
{
	(void) arg;
	if (ps_archive_progress_begin(s, 3, 1, 2, 100, 192, 64) != 0)
		return -1;
	s->start_nonce_hi = 2;
	return 0;
}
static int begin_first(PsArchiveProgress *s, void *arg)
{
	(void) arg;
	if (ps_archive_progress_begin(s, 3, 1, 1, 100, 192, 64) != 0)
		return -1;
	s->start_nonce_hi = 1;
	return 0;
}
static int stale_safe(PsArchiveProgress *s, void *arg)
{
	(void) arg;
	return ps_archive_progress_safe(s, 3, 1, 1, 100, 180);
}
static int begin_third(PsArchiveProgress *s, void *arg)
{
	(void) arg;
	if (ps_archive_progress_begin(s, 3, 1, 3, 110, 192, 64) != 0)
		return -1;
	s->start_nonce_hi = 3;
	return 0;
}
/* Emulate a provider that committed the replacement but lost its reply. */
static int persist_then_fail(uint32_t tl, const void *buf, uint32_t len)
{
	if (PsStoragePosix.archive_progress_replace(tl, buf, len) != 0)
		return -1;
	errno = EIO;
	return -1;
}
typedef struct RegistryRequest
{
	PsArchiveProgressRegistry *registry;
	uint64_t target;
	int result;
} RegistryRequest;
typedef struct StartRequest
{
	PsArchiveProgressRegistry *registry;
	uint64_t nonce;
	int result;
} StartRequest;
typedef struct MarkerAuthority
{
	pthread_mutex_t lock;
	uint64_t marker;
	uint64_t incarnation;
	int fail;
	int calls;
} MarkerAuthority;
static int marker_authority(void *arg, uint32_t timeline,
						   uint64_t *marker, uint64_t *incarnation)
{
	MarkerAuthority *authority = arg;
	(void) timeline;
	pthread_mutex_lock(&authority->lock);
	authority->calls++;
	if (authority->fail)
	{
		pthread_mutex_unlock(&authority->lock);
		return -1;
	}
	*marker = authority->marker;
	*incarnation = authority->incarnation;
	return 0;
}
static void marker_release(void *arg, uint32_t timeline)
{
	MarkerAuthority *authority = arg;
	(void) timeline;
	pthread_mutex_unlock(&authority->lock);
}
static void *start_worker(void *arg)
{
	StartRequest *request = arg;
	request->result = ps_archive_progress_registry_start(request->registry, 5,
								  0, request->nonce, 99, 99, 100, 0, 0, NULL);
	return NULL;
}
static int registry_begin(PsArchiveProgress *s, void *arg)
{
	(void) arg;
	if (ps_archive_progress_begin(s, 4, 1, 1, 100, 192, 64) != 0)
		return -1;
	s->start_nonce_hi = 1;
	return 0;
}
static int registry_safe(PsArchiveProgress *s, void *arg)
{
	(void) arg;
	return ps_archive_progress_safe(s, 4, 1, 1, 100, 180);
}
static int registry_freeze(PsArchiveProgress *s, void *arg)
{
	(void) arg;
	return ps_archive_progress_freeze_budget(s, 4, 1, 1, 100, 192, 64);
}
static int registry_prepare(PsArchiveProgress *s, void *arg)
{
	return ps_archive_progress_prepare(s, 4, 1, 1, 100, 1, *(uint64_t *) arg, 256);
}
static void *registry_worker(void *arg)
{
	RegistryRequest *request = arg;
	request->result = ps_archive_progress_registry_apply(request->registry, 4,
														 registry_prepare,
														 &request->target, NULL);
	return NULL;
}
static int crash_begin(PsArchiveProgress *s, void *arg)
{
	(void) arg;
	if (ps_archive_progress_begin(s, 6, 1, 1, 100, 192, 64) != 0)
		return -1;
	s->start_nonce_hi = 1;
	return 0;
}
static int crash_safe(PsArchiveProgress *s, void *arg)
{
	(void) arg;
	return ps_archive_progress_safe(s, 6, 1, 1, 100, 180);
}
static int crash_freeze(PsArchiveProgress *s, void *arg)
{
	(void) arg;
	return ps_archive_progress_freeze_budget(s, 6, 1, 1, 100, 192, 64);
}
static int crash_pin(PsArchiveProgress *s, void *arg)
{
	(void) arg;
	return ps_archive_progress_prepare(s, 6, 1, 1, 100, 1, 200, 256);
}
int main(void)
{
	char dir[PS_TEST_TEMP_PATH_MAX] = "psarchiveprogressXXXXXX";
	char path[1024];
	unsigned char encoded[PS_ARCHIVE_PROGRESS_FORMAT_BYTES], readbuf[sizeof(encoded)];
	PsArchiveProgress state = {0}, recovered = {0};
	PsArchiveProgressStore progress;
	PsArchiveProgressRegistry registry;
	MarkerAuthority authority = {PTHREAD_MUTEX_INITIALIZER, 99, 100, 0, 0};
	PsStorage failing = PsStoragePosix;
	pthread_t ta, tb;
	RegistryRequest a = {&registry, 200, -1}, b = {&registry, 201, -1};
	StartRequest sa = {&registry, 1, -1}, sb = {&registry, 2, -1};
	int fd;
	pid_t child;
	int status;
	CHECK(ps_test_mkdtemp(dir, sizeof(dir)) != NULL);
	CHECK(PsStoragePosix.open(dir, 0) == 0);
	CHECK(PsStoragePosix.archive_progress_read(3, readbuf, sizeof(readbuf)) == 1);
	CHECK(ps_archive_progress_begin(&state, 3, 1, 1, 100, 192, 64) == 0);
	state.start_nonce_hi = 1;
	CHECK(ps_archive_progress_safe(&state, 3, 1, 1, 100, 180) == 0);
	CHECK(ps_archive_progress_freeze_budget(&state, 3, 1, 1, 100, 192, 64) == 0);
	CHECK(ps_archive_progress_prepare(&state, 3, 1, 1, 100, 1, 200, 256) == 0);
	CHECK(ps_archive_progress_activate(&state, 3, 1, 1, 100, 1, 200, 256) == 0);
	CHECK(ps_archive_progress_encode(&state, encoded, sizeof(encoded)) == 0);
	CHECK(PsStoragePosix.archive_progress_replace(3, encoded, sizeof(encoded)) == 0);
	PsStoragePosix.close();
	CHECK(PsStoragePosix.open(dir, 0) == 0);
	CHECK(PsStoragePosix.archive_progress_read(3, readbuf, sizeof(readbuf)) == 0);
	CHECK(ps_archive_progress_decode(&recovered, readbuf, sizeof(readbuf)) == 0);
	CHECK(recovered.attempt == 1 && recovered.target == 200 && recovered.grant_end == 256 &&
		  recovered.budget_frozen && recovered.original_limit == 192 &&
		  recovered.max_extra == 64);
	CHECK(ps_archive_progress_store_open(&progress, &PsStoragePosix, 3) == 0);
	CHECK(ps_archive_progress_txn_apply(&progress.transaction, begin_second, NULL, &recovered) == 0);
	CHECK(recovered.attempt == 2 && recovered.safe_lsn == 0 && recovered.target == 200);
	CHECK(ps_archive_progress_txn_apply(&progress.transaction, stale_safe, NULL, NULL) != 0);
	ps_archive_progress_store_close(&progress);
	PsStoragePosix.close();
	CHECK(PsStoragePosix.open(dir, 0) == 0);
	CHECK(ps_archive_progress_store_open(&progress, &PsStoragePosix, 3) == 0);
	CHECK(progress.transaction.state.attempt == 2 && progress.transaction.state.target == 200);
	ps_archive_progress_store_close(&progress);
	failing.archive_progress_replace = persist_then_fail;
	CHECK(ps_archive_progress_store_open(&progress, &failing, 3) == 0);
	CHECK(ps_archive_progress_txn_apply(&progress.transaction, begin_third, NULL, NULL) != 0);
	CHECK(progress.transaction.state.attempt == 2 && progress.transaction.poisoned);
	CHECK(ps_archive_progress_txn_apply(&progress.transaction, begin_second, NULL, NULL) != 0);
	ps_archive_progress_store_close(&progress);
	CHECK(ps_archive_progress_store_open(&progress, &PsStoragePosix, 3) == 0);
	CHECK(progress.transaction.state.attempt == 3 && progress.transaction.state.marker == 110);
	ps_archive_progress_store_close(&progress);
	CHECK(ps_archive_progress_registry_open(&registry, &PsStoragePosix, 8) == 0);
	CHECK(ps_archive_progress_registry_start(&registry, 7, 0, 42, 43,
		  99, 100, 0, 0, NULL) != 0);
	CHECK(ps_archive_progress_registry_set_marker_authority(&registry,
		  marker_authority, marker_release, &authority) == 0);
	CHECK(ps_archive_progress_registry_apply(&registry, 4, registry_begin, NULL, NULL) == 0);
	CHECK(ps_archive_progress_registry_apply(&registry, 4, registry_safe, NULL, NULL) == 0);
	CHECK(ps_archive_progress_registry_apply(&registry, 4, registry_freeze, NULL, NULL) == 0);
	CHECK(pthread_create(&ta, NULL, registry_worker, &a) == 0);
	CHECK(pthread_create(&tb, NULL, registry_worker, &b) == 0);
	CHECK(pthread_join(ta, NULL) == 0 && pthread_join(tb, NULL) == 0);
	CHECK((a.result == 0) != (b.result == 0));
	CHECK(ps_archive_progress_registry_apply(&registry, 8, registry_begin, NULL, NULL) != 0);
	ps_archive_progress_registry_close(&registry);
	CHECK(ps_archive_progress_registry_open(&registry, &PsStoragePosix, 8) == 0);
	CHECK(ps_archive_progress_registry_set_marker_authority(&registry,
		  marker_authority, marker_release, &authority) == 0);
	CHECK(ps_archive_progress_registry_apply(&registry, 4, registry_safe, NULL, &recovered) == 0);
	CHECK(recovered.target == (a.result == 0 ? a.target : b.target));
	CHECK(ps_archive_progress_registry_start(&registry, 7, 0, 42, 43, 100, 100, 0, 0, NULL) != 0);
	authority.fail = 1;
	CHECK(ps_archive_progress_registry_start(&registry, 7, 0, 42, 43, 99, 100, 0, 0, NULL) != 0);
	authority.fail = 0;
	CHECK(ps_archive_progress_registry_start(&registry, 7, 0, 42, 43, 99, 100, 0, 0, &recovered) == 0);
	CHECK(recovered.attempt == 1 && recovered.start_nonce_hi == 42 && recovered.start_nonce_lo == 43);
	CHECK(ps_archive_progress_registry_safe(&registry, 7, 100, 1,
		  99, 180, &recovered) == 0 && recovered.safe_lsn == 180);
	CHECK(ps_archive_progress_registry_freeze_budget(&registry, 7, 100, 1,
		  99, 192, 64, &recovered) == 0 && recovered.budget_frozen);
	CHECK(ps_archive_progress_registry_freeze_budget(&registry, 7, 100, 1,
		  99, 193, 64, NULL) != 0);
	CHECK(ps_archive_progress_registry_prepare(&registry, 7, 100, 1,
		  99, 1, 200, 256, &recovered) == 0 && recovered.target == 200 &&
		  recovered.grant_end == 256);
	CHECK(ps_archive_progress_registry_activate(&registry, 7, 100, 1,
		  99, 1, 200, 256, &recovered) == 0 && recovered.grant_active);
	CHECK(ps_archive_progress_registry_safe(&registry, 7, 101, 1,
		  99, 181, NULL) != 0); /* mismatched incarnation */
	CHECK(ps_archive_progress_registry_snapshot(&registry, 7, &recovered) == 0 && recovered.attempt == 1);
	CHECK(ps_archive_progress_registry_snapshot(&registry, 8, &recovered) != 0);
	pthread_mutex_lock(&authority.lock);
	authority.marker = 101;
	pthread_mutex_unlock(&authority.lock);
	CHECK(ps_archive_progress_registry_start(&registry, 7, 0, 42, 43, 99, 100, 0, 0, &recovered) == 0);
	CHECK(ps_archive_progress_registry_safe(&registry, 7, 100, 1,
		  99, 181, NULL) != 0); /* marker moved after the request */
	CHECK(ps_archive_progress_registry_start(&registry, 7, 1, 44, 45, 99, 100, 0, 0, NULL) != 0);
	pthread_mutex_lock(&authority.lock);
	authority.marker = 99;
	pthread_mutex_unlock(&authority.lock);
	CHECK(ps_archive_progress_registry_start(&registry, 7, 0, 42, 43, 99, 100, 0, 0, &recovered) == 0);
	CHECK(recovered.attempt == 1);
	CHECK(ps_archive_progress_registry_start(&registry, 7, 0, 42, 44, 99, 100, 0, 0, NULL) != 0);
	CHECK(ps_archive_progress_registry_start(&registry, 7, 1, 42, 43, 99, 100, 0, 0, NULL) != 0);
	ps_archive_progress_registry_close(&registry);
	CHECK(ps_archive_progress_registry_open(&registry, &PsStoragePosix, 8) == 0);
	CHECK(ps_archive_progress_registry_set_marker_authority(&registry,
		  marker_authority, marker_release, &authority) == 0);
	CHECK(ps_archive_progress_registry_start(&registry, 7, 0, 42, 43, 99, 100, 0, 0, &recovered) == 0);
	CHECK(recovered.attempt == 1);
	CHECK(ps_archive_progress_registry_start(&registry, 7, 1, 44, 45, 99, 100, 0, 0, &recovered) == 0);
	CHECK(recovered.attempt == 2 && recovered.budget_frozen &&
		  recovered.target == 200 && recovered.grant_end == 256 &&
		  recovered.original_limit == 192 && recovered.max_extra == 64);
	CHECK(ps_archive_progress_registry_safe(&registry, 7, 100, 2,
		  99, 181, &recovered) == 0 && recovered.safe_lsn == 181);
	CHECK(ps_archive_progress_registry_freeze_budget(&registry, 7, 100, 2,
		  99, 193, 64, NULL) != 0); /* a changed writer GUC cannot ratchet it */
	CHECK(ps_archive_progress_registry_start(&registry, 7, 0, 42, 43, 99, 100, 0, 0, NULL) != 0);
	authority.incarnation++;
	CHECK(ps_archive_progress_registry_start(&registry, 7, 2, 46, 47, 99, 101, 0, 0, NULL) != 0);
	authority.incarnation--;
	CHECK(pthread_create(&ta, NULL, start_worker, &sa) == 0);
	CHECK(pthread_create(&tb, NULL, start_worker, &sb) == 0);
	CHECK(pthread_join(ta, NULL) == 0 && pthread_join(tb, NULL) == 0);
	CHECK((sa.result == 0) != (sb.result == 0));
	CHECK(ps_archive_progress_registry_start(&registry, 5, 0,
		  sa.result == 0 ? sa.nonce : sb.nonce, 99, 99, 100, 0, 0, &recovered) == 0);
	CHECK(recovered.attempt == 1);
	ps_archive_progress_registry_close(&registry);
	CHECK(PsStoragePosix.archive_progress_read(0, readbuf, sizeof(readbuf)) == 1);
	CHECK(PsStoragePosix.archive_progress_read(3, readbuf, sizeof(readbuf) - 1) != 0);
	snprintf(path, sizeof(path), "%s/archive_progress_3", dir);
	fd = open(path, O_WRONLY);
	CHECK(fd >= 0 && pwrite(fd, "x", 1, 24) == 1 && close(fd) == 0);
	CHECK(PsStoragePosix.archive_progress_read(3, readbuf, sizeof(readbuf)) == 0);
	CHECK(ps_archive_progress_decode(&recovered, readbuf, sizeof(readbuf)) != 0);
	CHECK(unlink(path) == 0 && symlink("/dev/null", path) == 0);
	CHECK(PsStoragePosix.archive_progress_read(3, readbuf, sizeof(readbuf)) != 0);
	CHECK(unlink(path) == 0);
	PsStoragePosix.close();
	/* The process exits without closing either the registry or the provider.
	 * The store-owner lease is then recovered by a different process. */
	child = fork();
	CHECK(child >= 0);
	if (child == 0)
	{
		PsArchiveProgressRegistry r;
		if (PsStoragePosix.open(dir, 0) != 0 ||
			ps_archive_progress_registry_open(&r, &PsStoragePosix, 8) != 0 ||
			ps_archive_progress_registry_apply(&r, 6, crash_begin, NULL, NULL) != 0 ||
			ps_archive_progress_registry_apply(&r, 6, crash_safe, NULL, NULL) != 0 ||
			ps_archive_progress_registry_apply(&r, 6, crash_freeze, NULL, NULL) != 0 ||
			ps_archive_progress_registry_apply(&r, 6, crash_pin, NULL, NULL) != 0)
			_exit(1);
		_exit(0);
	}
	CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	CHECK(PsStoragePosix.open(dir, 0) == 0);
	CHECK(ps_archive_progress_registry_open(&registry, &PsStoragePosix, 8) == 0);
	CHECK(ps_archive_progress_registry_apply(&registry, 6, crash_safe, NULL, &recovered) == 0);
	CHECK(recovered.attempt == 1 && recovered.target == 200 && recovered.grant_end == 256);
	ps_archive_progress_registry_close(&registry);
	PsStoragePosix.close();
	/* A rename can be visible despite a failed directory fsync. Reopening only
	 * the progress store must not clear the fence until reconciliation works. */
	CHECK(setenv("PAGESTORE_TEST_FAIL_ARCHIVE_PROGRESS_READ_DIR_FSYNC", "2", 1) == 0);
	CHECK(setenv("PAGESTORE_TEST_FAIL_ARCHIVE_PROGRESS_REPLACE_DIR_FSYNC", "1", 1) == 0);
	CHECK(PsStoragePosix.open(dir, 0) == 0);
	CHECK(ps_archive_progress_store_open(&progress, &PsStoragePosix, 3) == 0);
	CHECK(ps_archive_progress_txn_apply(&progress.transaction, begin_first, NULL, NULL) != 0);
	CHECK(progress.transaction.poisoned);
	ps_archive_progress_store_close(&progress);
	CHECK(ps_archive_progress_store_open(&progress, &PsStoragePosix, 3) != 0);
	CHECK(ps_archive_progress_store_open(&progress, &PsStoragePosix, 3) == 0);
	CHECK(progress.transaction.state.attempt == 1 && progress.transaction.state.marker == 100);
	ps_archive_progress_store_close(&progress);
	PsStoragePosix.close();
	CHECK(unsetenv("PAGESTORE_TEST_FAIL_ARCHIVE_PROGRESS_READ_DIR_FSYNC") == 0);
	CHECK(unsetenv("PAGESTORE_TEST_FAIL_ARCHIVE_PROGRESS_REPLACE_DIR_FSYNC") == 0);
	ps_test_remove_tree(dir);
	puts("archive progress storage tests passed");
	return 0;
}
