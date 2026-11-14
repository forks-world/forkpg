#ifndef PAGESTORE_ARCHIVE_PROGRESS_TXN_H
#define PAGESTORE_ARCHIVE_PROGRESS_TXN_H

#include <pthread.h>
#include "pagestore_archive_progress.h"

/* A persistence implementation must provide atomic, durable replacement and
 * report all ambiguous writes as errors. After any such error this context is
 * fenced until reopened from validated durable state. */
typedef int (*PsArchiveProgressPersist)(void *arg, const PsArchiveProgress *state);
typedef int (*PsArchiveProgressTransition)(PsArchiveProgress *state, void *arg);

typedef struct PsArchiveProgressTxn
{
	pthread_mutex_t lock;
	PsArchiveProgress state;
	PsArchiveProgressPersist persist;
	void *persist_arg;
	int poisoned;
} PsArchiveProgressTxn;

int ps_archive_progress_txn_init(PsArchiveProgressTxn *tx,
								  const PsArchiveProgress *recovered,
								  PsArchiveProgressPersist persist, void *arg);
void ps_archive_progress_txn_destroy(PsArchiveProgressTxn *tx);
int ps_archive_progress_txn_apply(PsArchiveProgressTxn *tx,
								   PsArchiveProgressTransition transition,
								   void *arg, PsArchiveProgress *result);
/* Never return an uncommitted or poisoned snapshot. */
int ps_archive_progress_txn_snapshot(PsArchiveProgressTxn *tx,
									 PsArchiveProgress *result);
#endif
