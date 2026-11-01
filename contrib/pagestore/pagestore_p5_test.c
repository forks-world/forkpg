/* Finite pins must freeze rewrites below R, and registration must be atomic. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "pagestore_test_temp.h"
#include "pagestore_core.h"
#include "pagestore_retention.h"
#include "pagestore_fault.h"

static const char *test_program;

static int	failures,
			checks;
static void
check(int ok, const char *message)
{
	checks++;
	if (!ok)
	{
		fprintf(stderr, "FAIL: %s\n", message);
		failures++;
	}
}
static void
write_value(const PsKey *key, uint64_t lsn, unsigned char value)
{
	unsigned char page[8192];

	memset(page, value, sizeof(page));
	if (key->klass == PS_KLASS_RELATION)
	{
		uint32_t	hi = lsn >> 32,
					lo = (uint32_t) lsn;

		memcpy(page, &hi, 4);
		memcpy(page + 4, &lo, 4);
	}
	ps_lock_shard_wr(ps_shard_of(key));
	check(append_page(0, key, 0, page, lsn, NULL) == 0, "append version");
	ps_unlock_shard(ps_shard_of(key));
}
static int
read_value(const PsKey *key, uint64_t lsn, uint64_t seq, int value)
{
	unsigned char page[8192];
	int			rc;
	uint64_t	ver = 0,
				resolved_seq = 0;

	ps_lock_shard_rd(ps_shard_of(key));
	rc = read_resolve_version(0, key, 0, lsn, seq, page, &ver, &resolved_seq);
	ps_unlock_shard(ps_shard_of(key));
	if (!(value < 0 ? rc == value : rc == 1 && page[16] == value))
		fprintf(stderr, "read class=%u R=%llu S=%llu rc=%d value=%x expected=%x\n", key->klass, (unsigned long long) lsn, (unsigned long long) seq, rc, page[16], value);
	if (rc == 0)
		fprintf(stderr, "resolved=%llu/%llu\n", (unsigned long long) ver, (unsigned long long) resolved_seq);
	return value < 0 ? rc == value : rc == 1 && page[16] == value;
}
static int
set_pin(uint64_t owner, uint64_t lsn, uint64_t seq, uint32_t resources)
{
	PsChannel	ch = {.opcode = PS_OP_RETENTION_PIN_SET, .timeline = 0,
		.blocknum = PS_RETENTION_OWNER_READER, .old_nblocks = 1,
		.parent_timeline = resources, .req_seq = owner, .req_lsn = lsn,
	.nblocks = (uint32_t) seq, .pad1 = (uint32_t) (seq >> 32)};

	ps_handle_meta(&ch);
	return ch.status;
}

static void
remove_store(const char *path)
{
	struct stat st;

	ps_test_remove_tree(path);
	check(lstat(path, &st) != 0 && errno == ENOENT, "remove private test store");
}

static void
test_legacy_upgrade(void)
{
	char		store[PS_TEST_TEMP_PATH_MAX] = "pagestore-p5-upgrade-XXXXXX";
	char		path[256];
	struct stat before,
				after;
	uint64_t	legacy_seq = UINT64_C(0x100000002);
	PsRetentionPin pin = {0, PS_RETENTION_OWNER_READER, PS_RETENTION_RESOURCE_ALL,
	1, 3001, 200, legacy_seq};
	PsKey		relation = {.klass = PS_KLASS_RELATION, .relNumber = 66};
	PsKey		control = {.klass = PS_KLASS_CONTROL};

	check(ps_test_mkdtemp(store, sizeof(store)) != NULL && ps_retention_open(store) == 0 &&
		  ps_retention_set(&pin) == 0, "seed a pre-activation pin with a high sequence word");
	ps_retention_close();
	check(ps_core_open(store) == 0, "upgrade legacy pin store");
	write_value(&relation, 100, 0x51);
	write_value(&relation, 100, 0x52);
	write_value(&control, 200, 0x61);
	write_value(&control, 200, 0x62);
	check(read_value(&control, 200, legacy_seq, 0x62),
		  "legacy CONTROL retry stays unbounded at exactly R");
	check(read_value(&relation, 200, legacy_seq, 0x52),
		  "pre-activation pin preserves LSN-only rewrites below its strict boundary");
	check(set_pin(3002, 200, legacy_seq, PS_RETENTION_RESOURCE_PAGE_HISTORY) == PS_STATUS_OK,
		  "legacy explicit SET remains available without a hand-off");
	check(ps_admission_barrier_register(0, 300) > legacy_seq,
		  "activation observes recovered high-word sequences before sampling");
	snprintf(path, sizeof(path), "%s/timelines", store);
	check(stat(path, &before) == 0 && before.st_size == 4 * 64,
		  "startup persists three cap activations and one index-base activation");
	ps_core_close();
	check(ps_core_open(store) == 0 && stat(path, &after) == 0 &&
		  before.st_size == after.st_size, "restart preserves epochs without appending new activation");
	check(read_value(&relation, 200, legacy_seq, 0x52), "legacy interpretation survives restart");
	for (unsigned i = 0; i < 32; i++)
		ps_core_maintenance();
	check(read_value(&control, 200, legacy_seq, 0x62),
		  "legacy CONTROL remains readable after restart and compaction");
	ps_core_close();
	remove_store(store);
}

static void
test_wal_first_arrival(void)
{
	for (unsigned progress = 0; progress < 2; progress++)
	{
		char		store[PS_TEST_TEMP_PATH_MAX] = "pagestore-p5-wal-first-XXXXXX";
		PsKey		relation = {.klass = PS_KLASS_RELATION, .relNumber = 88};
		PsChannel	ch = {.opcode = PS_OP_CREATE, .key = relation, .req_lsn = 50};
		PsRetentionPin pin;
		uint64_t	seq;

		check(ps_test_mkdtemp(store, sizeof(store)) && ps_core_open(store) == 0, "open first-arrival regression store");
		ps_lock_shard_wr(ps_shard_of(&relation));
		ps_handle_meta(&ch);
		ps_unlock_shard(ps_shard_of(&relation));
		check(ch.status == PS_STATUS_OK, "create first-arrival relation");
		seq = ps_admission_barrier();
		if (progress)
		{
			memset(&ch, 0, sizeof(ch));
			ch.opcode = PS_OP_WAL_APPEND;
			ch.datalen = 150;
			ps_handle_meta(&ch);
			check(ch.status == PS_STATUS_OK, "ship progress prefix");
			memset(&ch, 0, sizeof(ch));
			ch.opcode = PS_OP_WAL_INDEX_PROGRESS;
			ch.req_seq = 150;
			ps_lock_shard_wr(0);
			ps_handle_meta(&ch);
			ps_unlock_shard(0);
			check(ch.status == PS_STATUS_OK, "publish finite WAL-index progress before first page arrival");
		}
		else
		{
			pin = (PsRetentionPin)
			{
				0, PS_RETENTION_OWNER_CONFIGURED,
					PS_RETENTION_RESOURCE_WAL_INDEX, 1, 7001, 150, seq
			};
			check(ps_retention_set(&pin) == 0, "register WAL-index-only first-arrival horizon");
		}
		write_value(&relation, 100, 0x71);
		write_value(&relation, 100, 0x72);
		write_value(&relation, 200, 0x73);
		pin = (PsRetentionPin)
		{
			0, PS_RETENTION_OWNER_READER,
				PS_RETENTION_RESOURCE_PAGE_HISTORY, 1, 7002, 300, ps_admission_barrier()
		};
		check(ps_retention_set(&pin) == 0, "advance operational page floor past the WAL-only view");
		for (unsigned i = 0; i < 32; i++)
			ps_core_maintenance();
		check(ps_test_page_version_count(0, &relation, 0) == 2,
			  "page compaction retains the original first arrival for the WAL-index view");
		check(read_value(&relation, 150, seq, -2), "WAL-index preservation does not grant relation read authority");

		/*
		 * Trusted test-only registration exposes the already-retained
		 * identity.
		 */
		pin = (PsRetentionPin)
		{
			0, PS_RETENTION_OWNER_READER,
				PS_RETENTION_RESOURCE_PAGE_HISTORY, 1, 7003, 150, seq
		};
		check(ps_retention_set(&pin) == 0 && read_value(&relation, 150, seq, 0x71),
			  "compaction cannot promote a post-horizon rewrite into the original first arrival");
		ps_core_close();
		check(ps_core_open(store) == 0 && read_value(&relation, 150, seq, 0x71),
			  "WAL-index first-arrival identity survives restart");
		ps_core_close();
		remove_store(store);
	}
}

static void
test_checkpoint_retry(void)
{
	char store[PS_TEST_TEMP_PATH_MAX] = "pagestore-p5-checkpoint-retry-XXXXXX";
	uint64_t prior, current, retry;
	PsRetentionPin slots[2];

	check(ps_test_mkdtemp(store, sizeof(store)) && ps_core_open(store) == 0, "open checkpoint retry store");
	prior = ps_admission_barrier_register(0, 200);
	current = ps_admission_barrier_register(0, 300);
	retry = ps_admission_barrier_register(0, 300);
	check(prior && current && retry > current, "retry the current checkpoint barrier");
	check(ps_retention_checkpoint_lookup(0, 1, &slots[0]) == 1 &&
		  ps_retention_checkpoint_lookup(0, 2, &slots[1]) == 1 &&
		  slots[0].lsn != slots[1].lsn,
		  "same-R retry keeps two distinct checkpoint slots");
	ps_core_close();
	check(ps_core_open(store) == 0 && ps_admission_barrier_register(0, 300),
		  "checkpoint retry after restart retains the prior slot");
	check(set_pin(8001, 200, prior, PS_RETENTION_RESOURCE_PAGE_HISTORY) == PS_STATUS_OK,
		  "handoff from the prior checkpoint remains possible after same-R retries");
	ps_core_close();
	remove_store(store);
}

static void
test_late_wal_reserve(void)
{
	char store[PS_TEST_TEMP_PATH_MAX] = "pagestore-p5-late-wal-XXXXXX";
	PsKey relation = {.klass = PS_KLASS_RELATION, .relNumber = 89};
	PsChannel ch = {.opcode = PS_OP_CREATE, .key = relation, .req_lsn = 50};
	PsRetentionPin pin;

	check(ps_test_mkdtemp(store, sizeof(store)) && ps_core_open(store) == 0, "open late WAL-index reserve store");
	ps_lock_shard_wr(ps_shard_of(&relation));
	ps_handle_meta(&ch);
	ps_unlock_shard(ps_shard_of(&relation));
	check(ch.status == PS_STATUS_OK, "create late-reserve relation");
	write_value(&relation, 100, 0x81);
	write_value(&relation, 100, 0x82);
	write_value(&relation, 200, 0x83);
	pin = (PsRetentionPin) {0, PS_RETENTION_OWNER_READER,
		PS_RETENTION_RESOURCE_PAGE_HISTORY, 1, 8101, 300, ps_admission_barrier()};
	check(ps_retention_set(&pin) == 0, "advance page floor before WAL-index reserve");
	for (unsigned i = 0; i < 32; i++)
		ps_core_maintenance();
	check(ps_test_page_version_count(0, &relation, 0) == 1,
		  "page compaction has already removed the old first arrival");
	for (unsigned restart = 0; restart < 2; restart++)
	{
		ch = (PsChannel) {.opcode = PS_OP_RETENTION_PIN_RESERVE, .timeline = 0,
			.blocknum = PS_RETENTION_OWNER_READER, .old_nblocks = 1,
			.parent_timeline = PS_RETENTION_RESOURCE_WAL_INDEX,
			.req_seq = 8102 + restart, .req_lsn = 150};
		ps_handle_meta(&ch);
		check(ch.status == PS_STATUS_ERROR,
			  "WAL-index-only reserve refuses reclaimed page history");
		ch.req_lsn = 350;
		ch.status = PS_STATUS_OK;
		ps_handle_meta(&ch);
		check(ch.status == PS_STATUS_OK,
			  "WAL-index-only reserve admits available page history");
		check(ps_retention_lookup(0, PS_RETENTION_OWNER_READER, 8102 + restart, &pin) == 1 &&
			  pin.resources == PS_RETENTION_RESOURCE_WAL_INDEX &&
			  read_value(&relation, 350, pin.admission_seq, -2),
			  "page admission validation does not grant WAL-only owners relation reads");
		ps_core_close();
		if (!restart)
			check(ps_core_open(store) == 0, "reopen late-reserve store");
	}
	remove_store(store);
}

static int
begin_delete(uint32_t timeline, uint64_t incarnation, uint32_t *reason)
{
	PsChannel	ch = {.opcode = PS_OP_BEGIN_DELETE, .timeline = timeline, .req_seq = incarnation};

	ps_lifecycle_write_lock();
	ps_admission_write_lock();
	ps_lock_map_wr();
	ps_handle_meta(&ch);
	ps_unlock_map();
	ps_admission_write_unlock();
	ps_lifecycle_write_unlock();
	if (reason)
		*reason = ch.result;
	return ch.status == PS_STATUS_OK;
}

static void
test_checkpoint_delete(void)
{
	char		store[PS_TEST_TEMP_PATH_MAX] = "pagestore-p5-delete-XXXXXX";
	PsChannel	ch = {.opcode = PS_OP_CREATE_BRANCH, .timeline = 1,
	.parent_timeline = 0, .req_lsn = 100, .req_seq = 1};
	PsRetentionPin pin;
	uint64_t	seq;
	uint32_t	reason;

	check(ps_test_mkdtemp(store, sizeof(store)) && ps_core_open(store) == 0, "open checkpoint deletion store");
	ps_lock_map_wr();
	ps_handle_meta(&ch);
	ps_unlock_map();
	check(ch.status == PS_STATUS_OK, "create checkpoint-bearing timeline");
	seq = ps_admission_barrier_register(1, 200);
	pin = (PsRetentionPin)
	{
		1, PS_RETENTION_OWNER_READER,
			PS_RETENTION_RESOURCE_ALL, 1, 7004, 200, seq
	};
	check(seq && ps_retention_handoff(&pin) == 0, "retain checkpoint as an external reader");
	check(!begin_delete(1, 1, &reason) && reason == PS_DELETE_REFUSE_OWNER,
		  "external owner still vetoes timeline deletion");
	check(ps_retention_drop(1, pin.owner_kind, pin.owner_id, 1) == 0 &&
		  begin_delete(1, 1, NULL), "internal checkpoint owners do not veto deletion");
	check(ps_retention_lookup(1, PS_RETENTION_OWNER_CHECKPOINT_FENCE, 1, &pin) == 0,
		  "deletion retires daemon checkpoint slots");
	for (unsigned i = 0; i < 32; i++)
		ps_core_maintenance();
	ps_core_close();
	check(ps_core_open(store) == 0 &&
		  ps_retention_lookup(1, PS_RETENTION_OWNER_CHECKPOINT_FENCE, 1, &pin) == 0,
		  "deleted checkpoint slots stay retired after recovery");
	memset(&ch, 0, sizeof(ch));
	ch.opcode = PS_OP_CREATE_BRANCH;
	ch.timeline = 1;
	ch.parent_timeline = 0;
	ch.req_lsn = 100;
	ch.req_seq = 1;
	ch.incarnation = 2;
	ps_lock_map_wr();
	ps_handle_meta(&ch);
	ps_unlock_map();
	check(ch.status == PS_STATUS_OK && ps_admission_barrier_register(1, 200) != 0,
		  "reused timeline advances checkpoint generations beyond old tombstones");
	ps_core_close();
	{
		char		fault_dir[PS_TEST_TEMP_PATH_MAX] = "pagestore-p5-delete-fault-XXXXXX";
		char		arm[256];
		FILE	   *file;
		int			status = 0;
		pid_t		child;

		check(ps_test_mkdtemp(fault_dir, sizeof(fault_dir)) != NULL, "create crash-control directory");
		snprintf(arm, sizeof(arm), "%s/arm", fault_dir);
		file = fopen(arm, "w");
		check(file != NULL && fclose(file) == 0, "arm deletion crash");
		child = fork();
		if (child == 0)
		{
			execl(test_program, test_program, "--delete-crash", store, fault_dir, (char *) NULL);
			_exit(127);
		}
		check(child > 0 && waitpid(child, &status, 0) == child &&
			  WIFEXITED(status) && WEXITSTATUS(status) == PS_FAULT_CRASH_EXIT,
			  "crash after durable DELETING before checkpoint retirement");
		check(ps_core_open(store) == 0 &&
			  ps_retention_lookup(1, PS_RETENTION_OWNER_CHECKPOINT_FENCE, 1, &pin) == 0 &&
			  begin_delete(1, 2, NULL),
			  "recovery completes checkpoint retirement and accepts exact deletion retry");
		ps_core_close();
		remove_store(fault_dir);
	}
	remove_store(store);
}

typedef struct HandoffRace
{
	pthread_barrier_t start;
	uint64_t	owner;
	int			drop_result;
} HandoffRace;
static void *
drop_source(void *arg)
{
	HandoffRace *race = arg;

	pthread_barrier_wait(&race->start);
	race->drop_result = ps_retention_drop(0, PS_RETENTION_OWNER_CONFIGURED, race->owner, 1);
	return NULL;
}
static void
test_handoff_race(void)
{
	for (unsigned i = 0; i < 16; i++)
	{
		uint64_t	seq = ps_admission_barrier();
		PsRetentionPin source = {0, PS_RETENTION_OWNER_CONFIGURED, PS_RETENTION_RESOURCE_ALL,
		1, 4000 + i, 900, seq};
		PsRetentionPin target = source,
					got;
		HandoffRace race = {.owner = source.owner_id};
		pthread_t	thread;
		int			result;

		target.owner_kind = PS_RETENTION_OWNER_READER;
		target.owner_id += 100;
		check(ps_retention_set(&source) == 0, "publish the race's source registration");
		pthread_barrier_init(&race.start, NULL, 2);
		check(pthread_create(&thread, NULL, drop_source, &race) == 0, "start concurrent source release");
		pthread_barrier_wait(&race.start);
		result = ps_retention_handoff(&target);
		pthread_join(thread, NULL);
		pthread_barrier_destroy(&race.start);
		check(race.drop_result == 0 &&
			  ((result == PS_RETENTION_OK &&
				ps_retention_lookup(0, target.owner_kind, target.owner_id, &got) == 1 &&
				got.admission_seq == seq) ||
			   (result == PS_RETENTION_NOT_REGISTERED &&
				ps_retention_lookup(0, target.owner_kind, target.owner_id, &got) == 0)),
			  "source release and hand-off have one atomic ordering");
		if (result == PS_RETENTION_OK)
			check(ps_retention_drop(0, target.owner_kind, target.owner_id, 1) == 0,
				  "release the race's retained target");
	}
}

int
main(int argc, char **argv)
{
	char		store[PS_TEST_TEMP_PATH_MAX] = "pagestore-p5-XXXXXX";
	PsKey		relation = {.klass = PS_KLASS_RELATION, .relNumber = 55};
	PsKey		control = {.klass = PS_KLASS_CONTROL};
	PsKey		artifact = {.klass = PS_KLASS_SLRU, .relNumber = 77};
	PsRetentionPin pin;
	uint64_t	seq,
				other,
				token = 0,
				pending = 0,
				superseded = 0;
	uint32_t	count;

	page_size = 8192;
	flush_pages = 1;
	compact_layers = 1;
	cache_pages = 0;
	ps_nshards = 3;
	use_layers = 1;
	ps_storage = &PsStoragePosix;
	test_program = argv[0];
	if (argc == 4 && strcmp(argv[1], "--delete-crash") == 0)
	{
		if (setenv("PAGESTORE_TEST_FAULT_NAME", "timeline_delete.after_deleting", 1) != 0 ||
			setenv("PAGESTORE_TEST_FAULT_ACTION", "crash", 1) != 0 ||
			setenv("PAGESTORE_TEST_FAULT_HIT", "1", 1) != 0 ||
			setenv("PAGESTORE_TEST_FAULT_DIR", argv[3], 1) != 0 ||
			ps_core_open(argv[2]) != 0 || ps_fault_init(argv[2]) != 0)
			return 127;
		(void) begin_delete(1, 2, NULL);
		return 126;
	}
	test_legacy_upgrade();
	test_wal_first_arrival();
	test_checkpoint_delete();
	test_checkpoint_retry();
	test_late_wal_reserve();
	check(ps_test_mkdtemp(store, sizeof(store)) && ps_core_open(store) == 0, "open P5 store");
	{
		PsChannel	ch = {.opcode = PS_OP_CREATE, .key = relation, .req_lsn = 50};

		ps_lock_shard_wr(ps_shard_of(&relation));
		ps_handle_meta(&ch);
		ps_unlock_shard(ps_shard_of(&relation));
		check(ch.status == PS_STATUS_OK, "create relation");
	}
	write_value(&relation, 100, 0x11);
	seq = ps_admission_barrier_register(0, 200);
	check(seq != 0, "barrier samples and registers atomically");
	check(set_pin(101, 200, seq, PS_RETENTION_RESOURCE_PAGE_HISTORY) == PS_STATUS_OK,
		  "hand off the checkpoint registration");
	write_value(&relation, 100, 0x22);
	check(read_value(&relation, 200, seq, 0x11), "pin hides a rewrite strictly below R");
	check(read_value(&relation, UINT64_MAX, 0, 0x22), "newest sees rewrite");
	other = ps_admission_barrier();
	check(read_value(&relation, 200, other, -2), "unregistered post-activation read refuses");
	check(set_pin(102, 200, other, PS_RETENTION_RESOURCE_PAGE_HISTORY) != PS_STATUS_OK,
		  "sample then SET cannot manufacture registration");
	pin = (PsRetentionPin)
	{
		0, PS_RETENTION_OWNER_CONFIGURED,
			PS_RETENTION_RESOURCE_WAL, 1, 203, 300, other
	};
	check(ps_retention_set(&pin) == 0, "set WAL-only internal source");
	check(set_pin(104, 300, other, PS_RETENTION_RESOURCE_PAGE_HISTORY) != PS_STATUS_OK,
		  "hand-off requires a resource superset");
	check(ps_retention_drop(0, pin.owner_kind, pin.owner_id, 1) == 0,
		  "release internal source");
	check(ps_admission_barrier_register(0, 400) && ps_admission_barrier_register(0, 500),
		  "advance checkpoint slots");
	check(ps_retention_count(&count) == 0 && count == 3,
		  "checkpoint namespace keeps two active slots plus reader");
	check(set_pin(105, 200, seq, PS_RETENTION_RESOURCE_PAGE_HISTORY) == PS_STATUS_OK,
		  "reader source survives checkpoint slot eviction");
	write_value(&control, 600, 0x31);
	other = ps_admission_barrier();
	pin = (PsRetentionPin)
	{
		0, PS_RETENTION_OWNER_CONFIGURED,
			PS_RETENTION_RESOURCE_WAL, 1, 204, 650, other
	};
	check(ps_retention_set(&pin) == 0, "register a WAL-only control view");
	check(read_value(&control, 650, other, 0x31), "WAL-only owner may restore its retained control era");
	check(ps_retention_drop(0, pin.owner_kind, pin.owner_id, 1) == 0,
		  "release WAL-only control owner");
	ps_lock_shard_wr(ps_shard_of(&artifact));
	check(ps_artifact_begin(0, &artifact, 700, &token, NULL) == 0,
		  "begin capped artifact");
	ps_unlock_shard(ps_shard_of(&artifact));
	write_value(&control, 600, 0x32);
	check(read_value(&control, 700, token, 0x31), "inflight artifact freezes control era");
	ps_lock_shard_wr(ps_shard_of(&artifact));
	check(ps_artifact_commit(0, &artifact, 700, token, 0, NULL) == 0,
		  "commit empty artifact retains its control cap");
	ps_unlock_shard(ps_shard_of(&artifact));
	ps_core_close();
	check(ps_core_open(store) == 0, "restart activated store");
	check(read_value(&relation, 200, seq, 0x11), "finite pin survives restart");
	check(read_value(&control, 700, token, 0x31), "committed control cap rebuilt on restart");
	ps_lock_shard_wr(ps_shard_of(&artifact));
	check(ps_artifact_begin(0, &artifact, 800, &superseded, NULL) == 0 &&
		  ps_artifact_begin(0, &artifact, 800, &pending, NULL) == 0 && pending > superseded,
		  "a new BEGIN supersedes only the unfinished control cap");
	ps_unlock_shard(ps_shard_of(&artifact));
	check(read_value(&control, 800, superseded, -2) &&
		  read_value(&control, 800, pending, 0x32), "superseded token cannot read a capped control era");
	ps_lock_shard_wr(ps_shard_of(&artifact));
	check(ps_artifact_drop(0, &artifact, 800, NULL) == 0, "DROP releases the unfinished attempt");
	ps_unlock_shard(ps_shard_of(&artifact));
	check(read_value(&control, 800, pending, -2), "DROP revokes the unfinished control cap");
	ps_lock_shard_wr(ps_shard_of(&artifact));
	check(ps_artifact_begin(0, &artifact, 900, &pending, NULL) == 0, "begin an attempt before restart");
	ps_unlock_shard(ps_shard_of(&artifact));
	ps_core_close();
	check(ps_core_open(store) == 0 && read_value(&control, 900, pending, -2),
		  "recovery never reconstructs an unfinished control cap");
	check(read_value(&control, 700, token, 0x31), "DROP and restart preserve completed control eras");
	for (unsigned i = 0; i < 32; i++)
		ps_core_maintenance();
	check(read_value(&relation, 200, seq, 0x11) && read_value(&control, 700, token, 0x31),
		  "compaction preserves positional pin and artifact views");
	test_handoff_race();
	ps_core_close();
	remove_store(store);
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
