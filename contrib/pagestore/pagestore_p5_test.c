/* Finite pins must freeze rewrites below R, and registration must be atomic. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "pagestore_core.h"
#include "pagestore_retention.h"

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
	char		command[256];

	snprintf(command, sizeof(command), "rm -rf -- '%s'", path);
	check(system(command) == 0, "remove private test store");
}

static void
test_legacy_upgrade(void)
{
	char		store[] = "/tmp/pagestore-p5-upgrade-XXXXXX";
	char		path[256];
	struct stat before,
				after;
	uint64_t	legacy_seq = UINT64_C(0x100000002);
	PsRetentionPin pin = {0, PS_RETENTION_OWNER_READER, PS_RETENTION_RESOURCE_ALL,
	1, 3001, 200, legacy_seq};
	PsKey		relation = {.klass = PS_KLASS_RELATION, .relNumber = 66};

	check(mkdtemp(store) != NULL && ps_retention_open(store) == 0 &&
		  ps_retention_set(&pin) == 0, "seed a pre-activation pin with a high sequence word");
	ps_retention_close();
	check(ps_core_open(store) == 0, "upgrade legacy pin store");
	write_value(&relation, 100, 0x51);
	write_value(&relation, 100, 0x52);
	check(read_value(&relation, 200, legacy_seq, 0x52),
		  "pre-activation pin preserves LSN-only rewrites below its strict boundary");
	check(set_pin(3002, 200, legacy_seq, PS_RETENTION_RESOURCE_PAGE_HISTORY) == PS_STATUS_OK,
		  "legacy explicit SET remains available without a hand-off");
	check(ps_admission_barrier_register(0, 300) > legacy_seq,
		  "activation observes recovered high-word sequences before sampling");
	snprintf(path, sizeof(path), "%s/timelines", store);
	check(stat(path, &before) == 0 && before.st_size == 3 * 64,
		  "startup persists exactly three independent activation records");
	ps_core_close();
	check(ps_core_open(store) == 0 && stat(path, &after) == 0 &&
		  before.st_size == after.st_size, "restart preserves epochs without appending new activation");
	check(read_value(&relation, 200, legacy_seq, 0x52), "legacy interpretation survives restart");
	ps_core_close();
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
main(void)
{
	char		store[] = "/tmp/pagestore-p5-XXXXXX";
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
	test_legacy_upgrade();
	check(mkdtemp(store) && ps_core_open(store) == 0, "open P5 store");
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
