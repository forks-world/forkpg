/* Exercise the real SQL redo helpers with deterministic index-view replies.
 * Keep the callbacks private to the backend-only fault-test module. */
#define pagestore_localsvc_wal_base_state test_redo_base_state
#define pagestore_localsvc_read_at_found test_redo_ordinary_base
#define pagestore_localsvc_nblocks_asof_checked test_redo_ordinary_size
#define pagestore_localsvc_block_death_asof test_redo_ordinary_death
#include "pagestore.c"
#undef pagestore_localsvc_wal_base_state
#undef pagestore_localsvc_read_at_found
#undef pagestore_localsvc_nblocks_asof_checked
#undef pagestore_localsvc_block_death_asof

PG_FUNCTION_INFO_V1(pagestore_test_redo_base);

static int redo_test_mode;
static int redo_index_calls;
static int redo_ordinary_calls;

int
test_redo_base_state(const PageStoreRelKey *key, BlockNumber blocknum,
					 uint64 lsn, void *out, PsWalBaseState *state)
{
	redo_index_calls++;
	memset(state, 0, sizeof(*state));
	if (redo_test_mode == 3) return -1;
	if (redo_test_mode == 4) return -2;
	state->nblocks = redo_test_mode == 2 ? 0 : 1;
	state->death_lsn = 900;
	state->death_seq = 10;
	if (redo_test_mode != 0) return 0;
	memset(out, 0, BLCKSZ);
	PageSetLSN((Page) out, 1000);
	state->found = 1;
	state->version_lsn = 1100; /* admission position, above pd_lsn */
	state->version_seq = 20;
	return 1;
}

int
test_redo_ordinary_base(const PageStoreRelKey *key, BlockNumber blocknum,
					   uint64 lsn, void *out, uint64 *version, uint64 *seq)
{
	redo_ordinary_calls++;
	memset(out, 0, BLCKSZ);
	PageSetLSN((Page) out, 500);
	*version = 500;
	*seq = 5;
	return 1;
}

bool
test_redo_ordinary_size(const PageStoreRelKey *key, uint64 lsn, uint64 *nblocks)
{
	redo_ordinary_calls++;
	*nblocks = 1;
	return true;
}

bool
test_redo_ordinary_death(const PageStoreRelKey *key, BlockNumber blocknum,
						 uint64 lsn, uint64 *death, uint64 *seq)
{
	redo_ordinary_calls++;
	*death = *seq = 0;
	return true;
}

Datum
pagestore_test_redo_base(PG_FUNCTION_ARGS)
{
	PsRedoStoredView view = {0};
	PageStoreRelKey key = {0};
	char page[BLCKSZ];
	XLogRecPtr end = 0;
	uint64 seq = 0;
	uint64 death_seq = 0;
	bool found;
	bool ok;

	redo_test_mode = PG_GETARG_INT32(0);
	redo_index_calls = redo_ordinary_calls = 0;
	if (redo_test_mode == 3)
	{
		MemoryContext context = CurrentMemoryContext;
		volatile bool caught = false;

		PG_TRY();
		{
			(void) ps_redo_stored_base(&view, &key, 0, 1500, page, &end, &seq);
		}
		PG_CATCH();
		{
			MemoryContextSwitchTo(context);
			caught = geterrcode() == ERRCODE_IO_ERROR;
			FlushErrorState();
		}
		PG_END_TRY();
		PG_RETURN_BOOL(caught && redo_index_calls == 1 && redo_ordinary_calls == 0);
	}
	found = ps_redo_stored_base(&view, &key, 0, 1500, page, &end, &seq);
	if (redo_test_mode == 4)
	{
		ok = found && end == 500 && seq == 5 &&
			ps_redo_nblocks_asof(&view, &key, 0, 1500) == 1 &&
			ps_redo_block_death(&view, &key, 0, 1500, &death_seq) == 0 &&
			redo_index_calls == 1 && redo_ordinary_calls == 3;
	}
	else
	{
		ok = (found == (redo_test_mode != 2)) &&
			ps_redo_nblocks_asof(&view, &key, 0, 1500) == view.state.nblocks &&
			ps_redo_block_death(&view, &key, 0, 1500, &death_seq) == 900 &&
			death_seq == 10 && redo_index_calls == 1 && redo_ordinary_calls == 0;
		if (redo_test_mode == 0) ok = ok && end == 1100 && seq == 20;
		if (redo_test_mode == 1)
		{
			ok = ok && end == InvalidXLogRecPtr;
			for (int i = 0; i < BLCKSZ; i++) ok = ok && page[i] == 0;
		}
	}
	PG_RETURN_BOOL(ok);
}
