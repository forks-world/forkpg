/* Exercise the production catch with a deterministic failing I/O callback.
 * This module is for the integration test only; never installed as pagestore. */
#define pagestore_localsvc_obj_write_prepare_timeout test_control_prepare
#include "pagestore_control.c"
#undef pagestore_localsvc_obj_write_prepare_timeout
#include "access/table.h"
#include "storage/bufmgr.h"

PG_FUNCTION_INFO_V1(pagestore_test_control_error);

static bool injected_control_error;

BlockNumber
test_control_prepare(uint32 klass, const PageStoreRelKey *key, int timeout_ms)
{
	injected_control_error = true;
	ereport(ERROR, (errmsg("injected control mirror write failure")));
	return 0;
}

Datum
pagestore_test_control_error(PG_FUNCTION_ARGS)
{
	Relation rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	Buffer buffer = ReadBuffer(rel, 0);
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
		InterruptHoldoffCount == interrupts && QueryCancelHoldoffCount == cancels;
	/* Report an ordinary test failure even against the unfixed drain; do
	 * not let its corrupted count abort the test backend on unlock. */
	InterruptHoldoffCount = interrupts;
	QueryCancelHoldoffCount = cancels;
	ps_control_queue_count = 0;
	RESUME_CANCEL_INTERRUPTS();
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buffer);
	table_close(rel, AccessShareLock);
	PG_RETURN_BOOL(ok);
}
