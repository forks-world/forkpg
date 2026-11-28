/* Archive-progress state transitions. The caller must serialize and durably
 * persist changes before acknowledging them. This module does not perform I/O
 * and a progress grant must never be used as a retention or durability fence. */
#ifndef PAGESTORE_ARCHIVE_PROGRESS_H
#define PAGESTORE_ARCHIVE_PROGRESS_H

#include <stdint.h>

typedef struct PsArchiveProgress
{
	uint32_t timeline;
	uint64_t incarnation;
	uint64_t attempt;
	uint64_t marker;
	uint64_t safe_lsn;
	uint64_t target;
	uint64_t grant_end;
	uint64_t original_limit;
	uint64_t max_extra;
	uint64_t start_nonce_hi;
	uint64_t start_nonce_lo;
	uint32_t budget_frozen;
	uint32_t grant_active;
	uint32_t candidate_wal_tli;
} PsArchiveProgress;

/* Validate decoded durable state before it is installed in an authority. */
static inline int
ps_archive_progress_valid(const PsArchiveProgress *s)
{
	if (!s)
		return 0;
	if (!s->attempt)
		return s->timeline == 0 && s->incarnation == 0 && s->marker == 0 && s->safe_lsn == 0 &&
			s->target == 0 && s->grant_end == 0 && s->original_limit == 0 &&
			s->max_extra == 0 && s->start_nonce_hi == 0 && s->start_nonce_lo == 0 &&
			s->budget_frozen == 0 && s->grant_active == 0 &&
			s->candidate_wal_tli == 0;
	if (!s->marker || !s->incarnation || s->budget_frozen > 1 ||
		s->grant_active > 1 ||
		(!s->budget_frozen && (s->original_limit || s->max_extra || s->target || s->grant_end)) ||
		(s->budget_frozen && s->original_limit > UINT64_MAX - s->max_extra) ||
		(s->safe_lsn && s->safe_lsn <= s->marker) ||
		((s->target == 0) != (s->grant_end == 0)) ||
		((s->target == 0) != (s->candidate_wal_tli == 0)))
		return 0;
	if (s->grant_active && !s->target)
		return 0;
	return !s->target || (s->budget_frozen && s->target > s->marker && s->grant_end > s->target &&
		s->grant_end <= s->original_limit + s->max_extra);
}

/* Durable records must always carry the nonce that makes START retries
 * idempotent. The transition helper temporarily creates state before the
 * registry attaches its nonce, so keep this stricter predicate separate. */
static inline int
ps_archive_progress_persistable(const PsArchiveProgress *s)
{
	return ps_archive_progress_valid(s) &&
		(!s->attempt || s->start_nonce_hi != 0 || s->start_nonce_lo != 0);
}

/* The owner supplies a unique, durably fenced attempt and a valid marker.
 * A previously committed grant remains fixed even after attempt replacement. */
static inline int
ps_archive_progress_begin(PsArchiveProgress *s, uint32_t timeline,
						  uint64_t incarnation,
						  uint64_t attempt, uint64_t marker,
						  uint64_t original_limit, uint64_t max_extra)
{
	(void) original_limit;
	(void) max_extra;
	/* An exact retry after a lost reply must preserve later transitions. */
	if (s && s->attempt == attempt && attempt != 0 && s->timeline == timeline &&
		s->incarnation == incarnation && s->marker == marker)
		return ps_archive_progress_valid(s) ? 0 : -1;
	if (!ps_archive_progress_valid(s) || !incarnation || attempt == 0 || marker == 0 || s->attempt == UINT64_MAX ||
		attempt != s->attempt + 1 ||
		(s->marker != 0 && marker < s->marker) ||
		(s->attempt != 0 && (s->timeline != timeline || s->incarnation != incarnation)))
		return -1;
	/* A restart fences feedback but cannot replace a committed target while
	 * the marker stays put. A completed restartpoint starts a new budget. */
	if (s->marker != marker)
	{
		s->target = 0;
		s->grant_end = 0;
		s->original_limit = 0;
		s->max_extra = 0;
		s->budget_frozen = 0;
		s->grant_active = 0;
		s->candidate_wal_tli = 0;
	}
	else if (!s->grant_active)
	{
		/* A new attempt invalidates an unactivated candidate. An activated
		 * bounded grant remains fixed, but still requires fresh SAFE. */
		s->target = 0;
		s->grant_end = 0;
		s->candidate_wal_tli = 0;
	}
	s->timeline = timeline;
	s->incarnation = incarnation;
	s->attempt = attempt;
	s->marker = marker;
	s->safe_lsn = 0;
	s->start_nonce_hi = 0;
	s->start_nonce_lo = 0;
	return 0;
}

static inline int
ps_archive_progress_freeze_budget(PsArchiveProgress *s, uint32_t timeline,
								  uint64_t incarnation, uint64_t attempt,
								  uint64_t marker, uint64_t original_limit,
								  uint64_t max_extra)
{
	if (!ps_archive_progress_valid(s) || s->timeline != timeline ||
		s->incarnation != incarnation || s->attempt != attempt ||
		s->marker != marker || !s->safe_lsn ||
		original_limit > UINT64_MAX - max_extra)
		return -1;
	if (s->budget_frozen)
		return s->original_limit == original_limit &&
			s->max_extra == max_extra ? 0 : -1;
	s->original_limit = original_limit;
	s->max_extra = max_extra;
	s->budget_frozen = 1;
	return 0;
}

/* The caller must publish this only after PostgreSQL's consistency checks
 * succeeded with invalid-page checking enabled. */
static inline int
ps_archive_progress_safe(PsArchiveProgress *s, uint32_t timeline,
						 uint64_t incarnation, uint64_t attempt,
						 uint64_t marker, uint64_t lsn)
{
	if (!ps_archive_progress_valid(s) || s->timeline != timeline ||
		s->incarnation != incarnation || s->attempt != attempt ||
		s->marker != marker || !attempt || !marker || lsn <= marker ||
		(s->safe_lsn && s->safe_lsn != lsn))
		return -1;
	s->safe_lsn = lsn;
	return 0;
}

/* target_end is the complete-segment end following a completed checkpoint;
 * its derivation is validated by the caller. max_extra bounds the whole grant
 * relative to the original limit, not relative to a retry or shipped bytes. */
static inline int
ps_archive_progress_prepare(PsArchiveProgress *s, uint32_t timeline,
						uint64_t incarnation, uint64_t attempt, uint64_t marker,
						uint32_t candidate_wal_tli, uint64_t target,
						uint64_t target_end)
{
	uint64_t cap;
	if (!ps_archive_progress_valid(s) || s->timeline != timeline ||
		s->incarnation != incarnation || s->attempt != attempt ||
		s->marker != marker || !s->safe_lsn || !s->budget_frozen || target <= s->safe_lsn ||
		!candidate_wal_tli || target_end <= target ||
		s->original_limit > UINT64_MAX - s->max_extra)
		return -1;
	cap = s->original_limit + s->max_extra;
	if (target_end > cap)
		return -1;
	if (s->target)
		return !s->grant_active && s->candidate_wal_tli == candidate_wal_tli &&
			s->target == target &&
			s->grant_end == target_end ? 0 : -1;
	s->target = target;
	s->grant_end = target_end;
	s->candidate_wal_tli = candidate_wal_tli;
	return 0;
}

/* Activation is a separate durable transition after the writer has proved
 * that the complete target segment is flushed and archive-notified. */
static inline int
ps_archive_progress_activate(PsArchiveProgress *s, uint32_t timeline,
							 uint64_t incarnation, uint64_t attempt,
							 uint64_t marker, uint32_t candidate_wal_tli,
							 uint64_t target,
							 uint64_t target_end)
{
	if (!ps_archive_progress_valid(s) || s->timeline != timeline ||
		s->incarnation != incarnation || s->attempt != attempt ||
		s->marker != marker || !s->safe_lsn || !s->budget_frozen ||
		!s->target || s->candidate_wal_tli != candidate_wal_tli ||
		s->target != target || s->grant_end != target_end)
		return -1;
	s->grant_active = 1;
	return 0;
}
#endif
