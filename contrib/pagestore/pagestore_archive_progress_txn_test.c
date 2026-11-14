#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pagestore_archive_progress_txn.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
typedef struct Fixture { PsArchiveProgress durable; int fail; int writes; } Fixture;
typedef struct Request { PsArchiveProgressTxn *tx; uint64_t target; int rc; } Request;
static int save(void *arg, const PsArchiveProgress *s) {
	Fixture *f = arg; f->writes++;
	if (f->fail) { f->durable = *s; return -1; } /* ambiguous commit */
	f->durable = *s; return 0;
}
static int begin(PsArchiveProgress *s, void *arg) {
	(void) arg;
	if (ps_archive_progress_begin(s, 3, 1, 1, 100, 192, 64) != 0) return -1;
	s->start_nonce_hi = 1;
	return 0;
}
static int safe(PsArchiveProgress *s, void *arg) {
	(void) arg; return ps_archive_progress_safe(s, 3, 1, 1, 100, 180);
}
static int freeze(PsArchiveProgress *s, void *arg) {
	(void) arg; return ps_archive_progress_freeze_budget(s, 3, 1, 1, 100, 192, 64);
}
static int pin(PsArchiveProgress *s, void *arg) {
	uint64_t target = *(uint64_t *)arg;
	return ps_archive_progress_prepare(s, 3, 1, 1, 100, 1, target, 256);
}
static void *worker(void *arg) {
	Request *r = arg; r->rc = ps_archive_progress_txn_apply(r->tx, pin, &r->target, NULL); return NULL;
}
int main(void) {
	Fixture f = {0}; PsArchiveProgressTxn tx; PsArchiveProgress zero = {0}, snapshot;
	Request a = {&tx, 200, -1}, b = {&tx, 201, -1}; pthread_t ta, tb;
	CHECK(ps_archive_progress_txn_init(&tx, &zero, save, &f) == 0);
	CHECK(ps_archive_progress_txn_apply(&tx, begin, NULL, NULL) == 0);
	CHECK(ps_archive_progress_txn_apply(&tx, safe, NULL, NULL) == 0);
	CHECK(ps_archive_progress_txn_apply(&tx, freeze, NULL, NULL) == 0);
	CHECK(pthread_create(&ta, NULL, worker, &a) == 0);
	CHECK(pthread_create(&tb, NULL, worker, &b) == 0);
	CHECK(pthread_join(ta, NULL) == 0 && pthread_join(tb, NULL) == 0);
	CHECK((a.rc == 0) != (b.rc == 0));
	CHECK(f.durable.target == (a.rc == 0 ? a.target : b.target));
	CHECK(ps_archive_progress_txn_snapshot(&tx, &snapshot) == 0 &&
			snapshot.target == f.durable.target && f.writes == 4);
	f.fail = 1;
	CHECK(ps_archive_progress_txn_apply(&tx, safe, NULL, NULL) != 0);
	CHECK(ps_archive_progress_txn_snapshot(&tx, &snapshot) != 0);
	f.fail = 0;
	CHECK(ps_archive_progress_txn_apply(&tx, safe, NULL, NULL) != 0);
	CHECK(f.writes == 5);
	ps_archive_progress_txn_destroy(&tx);
	CHECK(ps_archive_progress_txn_init(&tx, &f.durable, save, &f) == 0);
	CHECK(ps_archive_progress_txn_apply(&tx, pin, &f.durable.target, NULL) == 0);
	ps_archive_progress_txn_destroy(&tx);
	f.durable.grant_end = f.durable.target;
	CHECK(ps_archive_progress_txn_init(&tx, &f.durable, save, &f) != 0);
	puts("archive progress serialized transaction tests passed"); return 0;
}
