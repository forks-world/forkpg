/* Exercise fuzzer bookkeeping and replay I/O without starting a daemon. */
#define main pagestore_fuzz_main
#include "pagestore_fuzz_test.c"
#undef main

#define REQUIRE(c) do { if (!(c)) { \
	fprintf(stderr, "failed at line %d: %s\n", __LINE__, #c); \
	return 1; } } while (0)

int
main(void)
{
	char path[] = "/tmp/pagestore-fuzz-helpers-XXXXXX";
	char invalid[128];
	char contents[128] = {0};
	int actions[2];
	int fd = mkstemp(path);
	FILE *f;

	REQUIRE(fd >= 0);
	close(fd);
	actions[0] = find_action_index("env_sleep");
	actions[1] = find_action_index("readv");
	REQUIRE(actions[0] >= 0 && actions[1] >= 0);
	g_seed = 42;
	g_seq_actions = actions;
	g_seq_len = 2;
	REQUIRE(write_seq_file(path));
	f = fopen(path, "r");
	REQUIRE(f != NULL);
	REQUIRE(fread(contents, 1, sizeof(contents) - 1, f) > 0);
	REQUIRE(fclose(f) == 0);
	REQUIRE(strcmp(contents, "# replay seed=42\nenv_sleep\nreadv\n") == 0);

	/* A regular file cannot be a parent directory: exercise fopen failure. */
	snprintf(invalid, sizeof(invalid), "%s/child", path);
	REQUIRE(!write_seq_file(invalid));
	REQUIRE(errno == ENOTDIR);
	unlink(path);

	/* Linux /dev/full accepts open but fails buffered writes/close. */
	if (access("/dev/full", W_OK) == 0)
	{
		REQUIRE(!write_seq_file("/dev/full"));
		REQUIRE(errno == ENOSPC);
		g_seq_len = 10000;
		g_seq_actions = calloc((size_t) g_seq_len, sizeof(int));
		REQUIRE(g_seq_actions != NULL);
		REQUIRE(!write_seq_file("/dev/full"));
		REQUIRE(errno == ENOSPC);
		free(g_seq_actions);
	}
	g_seq_actions = NULL;
	g_seq_len = 0;

	/* Ineligible environment actions must count a skip without IPC. */
	for (uint32_t i = 0; i < FZ_NREADERS; i++)
		g_reader[i].held = 1;
	env_reader_reserve();
	REQUIRE(g_env_cov[ENV_READER_RESERVE][1] == 1);
	memset(g_reader, 0, sizeof(g_reader));
	env_reader_advance();
	env_reader_drop();
	REQUIRE(g_env_cov[ENV_READER_ADVANCE][1] == 1);
	REQUIRE(g_env_cov[ENV_READER_DROP][1] == 1);
	for (uint32_t i = 1; i < FZ_NTL; i++)
	{
		g_tl[i].known = 1;
		g_tl[i].state = PS_TIMELINE_LIVE;
	}
	env_branch_create();
	env_branch_write(); /* live but no writable relation */
	REQUIRE(g_env_cov[ENV_BRANCH_CREATE][1] == 1);
	REQUIRE(g_env_cov[ENV_BRANCH_WRITE][1] == 1);
	memset(g_tl, 0, sizeof(g_tl));
	env_branch_write();
	env_branch_begin_delete();
	env_wait_deleted();
	REQUIRE(g_env_cov[ENV_BRANCH_WRITE][1] == 2);
	REQUIRE(g_env_cov[ENV_BRANCH_BEGIN_DELETE][1] == 1);
	REQUIRE(g_env_cov[ENV_WAIT_DELETED][1] == 1);
	REQUIRE(artifact_growth_refusal_ok(PS_STATUS_OK, PS_ARTIFACT_REFUSE_NONE));
	REQUIRE(!artifact_growth_refusal_ok(PS_STATUS_OK,
									  PS_ARTIFACT_REFUSE_FORKMETA_CUTOFF));
	puts("fuzzer helper regressions passed");
	return 0;
}
