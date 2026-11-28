/* Exercise a real importer against a full, crash-abandoned channel pool. */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pagestore_channel_owner.h"
#include "pagestore_shm.h"

static int
run_import(const char *program, const char *name, const char *dir)
{
	int status;
	pid_t pid = fork();

	if (pid == 0)
	{
		int nullfd = open("/dev/null", O_WRONLY);
		if (nullfd >= 0)
			dup2(nullfd, STDERR_FILENO);
		execl(program, program, "--shm", name, "--pgdata", dir, (char *) NULL);
		_exit(127);
	}
	if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
		return -1;
	return WEXITSTATUS(status);
}

int
main(int argc, char **argv)
{
	char name[96], dir[] = "/tmp/ps-channel-owner-XXXXXX", byte;
	int fd = -1, ready[2], status, ok = 0;
	void *shm = MAP_FAILED;
	PsShmHeader *hdr;
	PsChannel *ch;
	pid_t holder = -1, owner = -1;

	if (argc != 2 || !mkdtemp(dir))
		return 1;
	snprintf(name, sizeof(name), "/ps_channel_owner_%ld", (long) getpid());
	fd = ps_shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0 || ftruncate(fd, PS_SHM_SIZE) != 0)
		goto done;
	shm = mmap(NULL, PS_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (shm == MAP_FAILED)
		goto done;
	hdr = shm;
	hdr->magic = PS_SHM_MAGIC;
	hdr->version = PS_SHM_VERSION;
	hdr->page_size = PS_DEFAULT_PAGE_SIZE;
	hdr->io_unit = PS_IO_UNIT;
	hdr->nchannels = 1;
	hdr->nshards = 1;
	hdr->channel_stride = PS_CHANNEL_STRIDE;
	hdr->channels_off = PS_CHANNELS_OFF;
	hdr->startup_state = PS_SHM_READY;
	if (!ps_channel_pid_namespace(&hdr->channel_pidns_dev, &hdr->channel_pidns_ino) ||
		!__atomic_always_lock_free(sizeof(uint64_t), 0))
	{
		ok = 1; /* unsupported environments retain safe legacy behavior */
		goto done;
	}
	if (pipe(ready) != 0)
		goto done;
	holder = fork();
	if (holder == 0)
	{
		struct flock lock = {0};
		int own = ps_shm_open(name, O_RDWR, 0);

		close(ready[0]);
		lock.l_type = F_WRLCK;
		lock.l_whence = SEEK_SET;
		lock.l_start = PS_INSPECTION_DAEMON_LOCK_BYTE;
		lock.l_len = 1;
		if (own < 0 || fcntl(own, F_SETLK, &lock) != 0 || write(ready[1], "x", 1) != 1)
			_exit(1);
		for (;;) pause();
	}
	close(ready[1]);
	if (holder < 0 || read(ready[0], &byte, 1) != 1)
	{
		close(ready[0]);
		goto done;
	}
	close(ready[0]);
	if (pipe(ready) != 0)
		goto done;
	ch = ps_channel(shm, 0);
	owner = fork();
	if (owner == 0)
	{
		uint64_t claim = ps_channel_owner_claim(hdr);
		close(ready[0]);
		ps_store_release_u64(&ch->claimed, claim);
		ps_store_release(&ch->state, PS_STATE_DONE);
		if (write(ready[1], "x", 1) != 1) _exit(1);
		for (;;) pause();
	}
	close(ready[1]);
	if (owner < 0 || read(ready[0], &byte, 1) != 1)
	{
		close(ready[0]);
		goto done;
	}
	close(ready[0]);
	if (run_import(argv[1], name, dir) != 2)
		goto done; /* live owner is protected */
	ps_store_release(&ch->state, PS_STATE_REQUEST);
	kill(owner, SIGKILL);
	if (waitpid(owner, &status, 0) != owner)
		goto done;
	owner = -1;
	if (run_import(argv[1], name, dir) != 2)
		goto done; /* daemon still owes completion */
	ps_store_release(&ch->state, PS_STATE_DONE);
	if (run_import(argv[1], name, dir) != 0 || ps_load_acquire_u64(&ch->claimed) != 0)
		goto done; /* real client reclaims and releases the completed slot */
	ok = 1;
done:
	if (owner > 0) { kill(owner, SIGKILL); waitpid(owner, &status, 0); }
	if (holder > 0) { kill(holder, SIGKILL); waitpid(holder, &status, 0); }
	if (shm != MAP_FAILED) munmap(shm, PS_SHM_SIZE);
	if (fd >= 0) close(fd);
	ps_shm_unlink(name);
	rmdir(dir);
	if (!ok) fprintf(stderr, "channel owner integration failed\n");
	return ok ? 0 : 1;
}
