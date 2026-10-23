/* Private copy of the production SLRU catch, with a failing write callback. */
#define pagestore_localsvc_obj_write_timeout test_slru_write
#include "pagestore_slru.c"
#undef pagestore_localsvc_obj_write_timeout
#include "access/table.h"
#include "storage/bufmgr.h"

PG_FUNCTION_INFO_V1(pagestore_test_slru_error);

static bool injected_slru_error;

void
test_slru_write(uint32 klass, const PageStoreRelKey *key, BlockNumber block,
				const void *page, uint64 version, int timeout_ms)
{
	injected_slru_error = true;
	ereport(ERROR, (errmsg("injected SLRU mirror write failure")));
}

Datum
pagestore_test_slru_error(PG_FUNCTION_ARGS)
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
	memset(&ps_slru_queue[0], 0, sizeof(ps_slru_queue[0]));
	ps_slru_queue[0].used = true;
	ps_slru_queue[0].posted_fence = 1; /* retry path: no read-side probe */
	ps_slru_queue_count = 1;
	ps_slru_drain();
	ok = injected_slru_error && ps_slru_queue_count == 1 &&
		!ps_slru_queue[0].shipped &&
		InterruptHoldoffCount == interrupts && QueryCancelHoldoffCount == cancels &&
		CurrentMemoryContext == context;
	MemoryContextSwitchTo(context);
	InterruptHoldoffCount = interrupts;
	QueryCancelHoldoffCount = cancels;
	ps_slru_queue[0].used = false;
	ps_slru_queue_count = 0;
	RESUME_CANCEL_INTERRUPTS();
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buffer);
	table_close(rel, AccessShareLock);
	PG_RETURN_BOOL(ok);
}
