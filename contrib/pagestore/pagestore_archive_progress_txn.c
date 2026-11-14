#include <string.h>
#include "pagestore_archive_progress_txn.h"

int
ps_archive_progress_txn_init(PsArchiveProgressTxn *tx,
							 const PsArchiveProgress *recovered,
							 PsArchiveProgressPersist persist, void *arg)
{
	if (!tx || !ps_archive_progress_persistable(recovered) || !persist)
		return -1;
	memset(tx, 0, sizeof(*tx));
	if (pthread_mutex_init(&tx->lock, NULL) != 0)
		return -1;
	tx->state = *recovered;
	tx->persist = persist;
	tx->persist_arg = arg;
	return 0;
}

void
ps_archive_progress_txn_destroy(PsArchiveProgressTxn *tx)
{
	pthread_mutex_destroy(&tx->lock);
}

int
ps_archive_progress_txn_apply(PsArchiveProgressTxn *tx,
							  PsArchiveProgressTransition transition,
							  void *arg, PsArchiveProgress *result)
{
	PsArchiveProgress next;
	int rc = -1;
	if (!tx || !transition || pthread_mutex_lock(&tx->lock) != 0)
		return -1;
	if (tx->poisoned)
		goto done;
	next = tx->state;
	if (transition(&next, arg) != 0 || !ps_archive_progress_persistable(&next))
		goto done;
	if (tx->persist(tx->persist_arg, &next) != 0)
	{
		tx->poisoned = 1;
		goto done;
	}
	tx->state = next;
	if (result)
		*result = next;
	rc = 0;
done:
	pthread_mutex_unlock(&tx->lock);
	return rc;
}

int
ps_archive_progress_txn_snapshot(PsArchiveProgressTxn *tx, PsArchiveProgress *result)
{
	int rc = -1;
	if (!tx || !result || pthread_mutex_lock(&tx->lock) != 0)
		return -1;
	if (!tx->poisoned)
	{
		*result = tx->state;
		rc = 0;
	}
	pthread_mutex_unlock(&tx->lock);
	return rc;
}
