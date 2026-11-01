/* Exercise the production catch with a deterministic failing I/O callback.
 * This module is for the integration test only; never installed as pagestore. */
#define pagestore_localsvc_obj_write_prepare_timeout test_control_prepare
#define pagestore_localsvc_obj_write_post_timeout test_control_post
#define pagestore_localsvc_admission_fence_active test_control_fence_active
#define pagestore_localsvc_admission_barrier_timeout test_control_barrier
#define pagestore_localsvc_admission_fence_end test_control_fence_end
#define pagestore_localsvc_store_sync_timeout test_control_sync
#define pagestore_slru_note_checkpoint_redo test_control_slru
#define pagestore_publish_checkpoint_reader_snapshot test_control_snapshot
#include "pagestore_control.c"
#undef pagestore_localsvc_obj_write_prepare_timeout
#undef pagestore_localsvc_obj_write_post_timeout
#undef pagestore_localsvc_admission_fence_active
#undef pagestore_localsvc_admission_barrier_timeout
#undef pagestore_localsvc_admission_fence_end
#undef pagestore_localsvc_store_sync_timeout
#undef pagestore_slru_note_checkpoint_redo
#undef pagestore_publish_checkpoint_reader_snapshot
#include "access/table.h"
#include "storage/bufmgr.h"

PG_FUNCTION_INFO_V1(pagestore_test_control_error);
PG_FUNCTION_INFO_V1(pagestore_test_control_reclaimed);

static bool injected_control_error;
static int control_mode;
static int control_fence_ended, control_synced, control_fence_writes, control_snapshots;

BlockNumber
test_control_prepare(uint32 klass, const PageStoreRelKey *key, int timeout_ms)
{
	if (control_mode != 0)
		return 0;
	injected_control_error = true;
	ereport(ERROR, (errmsg("injected control mirror write failure")));
	return 0;
}

Datum
pagestore_test_control_error(PG_FUNCTION_ARGS)
{
	Relation rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	Buffer buffer = ReadBuffer(rel, 0);
	MemoryContext context = CurrentMemoryContext;
	int interrupts;
	int cancels;
	bool ok;

	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	HOLD_CANCEL_INTERRUPTS();
	interrupts = InterruptHoldoffCount;
	cancels = QueryCancelHoldoffCount;
	ps_control_queue_head = 0;
	ps_control_queue_count = 1;
	memset(&ps_control_queue[0], 0, sizeof(ps_control_queue[0]));
	ps_control_queue[0].update_lsn = 1;
	ps_control_drain();
	ok = injected_control_error && ps_control_queue_count == 1 &&
		InterruptHoldoffCount == interrupts && QueryCancelHoldoffCount == cancels &&
		CurrentMemoryContext == context;
	/* Report an ordinary test failure even against the unfixed drain; do
	 * not let its corrupted count abort the test backend on unlock. */
	MemoryContextSwitchTo(context);
	InterruptHoldoffCount = interrupts;
	QueryCancelHoldoffCount = cancels;
	ps_control_queue_count = 0;
	RESUME_CANCEL_INTERRUPTS();
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buffer);
	table_close(rel, AccessShareLock);
	PG_RETURN_BOOL(ok);
}

uint64
test_control_post(uint32 klass, const PageStoreRelKey *key, BlockNumber block,
				  const void *page, uint64 version, BlockNumber nb, int timeout_ms)
{
	if (block == PS_ADMISSION_FENCE_BLOCK)
		control_fence_writes++;
	return 0;
}

bool
test_control_fence_active(uint64 token)
{
	return token == 42;
}

uint64
test_control_barrier(uint64 lsn, int timeout_ms, bool *reclaimed)
{
	if (control_mode == 2)
		ereport(ERROR, (errmsg("injected transient barrier failure")));
	*reclaimed = true;
	return 0;
}

void
test_control_fence_end(uint64 token)
{
	if (token == 42)
		control_fence_ended++;
}

void
test_control_sync(int timeout_ms)
{
	control_synced++;
}

void
test_control_slru(XLogRecPtr redo)
{
}

void
test_control_snapshot(const struct ControlFileData *control)
{
	control_snapshots++;
}

/* Exercise the real drain for both a permanent refusal and a transient
 * failure: only the former may release the gate without a reader fence. */
Datum
pagestore_test_control_reclaimed(PG_FUNCTION_ARGS)
{
	bool ok;

	control_mode = 1;
	control_fence_ended = control_synced = control_fence_writes = control_snapshots = 0;
	ps_control_queue_head = 0;
	ps_control_queue_count = 1;
	memset(&ps_control_queue[0], 0, sizeof(ps_control_queue[0]));
	ps_control_queue[0].update_lsn = 200;
	ps_control_queue[0].image.checkPointCopy.redo = 100;
	ps_control_queue[0].fence_token = 42;
	ps_control_queue[0].writer_checkpoint = true;
	ps_control_drain();
	ok = ps_control_queue_count == 0 && control_fence_ended == 1 &&
		control_synced == 1 && control_fence_writes == 0 && control_snapshots == 0;

	control_mode = 2;
	control_fence_ended = control_synced = control_fence_writes = control_snapshots = 0;
	ps_control_queue_head = 0;
	ps_control_queue_count = 1;
	memset(&ps_control_queue[0], 0, sizeof(ps_control_queue[0]));
	ps_control_queue[0].update_lsn = 200;
	ps_control_queue[0].image.checkPointCopy.redo = 100;
	ps_control_queue[0].fence_token = 42;
	ps_control_drain();
	ok = ok && ps_control_queue_count == 1 &&
		ps_control_queue[0].fence_token == 42 && control_fence_ended == 0 &&
		control_synced == 0 && control_fence_writes == 0 && control_snapshots == 0;
	ps_control_queue_count = 0;
	control_mode = 0;
	PG_RETURN_BOOL(ok);
}
