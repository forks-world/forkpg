/*
 * P2 (BRANCH_SNAPSHOT_SEQ_CAP.md S3.7, S8.2) property tests for the
 * forkmeta planner, ps_forkmeta_prune_plan_capped().  Merge blockers.
 *
 * The pre-P2 ps_forkmeta_prune_plan_required() (and its event_visible()/
 * retain_horizon() helpers) were left untouched by this phase -- the P2
 * work added ps_forkmeta_prune_plan_capped() alongside them rather than
 * replacing them -- so the differential here compares that still-original
 * function directly against a call through the capped engine (every fence
 * converted to positional, S = PS_FORKMETA_SEQ_UNBOUNDED): this is the
 * same "old code vs new code, all caps positional" property the page
 * planner's frozen-copy differential proves, without needing a separate
 * copy since the original is still in the tree.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pagestore_admissible.h"
#include "pagestore_forkmeta_prune.h"

static int checks;
static int failed;

static void
check(int ok, const char *name)
{
	checks++;
	if (!ok)
	{
		fprintf(stderr, "FAIL: %s\n", name);
		failed++;
	}
}

static uint64_t
xorshift(uint64_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 7;
	*s ^= *s << 17;
	return *s;
}

static PsForkMetaViewFence
to_view(PsForkMetaFence f)
{
	PsForkMetaViewFence v;

	v.lsn = f.lsn;
	v.seq = PS_FORKMETA_SEQ_UNBOUNDED;
	v.strict_seq = (f.admission_seq == 0) ? PS_FORKMETA_SEQ_UNBOUNDED :
		f.admission_seq;
	return v;
}

#define MAXN 20

static void
differential_fuzz(uint64_t seed, uint32_t niter)
{
	uint64_t	s = seed ? seed : 1;
	uint32_t	mismatches = 0;

	for (uint32_t iter = 0; iter < niter; iter++)
	{
		PsForkMetaEvent events[MAXN];
		PsForkMetaFence fences[4];
		PsForkMetaViewFence vfences[4];
		unsigned char keep_old[MAXN];
		unsigned char keep_new[MAXN];
		uint32_t	n = 1 + (uint32_t) (xorshift(&s) % MAXN);
		uint32_t	nfences = (uint32_t) (xorshift(&s) % 4);
		uint64_t	lsn = 0,
					seq = 0;
		PsForkMetaFence cutoff;
		int			rc_old,
					rc_new;

		for (uint32_t i = 0; i < n; i++)
		{
			if (xorshift(&s) % 3 != 0)
				lsn += 1 + xorshift(&s) % 10;
			seq += (xorshift(&s) % 5 == 0) ? 0 : 1 + xorshift(&s) % 10;
			events[i].lsn = lsn ? lsn : 1;
			events[i].admission_seq = seq;
			events[i].nblocks = (uint32_t) (xorshift(&s) % 50);
			switch (xorshift(&s) % 3)
			{
				case 0:
					events[i].kind = PS_FORKMETA_GROW;
					break;
				case 1:
					events[i].kind = PS_FORKMETA_SET;
					break;
				default:
					events[i].kind = PS_FORKMETA_DEAD;
					events[i].nblocks = 0;
			}
			events[i].flags = 0;
		}
		cutoff.lsn = 1 + xorshift(&s) % (lsn + 5);
		cutoff.admission_seq = 1 + xorshift(&s) % (seq + 5);
		for (uint32_t f = 0; f < nfences; f++)
		{
			fences[f].lsn = 1 + xorshift(&s) % (cutoff.lsn);
			fences[f].admission_seq = (xorshift(&s) % 3 == 0) ? 0 :
				1 + xorshift(&s) % (seq + 5);
			if (fences[f].lsn == cutoff.lsn &&
				fences[f].admission_seq > cutoff.admission_seq)
				fences[f].admission_seq = cutoff.admission_seq;
			vfences[f] = to_view(fences[f]);
		}
		rc_old = ps_forkmeta_prune_plan_required(events, n, cutoff, fences,
												 nfences, NULL, keep_old);
		rc_new = ps_forkmeta_prune_plan_capped(events, n, cutoff, vfences,
											   nfences, 0, 0, NULL, keep_new);
		if (rc_old != rc_new ||
			(rc_old >= 0 && memcmp(keep_old, keep_new, n) != 0))
			mismatches++;
	}
	{
		char		name[128];

		snprintf(name, sizeof(name),
				 "old-vs-capped(positional) differential (seed=%llu, "
				 "niter=%u, mismatches=%u)",
				 (unsigned long long) seed, niter, mismatches);
		check(mismatches == 0, name);
	}
}

/* Local duplicate of pagestore_forkmeta_prune.c's static event_admit_flags():
 * SET/DEAD are always META regardless of the stored flags. */
static unsigned char
test_event_admit_flags(const PsForkMetaEvent *ev)
{
	unsigned char flags = ev->flags;

	if (ev->kind == PS_FORKMETA_SET || ev->kind == PS_FORKMETA_DEAD)
		flags |= PS_FORKMETA_F_META;
	return flags;
}

/*
 * What a reader applying cap `ac` directly to `events` resolves as the
 * fork's size: mirrors retain_horizon_capped()'s own two walks (the newest
 * visible definitive event, then the largest visible GROW strictly after
 * it, or over the whole prefix when none is visible) but returns the
 * resolved size instead of a keep mask.  Used as the fuzz oracle below: a
 * sound planner must resolve the same value before and after pruning.
 */
static uint32_t
test_mask_resolve(const PsForkMetaEvent *events, uint32_t n,
				  const PsAdmitCap *ac)
{
	uint32_t	latest_def = 0;
	int			have_def = 0;
	uint32_t	resolved = 0;

	for (uint32_t i = n; i > 0; i--)
	{
		uint32_t	idx = i - 1;

		if (ps_event_hidden(events[idx].lsn, events[idx].admission_seq,
							test_event_admit_flags(&events[idx]), ac, 0, 0))
			continue;
		if (events[idx].kind != PS_FORKMETA_SET &&
			events[idx].kind != PS_FORKMETA_DEAD)
			continue;
		latest_def = idx;
		have_def = 1;
		break;
	}
	resolved = have_def ?
		(events[latest_def].kind == PS_FORKMETA_DEAD ? 0 :
		 events[latest_def].nblocks) : 0;
	for (uint32_t i = have_def ? latest_def + 1 : 0; i < n; i++)
		if (!ps_event_hidden(events[i].lsn, events[i].admission_seq,
							 test_event_admit_flags(&events[i]), ac, 0, 0) &&
			events[i].kind == PS_FORKMETA_GROW &&
			events[i].nblocks > resolved)
			resolved = events[i].nblocks;
	return resolved;
}

/*
 * Codex 4114217415 property: a single live view pinned exactly at the
 * cutoff's own lsn (the specific position the finding is about), with a
 * randomized finite/unbounded S and X on either side of cutoff.admission_seq,
 * so X is (at least sometimes) the sole binding constraint regardless of S.
 * Whatever ps_forkmeta_prune_plan_capped() keeps under the fence(s)
 * ps_forkmeta_derive_fences() derives for that view, the view must resolve
 * the identical size from the kept-only events as it does from the full,
 * unpruned array -- pruning must never change what a live view itself
 * reads.
 */
static void
derive_fences_equal_lsn_property(uint64_t seed, uint32_t niter)
{
	uint64_t	s = seed ? seed : 1;
	uint32_t	violations = 0;

	for (uint32_t iter = 0; iter < niter; iter++)
	{
		PsForkMetaEvent events[MAXN];
		PsForkMetaEvent kept_events[MAXN];
		unsigned char keep[MAXN];
		uint32_t	n = 1 + (uint32_t) (xorshift(&s) % MAXN);
		uint64_t	lsn = 0,
					seq = 0;
		PsForkMetaFence cutoff;
		PsForkMetaViewFence view;
		PsForkMetaViewFence derived[1];
		uint32_t	nderived;
		uint32_t	nkept = 0;
		PsAdmitCap	view_cap;
		int			rc;

		for (uint32_t i = 0; i < n; i++)
		{
			if (xorshift(&s) % 3 != 0)
				lsn += 1 + xorshift(&s) % 10;
			seq += (xorshift(&s) % 5 == 0) ? 0 : 1 + xorshift(&s) % 10;
			events[i].lsn = lsn ? lsn : 1;
			events[i].admission_seq = seq;
			events[i].nblocks = (uint32_t) (xorshift(&s) % 50);
			switch (xorshift(&s) % 3)
			{
				case 0:
					events[i].kind = PS_FORKMETA_GROW;
					break;
				case 1:
					events[i].kind = PS_FORKMETA_SET;
					break;
				default:
					events[i].kind = PS_FORKMETA_DEAD;
					events[i].nblocks = 0;
			}
			events[i].flags = 0;
		}
		cutoff.lsn = 1 + xorshift(&s) % (lsn + 5);
		/*
		 * A real cutoff.admission_seq is the admission clock's value as of
		 * cutoff.lsn, so (barring PAGE-class GROWs, which ignore S
		 * entirely, S1.5) it is always >= every SET/DEAD event's seq
		 * strictly below cutoff.lsn.  Pick it that way here too, so that
		 * "view.seq >= cutoff.admission_seq" genuinely implies "the view's
		 * own S already admits everything strictly below the cutoff" --
		 * an independently random cutoff.admission_seq can defeat that on
		 * its own (by landing below events the log has already passed at
		 * a lower lsn), which is a fuzz-generator artifact, not the
		 * X-vs-S hole this property targets. */
		{
			uint64_t	seq_floor = 0;

			for (uint32_t i = 0; i < n; i++)
				if (events[i].lsn < cutoff.lsn && events[i].admission_seq > seq_floor)
					seq_floor = events[i].admission_seq;
			cutoff.admission_seq = seq_floor + 1 + xorshift(&s) % 10;
		}
		view.lsn = cutoff.lsn;			/* the exact position this finding is about */
		view.seq = (xorshift(&s) % 4 == 0) ? PS_FORKMETA_SEQ_UNBOUNDED :
			xorshift(&s) % (cutoff.admission_seq + 5);
		view.strict_seq = (xorshift(&s) % 4 == 0) ? PS_FORKMETA_SEQ_UNBOUNDED :
			1 + xorshift(&s) % (cutoff.admission_seq + 5);

		nderived = ps_forkmeta_derive_fences(&view, 1, cutoff, derived);
		rc = ps_forkmeta_prune_plan_capped(events, n, cutoff, derived,
										   nderived, 0, 0, NULL, keep);
		if (rc < 0)
			continue;			/* not a valid input for this call; skip */
		for (uint32_t i = 0; i < n; i++)
			if (keep[i])
				kept_events[nkept++] = events[i];
		view_cap.lsn = view.lsn;
		view_cap.seq = view.seq;
		view_cap.strict_seq = view.strict_seq;
		if (test_mask_resolve(events, n, &view_cap) !=
			test_mask_resolve(kept_events, nkept, &view_cap))
			violations++;
	}
	{
		char		name[160];

		snprintf(name, sizeof(name),
				 "equal-LSN view resolves the same size before and after "
				 "pruning (seed=%llu, niter=%u, violations=%u)",
				 (unsigned long long) seed, niter, violations);
		check(violations == 0, name);
	}
}

int
main(void)
{
	static const uint64_t seeds[] = {1, 2, 42, 1337, 424242, 7};
	unsigned char keep[8];

	for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++)
		differential_fuzz(seeds[i], 4000);
	for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++)
		derive_fences_equal_lsn_property(seeds[i], 2000);

	/* --- S3.7 item 2 closure: META_FIRST ---
	 * Note: the operational cutoff is itself always a positional (S =
	 * infinity) mask, exactly like the page planner's floor (S3.5 item 1's
	 * analog) -- it independently retains its own newest visible
	 * definitive event regardless of any fence, so events[1] (seq 5) is
	 * kept in every case below via the cutoff alone.  The cases isolate
	 * what the fence and closure specifically contribute: events[0]. */
	{
		/* A fence with S=3 (X = cutoff's own seq, the only valid same-lsn
		 * strict bound, S3.5/S3.7's fence-domain rule) cannot see the
		 * non-first SET at seq 5 (S1.3: "Non-first post-S META is
		 * hidden") but does see the first one at seq 1 (the META_FIRST
		 * escape) directly, so the fence's own selection is events[0]. */
		PsForkMetaEvent ev[] =
		{
			{20, 1, 3, PS_FORKMETA_SET, PS_FORKMETA_F_META_FIRST},
			{20, 5, 3, PS_FORKMETA_SET, 0},
		};
		PsForkMetaViewFence f[] = {{20, 3, 5}};
		int rc = ps_forkmeta_prune_plan_capped(ev, 2, (PsForkMetaFence) {20, 5},
											   f, 1, 0, 0, NULL, keep);

		check(rc == 2 && keep[0] && keep[1],
			  "a finite-S fence's own META_FIRST escape selects the first "
			  "SET; the cutoff independently keeps the newest");
	}
	{
		/* A fence with S=10 sees *both* SETs directly (no escape needed),
		 * so its own greatest-admissible selection is events[1] (seq 5,
		 * non-first) -- exactly like the cutoff's own pick.  Closure must
		 * still separately retain events[0] (META_FIRST) because a fence
		 * selected a non-first META at this position (S3.7 item 2),
		 * distinguishing this from the differential fuzz's positional-only
		 * (S = infinity) cases, where closure never fires at all. */
		PsForkMetaEvent ev[] =
		{
			{20, 1, 3, PS_FORKMETA_SET, PS_FORKMETA_F_META_FIRST},
			{20, 5, 3, PS_FORKMETA_SET, 0},
		};
		PsForkMetaViewFence f[] = {{20, 10, 5}};
		int rc = ps_forkmeta_prune_plan_capped(ev, 2, (PsForkMetaFence) {20, 5},
											   f, 1, 0, 0, NULL, keep);

		check(rc == 2 && keep[0] && keep[1],
			  "closure retains META_FIRST alongside a fence's own "
			  "non-first selection");
	}
	/* --- S3.7 item 2 closure: UNSTAMPED --- */
	{
		/* A fence with S=1 hides the UNSTAMPED event (seq 1 > S... use S=0
		 * equivalent by keeping S below both) directly, but retains the
		 * later same-LSN GROW (seq 5, PAGE-class, p > B_k is enough) --
		 * closure must then retain the hidden UNSTAMPED event too. */
		PsForkMetaEvent ev[] =
		{
			{20, 1, 3, PS_FORKMETA_GROW, PS_FORKMETA_F_UNSTAMPED},
			{20, 5, 4, PS_FORKMETA_GROW, 0},
		};
		PsForkMetaViewFence f[] = {{20, 0, 5}};
		int rc = ps_forkmeta_prune_plan_capped(ev, 2, (PsForkMetaFence) {20, 5},
											   f, 1, 0, 0, NULL, keep);

		check(rc == 2 && keep[0] && keep[1],
			  "a retained later same-LSN event also retains a hidden "
			  "UNSTAMPED event there");
	}
	{
		/* No later same-LSN event at all (the other event is at a
		 * different LSN): the UNSTAMPED event stays unprotected (S1.6: no
		 * escape of its own). */
		PsForkMetaEvent ev[] =
		{
			{20, 1, 3, PS_FORKMETA_GROW, PS_FORKMETA_F_UNSTAMPED},
			{40, 1, 4, PS_FORKMETA_GROW, 0},
		};
		PsForkMetaViewFence f[] = {{40, 0, 1}};
		int rc = ps_forkmeta_prune_plan_capped(ev, 2, (PsForkMetaFence) {40, 1},
											   f, 1, 0, 0, NULL, keep);

		check(rc >= 0 && !keep[0],
			  "an UNSTAMPED event with no later same-LSN survivor stays "
			  "unprotected");
	}
	/* --- S3.7 item 5: derived fences --- */
	{
		PsForkMetaViewFence views[] =
		{
			{100, 5, PS_FORKMETA_SEQ_UNBOUNDED},		/* beyond cutoff, finite S < cutoff_seq: derives */
			{100, PS_FORKMETA_SEQ_UNBOUNDED, PS_FORKMETA_SEQ_UNBOUNDED}, /* positional: no derivation */
			{100, 50, PS_FORKMETA_SEQ_UNBOUNDED},		/* S >= cutoff_seq: no derivation needed */
			{5, 1, PS_FORKMETA_SEQ_UNBOUNDED},			/* already below cutoff: no derivation */
		};
		PsForkMetaFence cutoff = {20, 10};
		PsForkMetaViewFence out[4];
		uint32_t	n = ps_forkmeta_derive_fences(views, 4, cutoff, out);

		check(n == 1 && out[0].lsn == 20 && out[0].seq == 5 &&
			  out[0].strict_seq == 10,
			  "derive exactly one cutoff-position fence for the one view "
			  "that needs it");
	}
	{
		/* The derived fence actually protects the cutoff-position mask: a
		 * view far beyond the cutover (L=100) with S=1 needs (20,1) to
		 * stay resolvable through the cutover the way it read before. */
		PsForkMetaEvent ev[] =
		{
			{20, 1, 3, PS_FORKMETA_SET, PS_FORKMETA_F_META_FIRST},
			{20, 5, 3, PS_FORKMETA_SET, 0},
		};
		PsForkMetaViewFence views[] = {{100, 1, PS_FORKMETA_SEQ_UNBOUNDED}};
		PsForkMetaFence cutoff = {20, 5};
		PsForkMetaViewFence derived[1];
		uint32_t	nderived = ps_forkmeta_derive_fences(views, 1, cutoff,
														 derived);
		int			rc;

		check(nderived == 1, "one fence derived for the beyond-cutover view");
		rc = ps_forkmeta_prune_plan_capped(ev, 2, cutoff, derived, nderived,
										   0, 0, NULL, keep);
		check(rc == 2 && keep[0] && keep[1],
			  "the derived fence keeps the first arrival the beyond-cutover "
			  "view's own S=1 needs, alongside the cutoff's own newest pick");
	}
	{
		/* Codex 4114217415's exact counterexample: a view with L ==
		 * cutoff.lsn whose own X (5) is stricter than both S (20) and
		 * cutoff.admission_seq (10).  The old "S >= cutoff.admission_seq =>
		 * sees everything" test skipped this view entirely, so the capped
		 * planner kept only the larger, later PAGE GROW (seq 10, size 20) --
		 * invisible to this view (seq 10 > X 5, no escape for a PAGE-class
		 * GROW's hard boundary) -- and dropped the one GROW the view can
		 * actually see (seq 5, size 10). */
		PsForkMetaEvent ev[] =
		{
			{20, 5, 10, PS_FORKMETA_GROW, 0},
			{20, 10, 20, PS_FORKMETA_GROW, 0},
		};
		PsForkMetaViewFence views[] = {{20, 20, 5}};
		PsForkMetaFence cutoff = {20, 10};
		PsForkMetaViewFence derived[1];
		uint32_t	nderived = ps_forkmeta_derive_fences(views, 1, cutoff,
														 derived);
		int			rc_nofence;
		int			rc;

		check(nderived == 1 && derived[0].lsn == 20 && derived[0].seq == 20 &&
			  derived[0].strict_seq == 5,
			  "an equal-LSN view derives a fence from its own X even though "
			  "S alone would not (Codex 4114217415)");
		rc_nofence = ps_forkmeta_prune_plan_capped(ev, 2, cutoff, NULL, 0,
												   0, 0, NULL, keep);
		check(rc_nofence == 1 && !keep[0] && keep[1],
			  "before the fix: the cutoff alone keeps only the larger, "
			  "later GROW, which is invisible to the view's own X = 5");
		rc = ps_forkmeta_prune_plan_capped(ev, 2, cutoff, derived, nderived,
										   0, 0, NULL, keep);
		check(rc == 2 && keep[0] && keep[1],
			  "the derived fence also keeps the GROW the view's X = 5 "
			  "actually admits (size 10), alongside the cutoff's own pick");
	}

	printf("pagestore_forkmeta_prune_capped_test: %d checks, %d failed\n",
		   checks, failed);
	return failed != 0;
}
