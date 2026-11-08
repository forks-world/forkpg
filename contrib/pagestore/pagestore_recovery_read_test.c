/* Test the actual private read policies with controlled recovery positions. */
#include "postgres.h"
#include "fmgr.h"
#include "access/xlog.h"
#include "access/xlogrecovery.h"
#include "miscadmin.h"

static bool test_recovery;
static bool test_startup;
static XLogRecPtr test_current;
static XLogRecPtr test_completed;

#undef AmStartupProcess
#define AmStartupProcess() (test_startup)
#define RecoveryInProgress() (test_recovery)
#define GetCurrentReplayRecPtr(tli) (test_current)
#define GetXLogReplayRecPtr(tli) (test_completed)
#include "backend_localsvc.c"

PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(pagestore_test_recovery_read);
PG_FUNCTION_INFO_V1(pagestore_test_recovery_write_flag);

static bool test_materializer;

/* The private test backend is never initialized or used for materialization. */
bool
pagestore_materializer_enabled(void)
{
	return test_materializer;
}

Datum
pagestore_test_recovery_write_flag(PG_FUNCTION_ARGS)
{
	PsChannel ch = {0};
	bool ok;

	test_materializer = true;
	test_recovery = true;
	test_completed = 100;
	ls_recovery_floor(&ch);
	ok = ch.is_redo == PS_WRITE_REPLAY_FLOOR && ch.req_lsn == 100 &&
		ch.req_floor_lsn == 0;
	test_recovery = false;
	ls_recovery_floor(&ch);
	ok = ok && ch.is_redo == PS_WRITE_REPLAY_FLOOR && ch.req_lsn == 0;
	test_materializer = false;
	test_recovery = true;
	ls_recovery_floor(&ch);
	ok = ok && ch.is_redo == 0 && ch.req_lsn == 0;
	test_recovery = false;
	PG_RETURN_BOOL(ok);
}

Datum
pagestore_test_recovery_read(PG_FUNCTION_ARGS)
{
	int mode = PG_GETARG_INT32(0);
	uint64 page_cap;
	uint64 metadata_cap;
	uint64 expected_page;
	uint64 expected_metadata;
	PsShmHeader *hdr;
	PsChannel  *ch;
	PageStoreRelKey key = {0};
	void *saved_shm;
	bool redo_flag_ok;


	test_recovery = mode != 0 && mode != 5;
	test_startup = mode >= 2 && mode != 5;
	test_current = mode == 3 ? 0 : mode == 6 ? 250 : 150;
	localsvc_recovery_read_floor = 200;
	test_completed = 100;
	localsvc_read_lsn = mode == 4 || mode == 5 ? 75 : 0;
	if (mode < 0 || mode > 6)
		PG_RETURN_BOOL(false);
	page_cap = ls_page_read_lsn();
	metadata_cap = ls_read_lsn();
	expected_metadata = mode == 0 ? 0 : mode == 1 || mode == 3 ? 100 :
		mode == 4 || mode == 5 ? 75 : mode == 6 ? 250 : 150;
	expected_page = mode == 4 || mode == 5 ? 75 : mode == 1 ? 100 :
		mode == 0 ? 0 : mode == 6 ? 250 : 200;
	/* With redo at 150, durable physical page 200 must win over old image
	 * 50. PostgreSQL then skips the already-applied record using pd_lsn.
	 * A page cap of 150 would incorrectly reload image 50. */
	if (mode == 2 && !(page_cap == 200 && metadata_cap == 150))
		PG_RETURN_BOOL(false);
	/* The marker is capability-negotiated and must never survive channel reuse
	 * into a reader, standby backend, ordinary write, or older daemon. */
	hdr = palloc0(sizeof(*hdr));
	ch = palloc0(sizeof(*ch));
	saved_shm = ls_shm;
	ls_shm = hdr;
	hdr->frontend_capabilities = PS_FRONTEND_CAP_BRANCH_REDO_READ;
	ch->req_lsn = page_cap;
	ch->req_seq = localsvc_read_lsn ? 1 : 0;
	ls_read_opcode(ch, &key);
	redo_flag_ok = (ch->is_redo == PS_READ_BRANCH_REDO) ==
		(test_recovery && test_startup && localsvc_read_lsn == 0);
	hdr->frontend_capabilities = 0;
	ch->is_redo = PS_READ_BRANCH_REDO;
	ls_read_opcode(ch, &key);
	redo_flag_ok = redo_flag_ok && ch->is_redo == 0;
	ls_shm = saved_shm;
	pfree(ch);
	pfree(hdr);
	PG_RETURN_BOOL(page_cap == expected_page && metadata_cap == expected_metadata && redo_flag_ok);
}
