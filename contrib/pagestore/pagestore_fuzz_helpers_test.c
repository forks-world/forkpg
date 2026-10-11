/* Exercise fuzzer bookkeeping and replay I/O without starting a daemon. */
int pagestore_fuzz_main(int argc, char **argv);
#define main pagestore_fuzz_main
#include "pagestore_fuzz_test.c"
#undef main
#include <poll.h>

#define REQUIRE(c) do { if (!(c)) { \
	fprintf(stderr, "failed at line %d: %s\n", __LINE__, #c); \
	return 1; } } while (0)

int
main(int argc, char **argv)
{
	char path[] = "/tmp/pagestore-fuzz-helpers-XXXXXX";
	char invalid[128];
	char contents[128] = {0};
	int actions[2];
	int fd = mkstemp(path);
	FILE *f;

	if (argc == 5 && strcmp(argv[1], "--signal-candidate") == 0)
	{
		char store[600], shm[64];
		pid_t self = getpid();
		pid_t daemon;
		int shmfd;
		int pipefd = atoi(argv[4]);

		/* A fake replay leaves the same resources as a crashed real one. */
		close(fd);
		unlink(path);
		snprintf(store, sizeof(store), "%s/pagestore-fuzz-%d", argv[2], (int) self);
		snprintf(shm, sizeof(shm), "/psfuzz_%d", (int) self);
		REQUIRE(mkdir(store, 0700) == 0);
		shmfd = shm_open(shm, O_CREAT | O_EXCL | O_RDWR, 0600);
		REQUIRE(shmfd >= 0);
		close(shmfd);
		f = fopen(argv[3], "w");
		REQUIRE(f != NULL);
		REQUIRE(fprintf(f, "%d\n", (int) self) > 0);
		REQUIRE(fclose(f) == 0);
		daemon = fork();
		REQUIRE(daemon >= 0);
		if (daemon == 0)
		{
			/* Keep the inherited pipe open until group cleanup kills us. */
			for (;;)
				pause();
		}
		close(pipefd);
		kill(self, SIGKILL);
		_exit(1);
	}

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
	/* A parent settling an attempt after the fork must not replace the
	 * child's frozen absence, nor an earlier settled generation. */
	g_tl[1].known = 1;
	g_tl[1].state = PS_TIMELINE_LIVE;
	g_tl[1].has_parent = 1;
	g_tl[1].parent = 0;
	g_tl[1].branch_lsn = 100;
	g_tl[1].branch_seq = 10;
	g_bugb_workaround = 0;
	for (int prior = 0; prior < 2; prior++)
	{
		FzArtifact *child = &g_artifact[1][0][0];
		FzRel published = {0};

		memset(child, 0, sizeof(*child));
		child->state = prior ? FZ_ART_COMMITTED : FZ_ART_NONE;
		child->lsn = prior ? 50 : 0;
		child->visible.exists = prior;
		child->visible.nblocks = prior;
		child->inherited_open_pending = 1;
		child->inherited_open_lsn = 100;
		child->inherited_open_token = 9;
		published.exists = 1;
		published.nblocks = 3;
		artifact_apply_parent_commit(child, 100, 9, &published);
		REQUIRE(!child->inherited_open_pending);
		REQUIRE(child->state == (prior ? FZ_ART_COMMITTED : FZ_ART_NONE));
		REQUIRE(child->lsn == (prior ? 50 : 0));
		REQUIRE(child->visible.exists == prior);
		REQUIRE(child->visible.nblocks == (uint32_t) prior);
	}
	{
		char base[] = "/tmp/pagestore-shrink-helper-XXXXXX";
		char pidpath[600], store[600], shm[64], pipearg[32];
		char *candidate_argv[] = {argv[0], "--signal-candidate", base,
			pidpath, pipearg, NULL};
		int pipes[2], candidate_pid = 0;
		struct pollfd pollfd;
		char byte;
		FzShrinkCandidateResult result;

		REQUIRE(mkdtemp(base) != NULL);
		snprintf(pidpath, sizeof(pidpath), "%s/candidate.pid", base);
		REQUIRE(pipe(pipes) == 0);
		snprintf(pipearg, sizeof(pipearg), "%d", pipes[1]);
		g_store_base = base;
		g_argv = candidate_argv;
		resolve_self_exe(argv[0]);
		result = shrink_try_candidate("unused.opseq", "unused", 1, 5);
		close(pipes[1]);
		pollfd.fd = pipes[0];
		pollfd.events = POLLIN | POLLHUP;
		pollfd.revents = 0;
		/* EOF proves the grandchild also died, without relying on zombie
		 * reaping by the container's init process. */
		REQUIRE(poll(&pollfd, 1, 5000) > 0);
		REQUIRE(read(pipes[0], &byte, 1) == 0);
		close(pipes[0]);
		REQUIRE(result == FZ_SHRINK_NOT_REPRODUCED);
		f = fopen(pidpath, "r");
		REQUIRE(f != NULL);
		REQUIRE(fscanf(f, "%d", &candidate_pid) == 1 && candidate_pid > 0);
		REQUIRE(fclose(f) == 0);
		snprintf(store, sizeof(store), "%s/pagestore-fuzz-%d", base, candidate_pid);
		snprintf(shm, sizeof(shm), "/psfuzz_%d", candidate_pid);
		REQUIRE(access(store, F_OK) < 0 && errno == ENOENT);
		REQUIRE(shm_open(shm, O_RDWR, 0600) < 0 && errno == ENOENT);
		unlink(pidpath);
		REQUIRE(rmdir(base) == 0);
	}
	puts("fuzzer helper regressions passed");
	return 0;
}
