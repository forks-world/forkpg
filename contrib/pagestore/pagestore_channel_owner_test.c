/* Process-death and generation checks for channel ownership. */
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGESTORE_CHANNEL_OWNER_TEST
#include "pagestore_channel_owner.h"

void (*ps_test_channel_reclaim_hook)(PsChannel *);
static uint64_t replacement_token;

static void
simulate_reused_pid(PsChannel *ch)
{
	ps_test_channel_reclaim_hook = NULL;
	ps_store_release_u64(&ch->claimed, replacement_token);
}

typedef struct Fixture
{
	PsShmHeader header;
	PsChannel channels[PS_MAX_CHANNELS];
} Fixture;

int
main(void)
{
	Fixture *f;
	uint64_t mine, old, replacement;
	PsChannel *ch;
	pid_t child;
	int status, ready[2];
	char byte;

	f = mmap(NULL, sizeof(*f), PROT_READ | PROT_WRITE,
			 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (f == MAP_FAILED)
		return 1;
	ch = &f->channels[0];
	if (!ps_channel_pid_namespace(&f->header.channel_pidns_dev,
			&f->header.channel_pidns_ino) ||
		!__atomic_always_lock_free(sizeof(uint64_t), 0))
	{
		if (ps_channel_owner_claim(&f->header) != 1)
			return 1;
		puts("PID ownership unavailable: safe legacy fallback");
		return 0;
	}
	mine = ps_channel_owner_claim(&f->header);
	if ((mine >> 32) == 0 || pipe(ready) != 0)
		return 1;
	child = fork();
	if (child == 0)
	{
		uint64_t claim = ps_channel_owner_claim(&f->header);
		close(ready[0]);
		ps_store_release_u64(&ch->claimed, claim);
		ps_store_release(&ch->state, PS_STATE_DONE);
		for (uint32_t i = 1; i < PS_MAX_CHANNELS; i++)
		{
			claim = ps_channel_owner_claim(&f->header);
			ps_store_release_u64(&f->channels[i].claimed, claim);
			ps_store_release(&f->channels[i].state,
				(i & 1) ? PS_STATE_REQUEST : PS_STATE_DONE);
		}
		if (write(ready[1], "x", 1) != 1)
			_exit(1);
		for (;;)
			pause();
	}
	close(ready[1]);
	if (child < 0 || read(ready[0], &byte, 1) != 1)
		return 1;
	close(ready[0]);
	if (ps_channel_reclaim_dead(ch, mine) ||
		ps_channel_reclaim_dead(&f->channels[2], mine))
	{
		kill(child, SIGKILL);
		waitpid(child, &status, 0);
		return 1; /* live owner remains protected even after completion */
	}
	if (kill(child, SIGKILL) != 0 || waitpid(child, &status, 0) != child ||
		!WIFSIGNALED(status))
		return 1;
	old = ps_load_acquire_u64(&ch->claimed);
	ps_store_release(&ch->state, PS_STATE_REQUEST);
	if (ps_channel_reclaim_dead(ch, mine))
		return 1; /* daemon may still be completing the old request */
	ps_store_release(&ch->state, PS_STATE_DONE);
	/* Model PID reuse after the liveness check but before its CAS. */
	replacement = ((mine >> 32) + 100) << 32 | (uint32_t) old;
	replacement_token = replacement;
	ps_test_channel_reclaim_hook = simulate_reused_pid;
	if (ps_channel_reclaim_dead(ch, mine) ||
		ps_load_acquire_u64(&ch->claimed) != replacement)
		return 1;
	ps_store_release_u64(&ch->claimed, old);
	if (!ps_channel_reclaim_dead(ch, mine))
		return 1;
	for (uint32_t i = 1; i < PS_MAX_CHANNELS; i++)
	{
		uint64_t claim = ps_channel_owner_claim(&f->header);

		if ((i & 1) && ps_channel_reclaim_dead(&f->channels[i], claim))
			return 1;
		ps_store_release(&f->channels[i].state, PS_STATE_DONE);
		if (!ps_channel_reclaim_dead(&f->channels[i], claim))
			return 1;
	}
	ps_store_release_u64(&ch->claimed, 1);
	if (ps_channel_reclaim_dead(ch, mine))
		return 1;
	f->header.channel_pidns_ino ^= 1;
	if (ps_channel_owner_claim(&f->header) != 1)
		return 1;
	f->header.channel_pidns_ino ^= 1;
	f->header.channel_owner_generation = UINT32_MAX;
	if (ps_channel_owner_claim(&f->header) != 1)
		return 1;
	munmap(f, sizeof(*f));
	puts("channel owner recovery checks passed");
	return 0;
}
